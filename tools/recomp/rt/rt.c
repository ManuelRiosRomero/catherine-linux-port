// M1 runtime: just enough to run lifted functions under the differential checker.
// Leaving the lifted set (unknown call/jump target, unimplemented op) unwinds back to rt_enter.
#define _GNU_SOURCE
#include "rt.h"
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

uint8_t *M;
static __thread jmp_buf leave;        // per thread: each host thread runs its own rt_enter
__thread uint32_t rt_left_at;         // target address that left the lifted set
__thread char rt_msg[128];            // why we stopped (unimplemented op)

static Fn lookup(uint32_t a) {
    uint32_t lo = 0, hi = fn_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (fn_table[mid].addr < a) lo = mid + 1; else hi = mid;
    }
    return lo < fn_count && fn_table[lo].addr == a ? fn_table[lo].fn : 0;
}

Fn (*rt_fallback)(uint32_t t);   // host hook: resolves import stubs etc.; NULL under the checker

// Indirect calls and jumps (vtables, callbacks, every import call) are hot: a small per-thread cache in front of the
// binary search (5.9% of the CPU in the Stray Sheep bar before). Only fn_table hits are cached: a fallback answer can
// depend on the call (Steam stubs record which one was taken). Host stubs live at 0xFFC00000+ and are never in fn_table.
static __thread struct { uint32_t a; Fn f; } cache[4096];
static Fn resolve(uint32_t t) {
    Fn f = 0;
    if (t < 0xFFC00000u) {
        unsigned h = (t >> 2 ^ t >> 14) & 4095;
        if (cache[h].a == t && cache[h].f) return cache[h].f;
        if ((f = lookup(t))) { cache[h].a = t; cache[h].f = f; return f; }
    }
    if (rt_fallback) f = rt_fallback(t);
    if (!f) { rt_left_at = t; longjmp(leave, 1); }
    return f;
}

// Debug: CATH_CHECK_REGS=1 checks every indirect call / import call keeps EBX, EBP, ESI, EDI and pops a sane amount
// (the return address plus up to 32 stdcall arguments). Direct calls are plain C calls and are not checked.
static int check_regs = -1;
void rt_call(CPU *c, uint32_t t) {
    Fn f = resolve(t);
    if (check_regs < 0) check_regs = getenv("CATH_CHECK_REGS") != NULL;
    if (!check_regs) { f(c); return; }
    static const int off[4] = {0xc, 0x14, 0x18, 0x1c};
    static const char *name[4] = {"EBX", "EBP", "ESI", "EDI"};
    static int reports;
    uint32_t before[4], sp = *(uint32_t *)(c->r + 0x10);
    for (int i = 0; i < 4; i++) before[i] = *(uint32_t *)(c->r + off[i]);
    f(c);
    uint32_t pop = *(uint32_t *)(c->r + 0x10) - sp;
    for (int i = 0; i < 4 && reports < 20; i++)
        if (*(uint32_t *)(c->r + off[i]) != before[i])
            fprintf(stderr, "[regs] call %08x (returns to %08x) changed %s %08x -> %08x\n", t, *(uint32_t *)(M + sp), name[i],
                    before[i], *(uint32_t *)(c->r + off[i])), reports++;
    if ((pop < 4 || pop > 4 + 128 || pop % 4) && reports < 20)
        fprintf(stderr, "[regs] call %08x (returns to %08x) moved ESP by %d\n", t, *(uint32_t *)(M + sp), (int)pop), reports++;
}

void rt_jump(CPU *c, uint32_t t) { resolve(t)(c); }   // a tail jump may land in a shared epilogue: never checked

// ponytail: one global lock for every x86 LOCK-prefixed op; per-address locks if contention shows up.
static pthread_mutex_t big = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
void rt_lock(void) { pthread_mutex_lock(&big); }
void rt_unlock(void) { pthread_mutex_unlock(&big); }

// Ghidra models CPUID as user ops that return a pointer to {EAX, EBX, EDX, ECX}. We run the real host CPUID
// but hide AVX/AVX2/FMA/AVX-512 so the CRT's memcpy etc. stay on the SSE2 paths the checker verified.
uint32_t rt_cpuid(CPU *c, int op) {
    static const uint32_t leaf_of[] = {0, 0, 1, 2, 3, 4, 5, 6, 7, 9, 0xa, 0xb, 0xd, 0xf, 0x80000002, 0x80000003, 0x80000004};
    uint32_t leaf = op == 45 ? *(uint32_t *)c->r : (op >= 46 && op <= 61) ? leaf_of[op - 45] : 0;
    uint32_t sub = *(uint32_t *)(c->r + 4), a, b, cc, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(cc), "=d"(d) : "a"(leaf), "c"(sub));
    if (leaf == 1) cc &= ~((1u << 28) | (1u << 27) | (1u << 12));   // AVX, OSXSAVE, FMA
    if (leaf == 7 && sub == 0) b &= ~((1u << 5) | 0xFFFF0000u);       // AVX2, AVX-512 family
    uint32_t p = *(uint32_t *)(c->r + 0x110) + 0xFE0;                 // scratch in this thread's TEB
    uint32_t out[4] = {a, b, d, cc};
    memcpy(M + p, out, 16);
    return p;
}

void rt_unimpl(const char *what, uint32_t at) {
    snprintf(rt_msg, sizeof rt_msg, "%s @%x", what, at);
    longjmp(leave, 2);
}

#ifdef RT_TRACE
uint32_t rt_tr[1 << 20], rt_trn;   // first 1M instruction addresses of the current rt_enter
void rt_trace(uint32_t a) { if (rt_trn < (1u << 20)) rt_tr[rt_trn] = a; rt_trn++; }
#endif

// Checker entry: 0 = returned normally, 1 = left lifted set at rt_left_at, 2 = unimplemented (rt_msg).
int rt_enter(CPU *c, uint32_t addr) {
#ifdef RT_TRACE
    rt_trn = 0;
#endif
    int r = setjmp(leave);
    if (r) return r;
    rt_call(c, addr);
    return 0;
}
