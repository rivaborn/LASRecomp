// New in BotW Recomp (based on LAS Recomp by Peiikai, MIT).
// Per-core priority scheduling, as Horizon does it: each of the 4 cores runs one guest thread at a time, the
// highest-priority ready thread placed on it (lower number = higher priority; equal priority in arrival order).
// Games rely on it: a thread created at a lower priority on its creator's core doesn't start until the creator
// waits (Breath of the Wild's workers otherwise run ahead of the heaps their creator sets up).
//
// A thread holds its core while it runs guest code. It gives it up while blocked in the kernel (kernel.c kwait,
// SleepThread) and while host code works for it (an IPC request, an HLE function: sched_pause). Preemption: when a
// thread waits for a core held by a lower-priority one, the holder's Cpu.preempt is set, and the holder yields at its
// next function entry (FN_ENTER, cpu.h) or svc. If a holder still keeps the core 5 s (it never enters a function or
// makes an svc), the waiting thread runs anyway, so nothing can deadlock; this is logged.
#include <stdlib.h>

#include "kernel.h"
#include "sched.h"

#define NCORES 4
#define MAX_HOLDERS 8
#define STARVE_NS 5000000000ull  // (only a deadlock breaker)

typedef struct {
    KThread *holders[MAX_HOLDERS];  // holding the core: one, more only through the deadlock breaker
    int nholders;
    uint64_t last_safe;             // a holder's last safe point (plat_now_ns)
    KThread *ready;                 // waiting for the core (unordered; the best is picked)
    pthread_cond_t cond;
} Core;

static Core cores[NCORES];
static pthread_mutex_t sched_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t seq;
static int default_core;
static int disabled = -1;
static _Atomic uint64_t escapes;

void sched_init(int def_core) {
    default_core = def_core >= 0 && def_core < NCORES ? def_core : 0;
    for (int i = 0; i < NCORES; i++) {
        cores[i].nholders = 0;
        cores[i].ready = NULL;
        pthread_cond_init(&cores[i].cond, NULL);
    }
}

int sched_core_for(int ideal) {
    static int one = -1;  // BOTW_RECOMP_ONE_CORE (debugging): every thread on one core, guest code strictly one at a time
    if (one < 0) one = getenv("BOTW_RECOMP_ONE_CORE") != NULL;
    if (one) return default_core;
    return ideal >= 0 && ideal < NCORES ? ideal : default_core;
}

static bool off(void) {
    if (disabled < 0) disabled = getenv("BOTW_RECOMP_NO_SCHED") != NULL;  // debugging: every thread free, as before
    return disabled;
}

static KThread *best(Core *c) {
    KThread *b = NULL;
    for (KThread *t = c->ready; t; t = t->rnext)
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->sched_seq < b->sched_seq)) b = t;
    return b;
}

static void ready_remove(Core *c, KThread *t) {
    for (KThread **p = &c->ready; *p; p = &(*p)->rnext)
        if (*p == t) { *p = t->rnext; return; }
}

// Ask the holders that a waiting thread outranks to yield (sched_lock held).
static void request_preemption(Core *c) {
    KThread *b = best(c);
    for (int i = 0; i < c->nholders; i++) {
        KThread *h = c->holders[i];
        h->cpu.preempt = b && b->prio < h->prio;
    }
}

// Take t's core; blocks until t is the best ready thread there and the core is free. Caller holds no kernel lock.
static void enter(KThread *t, bool keep_place) {
    if (off() || t->on_core) return;
    pthread_mutex_lock(&sched_lock);
    Core *c = &cores[t->core];
    if (!keep_place || !t->sched_seq) t->sched_seq = ++seq;
    t->rnext = c->ready;
    c->ready = t;
    request_preemption(c);
    for (;;) {
        if (g_rt_unwind) break;  // reboot: run to the next unwind point
        if (best(c) == t) {
            if (c->nholders == 0) break;
            if (plat_now_ns() - c->last_safe > STARVE_NS && c->nholders < MAX_HOLDERS) {
                uint64_t n = ++escapes;
                if ((n & (n - 1)) == 0) {
                    KThread *h = c->holders[0];
                    LOG("[sched] core %d: thread %llx (prio %d, pc %llx lr %llx) kept the core 5 s without yielding; "
                        "thread %llx (prio %d) runs alongside (%llu)\n", t->core, (unsigned long long)h->id, h->prio,
                        (unsigned long long)h->cpu.pc, (unsigned long long)h->cpu.x[30], (unsigned long long)t->id,
                        t->prio, (unsigned long long)n);
                }
                break;
            }
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20000000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&c->cond, &sched_lock, &ts);
    }
    ready_remove(c, t);
    c->holders[c->nholders++] = t;
    c->last_safe = plat_now_ns();
    t->on_core = true;
    t->held_core = t->core;
    t->cpu.preempt = 0;
    request_preemption(c);
    pthread_cond_broadcast(&c->cond);  // (the next best re-evaluates)
    pthread_mutex_unlock(&sched_lock);
}

void sched_enter(KThread *t) { enter(t, false); }

// Give t's core up (t blocks, sleeps, ends or moves).
void sched_leave(KThread *t) {
    if (off() || !t->on_core) return;
    pthread_mutex_lock(&sched_lock);
    Core *c = &cores[t->held_core];
    for (int i = 0; i < c->nholders; i++)
        if (c->holders[i] == t) { c->holders[i] = c->holders[--c->nholders]; break; }
    t->on_core = false;
    t->cpu.preempt = 0;
    pthread_cond_broadcast(&c->cond);
    pthread_mutex_unlock(&sched_lock);
}

// Safe point (svc, or a function entry with Cpu.preempt set): step aside if a higher-priority thread waits here.
void sched_check(KThread *t) {
    if (off() || !t->on_core) return;
    pthread_mutex_lock(&sched_lock);
    Core *c = &cores[t->held_core];
    c->last_safe = plat_now_ns();
    KThread *b = best(c);
    bool preempt = b && b->prio < t->prio;
    if (!preempt) t->cpu.preempt = 0;
    pthread_mutex_unlock(&sched_lock);
    if (preempt) {
        sched_leave(t);
        enter(t, true);  // (preempted, not blocked: it keeps its place among its priority)
    }
}

// FN_ENTER (cpu.h) when Cpu.preempt is set.
void rt_preempt(Cpu *c) {
    KThread *t = kernel_current_thread();
    if (t) sched_check(t);
    else c->preempt = 0;
}

// Host work that may block (an IPC request served by the runtime, an HLE function waiting on the GPU or vsync):
// on a Switch the thread would be blocked in the kernel or another process, and the core free. It keeps its place
// among equal priorities when it comes back.
void sched_pause(KThread *t) { sched_leave(t); }
void sched_resume(KThread *t) { enter(t, true); }

// svcSleepThread(0 / -1 / -2): let ready threads of the same or higher priority on this core run first.
void ksched_yield(KThread *t) {
    if (off() || !t->on_core) return;
    pthread_mutex_lock(&sched_lock);
    KThread *b = best(&cores[t->held_core]);
    bool give = b && b->prio <= t->prio;
    pthread_mutex_unlock(&sched_lock);
    if (give) {
        sched_leave(t);
        sched_enter(t);  // (a new sequence number: behind the equal-priority ones)
    }
}

// SetThreadCoreMask of the running thread: re-queue it where it now belongs.
void sched_moved(KThread *t, int core) {
    if (off()) { t->core = core; return; }
    bool was = t->on_core;
    if (was) sched_leave(t);
    t->core = core;
    if (was) sched_enter(t);
}
