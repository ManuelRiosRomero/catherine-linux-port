// CATH_PROFILE=1: a sampling profiler for performance work on the Deck. Creating ~/catherine-port/profile.on starts
// sampling where the CPU is (SIGPROF every 1 ms of process CPU time, any thread), deleting it stops and writes
// ~/catherine-port/profile.txt: "samples object offset [symbol]" per code address, offsets relative to the object's load
// base (build/prof_report.sh names the main binary's with nm). A file, not a signal: threads Steam's preloaded overlay
// starts before main don't block SIGUSR1, so a process-wide signal killed the game.
#define _GNU_SOURCE
#include "host.h"
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>

#define MAX_SAMPLES (1u << 23)   // ~2 h of one busy thread
volatile int prof_on;   // sampling window open: wait functions add their blocked time (prof_wait)
static struct { uint32_t key, caller, n; uint64_t ns; } waits[4096];   // key = thread id (low 16 bits) << 16 | kind
static pthread_mutex_t wait_lk = PTHREAD_MUTEX_INITIALIZER;
uint32_t prof_tid(void) { static __thread uint32_t t; return t ? t : (t = (uint32_t)gettid()); }
uint64_t prof_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000ull + t.tv_nsec; }
// time a guest thread spent blocked in a wait (kind: 1 WaitFor*, 2 Sleep, 3 EnterCriticalSection, 4 condition variable, 5/6 the same lock wait keyed by lock address / holder thread),
// by guest caller (the return address on the guest stack)
void prof_wait(int kind, uint32_t caller, uint64_t t0) {
    uint64_t ns = prof_now() - t0;
    uint32_t key = (uint32_t)(gettid() & 0xFFFF) << 16 | kind, h = (key * 2654435761u ^ caller * 40503u) & 4095;
    pthread_mutex_lock(&wait_lk);
    for (uint32_t k = 0; k < 4096; k++, h = (h + 1) & 4095)
        if (!waits[h].n || (waits[h].key == key && waits[h].caller == caller)) {
            waits[h].key = key, waits[h].caller = caller, waits[h].n++, waits[h].ns += ns;
            break;
        }
    pthread_mutex_unlock(&wait_lk);
}
// Main thread wall clock: every 1 ms of real time, also while it is blocked (ITIMER_PROF only sees CPU time): where
// the host is (rip) and the guest return address on top of the guest stack (= which game call site, inside an import).
#define MAX_WALL (1u << 20)
static CPU *main_cpu;
static timer_t wall_timer;
static uint64_t *wall_rip;
static uint32_t *wall_ret;
static volatile uint32_t n_wall;
static void on_wall(int sig, siginfo_t *si, void *uc) {
    (void)sig, (void)si;
    if (!prof_on) return;
    uint32_t i = __atomic_fetch_add(&n_wall, 1, __ATOMIC_RELAXED);
    if (i < MAX_WALL) wall_rip[i] = (uint64_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP], wall_ret[i] = U32(REG(main_cpu, R_ESP));
}
void prof_main(CPU *c) {   // main.c, on the main thread once its CPU exists
    if (!getenv("CATH_PROFILE")) return;
    main_cpu = c;
    wall_rip = malloc(MAX_WALL * sizeof *wall_rip), wall_ret = malloc(MAX_WALL * sizeof *wall_ret);
    struct sigaction sa = {.sa_sigaction = on_wall, .sa_flags = SA_SIGINFO | SA_RESTART};
    sigaction(SIGRTMIN + 2, &sa, NULL);
    struct sigevent ev = {.sigev_notify = SIGEV_THREAD_ID, .sigev_signo = SIGRTMIN + 2};
    ev._sigev_un._tid = gettid();
    timer_create(CLOCK_MONOTONIC, &ev, &wall_timer);
}
static void wall_dump(FILE *f) {   // "# wall <object> <symbol> <guest return> <samples>", same (object, symbol, return) summed
    uint32_t n = n_wall < MAX_WALL ? n_wall : MAX_WALL;
    static struct { const char *obj, *sym; uint32_t ret, n; } agg[8192];
    uint32_t used = 0;
    for (uint32_t i = 0; i < n; i++) {
        Dl_info d;
        const char *obj = "?", *sym = "-";
        if (dladdr((void *)wall_rip[i], &d) && d.dli_fname) obj = d.dli_fname, sym = d.dli_sname ? d.dli_sname : "-";
        int game = strstr(obj, "catherine-port/catherine") != NULL;   // in the binary: the guest stack top means nothing
        uint32_t ret = game ? 0 : wall_ret[i], k = 0;
        if (game) sym = "-";
        for (; k < used; k++) if (agg[k].ret == ret && !strcmp(agg[k].obj, obj) && !strcmp(agg[k].sym, sym)) break;
        if (k == used && used < 8192) agg[used++] = (typeof(agg[0])){obj, sym, ret, 0};
        if (k < used) agg[k].n++;
    }
    for (uint32_t k = 0; k < used; k++) fprintf(f, "# wall %s %s %08x %u\n", agg[k].obj, agg[k].sym, agg[k].ret, agg[k].n);
}
static uint64_t *pcs;   // code address (x86-64 user space: 47 bits), low 16 bits of the thread id on top
static volatile uint32_t n_pcs;

static void on_prof(int sig, siginfo_t *si, void *uc) {
    (void)sig, (void)si;
    uint32_t i = __atomic_fetch_add(&n_pcs, 1, __ATOMIC_RELAXED);
    if (i < MAX_SAMPLES) pcs[i] = (uint64_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP] | (uint64_t)(gettid() & 0xFFFF) << 48;
}
static int cmp(const void *a, const void *b) { uint64_t x = *(uint64_t *)a, y = *(uint64_t *)b; return x < y ? -1 : x > y; }

static void *prof_thread(void *arg) {
    (void)arg;
    char flag[1024], path[1024];
    snprintf(flag, sizeof flag, "%s/catherine-port/profile.on", getenv("HOME"));
    snprintf(path, sizeof path, "%s/catherine-port/profile.txt", getenv("HOME"));
    for (;;) {
        while (access(flag, F_OK)) usleep(250000);
        n_pcs = 0;
        memset(waits, 0, sizeof waits);
        prof_on = 1;
        n_wall = 0;
        struct itimerspec ms1 = {{0, 1000000}, {0, 1000000}}, stop = {0};
        if (main_cpu) timer_settime(wall_timer, 0, &ms1, NULL);
        struct itimerval on = {{0, 1000}, {0, 1000}}, off = {0};
        setitimer(ITIMER_PROF, &on, NULL);
        fprintf(stderr, "[prof] sampling\n");
        while (!access(flag, F_OK)) usleep(250000);
        setitimer(ITIMER_PROF, &off, NULL);
        prof_on = 0;
        if (main_cpu) timer_settime(wall_timer, 0, &stop, NULL);
        uint32_t n = n_pcs < MAX_SAMPLES ? n_pcs : MAX_SAMPLES;
        qsort(pcs, n, sizeof *pcs, cmp);
        FILE *f = fopen(path, "w");
        DIR *dir = opendir("/proc/self/task");   // "# thread <tid> <name>" lines: which thread is the main one, dxvk, SDL
        for (struct dirent *e; f && dir && (e = readdir(dir));) {
            char p[64], name[64] = "";
            if (e->d_name[0] == '.') continue;
            snprintf(p, sizeof p, "/proc/self/task/%s/comm", e->d_name);
            FILE *cf = fopen(p, "r");
            if (cf) { if (!fgets(name, sizeof name, cf)) name[0] = 0; fclose(cf); }
            name[strcspn(name, "\n")] = 0;
            fprintf(f, "# thread %u %s%s\n", (unsigned)atoi(e->d_name) & 0xFFFF, name, atoi(e->d_name) == getpid() ? " (main)" : "");
        }
        if (dir) closedir(dir);
        if (f && main_cpu) wall_dump(f);
        for (int k = 0; f && k < 4096; k++)   // "# wait <tid> <kind> <caller> <count> <ms>"
            if (waits[k].n) fprintf(f, "# wait %u %u %08x %u %.1f\n", waits[k].key >> 16, waits[k].key & 0xFFFF,
                                    waits[k].caller, waits[k].n, waits[k].ns / 1e6);
        for (uint32_t i = 0, j; f && i < n; i = j) {
            for (j = i; j < n && pcs[j] == pcs[i]; j++) {}
            Dl_info d;
            uint64_t pc = pcs[i] & ((1ull << 48) - 1);
            unsigned tid = (unsigned)(pcs[i] >> 48);
            if (dladdr((void *)pc, &d) && d.dli_fname)
                fprintf(f, "%u %s %lx %s %u\n", j - i, d.dli_fname, (unsigned long)(pc - (uint64_t)d.dli_fbase),
                        d.dli_sname ? d.dli_sname : "-", tid);
            else fprintf(f, "%u ? %lx - %u\n", j - i, (unsigned long)pc, tid);
        }
        if (f) fclose(f);
        fprintf(stderr, "[prof] %u samples -> %s\n", n, path);
    }
    return NULL;
}

void prof_start(void) {
    if (!getenv("CATH_PROFILE")) return;
    pcs = malloc(MAX_SAMPLES * sizeof *pcs);
    struct sigaction sa = {.sa_sigaction = on_prof, .sa_flags = SA_SIGINFO | SA_RESTART};
    sigaction(SIGPROF, &sa, NULL);
    pthread_t t;
    pthread_create(&t, NULL, prof_thread, NULL);
}
