// MSVCP140 iostreams for char (the 45 imports). The game's inline template code (basic_filebuf, ifstream, sentries,
// use_facet) touches these objects directly, so every object uses MSVC's x86 layout from microsoft/STL (Apache-2.0):
//   basic_streambuf (0x38): vptr, _Gfirst 4, _Pfirst 8, _IGfirst c, _IPfirst 10, _Gnext 14, _Pnext 18, _IGnext 1c,
//                    _IPnext 20, _Gcount 24, _Pcount 28, _IGcount 2c, _IPcount 30, _Plocale 34
//   ios_base/basic_ios (0x48): vptr, _Stdstr 4, (8: zeroed by _Init), _Mystate c, _Except 10, _Fmtfl 14, _Prec 18 (i64),
//                    _Wide 20 (i64), _Arr 28, _Calls 2c, _Ploc 30, _Mystrbuf 38, _Tiestr 3c, _Fillch 40  (from the real
//                    msvcp140.dll: rdstate = [ecx+c], exceptions = [ecx+10], flags = [ecx+14]; the game inlines [ios+c] & 6)
//   basic_istream: vbptr 0, _Chcount 8 (i64), virtual base basic_ios at this + vbtable[1]; basic_ostream: vbptr 0
//   locale (8 bytes: MSVC gives its 2nd empty base a slot): _Ptr at +4; _Locimp: vptr, _Myrefs 4, _Facetvec 8, _Facetcount c, _Catmask 10, _Xparent 14, _Name 18
//   fpos (0x18): _Myoff i64, _Fpos i64, _Mystate 8 bytes
// Calls back into the game's own streambuf (its basic_filebuf) go through its vtable, like the real DLL does:
//   0 dtor, 1 _Lock, 2 _Unlock, 3 overflow, 4 pbackfail, 5 showmanyc, 6 underflow, 7 uflow, 8 xsgetn, 9 xsputn,
//   10 seekoff, 11 seekpos, 12 setbuf, 13 sync, 14 imbue
#define _GNU_SOURCE
#include "host.h"
#include <ctype.h>
#include <pthread.h>
#include <string.h>

#define MSVCP "MSVCP140.dll"
#define INTERNAL "msvcp.internal"   // our own vtable targets, looked up with dyn_stub
#define SB "?$basic_streambuf@DU?$char_traits@D@std@@@std@@"
#define IOS "?$basic_ios@DU?$char_traits@D@std@@@std@@"
#define IS "?$basic_istream@DU?$char_traits@D@std@@@std@@"
#define OS "?$basic_ostream@DU?$char_traits@D@std@@@std@@"
#define CVT "?$codecvt@DDU_Mbstatet@@@std@@"
#define EOF_ 0xFFFFFFFFu

enum { GOOD = 0, EOFBIT = 1, FAILBIT = 2, BADBIT = 4 };

// ---------- calling guest code (thiscall: this in ECX, callee pops) ----------

static uint32_t this_call(CPU *c, uint32_t self, uint32_t fn, int n, const uint32_t *args) {
    uint32_t esp = REG(c, R_ESP);
    for (int i = n - 1; i >= 0; i--) { REG(c, R_ESP) -= 4; U32(REG(c, R_ESP)) = args[i]; }
    REG(c, R_ESP) -= 4;
    U32(REG(c, R_ESP)) = 0;
    REG(c, R_ECX) = self;
    rt_call(c, fn);
    REG(c, R_ESP) = esp;
    return REG(c, R_EAX);
}
static uint32_t vcall(CPU *c, uint32_t self, int slot, int n, const uint32_t *args) {
    return this_call(c, self, U32(U32(self) + 4 * slot), n, args);
}
static int64_t ret64(CPU *c) { return (int64_t)(((uint64_t)REG(c, R_EDX) << 32) | REG(c, R_EAX)); }
static void ret_i64(CPU *c, int64_t v, int nargs) { REG(c, R_EDX) = (uint32_t)((uint64_t)v >> 32); ret_std(c, (uint32_t)v, nargs); }
#define I64ARG(c, i) V(GM(REG(c, R_ESP) + 4 + 4 * (i)), int64_t)   // 64-bit argument starting at slot i
#define THIS REG(c, R_ECX)

// ---------- vtables for objects we create (entries resolve to the INTERNAL shims below) ----------

static uint32_t make_vtable(const char *const *names, int n) {
    uint32_t vt = gmalloc(4 * n);
    for (int i = 0; i < n; i++) {
        uint32_t s = dyn_stub(INTERNAL, names[i]);
        if (!s) die("msvcp: internal vtable entry %s missing", names[i]);
        U32(vt + 4 * i) = s;
    }
    return vt;
}
static uint32_t vt_locimp, vt_codecvt, vt_streambuf, vt_ios, vt_istream_vb, vt_ostream_vb, global_locimp;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void setup(void) {
    static const char *facet[] = {"facet_dtor", "facet_incref", "facet_decref"};
    static const char *cvt[] = {"facet_dtor", "facet_incref", "facet_decref", "cvt_noconv", "cvt_one", "cvt_one",
                                "cvt_do_in", "cvt_do_out", "cvt_do_unshift", "cvt_do_length"};
    static const char *sb[] = {"sb_vdtor", "sb_nop0", "sb_nop0", "sb_eof1", "sb_eof1", "sb_zero64", "sb_eof0",
                               "sb_uflow", "sb_xsgetn", "sb_xsputn", "sb_seekoff", "sb_seekpos", "sb_setbuf", "sb_zero0", "sb_nop1"};
    static const char *ios[] = {"ios_vdtor"};
    vt_locimp = make_vtable(facet, 3);
    vt_codecvt = make_vtable(cvt, 10);
    vt_streambuf = make_vtable(sb, 15);
    vt_ios = make_vtable(ios, 1);
    vt_istream_vb = gmalloc(8); U32(vt_istream_vb) = 0; U32(vt_istream_vb + 4) = 0x10;   // vbtable: ios at +0x10
    vt_ostream_vb = gmalloc(8); U32(vt_ostream_vb) = 0; U32(vt_ostream_vb + 4) = 0x8;    // vbtable: ios at +0x8
    // the one global "C" locale; never freed (huge refcount), like the CRT's classic locale
    global_locimp = gmalloc(0x30);
    memset(GM(global_locimp), 0, 0x30);
    U32(global_locimp) = vt_locimp;
    U32(global_locimp + 4) = 1u << 30;
    U32(global_locimp + 0x10) = 0x3F;           // _Catmask = all
    uint32_t name = gmalloc(2);
    strcpy(GSTR(name), "C");
    U32(global_locimp + 0x18) = name;          // _Name._Myptr
}
static void init_once(void) { pthread_once(&once, setup); }
static uint32_t new_locale(void) {             // `new locale` = {_Ptr = global}, with a reference
    init_once();
    uint32_t l = gmalloc(8);
    U32(l) = 0;
    U32(l + 4) = global_locimp;
    __atomic_add_fetch((uint32_t *)GM(global_locimp + 4), 1, __ATOMIC_SEQ_CST);
    return l;
}
static void delete_locale(uint32_t l) {        // ~locale + delete; facets/locimps we hand out are immortal
    if (!l) return;
    uint32_t imp = U32(l + 4);
    if (imp) __atomic_sub_fetch((uint32_t *)GM(imp + 4), 1, __ATOMIC_SEQ_CST);
    gfree(l);
}

// facet / _Locimp virtuals: [0] scalar deleting dtor, [1] _Incref, [2] _Decref
IMPX(INTERNAL, "facet_dtor", facet_dtor) { if (ARG(c, 0) & 1) gfree(THIS); ret_std(c, THIS, 1); }
IMPX(INTERNAL, "facet_incref", facet_incref) { __atomic_add_fetch((uint32_t *)GM(THIS + 4), 1, __ATOMIC_SEQ_CST); ret_std(c, 0, 0); }
IMPX(INTERNAL, "facet_decref", facet_decref) {
    uint32_t n = __atomic_sub_fetch((uint32_t *)GM(THIS + 4), 1, __ATOMIC_SEQ_CST);
    ret_std(c, n == 0 ? THIS : 0, 0);
}

// ---------- locale ----------

IMPX(MSVCP, "?_Getgloballocale@locale@std@@CAPAV_Locimp@12@XZ", getgloballocale) { init_once(); RET_CDECL(c, global_locimp); }
// locale::id::operator size_t: lazily number facet kinds (the id object may be a data import: plain guest memory)
static uint32_t id_cnt;
IMPX(MSVCP, "??Bid@locale@std@@QAEIXZ", locale_id) {
    if (!U32(THIS)) {
        uint32_t want = __atomic_add_fetch(&id_cnt, 1, __ATOMIC_SEQ_CST), zero = 0;
        __atomic_compare_exchange_n((uint32_t *)GM(THIS), &zero, want, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    ret_std(c, U32(THIS), 0);
}
// _Lockit: one recursive mutex per lock kind (_LOCK_LOCALE = 0, ...)
static pthread_mutex_t lockits[8];
static pthread_once_t lockit_once = PTHREAD_ONCE_INIT;
static void lockit_setup(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    for (int i = 0; i < 8; i++) pthread_mutex_init(&lockits[i], &a);
}
IMPX(MSVCP, "??0_Lockit@std@@QAE@H@Z", lockit_ctor) {
    pthread_once(&lockit_once, lockit_setup);
    uint32_t k = ARG(c, 0) & 7;
    U32(THIS) = k;
    pthread_mutex_lock(&lockits[k]);
    ret_std(c, THIS, 1);
}
IMPX(MSVCP, "??1_Lockit@std@@QAE@XZ", lockit_dtor) { pthread_mutex_unlock(&lockits[U32(THIS) & 7]); ret_std(c, 0, 0); }

// ---------- codecvt<char, char, _Mbstatet>: always "no conversion" ----------

IMPX(MSVCP, "?_Getcat@" CVT "SAIPAPBVfacet@locale@2@PBV42@@Z", cvt_getcat) {   // static (const facet**, const locale*)
    init_once();
    uint32_t pp = ARG(c, 0);
    if (pp && !U32(pp)) {
        uint32_t f = gmalloc(0x20);
        memset(GM(f), 0, 0x20);
        U32(f) = vt_codecvt;
        U32(pp) = f;
    }
    RET_CDECL(c, 2);   // _X_CTYPE
}
IMPX(MSVCP, "?always_noconv@codecvt_base@std@@QBE_NXZ", cvt_always_noconv) { ret_std(c, 1, 0); }
// in/out(state&, from, from_end, from_next&, to, to_end, to_next&) and unshift(state&, to, to_end, to_next&): noconv
static void noconv7(CPU *c) { U32(ARG(c, 3)) = ARG(c, 1); U32(ARG(c, 6)) = ARG(c, 4); ret_std(c, 3, 7); }
IMPX(MSVCP, "?in@" CVT "QBEHAAU_Mbstatet@@PBD1AAPBDPAD3AAPAD@Z", cvt_in) { noconv7(c); }
IMPX(MSVCP, "?out@" CVT "QBEHAAU_Mbstatet@@PBD1AAPBDPAD3AAPAD@Z", cvt_out) { noconv7(c); }
IMPX(MSVCP, "?unshift@" CVT "QBEHAAU_Mbstatet@@PAD1AAPAD@Z", cvt_unshift) { U32(ARG(c, 3)) = ARG(c, 1); ret_std(c, 3, 4); }
IMPX(INTERNAL, "cvt_noconv", cvt_v_noconv) { ret_std(c, 1, 0); }
IMPX(INTERNAL, "cvt_one", cvt_v_one) { ret_std(c, 1, 0); }   // do_max_length / do_encoding
IMPX(INTERNAL, "cvt_do_in", cvt_v_in) { noconv7(c); }
IMPX(INTERNAL, "cvt_do_out", cvt_v_out) { noconv7(c); }
IMPX(INTERNAL, "cvt_do_unshift", cvt_v_unshift) { U32(ARG(c, 3)) = ARG(c, 1); ret_std(c, 3, 4); }
IMPX(INTERNAL, "cvt_do_length", cvt_v_length) {   // (state&, first, last, max): bytes = min(max, last - first)
    uint32_t n = ARG(c, 2) - ARG(c, 1);
    ret_std(c, n < ARG(c, 3) ? n : ARG(c, 3), 4);
}

// ---------- basic_streambuf ----------

#define G_GNEXT(sb) U32(U32((sb) + 0x1C))
#define G_GCOUNT(sb) U32(U32((sb) + 0x2C))
#define G_PNEXT(sb) U32(U32((sb) + 0x20))
#define G_PCOUNT(sb) U32(U32((sb) + 0x2C + 4))
static int32_t gnavail(uint32_t sb) { return G_GNEXT(sb) ? (int32_t)G_GCOUNT(sb) : 0; }
static int32_t pnavail(uint32_t sb) { return G_PNEXT(sb) ? (int32_t)G_PCOUNT(sb) : 0; }
static uint32_t gninc(uint32_t sb) { G_GCOUNT(sb)--; return G_GNEXT(sb)++; }
static void sb_init(uint32_t sb) {
    U32(sb + 0x0C) = sb + 0x04;   // _IGfirst = &_Gfirst
    U32(sb + 0x10) = sb + 0x08;   // _IPfirst = &_Pfirst
    U32(sb + 0x1C) = sb + 0x14;   // _IGnext = &_Gnext
    U32(sb + 0x20) = sb + 0x18;   // _IPnext = &_Pnext
    U32(sb + 0x2C) = sb + 0x24;   // _IGcount = &_Gcount
    U32(sb + 0x30) = sb + 0x28;   // _IPcount = &_Pcount
    U32(sb + 0x04) = U32(sb + 0x08) = U32(sb + 0x14) = U32(sb + 0x18) = U32(sb + 0x24) = U32(sb + 0x28) = 0;
}
static uint32_t sgetc_(CPU *c, uint32_t sb) { return gnavail(sb) > 0 ? *(uint8_t *)GM(G_GNEXT(sb)) : vcall(c, sb, 6, 0, NULL); }
static uint32_t sbumpc_(CPU *c, uint32_t sb) { return gnavail(sb) > 0 ? *(uint8_t *)GM(gninc(sb)) : vcall(c, sb, 7, 0, NULL); }
static uint32_t snextc_(CPU *c, uint32_t sb) {
    if (gnavail(sb) > 1) { G_GCOUNT(sb)--; return *(uint8_t *)GM(++G_GNEXT(sb)); }
    return sbumpc_(c, sb) == EOF_ ? EOF_ : sgetc_(c, sb);
}
static uint32_t uflow_(CPU *c, uint32_t sb) { return vcall(c, sb, 6, 0, NULL) == EOF_ ? EOF_ : *(uint8_t *)GM(gninc(sb)); }
static int64_t xsgetn_(CPU *c, uint32_t sb, uint32_t p, int64_t n) {
    int64_t start = n;
    while (n > 0) {
        int32_t avail = gnavail(sb);
        if (avail > 0) {
            int64_t k = n < avail ? n : avail;
            memmove(GM(p), GM(G_GNEXT(sb)), (size_t)k);
            p += (uint32_t)k; n -= k;
            G_GCOUNT(sb) -= (uint32_t)k; G_GNEXT(sb) += (uint32_t)k;
        } else {
            uint32_t m = vcall(c, sb, 7, 0, NULL);   // uflow
            if (m == EOF_) break;
            *(uint8_t *)GM(p++) = (uint8_t)m;
            n--;
        }
    }
    return start - n;
}
static int64_t xsputn_(CPU *c, uint32_t sb, uint32_t p, int64_t n) {
    int64_t start = n;
    while (n > 0) {
        int32_t avail = pnavail(sb);
        if (avail > 0) {
            int64_t k = n < avail ? n : avail;
            memmove(GM(G_PNEXT(sb)), GM(p), (size_t)k);
            p += (uint32_t)k; n -= k;
            G_PCOUNT(sb) -= (uint32_t)k; G_PNEXT(sb) += (uint32_t)k;
        } else {
            uint32_t ch = *(uint8_t *)GM(p);
            if (vcall(c, sb, 3, 1, &ch) == EOF_) break;   // overflow
            p++; n--;
        }
    }
    return start - n;
}

IMPX(MSVCP, "??0" SB "IAE@XZ", sb_ctor) {
    init_once();
    U32(THIS) = vt_streambuf;
    U32(THIS + 0x34) = new_locale();
    sb_init(THIS);
    ret_std(c, THIS, 0);
}
IMPX(MSVCP, "??1" SB "UAE@XZ", sb_dtor) {
    init_once();
    U32(THIS) = vt_streambuf;
    delete_locale(U32(THIS + 0x34));
    U32(THIS + 0x34) = 0;
    ret_std(c, 0, 0);
}
IMPX(MSVCP, "?_Init@" SB "IAEXXZ", sb_Init) { sb_init(THIS); ret_std(c, 0, 0); }
IMPX(MSVCP, "?getloc@" SB "QBE?AVlocale@2@XZ", sb_getloc) {   // returns locale by value through the hidden pointer
    uint32_t ret = ARG(c, 0), imp = U32(U32(THIS + 0x34) + 4);
    U32(ret) = 0;
    U32(ret + 4) = imp;
    if (imp) __atomic_add_fetch((uint32_t *)GM(imp + 4), 1, __ATOMIC_SEQ_CST);
    ret_std(c, ret, 1);
}
IMPX(MSVCP, "?sgetc@" SB "QAEHXZ", sb_sgetc) { ret_std(c, sgetc_(c, THIS), 0); }
IMPX(MSVCP, "?sbumpc@" SB "QAEHXZ", sb_sbumpc) { ret_std(c, sbumpc_(c, THIS), 0); }
IMPX(MSVCP, "?snextc@" SB "QAEHXZ", sb_snextc) { ret_std(c, snextc_(c, THIS), 0); }
IMPX(MSVCP, "?_Pninc@" SB "IAEPADXZ", sb_pninc) { uint32_t sb = THIS; G_PCOUNT(sb)--; ret_std(c, G_PNEXT(sb)++, 0); }
IMPX(MSVCP, "?_Lock@" SB "UAEXXZ", sb_lock) { ret_std(c, 0, 0); }
IMPX(MSVCP, "?_Unlock@" SB "UAEXXZ", sb_unlock) { ret_std(c, 0, 0); }
IMPX(MSVCP, "?setbuf@" SB "MAEPAV12@PAD_J@Z", sb_setbuf) { ret_std(c, THIS, 3); }
IMPX(MSVCP, "?imbue@" SB "MAEXABVlocale@2@@Z", sb_imbue) { ret_std(c, 0, 1); }
IMPX(MSVCP, "?sync@" SB "MAEHXZ", sb_sync) { ret_std(c, 0, 0); }
IMPX(MSVCP, "?showmanyc@" SB "MAE_JXZ", sb_showmanyc) { ret_i64(c, 0, 0); }
IMPX(MSVCP, "?uflow@" SB "MAEHXZ", sb_uflow) { ret_std(c, uflow_(c, THIS), 0); }
IMPX(MSVCP, "?xsgetn@" SB "MAE_JPAD_J@Z", sb_xsgetn) { ret_i64(c, xsgetn_(c, THIS, ARG(c, 0), I64ARG(c, 1)), 3); }
IMPX(MSVCP, "?xsputn@" SB "MAE_JPBD_J@Z", sb_xsputn) { ret_i64(c, xsputn_(c, THIS, ARG(c, 0), I64ARG(c, 1)), 3); }

// base-class virtuals for streambufs we constructed (used only until the derived constructor installs its vtable)
IMPX(INTERNAL, "sb_vdtor", sb_vdtor) {
    delete_locale(U32(THIS + 0x34));
    U32(THIS + 0x34) = 0;
    if (ARG(c, 0) & 1) gfree(THIS);
    ret_std(c, THIS, 1);
}
IMPX(INTERNAL, "sb_nop0", sb_nop0) { ret_std(c, 0, 0); }
IMPX(INTERNAL, "sb_nop1", sb_nop1) { ret_std(c, 0, 1); }
IMPX(INTERNAL, "sb_zero0", sb_zero0) { ret_std(c, 0, 0); }
IMPX(INTERNAL, "sb_eof0", sb_eof0) { ret_std(c, EOF_, 0); }
IMPX(INTERNAL, "sb_eof1", sb_eof1) { ret_std(c, EOF_, 1); }
IMPX(INTERNAL, "sb_zero64", sb_zero64) { ret_i64(c, 0, 0); }
IMPX(INTERNAL, "sb_uflow", sb_v_uflow) { ret_std(c, uflow_(c, THIS), 0); }
IMPX(INTERNAL, "sb_xsgetn", sb_v_xsgetn) { ret_i64(c, xsgetn_(c, THIS, ARG(c, 0), I64ARG(c, 1)), 3); }
IMPX(INTERNAL, "sb_xsputn", sb_v_xsputn) { ret_i64(c, xsputn_(c, THIS, ARG(c, 0), I64ARG(c, 1)), 3); }
static void bad_pos(uint32_t p) { memset(GM(p), 0, 24); V(GM(p), int64_t) = -1; }
IMPX(INTERNAL, "sb_seekoff", sb_v_seekoff) { bad_pos(ARG(c, 0)); ret_std(c, ARG(c, 0), 5); }   // (ret*, off i64, way, mode)
IMPX(INTERNAL, "sb_seekpos", sb_v_seekpos) { bad_pos(ARG(c, 0)); ret_std(c, ARG(c, 0), 8); }   // (ret*, fpos 24 bytes, mode)
IMPX(INTERNAL, "sb_setbuf", sb_v_setbuf) { ret_std(c, THIS, 3); }

// ---------- basic_ios ----------

#define STATE(ios) U32((ios) + 0x0C)
#define EXCEPT(ios) U32((ios) + 0x10)
#define FMTFL(ios) U32((ios) + 0x14)
static void ios_clear(uint32_t ios, uint32_t st) {   // basic_ios::clear: a missing streambuf always means badbit
    if (!U32(ios + 0x38)) st |= BADBIT;
    st &= 0x17;
    STATE(ios) = st;
    if (st & EXCEPT(ios)) die("iostream exception: state %x with exception mask %x", st, EXCEPT(ios));
}
static void ios_setstate(uint32_t ios, uint32_t st) { if (st) ios_clear(ios, STATE(ios) | st); }
static void ios_init(uint32_t ios, uint32_t sb) {     // basic_ios::init (ios_base::_Init + stream buffer)
    U32(ios + 0x30) = new_locale();
    U32(ios + 0x04) = U32(ios + 0x08) = 0;
    EXCEPT(ios) = 0;
    FMTFL(ios) = 0x201;                            // skipws | dec
    V(GM(ios + 0x18), int64_t) = 6;                // precision
    V(GM(ios + 0x20), int64_t) = 0;                // width
    U32(ios + 0x28) = U32(ios + 0x2C) = 0;
    STATE(ios) = 0;
    U32(ios + 0x38) = sb;
    U32(ios + 0x3C) = 0;
    *(uint8_t *)GM(ios + 0x40) = ' ';
    if (!sb) ios_setstate(ios, BADBIT);
}
static void ios_destroy(uint32_t ios) {                // ios_base::_Ios_base_dtor: locale + callback lists
    delete_locale(U32(ios + 0x30));
    U32(ios + 0x30) = 0;
    for (uint32_t p = U32(ios + 0x28), n; p; p = n) { n = U32(p); gfree(p); }
    for (uint32_t p = U32(ios + 0x2C), n; p; p = n) { n = U32(p); gfree(p); }
    U32(ios + 0x28) = U32(ios + 0x2C) = 0;
}
IMPX(MSVCP, "??0" IOS "IAE@XZ", ios_ctor) {
    init_once();
    memset(GM(THIS), 0, 0x48);
    U32(THIS) = vt_ios;
    ret_std(c, THIS, 0);
}
IMPX(MSVCP, "??1" IOS "UAE@XZ", ios_dtor) { init_once(); U32(THIS) = vt_ios; ios_destroy(THIS); ret_std(c, 0, 0); }
IMPX(INTERNAL, "ios_vdtor", ios_vdtor) { ios_destroy(THIS); if (ARG(c, 0) & 1) gfree(THIS); ret_std(c, THIS, 1); }
IMPX(MSVCP, "?setstate@" IOS "QAEXH_N@Z", ios_setstate_) { ios_setstate(THIS, ARG(c, 0)); ret_std(c, 0, 2); }
IMPX(MSVCP, "?widen@" IOS "QBEDD@Z", ios_widen) { ret_std(c, ARG(c, 0) & 0xFF, 1); }   // char -> char is the identity

// ---------- basic_istream / basic_ostream (virtual base basic_ios at this + vbtable[1]) ----------

static uint32_t ios_of(uint32_t s) { return s + U32(U32(s) + 4); }
static uint32_t rdbuf(uint32_t s) { return U32(ios_of(s) + 0x38); }
static void lock_sb(CPU *c, uint32_t s) { if (rdbuf(s)) vcall(c, rdbuf(s), 1, 0, NULL); }
static void unlock_sb(CPU *c, uint32_t s) { if (rdbuf(s)) vcall(c, rdbuf(s), 2, 0, NULL); }

// _Ipfx: test state, skip whitespace unless noskip. (Never tied: no stream we create has a tie.)
static int ipfx(CPU *c, uint32_t s, int noskip) {
    uint32_t ios = ios_of(s);
    if (STATE(ios)) { ios_setstate(ios, FAILBIT); return 0; }
    if (!noskip && (FMTFL(ios) & 1)) {
        uint32_t sb = rdbuf(s);
        for (uint32_t m = sgetc_(c, sb);; m = snextc_(c, sb)) {
            if (m == EOF_) { ios_setstate(ios, EOFBIT | FAILBIT); break; }
            if (!isspace((int)m)) break;
        }
    }
    return STATE(ios) == 0;
}
static void stream_ctor(CPU *c, uint32_t vbtable) {   // (streambuf*, isstd, hidden "most derived" flag)
    if (ARG(c, 2)) {                                   // we are the most-derived class: build the virtual base too
        U32(THIS) = vbtable;
        uint32_t ios = ios_of(THIS);
        memset(GM(ios), 0, 0x48);
        U32(ios) = vt_ios;
    }
    ios_init(ios_of(THIS), ARG(c, 0));                 // ponytail: isstd (_Addstd) only matters for cin/cout/cerr
}
IMPX(MSVCP, "??0" IS "QAE@PAV?$basic_streambuf@DU?$char_traits@D@std@@@1@_N@Z", is_ctor) {
    init_once();
    V(GM(THIS + 8), int64_t) = 0;                      // _Chcount
    stream_ctor(c, vt_istream_vb);
    ret_std(c, THIS, 3);
}
IMPX(MSVCP, "??0" OS "QAE@PAV?$basic_streambuf@DU?$char_traits@D@std@@@1@_N@Z", os_ctor) {
    init_once();
    stream_ctor(c, vt_ostream_vb);
    ret_std(c, THIS, 3);
}
IMPX(MSVCP, "??1" IS "UAE@XZ", is_dtor) { ret_std(c, 0, 0); }   // empty in the STL; basic_ios is destroyed separately
IMPX(MSVCP, "??1" OS "UAE@XZ", os_dtor) { ret_std(c, 0, 0); }
IMPX(MSVCP, "?_Ipfx@" IS "QAE_N_N@Z", is_ipfx) {
    uint32_t s = THIS;
    ret_std(c, (uint32_t)ipfx(c, s, ARG(c, 0) & 0xFF), 1);
}
IMPX(MSVCP, "?read@" IS "QAEAAV12@PAD_J@Z", is_read) {   // read(char*, streamsize)
    uint32_t s = THIS, p = ARG(c, 0), st = 0;
    int64_t n = I64ARG(c, 1);
    V(GM(s + 8), int64_t) = 0;
    lock_sb(c, s);
    if (ipfx(c, s, 1) && n > 0) {
        uint32_t a[3] = {p, (uint32_t)n, (uint32_t)((uint64_t)n >> 32)};
        vcall(c, rdbuf(s), 8, 3, a);                   // sgetn -> xsgetn
        int64_t got = ret64(c);
        V(GM(s + 8), int64_t) = got;
        if (got != n) st |= EOFBIT | FAILBIT;
    }
    unlock_sb(c, s);
    ios_setstate(ios_of(s), st);
    ret_std(c, s, 3);
}
// pubseekoff(off, way, in) on the game's streambuf; returns the resulting offset (-1 on failure)
static int64_t seekoff(CPU *c, uint32_t s, int64_t off, uint32_t way, uint32_t out) {
    uint32_t a[5] = {out, (uint32_t)off, (uint32_t)((uint64_t)off >> 32), way, 1 /* ios_base::in */};
    vcall(c, rdbuf(s), 10, 5, a);
    return V(GM(out), int64_t) + V(GM(out + 8), int64_t);   // fpos -> streamoff = _Myoff + _Fpos
}
static __thread uint32_t posbuf;
IMPX(MSVCP, "?seekg@" IS "QAEAAV12@_JH@Z", is_seekg) {   // seekg(off_type, seekdir)
    uint32_t s = THIS, ios = ios_of(s), st = 0;
    int64_t off = I64ARG(c, 0);
    uint32_t way = ARG(c, 2);
    if (!posbuf) posbuf = gmalloc(24);
    ios_clear(ios, STATE(ios) & ~EOFBIT);
    lock_sb(c, s);
    ipfx(c, s, 1);
    if (!(STATE(ios) & (FAILBIT | BADBIT)) && seekoff(c, s, off, way, posbuf) == -1) st |= FAILBIT;
    unlock_sb(c, s);
    ios_setstate(ios, st);
    ret_std(c, s, 3);
}
IMPX(MSVCP, "?tellg@" IS "QAE?AV?$fpos@U_Mbstatet@@@2@XZ", is_tellg) {   // returns fpos through the hidden pointer
    uint32_t s = THIS, ret = ARG(c, 0), ios = ios_of(s);
    lock_sb(c, s);
    ipfx(c, s, 1);
    if (!(STATE(ios) & (FAILBIT | BADBIT))) seekoff(c, s, 0, 1 /* cur */, ret);
    else bad_pos(ret);
    unlock_sb(c, s);
    ret_std(c, ret, 1);
}
IMPX(MSVCP, "?write@" OS "QAEAAV12@PBD_J@Z", os_write) {   // write(const char*, streamsize)
    uint32_t s = THIS, ios = ios_of(s), st = 0;
    int64_t n = I64ARG(c, 1);
    lock_sb(c, s);
    if (STATE(ios)) st |= BADBIT;
    else if (n > 0) {
        uint32_t a[3] = {ARG(c, 0), (uint32_t)n, (uint32_t)((uint64_t)n >> 32)};
        vcall(c, rdbuf(s), 9, 3, a);                   // sputn -> xsputn
        if (ret64(c) != n) st |= BADBIT;
    }
    if (!STATE(ios) && (FMTFL(ios) & 2) && vcall(c, rdbuf(s), 13, 0, NULL) == EOF_) st |= BADBIT;   // unitbuf: sync
    unlock_sb(c, s);
    ios_setstate(ios, st);
    ret_std(c, s, 3);
}

// ---------- _Fiopen + the std::_X* throwers ----------

IMPX(MSVCP, "?_Fiopen@std@@YAPAU_iobuf@@PBDHH@Z", fiopen) {   // (name, openmode, prot)
    static const uint32_t valid[] = {0x01, 0x02, 0x12, 0x0A, 0x21, 0x22, 0x32, 0x2A, 0x03, 0x13, 0x0B, 0x23, 0x33, 0x2B};
    static const char *modes[] = {"r", "w", "w", "a", "rb", "wb", "wb", "ab", "r+", "w+", "a+", "r+b", "w+b", "a+b"};
    uint32_t m = ARG(c, 1), ate = m & 0x04;
    if (m & 0x40) m |= 0x01;   // _Nocreate: must exist
    if (m & 0x08) m |= 0x02;   // app implies out
    m &= ~(0x04u | 0x40u);
    for (size_t i = 0; i < sizeof valid / sizeof *valid; i++) {
        if (valid[i] != m) continue;
        RET_CDECL(c, guest_fopen(GSTR(ARG(c, 0)), modes[i], ate != 0));
        return;
    }
    RET_CDECL(c, 0);   // _Noreplace and other modes: not used by the game
}
IMPX(MSVCP, "?_Xbad_alloc@std@@YAXXZ", xbad_alloc) { die("std::bad_alloc (out of guest memory?)"); }
IMPX(MSVCP, "?_Xlength_error@std@@YAXPBD@Z", xlength) { die("std::length_error: %s", GSTR(ARG(c, 0))); }
IMPX(MSVCP, "?_Xout_of_range@std@@YAXPBD@Z", xrange) { die("std::out_of_range: %s", GSTR(ARG(c, 0))); }
IMPX(MSVCP, "?_Xinvalid_argument@std@@YAXPBD@Z", xinvalid) { die("std::invalid_argument: %s", GSTR(ARG(c, 0))); }
