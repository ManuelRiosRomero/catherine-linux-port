// Native host: maps Catherine.exe into guest memory, wires its imports to our shims, and runs the lifted code.
// Usage: catherine <path/to/Catherine.exe>   (run from the game folder; CATH_TRACE=1 logs every import call)
#define _GNU_SOURCE
#include "host.h"
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

static char *imp_names[4096];        // "DLL!name" per import stub index
static Fn imp_fns[4096];
static uint32_t n_imps, missing;
static int trace;

static void missing_import(CPU *c) {
    die("missing import %s (called from %08x, args %08x %08x %08x %08x)", imp_names[missing], U32(REG(c, R_ESP)),
        ARG(c, 0), ARG(c, 1), ARG(c, 2), ARG(c, 3));
}

static uint32_t new_stub(const char *dll, const char *name, Fn f) {
    for (uint32_t i = 0; f && i < n_imps; i++) if (imp_fns[i] == f) return G_STUBS + 16 * i;   // NULL: keep each name
    if (n_imps == 4096) die("too many imports");
    imp_names[n_imps] = malloc(strlen(dll) + strlen(name) + 2);
    sprintf(imp_names[n_imps], "%s!%s", dll, name);
    imp_fns[n_imps] = f;
    return G_STUBS + 16 * n_imps++;
}

// GetProcAddress: give an implemented function a fresh stub address (same mechanism as static imports).
uint32_t dyn_stub(const char *dll, const char *name) {
    Fn f = import_handler(dll, name);
    if (!f) f = import_handler(NULL, name);           // api-ms-win-* forwarders: match the name in any DLL
    return f ? new_stub(dll, name, f) : 0;
}

// Guest COM vtable slots: a stub even when not implemented yet; calling one then stops with "missing import dll!name".
uint32_t named_stub(const char *dll, const char *name) { return new_stub(dll, name, import_handler(dll, name)); }

extern const NativeFn __start_cath_natives[], __stop_cath_natives[];

static Fn fallback(uint32_t t) {
    for (const NativeFn *n = __start_cath_natives; n < __stop_cath_natives; n++) if (n->addr == t) return n->fn;
    Fn s = steam_resolve(t);
    if (s) return s;
    if ((s = d3d9_resolve(t))) return s;
    uint32_t i = (t - G_STUBS) / 16;
    if (t < G_STUBS || i >= n_imps) return NULL;
    if (trace) fprintf(stderr, "[imp] %s\n", imp_names[i]);
    if (imp_fns[i]) return imp_fns[i];
    missing = i;
    return missing_import;
}

// Translated Microsoft DLLs: rebased copies (tools/recomp/rebase.py) next to this binary, mapped at their own
// ImageBase, their code lifted into the same fn_table as the exe. D3DCompiler first: d3dx9 LoadLibrary's it.
static const char *dll_files[] = {"D3DCompiler_43.dll", "d3dx9_43.dll"};
static uint32_t dll_bases[2];

uint32_t dll_base(const char *name) {
    for (int i = 0; i < 2; i++) if (dll_bases[i] && !strcasecmp(name, dll_files[i])) return dll_bases[i];
    return 0;
}
uint32_t dll_is_loaded(uint32_t h) {
    for (int i = 0; i < 2; i++) if (dll_bases[i] && h == dll_bases[i]) return 1;
    return 0;
}

// Export by name, or by ordinal when name < 0x10000 (GetProcAddress convention). 0 if absent.
uint32_t dll_export(uint32_t base, uint32_t name) {
    uint32_t pe = base + U32(base + 0x3C), ed = base + U32(pe + 0x78);
    uint32_t nfn = U32(ed + 0x14), nnames = U32(ed + 0x18), fns = base + U32(ed + 0x1C);
    uint32_t names = base + U32(ed + 0x20), ords = base + U32(ed + 0x24), i = 0xFFFFFFFF;
    if (name < 0x10000) i = name - U32(ed + 0x10);
    else for (uint32_t k = 0; k < nnames; k++)
        if (!strcmp(GSTR(base + U32(names + 4 * k)), GSTR(name))) { i = V(GM(ords + 2 * k), uint16_t); break; }
    if (i >= nfn || !U32(fns + 4 * i)) return 0;
    uint32_t rva = U32(fns + 4 * i);
    if (rva >= U32(pe + 0x78) && rva < U32(pe + 0x78) + U32(pe + 0x7C)) die("forwarded export %s", GSTR(base + rva));
    return base + rva;
}

// msvcrt.dll exports a few variables: the IAT slot must hold the variable's guest address, not a stub.
static uint32_t data_import(const char *dll, const char *name) {
    if (!strcasecmp(dll, "MSVCP140.dll") && !strncmp(name, "?id@", 4)) {   // a facet's static locale::id: numbered lazily
        uint32_t p = gmalloc(4);
        U32(p) = 0;
        return p;
    }
    if (strcasecmp(dll, "msvcrt.dll")) return 0;
    if (!strcmp(name, "_iob")) return stdio_iob();
    if (!strcmp(name, "__pioinfo") || !strcmp(name, "__badioinfo")) {   // lowio tables: zeroed (never set up)
        uint32_t p = gmalloc(0x1000);
        memset(GM(p), 0, 0x1000);
        return p;
    }
    return 0;
}

// Imports by ordinal get a name like "#12" (none expected in this exe).
static void bind_imports(uint32_t base) {
    uint32_t pe = base + U32(base + 0x3C), dir = U32(pe + 0x80);  // data directory 1 = imports
    for (uint32_t d = base + dir; U32(d + 12); d += 20) {
        const char *dll = (const char *)GM(base + U32(d + 12));
        uint32_t names = U32(d) ? U32(d) : U32(d + 16), iat = base + U32(d + 16);
        for (uint32_t k = 0; U32(base + names + 4 * k); k++) {
            uint32_t th = U32(base + names + 4 * k);
            char buf[256];
            if (th & 0x80000000u) snprintf(buf, sizeof buf, "#%u", th & 0xFFFF);
            else snprintf(buf, sizeof buf, "%s", (const char *)GM(base + th + 2));
            uint32_t mod = dll_base(dll), direct = mod ? dll_export(mod, th & 0x80000000u ? th & 0xFFFF : base + th + 2)
                                                      : data_import(dll, buf);
            if (mod && !direct) die("%s has no export %s", dll, buf);
            if (direct) { U32(iat + 4 * k) = direct; continue; }   // lifted DLL code or guest data: no stub
            if (n_imps == 4096) die("too many imports");
            imp_names[n_imps] = malloc(strlen(dll) + strlen(buf) + 2);
            sprintf(imp_names[n_imps], "%s!%s", dll, buf);
            imp_fns[n_imps] = import_handler(dll, buf);
            // readiness report (tools/recomp/readiness.py): an import the game would stop on, with its IAT slot
            if (!imp_fns[n_imps] && getenv("CATH_REPORT")) printf("missing-import %s!%s %08x\n", dll, buf, iat + 4 * k);
            U32(iat + 4 * k) = G_STUBS + 16 * n_imps;
            n_imps++;
        }
    }
}

static uint32_t load_pe(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    uint8_t *d = malloc(n);
    rewind(f);
    if (fread(d, 1, n, f) != (size_t)n) die("read failed: %s", path);
    fclose(f);
    uint32_t pe = *(uint32_t *)(d + 0x3C), opt = pe + 24;
    uint16_t nsec = *(uint16_t *)(d + pe + 6), optsz = *(uint16_t *)(d + pe + 20);
    uint32_t base = *(uint32_t *)(d + opt + 28), hdrs = *(uint32_t *)(d + opt + 60);
    memcpy(GM(base), d, hdrs);
    for (int i = 0; i < nsec; i++) {
        uint8_t *s = d + opt + optsz + 40 * i;
        uint32_t vsz = *(uint32_t *)(s + 8), va = *(uint32_t *)(s + 12), rsz = *(uint32_t *)(s + 16), raw = *(uint32_t *)(s + 20);
        memcpy(GM(base + va), d + raw, vsz && vsz < rsz ? vsz : rsz);
    }
    free(d);
    return base;
}

// Runs guest code at fn from the current CPU state; on a stop, prints registers + EBP backtrace and exits.
static void enter(CPU *c, uint32_t fn) {
    int r = rt_enter(c, fn);
    if (r) {
        // Registers are exactly where the lifted code stopped: print them and walk the EBP frame chain.
        // [ESP] = return address of the call that left lifted code (CALL pushes it before jumping)
        fprintf(stderr, "catherine: stopped: EAX=%08x ECX=%08x EDX=%08x ESI=%08x EDI=%08x ESP=%08x EBP=%08x [ESP]=%08x\n  backtrace:",
                REG(c, 0), REG(c, 4), REG(c, 8), REG(c, 0x18), REG(c, 0x1C), REG(c, R_ESP), REG(c, 0x14), U32(REG(c, R_ESP)));
        for (uint32_t ebp = REG(c, 0x14), i = 0; i < 20 && ebp >= G_STACK_LO && ebp < G_STACK_HI; i++, ebp = U32(ebp))
            fprintf(stderr, " %08x", U32(ebp + 4));
        fprintf(stderr, "\n");
    }
    if (r == 1) die("jumped to unknown address %08x", rt_left_at);
    if (r == 2) die("unimplemented: %s", rt_msg);
}

char **host_argv;
char host_cwd[4096];

int main(int argc, char **argv) {
    if (argc < 2) die("usage: catherine <Catherine.exe>");
    host_argv = argv;
    if (!getcwd(host_cwd, sizeof host_cwd)) host_cwd[0] = 0;
    // The game rewrites small WRITEONLY vertex/index buffers (D3DPOOL_DEFAULT, no lock flags) every frame. DXVK maps
    // those directly, so each lock waited for the GPU (30% of the main thread, 52 FPS with the GPU 37% busy in Stage 1).
    // Through DXVK's staging copy instead: 76-90 FPS. Game speed does not follow the frame rate (an in-engine cutscene
    // takes the same real time capped at 60 or not). A DXVK_CONFIG set by the user wins.
    setenv("DXVK_CONFIG", "d3d9.allowDirectBufferMapping = False", 0);
    trace = getenv("CATH_TRACE") != NULL;
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY);   // lets deck_run.sh's eu-stack read a hung run (SteamOS: ptrace_scope 1)
    M = mmap(NULL, (1ull << 32) + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (M == MAP_FAILED) die("cannot reserve 4 GB guest memory");
    uint32_t base = load_pe(argv[1]);
    char dir[4096];
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 64);
    while (n > 0 && dir[n - 1] != '/') n--;
    for (int i = 0; i < 2; i++) {
        snprintf(dir + n, 64, "%s", dll_files[i]);
        if (access(dir, R_OK) == 0) dll_bases[i] = load_pe(dir);
        else fprintf(stderr, "catherine: %s not found (its imports will be missing)\n", dir);
    }
    for (int i = 0; i < 2; i++) if (dll_bases[i]) bind_imports(dll_bases[i]);
    bind_imports(base);
    if (getenv("CATH_REPORT")) return 0;   // imports listed above; nothing runs
    rt_fallback = fallback;
    prof_start();                        // CATH_PROFILE=1 only (prof.c)

    U32(G_PEB + 0x08) = base;            // PEB.ImageBaseAddress (teb_init reads it to find the TLS directory)
    teb_init(G_TEB, G_STACK_LO, G_STACK_HI);
    U32(G_TEB + 0x24) = 1;               // main thread id

    static CPU cpu;
    CPU *c = &cpu;
    prof_main(c);                        // CATH_PROFILE only
    REG(c, R_ESP) = G_STACK_HI - 16;
    U32(REG(c, R_ESP)) = 0;              // entry point "returns" to 0 = process end
    REG(c, R_FS_OFFSET) = G_TEB;
    V(c->r + 0x10A0, uint16_t) = 0x27F;  // FPUControlWord: Windows starts processes with 53-bit x87 precision
    V(c->r + 0x1094, uint32_t) = 0x1F80; // MXCSR
    uint16_t cw = 0x27F;                 // lifted x87 math runs on the host FPU, so match the precision there too
    __asm__ volatile("fldcw %0" : : "m"(cw));

    uint32_t entry = base + U32(base + U32(base + 0x3C) + 40);
    for (int i = 0; i < 2; i++) {        // DllMain(base, DLL_PROCESS_ATTACH, 0), stdcall
        uint32_t b = dll_bases[i], sp = REG(c, R_ESP);
        if (!b) continue;
        uint32_t args[4] = {0, b, 1, 0};   // return address, then the 3 arguments
        REG(c, R_ESP) -= 16;
        memcpy(GM(REG(c, R_ESP)), args, 16);
        enter(c, b + U32(b + U32(b + 0x3C) + 40));
        if (!REG(c, R_EAX)) die("%s: DllMain failed", dll_files[i]);
        REG(c, R_ESP) = sp;
    }
    tls_callbacks(c, 1);                 // DLL_PROCESS_ATTACH
    fprintf(stderr, "catherine: %u imports bound (%u stubs), entry %08x\n", n_imps, n_imps, entry);
    enter(c, entry);
    fprintf(stderr, "catherine: entry point returned, EAX=%08x\n", REG(c, R_EAX));
    return (int)REG(c, R_EAX);
}
