// Host side of the port: loader, guest memory map, and the Win32 import shims.
#pragma once
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>

// Guest memory map (32-bit addresses inside the 4 GB block at M)
#define G_TEB        0x00030000u   // thread block; FS:[0] points here
#define G_PEB        0x00031000u
#define G_STACK_LO   0x00100000u   // main thread stack: 2 MB below the image
#define G_STACK_HI   0x00300000u
#define G_HEAP_LO    0x10000000u   // malloc/HeapAlloc arena
#define G_HEAP_HI    0xFE000000u
#define G_STUBS      0xFFF00000u   // import i lives at G_STUBS + 16*i (never executed, only looked up)

// Register file offsets (Ghidra x86 register space)
#define R_EAX 0x0
#define R_ECX 0x4
#define R_EDX 0x8
#define R_ESP 0x10
#define R_FS_OFFSET 0x110
#define REG(c, off) V((c)->r + (off), uint32_t)
#define U32(a) V(GM(a), uint32_t)
#define ARG(c, i) U32(REG(c, R_ESP) + 4 + 4 * (i))   // i-th stack argument (0-based) at import entry

// Return from an import: set EAX, pop the return address, and for stdcall also the arguments.
static inline void ret_std(CPU *c, uint32_t v, int nargs) { REG(c, R_EAX) = v; REG(c, R_ESP) += 4 + 4 * nargs; }
#define RET_CDECL(c, v) ret_std(c, v, 0)

#define die(...) (fprintf(stderr, "catherine: " __VA_ARGS__), fputc('\n', stderr), exit(1))

extern char **host_argv;   // main.c: the command line and folder we started with (the game restarts itself with them)
extern char host_cwd[4096];
// steam.c: Steam Input reported a connected controller. As on Windows, the game then sees that controller only through
// Steam Input; xinput.c reports no pad (both at once = every press arrives twice: dialogue skips ahead).
extern int steam_input_pads;

// guest heap (heap.c)
uint32_t gmalloc(uint32_t n);
void gfree(uint32_t p);
uint32_t gsize(uint32_t p);
uint32_t grealloc(uint32_t p, uint32_t n);

// import registry (win32.c): handler for "DLL!name" (DLL case-insensitive), or NULL
Fn import_handler(const char *dll, const char *name);
uint32_t dyn_stub(const char *dll, const char *name);
uint32_t named_stub(const char *dll, const char *name);   // stub even if unimplemented (guest COM vtables)
// input state from the SDL event pump (user32.c), for dinput.c
void input_keys(uint8_t dik[256]);                        // DirectInput keyboard state: 0x80 = down, by DIK code
uint16_t input_scripted(void);                            // CATH_INPUT presses held right now (XInput button bits)
void input_mouse(int32_t d[3], uint8_t buttons[8]);       // x/y/wheel motion since the last call, buttons 0x80 = down
const char *input_key_name(int dik);                      // key name for a DIK code, NULL = no such key
// translated DLLs (main.c): base of a loaded DLL by file name, and its export by name/ordinal (GetProcAddress rules)
uint32_t dll_base(const char *name);
uint32_t dll_is_loaded(uint32_t h);
uint32_t dll_export(uint32_t base, uint32_t name);
uint32_t stdio_iob(void);       // stdio.c: msvcrt's _iob[3] (stdin, stdout, stderr)
Fn steam_resolve(uint32_t t);   // steam.c: guest Steam interface method stubs
Fn d3d9_resolve(uint32_t t);    // d3d9_bridge.cpp: guest D3D9 COM method stubs

// threads (sync.c)
void teb_init(uint32_t teb, uint32_t stack_lo, uint32_t stack_hi);
void tls_callbacks(CPU *c, uint32_t reason);
uint32_t handle_for_fd(int fd);   // file handles share the kernel object table
int fd_of(uint32_t h);

// Define an import shim and register it (via a linker section, so there is no table to keep in sync).
// IMP("KERNEL32.dll", GetTickCount) { ...; ret_std(c, v, 0); }   IMPX for names that aren't C identifiers.
typedef struct { const char *dll, *name; Fn fn; } Import;
#define IMPX(dll, name, id)                                                                                  \
    static void imp_##id(CPU *c);                                                                            \
    static const Import reg_##id __attribute__((used, section("cath_imports"), aligned(sizeof(void *)))) = \
        {dll, name, imp_##id};                                                                               \
    static void imp_##id(CPU *c)
#define IMP(dll, name) IMPX(dll, #name, name)

// Call guest code from a shim: pushes args (last first) and a dummy return address, runs it, returns EAX.
uint32_t guest_call(CPU *c, uint32_t fn, int stdcall, int n, ...);

#define GSTR(a) ((char *)GM(a))
void host_path(const char *win, char *out, size_t cap);   // path.c
uint32_t guest_fopen(const char *win, const char *mode, int at_end);   // stdio.c
void ret_st0(CPU *c, long double v);       // return a double the x87 way (push onto ST0)

// Native replacement for a guest function the lifter leaves out (tools/recomp/native.txt); calls to it land here.
// NATIVE(0x6a96d0, WmvPlayer_OpenURL) { ...; ret_std(c, hr, 2); }   (same calling-convention rules as imports)
typedef struct { uint32_t addr; Fn fn; } NativeFn;
#define NATIVE(addr, id)                                                                                    \
    static void nat_##id(CPU *c);                                                                           \
    static const NativeFn natreg_##id __attribute__((used, section("cath_natives"), aligned(sizeof(void *)))) = \
        {addr, nat_##id};                                                                                   \
    static void nat_##id(CPU *c)

void event_signal(uint32_t h);   // sync.c: SetEvent from host code
uint32_t wait_handle(uint32_t h, uint32_t ms);   // sync.c: WaitForSingleObject from host code
void dump_waiters(void);                         // sync.c: print blocked threads (test runs)
void guest_cs(CPU *c, uint32_t cs, int lock);   // win32.c: Enter (1) / Leave (0) a guest CRITICAL_SECTION
extern volatile int prof_on;   // prof.c: CATH_PROFILE wait timing
uint64_t prof_now(void);
void prof_wait(int kind, uint32_t caller, uint64_t t0);
uint32_t prof_tid(void);   // gettid, cached per thread
void prof_start(void);   // prof.c: CATH_PROFILE sampler (~/catherine-port/profile.on start/stop)
void prof_main(CPU *c);   // prof.c: main-thread wall-clock sampler (CATH_PROFILE)
