// Per-core priority scheduling of guest threads (runtime/sched.c).
#pragma once
#include <stdbool.h>

typedef struct KThread KThread;

void sched_init(int default_core);   // (kernel_init) the program's default core, from main.npdm
int sched_core_for(int ideal);       // core for an ideal-core argument (-2 etc.: the default core)
void sched_enter(KThread *t);        // take t's core (blocks); no kernel lock held
void sched_leave(KThread *t);        // give it up
void sched_pause(KThread *t);        // host work that may block (IPC, HLE): core free meanwhile
void sched_resume(KThread *t);       // back, keeping its place among equal priorities
void sched_check(KThread *t);        // safe point: yield to a higher-priority waiter
void ksched_yield(KThread *t);        // svcSleepThread(<= 0)
void sched_moved(KThread *t, int core);
