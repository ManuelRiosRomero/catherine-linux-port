// Win32 / CRT import shims. Each handler reads its arguments from the guest stack (ARG) and returns with
// ret_std (stdcall: pops its args) or RET_CDECL (caller pops). Grown one missing import at a time.
#define _GNU_SOURCE
#include "host.h"
#include <execinfo.h>
#include <signal.h>
#include <sys/mman.h>
#include <ctype.h>
#include <errno.h>
#include <iconv.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

extern const Import __start_cath_imports[], __stop_cath_imports[];

Fn import_handler(const char *dll, const char *name) {   // dll NULL = any DLL
    for (const Import *i = __start_cath_imports; i < __stop_cath_imports; i++)
        if ((!dll || !strcasecmp(i->dll, dll)) && !strcmp(i->name, name)) return i->fn;
    // msvcrt.dll (the old CRT Microsoft's DLLs use) shares most functions with the UCRT shims, by name
    if (dll && !strcasecmp(dll, "msvcrt.dll")) return import_handler(NULL, name);
    return NULL;
}

uint32_t guest_call(CPU *c, uint32_t fn, int stdcall, int n, ...) {
    uint32_t args[16];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    for (int i = n - 1; i >= 0; i--) { REG(c, R_ESP) -= 4; U32(REG(c, R_ESP)) = args[i]; }
    REG(c, R_ESP) -= 4;
    U32(REG(c, R_ESP)) = 0;               // return address: never used, the C call stack returns for us
    rt_call(c, fn);
    if (!stdcall) REG(c, R_ESP) += 4 * n;
    return REG(c, R_EAX);
}

void ret_st0(CPU *c, long double v) {
    c->r[X87_TOP] = (c->r[X87_TOP] + 7) & 7;   // x87 push (rt.h STP)
    st10(STP(c, 0), v);
}

#define TEB() REG(c, R_FS_OFFSET)
#define LAST_ERROR(c) U32(REG(c, R_FS_OFFSET) + 0x34)
#define FAKE_EXE "C:\\Game\\Catherine.exe"   // Windows-style path the game sees; C:\Game\ maps to the game folder

static uint32_t gdup(const char *s) {          // copy a host string into guest memory (never freed: startup only)
    uint32_t p = gmalloc(strlen(s) + 1);
    strcpy(GSTR(p), s);
    return p;
}

// ---------- time ----------

static uint64_t mono_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec;
}

IMP("KERNEL32.dll", GetSystemTimeAsFileTime) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    uint64_t ft = ((uint64_t)t.tv_sec + 11644473600u) * 10000000u + t.tv_nsec / 100;   // 100 ns since 1601
    V(GM(ARG(c, 0)), uint64_t) = ft;
    ret_std(c, 0, 1);
}
IMP("KERNEL32.dll", GetTickCount) { ret_std(c, (uint32_t)(mono_ns() / 1000000), 0); }
IMP("KERNEL32.dll", GetTickCount64) {
    uint64_t ms = mono_ns() / 1000000;
    REG(c, R_EDX) = (uint32_t)(ms >> 32);
    ret_std(c, (uint32_t)ms, 0);
}
// QPC in 100 ns units at 10 MHz, like modern Windows: small numbers keep the game's 64-bit math far from overflow.
IMP("KERNEL32.dll", QueryPerformanceCounter) { V(GM(ARG(c, 0)), uint64_t) = mono_ns() / 100; ret_std(c, 1, 1); }
IMP("KERNEL32.dll", QueryPerformanceFrequency) { V(GM(ARG(c, 0)), uint64_t) = 10000000; ret_std(c, 1, 1); }
IMP("KERNEL32.dll", Sleep) {
    uint64_t t0 = prof_on ? prof_now() : 0;
    usleep(ARG(c, 0) * 1000u);
    if (t0) prof_wait(2, U32(REG(c, R_ESP)), t0);
    ret_std(c, 0, 1);
}
IMP("KERNEL32.dll", SleepEx) { usleep(ARG(c, 0) * 1000u); ret_std(c, 0, 2); }

// ---------- process / thread identity ----------

IMP("KERNEL32.dll", GetCurrentProcessId) { ret_std(c, (uint32_t)getpid(), 0); }
IMP("KERNEL32.dll", GetCurrentThreadId) { ret_std(c, U32(TEB() + 0x24), 0); }   // set per thread in sync.c / main.c
IMP("KERNEL32.dll", GetCurrentProcess) { ret_std(c, 0xFFFFFFFF, 0); }   // pseudo-handles, as on Windows
IMP("KERNEL32.dll", GetCurrentThread) { ret_std(c, 0xFFFFFFFE, 0); }
IMP("KERNEL32.dll", IsDebuggerPresent) { ret_std(c, 0, 0); }
IMP("KERNEL32.dll", IsProcessorFeaturePresent) {
    uint32_t f = ARG(c, 0);   // 10 = SSE2, 6/7 = SSE/3DNow-ish, 17 = NX, 23 = __fastfail; report a modern x86 CPU
    ret_std(c, f == 10 || f == 6 || f == 13 || f == 17 || f == 23, 1);
}
IMP("KERNEL32.dll", InitializeSListHead) { V(GM(ARG(c, 0)), uint64_t) = 0; ret_std(c, 0, 1); }
IMP("KERNEL32.dll", SetUnhandledExceptionFilter) { ret_std(c, 0, 1); }
IMP("KERNEL32.dll", UnhandledExceptionFilter) { ret_std(c, 0, 1); }
IMP("KERNEL32.dll", SetErrorMode) { ret_std(c, 0, 1); }
IMP("KERNEL32.dll", GetLastError) { ret_std(c, LAST_ERROR(c), 0); }
IMP("KERNEL32.dll", SetLastError) { LAST_ERROR(c) = ARG(c, 0); ret_std(c, 0, 1); }
IMP("KERNEL32.dll", TerminateProcess) { fflush(NULL); _exit((int)ARG(c, 1)); }
IMP("KERNEL32.dll", OutputDebugStringA) { fprintf(stderr, "[debug] %s", GSTR(ARG(c, 0))); ret_std(c, 0, 1); }
// ---------- modules: every "DLL" is a fake handle; GetProcAddress finds our shims by name ----------

#define G_MODULES 0x7F000000u           // fake HMODULE i = G_MODULES + 0x10000 * i (never dereferenced)
static char *mod_names[256];
static uint32_t n_mods;

static void wide_to_host(uint32_t w, char *out, size_t cap) {   // ASCII-only UTF-16 -> char (DLL/proc names)
    size_t i = 0;
    for (; i + 1 < cap && V(GM(w + 2 * i), uint16_t); i++) out[i] = (char)V(GM(w + 2 * i), uint16_t);
    out[i] = 0;
}
static const char *basename_of(const char *p) {
    const char *b = p;
    for (; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    return b;
}
// DLLs we can stand in for. Others "fail to load" so the game takes its fallback path (logged, to decide later).
static int dll_supported(const char *n) {
    return !strncasecmp(n, "kernel32", 8) || !strncasecmp(n, "api-ms-win-", 11) || !strncasecmp(n, "ntdll", 5) ||
           !strcasecmp(n, "d3d9.dll") || !strcasecmp(n, "xinput1_3.dll");
}
static uint32_t module_handle(const char *path, int load) {
    const char *n = basename_of(path);
    if (dll_base(n)) return dll_base(n);   // a translated DLL: its real image
    for (uint32_t i = 0; i < n_mods; i++) if (!strcasecmp(mod_names[i], n)) return G_MODULES + 0x10000 * (i + 1);
    if (!dll_supported(n)) { fprintf(stderr, "[dll] %s %s: refused\n", load ? "LoadLibrary" : "GetModuleHandle", n); return 0; }
    if (n_mods == 256) die("too many modules");
    mod_names[n_mods] = strdup(n);
    return G_MODULES + 0x10000 * ++n_mods;
}
uint32_t dyn_stub(const char *dll, const char *name);
static void get_module(CPU *c, int wide, int load, int nargs) {
    char buf[260];
    uint32_t p = ARG(c, 0);
    if (!p) { ret_std(c, U32(G_PEB + 8), nargs); return; }     // NULL = the exe itself
    if (wide) wide_to_host(p, buf, sizeof buf); else snprintf(buf, sizeof buf, "%s", GSTR(p));
    ret_std(c, module_handle(buf, load), nargs);
}
IMP("KERNEL32.dll", GetModuleHandleA) { get_module(c, 0, 0, 1); }
IMP("KERNEL32.dll", GetModuleHandleW) { get_module(c, 1, 0, 1); }
IMP("KERNEL32.dll", LoadLibraryA) { get_module(c, 0, 1, 1); }
IMP("KERNEL32.dll", LoadLibraryExW) { get_module(c, 1, 1, 3); }
IMP("KERNEL32.dll", FreeLibrary) { ret_std(c, 1, 1); }
IMP("KERNEL32.dll", DisableThreadLibraryCalls) { ret_std(c, 1, 1); }
IMP("KERNEL32.dll", GetProcAddress) {
    uint32_t h = ARG(c, 0), name = ARG(c, 1), i = (h - G_MODULES) / 0x10000;
    if (dll_is_loaded(h)) { ret_std(c, dll_export(h, name), 2); return; }
    const char *dll = h == U32(G_PEB + 8) ? "Catherine.exe" : (i >= 1 && i <= n_mods) ? mod_names[i - 1] : "?";
    if (name < 0x10000) { fprintf(stderr, "[dll] GetProcAddress %s!#%u: no ordinals\n", dll, name); ret_std(c, 0, 2); return; }
    uint32_t stub = dyn_stub(dll, GSTR(name));
    if (!stub) fprintf(stderr, "[dll] GetProcAddress %s!%s: not implemented\n", dll, GSTR(name));
    ret_std(c, stub, 2);
}

// ---------- TLS slots live in the TEB, so inline fs:[0xE10 + 4*i] reads also work ----------

static uint64_t tls_used;
IMP("KERNEL32.dll", TlsAlloc) {
    for (int i = 0; i < 64; i++)
        if (!(tls_used >> i & 1)) { tls_used |= 1ull << i; ret_std(c, i, 0); return; }
    ret_std(c, 0xFFFFFFFF, 0);
}
IMP("KERNEL32.dll", TlsFree) { tls_used &= ~(1ull << (ARG(c, 0) & 63)); ret_std(c, 1, 1); }
IMP("KERNEL32.dll", TlsSetValue) { U32(TEB() + 0xE10 + 4 * ARG(c, 0)) = ARG(c, 1); ret_std(c, 1, 2); }
IMP("KERNEL32.dll", TlsGetValue) { ret_std(c, U32(TEB() + 0xE10 + 4 * ARG(c, 0)), 1); }

// ---------- critical sections: host recursive mutex, its index kept in the guest struct's LockSemaphore ----------

static pthread_mutex_t *mutexes[1 << 16];
static uint32_t n_mutex, free_mutex[1 << 16], n_free;   // slots given back by DeleteCriticalSection
static pthread_mutex_t mutex_alloc_lock = PTHREAD_MUTEX_INITIALIZER;

// Debug: CATH_WATCH=<guest hex address> makes that page read-only once the critical section holding the address is
// initialized. Other writes to the page are single-stepped (unprotect, trap flag, re-protect); a write touching the
// watched word stops with a host backtrace (resolve with addr2line -f -e catherine).
static uint32_t watch_w = 1;
static void watch_hit(int sig, siginfo_t *si, void *ctx) {
    (void)sig;
    uint32_t a = (uint32_t)((uint8_t *)si->si_addr - M);
    if (a + 64 <= watch_w || a > watch_w + 3) {  // 64: widest vector store that could still cover the word
        mprotect(GM(watch_w & ~0xFFFu), 0x1000, PROT_READ | PROT_WRITE);
        ((ucontext_t *)ctx)->uc_mcontext.gregs[REG_EFL] |= 0x100;
        return;
    }
    void *bt[64];
    fprintf(stderr, "catherine: write to watched word, guest %08x\n", a);
    backtrace_symbols_fd(bt, backtrace(bt, 64), 2);
    _exit(3);
}
static void watch_step(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)si;
    mprotect(GM(watch_w & ~0xFFFu), 0x1000, PROT_READ);
    ((ucontext_t *)ctx)->uc_mcontext.gregs[REG_EFL] &= ~0x100;
}
static void watch_check(uint32_t cs) {
    if (watch_w == 1) watch_w = getenv("CATH_WATCH") ? (uint32_t)strtoul(getenv("CATH_WATCH"), NULL, 16) : 0;
    if (!watch_w || watch_w < cs || watch_w >= cs + 24) return;
    struct sigaction sa = {.sa_sigaction = watch_hit, .sa_flags = SA_SIGINFO};
    sigaction(SIGSEGV, &sa, NULL);
    sa.sa_sigaction = watch_step;
    sigaction(SIGTRAP, &sa, NULL);
    mprotect(GM(watch_w & ~0xFFFu), 0x1000, PROT_READ);
    fprintf(stderr, "[watch] page %08x is read-only now\n", watch_w & ~0xFFFu);
}

static void cs_init(CPU *c, int nargs) {
    uint32_t cs = ARG(c, 0);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_lock(&mutex_alloc_lock);
    // the game creates and deletes critical sections all the time (~65k slots used up after 2.5 h): reuse slots
    uint32_t i = n_free ? free_mutex[--n_free] : ++n_mutex;
    if (i >= 1 << 16) die("too many live critical sections");
    if (i == n_mutex && !(i & 0xFFF)) fprintf(stderr, "[win32] %u critical sections live\n", i);   // growth = a leak
    if (!mutexes[i]) mutexes[i] = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(mutexes[i], &a);
    pthread_mutex_unlock(&mutex_alloc_lock);
    memset(GM(cs), 0, 24);
    U32(cs + 4) = 0xFFFFFFFF;          // LockCount -1 = unlocked, in case game code peeks
    U32(cs + 16) = i;
    watch_check(cs);
    ret_std(c, 1, nargs);
}
IMP("KERNEL32.dll", InitializeCriticalSection) { cs_init(c, 1); }
IMP("KERNEL32.dll", InitializeCriticalSectionAndSpinCount) { cs_init(c, 2); }
IMP("KERNEL32.dll", InitializeCriticalSectionEx) { cs_init(c, 3); }
static pthread_mutex_t *cs_mutex_at(CPU *c, uint32_t cs) {
    uint32_t i = U32(cs + 16);
    if (!i || i > n_mutex)
        die("critical section %08x used before InitializeCriticalSection (called from %08x)", cs, U32(REG(c, R_ESP)));
    return mutexes[i];
}
#define cs_mutex(c) cs_mutex_at(c, ARG(c, 0))
IMP("KERNEL32.dll", EnterCriticalSection) {
    pthread_mutex_t *m = cs_mutex(c);
    uint32_t cs = ARG(c, 0);
    if (prof_on && pthread_mutex_trylock(m)) {   // contended: time the wait, by call site, by lock and by holder
        uint64_t t0 = prof_now();
        uint32_t holder = U32(cs + 12);
        pthread_mutex_lock(m);
        prof_wait(3, U32(REG(c, R_ESP)), t0);
        prof_wait(5, cs, t0);
        prof_wait(6, holder, t0);
    } else if (!prof_on) pthread_mutex_lock(m);
    if (prof_on) U32(cs + 12) = prof_tid();   // OwningThread, for the holder above
    ret_std(c, 0, 1);
}
IMP("KERNEL32.dll", LeaveCriticalSection) { pthread_mutex_unlock(cs_mutex(c)); ret_std(c, 0, 1); }
IMP("KERNEL32.dll", DeleteCriticalSection) {
    uint32_t cs = ARG(c, 0), i = U32(cs + 16);
    if (i && i <= n_mutex) {
        pthread_mutex_destroy(mutexes[i]);
        pthread_mutex_lock(&mutex_alloc_lock);
        free_mutex[n_free++] = i;
        pthread_mutex_unlock(&mutex_alloc_lock);
        U32(cs + 16) = 0;   // a use after delete now stops by name instead of locking a reused slot
    }
    ret_std(c, 0, 1);
}
void guest_cs(CPU *c, uint32_t cs, int lock) {   // Enter/LeaveCriticalSection from host code
    pthread_mutex_t *m = cs_mutex_at(c, cs);
    lock ? pthread_mutex_lock(m) : pthread_mutex_unlock(m);
    if (lock && prof_on) U32(cs + 12) = prof_tid();
}

// ---------- condition variables (CRT thread-safe statics, game threads): host pthread_cond per CV ----------

static pthread_cond_t *conds[1 << 14];
static uint32_t n_cond;
static pthread_cond_t *cv_get(uint32_t cv) {   // CONDITION_VARIABLE is one pointer: we keep our index in it
    uint32_t i = U32(cv);
    if (i) return conds[i];
    pthread_mutex_lock(&mutex_alloc_lock);
    if (!(i = U32(cv))) {
        if (++n_cond >= 1 << 14) die("too many condition variables");
        if (!(n_cond & 0x3FF)) fprintf(stderr, "[win32] %u condition variables\n", n_cond);
        conds[n_cond] = malloc(sizeof(pthread_cond_t));
        pthread_cond_init(conds[n_cond], NULL);
        U32(cv) = i = n_cond;
    }
    pthread_mutex_unlock(&mutex_alloc_lock);
    return conds[i];
}
IMP("KERNEL32.dll", InitializeConditionVariable) { U32(ARG(c, 0)) = 0; ret_std(c, 0, 1); }
IMP("KERNEL32.dll", WakeConditionVariable) { pthread_cond_signal(cv_get(ARG(c, 0))); ret_std(c, 0, 1); }
IMP("KERNEL32.dll", WakeAllConditionVariable) { pthread_cond_broadcast(cv_get(ARG(c, 0))); ret_std(c, 0, 1); }
IMP("KERNEL32.dll", SleepConditionVariableCS) {
    pthread_cond_t *cv = cv_get(ARG(c, 0));
    uint32_t ms = ARG(c, 2);
    int r;
    if (ms == 0xFFFFFFFF) r = pthread_cond_wait(cv, cs_mutex_at(c, ARG(c, 1)));
    else {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_sec += ms / 1000; t.tv_nsec += (ms % 1000) * 1000000L;
        if (t.tv_nsec >= 1000000000L) t.tv_sec++, t.tv_nsec -= 1000000000L;
        r = pthread_cond_timedwait(cv, cs_mutex_at(c, ARG(c, 1)), &t);
    }
    if (r) LAST_ERROR(c) = 1460;      // ERROR_TIMEOUT
    ret_std(c, r == 0, 3);
}
IMP("KERNEL32.dll", GetModuleFileNameA) {
    uint32_t buf = ARG(c, 1), n = ARG(c, 2), len = strlen(FAKE_EXE);
    if (n) { strncpy(GSTR(buf), FAKE_EXE, n); GSTR(buf)[n - 1] = 0; }
    ret_std(c, len < n ? len : n, 3);
}
// The game only starts itself (GetModuleFileNameA(NULL) -> CreateProcessA -> ExitProcess: a restart after some
// settings changes), so re-run this binary the way it was started. Nothing else may be launched.
IMP("KERNEL32.dll", CreateProcessA) {   // (app, cmdline, ..., si, pi): 10 args
    const char *app = ARG(c, 0) ? GSTR(ARG(c, 0)) : "";
    if (strcasecmp(app, FAKE_EXE)) die("CreateProcessA(\"%s\"): launching other programs is not supported", app);
    fprintf(stderr, "[kernel32] the game restarts itself\n");
    if (host_cwd[0] && chdir(host_cwd)) {}
    execv("/proc/self/exe", host_argv);
    die("restart failed: %s", strerror(errno));
}
IMP("KERNEL32.dll", GetCommandLineA) {
    static uint32_t s;
    if (!s) s = gdup("\"" FAKE_EXE "\"");
    ret_std(c, s, 0);
}
static void startup_info(CPU *c) { memset(GM(ARG(c, 0)), 0, 68); U32(ARG(c, 0)) = 68; ret_std(c, 0, 1); }
IMP("KERNEL32.dll", GetStartupInfoW) { startup_info(c); }
IMP("KERNEL32.dll", GetStartupInfoA) { startup_info(c); }
IMP("KERNEL32.dll", GetSystemInfo) {
    uint32_t p = ARG(c, 0), n = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);
    memset(GM(p), 0, 36);
    U32(p + 4) = 4096;                 // page size
    U32(p + 8) = 0x10000;              // min app address
    U32(p + 12) = 0x7FFEFFFF;          // max app address
    U32(p + 16) = n >= 32 ? 0xFFFFFFFF : (1u << n) - 1;
    U32(p + 20) = n;
    U32(p + 24) = 586;                 // PROCESSOR_INTEL_PENTIUM
    U32(p + 28) = 0x10000;             // allocation granularity
    V(GM(p + 32), uint16_t) = 6;       // processor level
    ret_std(c, 0, 1);
}
IMP("KERNEL32.dll", GlobalMemoryStatusEx) {
    uint32_t p = ARG(c, 0);
    U32(p + 4) = 25;                                     // memory load %
    V(GM(p + 8), uint64_t) = 4ull << 30;                 // total phys
    V(GM(p + 16), uint64_t) = 3ull << 30;                // avail phys
    V(GM(p + 24), uint64_t) = 8ull << 30;                // page file
    V(GM(p + 32), uint64_t) = 6ull << 30;
    V(GM(p + 40), uint64_t) = 4ull << 30;                // a large-address-aware 32-bit process sees 4 GB
    V(GM(p + 48), uint64_t) = 3ull << 30;
    V(GM(p + 56), uint64_t) = 0;
    ret_std(c, 1, 1);
}
IMP("KERNEL32.dll", GetProcessAffinityMask) {
    uint32_t n = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN), m = n >= 32 ? 0xFFFFFFFF : (1u << n) - 1;
    U32(ARG(c, 1)) = m; U32(ARG(c, 2)) = m;
    ret_std(c, 1, 3);
}
IMP("KERNEL32.dll", SetThreadAffinityMask) { ret_std(c, 0xFFFFFFFF, 2); }
IMP("KERNEL32.dll", SetThreadPriority) { ret_std(c, 1, 2); }
IMP("KERNEL32.dll", SetThreadExecutionState) { ret_std(c, 0x80000000, 1); }
IMP("KERNEL32.dll", VerSetConditionMask) { REG(c, R_EDX) = 0; ret_std(c, 0, 4); }   // ULONGLONG arg = 2 slots
IMP("KERNEL32.dll", VerifyVersionInfoA) { ret_std(c, 1, 4); }   // "yes, Windows is new enough" (DWORDLONG = 2 slots)
IMP("KERNEL32.dll", VerifyVersionInfoW) { ret_std(c, 1, 4); }
// Windows 7 SP1 (6.1.7601), what d3dx9_43 era code expects
IMP("KERNEL32.dll", GetVersion) { ret_std(c, 7601u << 16 | 1 << 8 | 6, 0); }
IMP("KERNEL32.dll", GetVersionExA) {   // OSVERSIONINFOA: size, major, minor, build, platform, szCSDVersion[128]
    uint32_t p = ARG(c, 0);
    memset(GM(p + 4), 0, U32(p) - 4);
    U32(p + 4) = 6; U32(p + 8) = 1; U32(p + 12) = 7601; U32(p + 16) = 2;
    strcpy(GSTR(p + 20), "Service Pack 1");
    ret_std(c, 1, 1);
}
IMP("KERNEL32.dll", GetACP) { ret_std(c, 1252, 0); }
IMP("KERNEL32.dll", IsDBCSLeadByte) { ret_std(c, 0, 1); }   // code page 1252 has no lead bytes
IMP("KERNEL32.dll", lstrcmpW) {   // ponytail: code-unit order, not Windows' linguistic order; same result for equality
    uint32_t a = ARG(c, 0), b = ARG(c, 1);
    uint16_t x, y;
    do { x = V(GM(a), uint16_t); y = V(GM(b), uint16_t); a += 2; b += 2; } while (x && x == y);
    ret_std(c, x < y ? 0xFFFFFFFFu : x > y, 2);
}
IMP("KERNEL32.dll", lstrcmpiA) { ret_std(c, (uint32_t)strcasecmp(GSTR(ARG(c, 0)), GSTR(ARG(c, 1))), 2); }
// parameter probes: every guest address is mapped, so "not bad"
IMP("KERNEL32.dll", IsBadReadPtr) { ret_std(c, 0, 2); }
IMP("KERNEL32.dll", IsBadWritePtr) { ret_std(c, 0, 2); }
IMP("KERNEL32.dll", IsBadCodePtr) { ret_std(c, 0, 1); }


// ---------- interlocked ----------

// Lifted LOCK-prefixed ops are atomic only under rt_lock (load-modify-store), so these must take the same lock:
// a host atomic here would race with an inlined `lock xadd` on the same refcount and lose updates.
static uint32_t interlocked(CPU *c, int op) {
    rt_lock();
    uint32_t *p = (uint32_t *)GM(ARG(c, 0)), old = *p;
    switch (op) {
    case 0: *p = old + 1; old += 1; break;        // Increment: returns the new value
    case 1: *p = old - 1; old -= 1; break;        // Decrement: returns the new value
    case 2: *p = ARG(c, 1); break;                // Exchange
    case 3: *p = old + ARG(c, 1); break;          // ExchangeAdd
    case 4: if (old == ARG(c, 2)) *p = ARG(c, 1); break;   // CompareExchange(dest, exchange, comparand)
    }
    rt_unlock();
    return old;
}
IMP("KERNEL32.dll", InterlockedIncrement) { ret_std(c, interlocked(c, 0), 1); }
IMP("KERNEL32.dll", InterlockedDecrement) { ret_std(c, interlocked(c, 1), 1); }
IMP("KERNEL32.dll", InterlockedExchange) { ret_std(c, interlocked(c, 2), 2); }
IMP("KERNEL32.dll", InterlockedExchangeAdd) { ret_std(c, interlocked(c, 3), 2); }
IMP("KERNEL32.dll", InterlockedCompareExchange) { ret_std(c, interlocked(c, 4), 3); }

// ---------- text encoding (code page 0 = ANSI = 1252 on an English Windows) ----------

static const char *cp_name(uint32_t cp) {
    return cp == 65001 ? "UTF-8" : cp == 932 ? "CP932" : "CP1252";
}
static uint32_t convert(const char *to, const char *from, const char *src, size_t n, char *dst, size_t cap) {
    char tmp[8192];
    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) return 0;
    char *in = (char *)src, *out = cap ? dst : tmp;
    size_t inleft = n, outleft = cap ? cap : sizeof tmp;
    size_t r = iconv(cd, &in, &inleft, &out, &outleft);
    iconv_close(cd);
    if (r == (size_t)-1 && inleft) return 0;   // invalid input or buffer too small: Windows returns 0 too
    return (uint32_t)((cap ? cap : sizeof tmp) - outleft);
}
// Windows' 1252 for 0x80-0x9F. Unlike glibc's CP1252, the 5 unassigned bytes still convert (to U+0081...): on Windows
// MultiByteToWideChar(CP_ACP) never fails, and the game uses its output without checking the result.
static const uint16_t cp1252_80[32] = {0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160,
    0x2039, 0x152, 0x8D, 0x17D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161,
    0x203A, 0x153, 0x9D, 0x17E, 0x178};
IMP("KERNEL32.dll", MultiByteToWideChar) {
    uint32_t src = ARG(c, 2), cap = ARG(c, 5);
    int32_t n = (int32_t)ARG(c, 3);
    size_t len = n < 0 ? strlen(GSTR(src)) + 1 : (size_t)n;
    if (!strcmp(cp_name(ARG(c, 0)), "CP1252")) {
        if (cap && cap < len) { LAST_ERROR(c) = 122; ret_std(c, 0, 6); return; }   // ERROR_INSUFFICIENT_BUFFER
        for (size_t i = 0; cap && i < len; i++) {
            uint8_t b = (uint8_t)GSTR(src)[i];
            V(GM(ARG(c, 4) + 2 * i), uint16_t) = b >= 0x80 && b < 0xA0 ? cp1252_80[b - 0x80] : b;
        }
        ret_std(c, (uint32_t)len, 6);
        return;
    }
    uint32_t bytes = convert("UTF-16LE", cp_name(ARG(c, 0)), GSTR(src), len, cap ? GSTR(ARG(c, 4)) : NULL, 2 * cap);
    ret_std(c, bytes / 2, 6);
}
IMP("KERNEL32.dll", WideCharToMultiByte) {
    uint32_t src = ARG(c, 2), cap = ARG(c, 5);
    int32_t n = (int32_t)ARG(c, 3);
    size_t len = n;
    if (n < 0) { len = 0; while (V(GM(src + 2 * len), uint16_t)) len++; len++; }
    // 1252 with //TRANSLIT: an unmappable character becomes '?' or a best fit (U+0100 -> 'A'), as on Windows, not a failure
    const char *to = strcmp(cp_name(ARG(c, 0)), "CP1252") ? cp_name(ARG(c, 0)) : "CP1252//TRANSLIT";
    ret_std(c, convert(to, "UTF-16LE", GSTR(src), 2 * len, cap ? GSTR(ARG(c, 4)) : NULL, cap), 8);
}

// ---------- local heap ----------

IMP("KERNEL32.dll", LocalAlloc) {
    uint32_t p = gmalloc(ARG(c, 1));
    if (p && (ARG(c, 0) & 0x40)) memset(GM(p), 0, ARG(c, 1));   // LMEM_ZEROINIT
    ret_std(c, p, 2);
}
IMP("KERNEL32.dll", LocalFree) { gfree(ARG(c, 0)); ret_std(c, 0, 1); }
// Win32 heaps: all one guest arena; a heap handle is just a nonzero token
IMP("KERNEL32.dll", GetProcessHeap) { ret_std(c, 0xC0DE0100u, 0); }
IMP("KERNEL32.dll", HeapCreate) { ret_std(c, 0xC0DE0101u, 3); }
IMP("KERNEL32.dll", HeapDestroy) { ret_std(c, 1, 1); }   // ponytail: blocks of a destroyed heap leak
IMP("KERNEL32.dll", HeapAlloc) {
    uint32_t p = gmalloc(ARG(c, 2));
    if (p && (ARG(c, 1) & 8)) memset(GM(p), 0, ARG(c, 2));   // HEAP_ZERO_MEMORY
    ret_std(c, p, 3);
}
IMP("KERNEL32.dll", HeapFree) { gfree(ARG(c, 2)); ret_std(c, 1, 3); }

// ---------- CRT startup (api-ms-win-crt-runtime) ----------

#define CRT_RT "api-ms-win-crt-runtime-l1-1-0.dll"
IMP(CRT_RT, _set_app_type) { RET_CDECL(c, 0); }
IMP(CRT_RT, _seh_filter_exe) { RET_CDECL(c, 0); }   // EXCEPTION_CONTINUE_SEARCH
IMP(CRT_RT, _configure_narrow_argv) { RET_CDECL(c, 0); }
IMP(CRT_RT, _initialize_narrow_environment) { RET_CDECL(c, 0); }
IMP(CRT_RT, _initialize_onexit_table) { memset(GM(ARG(c, 0)), 0, 12); RET_CDECL(c, 0); }
// ponytail: atexit/onexit callbacks are accepted but never run; run them at exit if shutdown order ever matters
IMP(CRT_RT, _register_onexit_function) { RET_CDECL(c, 0); }
IMP(CRT_RT, _crt_atexit) { RET_CDECL(c, 0); }
IMP(CRT_RT, _register_thread_local_exe_atexit_callback) { RET_CDECL(c, 0); }
IMP(CRT_RT, _get_narrow_winmain_command_line) {
    static uint32_t s;
    if (!s) s = gdup("");
    RET_CDECL(c, s);
}
IMP(CRT_RT, _initterm) {
    for (uint32_t p = ARG(c, 0), end = ARG(c, 1); p < end; p += 4) {
        uint32_t esp = REG(c, R_ESP);
        if (U32(p)) guest_call(c, U32(p), 0, 0);
        REG(c, R_ESP) = esp;
    }
    RET_CDECL(c, 0);
}
IMP(CRT_RT, _initterm_e) {
    for (uint32_t p = ARG(c, 0), end = ARG(c, 1); p < end; p += 4) {
        uint32_t esp = REG(c, R_ESP), r = U32(p) ? guest_call(c, U32(p), 0, 0) : 0;
        REG(c, R_ESP) = esp;
        if (r) { RET_CDECL(c, r); return; }
    }
    RET_CDECL(c, 0);
}
IMP(CRT_RT, exit) { fflush(NULL); exit((int)ARG(c, 0)); }
IMP(CRT_RT, _exit) { fflush(NULL); _exit((int)ARG(c, 0)); }
IMP(CRT_RT, _cexit) { RET_CDECL(c, 0); }
IMP(CRT_RT, _c_exit) { RET_CDECL(c, 0); }
IMP(CRT_RT, terminate) { die("std::terminate called"); }
IMP(CRT_RT, _invalid_parameter_noinfo) { RET_CDECL(c, 0); }
IMP(CRT_RT, _invalid_parameter_noinfo_noreturn) { die("CRT invalid parameter (noreturn)"); }
IMP(CRT_RT, _getpid) { RET_CDECL(c, (uint32_t)getpid()); }
IMP(CRT_RT, _errno) { RET_CDECL(c, TEB() + 0xFF0); }   // per-thread errno slot in the TEB's spare tail

// _controlfp_s / _control87: MS bit layout. We track the value and apply precision + rounding to the host x87,
// because lifted x87 math runs on the host FPU.
// Per thread, like the hardware word the real CRT reads (fstcw): one thread's save/set/restore must not leak into another.
static __thread uint32_t fp_cw = 0x9001F;   // _CW_DEFAULT: 53-bit precision, round to nearest, all exceptions masked
static uint32_t set_fpcw(uint32_t nw, uint32_t mask) {
    fp_cw = (fp_cw & ~mask) | (nw & mask);
    uint32_t pc = fp_cw & 0x30000, rc = fp_cw & 0x300;
    uint16_t x = 0x3F | (pc == 0x20000 ? 0 : pc == 0x10000 ? 0x200 : 0x300) | (uint16_t)(rc << 2);
    __asm__ volatile("fldcw %0" : : "m"(x));
    return fp_cw;
}
IMP(CRT_RT, _controlfp_s) {
    uint32_t v = set_fpcw(ARG(c, 1), ARG(c, 2) & 0x30300);
    if (ARG(c, 0)) U32(ARG(c, 0)) = v;
    RET_CDECL(c, 0);
}
IMP(CRT_RT, _control87) { RET_CDECL(c, set_fpcw(ARG(c, 0), ARG(c, 1) & 0x30300)); }
IMP("msvcrt.dll", _controlfp) { RET_CDECL(c, set_fpcw(ARG(c, 0), ARG(c, 1) & 0x30300)); }
IMP("msvcrt.dll", _clearfp) { __asm__ volatile("fnclex"); RET_CDECL(c, 0); }   // previous status: none raised

IMP("api-ms-win-crt-locale-l1-1-0.dll", _configthreadlocale) { RET_CDECL(c, 2); }   // _DISABLE_PER_THREAD_LOCALE
IMP("api-ms-win-crt-math-l1-1-0.dll", __setusermatherr) { RET_CDECL(c, 0); }
IMP("api-ms-win-crt-stdio-l1-1-0.dll", _set_fmode) { RET_CDECL(c, 0); }
IMP("api-ms-win-crt-stdio-l1-1-0.dll", __p__commode) {
    static uint32_t p;
    if (!p) p = gmalloc(4), U32(p) = 0;
    RET_CDECL(c, p);
}

// ---------- CRT heap ----------

#define CRT_HEAP "api-ms-win-crt-heap-l1-1-0.dll"
IMP(CRT_HEAP, malloc) { RET_CDECL(c, gmalloc(ARG(c, 0))); }
IMP(CRT_HEAP, free) { gfree(ARG(c, 0)); RET_CDECL(c, 0); }
IMP(CRT_HEAP, calloc) {
    uint64_t n = (uint64_t)ARG(c, 0) * ARG(c, 1);
    uint32_t p = n < 0x7FFFFFF0u ? gmalloc((uint32_t)n) : 0;
    if (p) memset(GM(p), 0, n);
    RET_CDECL(c, p);
}
IMP(CRT_HEAP, realloc) { RET_CDECL(c, grealloc(ARG(c, 0), ARG(c, 1))); }
IMP(CRT_HEAP, _set_new_mode) { RET_CDECL(c, 0); }
IMP(CRT_HEAP, _callnewh) { RET_CDECL(c, 0); }   // no new-handler installed: operator new then throws bad_alloc
// aligned: over-allocate, align up, keep the real block address just below the user pointer
static uint32_t aligned_alloc32(uint32_t n, uint32_t align) {
    if (align < 4) align = 4;
    uint32_t raw = gmalloc(n + align + 4);
    if (!raw) return 0;
    uint32_t p = (raw + 4 + align - 1) & ~(align - 1);
    U32(p - 4) = raw;
    return p;
}
IMP(CRT_HEAP, _aligned_malloc) { RET_CDECL(c, aligned_alloc32(ARG(c, 0), ARG(c, 1))); }
IMP(CRT_HEAP, _aligned_free) { if (ARG(c, 0)) gfree(U32(ARG(c, 0) - 4)); RET_CDECL(c, 0); }
IMP(CRT_HEAP, _aligned_realloc) {
    uint32_t old = ARG(c, 0), n = ARG(c, 1), p = n ? aligned_alloc32(n, ARG(c, 2)) : 0;
    if (old) {
        uint32_t raw = U32(old - 4), have = gsize(raw) - (old - raw);
        if (p) memcpy(GM(p), GM(old), have < n ? have : n);
        gfree(raw);
    }
    RET_CDECL(c, p);
}

// VirtualAlloc from the guest arena, 64 KB aligned and zeroed. Committing inside an earlier reservation = no-op.
// ponytail: reserve == commit, MEM_DECOMMIT ignored; fine for d3dx9's scratch buffers
IMP("KERNEL32.dll", VirtualAlloc) {   // (addr, size, type, protect)
    if (ARG(c, 0)) { ret_std(c, ARG(c, 0), 4); return; }
    uint32_t p = aligned_alloc32(ARG(c, 1), 0x10000);
    if (p) memset(GM(p), 0, ARG(c, 1));
    ret_std(c, p, 4);
}
IMP("KERNEL32.dll", VirtualFree) {    // (addr, size, type): MEM_RELEASE = 0x8000
    if (ARG(c, 2) & 0x8000) gfree(U32(ARG(c, 0) - 4));
    ret_std(c, 1, 3);
}

// ---------- msvcrt.dll only (the old CRT of Microsoft's DLLs); same-named functions come from the UCRT shims ----------

#define MSVCRT "msvcrt.dll"
static pthread_mutex_t crt_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
IMP(MSVCRT, _lock) { pthread_mutex_lock(&crt_lock); RET_CDECL(c, 0); }   // ponytail: one lock for all CRT lock ids
IMP(MSVCRT, _unlock) { pthread_mutex_unlock(&crt_lock); RET_CDECL(c, 0); }
// ponytail: DLL atexit callbacks are accepted but never run (same as the exe's)
IMP(MSVCRT, _onexit) { RET_CDECL(c, ARG(c, 0)); }
IMP(MSVCRT, __dllonexit) { RET_CDECL(c, ARG(c, 0)); }
IMP(MSVCRT, _amsg_exit) { die("CRT fatal error R60%02u", ARG(c, 0)); }
IMP(MSVCRT, _XcptFilter) { RET_CDECL(c, 0); }   // EXCEPTION_CONTINUE_SEARCH
IMP(MSVCRT, abort) { die("abort() called"); }
IMPX(MSVCRT, "?terminate@@YAXXZ", ms_terminate) { die("std::terminate called"); }
IMP(MSVCRT, __CxxFrameHandler) { die("C++ exception handler called (no exception dispatch)"); }
IMPX(MSVCRT, "??2@YAPAXI@Z", ms_new) { RET_CDECL(c, gmalloc(ARG(c, 0))); }   // operator new
IMPX(MSVCRT, "??3@YAXPAX@Z", ms_delete) { gfree(ARG(c, 0)); RET_CDECL(c, 0); }   // operator delete
IMPX(MSVCRT, "??1type_info@@UAE@XZ", ms_typeinfo_dtor) { RET_CDECL(c, 0); }   // thiscall, no stack args
// ponytail: setjmp records nothing; a longjmp (d3dx9's JPEG/PNG error path) stops with its name instead
IMP(MSVCRT, _setjmp3) { RET_CDECL(c, 0); }
IMP(MSVCRT, longjmp) { die("longjmp called (guest setjmp/longjmp not supported yet)"); }

// ---------- C++ exceptions: a throw stops the game with the exception's type and message ----------
// ponytail: no unwinding. The exe has 142 throw sites (STL errors: length_error, bad_alloc, ...) but only 20 try
// blocks, all catch(...) cleanup-and-rethrow, so a throw is an error the game can't recover from anyway. Add SEH
// dispatch (walk FS:[0], run __CxxFrameHandler3's unwind maps, resume in the catch funclet) if a run ever needs it.
IMP("VCRUNTIME140.dll", _CxxThrowException) {   // (object, ThrowInfo*), never returns
    uint32_t obj = ARG(c, 0), ti = ARG(c, 1), cta = ti ? U32(ti + 12) : 0;
    const char *type = cta && U32(cta) ? GSTR(U32(U32(cta + 4) + 4) + 8) : "?";   // first CatchableType's TypeDescriptor name
    int is_std = 0;   // derived from std::exception: the object holds __std_exception_data { what, do_free } at +4
    for (uint32_t i = 0; cta && i < U32(cta); i++) is_std |= !strcmp(GSTR(U32(U32(cta + 4 + 4 * i) + 4) + 8), ".?AVexception@std@@");
    fprintf(stderr, "catherine: throw backtrace (EBP chain):");   // who threw: the throw site alone is a CRT helper
    for (uint32_t ebp = REG(c, 0x14), i = 0; i < 20 && ebp >= 0x10000 && ebp < 0xFFFF0000u; i++, ebp = U32(ebp))   // any thread's stack
        fprintf(stderr, " %08x", U32(ebp + 4));
    fprintf(stderr, "\n");
    die("C++ exception %s thrown at %x: %s", type, U32(REG(c, R_ESP)) - 5,
        is_std && obj && U32(obj + 4) ? GSTR(U32(obj + 4)) : "(no message)");
}
IMP("VCRUNTIME140.dll", __std_exception_destroy) {   // (__std_exception_data*): free the message if owned
    uint32_t d = ARG(c, 0);
    if (V(GM(d + 4), uint8_t)) gfree(U32(d));
    U32(d) = 0; V(GM(d + 4), uint8_t) = 0;
    RET_CDECL(c, 0);
}
// only an exception dispatcher calls these (from a frame's SEH registration), and there is none: see above
IMP("VCRUNTIME140.dll", __CxxFrameHandler3) { die("C++ exception handler called (no exception dispatch)"); }
IMP("VCRUNTIME140.dll", _except_handler4_common) { die("SEH handler called (no exception dispatch)"); }
IMP("KERNEL32.dll", RtlUnwind) { die("RtlUnwind called (no exception dispatch)"); }

// ---------- memory / string basics (VCRUNTIME140 + api-ms-win-crt-string) ----------

#define VCRT "VCRUNTIME140.dll"
#define CRT_STR "api-ms-win-crt-string-l1-1-0.dll"
IMP(VCRT, memset) { memset(GM(ARG(c, 0)), (int)ARG(c, 1), ARG(c, 2)); RET_CDECL(c, ARG(c, 0)); }
IMP(VCRT, memcpy) { memmove(GM(ARG(c, 0)), GM(ARG(c, 1)), ARG(c, 2)); RET_CDECL(c, ARG(c, 0)); }
IMP(VCRT, memmove) { memmove(GM(ARG(c, 0)), GM(ARG(c, 1)), ARG(c, 2)); RET_CDECL(c, ARG(c, 0)); }
static uint32_t gptr(const void *p) { return p ? (uint32_t)((const uint8_t *)p - M) : 0; }   // host -> guest address
IMP(VCRT, memchr) { RET_CDECL(c, gptr(memchr(GM(ARG(c, 0)), (int)ARG(c, 1), ARG(c, 2)))); }
IMP(VCRT, strchr) { RET_CDECL(c, gptr(strchr(GSTR(ARG(c, 0)), (int)ARG(c, 1)))); }
IMP(VCRT, strrchr) { RET_CDECL(c, gptr(strrchr(GSTR(ARG(c, 0)), (int)ARG(c, 1)))); }
IMP(VCRT, strstr) { RET_CDECL(c, gptr(strstr(GSTR(ARG(c, 0)), GSTR(ARG(c, 1))))); }
IMP(VCRT, _purecall) { die("pure virtual function called"); }
IMP(CRT_STR, strlen) { RET_CDECL(c, strlen(GSTR(ARG(c, 0)))); }
IMP(CRT_STR, strcmp) { RET_CDECL(c, (uint32_t)strcmp(GSTR(ARG(c, 0)), GSTR(ARG(c, 1)))); }
IMP(CRT_STR, strncmp) { RET_CDECL(c, (uint32_t)strncmp(GSTR(ARG(c, 0)), GSTR(ARG(c, 1)), ARG(c, 2))); }
IMP(CRT_STR, _stricmp) { RET_CDECL(c, (uint32_t)strcasecmp(GSTR(ARG(c, 0)), GSTR(ARG(c, 1)))); }
IMP(CRT_STR, _strnicmp) { RET_CDECL(c, (uint32_t)strncasecmp(GSTR(ARG(c, 0)), GSTR(ARG(c, 1)), ARG(c, 2))); }
// The CRT copies strings one byte at a time from the front, so overlapping copies (strcpy(p, p + 5)) work on Windows;
// glibc's block copies garble them ("title_camera_mera_root" from Gamebryo's KF names). Copy the way MSVC does.
static void fwd_copy(char *d, const char *s, size_t n) { for (size_t i = 0; i < n; i++) d[i] = s[i]; }
IMP(CRT_STR, strncpy) {
    char *d = GSTR(ARG(c, 0));
    const char *s = GSTR(ARG(c, 1));
    size_t n = ARG(c, 2), i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    RET_CDECL(c, ARG(c, 0));
}
IMP(CRT_STR, _strdup) { RET_CDECL(c, ARG(c, 0) ? gdup(GSTR(ARG(c, 0))) : 0); }
IMP(CRT_STR, strcspn) { RET_CDECL(c, strcspn(GSTR(ARG(c, 0)), GSTR(ARG(c, 1)))); }
IMP(CRT_STR, strpbrk) { RET_CDECL(c, gptr(strpbrk(GSTR(ARG(c, 0)), GSTR(ARG(c, 1))))); }
// *_s variants: 0 on success, 22 (EINVAL) / 34 (ERANGE) on failure, empty dst on failure like the CRT
IMP(CRT_STR, strcpy_s) {
    uint32_t d = ARG(c, 0), n = ARG(c, 1);
    const char *s = GSTR(ARG(c, 2));
    if (!d || !n) { RET_CDECL(c, 22); return; }
    if (strlen(s) >= n) { GSTR(d)[0] = 0; RET_CDECL(c, 34); return; }
    fwd_copy(GSTR(d), s, strlen(s) + 1);
    RET_CDECL(c, 0);
}
IMP(CRT_STR, strcat_s) {
    uint32_t d = ARG(c, 0), n = ARG(c, 1);
    size_t have = strnlen(GSTR(d), n), add = strlen(GSTR(ARG(c, 2)));
    if (have + add >= n) { GSTR(d)[0] = 0; RET_CDECL(c, 34); return; }
    fwd_copy(GSTR(d) + have, GSTR(ARG(c, 2)), add + 1);
    RET_CDECL(c, 0);
}
IMP(CRT_STR, strncpy_s) {
    uint32_t d = ARG(c, 0), n = ARG(c, 1), cnt = ARG(c, 3);
    const char *s = GSTR(ARG(c, 2));
    size_t len = strnlen(s, cnt == 0xFFFFFFFF ? n : cnt);   // _TRUNCATE = (size_t)-1
    if (len >= n) {
        if (cnt == 0xFFFFFFFF) { fwd_copy(GSTR(d), s, n - 1); GSTR(d)[n - 1] = 0; RET_CDECL(c, 80); return; }   // STRUNCATE
        GSTR(d)[0] = 0; RET_CDECL(c, 34); return;
    }
    fwd_copy(GSTR(d), s, len);
    GSTR(d)[len] = 0;
    RET_CDECL(c, 0);
}
IMP(CRT_STR, memcpy_s) {
    if (ARG(c, 3) > ARG(c, 1)) { memset(GM(ARG(c, 0)), 0, ARG(c, 1)); RET_CDECL(c, 34); return; }
    memmove(GM(ARG(c, 0)), GM(ARG(c, 2)), ARG(c, 3));
    RET_CDECL(c, 0);
}
#define CTYPE(f) IMP(CRT_STR, f) { RET_CDECL(c, (uint32_t)f((int)ARG(c, 0))); }
CTYPE(isalpha) CTYPE(isdigit) CTYPE(isspace) CTYPE(isalnum) CTYPE(isxdigit) CTYPE(isupper) CTYPE(islower)
CTYPE(isprint) CTYPE(isgraph) CTYPE(toupper) CTYPE(tolower)
