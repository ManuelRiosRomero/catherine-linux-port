// Guest heap: every malloc/HeapAlloc must return a 32-bit guest address, so we manage our own arena.
// ponytail: power-of-two size classes with per-class free lists (up to 2x waste, never returns memory);
// swap in a real allocator (dlmalloc mspace over the arena) if memory use or fragmentation becomes a problem.
#include "host.h"
#include <execinfo.h>
#include <pthread.h>

#define HDR 16                                  // header keeps user blocks 16-byte aligned (SSE-safe)
static uint32_t bump = G_HEAP_LO, free_list[32];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// Debug: CATH_HEAPWATCH=<guest hex address> prints a host backtrace whenever the block holding it is handed out or freed
static uint32_t heap_w = 1;
static void heap_watch(const char *what, uint32_t p, int k) {
    if (heap_w == 1) heap_w = getenv("CATH_HEAPWATCH") ? (uint32_t)strtoul(getenv("CATH_HEAPWATCH"), NULL, 16) : 0;
    if (!heap_w || heap_w < p || heap_w >= p + (1u << k)) return;
    void *bt[32];
    fprintf(stderr, "[heapwatch] %s %08x (class %d)\n", what, p, k);
    backtrace_symbols_fd(bt, backtrace(bt, 32), 2);
}

static int size_class(uint32_t n) {
    int k = 4;                                  // smallest block: 16 bytes of payload
    while ((1u << k) < n) k++;
    return k;
}

uint32_t gmalloc(uint32_t n) {
    if (n > 0x7FFFFFF0u) { fprintf(stderr, "[heap] refused %u bytes (arena used %08x)\n", n, bump); return 0; }
    int k = size_class(n ? n : 1);
    pthread_mutex_lock(&lock);
    uint32_t p = free_list[k];
    if (p) {
        free_list[k] = U32(p);                  // free blocks store the next pointer in their payload
    } else {
        uint64_t end = (uint64_t)bump + HDR + (1ull << k);
        if (end > G_HEAP_HI) {
            pthread_mutex_unlock(&lock);
            fprintf(stderr, "[heap] arena full: %u bytes refused (arena used %08x)\n", n, bump);
            return 0;
        }
        p = bump + HDR;
        bump = (uint32_t)end;
    }
    pthread_mutex_unlock(&lock);
    U32(p - HDR) = k;
    U32(p - HDR + 4) = n;                       // requested size, for HeapSize/_msize
    heap_watch("alloc", p, k);
    return p;
}

void gfree(uint32_t p) {
    if (!p) return;
    int k = U32(p - HDR);
    if (k & 0x100) die("double free of guest block %08x", p);
    heap_watch("free", p, k);
    U32(p - HDR) = k | 0x100;                   // freed mark; gmalloc rewrites the header
    pthread_mutex_lock(&lock);
    U32(p) = free_list[k];
    free_list[k] = p;
    pthread_mutex_unlock(&lock);
}

uint32_t gsize(uint32_t p) { return U32(p - HDR + 4); }

uint32_t grealloc(uint32_t p, uint32_t n) {
    if (!p) return gmalloc(n);
    if (!n) { gfree(p); return 0; }
    if (n <= (1u << U32(p - HDR))) { U32(p - HDR + 4) = n; return p; }
    uint32_t q = gmalloc(n);
    if (q) { memcpy(GM(q), GM(p), gsize(p)); gfree(p); }
    return q;
}
