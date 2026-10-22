// Debugging: LARECOMP_HEAP_FAIL=<hex guest addr>[,...] wraps those allocator functions (virtual calls reach them
// through the dispatcher, where address overrides apply) and logs every call that returns NULL: the heap object, a
// name found in its first bytes, the size and alignment asked for, and the caller. For finding which game heap runs
// out (Breath of the Wild: sead::ExpHeap::tryAlloc 0x700125ac00, sead::FrameHeap::tryAlloc 0x700125d560).
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../kernel.h"

static const char *heap_name(uint64_t heap) {
    for (uint64_t o = 0x08; o < 0x80; o += 8) {
        uint64_t p = *(uint64_t *)(uintptr_t)(heap + o);
        if (p < GUEST_LO || p >= GUEST_HI) continue;
        const char *s = (const char *)(uintptr_t)p;
        int n = 0;
        while (n < 48 && isprint((unsigned char)s[n])) n++;
        if (n >= 3 && s[n] == 0) return s;
    }
    return "?";
}

static void heapfail_wrap(Cpu *c) {
    uint64_t heap = c->x[0], size = c->x[1], lr = c->x[30];
    int64_t align = (int64_t)(int32_t)c->x[2];
    GuestFn f = dispatch_lookup(c->pc);
    if (!f) FATAL("heapfail: no function at %llx\n", (unsigned long long)c->pc);
    f(c);
    static const char *trace = (const char *)-1;  // LARECOMP_HEAP_TRACE=<substring>: every call on heaps so named
    if (trace == (const char *)-1) trace = getenv("LARECOMP_HEAP_TRACE");
    if (trace && strstr(heap_name(heap), trace)) {
        // which guest thread allocates on each traced heap: a second one is logged once ([heapshare])
        static struct { uint64_t heap, tid; int prio; } owners[4096];
        static int nowners;
        static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
        KThread *t = kernel_current_thread();
        uint64_t tid = t ? t->id : 0;
        int prio = t ? t->prio : -1;
        pthread_mutex_lock(&m);
        int i = 0;
        while (i < nowners && owners[i].heap != heap) i++;
        if (i == nowners && nowners < 4096) owners[nowners++] = (typeof(owners[0])){heap, tid, prio};
        else if (i < nowners && owners[i].tid != tid && owners[i].tid != ~0ull) {
            LOG("[heapshare] %s (heap %llx): thread %llx (prio %d) after thread %llx (prio %d), lr %llx\n",
                heap_name(heap), (unsigned long long)heap, (unsigned long long)tid, prio,
                (unsigned long long)owners[i].tid, owners[i].prio, (unsigned long long)lr);
            owners[i].tid = ~0ull;  // (once per heap)
        }
        pthread_mutex_unlock(&m);
        if (getenv("LARECOMP_HEAP_TRACE_ALL"))
            LOG("[heaptrace] %s size %#llx align %lld -> %llx (lr %llx, thread %llx)\n", heap_name(heap),
                (unsigned long long)size, (long long)align, (unsigned long long)c->x[0], (unsigned long long)lr,
                (unsigned long long)tid);
    }
    if (!c->x[0]) {
        static _Atomic int n;
        if (n < 8) {  // the heap object's first words: its bounds and allocation fronts
            const uint64_t *w = (const uint64_t *)(uintptr_t)heap;
            LOG("[heapfail] heap %llx words:", (unsigned long long)heap);
            for (int i = 0; i < 32; i++) LOG("%s%llx", i % 8 ? " " : "\n   ", (unsigned long long)w[i]);
            LOG("\n");
        }
        if (n++ < 200)
            LOG("[heapfail] %llx failed: heap %llx \"%s\" size %#llx align %lld, caller lr %llx\n",
                (unsigned long long)c->pc, (unsigned long long)heap, heap_name(heap), (unsigned long long)size,
                (long long)align, (unsigned long long)lr);
    }
}

void heapfail_init(void) {
    const char *e = getenv("LARECOMP_HEAP_FAIL");
    for (const char *p = e; p && *p;) {
        uint64_t a = strtoull(p, (char **)&p, 16);
        if (a) hle_override(a, heapfail_wrap, "heapfail");
        while (*p == ',' || *p == ' ') p++;
    }
}
