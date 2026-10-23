// New in BotW Recomp (based on LAS Recomp by Peiikai, MIT).
// Host objects that guest state points to (kernel objects, IPC sessions and services, NVN host objects), for
// checkpoints (runtime/checkpoint.c). With BOTW_RECOMP_CHECKPOINT set they come from one arena, which a checkpoint copies
// and a rewind writes back whole (allocator state included: it lives at the arena's start). Otherwise these are calloc
// and free, as before.
//
// Allocator: size classes of 16 bytes up to 4 KB (free lists), larger blocks first-fit from a free list; a 16-byte
// header before each block holds its size. The arena is reserved once (4 GB) and committed as it grows.
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "platform.h"
#include "rt.h"

#define ARENA_RESERVE (4ull << 30)
#define NCLASS 256  // 16 .. 4096 bytes
#define HDR 16

typedef struct Block { struct Block *next; uint64_t size; } Block;  // a free block (size includes the header)
typedef struct {
    uint64_t top;            // bytes used from the arena's start (this struct first)
    uint64_t committed;
    Block *free_small[NCLASS];
    Block *free_large;       // first fit
    uint64_t live_bytes;
} ArenaHead;

static uint8_t *g_base;  // NULL: the arena is off (calloc / free)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_on = -1;

static bool arena_on(void) {
    if (g_on < 0) {
        g_on = getenv("BOTW_RECOMP_CHECKPOINT") != NULL;
        if (g_on) {
            g_base = (uint8_t *)(uintptr_t)plat_reserve(ARENA_RESERVE);
            if (!g_base) FATAL("checkpoint arena: no address space\n");
            plat_commit((uint64_t)(uintptr_t)g_base, 1 << 20);
            ArenaHead *h = (ArenaHead *)g_base;
            memset(h, 0, sizeof *h);
            h->top = (sizeof *h + 63) & ~63ull;
            h->committed = 1 << 20;
            LOG("[checkpoint] arena at %p\n", (void *)g_base);
        }
    }
    return g_on;
}

static void *raw_take(ArenaHead *h, uint64_t size) {  // from the top (lock held)
    if (h->top + size > ARENA_RESERVE) FATAL("checkpoint arena full (%llu bytes)\n", (unsigned long long)h->top);
    while (h->top + size > h->committed) {
        plat_commit((uint64_t)(uintptr_t)g_base + h->committed, 64 << 20);
        h->committed += 64 << 20;
    }
    void *p = g_base + h->top;
    h->top += size;
    return p;
}

void *rt_obj_calloc(size_t n, size_t size) {
    if (!arena_on()) return calloc(n, size);
    uint64_t want = ((uint64_t)n * size + HDR + 15) & ~15ull;
    pthread_mutex_lock(&g_lock);
    ArenaHead *h = (ArenaHead *)g_base;
    uint8_t *b = NULL;
    if (want <= NCLASS * 16) {
        Block **fl = &h->free_small[want / 16 - 1];
        if (*fl) { b = (uint8_t *)*fl; *fl = (*fl)->next; }
    } else {
        for (Block **p = &h->free_large; *p; p = &(*p)->next)
            if ((*p)->size >= want) {
                b = (uint8_t *)*p;
                want = (*p)->size;  // (the whole block: no splitting)
                *p = (*p)->next;
                break;
            }
    }
    if (!b) b = raw_take(h, want);
    h->live_bytes += want;
    pthread_mutex_unlock(&g_lock);
    memset(b, 0, want);
    ((uint64_t *)b)[0] = want;
    return b + HDR;
}

void rt_obj_free(void *p) {
    if (!p) return;
    if (!arena_on()) { free(p); return; }
    uint8_t *b = (uint8_t *)p - HDR;
    if (b < g_base || b >= g_base + ARENA_RESERVE) { free(p); return; }  // (allocated before: not ours)
    uint64_t size = ((uint64_t *)b)[0];
    pthread_mutex_lock(&g_lock);
    ArenaHead *h = (ArenaHead *)g_base;
    Block *k = (Block *)b;
    k->size = size;
    if (size <= NCLASS * 16) { k->next = h->free_small[size / 16 - 1]; h->free_small[size / 16 - 1] = k; }
    else { k->next = h->free_large; h->free_large = k; }
    h->live_bytes -= size;
    pthread_mutex_unlock(&g_lock);
}

void *rt_obj_realloc(void *p, size_t size) {
    if (!arena_on()) return realloc(p, size);
    if (!p) return rt_obj_calloc(1, size);
    uint64_t old = ((uint64_t *)((uint8_t *)p - HDR))[0] - HDR;
    if (size <= old) return p;
    void *q = rt_obj_calloc(1, size);
    memcpy(q, p, old);
    rt_obj_free(p);
    return q;
}

bool rt_arena_active(void) { return arena_on(); }

// checkpoint.c: the arena's used bytes (allocator state included), to copy out and back
void rt_arena_span(uint8_t **base, uint64_t *used) {
    *base = g_base;
    *used = g_base ? ((ArenaHead *)g_base)->top : 0;
}
void rt_arena_lock(bool on) {
    if (on) pthread_mutex_lock(&g_lock);
    else pthread_mutex_unlock(&g_lock);
}
