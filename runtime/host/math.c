// CRT math helpers. Three calling conventions:
//   _libm_sse2_*_precise: argument(s) in XMM0 (XMM1), result in XMM0  (no stack args)
//   _CI*: x87 intrinsics: argument(s) on the x87 stack (ST0, or ST1 op ST0), result replaces them in ST0
//   floor/ceil: plain cdecl double on the stack, result in ST0
// Results come from glibc, so transcendental functions can differ from MSVC's libm in the last bit.
#include "host.h"
#include <math.h>
#include <string.h>

#define XMM0 (c->r + 0x1200)
#define XMM1 (c->r + 0x1240)
#define ST(i) STP(c, i)
#define MATH "api-ms-win-crt-math-l1-1-0.dll"

static double xmm(uint8_t *r) { double d; memcpy(&d, r, 8); return d; }
static void ret_xmm(CPU *c, double d) { memcpy(XMM0, &d, 8); RET_CDECL(c, REG(c, R_EAX)); }
static void x87_pop(CPU *c) { c->r[X87_TOP] = (c->r[X87_TOP] + 1) & 7; }

#define SSE1(fn) IMPX(MATH, "_libm_sse2_" #fn "_precise", sse_##fn) { ret_xmm(c, fn(xmm(XMM0))); }
SSE1(exp) SSE1(cos) SSE1(sin) SSE1(tan) SSE1(atan) SSE1(asin) SSE1(acos) SSE1(sqrt)
IMPX(MATH, "_libm_sse2_pow_precise", sse_pow) { ret_xmm(c, pow(xmm(XMM0), xmm(XMM1))); }

#define CI1(fn) IMPX(MATH, "_CI" #fn, ci_##fn) { st10(ST(0), (long double)fn((double)ld10(ST(0)))); RET_CDECL(c, REG(c, R_EAX)); }
CI1(sin) CI1(cos) CI1(tan) CI1(sqrt) CI1(log) CI1(exp) CI1(asin) CI1(acos) CI1(atan) CI1(sinh) CI1(cosh) CI1(tanh)
// two-argument forms compute f(ST1, ST0), pop once, and leave the result in ST0
#define CI2(name, fn)                                                                  \
    IMPX(MATH, "_CI" #name, ci_##name) {                                              \
        double r = fn((double)ld10(ST(1)), (double)ld10(ST(0)));                      \
        x87_pop(c);                                                                    \
        st10(ST(0), (long double)r);                                                   \
        RET_CDECL(c, REG(c, R_EAX));                                                   \
    }
CI2(pow, pow) CI2(atan2, atan2) CI2(fmod, fmod)

IMPX(MATH, "floor", floor_) { double d; memcpy(&d, GM(REG(c, R_ESP) + 4), 8); ret_st0(c, floor(d)); RET_CDECL(c, REG(c, R_EAX)); }
// msvcrt.dll extras (cdecl, double args on the stack)
#define DARG(i) (*(double *)GM(REG(c, R_ESP) + 4 + 8 * (i)))
IMP("msvcrt.dll", _finite) { RET_CDECL(c, (uint32_t)isfinite(DARG(0))); }
IMP("msvcrt.dll", _isnan) { RET_CDECL(c, (uint32_t)isnan(DARG(0))); }
IMP("msvcrt.dll", _fpclass) {
    double d = DARG(0);
    uint64_t b; memcpy(&b, &d, 8);
    int neg = signbit(d), k = fpclassify(d);
    uint32_t r = k == FP_NAN ? (b >> 51 & 1 ? 2 : 1) : k == FP_INFINITE ? (neg ? 4 : 0x200) : k == FP_ZERO ? (neg ? 0x20 : 0x40)
               : k == FP_SUBNORMAL ? (neg ? 0x10 : 0x80) : (neg ? 8 : 0x100);   // _FPCLASS_* bits
    RET_CDECL(c, r);
}
IMP("msvcrt.dll", modf) { double ip, f = modf(DARG(0), &ip); memcpy(GM(U32(REG(c, R_ESP) + 12)), &ip, 8); ret_st0(c, f); RET_CDECL(c, 0); }
IMP("msvcrt.dll", frexp) { int e; double m = frexp(DARG(0), &e); U32(U32(REG(c, R_ESP) + 12)) = (uint32_t)e; ret_st0(c, m); RET_CDECL(c, 0); }
IMP("msvcrt.dll", ldexp) { ret_st0(c, ldexp(DARG(0), (int)U32(REG(c, R_ESP) + 12))); RET_CDECL(c, 0); }
IMP("msvcrt.dll", strtod) {
    char *end;
    double d = strtod(GSTR(ARG(c, 0)), &end);
    if (ARG(c, 1)) U32(ARG(c, 1)) = ARG(c, 0) + (uint32_t)(end - GSTR(ARG(c, 0)));
    ret_st0(c, d);
    RET_CDECL(c, 0);
}
// _except1(flags, op, double arg, double result, cw): the CRT's FP-error hook (inf/NaN input); errno is not kept
IMP(MATH, _except1) { ret_st0(c, DARG(2)); RET_CDECL(c, 0); }   // DARG(2) = stack bytes 16..23 = result
IMPX(MATH, "ceil", ceil_) { double d; memcpy(&d, GM(REG(c, R_ESP) + 4), 8); ret_st0(c, ceil(d)); RET_CDECL(c, REG(c, R_EAX)); }
