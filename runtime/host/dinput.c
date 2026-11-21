// DirectInput 8 on SDL3: guest COM objects (stdcall, `this` = first stack argument) for the system keyboard and mouse.
// Each vtable slot is a named stub, so a method the game calls that isn't written yet stops with its name.
// Joysticks: none are enumerated (the gamepad goes through XInput).
#include "host.h"
#include <string.h>
#include <time.h>

static uint32_t ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }

#define DI "DINPUT8.dll"
enum { K_DI, K_KEYBOARD, K_MOUSE };
// guest object: +0 vtable, +4 kind, +8 refcount, +12 data format size (GetDeviceState size)
#define KIND(o) U32((o) + 4)
#define REFS(o) U32((o) + 8)

static const char *di_methods[] = {"QueryInterface", "AddRef", "Release", "CreateDevice", "EnumDevices",
    "GetDeviceStatus", "RunControlPanel", "Initialize", "FindDevice", "EnumDevicesBySemantics", "ConfigureDevices"};
static const char *dev_methods[] = {"QueryInterface", "AddRef", "Release", "GetCapabilities", "EnumObjects",
    "GetProperty", "SetProperty", "Acquire", "Unacquire", "GetDeviceState", "GetDeviceData", "SetDataFormat",
    "SetEventNotification", "SetCooperativeLevel", "GetObjectInfo", "GetDeviceInfo", "RunControlPanel", "Initialize",
    "CreateEffect", "EnumEffects", "GetEffectInfo", "GetForceFeedbackState", "SendForceFeedbackCommand",
    "EnumCreatedEffectObjects", "Escape", "Poll", "SendDeviceData", "EnumEffectsInFile", "WriteEffectToFile",
    "BuildActionMap", "SetActionMap", "GetImageInfo"};

static uint32_t make_vtable(const char *iface, const char **names, int n) {
    uint32_t vt = gmalloc(4 * n);
    char buf[96];
    for (int i = 0; i < n; i++) {
        snprintf(buf, sizeof buf, "%s::%s", iface, names[i]);
        U32(vt + 4 * i) = named_stub(DI, strdup(buf));
    }
    return vt;
}
static uint32_t new_obj(int kind) {
    static uint32_t vt_di, vt_dev;
    if (!vt_di) vt_di = make_vtable("IDirectInput8A", di_methods, 11);
    if (!vt_dev) vt_dev = make_vtable("IDirectInputDevice8A", dev_methods, 32);
    uint32_t o = gmalloc(16);
    U32(o) = kind == K_DI ? vt_di : vt_dev;
    KIND(o) = kind; REFS(o) = 1; U32(o + 12) = 0;
    return o;
}

IMP(DI, DirectInput8Create) {   // (hinst, version, riid, out, outer)
    U32(ARG(c, 3)) = new_obj(K_DI);
    ret_std(c, 0, 5);
}

#define M(iface, name) IMPX(DI, #iface "::" #name, iface##_##name)
M(IDirectInput8A, AddRef) { ret_std(c, ++REFS(ARG(c, 0)), 1); }
M(IDirectInput8A, Release) { ret_std(c, REFS(ARG(c, 0)) ? --REFS(ARG(c, 0)) : 0, 1); }   // ponytail: never freed
M(IDirectInput8A, CreateDevice) {   // (this, rguid, out, outer): GUID_SysKeyboard / GUID_SysMouse only
    uint32_t g = ARG(c, 1), d1 = U32(g);
    static const uint8_t tail[12] = {0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
    int sys = !memcmp(GM(g + 4), tail, 12);
    int kind = sys && d1 == 0x6F1D2B61 ? K_KEYBOARD : sys && d1 == 0x6F1D2B60 ? K_MOUSE : -1;
    fprintf(stderr, "[dinput] CreateDevice %08x -> %s\n", d1, kind == K_KEYBOARD ? "keyboard" : kind == K_MOUSE ? "mouse" : "none");
    if (kind < 0) { U32(ARG(c, 2)) = 0; ret_std(c, 0x80040154, 4); return; }   // DIERR_DEVICENOTREG
    U32(ARG(c, 2)) = new_obj(kind);
    ret_std(c, 0, 4);
}
// (this, type, callback, ref, flags): the system keyboard and mouse; no game controllers.
// type: 0 all, 2 pointer, 3 keyboard (DI8DEVCLASS_*) or a DI8DEVTYPE_* (0x12 mouse, 0x13 keyboard)
M(IDirectInput8A, EnumDevices) {
    uint32_t type = ARG(c, 1), cb = ARG(c, 2), ref = ARG(c, 3), ddi = gmalloc(0x244);
    for (int k = 0; k < 2; k++) {   // DIDEVICEINSTANCEA (580 bytes)
        int kb = k == 0;
        if (!(type == 0 || type == (kb ? 3u : 2u) || type == (kb ? 0x13u : 0x12u))) continue;
        static const uint8_t tail[12] = {0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
        memset(GM(ddi), 0, 0x244);
        U32(ddi) = 0x244;
        for (int g = 4; g <= 0x14; g += 0x10) { U32(ddi + g) = kb ? 0x6F1D2B61 : 0x6F1D2B60; memcpy(GM(ddi + g + 4), tail, 12); }
        U32(ddi + 0x24) = kb ? 0x413 : 0x112;   // keyboard: PC enhanced; mouse: traditional
        strcpy(GSTR(ddi + 0x28), kb ? "Keyboard" : "Mouse");
        strcpy(GSTR(ddi + 0x12C), kb ? "Keyboard" : "Mouse");
        V(GM(ddi + 0x240), uint16_t) = 1;       // HID usage page: generic desktop
        V(GM(ddi + 0x242), uint16_t) = kb ? 6 : 2;
        uint32_t esp = REG(c, R_ESP), more = guest_call(c, cb, 1, 2, ddi, ref);
        REG(c, R_ESP) = esp;
        if (!more) break;                         // DIENUM_STOP
    }
    gfree(ddi);
    ret_std(c, 0, 5);
}
// (this, user, actionformat, callback, ref, flags): action-mapped devices; none for now
M(IDirectInput8A, EnumDevicesBySemantics) { ret_std(c, 0, 6); }

#define D(name) M(IDirectInputDevice8A, name)
D(AddRef) { ret_std(c, ++REFS(ARG(c, 0)), 1); }
D(Release) { ret_std(c, REFS(ARG(c, 0)) ? --REFS(ARG(c, 0)) : 0, 1); }
D(SetDataFormat) { U32(ARG(c, 0) + 12) = U32(ARG(c, 1) + 12); ret_std(c, 0, 2); }   // DIDATAFORMAT.dwDataSize
D(SetCooperativeLevel) { ret_std(c, 0, 3); }
D(SetProperty) { ret_std(c, 0, 3); }        // DIPROP_BUFFERSIZE etc.: our event buffer has no size limit
D(Acquire) { ret_std(c, 0, 1); }
D(Unacquire) { ret_std(c, 0, 1); }
D(Poll) { ret_std(c, 1, 1); }               // DI_NOEFFECT: keyboard and mouse need no polling

// (this, callback, ref, flags): DIDEVICEOBJECTINSTANCEA (316 bytes) per key, or per mouse axis/button.
// flags filter by DIDFT_* type bits (0 = all): 3 axes, 0xC buttons.
D(EnumObjects) {
    uint32_t dev = ARG(c, 0), cb = ARG(c, 1), ref = ARG(c, 2), flags = ARG(c, 3) & 0xFF, o = gmalloc(316);
    int kb = KIND(dev) == K_KEYBOARD, n = kb ? 256 : 3 + 5;
    for (int i = 0; i < n; i++) {
        const char *name;
        static const char *mouse_names[] = {"X-axis", "Y-axis", "Wheel", "Button 0", "Button 1", "Button 2", "Button 3", "Button 4"};
        uint32_t d1, ofs, type;   // guidType Data1: GUID_Key 55728220, GUID_XAxis A36D02E0 (+1 Y, +2 Z), GUID_Button A36D02F0
        uint16_t d2;
        if (kb) {
            if (!(name = input_key_name(i))) continue;
            d1 = 0x55728220; d2 = 0xD33C; ofs = i; type = (uint32_t)i << 8 | 4;      // DIDFT_PSHBUTTON, instance = DIK
        } else {
            name = mouse_names[i];
            d1 = i < 3 ? 0xA36D02E0 + i : 0xA36D02F0; d2 = 0xC9F3;
            ofs = i < 3 ? 4 * i : 12 + (i - 3);
            type = i < 3 ? (uint32_t)i << 8 | 1 : (uint32_t)(i - 3) << 8 | 4;   // DIDFT_RELAXIS / DIDFT_PSHBUTTON
        }
        if (flags && !(flags & type & 0xFF)) continue;
        memset(GM(o), 0, 316);
        static const uint8_t g_tail[10] = {0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
        U32(o) = 316; U32(o + 4) = d1; V(GM(o + 8), uint16_t) = d2; memcpy(GM(o + 10), g_tail, 10);
        U32(o + 0x14) = ofs; U32(o + 0x18) = type;
        snprintf(GSTR(o + 0x20), 260, "%s", name);
        uint32_t esp = REG(c, R_ESP), more = guest_call(c, cb, 1, 2, o, ref);
        REG(c, R_ESP) = esp;
        if (!more) break;
    }
    gfree(o);
    ret_std(c, 0, 4);
}

// current state in DirectInput layout: keyboard 256 bytes; mouse DIMOUSESTATE2 = lX, lY, lZ, 8 buttons (20 bytes)
static void snapshot(int kind, uint8_t *out) {
    if (kind == K_KEYBOARD) { input_keys(out); return; }
    int32_t d[3];
    input_mouse(d, out + 12);
    memcpy(out, d, 12);
}
D(GetDeviceState) {   // (this, size, data)
    uint8_t s[256];
    snapshot(KIND(ARG(c, 0)), s);
    uint32_t n = ARG(c, 1) < 256 ? ARG(c, 1) : 256;
    memcpy(GM(ARG(c, 2)), s, n);
    ret_std(c, 0, 3);
}
// Buffered input = differences between snapshots. DIDEVICEOBJECTDATA (DX8): ofs, data, timestamp, sequence, appdata.
// ponytail: one device of each kind; events of a call get the same timestamp
static uint8_t prev[3][256];
D(GetDeviceData) {   // (this, cbObjectData, rgdod, pdwInOut, flags: DIGDD_PEEK = 1)
    static uint32_t seq;
    int kind = KIND(ARG(c, 0));
    uint32_t sz = ARG(c, 1), out = ARG(c, 2), cap = U32(ARG(c, 3)), n = 0, now = ms_now();
    uint8_t s[256];
    snapshot(kind, s);
    int len = kind == K_KEYBOARD ? 256 : 20;
    for (int ofs = 0; ofs < len && n < cap; ofs++) {
        uint32_t v;
        if (kind == K_MOUSE && ofs < 12) {         // axes: report motion as a relative event
            if (ofs % 4) continue;
            memcpy(&v, s + ofs, 4);
            if (!v) continue;
        } else {
            if (s[ofs] == prev[kind][ofs]) continue;
            v = s[ofs];
        }
        if (out) {
            uint32_t e = out + sz * n;
            memset(GM(e), 0, sz);
            U32(e) = ofs; U32(e + 4) = v; U32(e + 8) = now; U32(e + 12) = ++seq;
        }
        n++;
    }
    if (!(ARG(c, 4) & 1)) memcpy(prev[kind], s, 256);
    U32(ARG(c, 3)) = n;
    ret_std(c, 0, 5);
}
