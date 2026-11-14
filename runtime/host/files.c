// Win32 file API (CreateFile/ReadFile/...), directory search, .ini profile strings, shell folders, GUIDs.
// File handles are host fds wrapped in kernel-object handles from sync.c.
#define _GNU_SOURCE
#include "host.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#define K32 "KERNEL32.dll"
#define LAST_ERROR(c) U32(REG(c, R_FS_OFFSET) + 0x34)
#define INVALID 0xFFFFFFFFu

static void set_err(CPU *c) {   // errno -> Win32 error code (the ones games check)
    LAST_ERROR(c) = errno == ENOENT ? 2 : errno == EACCES ? 5 : errno == EEXIST ? 183 : errno == ENOTDIR ? 3 : 31;
}
static void wide_arg(uint32_t w, char *out, size_t cap) {
    size_t i = 0;
    for (; w && i + 1 < cap && V(GM(w + 2 * i), uint16_t); i++) out[i] = (char)V(GM(w + 2 * i), uint16_t);
    out[i] = 0;
}

// ---------- files ----------

static void create_file(CPU *c, const char *win) {   // (name, access, share, sa, disposition, flags, template)
    char hp[1024];
    host_path(win, hp, sizeof hp);
    uint32_t acc = ARG(c, 1), disp = ARG(c, 4);
    int fl = (acc & 0x40000000) ? ((acc & 0x80000000) ? O_RDWR : O_WRONLY) : O_RDONLY;   // GENERIC_WRITE / READ
    switch (disp) {
    case 1: fl |= O_CREAT | O_EXCL; break;    // CREATE_NEW
    case 2: fl |= O_CREAT | O_TRUNC; break;   // CREATE_ALWAYS
    case 4: fl |= O_CREAT; break;             // OPEN_ALWAYS
    case 5: fl |= O_TRUNC; break;             // TRUNCATE_EXISTING
    }
    int fd = open(hp, fl, 0644);
    if (getenv("CATH_TRACE_FILES"))
        fprintf(stderr, "[file] CreateFile %s access %08x disp %u -> %s fd %d%s\n", win, acc, disp, hp, fd, fd < 0 ? " FAILED" : "");
    if (fd < 0) { set_err(c); ret_std(c, INVALID, 7); return; }
    LAST_ERROR(c) = 0;
    ret_std(c, handle_for_fd(fd), 7);
}
IMP(K32, CreateFileA) { create_file(c, GSTR(ARG(c, 0))); }
IMP(K32, CreateFileW) { char n[1024]; wide_arg(ARG(c, 0), n, sizeof n); create_file(c, n); }
IMP(K32, GetFileSizeEx) {   // (h, LARGE_INTEGER*)
    struct stat s;
    int fd = fd_of(ARG(c, 0));
    if (fd < 0 || fstat(fd, &s)) { ret_std(c, 0, 2); return; }
    V(GM(ARG(c, 1)), int64_t) = s.st_size;
    ret_std(c, 1, 2);
}
// File mappings, read-only use (d3dx9 loads files this way): the mapping handle owns a dup'd fd, and a view is
// a guest heap copy of the file. ponytail: writes to a view never reach the file; add msync-like flush if needed.
IMP(K32, CreateFileMappingA) {   // (file, sa, protect, size hi, size lo, name)
    int fd = fd_of(ARG(c, 0));
    ret_std(c, fd < 0 ? 0 : handle_for_fd(dup(fd)), 6);
}
IMP(K32, MapViewOfFile) {   // (mapping, access, offset hi, offset lo, bytes; 0 = to end)
    struct stat s;
    int fd = fd_of(ARG(c, 0));
    uint64_t off = (uint64_t)ARG(c, 2) << 32 | ARG(c, 3), n = ARG(c, 4);
    if (fd < 0 || fstat(fd, &s) || off > (uint64_t)s.st_size) { ret_std(c, 0, 5); return; }
    if (!n) n = s.st_size - off;
    uint32_t p = gmalloc((uint32_t)n);
    if (p && pread(fd, GM(p), n, off) != (ssize_t)n) { gfree(p); p = 0; }
    ret_std(c, p, 5);
}
IMP(K32, UnmapViewOfFile) { gfree(ARG(c, 0)); ret_std(c, 1, 1); }
IMP(K32, ReadFile) {   // (h, buf, n, read*, overlapped)
    int fd = fd_of(ARG(c, 0));
    ssize_t r = fd < 0 ? -1 : read(fd, GM(ARG(c, 1)), ARG(c, 2));
    if (ARG(c, 3)) U32(ARG(c, 3)) = r > 0 ? (uint32_t)r : 0;
    if (r < 0) set_err(c);
    ret_std(c, r >= 0, 5);
}
IMP(K32, WriteFile) {
    int fd = fd_of(ARG(c, 0));
    ssize_t r = fd < 0 ? -1 : write(fd, GM(ARG(c, 1)), ARG(c, 2));
    if (getenv("CATH_TRACE_FILES") && fd > 2) fprintf(stderr, "[file] WriteFile fd %d %u bytes -> %zd\n", fd, ARG(c, 2), r);
    if (ARG(c, 3)) U32(ARG(c, 3)) = r > 0 ? (uint32_t)r : 0;
    if (r < 0) set_err(c);
    ret_std(c, r >= 0, 5);
}
IMP(K32, SetFilePointer) {   // (h, lo, hi*, method) -> new low dword
    int fd = fd_of(ARG(c, 0));
    int64_t off = (int32_t)ARG(c, 1);
    if (ARG(c, 2)) off = (int64_t)(((uint64_t)U32(ARG(c, 2)) << 32) | ARG(c, 1));
    off_t r = fd < 0 ? -1 : lseek(fd, off, (int)ARG(c, 3));
    if (r < 0) { set_err(c); ret_std(c, INVALID, 4); return; }
    if (ARG(c, 2)) U32(ARG(c, 2)) = (uint32_t)((uint64_t)r >> 32);
    ret_std(c, (uint32_t)r, 4);
}
IMP(K32, GetFileSize) {
    struct stat s;
    int fd = fd_of(ARG(c, 0));
    if (fd < 0 || fstat(fd, &s)) { ret_std(c, INVALID, 2); return; }
    if (ARG(c, 1)) U32(ARG(c, 1)) = (uint32_t)((uint64_t)s.st_size >> 32);
    ret_std(c, (uint32_t)s.st_size, 2);
}
IMP(K32, SetEndOfFile) { int fd = fd_of(ARG(c, 0)); ret_std(c, fd >= 0 && ftruncate(fd, lseek(fd, 0, SEEK_CUR)) == 0, 1); }
IMP(K32, FlushFileBuffers) { int fd = fd_of(ARG(c, 0)); ret_std(c, fd >= 0 && fsync(fd) == 0, 1); }
IMP(K32, GetFileType) { ret_std(c, fd_of(ARG(c, 0)) >= 0 ? 1 : 2, 1); }   // FILE_TYPE_DISK; std handles: CHAR
IMP(K32, GetStdHandle) {   // STD_INPUT/OUTPUT/ERROR = -10/-11/-12
    int32_t n = (int32_t)ARG(c, 0);
    ret_std(c, n == -10 ? handle_for_fd(0) : n == -11 ? handle_for_fd(1) : handle_for_fd(2), 1);
}
IMP(K32, PeekNamedPipe) { ret_std(c, 0, 6); }

static uint32_t attrs_of(const char *win) {
    char hp[1024];
    struct stat s;
    host_path(win, hp, sizeof hp);
    if (stat(hp, &s)) return INVALID;
    return S_ISDIR(s.st_mode) ? 0x10 : 0x80;   // FILE_ATTRIBUTE_DIRECTORY / NORMAL
}
IMP(K32, GetFileAttributesA) {
    uint32_t a = attrs_of(GSTR(ARG(c, 0)));
    if (a == INVALID) LAST_ERROR(c) = 2;
    ret_std(c, a, 1);
}
IMP(K32, GetFileAttributesW) {
    char n[1024];
    wide_arg(ARG(c, 0), n, sizeof n);
    uint32_t a = attrs_of(n);
    if (a == INVALID) LAST_ERROR(c) = 2;
    ret_std(c, a, 1);
}
IMP(K32, CreateDirectoryA) {
    char hp[1024];
    host_path(GSTR(ARG(c, 0)), hp, sizeof hp);
    int ok = mkdir(hp, 0755) == 0;
    if (!ok) set_err(c);
    ret_std(c, ok, 2);
}
IMP(K32, DeleteFileA) {
    char hp[1024];
    host_path(GSTR(ARG(c, 0)), hp, sizeof hp);
    if (getenv("CATH_TRACE_FILES")) fprintf(stderr, "[file] DeleteFile %s\n", hp);
    ret_std(c, unlink(hp) == 0, 1);
}
IMP(K32, MoveFileA) {
    char a[1024], b[1024];
    host_path(GSTR(ARG(c, 0)), a, sizeof a);
    host_path(GSTR(ARG(c, 1)), b, sizeof b);
    if (getenv("CATH_TRACE_FILES")) fprintf(stderr, "[file] MoveFile %s -> %s\n", a, b);
    ret_std(c, rename(a, b) == 0, 2);
}
// (name, n, buf, filepart*): relative names resolve against C:\Game; '/' becomes '\' as on Windows, and *filepart
// points at the last component in buf (d3dx9 splits the directory off with it to resolve #include files).
static void full_path(CPU *c, const char *n, int wide) {
    char full[1100];
    if (n[0] && n[1] == ':') snprintf(full, sizeof full, "%s", n); else snprintf(full, sizeof full, "C:\\Game\\%s", n);
    uint32_t len = strlen(full), last = 0, buf = ARG(c, 2), w = wide ? 2 : 1;
    for (uint32_t i = 0; i < len; i++) if (full[i] == '/' || full[i] == '\\') full[i] = '\\', last = i + 1;
    if (len + 1 > ARG(c, 1)) { ret_std(c, len + 1, 4); return; }
    for (uint32_t i = 0; i <= len; i++)
        if (wide) V(GM(buf + 2 * i), uint16_t) = (uint8_t)full[i]; else GSTR(buf)[i] = full[i];
    if (ARG(c, 3)) U32(ARG(c, 3)) = last < len ? buf + w * last : 0;   // NULL when the name ends in a separator
    ret_std(c, len, 4);
}
IMP(K32, GetFullPathNameW) { char n[1024]; wide_arg(ARG(c, 0), n, sizeof n); full_path(c, n, 1); }
IMP(K32, GetFullPathNameA) { full_path(c, GSTR(ARG(c, 0)), 0); }
IMP(K32, GetSystemDirectoryA) { const char *d = "C:\\Windows\\system32"; strncpy(GSTR(ARG(c, 0)), d, ARG(c, 1)); ret_std(c, strlen(d), 2); }
IMP(K32, GetSystemDirectoryW) {
    const char *d = "C:\\Windows\\system32";
    for (uint32_t i = 0; i <= strlen(d) && i < ARG(c, 1); i++) V(GM(ARG(c, 0) + 2 * i), uint16_t) = (uint8_t)d[i];
    ret_std(c, strlen(d), 2);
}

// ---------- FindFirstFile / FindNextFile (pattern = last path component, '*' and '?' wildcards) ----------

typedef struct { DIR *d; char dir[1024], pat[256]; } Find;
static Find finds[64];
static int find_next(uint32_t i, uint32_t data) {   // fills WIN32_FIND_DATAA (cFileName at +44, 260 bytes)
    Find *f = &finds[i];
    for (struct dirent *e; (e = readdir(f->d));) {
        if (fnmatch(f->pat, e->d_name, FNM_CASEFOLD)) continue;
        char p[1300];
        struct stat s;
        snprintf(p, sizeof p, "%s/%s", f->dir, e->d_name);
        if (stat(p, &s)) continue;
        memset(GM(data), 0, 320);
        U32(data) = S_ISDIR(s.st_mode) ? 0x10 : 0x80;
        U32(data + 28) = (uint32_t)((uint64_t)s.st_size >> 32);   // nFileSizeHigh
        U32(data + 32) = (uint32_t)s.st_size;                      // nFileSizeLow
        snprintf(GSTR(data + 44), 260, "%s", e->d_name);
        return 1;
    }
    return 0;
}
IMP(K32, FindFirstFileA) {
    char win[1024], hp[1024];
    snprintf(win, sizeof win, "%s", GSTR(ARG(c, 0)));
    char *slash = strrchr(win, '\\');
    if (!slash) slash = strrchr(win, '/');
    uint32_t i = 0;
    while (i < 64 && finds[i].d) i++;
    if (i == 64) { ret_std(c, INVALID, 2); return; }
    snprintf(finds[i].pat, sizeof finds[i].pat, "%s", slash ? slash + 1 : win);
    if (slash) { *slash = 0; host_path(win, hp, sizeof hp); } else snprintf(hp, sizeof hp, ".");
    snprintf(finds[i].dir, sizeof finds[i].dir, "%s", hp);
    if (!(finds[i].d = opendir(hp))) { LAST_ERROR(c) = 3; ret_std(c, INVALID, 2); return; }
    if (!find_next(i, ARG(c, 1))) { closedir(finds[i].d); finds[i].d = NULL; LAST_ERROR(c) = 2; ret_std(c, INVALID, 2); return; }
    ret_std(c, 0xC0F00000u + i, 2);   // find handles live apart from kernel objects (sync.c: 0x1000..0x41000)
}
IMP(K32, FindNextFileA) {
    uint32_t i = ARG(c, 0) - 0xC0F00000u;
    int ok = i < 64 && finds[i].d && find_next(i, ARG(c, 1));
    if (!ok) LAST_ERROR(c) = 18;   // ERROR_NO_MORE_FILES
    ret_std(c, ok, 2);
}
IMP(K32, FindClose) {
    uint32_t i = ARG(c, 0) - 0xC0F00000u;
    if (i < 64 && finds[i].d) { closedir(finds[i].d); finds[i].d = NULL; }
    ret_std(c, 1, 1);
}

// ---------- .ini profile strings (small parser; files are tiny) ----------

static int ini_get(const char *win, const char *sect, const char *key, char *out, size_t cap) {
    char hp[1024], line[1024], cur[256] = "";
    host_path(win, hp, sizeof hp);
    FILE *f = fopen(hp, "r");
    if (!f) return 0;
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        s[strcspn(s, "\r\n")] = 0;
        if (*s == '[') { char *e = strchr(s, ']'); if (e) *e = 0; snprintf(cur, sizeof cur, "%s", s + 1); continue; }
        char *eq = strchr(s, '=');
        if (!eq || strcasecmp(cur, sect)) continue;
        *eq = 0;
        for (char *e = eq - 1; e >= s && (*e == ' ' || *e == '\t'); e--) *e = 0;
        if (strcasecmp(s, key)) continue;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        snprintf(out, cap, "%s", v);
        found = 1;
    }
    fclose(f);
    return found;
}
static void ini_set(const char *win, const char *sect, const char *key, const char *val) {
    char hp[1024], line[1024], cur[256] = "";
    host_path(win, hp, sizeof hp);
    char *buf = NULL;
    size_t len = 0;
    FILE *o = open_memstream(&buf, &len), *f = fopen(hp, "r");
    int done = 0, in_sect = 0;
    while (f && fgets(line, sizeof line, f)) {
        char tmp[1024], *s = tmp;
        snprintf(tmp, sizeof tmp, "%s", line);
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') {
            if (in_sect && !done) { fprintf(o, "%s=%s\r\n", key, val); done = 1; }
            char *e = strchr(s, ']'); if (e) *e = 0;
            snprintf(cur, sizeof cur, "%s", s + 1);
            in_sect = !strcasecmp(cur, sect);
        } else if (in_sect && !done) {
            char *eq = strchr(s, '=');
            if (eq) { *eq = 0; for (char *e = eq - 1; e >= s && (*e == ' ' || *e == '\t'); e--) *e = 0;
                      if (!strcasecmp(s, key)) { fprintf(o, "%s=%s\r\n", key, val); done = 1; continue; } }
        }
        fputs(line, o);
    }
    if (f) fclose(f);
    if (!done) { if (!in_sect) fprintf(o, "[%s]\r\n", sect); fprintf(o, "%s=%s\r\n", key, val); }
    fclose(o);
    if ((f = fopen(hp, "w"))) { fwrite(buf, 1, len, f); fclose(f); }
    free(buf);
}
IMP(K32, GetPrivateProfileStringA) {   // (sect, key, default, buf, size, file)
    char v[1024];
    uint32_t buf = ARG(c, 3), n = ARG(c, 4);
    if (!ini_get(GSTR(ARG(c, 5)), GSTR(ARG(c, 0)), GSTR(ARG(c, 1)), v, sizeof v)) snprintf(v, sizeof v, "%s", ARG(c, 2) ? GSTR(ARG(c, 2)) : "");
    if (n) { strncpy(GSTR(buf), v, n - 1); GSTR(buf)[n - 1] = 0; }
    ret_std(c, n ? strlen(GSTR(buf)) : 0, 6);
}
IMP(K32, GetPrivateProfileIntA) {   // (sect, key, default, file)
    char v[64];
    ret_std(c, ini_get(GSTR(ARG(c, 3)), GSTR(ARG(c, 0)), GSTR(ARG(c, 1)), v, sizeof v) ? (uint32_t)strtol(v, NULL, 10) : ARG(c, 2), 4);
}
IMP(K32, WritePrivateProfileStringA) {   // (sect, key, value, file)
    if (ARG(c, 1) && ARG(c, 2)) ini_set(GSTR(ARG(c, 3)), GSTR(ARG(c, 0)), GSTR(ARG(c, 1)), GSTR(ARG(c, 2)));
    ret_std(c, 1, 4);
}
IMP(K32, GetPrivateProfileStringW) {
    char s[256], k[256], d[1024], f[1024], v[1024];
    wide_arg(ARG(c, 0), s, sizeof s); wide_arg(ARG(c, 1), k, sizeof k); wide_arg(ARG(c, 2), d, sizeof d); wide_arg(ARG(c, 5), f, sizeof f);
    if (!ini_get(f, s, k, v, sizeof v)) snprintf(v, sizeof v, "%s", d);
    uint32_t n = ARG(c, 4), len = strlen(v);
    if (len >= n) len = n ? n - 1 : 0;
    for (uint32_t i = 0; i < len; i++) V(GM(ARG(c, 3) + 2 * i), uint16_t) = (uint8_t)v[i];
    if (n) V(GM(ARG(c, 3) + 2 * len), uint16_t) = 0;
    ret_std(c, len, 6);
}
IMP(K32, WritePrivateProfileStringW) {
    char s[256], k[256], v[1024], f[1024];
    wide_arg(ARG(c, 0), s, sizeof s); wide_arg(ARG(c, 1), k, sizeof k); wide_arg(ARG(c, 2), v, sizeof v); wide_arg(ARG(c, 3), f, sizeof f);
    if (ARG(c, 1) && ARG(c, 2)) ini_set(f, s, k, v);
    ret_std(c, 1, 4);
}

// ---------- misc ----------

IMP("ole32.dll", CoCreateGuid) { getrandom(GM(ARG(c, 0)), 16, 0); ret_std(c, 0, 1); }
// SHGetFolderPathA(hwnd, csidl, token, flags, path): every user folder lives under a fake profile, which path.c maps
// to ~/.local/share/catherine-port/c/Users/deck/...
IMP("SHELL32.dll", SHGetFolderPathA) {
    uint32_t id = ARG(c, 1) & 0xFF;
    const char *p = id == 0x1A ? "C:\\Users\\deck\\AppData\\Roaming" : id == 0x1C ? "C:\\Users\\deck\\AppData\\Local"
                  : id == 0x05 ? "C:\\Users\\deck\\Documents" : "C:\\Users\\deck";
    char hp[1024];
    host_path(p, hp, sizeof hp);
    char cmd[1100];
    snprintf(cmd, sizeof cmd, "%s", hp);
    for (char *q = cmd + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(cmd, 0755); *q = '/'; }
    mkdir(cmd, 0755);
    strcpy(GSTR(ARG(c, 4)), p);
    ret_std(c, 0, 5);
}
IMP(K32, ExpandEnvironmentStringsA) {   // no %VARS% in our fake environment: copy through
    uint32_t n = strlen(GSTR(ARG(c, 0))) + 1;
    if (ARG(c, 2) >= n) memcpy(GSTR(ARG(c, 1)), GSTR(ARG(c, 0)), n);
    ret_std(c, n, 3);
}
IMP(K32, ExpandEnvironmentStringsW) {
    uint32_t s = ARG(c, 0), n = 0;
    while (V(GM(s + 2 * n), uint16_t)) n++;
    n++;
    if (ARG(c, 2) >= n) memcpy(GM(ARG(c, 1)), GM(s), 2 * n);
    ret_std(c, n, 3);
}

// FormatMessageA(flags, source, id, lang, buffer, size, args): system messages become "Win32 error N".
// FORMAT_MESSAGE_ALLOCATE_BUFFER (0x100): *buffer receives a LocalAlloc'd string.
IMP(K32, FormatMessageA) {
    char text[64];
    snprintf(text, sizeof text, "Win32 error %u\r\n", ARG(c, 2));
    uint32_t len = strlen(text), flags = ARG(c, 0), buf = ARG(c, 4);
    if (flags & 0x100) {
        uint32_t p = gmalloc(len + 1);
        memcpy(GSTR(p), text, len + 1);
        U32(buf) = p;
    } else {
        if (ARG(c, 5) <= len) { ret_std(c, 0, 7); return; }
        memcpy(GSTR(buf), text, len + 1);
    }
    ret_std(c, len, 7);
}
