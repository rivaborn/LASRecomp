// New in BotW Recomp (based on LAS Recomp by Peiikai, MIT).
// Debugging: BOTW_RECOMP_EXPHEAP_CHECK=<hex guest addr>[,...] wraps those sead::ExpHeap methods (virtual calls reach
// them through the dispatcher, where address overrides apply) and checks the heap's free list before and after
// each call: every link inside the guest window, each link's back pointer the one before it, the walk closing at
// the list head. The first break per heap is logged with the method, its caller and whether the list was already
// broken when the call began (something else wrote over it) or the call broke it (the heap's own code).
//
// The layout is sead's ExpHeap as Breath of the Wild 1.6.0 builds it (from its getFreeSize): free-list head at
// heap+0xe0 (prev, next), offset of a block's link at heap+0xf4 (int32). Its methods: tryAlloc 0x700125ac00,
// free 0x700125b670, tryRealloc 0x700125b970, freeAll 0x700125bc00, getFreeSize 0x700125be20,
// getMaxAllocatableSize 0x700125bec0, resizeFront 0x700125b680, resizeBack 0x700125b870.
#include <stdlib.h>

#include "../hle.h"

#define MAX_HEAPS 64
static uint64_t reported[MAX_HEAPS];
static int nreported;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static bool in_guest(uint64_t a) { return a >= GUEST_LO && a < GUEST_HI && !(a & 7); }

// 0: intact; else the address of the first bad link (or 1 if the walk never closed).
static uint64_t check(uint64_t heap) {
    uint64_t head = heap + 0xe0, prev = head, link = *(uint64_t *)(uintptr_t)(head + 8);
    for (int n = 0; n < 1000000; n++) {
        if (link == head) return 0;
        if (!in_guest(link)) return link ? link : 2;
        if (*(uint64_t *)(uintptr_t)link != prev) return link;
        prev = link;
        link = *(uint64_t *)(uintptr_t)(link + 8);
    }
    return 1;
}

static void report(uint64_t heap, uint64_t bad, uint64_t fn, uint64_t lr, const char *when) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nreported; i++)
        if (reported[i] == heap) { pthread_mutex_unlock(&lock); return; }
    if (nreported < MAX_HEAPS) reported[nreported++] = heap;
    pthread_mutex_unlock(&lock);
    LOG("[expheap] heap %llx free list broken %s %llx (caller lr %llx): bad link %llx\n", (unsigned long long)heap,
        when, (unsigned long long)fn, (unsigned long long)lr, (unsigned long long)bad);
}

static void wrap(Cpu *c) {
    uint64_t heap = c->x[0], fn = c->pc, lr = c->x[30];
    uint64_t before = in_guest(heap) ? check(heap) : 0;
    if (before) report(heap, before, fn, lr, "before");
    GuestFn f = dispatch_lookup(fn);
    if (!f) FATAL("expheap: no function at %llx\n", (unsigned long long)fn);
    f(c);
    if (!before && in_guest(heap)) {
        uint64_t after = check(heap);
        if (after) report(heap, after, fn, lr, "by");
    }
}

void expheap_check_init(void) {
    const char *e = getenv("BOTW_RECOMP_EXPHEAP_CHECK");
    for (const char *p = e; p && *p;) {
        uint64_t a = strtoull(p, (char **)&p, 16);
        if (a) {
            hle_override(a, wrap, "expheap_check");
            LOG("[expheap] checking calls of %llx\n", (unsigned long long)a);
        }
        while (*p == ',' || *p == ' ') p++;
    }
}
