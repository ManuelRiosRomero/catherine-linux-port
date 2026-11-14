// CRT printf family on guest varargs: each %-conversion is re-issued to the host printf with its argument
// fetched from the 32-bit guest va_list (ints 4 bytes, int64/double 8 bytes, pointers 4 bytes).
#define _GNU_SOURCE
#include "host.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// UTF-16 <-> UTF-8 (the formatter works in UTF-8; wide strings are converted at the edges)
static size_t utf16_to_utf8(uint32_t w, size_t nchars, char *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < nchars && n + 4 < cap; i++) {
        uint32_t u = V(GM(w + 2 * i), uint16_t);
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < nchars) {   // surrogate pair
            uint32_t lo = V(GM(w + 2 * (i + 1)), uint16_t);
            if (lo >= 0xDC00 && lo < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); i++; }
        }
        if (u < 0x80) out[n++] = (char)u;
        else if (u < 0x800) { out[n++] = (char)(0xC0 | u >> 6); out[n++] = (char)(0x80 | (u & 0x3F)); }
        else if (u < 0x10000) { out[n++] = (char)(0xE0 | u >> 12); out[n++] = (char)(0x80 | (u >> 6 & 0x3F)); out[n++] = (char)(0x80 | (u & 0x3F)); }
        else { out[n++] = (char)(0xF0 | u >> 18); out[n++] = (char)(0x80 | (u >> 12 & 0x3F)); out[n++] = (char)(0x80 | (u >> 6 & 0x3F)); out[n++] = (char)(0x80 | (u & 0x3F)); }
    }
    out[n] = 0;
    return n;
}
static size_t utf8_to_utf16(const char *s, size_t len, uint16_t *out, size_t cap) {   // returns UTF-16 units
    size_t n = 0;
    for (size_t i = 0; i < len;) {
        uint8_t b = (uint8_t)s[i];
        uint32_t u;
        int k = b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
        u = k == 1 ? b : k == 2 ? b & 0x1F : k == 3 ? b & 0x0F : b & 0x07;
        for (int j = 1; j < k && i + j < len; j++) u = u << 6 | ((uint8_t)s[i + j] & 0x3F);
        i += k;
        if (u >= 0x10000) {
            if (out && n + 1 < cap) { out[n] = (uint16_t)(0xD800 + ((u - 0x10000) >> 10)); out[n + 1] = (uint16_t)(0xDC00 + ((u - 0x10000) & 0x3FF)); }
            n += 2;
        } else { if (out && n < cap) out[n] = (uint16_t)u; n++; }
    }
    return n;
}
static void wide_arg(FILE *o, const char *spec, uint32_t w) {   // a UTF-16 guest string argument
    static __thread char buf[16384];
    size_t n = 0;
    if (w) while (V(GM(w + 2 * n), uint16_t) && n < 4000) n++;
    utf16_to_utf8(w, n, buf, sizeof buf);
    fprintf(o, spec, w ? buf : "(null)");
}

// Formats into a malloc'd host string (UTF-8); returns its length. *out must be freed.
// wide = the format came from a wide printf in MSVC's legacy mode: there %s/%c mean wide and %S/%hs narrow.
size_t guest_vformat_w(char **out, const char *f, uint32_t va, int wide) {
    size_t len;
    FILE *o = open_memstream(out, &len);
    while (*f) {
        if (*f != '%') { fputc(*f++, o); continue; }
        if (f[1] == '%') { fputc('%', o); f += 2; continue; }
        char spec[64] = "%";
        size_t n = 1;
        const char *p = f + 1;
        while (strchr("-+ #0", *p) && n < 40) spec[n++] = *p++;                  // flags
        for (int part = 0; part < 2; part++) {                                    // width, then .precision
            if (part) { if (*p != '.') break; spec[n++] = *p++; }
            if (*p == '*') { n += snprintf(spec + n, sizeof spec - n, "%d", (int32_t)U32(va)); va += 4; p++; }
            else while (*p >= '0' && *p <= '9' && n < 50) spec[n++] = *p++;
        }
        int wide64 = 0, half = 0, longmod = 0;
        for (;; p++) {                                                            // length modifiers
            if (!strncmp(p, "I64", 3)) { wide64 = 1; p += 2; }
            else if (!strncmp(p, "I32", 3)) p += 2;
            else if (*p == 'l' && p[1] == 'l') { wide64 = 1; p++; }
            else if (*p == 'l' || *p == 'w') longmod = 1;
            else if (*p == 'h') half++;
            else if (*p == 'L' || *p == 'z' || *p == 't' || *p == 'j' || *p == 'I') ;
            else break;
        }
        char conv = *p ? *p++ : 0;
        f = p;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
            if (wide64) { strcpy(spec + n, "ll"); n += 2; }
            else if (half == 1) spec[n++] = 'h';
            else if (half >= 2) { spec[n++] = 'h'; spec[n++] = 'h'; }
            spec[n++] = conv; spec[n] = 0;
            if (wide64) { fprintf(o, spec, V(GM(va), uint64_t)); va += 8; }
            else if (conv == 'd' || conv == 'i') { fprintf(o, spec, (int32_t)U32(va)); va += 4; }
            else { fprintf(o, spec, U32(va)); va += 4; }
            break;
        case 'c': case 'C': {
            int is_wide = longmod || (wide ? conv == 'c' && !half : conv == 'C');
            uint32_t ch = U32(va);
            va += 4;
            if (is_wide && (ch & 0xFFFF) >= 0x80) {   // non-ASCII wide char: print its UTF-8 bytes
                char u[8]; uint32_t tmp = gmalloc(4); V(GM(tmp), uint16_t) = (uint16_t)ch; V(GM(tmp + 2), uint16_t) = 0;
                utf16_to_utf8(tmp, 1, u, sizeof u); gfree(tmp);
                spec[n++] = 's'; spec[n] = 0; fprintf(o, spec, u);
            } else { spec[n++] = 'c'; spec[n] = 0; fprintf(o, spec, (int)(ch & 0xFF)); }
            break;
        }
        case 's': case 'S': {
            int is_wide = longmod || (wide ? conv == 's' && !half : conv == 'S');
            spec[n++] = 's'; spec[n] = 0;
            if (is_wide) wide_arg(o, spec, U32(va));
            else fprintf(o, spec, U32(va) ? GSTR(U32(va)) : "(null)");
            va += 4;
            break;
        }
        case 'p':
            fprintf(o, "%08X", U32(va)); va += 4;   // MSVC prints pointers as 8 upper-case hex digits
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            spec[n++] = conv; spec[n] = 0;
            fprintf(o, spec, V(GM(va), double)); va += 8;
            break;
        case 'n':
            fflush(o);
            U32(U32(va)) = (uint32_t)len; va += 4;
            break;
        default:   // unknown conversion: print it literally so it shows up in logs
            fputs(spec, o);
            if (conv) fputc(conv, o);
        }
    }
    fclose(o);
    return len;
}

size_t guest_vformat(char **out, const char *f, uint32_t va) { return guest_vformat_w(out, f, va, 0); }

// Wide printf: UTF-16 format -> UTF-8, format, UTF-8 result -> UTF-16. Returns the result length in UTF-16 units;
// writes at most cap units (incl. NUL) when buf is set. Options bit 2 (=4) = legacy wide specifiers (%s is wide).
static int32_t wide_format(uint32_t buf, uint32_t cap, uint32_t fmt, uint32_t va, uint32_t opts, int *fits) {
    size_t fn = 0;
    while (V(GM(fmt + 2 * fn), uint16_t)) fn++;
    char *f8 = malloc(4 * fn + 1), *s;
    utf16_to_utf8(fmt, fn, f8, 4 * fn + 1);
    size_t len = guest_vformat_w(&s, f8, va, (opts & 4) != 0);
    free(f8);
    size_t units = utf8_to_utf16(s, len, NULL, 0);
    *fits = units < cap;
    if (buf && cap) {
        size_t w = utf8_to_utf16(s, len, (uint16_t *)GM(buf), cap - 1);
        if (w > cap - 1) w = cap - 1;
        V(GM(buf + 2 * w), uint16_t) = 0;
    }
    free(s);
    return (int32_t)units;
}

// __stdio_common_vsprintf(options (8 bytes), buf, count, fmt, locale, va): options bit 1 = C99 snprintf return.
static void common_vsprintf(CPU *c, int secure) {
    uint32_t opts = ARG(c, 0), buf = ARG(c, 2), cap = ARG(c, 3), fmt = ARG(c, 4), va = ARG(c, 6);
    char *s;
    size_t len = guest_vformat(&s, GSTR(fmt), va);
    int32_t r = (int32_t)len;
    if (buf && cap) {
        if (len < cap) memcpy(GSTR(buf), s, len + 1);
        else if (secure) { GSTR(buf)[0] = 0; r = -1; }               // *_s: overflow empties the buffer
        else { memcpy(GSTR(buf), s, cap - 1); GSTR(buf)[cap - 1] = 0; if (!(opts & 2)) r = -1; }
    } else if (buf && !cap && !(opts & 2)) r = -1;
    free(s);
    RET_CDECL(c, (uint32_t)r);
}
#define CRT_STDIO "api-ms-win-crt-stdio-l1-1-0.dll"
IMP(CRT_STDIO, __stdio_common_vsprintf) { common_vsprintf(c, 0); }
IMP(CRT_STDIO, __stdio_common_vsprintf_s) { common_vsprintf(c, 1); }
// __stdio_common_vsnprintf_s(options, buf, size, count, fmt, locale, va): count = max chars, -1 = _TRUNCATE
IMP(CRT_STDIO, __stdio_common_vsnprintf_s) {
    uint32_t buf = ARG(c, 2), size = ARG(c, 3), count = ARG(c, 4), fmt = ARG(c, 5), va = ARG(c, 7);
    char *s;
    size_t len = guest_vformat(&s, GSTR(fmt), va), lim = count == 0xFFFFFFFF || count >= size ? size - 1 : count;
    int32_t r = (int32_t)len;
    if (len > lim) {
        if (count == 0xFFFFFFFF || count < size) { memcpy(GSTR(buf), s, lim); GSTR(buf)[lim] = 0; r = -1; }
        else { GSTR(buf)[0] = 0; r = -1; }
    } else memcpy(GSTR(buf), s, len + 1);
    free(s);
    RET_CDECL(c, (uint32_t)r);
}

// ---------- scanf family ----------
// Each conversion runs through host sscanf with a trailing %n (to learn how much input it used) and the result is
// stored through the guest pointer. options bit 0 (_CRT_INTERNAL_SCANF_SECURECRT) = sscanf_s: %s %c %[ take a size arg.
static int32_t guest_vscan(const char *in, size_t inlen, const char *f, uint32_t va, int secure) {
    int32_t assigned = 0;
    const char *p = in, *end = in + inlen;
    while (*f) {
        if (isspace((unsigned char)*f)) { while (p < end && isspace((unsigned char)*p)) p++; f++; continue; }
        if (*f != '%' || f[1] == '%') {                       // literal (or %%) must match exactly
            char want = *f == '%' ? '%' : *f;
            f += *f == '%' ? 2 : 1;
            if (p >= end || *p != want) return assigned ? assigned : (p >= end ? -1 : 0);
            p++;
            continue;
        }
        f++;
        int suppress = *f == '*';
        if (suppress) f++;
        int width = 0;
        while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0');
        int size = 4;                                         // bytes of the integer / float target
        if (!strncmp(f, "I64", 3) || !strncmp(f, "ll", 2)) { size = 8; f += f[0] == 'I' ? 3 : 2; }
        else if (!strncmp(f, "hh", 2)) { size = 1; f += 2; }
        else if (*f == 'h') { size = 2; f++; }
        else if (*f == 'l' || *f == 'L') { size = f[1] && strchr("eEfgGaA", f[1]) ? 8 : 4; f++; }  // %lf = double; %ld stays 4 bytes on Windows
        char conv = *f++;
        if (conv == 'n') { if (!suppress) U32(U32(va)) = (uint32_t)(p - in), va += 4; continue; }
        if (conv != 'c' && conv != '[') while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end) return assigned ? assigned : -1;

        char spec[300], scanset[260] = "";
        int used = 0, ok;
        if (conv == '[') {                                    // copy the scanset verbatim, ']' right after '[' or '^' is literal
            size_t k = 0;
            if (*f == '^') scanset[k++] = *f++;
            if (*f == ']') scanset[k++] = *f++;
            while (*f && *f != ']' && k < sizeof scanset - 1) scanset[k++] = *f++;
            scanset[k] = 0;
            if (*f == ']') f++;
        }
        char buf[4096];
        size_t avail = (size_t)(end - p);
        char *tmpin = strndup(p, avail);                      // host sscanf needs a NUL-terminated input
        if (conv == 's' || conv == 'c' || conv == '[') {
            int w = width ? width : conv == 'c' ? 1 : (int)sizeof buf - 1;
            if (w > (int)sizeof buf - 1) w = sizeof buf - 1;
            if (conv == '[') snprintf(spec, sizeof spec, "%%%d[%s]%%n", w, scanset);
            else snprintf(spec, sizeof spec, "%%%d%c%%n", w, conv);
            ok = sscanf(tmpin, spec, buf, &used);
            if (ok == 1 && !suppress) {
                uint32_t dst = U32(va), cap = secure ? U32(va + 4) : 0xFFFFFFFF;
                size_t n = conv == 'c' ? (size_t)used : strlen(buf);
                if (conv != 'c' && n + 1 > cap) { free(tmpin); return assigned; }   // sscanf_s: buffer too small
                memcpy(GSTR(dst), buf, n);
                if (conv != 'c') GSTR(dst)[n] = 0;
            }
            if (!suppress) va += secure ? 8 : 4;
        } else if (strchr("eEfgGaA", conv)) {
            double d;
            snprintf(spec, sizeof spec, width ? "%%%dlf%%n" : "%%lf%%n", width);
            ok = sscanf(tmpin, spec, &d, &used);
            if (ok == 1 && !suppress) { if (size == 8) V(GM(U32(va)), double) = d; else V(GM(U32(va)), float) = (float)d; }
            if (!suppress) va += 4;
        } else if (strchr("diuxXop", conv)) {
            long long v;
            char cc = conv == 'p' ? 'x' : conv == 'u' ? 'u' : conv;
            if (cc == 'X') cc = 'x';
            snprintf(spec, sizeof spec, width ? "%%%dll%c%%n" : "%%ll%c%%n", width, cc);
            if (!width) snprintf(spec, sizeof spec, "%%ll%c%%n", cc);
            ok = sscanf(tmpin, spec, &v, &used);
            if (ok == 1 && !suppress) {
                uint32_t dst = U32(va);
                if (size == 8) V(GM(dst), int64_t) = v;
                else if (size == 2) V(GM(dst), uint16_t) = (uint16_t)v;
                else if (size == 1) V(GM(dst), uint8_t) = (uint8_t)v;
                else U32(dst) = (uint32_t)v;
            }
            if (!suppress) va += 4;
        } else { free(tmpin); return assigned; }              // unknown conversion: stop like the CRT would
        free(tmpin);
        if (ok != 1) return assigned;
        p += used;
        if (!suppress) assigned++;
    }
    return assigned;
}
// __stdio_common_vsscanf(options (8 bytes), buffer, buflen (-1 = NUL-terminated), format, locale, va)
IMP(CRT_STDIO, __stdio_common_vsscanf) {
    uint32_t buf = ARG(c, 2), len = ARG(c, 3), fmt = ARG(c, 4), va = ARG(c, 6);
    size_t n = len == 0xFFFFFFFF ? strlen(GSTR(buf)) : strnlen(GSTR(buf), len);
    RET_CDECL(c, (uint32_t)guest_vscan(GSTR(buf), n, GSTR(fmt), va, ARG(c, 0) & 1));
}
// __stdio_common_vswscanf: same arguments, UTF-16 text and format.
// ponytail: narrowed to Latin-1 and scanned as narrow text; fine for numbers (the game's only use: "VID_%4X" in a
// gamepad device id), but %s/%c would store narrow strings. Widen those if a wide-string conversion ever shows up.
IMP(CRT_STDIO, __stdio_common_vswscanf) {
    uint32_t buf = ARG(c, 2), len = ARG(c, 3), fmt = ARG(c, 4), va = ARG(c, 6);
    char in[1024], f[256];
    size_t n = 0, k = 0;
    for (; n < sizeof in - 1 && (len == 0xFFFFFFFF || n < len) && V(GM(buf + 2 * n), uint16_t); n++) {
        uint16_t ch = V(GM(buf + 2 * n), uint16_t);
        in[n] = ch < 256 ? (char)ch : '?';
    }
    for (; k < sizeof f - 1 && V(GM(fmt + 2 * k), uint16_t); k++) f[k] = (char)V(GM(fmt + 2 * k), uint16_t);
    in[n] = f[k] = 0;
    RET_CDECL(c, (uint32_t)guest_vscan(in, n, f, va, ARG(c, 0) & 1));
}

// __stdio_common_vswprintf(options, buf, count, fmt, locale, va) and the _s variant (overflow -> empty + -1)
static void common_vswprintf(CPU *c, int secure) {
    uint32_t opts = ARG(c, 0), buf = ARG(c, 2), cap = ARG(c, 3), fmt = ARG(c, 4), va = ARG(c, 6);
    int fits;
    int32_t r = wide_format(buf, cap, fmt, va, opts, &fits);
    if (buf && cap && !fits) {
        if (secure) { V(GM(buf), uint16_t) = 0; r = -1; }
        else if (!(opts & 2)) r = -1;
    } else if (buf && !cap && !(opts & 2)) r = -1;
    RET_CDECL(c, (uint32_t)r);
}
IMP(CRT_STDIO, __stdio_common_vswprintf) { common_vswprintf(c, 0); }
IMP(CRT_STDIO, __stdio_common_vswprintf_s) { common_vswprintf(c, 1); }

// ---------- msvcrt.dll: old-style printf/scanf with the varargs right on the stack ----------
// _vsnprintf(buf, count, fmt, va): fits -> NUL-terminated, length; exactly count -> no NUL, count; longer -> -1, truncated.
static int32_t ms_vsnprintf(uint32_t buf, uint32_t cap, uint32_t fmt, uint32_t va) {
    char *s;
    size_t len = guest_vformat(&s, GSTR(fmt), va);
    memcpy(GSTR(buf), s, len < cap ? len + 1 : cap);
    free(s);
    return len <= cap ? (int32_t)len : -1;
}
IMP("msvcrt.dll", _vsnprintf) { RET_CDECL(c, (uint32_t)ms_vsnprintf(ARG(c, 0), ARG(c, 1), ARG(c, 2), ARG(c, 3))); }
IMP("msvcrt.dll", _snprintf) { RET_CDECL(c, (uint32_t)ms_vsnprintf(ARG(c, 0), ARG(c, 1), ARG(c, 2), REG(c, R_ESP) + 16)); }
IMP("msvcrt.dll", _vsnwprintf) {   // same rules in UTF-16 units; legacy wide specifiers (%s = wide string)
    uint32_t buf = ARG(c, 0), cap = ARG(c, 1), tmp = gmalloc(2 * cap + 2);
    int fits;
    int32_t units = wide_format(tmp, cap + 1, ARG(c, 2), ARG(c, 3), 4, &fits);
    memcpy(GM(buf), GM(tmp), 2 * ((uint32_t)units < cap ? (uint32_t)units + 1 : cap));
    gfree(tmp);
    RET_CDECL(c, (uint32_t)((uint32_t)units <= cap ? units : -1));
}
IMP("msvcrt.dll", sscanf) {   // (str, fmt, ...)
    const char *s = GSTR(ARG(c, 0));
    RET_CDECL(c, (uint32_t)guest_vscan(s, strlen(s), GSTR(ARG(c, 1)), REG(c, R_ESP) + 12, 0));
}
