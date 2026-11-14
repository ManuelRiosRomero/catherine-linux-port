// CRT stdio + low-level io. A guest FILE* is a 64-byte guest object; offset 16 (UCRT's _file) holds our index
// into `files`. Its _ptr/_base/_cnt (offsets 0/4/8) stay 0, so MSVCP's filebuf, which peeks at them through
// _get_stream_buffer_pointers, always sees an empty buffer and falls back to fgetc/fread/fwrite (which we serve).
// ponytail: text mode ("r"/"w" without "b") is treated as binary (no CRLF translation); add if a text file breaks.
#define _GNU_SOURCE
#include "host.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CRT_STDIO "api-ms-win-crt-stdio-l1-1-0.dll"
#define CRT_FS "api-ms-win-crt-filesystem-l1-1-0.dll"
#define MAX_FILES 1024
static FILE *files[MAX_FILES];
static uint32_t gfiles[MAX_FILES];   // guest FILE object per slot
// Saves are written safely: the game rewrites SaveGame.bin with fopen("wb") (truncate, then write), so a crash mid-save
// would leave a broken file for Steam Cloud to upload. Every "w" open outside the game folder goes to <name>.tmp
// instead, and fclose moves it over <name> once it is complete (rename is atomic).
static char *final_name[MAX_FILES];
static __thread char *opened_tmp;    // open_mapped -> file_new, same thread

static uint32_t file_new(FILE *f) {
    for (uint32_t i = 3; i < MAX_FILES; i++)
        if (!files[i]) {
            files[i] = f;
            final_name[i] = opened_tmp, opened_tmp = NULL;
            if (!gfiles[i]) gfiles[i] = gmalloc(64);
            memset(GM(gfiles[i]), 0, 64);
            U32(gfiles[i] + 16) = i;
            return gfiles[i];
        }
    die("too many open files");
}
static FILE *fh(uint32_t g) {
    if (!g) return NULL;
    uint32_t i = U32(g + 16);
    return i < MAX_FILES ? files[i] : NULL;
}

static FILE *open_mapped(const char *win, const char *mode) {
    char hp[1024], m[8];
    size_t k = 0;
    host_path(win, hp, sizeof hp);
    for (const char *p = mode; *p && k < 6; p++) if (strchr("rwa+", *p)) m[k++] = *p;   // drop b/t/ccs=...
    m[k] = 0;
    FILE *f;
    if (m[0] == 'w' && hp[0] == '/') {   // absolute host path = outside the game folder (saves, settings)
        char tmp[1040];
        snprintf(tmp, sizeof tmp, "%s.tmp", hp);
        if ((f = fopen(tmp, m))) opened_tmp = strdup(hp);
    } else f = fopen(hp, m);
    if (getenv("CATH_TRACE_FILES")) fprintf(stderr, "[file] fopen %s (%s) -> %s%s\n", win, mode, hp, f ? "" : " FAILED");
    return f;
}
IMP(CRT_STDIO, fopen) {
    FILE *f = open_mapped(GSTR(ARG(c, 0)), GSTR(ARG(c, 1)));
    RET_CDECL(c, f ? file_new(f) : 0);
}
IMP(CRT_STDIO, fopen_s) {   // fopen_s(FILE** out, name, mode) -> errno
    FILE *f = open_mapped(GSTR(ARG(c, 1)), GSTR(ARG(c, 2)));
    U32(ARG(c, 0)) = f ? file_new(f) : 0;
    RET_CDECL(c, f ? 0 : (errno ? (uint32_t)errno : 2));
}
IMP(CRT_STDIO, __acrt_iob_func) {   // 0 stdin, 1 stdout, 2 stderr
    uint32_t i = ARG(c, 0);
    if (i > 2) { RET_CDECL(c, 0); return; }
    if (!gfiles[i]) {
        files[i] = i == 0 ? stdin : i == 1 ? stdout : stderr;
        gfiles[i] = gmalloc(64);
        memset(GM(gfiles[i]), 0, 64);
        U32(gfiles[i] + 16) = i;
    }
    RET_CDECL(c, gfiles[i]);
}
// msvcrt.dll's _iob: three 32-byte FILEs in a row; msvcrt also keeps _file at offset 16, so fh() works on them.
uint32_t stdio_iob(void) {
    uint32_t p = gmalloc(96);
    memset(GM(p), 0, 96);
    for (uint32_t i = 0; i < 3; i++) {
        files[i] = i == 0 ? stdin : i == 1 ? stdout : stderr;
        U32(p + 32 * i + 16) = i;
    }
    return p;
}
IMP("msvcrt.dll", _fileno) { RET_CDECL(c, ARG(c, 0) ? U32(ARG(c, 0) + 16) : 0xFFFFFFFF); }
IMP("msvcrt.dll", _isatty) { RET_CDECL(c, (uint32_t)isatty((int)ARG(c, 0))); }
IMP(CRT_STDIO, fclose) {
    FILE *f = fh(ARG(c, 0));
    uint32_t i = ARG(c, 0) ? U32(ARG(c, 0) + 16) : 0;
    int r = 0;
    if (f && i > 2) {
        char *fin = final_name[i], tmp[1040];
        if (fin) fflush(f), fsync(fileno(f));
        r = fclose(f);
        if (fin) {
            snprintf(tmp, sizeof tmp, "%s.tmp", fin);
            if (r == 0 && rename(tmp, fin)) r = -1;
            if (getenv("CATH_TRACE_FILES")) fprintf(stderr, "[file] fclose: %s -> %s (%s)\n", tmp, fin, r ? "FAILED" : "ok");
            free(fin);
            final_name[i] = NULL;
        }
    }
    if (i > 2 && i < MAX_FILES) files[i] = NULL;
    RET_CDECL(c, (uint32_t)r);
}
IMP(CRT_STDIO, fread) {   // fread(buf, size, count, f)
    FILE *f = fh(ARG(c, 3));
    RET_CDECL(c, f && ARG(c, 1) ? (uint32_t)fread(GM(ARG(c, 0)), ARG(c, 1), ARG(c, 2), f) : 0);
}
IMP(CRT_STDIO, fwrite) {
    FILE *f = fh(ARG(c, 3));
    RET_CDECL(c, f && ARG(c, 1) ? (uint32_t)fwrite(GM(ARG(c, 0)), ARG(c, 1), ARG(c, 2), f) : 0);
}
size_t guest_vformat(char **out, const char *f, uint32_t va);   // printf.c
// __stdio_common_vfprintf(options (8 bytes), FILE*, format, locale, va)
IMP(CRT_STDIO, __stdio_common_vfprintf) {
    FILE *f = fh(ARG(c, 2));
    char *s;
    size_t len = guest_vformat(&s, GSTR(ARG(c, 3)), ARG(c, 5));
    int32_t r = f && fwrite(s, 1, len, f) == len ? (int32_t)len : -1;
    free(s);
    RET_CDECL(c, (uint32_t)r);
}
IMP(CRT_STDIO, fseek) { FILE *f = fh(ARG(c, 0)); RET_CDECL(c, f ? (uint32_t)fseek(f, (int32_t)ARG(c, 1), (int)ARG(c, 2)) : 0xFFFFFFFF); }
IMP(CRT_STDIO, _fseeki64) {   // (f, int64 offset = 2 slots, origin)
    FILE *f = fh(ARG(c, 0));
    RET_CDECL(c, f ? (uint32_t)fseeko(f, V(GM(REG(c, R_ESP) + 8), int64_t), (int)ARG(c, 3)) : 0xFFFFFFFF);
}
IMP(CRT_STDIO, ftell) { FILE *f = fh(ARG(c, 0)); RET_CDECL(c, f ? (uint32_t)ftell(f) : 0xFFFFFFFF); }
IMP(CRT_STDIO, feof) { FILE *f = fh(ARG(c, 0)); RET_CDECL(c, f ? (uint32_t)feof(f) : 0); }
IMP(CRT_STDIO, ferror) { FILE *f = fh(ARG(c, 0)); RET_CDECL(c, f ? (uint32_t)ferror(f) : 0); }
IMP(CRT_STDIO, fflush) { FILE *f = fh(ARG(c, 0)); RET_CDECL(c, (uint32_t)fflush(ARG(c, 0) ? f : NULL)); }
IMP(CRT_STDIO, fgetc) { FILE *f = fh(ARG(c, 0)); RET_CDECL(c, f ? (uint32_t)fgetc(f) : 0xFFFFFFFF); }
IMP(CRT_STDIO, fputc) { FILE *f = fh(ARG(c, 1)); RET_CDECL(c, f ? (uint32_t)fputc((int)ARG(c, 0), f) : 0xFFFFFFFF); }
IMP(CRT_STDIO, ungetc) { FILE *f = fh(ARG(c, 1)); RET_CDECL(c, f ? (uint32_t)ungetc((int)ARG(c, 0), f) : 0xFFFFFFFF); }
IMP(CRT_STDIO, fputs) { FILE *f = fh(ARG(c, 1)); RET_CDECL(c, f ? (uint32_t)fputs(GSTR(ARG(c, 0)), f) : 0xFFFFFFFF); }
IMP(CRT_STDIO, fgets) {
    FILE *f = fh(ARG(c, 2));
    RET_CDECL(c, f && fgets(GSTR(ARG(c, 0)), (int)ARG(c, 1), f) ? ARG(c, 0) : 0);
}
IMP(CRT_STDIO, setvbuf) { RET_CDECL(c, 0); }   // buffering stays host-side
IMP(CRT_STDIO, fgetpos) {   // fpos_t = int64
    FILE *f = fh(ARG(c, 0));
    if (!f) { RET_CDECL(c, 0xFFFFFFFF); return; }
    V(GM(ARG(c, 1)), int64_t) = ftello(f);
    RET_CDECL(c, 0);
}
IMP(CRT_STDIO, fsetpos) {
    FILE *f = fh(ARG(c, 0));
    RET_CDECL(c, f ? (uint32_t)fseeko(f, V(GM(ARG(c, 1)), int64_t), SEEK_SET) : 0xFFFFFFFF);
}
IMP(CRT_STDIO, _get_stream_buffer_pointers) {   // (f, char*** base, char*** ptr, int** count): our always-empty buffer
    uint32_t g = ARG(c, 0);
    if (ARG(c, 1)) U32(ARG(c, 1)) = g + 4;
    if (ARG(c, 2)) U32(ARG(c, 2)) = g + 0;
    if (ARG(c, 3)) U32(ARG(c, 3)) = g + 8;
    RET_CDECL(c, 0);
}
IMP(CRT_FS, _lock_file) { RET_CDECL(c, 0); }
IMP(CRT_FS, _unlock_file) { RET_CDECL(c, 0); }

// ---------- low-level io: guest fd = host fd ----------
// Host fds are shared with Mesa/DXVK/SDL; on Windows the CRT's fds are private. So only fds the game opened with
// _open are usable: a stray _close (double close, stale fd) would otherwise close a driver's file.
static uint8_t crt_fd[65536];
static int own(int fd, const char *fn) {
    if (fd >= 0 && fd < 65536 && (crt_fd[fd] || (fd <= 2 && *fn != 'c'))) return 1;
    static int told;
    if (!told++) fprintf(stderr, "[crt] _%s(%d): fd not opened by _open, refused (EBADF)\n", fn, fd);
    return 0;
}

IMP(CRT_STDIO, _open) {   // _open(path, oflag, pmode)
    uint32_t o = ARG(c, 1);
    int fl = (o & 3) == 1 ? O_WRONLY : (o & 3) == 2 ? O_RDWR : O_RDONLY;
    if (o & 0x8) fl |= O_APPEND;
    if (o & 0x100) fl |= O_CREAT;
    if (o & 0x200) fl |= O_TRUNC;
    if (o & 0x400) fl |= O_EXCL;
    char hp[1024];
    host_path(GSTR(ARG(c, 0)), hp, sizeof hp);
    int fd = open(hp, fl | O_CLOEXEC, 0644);
    if (fd >= 0 && fd < 65536) crt_fd[fd] = 1;
    RET_CDECL(c, (uint32_t)fd);
}
IMP(CRT_STDIO, _close) {
    int fd = (int)ARG(c, 0);
    if (!own(fd, "close")) { RET_CDECL(c, 0xFFFFFFFF); return; }
    crt_fd[fd] = 0;
    RET_CDECL(c, (uint32_t)close(fd));
}
IMP(CRT_STDIO, _read) { int fd = (int)ARG(c, 0); RET_CDECL(c, own(fd, "read") ? (uint32_t)read(fd, GM(ARG(c, 1)), ARG(c, 2)) : 0xFFFFFFFF); }
IMP(CRT_STDIO, _write) { int fd = (int)ARG(c, 0); RET_CDECL(c, own(fd, "write") ? (uint32_t)write(fd, GM(ARG(c, 1)), ARG(c, 2)) : 0xFFFFFFFF); }
IMP(CRT_STDIO, _lseeki64) {   // (fd, int64 offset, origin) -> int64
    int fd = (int)ARG(c, 0);
    int64_t r = own(fd, "lseeki64") ? lseek(fd, V(GM(REG(c, R_ESP) + 8), int64_t), (int)ARG(c, 3)) : -1;
    REG(c, R_EDX) = (uint32_t)((uint64_t)r >> 32);
    RET_CDECL(c, (uint32_t)r);
}
IMP(CRT_FS, _fstat64) {
    struct stat s;
    uint32_t g = ARG(c, 1);
    if (fstat((int)ARG(c, 0), &s)) { RET_CDECL(c, 0xFFFFFFFF); return; }
    memset(GM(g), 0, 56);
    V(GM(g + 6), uint16_t) = (S_ISDIR(s.st_mode) ? 0x4000 : 0x8000) | 0x1B6;
    V(GM(g + 8), uint16_t) = 1;
    V(GM(g + 24), int64_t) = s.st_size;
    V(GM(g + 32), int64_t) = s.st_atime;
    V(GM(g + 40), int64_t) = s.st_mtime;
    V(GM(g + 48), int64_t) = s.st_ctime;
    RET_CDECL(c, 0);
}

// For MSVCP140's _Fiopen (msvcp.c): open a Windows path with an fopen mode string, return a guest FILE* or 0.
uint32_t guest_fopen(const char *win, const char *mode, int at_end) {
    FILE *f = open_mapped(win, mode);
    if (f && at_end && fseek(f, 0, SEEK_END)) { fclose(f); return 0; }   // ios_base::ate
    return f ? file_new(f) : 0;
}
