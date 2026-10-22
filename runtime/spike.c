// SPIKE (branch spike/checkpoint, not for merging): measurements for a checkpoint/restore feasibility report.
//   LARECOMP_PARK_AT=<frame>      at that presented frame, park every guest thread at a safe point, measure, release
//   LARECOMP_PARK_REWIND=<frames> also: each parked thread saves its host stack + a setjmp point; <frames> later all
//                                 park again, each thread writes its old stack back and longjmps to the saved point
// Safe points: rt_unwind_point() (SVC entry/exit, HLE calls; the reboot's hook points) and kernel waits (kwait).
#ifdef _WIN32
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "kernel.h"
#include "rt.h"
#include "sched.h"

int kernel_thread_list(KThread **out, int max);
KThread *kernel_current_thread(void);
#define t_cur kernel_current_thread()

volatile int g_park_req;          // 1: park at the next safe point
static volatile int g_rewind_req; // 1: parked threads restore their saved stack and longjmp back
static pthread_mutex_t pk_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pk_cond = PTHREAD_COND_INITIALIZER, pk_done = PTHREAD_COND_INITIALIZER;
static int n_parked;

#define MAXT 512
typedef struct {
    KThread *t;
    bool saved, in_wait;
    CONTEXT ctx;          // full register context (RtlCaptureContext / RtlRestoreContext: no SEH unwind)
    volatile int rewinding;
    uint8_t *stack;       // copy of [lo, hi)
    uintptr_t lo, hi;
    int rewound;
} ThreadState;
static ThreadState ts[MAXT];
static __thread int my_slot = -1;
static int n_slots;

static int slot_of(KThread *t) {  // pk_lock held
    for (int i = 0; i < n_slots; i++) if (ts[i].t == t) return i;
    if (n_slots == MAXT) return -1;
    ts[n_slots].t = t;
    return n_slots++;
}

void spike_kwait_enter(void) { if (t_cur) t_cur->spike_in_wait = true; }
void spike_kwait_leave(void) { if (t_cur) t_cur->spike_in_wait = false; }

// Copy the saved stack back from below it (this frame must not lie inside the region it overwrites), then jump.
static void __attribute__((noinline)) restore_from_below(ThreadState *s) {
    fprintf(stderr, "[spike] thread %llu: copying %llu bytes of stack back, then longjmp\n",
            (unsigned long long)s->t->id, (unsigned long long)(s->hi - s->lo));
    memcpy((void *)s->lo, s->stack, s->hi - s->lo);
    s->rewound++;
    s->rewinding = 1;
    RtlRestoreContext(&s->ctx, NULL);
}

static void __attribute__((noinline)) rewind_now(ThreadState *s) {
    volatile uint8_t *pad = __builtin_alloca(s->hi - s->lo + 65536);  // move this thread's stack pointer below lo
    pad[0] = 0;
    if ((uintptr_t)pad + 4096 > s->lo) { LOG("[spike] rewind: frame not below the saved stack\n"); return; }
    restore_from_below(s);
}

static void park_here(void) {
    KThread *t = t_cur;
    pthread_mutex_lock(&pk_lock);
    int i = slot_of(t);
    ThreadState *s = i >= 0 ? &ts[i] : NULL;
    my_slot = i;
    bool want_save = s && !s->saved && getenv("LARECOMP_PARK_REWIND");
    pthread_mutex_unlock(&pk_lock);
    if (want_save) {
        s->rewinding = 0;
        RtlCaptureContext(&s->ctx);  // a rewind resumes here, with the old stack contents and registers
        if (s->rewinding) {
            s->rewinding = 0;
            LOG("[spike] thread %llu rewound to its saved point\n", (unsigned long long)t->id);
            return;
        }
        ULONG_PTR low, high;
        GetCurrentThreadStackLimits(&low, &high);
        s->lo = (uintptr_t)__builtin_frame_address(0) - 4096;  // (a little below this frame: setjmp's own)
        s->hi = (uintptr_t)high;
        s->stack = malloc(s->hi - s->lo);
        memcpy(s->stack, (void *)s->lo, s->hi - s->lo);
        s->saved = true;
    }
    sched_leave(t);  // (parked like a kernel wait: the core goes to the next thread, which must reach its own point)
    pthread_mutex_lock(&pk_lock);
    n_parked++;
    pthread_cond_broadcast(&pk_done);
    while (g_park_req) pthread_cond_wait(&pk_cond, &pk_lock);
    n_parked--;
    bool rewind = g_rewind_req && s && s->saved;
    pthread_mutex_unlock(&pk_lock);
    sched_enter(t);
    if (rewind) {
        fprintf(stderr, "[spike] thread %llu: rewinding (saved %llx..%llx, now at %p)\n", (unsigned long long)t->id,
                (unsigned long long)s->lo, (unsigned long long)s->hi, __builtin_frame_address(0));
        rewind_now(s);
    }
}

void spike_safe_point(void) {
    if (__builtin_expect(g_park_req, 0) && t_cur) park_here();
}

// ---- measurements at the parked point
static void measure_guest(void) {
    uint64_t committed = 0, zero_pages = 0, regions = 0;
    uint64_t t0 = plat_now_ns();
    for (uint64_t a = GUEST_LO; a < GUEST_HI;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
        uint64_t end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            regions++;
            committed += mi.RegionSize;
            for (uint64_t p = (uint64_t)(uintptr_t)mi.BaseAddress; p < end; p += 4096) {
                const uint64_t *q = (const uint64_t *)(uintptr_t)p;
                int z = 1;
                for (int k = 0; k < 512 && z; k++) z = q[k] == 0;
                zero_pages += z;
            }
        }
        a = end;
    }
    uint64_t scan = plat_now_ns() - t0;
    // copy speed: the non-zero pages into one buffer (what a dump has to move)
    uint8_t *buf = malloc((size_t)(committed - zero_pages * 4096) + 4096);
    t0 = plat_now_ns();
    uint64_t copied = 0;
    if (buf)
        for (uint64_t a = GUEST_LO; a < GUEST_HI;) {
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
            uint64_t end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
            if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                for (uint64_t p = (uint64_t)(uintptr_t)mi.BaseAddress; p < end; p += 4096) {
                    const uint64_t *q = (const uint64_t *)(uintptr_t)p;
                    int z = 1;
                    for (int k = 0; k < 512 && z; k++) z = q[k] == 0;
                    if (!z) { memcpy(buf + copied, q, 4096); copied += 4096; }
                }
            a = end;
        }
    uint64_t copy = plat_now_ns() - t0;
    free(buf);
    LOG("[spike] guest window: %llu committed regions, %.1f MB committed, %.1f MB of all-zero pages, %.1f MB to "
        "dump; scan %.0f ms, copy %.0f ms\n", (unsigned long long)regions, committed / 1048576.0,
        zero_pages * 4096 / 1048576.0, copied / 1048576.0, scan / 1e6, copy / 1e6);
}

static void classify_stacks(void) {
    // what the saved host stacks point to: a restore in another process would have to fix every host pointer
    HMODULE exe = GetModuleHandleA(NULL), game = GetModuleHandleA("lasgame.dll");
    uint64_t n_words = 0, guest = 0, heap = 0, module = 0, stackp = 0, small = 0, other = 0;
    for (int i = 0; i < n_slots; i++) {
        ThreadState *s = &ts[i];
        if (!s->saved) continue;
        for (uintptr_t o = 0; o + 8 <= s->hi - s->lo; o += 8) {
            uint64_t v;
            memcpy(&v, s->stack + o, 8);
            n_words++;
            if (v < 0x10000) { small++; continue; }
            if (v >= GUEST_LO && v < GUEST_HI) { guest++; continue; }
            if (v >= s->lo && v < s->hi) { stackp++; continue; }
            HMODULE m = NULL;
            MEMORY_BASIC_INFORMATION mi;
            if (VirtualQuery((void *)(uintptr_t)v, &mi, sizeof mi) && mi.State == MEM_COMMIT) {
                if (mi.Type == MEM_IMAGE) { module++; (void)m; (void)exe; (void)game; }
                else if (mi.Type == MEM_PRIVATE) heap++;
                else other++;
            } else {
                other++;
            }
        }
    }
    LOG("[spike] saved host stacks: %llu words: %llu small values, %llu guest addresses, %llu into the same stack, "
        "%llu into modules (code/data), %llu into private memory (heap, other stacks), %llu other\n",
        (unsigned long long)n_words, (unsigned long long)small, (unsigned long long)guest, (unsigned long long)stackp,
        (unsigned long long)module, (unsigned long long)heap, (unsigned long long)other);
}

// Park everything; returns the number of threads that did not park (they are listed). Called on a guest thread
// at a safe point (present), which counts as parked.
static int park_all(double timeout_s, uint64_t *ms_out) {
    KThread *th[MAXT];
    uint64_t t0 = plat_now_ns();
    if (t_cur) sched_leave(t_cur);  // (the coordinator gives up its core too while it waits)
    pthread_mutex_lock(&pk_lock);
    g_park_req = 1;
    int n = 0, missing = 0;
    for (;;) {
        n = kernel_thread_list(th, MAXT);
        int waiting = 0;
        for (int i = 0; i < n; i++) waiting += th[i]->spike_in_wait && th[i] != t_cur;
        missing = n - 1 - n_parked - waiting;
        if (missing <= 0 || plat_now_ns() - t0 > (uint64_t)(timeout_s * 1e9)) break;
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += 2000000;
        if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&pk_done, &pk_lock, &dl);
    }
    *ms_out = (plat_now_ns() - t0) / 1000000;
    int parked = n_parked;
    pthread_mutex_unlock(&pk_lock);
    if (t_cur) sched_enter(t_cur);
    LOG("[spike] park: %d threads, %d at a safe point, %d in kernel waits, %d not parked after %llu ms\n", n, parked,
        n - 1 - parked - (missing > 0 ? missing : 0), missing > 0 ? missing : 0, (unsigned long long)*ms_out);
    if (missing > 0)
        for (int i = 0; i < n; i++) {
            bool p = false;
            for (int k = 0; k < n_slots; k++) p |= ts[k].t == th[i] && ts[k].saved;
            if (th[i] != t_cur && !th[i]->spike_in_wait)
                LOG("[spike]   running: thread %llu prio %d core %d last pc %llx lr %llx%s\n", (unsigned long long)th[i]->id,
                    th[i]->prio, th[i]->core, (unsigned long long)th[i]->cpu.pc, (unsigned long long)th[i]->cpu.x[30],
                    p ? "" : "");
        }
    return missing > 0 ? missing : 0;
}

static void release_all(bool rewind) {
    pthread_mutex_lock(&pk_lock);
    g_rewind_req = rewind;
    g_park_req = 0;
    pthread_cond_broadcast(&pk_cond);
    pthread_mutex_unlock(&pk_lock);
}

// device.c, after each present (on the presenting guest thread, an HLE call: a safe point itself)
void spike_frame(uint64_t frame) {
    static long at = -1, rewind_after = -1;
    if (at < 0) {
        at = getenv("LARECOMP_PARK_AT") ? atol(getenv("LARECOMP_PARK_AT")) : 0;
        rewind_after = getenv("LARECOMP_PARK_REWIND") ? atol(getenv("LARECOMP_PARK_REWIND")) : 0;
    }
    if (!at) return;
    uint64_t ms;
    if ((long)frame == at) {
        park_all(10, &ms);
        measure_guest();
        if (rewind_after) {
            // (each parked thread saved its stack in park_here; this one is the coordinator and is not rewound)
            classify_stacks();
        }
        release_all(false);
        LOG("[spike] released at frame %llu\n", (unsigned long long)frame);
    } else if (rewind_after && (long)frame == at + rewind_after) {
        park_all(10, &ms);
        int saved = 0;
        for (int i = 0; i < n_slots; i++) saved += ts[i].saved;
        LOG("[spike] rewinding %d threads to frame %ld's park point (guest memory NOT restored: mechanism test)\n",
            saved, at);
        release_all(true);
    } else if (rewind_after && (long)frame == at + rewind_after + 120) {
        int rewound = 0;
        for (int i = 0; i < n_slots; i++) rewound += ts[i].rewound;
        LOG("[spike] 120 frames after the rewind: %d threads rewound, the game still presents frames\n", rewound);
    }
}
#else
void spike_safe_point(void) {}
void spike_frame(uint64_t frame) { (void)frame; }
void spike_kwait_enter(void) {}
void spike_kwait_leave(void) {}
#endif
