// CRT batch 2: strings, wide strings, conversions, sorting, random, time, environment, paths.
#define _GNU_SOURCE
#include "host.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CRT_STR "api-ms-win-crt-string-l1-1-0.dll"
#define CRT_CONV "api-ms-win-crt-convert-l1-1-0.dll"
#define CRT_UTIL "api-ms-win-crt-utility-l1-1-0.dll"
#define CRT_TIME "api-ms-win-crt-time-l1-1-0.dll"
#define CRT_ENV "api-ms-win-crt-environment-l1-1-0.dll"
#define CRT_FS "api-ms-win-crt-filesystem-l1-1-0.dll"
#define CRT_MB "api-ms-win-crt-multibyte-l1-1-0.dll"
#define CRT_RT "api-ms-win-crt-runtime-l1-1-0.dll"
#define VCRT "VCRUNTIME140.dll"
#define W(a, i) V(GM((a) + 2 * (i)), uint16_t)

static uint32_t gptr(const void *p) { return p ? (uint32_t)((const uint8_t *)p - M) : 0; }
static uint32_t wlen(uint32_t s) { uint32_t n = 0; while (W(s, n)) n++; return n; }
static uint16_t wlower(uint16_t ch) { return ch < 128 ? (uint16_t)tolower(ch) : ch; }

// ---------- narrow strings ----------

IMP(CRT_STR, strtok_s) {   // strtok_s(str, delim, context*)
    uint32_t s = ARG(c, 0) ? ARG(c, 0) : U32(ARG(c, 2));
    const char *d = GSTR(ARG(c, 1));
    if (!s) { RET_CDECL(c, 0); return; }
    s += strspn(GSTR(s), d);
    if (!*GSTR(s)) { U32(ARG(c, 2)) = s; RET_CDECL(c, 0); return; }
    uint32_t e = s + strcspn(GSTR(s), d);
    if (*GSTR(e)) { *GSTR(e) = 0; e++; }
    U32(ARG(c, 2)) = e;
    RET_CDECL(c, s);
}
IMP(CRT_RT, strerror) {
    static uint32_t buf;
    if (!buf) buf = gmalloc(128);
    snprintf(GSTR(buf), 128, "%s", strerror((int)ARG(c, 0)));
    RET_CDECL(c, buf);
}
IMP(CRT_RT, __sys_nerr) {
    static uint32_t p;
    if (!p) p = gmalloc(4), U32(p) = 141;   // count of MSVC errno messages
    RET_CDECL(c, p);
}

// ---------- wide strings (UTF-16 in guest memory) ----------

IMP(CRT_STR, wcslen) { RET_CDECL(c, wlen(ARG(c, 0))); }
static int wcmp(uint32_t a, uint32_t b, uint32_t n, int fold) {
    for (uint32_t i = 0; i < n; i++) {
        uint16_t x = W(a, i), y = W(b, i);
        if (fold) x = wlower(x), y = wlower(y);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}
IMP(CRT_STR, wcscmp) { RET_CDECL(c, (uint32_t)wcmp(ARG(c, 0), ARG(c, 1), 0xFFFFFFFF, 0)); }
IMP(CRT_STR, _wcsicmp) { RET_CDECL(c, (uint32_t)wcmp(ARG(c, 0), ARG(c, 1), 0xFFFFFFFF, 1)); }
IMP(CRT_STR, _wcsnicmp) { RET_CDECL(c, (uint32_t)wcmp(ARG(c, 0), ARG(c, 1), ARG(c, 2), 1)); }
IMP(CRT_STR, wcscpy_s) {
    uint32_t d = ARG(c, 0), n = ARG(c, 1), s = ARG(c, 2), len = wlen(s);
    if (len >= n) { if (n) W(d, 0) = 0; RET_CDECL(c, 34); return; }
    memmove(GM(d), GM(s), 2 * (len + 1));
    RET_CDECL(c, 0);
}
IMP(CRT_STR, _wcsupr) {
    for (uint32_t s = ARG(c, 0), i = 0; W(s, i); i++) if (W(s, i) < 128) W(s, i) = (uint16_t)toupper(W(s, i));
    RET_CDECL(c, ARG(c, 0));
}
IMP(VCRT, wcsrchr) {
    uint32_t s = ARG(c, 0), r = 0;
    uint16_t ch = (uint16_t)ARG(c, 1);
    for (uint32_t i = 0;; i++) { if (W(s, i) == ch) r = s + 2 * i; if (!W(s, i)) break; }
    RET_CDECL(c, r);
}
IMP(VCRT, wcsstr) {
    uint32_t s = ARG(c, 0), k = ARG(c, 1), n = wlen(k);
    for (uint32_t i = 0;; i++) {
        if (!wcmp(s + 2 * i, k, n, 0) || !n) { RET_CDECL(c, s + 2 * i); return; }
        if (!W(s, i)) break;
    }
    RET_CDECL(c, 0);
}

// ---------- conversions ----------

IMP(CRT_CONV, atoi) {   // UCRT clamps out-of-range input (atoi("3000000000") = INT_MAX); glibc's atoi wraps
    long long v = strtoll(GSTR(ARG(c, 0)), NULL, 10);
    RET_CDECL(c, (uint32_t)(int32_t)(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v));
}
IMP(CRT_CONV, atof) { ret_st0(c, atof(GSTR(ARG(c, 0)))); RET_CDECL(c, REG(c, R_EAX)); }
static void strto(CPU *c, int kind) {
    char *end;
    const char *s = GSTR(ARG(c, 0));
    long long v = kind == 0 ? (long long)(int32_t)strtol(s, &end, (int)ARG(c, 2)) : strtoll(s, &end, (int)ARG(c, 2));
    if (kind == 0 && (v > INT32_MAX || v < INT32_MIN)) v = v > 0 ? INT32_MAX : INT32_MIN;   // 32-bit long
    if (kind == 1) v = v > (long long)UINT32_MAX || v < -(long long)UINT32_MAX ? UINT32_MAX : (uint32_t)v;   // MSVC: overflow = ULONG_MAX
    if (ARG(c, 1)) U32(ARG(c, 1)) = gptr(end);
    REG(c, R_EDX) = (uint32_t)((uint64_t)v >> 32);
    RET_CDECL(c, (uint32_t)v);
}
IMP(CRT_CONV, strtol) { strto(c, 0); }
IMP(CRT_CONV, strtoul) { strto(c, 1); }
IMP(CRT_CONV, strtoll) { strto(c, 2); }
IMP(CRT_CONV, mbstowcs) {   // ASCII/Latin-1 widen; dst NULL = count only
    uint32_t d = ARG(c, 0), n = ARG(c, 2);
    const uint8_t *s = (const uint8_t *)GSTR(ARG(c, 1));
    uint32_t i = 0;
    for (; s[i] && (!d || i < n); i++) if (d) W(d, i) = s[i];
    if (d && i < n) W(d, i) = 0;
    RET_CDECL(c, i);
}
IMP(CRT_CONV, _itow_s) {    // _itow_s(value, buf, size, radix)
    char tmp[40];
    int32_t v = (int32_t)ARG(c, 0);
    int radix = (int)ARG(c, 3);
    uint32_t u = (radix == 10 && v < 0) ? (uint32_t)-(int64_t)v : (uint32_t)v, n = 0;
    do { int d = u % radix; tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= radix; } while (u);
    if (radix == 10 && v < 0) tmp[n++] = '-';
    if (n + 1 > ARG(c, 2)) { RET_CDECL(c, 34); return; }
    for (uint32_t i = 0; i < n; i++) W(ARG(c, 1), i) = (uint16_t)tmp[n - 1 - i];
    W(ARG(c, 1), n) = 0;
    RET_CDECL(c, 0);
}

// ---------- msvcrt.dll only ----------

#define MSVCRT "msvcrt.dll"
IMP(MSVCRT, atol) { RET_CDECL(c, (uint32_t)atoi(GSTR(ARG(c, 0)))); }   // long is 32-bit
IMPX(MSVCRT, "atoi", ms_atoi) { RET_CDECL(c, (uint32_t)atoi(GSTR(ARG(c, 0)))); }   // old msvcrt.dll wraps on overflow, like glibc
static void to_radix(CPU *c, uint32_t u, int neg) {   // _itoa/_ultoa(value, buf, radix) -> buf
    char tmp[40];
    uint32_t n = 0, radix = ARG(c, 2), buf = ARG(c, 1);
    do { uint32_t d = u % radix; tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= radix; } while (u);
    if (neg) tmp[n++] = '-';
    for (uint32_t i = 0; i < n; i++) GSTR(buf)[i] = tmp[n - 1 - i];
    GSTR(buf)[n] = 0;
    RET_CDECL(c, buf);
}
IMP(MSVCRT, _itoa) {
    int32_t v = (int32_t)ARG(c, 0);
    int neg = ARG(c, 2) == 10 && v < 0;
    to_radix(c, neg ? (uint32_t)-(int64_t)v : (uint32_t)v, neg);
}
IMP(MSVCRT, _ultoa) { to_radix(c, ARG(c, 0), 0); }
IMP(MSVCRT, _mbstrlen) { RET_CDECL(c, strlen(GSTR(ARG(c, 0)))); }   // code page 1252: one byte per char
IMP(MSVCRT, isleadbyte) { RET_CDECL(c, 0); }
IMP(MSVCRT, setlocale) {   // the "C" locale is the only one, whatever is asked
    static uint32_t s;
    if (!s) { s = gmalloc(2); strcpy(GSTR(s), "C"); }
    RET_CDECL(c, s);
}
#define WCTYPE(f, test) IMP(MSVCRT, f) { uint32_t ch = ARG(c, 0) & 0xFFFF; RET_CDECL(c, ch < 128 && test((int)ch)); }
WCTYPE(iswalpha, isalpha) WCTYPE(iswdigit, isdigit) WCTYPE(iswspace, isspace) WCTYPE(iswpunct, ispunct)
IMP(MSVCRT, wctomb) {   // (char* out, wchar) -> bytes; Latin-1 only
    uint32_t ch = ARG(c, 1) & 0xFFFF;
    if (!ARG(c, 0)) { RET_CDECL(c, 0); return; }
    if (ch > 0xFF) { RET_CDECL(c, 0xFFFFFFFF); return; }
    GSTR(ARG(c, 0))[0] = (char)ch;
    RET_CDECL(c, 1);
}
IMP(MSVCRT, wcstombs) {   // (dst, wide src, n) -> bytes; Latin-1 only, dst NULL = count only
    uint32_t d = ARG(c, 0), s = ARG(c, 1), n = ARG(c, 2), i = 0;
    for (; W(s, i) && (!d || i < n); i++) {
        if (W(s, i) > 0xFF) { RET_CDECL(c, 0xFFFFFFFF); return; }
        if (d) GSTR(d)[i] = (char)W(s, i);
    }
    if (d && i < n) GSTR(d)[i] = 0;
    RET_CDECL(c, i);
}

// ---------- qsort with a guest comparator; rand/srand with MSVC's exact LCG ----------

// The CRT's own algorithm (quicksort, median of three, <= 8 elements by repeated max-to-the-end), so elements that
// compare equal end up in the same order as on Windows; glibc's merge sort orders them differently.
static int gcmp(CPU *c, uint32_t cmp, uint32_t a, uint32_t b) {
    uint32_t esp = REG(c, R_ESP), r = guest_call(c, cmp, 0, 2, a, b);
    REG(c, R_ESP) = esp;
    return (int32_t)r;
}
static void gswap(uint32_t a, uint32_t b, uint32_t w) {
    if (a == b) return;
    uint8_t *p = GM(a), *q = GM(b);
    while (w--) { uint8_t t = *p; *p++ = *q; *q++ = t; }
}
IMP(CRT_UTIL, qsort) {   // (base, num, width, cmp)
    uint32_t num = ARG(c, 1), w = ARG(c, 2), cmp = ARG(c, 3), lostk[64], histk[64], lo, hi, mid, loguy, higuy;
    int sp = 0;
    if (num < 2 || !w) { RET_CDECL(c, 0); return; }
    lo = ARG(c, 0); hi = lo + w * (num - 1);
    for (;;) {
        uint32_t size = (hi - lo) / w + 1;
        if (size <= 8) {
            for (uint32_t h = hi; h > lo; h -= w) {
                uint32_t max = lo;
                for (uint32_t p = lo + w; p <= h; p += w) if (gcmp(c, cmp, p, max) > 0) max = p;
                gswap(max, h, w);
            }
        } else {
            mid = lo + (size / 2) * w;
            if (gcmp(c, cmp, lo, mid) > 0) gswap(lo, mid, w);
            if (gcmp(c, cmp, lo, hi) > 0) gswap(lo, hi, w);
            if (gcmp(c, cmp, mid, hi) > 0) gswap(mid, hi, w);
            loguy = lo; higuy = hi;
            for (;;) {
                if (mid > loguy) do loguy += w; while (loguy < mid && gcmp(c, cmp, loguy, mid) <= 0);
                if (mid <= loguy) do loguy += w; while (loguy <= hi && gcmp(c, cmp, loguy, mid) <= 0);
                do higuy -= w; while (higuy > mid && gcmp(c, cmp, higuy, mid) > 0);
                if (higuy < loguy) break;
                gswap(loguy, higuy, w);
                if (mid == higuy) mid = loguy;
            }
            higuy += w;
            if (mid < higuy) do higuy -= w; while (higuy > mid && gcmp(c, cmp, higuy, mid) == 0);
            if (mid >= higuy) do higuy -= w; while (higuy > lo && gcmp(c, cmp, higuy, mid) == 0);
            if ((int32_t)(higuy - lo) >= (int32_t)(hi - loguy)) {   // push the bigger part, go on with the smaller
                if (lo < higuy) { lostk[sp] = lo; histk[sp++] = higuy; }
                if (loguy < hi) { lo = loguy; continue; }
            } else {
                if (loguy < hi) { lostk[sp] = loguy; histk[sp++] = hi; }
                if (lo < higuy) { hi = higuy; continue; }
            }
        }
        if (--sp < 0) break;
        lo = lostk[sp]; hi = histk[sp];
    }
    RET_CDECL(c, 0);
}
static __thread uint32_t holdrand = 1;   // per thread, seed 1, like the CRT (ucrtbase rand: ptd+0x18)
IMP(CRT_UTIL, srand) { holdrand = ARG(c, 0); RET_CDECL(c, 0); }
IMP(CRT_UTIL, rand) { holdrand = holdrand * 214013u + 2531011u; RET_CDECL(c, (holdrand >> 16) & 0x7FFF); }

// ---------- time (struct tm is 9 ints, same layout as C) ----------

static void tm_to_guest(uint32_t g, const struct tm *t) {
    int v[9] = {t->tm_sec, t->tm_min, t->tm_hour, t->tm_mday, t->tm_mon, t->tm_year, t->tm_wday, t->tm_yday, t->tm_isdst};
    memcpy(GM(g), v, 36);
}
static void tm_from_guest(struct tm *t, uint32_t g) {
    int v[9];
    memcpy(v, GM(g), 36);
    memset(t, 0, sizeof *t);
    t->tm_sec = v[0]; t->tm_min = v[1]; t->tm_hour = v[2]; t->tm_mday = v[3]; t->tm_mon = v[4];
    t->tm_year = v[5]; t->tm_wday = v[6]; t->tm_yday = v[7]; t->tm_isdst = v[8];
}
IMP(CRT_TIME, _time64) {
    int64_t t = (int64_t)time(NULL);
    if (ARG(c, 0)) V(GM(ARG(c, 0)), int64_t) = t;
    REG(c, R_EDX) = (uint32_t)((uint64_t)t >> 32);
    RET_CDECL(c, (uint32_t)t);
}
static uint32_t static_tm(void) { static uint32_t p; if (!p) p = gmalloc(36); return p; }
static void tm_conv(CPU *c, int local) {
    time_t t = (time_t)V(GM(ARG(c, 0)), int64_t);
    struct tm r;
    if (!(local ? localtime_r(&t, &r) : gmtime_r(&t, &r))) { RET_CDECL(c, 0); return; }
    tm_to_guest(static_tm(), &r);
    RET_CDECL(c, static_tm());
}
IMP(CRT_TIME, _gmtime64) { tm_conv(c, 0); }
IMP(CRT_TIME, _localtime64) { tm_conv(c, 1); }
IMP(CRT_TIME, _gmtime64_s) {
    time_t t = (time_t)V(GM(ARG(c, 1)), int64_t);
    struct tm r;
    if (!gmtime_r(&t, &r)) { RET_CDECL(c, 22); return; }
    tm_to_guest(ARG(c, 0), &r);
    RET_CDECL(c, 0);
}
// msvcrt.dll (d3dx9): "MM/DD/YY" and "HH:MM:SS" into a 9-byte buffer
static void strtime_(CPU *c, const char *f) {
    time_t t = time(NULL);
    struct tm r;
    localtime_r(&t, &r);
    strftime(GSTR(ARG(c, 0)), 9, f, &r);
    RET_CDECL(c, ARG(c, 0));
}
IMP(MSVCRT, _strdate) { strtime_(c, "%m/%d/%y"); }
IMP(MSVCRT, _strtime) { strtime_(c, "%H:%M:%S"); }
IMP(MSVCRT, _tempnam) { RET_CDECL(c, 0); }   // ponytail: no temp files; NULL is the documented failure callers handle
IMP(CRT_TIME, strftime) {
    struct tm t;
    tm_from_guest(&t, ARG(c, 3));
    RET_CDECL(c, strftime(GSTR(ARG(c, 0)), ARG(c, 1), GSTR(ARG(c, 2)), &t));
}

// ---------- environment: the game sees an empty environment ----------

IMP(CRT_ENV, getenv) { RET_CDECL(c, 0); }
IMP(CRT_ENV, getenv_s) { U32(ARG(c, 0)) = 0; if (ARG(c, 2)) GSTR(ARG(c, 1))[0] = 0; RET_CDECL(c, 0); }

// ---------- multibyte (code page 1252 is single-byte, so these are the plain byte versions) ----------

IMP(CRT_MB, _mbschr) { RET_CDECL(c, gptr(strchr(GSTR(ARG(c, 0)), (int)ARG(c, 1)))); }
IMP(CRT_MB, _mbspbrk) { RET_CDECL(c, gptr(strpbrk(GSTR(ARG(c, 0)), GSTR(ARG(c, 1))))); }
IMP(CRT_MB, _mbsnbcpy) {   // byte-wise like the CRT (overlap-safe the same way: see fwd_copy in win32.c)
    char *d = GSTR(ARG(c, 0)); const char *s = GSTR(ARG(c, 1));
    size_t n = ARG(c, 2), i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    RET_CDECL(c, ARG(c, 0));
}

// ---------- paths ----------

IMP(CRT_FS, _splitpath_s) {   // (path, drive, dsz, dir, dirsz, fname, fsz, ext, esz)
    const char *p = GSTR(ARG(c, 0)), *drive_end = p, *dir_end, *dot = NULL;
    if (p[0] && p[1] == ':') drive_end = p + 2;
    dir_end = drive_end;
    for (const char *q = drive_end; *q; q++) if (*q == '\\' || *q == '/') dir_end = q + 1;
    for (const char *q = dir_end; *q; q++) if (*q == '.') dot = q;
    if (!dot) dot = dir_end + strlen(dir_end);
    const char *parts[4][2] = {{p, drive_end}, {drive_end, dir_end}, {dir_end, dot}, {dot, dot + strlen(dot)}};
    for (int i = 0; i < 4; i++) {
        uint32_t buf = ARG(c, 1 + 2 * i), sz = ARG(c, 2 + 2 * i);
        size_t n = parts[i][1] - parts[i][0];
        if (!buf) continue;
        if (n >= sz) { RET_CDECL(c, 34); return; }
        memcpy(GSTR(buf), parts[i][0], n);
        GSTR(buf)[n] = 0;
    }
    RET_CDECL(c, 0);
}
IMP("api-ms-win-crt-stdio-l1-1-0.dll", _getcwd) {   // the game's working directory is its install folder
    const char *cwd = "C:\\Game";
    uint32_t buf = ARG(c, 0), n = ARG(c, 1);
    if (!buf) buf = gmalloc(n > strlen(cwd) ? n : strlen(cwd) + 1);
    else if (strlen(cwd) >= n) { RET_CDECL(c, 0); return; }
    strcpy(GSTR(buf), cwd);
    RET_CDECL(c, buf);
}
IMP(CRT_FS, _mkdir) {
    char hp[1024];
    host_path(GSTR(ARG(c, 0)), hp, sizeof hp);
    RET_CDECL(c, mkdir(hp, 0755) == 0 || errno == EEXIST ? 0 : 0xFFFFFFFF);
}
IMP(CRT_FS, _chdir) { RET_CDECL(c, 0); }   // ponytail: cwd stays the game folder (all paths resolve against it)
static void stat_out(uint32_t g, const struct stat *s) {   // MSVC struct _stat64 (56 bytes)
    memset(GM(g), 0, 56);
    V(GM(g + 6), uint16_t) = (S_ISDIR(s->st_mode) ? 0x4000 : 0x8000) | 0x1B6;   // _S_IFDIR / _S_IFREG, rw
    V(GM(g + 8), uint16_t) = 1;
    V(GM(g + 24), int64_t) = s->st_size;
    V(GM(g + 32), int64_t) = s->st_atime;
    V(GM(g + 40), int64_t) = s->st_mtime;
    V(GM(g + 48), int64_t) = s->st_ctime;
}
IMP(CRT_FS, _stat64) {
    char hp[1024];
    struct stat s;
    host_path(GSTR(ARG(c, 0)), hp, sizeof hp);
    if (stat(hp, &s)) { RET_CDECL(c, 0xFFFFFFFF); return; }
    stat_out(ARG(c, 1), &s);
    RET_CDECL(c, 0);
}
