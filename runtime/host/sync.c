// Kernel objects: events, semaphores, threads, waits; plus per-thread TEB + static TLS setup.
// One global lock for every waitable object; each blocked thread sleeps on its own condvar and a change wakes only the
// threads waiting on that handle (registered and signalled under the same lock, so no wakeup is lost).
#define _GNU_SOURCE
#include "host.h"
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { OBJ_FREE, OBJ_EVENT, OBJ_SEM, OBJ_THREAD, OBJ_FILE };
typedef struct {
    int type, signaled, manual;      // event: signaled/manual-reset; thread: signaled = exited
    int32_t count, max;              // semaphore
    uint32_t exit_code;
    int suspended;                   // thread created with CREATE_SUSPENDED, not resumed yet
    int fd;                          // file
    uint32_t tid;                    // thread: its id, so a reused handle slot is never mistaken for this thread
} Obj;

#define MAX_OBJ 65536                // events, threads, files... (4096 could run out if the game forgets a CloseHandle)
#define H_BASE 0x1000u               // handle = H_BASE + 4 * index (never 0 or -1)
static Obj objs[MAX_OBJ];
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
typedef struct Waiter { pthread_cond_t cv; uint32_t n; const uint32_t *hs; struct Waiter *next; uint32_t ret; pid_t tid; } Waiter;
static Waiter *waiters;                // threads blocked on objects (under lk)
static __thread uint32_t wait_ret;     // guest return address of the current WaitFor* call (for dump_waiters)
static void wake(uint32_t h) {         // under lk
    for (Waiter *w = waiters; w; w = w->next)
        for (uint32_t i = 0; i < w->n; i++) if (w->hs[i] == h) { pthread_cond_signal(&w->cv); break; }
}
static int sleep_on(uint32_t n, const uint32_t *hs, const struct timespec *dl) {   // under lk; nonzero = timed out
    static __thread Waiter me;
    static __thread int init;
    if (!init) pthread_cond_init(&me.cv, NULL), init = 1;
    me.n = n, me.hs = hs, me.next = waiters, waiters = &me, me.ret = wait_ret, me.tid = gettid();
    int r = dl ? pthread_cond_timedwait(&me.cv, &lk, dl) : pthread_cond_wait(&me.cv, &lk);
    for (Waiter **p = &waiters; *p; p = &(*p)->next) if (*p == &me) { *p = me.next; break; }
    return r;
}

static uint32_t obj_new(int type) {
    static int high;   // highest slot ever used: logged per 1024, so a handle leak shows up long before the cap
    pthread_mutex_lock(&lk);
    for (int i = 1; i < MAX_OBJ; i++)
        if (objs[i].type == OBJ_FREE) {
            memset(&objs[i], 0, sizeof objs[i]);
            objs[i].type = type;
            if (i > high && !((high = i) & 0x3FF)) fprintf(stderr, "[sync] %d kernel object handles live\n", i);
            pthread_mutex_unlock(&lk);
            return H_BASE + 4 * i;
        }
    die("out of kernel object handles");
}
static Obj *obj(uint32_t h) {
    uint32_t i = (h - H_BASE) / 4;
    return h >= H_BASE && i < MAX_OBJ && objs[i].type != OBJ_FREE ? &objs[i] : NULL;
}

IMP("KERNEL32.dll", CloseHandle) {
    pthread_mutex_lock(&lk);
    Obj *o = obj(ARG(c, 0));
    if (o && o->type == OBJ_FILE && o->fd > 2) close(o->fd);
    if (o) o->type = OBJ_FREE, wake(ARG(c, 0));   // ponytail: no refcount; its waiters now get WAIT_FAILED
    pthread_mutex_unlock(&lk);
    ret_std(c, 1, 1);                   // files and other handles: accepted
}

uint32_t handle_for_fd(int fd) { uint32_t h = obj_new(OBJ_FILE); obj(h)->fd = fd; return h; }
int fd_of(uint32_t h) {
    pthread_mutex_lock(&lk);
    Obj *o = obj(h);
    int fd = o && o->type == OBJ_FILE ? o->fd : -1;
    pthread_mutex_unlock(&lk);
    return fd;
}

// ---------- events / semaphores ----------

static void create_event(CPU *c) {
    uint32_t h = obj_new(OBJ_EVENT);
    Obj *o = obj(h);
    o->manual = ARG(c, 1) != 0;
    o->signaled = ARG(c, 2) != 0;
    ret_std(c, h, 4);
}
IMP("KERNEL32.dll", CreateEventA) { create_event(c); }
IMP("KERNEL32.dll", CreateEventW) { create_event(c); }
static void set_event(CPU *c, int v) {
    pthread_mutex_lock(&lk);
    Obj *o = obj(ARG(c, 0));
    if (o) { o->signaled = v; if (v) wake(ARG(c, 0)); }
    pthread_mutex_unlock(&lk);
    ret_std(c, o != NULL, 1);
}
IMP("KERNEL32.dll", SetEvent) { set_event(c, 1); }
void event_signal(uint32_t h) {
    pthread_mutex_lock(&lk);
    Obj *o = obj(h);
    if (o) { o->signaled = 1; wake(h); }
    pthread_mutex_unlock(&lk);
}
IMP("KERNEL32.dll", ResetEvent) { set_event(c, 0); }
IMP("KERNEL32.dll", CreateSemaphoreA) {
    uint32_t h = obj_new(OBJ_SEM);
    Obj *o = obj(h);
    o->count = (int32_t)ARG(c, 1);
    o->max = (int32_t)ARG(c, 2);
    ret_std(c, h, 4);
}
IMP("KERNEL32.dll", ReleaseSemaphore) {
    pthread_mutex_lock(&lk);
    Obj *o = obj(ARG(c, 0));
    int ok = o && o->count + (int32_t)ARG(c, 1) <= o->max;
    if (ok) {
        if (ARG(c, 2)) U32(ARG(c, 2)) = o->count;
        o->count += ARG(c, 1);
        wake(ARG(c, 0));
    }
    pthread_mutex_unlock(&lk);
    ret_std(c, ok, 3);
}

// ---------- waits ----------

static int ready(Obj *o) {
    return o->type == OBJ_SEM ? o->count > 0 : o->signaled;
}
static void consume(Obj *o) {
    if (o->type == OBJ_SEM) o->count--;
    else if (o->type == OBJ_EVENT && !o->manual) o->signaled = 0;
}
// Returns WAIT_OBJECT_0 + i, WAIT_TIMEOUT (0x102) or WAIT_FAILED.
static uint32_t wait_for(uint32_t n, const uint32_t *hs, int all, uint32_t ms) {
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += ms / 1000;
    dl.tv_nsec += (ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) dl.tv_sec++, dl.tv_nsec -= 1000000000L;
    pthread_mutex_lock(&lk);
    for (;;) {
        uint32_t nready = 0, first = n;
        for (uint32_t i = 0; i < n; i++) {
            Obj *o = obj(hs[i]);
            if (!o) { pthread_mutex_unlock(&lk); return 0xFFFFFFFF; }
            if (ready(o)) { nready++; if (first == n) first = i; }
        }
        if (all ? nready == n : nready > 0) {
            for (uint32_t i = all ? 0 : first; i < (all ? n : first + 1); i++) consume(obj(hs[i]));
            pthread_mutex_unlock(&lk);
            return all ? 0 : first;
        }
        if (ms == 0) break;
        if (sleep_on(n, hs, ms == 0xFFFFFFFF ? NULL : &dl)) break;
    }
    pthread_mutex_unlock(&lk);
    return 0x102;
}
uint32_t wait_handle(uint32_t h, uint32_t ms) { wait_ret = 0; return wait_for(1, &h, 0, ms); }
void dump_waiters(void) {   // test runs (steam.c, waiters.on): every blocked thread and the state of what it waits on
    static const char *kind[] = {"free", "event", "sem", "thread", "file"};
    pthread_mutex_lock(&lk);
    for (Waiter *w = waiters; w; w = w->next) {
        fprintf(stderr, "[sync] tid %d waits from %08x on", (int)w->tid, w->ret);
        for (uint32_t i = 0; i < w->n; i++) {
            Obj *o = obj(w->hs[i]);
            if (o) fprintf(stderr, " %x(%s %s %d)", w->hs[i], kind[o->type], o->manual ? "manual" : "auto", o->type == OBJ_SEM ? o->count : o->signaled);
            else fprintf(stderr, " %x(gone)", w->hs[i]);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "[sync] end of waiters\n");
    pthread_mutex_unlock(&lk);
}
#define TIMED(kind, call) do { uint64_t t0_ = prof_on ? prof_now() : 0; call; if (t0_) prof_wait(kind, U32(REG(c, R_ESP)), t0_); } while (0)
IMP("KERNEL32.dll", WaitForSingleObject) { uint32_t h = ARG(c, 0), r; wait_ret = U32(REG(c, R_ESP)); TIMED(1, r = wait_for(1, &h, 0, ARG(c, 1))); ret_std(c, r, 2); }
IMP("KERNEL32.dll", WaitForSingleObjectEx) { uint32_t h = ARG(c, 0), r; wait_ret = U32(REG(c, R_ESP)); TIMED(1, r = wait_for(1, &h, 0, ARG(c, 1))); ret_std(c, r, 3); }
IMP("KERNEL32.dll", WaitForMultipleObjects) {
    uint32_t n = ARG(c, 0), hs[64];
    if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++) hs[i] = U32(ARG(c, 1) + 4 * i);
    uint32_t r;
    wait_ret = U32(REG(c, R_ESP));
    TIMED(1, r = wait_for(n, hs, ARG(c, 2) != 0, ARG(c, 3)));
    ret_std(c, r, 4);
}

// ---------- TEB + static TLS (the exe has a .tls section) ----------

void teb_init(uint32_t teb, uint32_t stack_lo, uint32_t stack_hi) {
    uint32_t base = U32(G_PEB + 8), pe = base + U32(base + 0x3C), tls_rva = U32(pe + 0x78 + 9 * 8);
    memset(GM(teb), 0, 0x1000);
    U32(teb + 0x00) = 0xFFFFFFFF;       // end of SEH chain
    U32(teb + 0x04) = stack_hi;
    U32(teb + 0x08) = stack_lo;
    U32(teb + 0x18) = teb;
    U32(teb + 0x30) = G_PEB;
    if (tls_rva) {                      // IMAGE_TLS_DIRECTORY32: start, end, &index, &callbacks, zerofill
        uint32_t d = base + tls_rva, start = U32(d), end = U32(d + 4), zero = U32(d + 16);
        uint32_t block = gmalloc(end - start + zero + 16), array = gmalloc(16);
        memcpy(GM(block), GM(start), end - start);
        memset(GM(block + end - start), 0, zero);
        U32(U32(d + 8)) = 0;            // our TLS index is 0
        U32(array) = block;
        U32(teb + 0x2C) = array;        // ThreadLocalStoragePointer
    }
}

// TLS callbacks run with DLL_PROCESS_ATTACH (1) for the main thread, DLL_THREAD_ATTACH (2) for others.
void tls_callbacks(CPU *c, uint32_t reason) {
    uint32_t base = U32(G_PEB + 8), pe = base + U32(base + 0x3C), tls_rva = U32(pe + 0x78 + 9 * 8);
    if (!tls_rva) return;
    for (uint32_t cb = U32(base + tls_rva + 12); cb && U32(cb); cb += 4) guest_call(c, U32(cb), 1, 3, base, reason, 0);
}

// ---------- threads ----------

typedef struct { uint32_t start, param, h, tid; } ThreadArgs;
static uint32_t next_tid = 1;          // main thread = 1; ids stay unique after CloseHandle frees the handle slot

static void *thread_main(void *p) {
    ThreadArgs a = *(ThreadArgs *)p;
    free(p);
    uint16_t cw = 0x27F;
    __asm__ volatile("fldcw %0" : : "m"(cw));
    pthread_mutex_lock(&lk);           // CREATE_SUSPENDED: wait for ResumeThread
    while (obj(a.h) && obj(a.h)->suspended) sleep_on(1, &a.h, NULL);
    pthread_mutex_unlock(&lk);

    uint32_t stack = gmalloc(1 << 20), teb = gmalloc(0x1000 + 0xFFF);
    teb = (teb + 0xFFF) & ~0xFFFu;
    teb_init(teb, stack, stack + (1 << 20));
    U32(teb + 0x24) = a.tid;            // ClientId.UniqueThread = the id CreateThread reported
    CPU *c = calloc(1, sizeof(CPU));
    REG(c, R_ESP) = stack + (1 << 20) - 16;
    REG(c, R_FS_OFFSET) = teb;
    V(c->r + 0x10A0, uint16_t) = 0x27F;
    V(c->r + 0x1094, uint32_t) = 0x1F80;
    tls_callbacks(c, 2);
    // push param + dummy return address, then run the thread function inside rt_enter (so rt_unimpl lands here)
    REG(c, R_ESP) -= 4; U32(REG(c, R_ESP)) = a.param;
    REG(c, R_ESP) -= 4; U32(REG(c, R_ESP)) = 0;
    int r = rt_enter(c, a.start);
    if (r == 1) die("thread %08x jumped to unknown address %08x", a.start, rt_left_at);
    if (r == 2) die("thread %08x: unimplemented %s", a.start, rt_msg);
    pthread_mutex_lock(&lk);
    Obj *o = obj(a.h);
    if (o && o->type == OBJ_THREAD && o->tid == a.tid) { o->signaled = 1; o->exit_code = REG(c, R_EAX); }
    wake(a.h);
    pthread_mutex_unlock(&lk);
    free(c);                            // ponytail: guest stack + TEB leak per thread; the engine keeps a fixed thread pool
    return NULL;
}

static uint32_t spawn(uint32_t start, uint32_t param, uint32_t flags, uint32_t tid_out) {
    uint32_t h = obj_new(OBJ_THREAD);
    uint32_t tid = __atomic_add_fetch(&next_tid, 1, __ATOMIC_SEQ_CST);
    obj(h)->suspended = (flags & 4) != 0;  // CREATE_SUSPENDED
    obj(h)->tid = tid;
    ThreadArgs *a = malloc(sizeof *a);
    *a = (ThreadArgs){start, param, h, tid};
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 16 << 20);   // host stack: lifted code recurses as deep as the guest does
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, thread_main, a)) die("pthread_create failed");
    if (tid_out) U32(tid_out) = tid;
    return h;
}
IMP("KERNEL32.dll", CreateThread) { ret_std(c, spawn(ARG(c, 2), ARG(c, 3), ARG(c, 4), ARG(c, 5)), 6); }
IMP("api-ms-win-crt-runtime-l1-1-0.dll", _beginthreadex) { RET_CDECL(c, spawn(ARG(c, 2), ARG(c, 3), ARG(c, 4), ARG(c, 5))); }
IMP("KERNEL32.dll", ResumeThread) {
    pthread_mutex_lock(&lk);
    Obj *o = obj(ARG(c, 0));
    uint32_t prev = o && o->suspended;
    if (o) o->suspended = 0;
    wake(ARG(c, 0));
    pthread_mutex_unlock(&lk);
    ret_std(c, prev, 1);
}
IMP("KERNEL32.dll", SuspendThread) { ret_std(c, 0, 1); }   // ponytail: can't pause a running pthread; unused at runtime so far

// ---------- winmm timers ----------
// timeSetEvent in event mode (TIME_CALLBACK_EVENT_SET/PULSE): a host thread signals the event every `delay` ms on an
// absolute schedule (no drift). CRI's dummy sound output paces itself this way. Callback-function mode isn't needed yet.
IMP("WINMM.dll", timeGetTime) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    ret_std(c, (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000), 0);
}
typedef struct { uint32_t delay, h, periodic; volatile int alive; } Timer;
static Timer timers[16];
static void *timer_main(void *p) {
    Timer *t = p;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    do {
        next.tv_nsec += (long)t->delay * 1000000L;
        while (next.tv_nsec >= 1000000000L) next.tv_sec++, next.tv_nsec -= 1000000000L;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        if (!t->alive) break;
        pthread_mutex_lock(&lk);
        Obj *o = obj(t->h);
        if (o) { o->signaled = 1; wake(t->h); }
        pthread_mutex_unlock(&lk);
    } while (t->periodic);
    t->alive = 0;
    return NULL;
}
IMP("WINMM.dll", timeSetEvent) {   // (delay, resolution, proc or event handle, user, flags) -> timer id
    uint32_t flags = ARG(c, 4);
    if (!(flags & 0x30)) die("timeSetEvent with a callback function (flags %x) not supported yet", flags);
    for (int i = 0; i < 16; i++) {
        if (timers[i].alive) continue;
        timers[i] = (Timer){ARG(c, 0) ? ARG(c, 0) : 1, ARG(c, 2), flags & 1, 1};
        pthread_t th;
        pthread_create(&th, NULL, timer_main, &timers[i]);
        pthread_detach(th);
        ret_std(c, i + 1, 5);
        return;
    }
    ret_std(c, 0, 5);
}
IMP("WINMM.dll", timeKillEvent) {
    uint32_t id = ARG(c, 0);
    if (id >= 1 && id <= 16) timers[id - 1].alive = 0;
    ret_std(c, 0, 1);   // TIMERR_NOERROR
}
