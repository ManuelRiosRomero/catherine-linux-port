// Runtime contract for lifted code. Guest = 32-bit x86, host = x86-64 Linux.
// Guest address a lives at M + a (M = 4 GB reservation). CPU registers live in a byte array
// indexed by Ghidra's register-space offsets, so sub-registers (AL/AH/AX/EAX) alias for free.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#define REGSZ 0x4000
typedef struct CPU { uint8_t r[REGSZ]; } CPU;
typedef void (*Fn)(CPU *);
typedef struct { uint32_t addr; Fn fn; } FnEntry;
// aligned(1): guest XMM loads/stores are often unaligned; the default 16-byte alignment makes GCC emit movaps -> SIGSEGV.
typedef unsigned __int128 u128 __attribute__((aligned(1)));

extern uint8_t *M;
extern const FnEntry fn_table[];
extern const uint32_t fn_count;

// Every varnode is accessed through a host pointer: c->r+off, u+slot, or M+addr.
#define V(p, T) (*(T *)(p))
#define GM(a) (M + (uint32_t)(a))

// x87 stack as on the real FPU: a top index (0-7) at 0x10b0 (unused by Ghidra's register map), ST(i) in 16-byte slot
// (top + i) & 7 at 0x1100. MMi share those slots; MMX code runs with an empty x87 stack, so top is 0 there as on hardware.
#define X87_TOP 0x10b0
#define STP(c, i) ((c)->r + 0x1100 + ((((c)->r[X87_TOP] + (i)) & 7) << 4))

static inline float rt_f32(uint32_t x) { float f; memcpy(&f, &x, 4); return f; }   // XMM lane kept in a uint32 local
static inline uint32_t rt_b32(float f) { uint32_t x; memcpy(&x, &f, 4); return x; }
// x87 ST(i) in c->r: one fldt / fstpt on the 10 bytes (a memcpy through a zeroed stack temporary cost ~6 instructions)
typedef struct { char b[10]; } x87bytes;
static inline long double ld10(const void *p) { long double x; __asm__("fldt %1" : "=t"(x) : "m"(*(const x87bytes *)p)); return x; }
static inline void st10(void *p, long double x) { __asm__("fstpt %0" : "=m"(*(x87bytes *)p) : "t"(x) : "st"); }

// Real x87 fsin/fcos (the host is x86-64 too): bit-exact with the original, unlike glibc sinl/cosl.
static inline long double rt_fsin(long double x) { __asm__("fsin" : "+t"(x)); return x; }
static inline long double rt_fcos(long double x) { __asm__("fcos" : "+t"(x)); return x; }

// SSE ops Ghidra models as user ops: 4 lanes of float divide / 32-bit multiply (low half).
static inline u128 rt_divps(u128 a, u128 b) {
    float x[4], y[4]; memcpy(x, &a, 16); memcpy(y, &b, 16);
    for (int i = 0; i < 4; i++) x[i] /= y[i];
    memcpy(&a, x, 16); return a;
}
static inline u128 rt_pmulld(u128 a, u128 b) {
    uint32_t x[4], y[4]; memcpy(x, &a, 16); memcpy(y, &b, 16);
    for (int i = 0; i < 4; i++) x[i] *= y[i];
    memcpy(&a, x, 16); return a;
}

// More MMX/SSE user ops: dst = op(dst, src), n = register size in bytes (8 = MMX, 16 = XMM). The lifter widens MMX
// operands to u128 and truncates the result; lanes past n are don't-care except where packing reads them by n.
static inline int16_t rt_sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static inline int8_t rt_sat8(int32_t v) { return v > 127 ? 127 : v < -128 ? -128 : (int8_t)v; }
#define RT_W16(name, expr)                                                             \
    static inline u128 rt_##name(u128 a, u128 b, int n) {                              \
        int16_t x[8], y[8]; memcpy(x, &a, 16); memcpy(y, &b, 16);                      \
        for (int i = 0; i < n / 2; i++) x[i] = (expr);                                 \
        memcpy(&a, x, 16); return a;                                                   \
    }
RT_W16(paddsw, rt_sat16(x[i] + y[i]))
RT_W16(psubsw, rt_sat16(x[i] - y[i]))
RT_W16(pmulhw, (int16_t)((x[i] * y[i]) >> 16))
// shift counts come from the low 64 bits of src; counts past 15 give 0 (logical) or sign fill (arithmetic)
RT_W16(psllw, (uint64_t)b > 15 ? 0 : (int16_t)((uint16_t)x[i] << (int)(uint64_t)b))
RT_W16(psraw, x[i] >> ((uint64_t)b > 15 ? 15 : (int)(uint64_t)b))
static inline u128 rt_pmaddwd(u128 a, u128 b, int n) {   // pairs of 16x16 products summed into 32-bit lanes
    int16_t x[8], y[8]; int32_t r[4]; memcpy(x, &a, 16); memcpy(y, &b, 16);
    for (int i = 0; i < 4; i++) r[i] = (int32_t)((uint32_t)(x[2 * i] * y[2 * i]) + (uint32_t)(x[2 * i + 1] * y[2 * i + 1]));
    memcpy(&a, r, 16); return a;   // n unused: lanes past n are don't-care
}
static inline u128 rt_packssdw(u128 a, u128 b, int n) {   // dst's dwords then src's, saturated to words
    int32_t x[4], y[4]; int16_t r[8]; memcpy(x, &a, 16); memcpy(y, &b, 16);
    for (int i = 0; i < n / 4; i++) r[i] = rt_sat16(x[i]), r[n / 4 + i] = rt_sat16(y[i]);
    memcpy(&a, r, 16); return a;
}
static inline u128 rt_packsswb(u128 a, u128 b, int n) {   // dst's words then src's, saturated to bytes
    int16_t x[8], y[8]; int8_t r[16]; memcpy(x, &a, 16); memcpy(y, &b, 16);
    for (int i = 0; i < n / 2; i++) r[i] = rt_sat8(x[i]), r[n / 2 + i] = rt_sat8(y[i]);
    memcpy(&a, r, 16); return a;
}
// reciprocal approximations: computed exactly (the hardware's ~12-bit estimate is only required to be close)
static inline u128 rt_rcpps(u128 a, u128 b, int n) {
    float y[4]; memcpy(y, &b, 16);
    for (int i = 0; i < 4; i++) y[i] = 1.0f / y[i];
    memcpy(&a, y, 16); return a;
}
static inline u128 rt_rcpss(u128 a, u128 b, int n) { float y; memcpy(&y, &b, 4); y = 1.0f / y; memcpy(&a, &y, 4); return a; }
static inline u128 rt_rsqrtss(u128 a, u128 b, int n) { float y; memcpy(&y, &b, 4); y = 1.0f / sqrtf(y); memcpy(&a, &y, 4); return a; }

// x86 returns the "integer indefinite" value when a float->int conversion overflows or is NaN.
#define TRUNC_TO(T, MIN, x) ((x) != (x) || (x) >= -(long double)(MIN) || (x) < (long double)(MIN) ? (T)(MIN) : (T)(x))

void rt_call(CPU *c, uint32_t target);   // CALLIND / unknown CALL target
extern Fn (*rt_fallback)(uint32_t target);
int rt_enter(CPU *c, uint32_t addr);     // 0 returned, 1 left lifted code at rt_left_at, 2 unimplemented (rt_msg)
extern __thread uint32_t rt_left_at;
extern __thread char rt_msg[128];
void rt_jump(CPU *c, uint32_t target);   // branch leaving the function body (tail jump)
void rt_lock(void);
void rt_unlock(void);
uint32_t rt_cpuid(CPU *c, int leaf_op);
void rt_unimpl(const char *what, uint32_t at);

// Debug builds (-DRT_TRACE) log every executed instruction address so the checker can find where paths split.
#ifdef RT_TRACE
void rt_trace(uint32_t addr);
#define TR(a) rt_trace(a)
#else
#define TR(a) ((void)0)
#endif
