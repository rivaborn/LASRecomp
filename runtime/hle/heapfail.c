// Debugging: LARECOMP_HEAP_FAIL=<hex guest addr>[,...] wraps those allocator functions (virtual calls reach them
// through the dispatcher, where address overrides apply) and logs every call that returns NULL: the heap object, a
// name found in its first bytes, the size and alignment asked for, and the caller. For finding which game heap runs
// out (Breath of the Wild: sead::ExpHeap::tryAlloc 0x700125ac00, sead::FrameHeap::tryAlloc 0x700125d560).
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../kernel.h"

typedef struct { uint64_t addr, size, lr; } TraceEntry;
static TraceEntry g_trace[4096];
static int g_trace_n;

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
    uint64_t heap = c->x[0], size = c->x[1], lr = c->x[30], fn_addr = c->pc;
    int64_t align = (int64_t)(int32_t)c->x[2];
    static uint64_t regs_lr = ~0ull;  // LARECOMP_HEAP_REGS=<hex lr>: the callee-saved registers of calls from there
    if (regs_lr == ~0ull) {
        const char *e = getenv("LARECOMP_HEAP_REGS");
        regs_lr = e ? strtoull(e, NULL, 16) : 0;
    }
    if (regs_lr && lr == regs_lr)
        LOG("[heapregs] %s size %#llx: x19 %llx x20 %llx x21 %llx x22 %llx x23 %llx x24 %llx x25 %llx x26 %llx "
            "x27 %llx x28 %llx [main+0x2cc2918] %llx\n", heap_name(heap), (unsigned long long)size, (unsigned long long)c->x[19],
            (unsigned long long)c->x[20], (unsigned long long)c->x[21], (unsigned long long)c->x[22],
            (unsigned long long)c->x[23], (unsigned long long)c->x[24], (unsigned long long)c->x[25],
            (unsigned long long)c->x[26], (unsigned long long)c->x[27], (unsigned long long)c->x[28],
            (unsigned long long)*(uint64_t *)(uintptr_t)0x7002cc2918ull);
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
        {   // remember this heap's allocations: on a failure they are listed with their first bytes
            static uint64_t last_heap;
            if (heap != last_heap) { last_heap = heap; g_trace_n = 0; }
            if (c->x[0] && g_trace_n < (int)(sizeof g_trace / sizeof *g_trace))
                g_trace[g_trace_n++] = (TraceEntry){c->x[0], size, lr};
        }
        if (getenv("LARECOMP_HEAP_TRACE_ALL"))
            LOG("[heaptrace] %s fn %llx size %#llx align %lld -> %llx (lr %llx, thread %llx)\n", heap_name(heap),
                (unsigned long long)fn_addr, (unsigned long long)size, (long long)align, (unsigned long long)c->x[0],
                (unsigned long long)lr, (unsigned long long)tid);
    }
    if (!c->x[0]) {
        static _Atomic int n;
        if (n < 8) {  // the heap object's first words: its bounds and allocation fronts
            const uint64_t *w = (const uint64_t *)(uintptr_t)heap;
            LOG("[heapfail] heap %llx words:", (unsigned long long)heap);
            for (int i = 0; i < 32; i++) LOG("%s%llx", i % 8 ? " " : "\n   ", (unsigned long long)w[i]);
            LOG("\n");
            // the heap's first allocation (a model's own object, for resource heaps): words, pointers inside the
            // heap as start+offset (comparable with another run's)
            const uint64_t start = w[3], obj = start + 0x60, *o = (const uint64_t *)(uintptr_t)obj;
            for (int k = 0; k < g_trace_n; k++) {  // this heap's earlier allocations (LARECOMP_HEAP_TRACE)
                const uint8_t *b = (const uint8_t *)(uintptr_t)g_trace[k].addr;
                LOG("[heapfail]   alloc %llx size %#llx lr %llx: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                    (unsigned long long)g_trace[k].addr, (unsigned long long)g_trace[k].size,
                    (unsigned long long)g_trace[k].lr, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
                    b[10], b[11]);
            }
            LOG("[heapfail] object %llx (start+0x60):", (unsigned long long)obj);
            for (int i = 0; i < 0x5f; i++) {
                uint64_t v = o[i];
                if (v >= start && v < w[29]) LOG("%s[%x]=S+%llx", i % 6 ? " " : "\n   ", i, (unsigned long long)(v - start));
                else LOG("%s[%x]=%llx", i % 6 ? " " : "\n   ", i, (unsigned long long)v);
            }
            LOG("\n");
        }
        if (n++ < 200)
            LOG("[heapfail] %llx failed: heap %llx \"%s\" size %#llx align %lld, caller lr %llx\n",
                (unsigned long long)c->pc, (unsigned long long)heap, heap_name(heap), (unsigned long long)size,
                (long long)align, (unsigned long long)lr);
    }
}

// LARECOMP_MEMCMP_TEST: call the guest's own memcmp (recompiled from the SDK) on known inputs and log any result
// whose sign differs from the host's.
static void memcmp_test(void) {
    uint64_t fn = loader_find_symbol("memcmp");
    if (!fn) { LOG("[memcmp-test] no memcmp symbol\n"); return; }
    uint8_t *mem = hle_alloc(0x2000);
    uint64_t stack = (uint64_t)(uintptr_t)hle_alloc(0x20000) + 0x20000;
    static const char *const pairs[][2] = {
        {"textures.bntx", "textures.bntx"}, {"textures.bntx", "Mt_Body_Mat00"}, {"textures.bntx", "textures.bnty"},
        {"aaaaaaaaaaaaa", "aaaaaaaaaaaab"}, {"Xaaaaaaaaaaaa", "aaaaaaaaaaaaa"}, {"abcdefghijklm", "abcdefgHijklm"},
        {"0123456789abcdef0123456789", "0123456789abcdef0123456789"}, {"0123456789abcdef0123456789", "0123456789abcdef012345678A"},
    };
    int bad = 0, total = 0;
    for (unsigned p = 0; p < sizeof pairs / sizeof *pairs; p++)
        for (int oa = 0; oa < 16; oa++)
            for (int ob = 0; ob < 16; ob += 5) {
                const char *s1 = pairs[p][0], *s2 = pairs[p][1];
                size_t n = strlen(s1);
                uint8_t *a = mem + 0x100 + oa, *b = mem + 0x1000 + ob;
                memcpy(a, s1, n);
                memcpy(b, s2, n);
                for (size_t len = 0; len <= n; len++) {
                    Cpu cc;
                    memset(&cc, 0, sizeof cc);
                    cc.sp = stack;
                    cc.excl_addr = ~0ull;
                    cc.x[0] = (uint64_t)(uintptr_t)a;
                    cc.x[1] = (uint64_t)(uintptr_t)b;
                    cc.x[2] = len;
                    guest_call(&cc, fn);
                    int got = (int)(int32_t)cc.x[0], want = memcmp(a, b, len);
                    total++;
                    if ((got < 0) != (want < 0) || (got > 0) != (want > 0)) {
                        if (bad++ < 20)
                            LOG("[memcmp-test] \"%s\" vs \"%s\" len %zu, offsets %d/%d: guest %d, host %d\n", s1, s2, len,
                                oa, ob, got, want);
                    }
                }
            }
    LOG("[memcmp-test] %d of %d calls wrong\n", bad, total);
}

void heapfail_init(void) {
    if (getenv("LARECOMP_MEMCMP_TEST")) memcmp_test();
    const char *e = getenv("LARECOMP_HEAP_FAIL");
    for (const char *p = e; p && *p;) {
        uint64_t a = strtoull(p, (char **)&p, 16);
        if (a) hle_override(a, heapfail_wrap, "heapfail");
        while (*p == ',' || *p == ' ') p++;
    }
}
