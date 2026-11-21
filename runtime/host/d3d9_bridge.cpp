// Guest D3D9 (32-bit COM, stdcall: `this` is the first stack argument) -> dxvk-native D3D9 (64-bit).
// Each native object gets one 8-byte guest wrapper {vtable, index}; all wrappers of one interface type share a vtable
// of stub addresses (type t, method i) -> the generated forwarder. Lock* calls hand the game a guest staging buffer
// and copy to/from the native mapping, because the game can only address its own 4 GB.
#include "d3d9_bridge.h"
#include <SDL3/SDL_timer.h>
#include <pthread.h>
#include <string>
#include <unordered_map>
#include <vector>

#define G_D3D 0xFFD00000u   // stub (type t, method i) = G_D3D + 16 * (256 * t + i)

struct LockState { uint32_t key, guest, size, flags; uint8_t *native; };
struct Wrap { void *native; int type; uint32_t gobj; std::vector<LockState> locks; };
static std::vector<Wrap> wraps(1);          // index 0 = none
static std::vector<uint32_t> free_slots;
static std::unordered_map<void *, uint32_t> by_native;
static std::unordered_map<void *, uint32_t> hwnd_of_native;   // native window -> guest HWND (for out-structs)
static uint32_t vtables[64];
static pthread_mutex_t mu = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
struct Guard { Guard() { pthread_mutex_lock(&mu); } ~Guard() { pthread_mutex_unlock(&mu); } };

static uint32_t vtable_for(int t) {
    if (!vtables[t]) {
        uint32_t n = (uint32_t)iface_method_count[t], vt = gmalloc(4 * n);
        for (uint32_t i = 0; i < n; i++) U32(vt + 4 * i) = G_D3D + 16 * (256 * t + i);
        vtables[t] = vt;
    }
    return vtables[t];
}

static void unwrap_dead(uint32_t g);
static bool is_base(int base, int t) {   // base == t or an interface t derives from
    static int parent[64];
    static const char *const tree[][2] = {
        {"IDirect3DSurface9", "IDirect3DResource9"}, {"IDirect3DVertexBuffer9", "IDirect3DResource9"},
        {"IDirect3DIndexBuffer9", "IDirect3DResource9"}, {"IDirect3DBaseTexture9", "IDirect3DResource9"},
        {"IDirect3DTexture9", "IDirect3DBaseTexture9"}, {"IDirect3DCubeTexture9", "IDirect3DBaseTexture9"},
        {"IDirect3DVolumeTexture9", "IDirect3DBaseTexture9"}, {"IDirect3D9Ex", "IDirect3D9"},
        {"IDirect3DSwapChain9Ex", "IDirect3DSwapChain9"}, {"IDirect3DDevice9Ex", "IDirect3DDevice9"}};
    if (!parent[0]) {
        for (int i = 0; i < 64; i++) parent[i] = -1;
        for (auto &e : tree) parent[iface_type(e[0])] = iface_type(e[1]);
    }
    for (; t >= 0; t = parent[t]) if (t == base) return true;
    return false;
}
uint32_t wrap(void *native, int type) {
    if (!native) return 0;
    Guard g;
    auto it = by_native.find(native);
    if (it != by_native.end()) {
        Wrap &w = wraps[it->second];
        if (type < 0 || is_base(type, w.type)) return w.gobj;
        if (is_base(w.type, type)) { w.type = type; U32(w.gobj) = vtable_for(type); return w.gobj; }   // learned a more derived type
        // Unrelated type at a known address: the old object died without us seeing its last Release (a texture's
        // surfaces go with the texture) and the host allocator reused its memory. Its wrapper would dispatch the new
        // object's calls to the wrong methods, popping the wrong number of arguments.
        unwrap_dead(w.gobj);
    }
    if (type < 0) die("d3d9: native object %p of unknown interface type", native);
    uint32_t i;
    if (!free_slots.empty()) { i = free_slots.back(); free_slots.pop_back(); }
    else { i = (uint32_t)wraps.size(); wraps.emplace_back(); }
    uint32_t gobj = gmalloc(8);
    U32(gobj) = vtable_for(type);
    U32(gobj + 4) = i;
    wraps[i] = Wrap{native, type, gobj, {}};
    by_native[native] = i;
    return gobj;
}
static Wrap *wrap_of(uint32_t g) {
    if (!g) return nullptr;
    uint32_t i = U32(g + 4);
    return i && i < wraps.size() && wraps[i].gobj == g ? &wraps[i] : nullptr;
}
void *nat(uint32_t g) {
    Guard l;
    static pthread_t first = pthread_self();   // log once if D3D9 is ever called from a second thread
    static bool told;
    if (!told && !pthread_equal(first, pthread_self())) { told = true; fprintf(stderr, "[d3d9] called from a second thread\n"); }
    Wrap *w = wrap_of(g);
    return w ? w->native : nullptr;
}
static void unwrap_dead(uint32_t g) {   // the native object is gone: forget its wrapper
    Guard l;
    Wrap *w = wrap_of(g);
    if (!w) return;
    for (auto &k : w->locks) gfree(k.guest);
    by_native.erase(w->native);
    uint32_t i = U32(g + 4);
    wraps[i] = Wrap{};
    free_slots.push_back(i);
    gfree(g);
}

HWND native_hwnd(uint32_t h) {
    if (!h) return nullptr;
    SDL_Window *s = hwnd_window(h);
    if (!s) fprintf(stderr, "[d3d9] HWND %08x has no SDL window\n", h);
    Guard l;
    if (s) hwnd_of_native[(void *)s] = h;
    return reinterpret_cast<HWND>(s);
}
static uint32_t guest_hwnd(HWND n) {
    Guard l;
    auto it = hwnd_of_native.find((void *)n);
    return it == hwnd_of_native.end() ? 0 : it->second;
}

// aspect-ratio diagnosis: distinct viewports / render targets / depth buffers the game asks for, with the guest caller
static Fn orig_viewport, orig_texture, orig_rt, orig_depth;
static void guest_stack(CPU *c, const char *what);
static void log_once(const char *fmt, uint32_t a, uint32_t b, uint32_t c2, uint32_t d, uint32_t e, uint32_t caller) {
    static std::vector<std::string> seen;
    char line[200];
    snprintf(line, sizeof line, fmt, a, b, c2, d, e, caller);
    for (auto &s : seen) if (s == line) return;
    if (seen.size() < 80) { seen.push_back(line); fprintf(stderr, "%s\n", line); }
}
static void log_viewport(CPU *c) {   // SetViewport(this, D3DVIEWPORT9* {X, Y, Width, Height, MinZ, MaxZ})
    uint32_t v = ARG(c, 1);
    log_once("[d3d9] viewport %u,%u %ux%u (z %u) from %08x", U32(v), U32(v + 4), U32(v + 8), U32(v + 12), 0, U32(REG(c, R_ESP)));
    static int traced;
    if (U32(v + 8) == 1280 && U32(v + 12) == 800 && traced++ < 3) guest_stack(c, "viewport 1280x800");
    orig_viewport(c);
}
static void guest_stack(CPU *c, const char *what) {   // likely return addresses on the guest stack (code range), innermost first
    std::string s;
    char b[16];
    uint32_t esp = REG(c, R_ESP);
    for (uint32_t k = 0, n = 0; k < 0x600 && n < 16; k += 4) {
        uint32_t v = U32(esp + k);
        if (v >= 0x401000 && v < 0xab6000) { snprintf(b, sizeof b, " %08x", v); s += b; n++; }
    }
    fprintf(stderr, "[d3d9] %s stack:%s\n", what, s.c_str());
}
static void log_texture(CPU *c) {   // CreateTexture(this, w, h, levels, usage, format, pool, out, share): usage 1 = render target
    if (ARG(c, 4) & 1) log_once("[d3d9] render target texture %ux%u levels %u usage %x format %u from %08x", ARG(c, 1), ARG(c, 2), ARG(c, 3), ARG(c, 4), ARG(c, 5), U32(REG(c, R_ESP)));
    static int traced;
    if ((ARG(c, 4) & 1) && ARG(c, 1) == 1280 && (ARG(c, 2) == 720 || ARG(c, 2) == 800) && traced++ < 6) guest_stack(c, ARG(c, 2) == 720 ? "rt 1280x720" : "rt 1280x800");
    orig_texture(c);
}
static void log_rt(CPU *c) {   // CreateRenderTarget(this, w, h, format, ms, msq, lockable, out, share)
    log_once("[d3d9] render target surface %ux%u format %u ms %u %u from %08x", ARG(c, 1), ARG(c, 2), ARG(c, 3), ARG(c, 4), 0, U32(REG(c, R_ESP)));
    orig_rt(c);
}
static void log_depth(CPU *c) {   // CreateDepthStencilSurface(this, w, h, format, ms, msq, discard, out, share)
    log_once("[d3d9] depth surface %ux%u format %u ms %u %u from %08x", ARG(c, 1), ARG(c, 2), ARG(c, 3), ARG(c, 4), 0, U32(REG(c, R_ESP)));
    orig_depth(c);
}

// main.c fallback: stub address -> generated forwarder
extern "C" Fn d3d9_resolve(uint32_t t) {
    if (t < G_D3D || t >= G_D3D + 16 * 256 * 64) return nullptr;
    uint32_t s = (t - G_D3D) / 16, type = s / 256, i = s % 256;
    if ((int)type >= iface_count || (int)i >= iface_method_count[type]) die("d3d9: bad stub %08x", t);
    static const bool trace_d3d = getenv("CATH_TRACE_D3D");   // once: this runs on every D3D9 call
    if (trace_d3d) fprintf(stderr, "[d3d9] %s::%s\n", iface_names[type], iface_method_names[type][i]);
    const char *m = iface_method_names[type][i];
    if (!strncmp(iface_names[type], "IDirect3DDevice9", 16)) {
        if (!strcmp(m, "SetViewport")) { orig_viewport = iface_methods[type][i]; return log_viewport; }
        if (!strcmp(m, "CreateTexture")) { orig_texture = iface_methods[type][i]; return log_texture; }
        if (!strcmp(m, "CreateRenderTarget")) { orig_rt = iface_methods[type][i]; return log_rt; }
        if (!strcmp(m, "CreateDepthStencilSurface")) { orig_depth = iface_methods[type][i]; return log_depth; }
    }
    return iface_methods[type][i];
}

// ---------- entry points (the game LoadLibrary's D3D9.DLL and GetProcAddress's these) ----------

IMPX("d3d9.dll", "Direct3DCreate9", Direct3DCreate9_std) {
    ret_std(c, wrap(Direct3DCreate9(ARG(c, 0)), iface_type("IDirect3D9")), 1);
}
IMPX("d3d9.dll", "Direct3DCreate9Ex", Direct3DCreate9Ex_std) {   // (sdk, IDirect3D9Ex** out) -> HRESULT
    IDirect3D9Ex *p = nullptr;
    HRESULT r = Direct3DCreate9Ex(ARG(c, 0), &p);
    if (ARG(c, 1)) U32(ARG(c, 1)) = SUCCEEDED(r) ? wrap(p, iface_type("IDirect3D9Ex")) : 0;
    ret_std(c, (uint32_t)r, 2);
}

// ---------- present / creation parameters: HWND makes the guest layout differ ----------
// guest D3DPRESENT_PARAMETERS (56 bytes): W 0, H 4, Fmt 8, Count c, MS 10, MSQ 14, Swap 18, hwnd 1c, Windowed 20,
// AutoDS 24, DSFmt 28, Flags 2c, Refresh 30, Interval 34
static void pp_in(uint32_t g, D3DPRESENT_PARAMETERS *p) {
    memset(p, 0, sizeof *p);
    p->BackBufferWidth = U32(g); p->BackBufferHeight = U32(g + 4); p->BackBufferFormat = (D3DFORMAT)U32(g + 8);
    p->BackBufferCount = U32(g + 0xC); p->MultiSampleType = (D3DMULTISAMPLE_TYPE)U32(g + 0x10);
    p->MultiSampleQuality = U32(g + 0x14); p->SwapEffect = (D3DSWAPEFFECT)U32(g + 0x18);
    p->hDeviceWindow = native_hwnd(U32(g + 0x1C)); p->Windowed = U32(g + 0x20);
    p->EnableAutoDepthStencil = U32(g + 0x24); p->AutoDepthStencilFormat = (D3DFORMAT)U32(g + 0x28);
    p->Flags = U32(g + 0x2C); p->FullScreen_RefreshRateInHz = U32(g + 0x30); p->PresentationInterval = U32(g + 0x34);
    fprintf(stderr, "[d3d9] present params %ux%u fmt %u count %u ms %u windowed %u depth %u/%u flags %x interval %x\n",
            p->BackBufferWidth, p->BackBufferHeight, p->BackBufferFormat, p->BackBufferCount, p->MultiSampleType,
            p->Windowed, p->EnableAutoDepthStencil, p->AutoDepthStencilFormat, p->Flags, p->PresentationInterval);
}
static void pp_out(const D3DPRESENT_PARAMETERS *p, uint32_t g) {
    U32(g) = p->BackBufferWidth; U32(g + 4) = p->BackBufferHeight; U32(g + 8) = p->BackBufferFormat;
    U32(g + 0xC) = p->BackBufferCount; U32(g + 0x10) = p->MultiSampleType; U32(g + 0x14) = p->MultiSampleQuality;
    U32(g + 0x18) = p->SwapEffect; U32(g + 0x1C) = guest_hwnd(p->hDeviceWindow); U32(g + 0x20) = p->Windowed;
    U32(g + 0x24) = p->EnableAutoDepthStencil; U32(g + 0x28) = p->AutoDepthStencilFormat; U32(g + 0x2C) = p->Flags;
    U32(g + 0x30) = p->FullScreen_RefreshRateInHz; U32(g + 0x34) = p->PresentationInterval;
}

void special_CreateDevice(CPU *c, int) {   // (this, adapter, type, focus, flags, pp*, out**)
    auto *d3d = (IDirect3D9 *)nat(ARG(c, 0));
    D3DPRESENT_PARAMETERS pp;
    pp_in(ARG(c, 5), &pp);
    IDirect3DDevice9 *dev = nullptr;
    HRESULT r = d3d->CreateDevice(ARG(c, 1), (D3DDEVTYPE)ARG(c, 2), native_hwnd(ARG(c, 3)), ARG(c, 4), &pp, &dev);
    fprintf(stderr, "[d3d9] CreateDevice -> %08x\n", (unsigned)r);
    pp_out(&pp, ARG(c, 5));
    if (ARG(c, 6)) U32(ARG(c, 6)) = SUCCEEDED(r) ? wrap(dev, iface_type("IDirect3DDevice9")) : 0;
    ret_std(c, (uint32_t)r, 7);
}
void special_CreateDeviceEx(CPU *c, int) {   // (this, adapter, type, focus, flags, pp*, D3DDISPLAYMODEEX*, out**)
    auto *d3d = (IDirect3D9Ex *)nat(ARG(c, 0));
    D3DPRESENT_PARAMETERS pp;
    pp_in(ARG(c, 5), &pp);
    IDirect3DDevice9Ex *dev = nullptr;
    HRESULT r = d3d->CreateDeviceEx(ARG(c, 1), (D3DDEVTYPE)ARG(c, 2), native_hwnd(ARG(c, 3)), ARG(c, 4), &pp,
                                    (D3DDISPLAYMODEEX *)hptr(ARG(c, 6)), &dev);
    fprintf(stderr, "[d3d9] CreateDeviceEx -> %08x\n", (unsigned)r);
    pp_out(&pp, ARG(c, 5));
    if (ARG(c, 7)) U32(ARG(c, 7)) = SUCCEEDED(r) ? wrap(dev, iface_type("IDirect3DDevice9Ex")) : 0;
    ret_std(c, (uint32_t)r, 8);
}
void special_Reset(CPU *c, int) {   // (this, pp*)
    auto *dev = (IDirect3DDevice9 *)nat(ARG(c, 0));
    D3DPRESENT_PARAMETERS pp;
    pp_in(ARG(c, 1), &pp);
    HRESULT r = dev->Reset(&pp);
    pp_out(&pp, ARG(c, 1));
    ret_std(c, (uint32_t)r, 2);
}
void special_ResetEx(CPU *c, int) {   // (this, pp*, D3DDISPLAYMODEEX*)
    auto *dev = (IDirect3DDevice9Ex *)nat(ARG(c, 0));
    D3DPRESENT_PARAMETERS pp;
    pp_in(ARG(c, 1), &pp);
    HRESULT r = dev->ResetEx(&pp, (D3DDISPLAYMODEEX *)hptr(ARG(c, 2)));
    pp_out(&pp, ARG(c, 1));
    ret_std(c, (uint32_t)r, 3);
}
void special_CreateAdditionalSwapChain(CPU *c, int) {   // (this, pp*, out**)
    auto *dev = (IDirect3DDevice9 *)nat(ARG(c, 0));
    D3DPRESENT_PARAMETERS pp;
    pp_in(ARG(c, 1), &pp);
    IDirect3DSwapChain9 *sc = nullptr;
    HRESULT r = dev->CreateAdditionalSwapChain(&pp, &sc);
    pp_out(&pp, ARG(c, 1));
    if (ARG(c, 2)) U32(ARG(c, 2)) = SUCCEEDED(r) ? wrap(sc, iface_type("IDirect3DSwapChain9")) : 0;
    ret_std(c, (uint32_t)r, 3);
}
void special_GetPresentParameters(CPU *c, int) {   // swap chain: (this, pp*)
    auto *sc = (IDirect3DSwapChain9 *)nat(ARG(c, 0));
    D3DPRESENT_PARAMETERS pp;
    HRESULT r = sc->GetPresentParameters(&pp);
    if (SUCCEEDED(r)) pp_out(&pp, ARG(c, 1));
    ret_std(c, (uint32_t)r, 2);
}
void special_GetCreationParameters(CPU *c, int) {   // (this, D3DDEVICE_CREATION_PARAMETERS*): guest 16 bytes
    auto *dev = (IDirect3DDevice9 *)nat(ARG(c, 0));
    D3DDEVICE_CREATION_PARAMETERS p;
    HRESULT r = dev->GetCreationParameters(&p);
    uint32_t g = ARG(c, 1);
    if (SUCCEEDED(r)) { U32(g) = p.AdapterOrdinal; U32(g + 4) = p.DeviceType; U32(g + 8) = guest_hwnd(p.hFocusWindow); U32(g + 12) = p.BehaviorFlags; }
    ret_std(c, (uint32_t)r, 2);
}
void special_GetAdapterMonitor(CPU *c, int) { ret_std(c, 0x4D4F4E31u, 2); }   // a stable fake HMONITOR
void special_GetDC(CPU *c, int) { ret_std(c, (uint32_t)D3DERR_INVALIDCALL, 2); }   // ponytail: GDI on surfaces not supported
void special_ReleaseDC(CPU *c, int) { ret_std(c, (uint32_t)D3DERR_INVALIDCALL, 2); }

// ---------- object lifetime ----------

void special_Release(CPU *c, int) {
    uint32_t g = ARG(c, 0);
    auto *u = (IUnknown *)nat(g);
    ULONG r = u ? u->Release() : 0;
    if (u && r == 0) unwrap_dead(g);
    ret_std(c, r, 1);
}
// which interface an IID names (for out-pointers typed by a riid argument); unknown -> deflt
static int iid_type(const GUID &riid, int deflt) {
    static const struct { const char *name; GUID iid; } known[] = {
        {"IDirect3D9", __uuidof(IDirect3D9)}, {"IDirect3D9Ex", __uuidof(IDirect3D9Ex)},
        {"IDirect3DDevice9", __uuidof(IDirect3DDevice9)}, {"IDirect3DDevice9Ex", __uuidof(IDirect3DDevice9Ex)},
        {"IDirect3DSwapChain9", __uuidof(IDirect3DSwapChain9)}, {"IDirect3DSwapChain9Ex", __uuidof(IDirect3DSwapChain9Ex)},
        {"IDirect3DSurface9", __uuidof(IDirect3DSurface9)}, {"IDirect3DTexture9", __uuidof(IDirect3DTexture9)},
        {"IDirect3DCubeTexture9", __uuidof(IDirect3DCubeTexture9)}, {"IDirect3DVolumeTexture9", __uuidof(IDirect3DVolumeTexture9)},
        {"IDirect3DBaseTexture9", __uuidof(IDirect3DBaseTexture9)}, {"IDirect3DResource9", __uuidof(IDirect3DResource9)},
        {"IDirect3DVertexBuffer9", __uuidof(IDirect3DVertexBuffer9)}, {"IDirect3DIndexBuffer9", __uuidof(IDirect3DIndexBuffer9)},
    };
    for (auto &k : known) if (!memcmp(&k.iid, &riid, sizeof riid)) return iface_type(k.name);
    return deflt;
}

void special_QueryInterface(CPU *c, int type) {   // (this, riid, out**)
    auto *u = (IUnknown *)nat(ARG(c, 0));
    const GUID &riid = *(const GUID *)GM(ARG(c, 1));
    void *p = nullptr;
    HRESULT r = u->QueryInterface(riid, &p);
    int t = iid_type(riid, type);   // default: the caller's own type
    if (SUCCEEDED(r) && p) {
        Guard l;
        // a different interface on the same object (e.g. Device9 -> Device9Ex) gets its own wrapper type
        auto it = by_native.find(p);
        if (it != by_native.end() && wraps[it->second].type != t && t >= 0) {
            uint32_t old = wraps[it->second].gobj;
            U32(old) = vtable_for(t);   // upgrade the existing wrapper's vtable to the richer interface
            wraps[it->second].type = t;
        }
    }
    if (ARG(c, 2)) U32(ARG(c, 2)) = SUCCEEDED(r) ? wrap(p, t) : 0;
    ret_std(c, (uint32_t)r, 3);
}

void special_GetContainer(CPU *c, int type) {   // Surface9/Volume9 (this, riid, out**): the texture/swap chain owning it
    void *n = nat(ARG(c, 0)), *p = nullptr;
    const GUID &riid = *(const GUID *)GM(ARG(c, 1));
    HRESULT r = type == iface_type("IDirect3DVolume9") ? ((IDirect3DVolume9 *)n)->GetContainer(riid, &p)
                                                      : ((IDirect3DSurface9 *)n)->GetContainer(riid, &p);
    if (ARG(c, 2)) U32(ARG(c, 2)) = SUCCEEDED(r) && p ? wrap(p, iid_type(riid, -1)) : 0;
    ret_std(c, (uint32_t)r, 3);
}

// ---------- Lock / Unlock with guest staging buffers ----------

static void fmt_info(D3DFORMAT f, uint32_t *bpp, uint32_t *block) {   // bytes per pixel, or per 4x4 block
    *block = 0;
    switch ((uint32_t)f) {
    case D3DFMT_DXT1: *block = 8; *bpp = 0; return;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: *block = 16; *bpp = 0; return;
    case D3DFMT_L8: case D3DFMT_A8: case D3DFMT_P8: case D3DFMT_A4L4: case D3DFMT_R3G3B2: *bpp = 1; return;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
    case D3DFMT_A8L8: case D3DFMT_L16: case D3DFMT_R16F: case D3DFMT_V8U8: case D3DFMT_D16: case D3DFMT_D16_LOCKABLE:
    case D3DFMT_A8R3G3B2: *bpp = 2; return;
    case D3DFMT_R8G8B8: *bpp = 3; return;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: *bpp = 8; return;
    case D3DFMT_A32B32G32R32F: *bpp = 16; return;
    default: *bpp = 4; return;   // A8R8G8B8, X8R8G8B8, A2R10G10B10, G16R16, R32F, D24S8, ...
    }
}
// bytes the game may touch from pBits: (rows - 1) * pitch + the bytes of one row
static uint32_t region_bytes(D3DFORMAT f, uint32_t w, uint32_t h, uint32_t pitch) {
    uint32_t bpp, block;
    fmt_info(f, &bpp, &block);
    uint32_t rows = block ? (h + 3) / 4 : h, row = block ? (w + 3) / 4 * block : w * bpp;
    if (!rows) return 0;
    if (row > pitch) row = pitch;
    return (rows - 1) * pitch + row;
}
static uint32_t begin_lock(uint32_t g, uint32_t key, uint8_t *native, uint32_t size, uint32_t flags) {
    Guard l;
    Wrap *w = wrap_of(g);
    uint32_t buf = gmalloc(size ? size : 1);
    if (!(flags & D3DLOCK_DISCARD)) memcpy(GM(buf), native, size);   // the game may read or partially overwrite
    w->locks.push_back(LockState{key, buf, size, flags, native});
    return buf;
}
static void end_lock(uint32_t g, uint32_t key) {
    Guard l;
    Wrap *w = wrap_of(g);
    if (!w) return;
    for (size_t i = 0; i < w->locks.size(); i++) {
        LockState k = w->locks[i];
        if (k.key != key) continue;
        if (!(k.flags & D3DLOCK_READONLY)) memcpy(k.native, GM(k.guest), k.size);
        gfree(k.guest);
        w->locks.erase(w->locks.begin() + i);
        return;
    }
}
static void rect_dims(const D3DSURFACE_DESC &d, uint32_t rect, uint32_t *w, uint32_t *h) {
    if (rect) { *w = U32(rect + 8) - U32(rect); *h = U32(rect + 12) - U32(rect + 4); }
    else { *w = d.Width; *h = d.Height; }
}

void special_LockRect(CPU *c, int type) {
    uint32_t g = ARG(c, 0);
    std::string name = iface_names[type];
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    uint32_t key, lrp, rect, flags, nargs;
    HRESULT r;
    if (name == "IDirect3DSurface9") {   // (this, lr*, rect*, flags)
        auto *s = (IDirect3DSurface9 *)nat(g);
        lrp = ARG(c, 1); rect = ARG(c, 2); flags = ARG(c, 3); key = 0; nargs = 4;
        s->GetDesc(&d);
        r = s->LockRect(&lr, (const RECT *)hptr(rect), flags);
    } else if (name == "IDirect3DTexture9") {   // (this, level, lr*, rect*, flags)
        auto *t = (IDirect3DTexture9 *)nat(g);
        key = ARG(c, 1); lrp = ARG(c, 2); rect = ARG(c, 3); flags = ARG(c, 4); nargs = 5;
        t->GetLevelDesc(key, &d);
        r = t->LockRect(key, &lr, (const RECT *)hptr(rect), flags);
    } else {   // IDirect3DCubeTexture9: (this, face, level, lr*, rect*, flags)
        auto *t = (IDirect3DCubeTexture9 *)nat(g);
        key = ARG(c, 1) << 16 | ARG(c, 2); lrp = ARG(c, 3); rect = ARG(c, 4); flags = ARG(c, 5); nargs = 6;
        t->GetLevelDesc(ARG(c, 2), &d);
        r = t->LockRect((D3DCUBEMAP_FACES)ARG(c, 1), ARG(c, 2), &lr, (const RECT *)hptr(rect), flags);
    }
    if (SUCCEEDED(r)) {
        uint32_t w, h;
        rect_dims(d, rect, &w, &h);
        uint32_t size = region_bytes(d.Format, w, h, (uint32_t)lr.Pitch);
        U32(lrp) = (uint32_t)lr.Pitch;
        U32(lrp + 4) = begin_lock(g, key, (uint8_t *)lr.pBits, size, flags);
    }
    ret_std(c, (uint32_t)r, nargs);
}
void special_UnlockRect(CPU *c, int type) {
    uint32_t g = ARG(c, 0);
    std::string name = iface_names[type];
    HRESULT r;
    if (name == "IDirect3DSurface9") { end_lock(g, 0); r = ((IDirect3DSurface9 *)nat(g))->UnlockRect(); ret_std(c, (uint32_t)r, 1); }
    else if (name == "IDirect3DTexture9") { end_lock(g, ARG(c, 1)); r = ((IDirect3DTexture9 *)nat(g))->UnlockRect(ARG(c, 1)); ret_std(c, (uint32_t)r, 2); }
    else {
        end_lock(g, ARG(c, 1) << 16 | ARG(c, 2));
        r = ((IDirect3DCubeTexture9 *)nat(g))->UnlockRect((D3DCUBEMAP_FACES)ARG(c, 1), ARG(c, 2));
        ret_std(c, (uint32_t)r, 3);
    }
}
// vertex / index buffers: (this, offset, size, void** data, flags); size 0 = to the end
// ponytail: copies the locked range through a fresh guest buffer every lock; keep a persistent per-buffer mirror if
// dynamic buffers show up in profiles.
void special_Lock(CPU *c, int type) {
    uint32_t g = ARG(c, 0), off = ARG(c, 1), size = ARG(c, 2), out = ARG(c, 3), flags = ARG(c, 4);
    std::string name = iface_names[type];
    void *p = nullptr;
    HRESULT r;
    uint32_t total, usage, pool;
    uint64_t t0 = SDL_GetTicksNS();
    if (name == "IDirect3DVertexBuffer9") {
        auto *b = (IDirect3DVertexBuffer9 *)nat(g);
        D3DVERTEXBUFFER_DESC d; b->GetDesc(&d); total = d.Size; usage = d.Usage; pool = d.Pool;
        r = b->Lock(off, size, &p, flags);
    } else {
        auto *b = (IDirect3DIndexBuffer9 *)nat(g);
        D3DINDEXBUFFER_DESC d; b->GetDesc(&d); total = d.Size; usage = d.Usage; pool = d.Pool;
        r = b->Lock(off, size, &p, flags);
    }
    // performance diagnosis: locks that made the CPU wait for the GPU (first 300 over 1 ms)
    static int slow;
    uint64_t dt = SDL_GetTicksNS() - t0;
    if (dt > 1000000 && slow++ < 300)
        fprintf(stderr, "[d3d9] slow %s lock %.1f ms: off %u size %u of %u flags %x usage %x pool %u (guest %08x)\n",
                name.c_str() + 9, dt / 1e6, off, size, total, flags, usage, pool, g);
    if (!size) size = total - off;
    if (SUCCEEDED(r) && out) U32(out) = begin_lock(g, 0, (uint8_t *)p, size, flags);
    ret_std(c, (uint32_t)r, 5);
}
void special_Unlock(CPU *c, int type) {
    uint32_t g = ARG(c, 0);
    end_lock(g, 0);
    HRESULT r = std::string(iface_names[type]) == "IDirect3DVertexBuffer9" ? ((IDirect3DVertexBuffer9 *)nat(g))->Unlock()
                                                                           : ((IDirect3DIndexBuffer9 *)nat(g))->Unlock();
    ret_std(c, (uint32_t)r, 1);
}
// volumes: Volume9 (this, lb*, box*, flags), VolumeTexture9 (this, level, lb*, box*, flags); D3DLOCKED_BOX = row, slice, bits
void special_LockBox(CPU *c, int type) {
    uint32_t g = ARG(c, 0), key, lbp, box, flags, nargs;
    D3DVOLUME_DESC d;
    D3DLOCKED_BOX lb;
    HRESULT r;
    if (std::string(iface_names[type]) == "IDirect3DVolume9") {
        auto *v = (IDirect3DVolume9 *)nat(g);
        key = 0; lbp = ARG(c, 1); box = ARG(c, 2); flags = ARG(c, 3); nargs = 4;
        v->GetDesc(&d);
        r = v->LockBox(&lb, (const D3DBOX *)hptr(box), flags);
    } else {
        auto *v = (IDirect3DVolumeTexture9 *)nat(g);
        key = ARG(c, 1); lbp = ARG(c, 2); box = ARG(c, 3); flags = ARG(c, 4); nargs = 5;
        v->GetLevelDesc(key, &d);
        r = v->LockBox(key, &lb, (const D3DBOX *)hptr(box), flags);
    }
    if (SUCCEEDED(r)) {
        uint32_t w = box ? U32(box + 4) - U32(box) : d.Width, h = box ? U32(box + 12) - U32(box + 8) : d.Height;
        uint32_t depth = box ? U32(box + 20) - U32(box + 16) : d.Depth;
        uint32_t size = depth ? (depth - 1) * (uint32_t)lb.SlicePitch + region_bytes(d.Format, w, h, (uint32_t)lb.RowPitch) : 0;
        U32(lbp) = (uint32_t)lb.RowPitch;
        U32(lbp + 4) = (uint32_t)lb.SlicePitch;
        U32(lbp + 8) = begin_lock(g, key, (uint8_t *)lb.pBits, size, flags);
    }
    ret_std(c, (uint32_t)r, nargs);
}
void special_UnlockBox(CPU *c, int type) {
    uint32_t g = ARG(c, 0);
    HRESULT r;
    if (std::string(iface_names[type]) == "IDirect3DVolume9") { end_lock(g, 0); r = ((IDirect3DVolume9 *)nat(g))->UnlockBox(); ret_std(c, (uint32_t)r, 1); }
    else { end_lock(g, ARG(c, 1)); r = ((IDirect3DVolumeTexture9 *)nat(g))->UnlockBox(ARG(c, 1)); ret_std(c, (uint32_t)r, 2); }
}
