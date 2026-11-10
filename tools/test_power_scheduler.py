#!/usr/bin/env python3
"""Actual scheduler drain/admission predicates; host IRQ boundary, no switches.
The guest vfork fixture separately exercises a live dependent child and parent.
"""
from pathlib import Path
import os, shlex, subprocess, tempfile
from test_acpi_boot_interrupts import function
ROOT = Path(__file__).resolve().parents[1]
HEADER = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define TASK_MAX 12
#define TASK_UNUSED 0
#define TASK_RUNNING 1
#define TASK_SLEEPING 2
#define TASK_ZOMBIE 3
struct task { unsigned state, power_role, held_mutexes, no_park_depth;
    struct task *power_creator; unsigned vfork_parent; bool idle; };
static struct task tasks[TASK_MAX];
static unsigned scheduler_lock;
static bool frozen, irq_enabled = true;
static uint64_t spin_lock_irqsave(unsigned *lock) {
    assert(irq_enabled && !*lock); irq_enabled=false; *lock=1; return 0x200;
}
static void spin_unlock_irqrestore(unsigned *lock, uint64_t flags) {
    assert(!irq_enabled && *lock && flags==0x200); *lock=0; irq_enabled=true;
}
static bool power_frozen(void) { return frozen; }
'''
MAIN = r'''
int main(void) {
    struct task *a=&tasks[1], *b=&tasks[9];
    assert(sched_power_drained() && irq_enabled && !scheduler_lock);
    a->state=TASK_SLEEPING; b->state=TASK_RUNNING; frozen=true;
    assert(sched_power_drained()); assert(!power_task_may_run(a));
    a->held_mutexes=2; b->held_mutexes=1;
    assert(!sched_power_drained() && power_task_may_run(a) && power_task_may_run(b));
    a->held_mutexes=0; assert(!sched_power_drained()); b->held_mutexes=0;
    a->no_park_depth=1;
    assert(!sched_power_drained() && power_task_may_run(a)); a->no_park_depth=0;
    a->power_creator=b; assert(!sched_power_drained()); a->power_creator=0;
    a->vfork_parent=42;
    assert(!sched_power_drained() && power_task_may_run(a)); a->vfork_parent=0;
    a->state=TASK_ZOMBIE; a->no_park_depth=1;
    assert(!sched_power_drained()); a->no_park_depth=0;
    a->state=TASK_UNUSED; a->held_mutexes=1;
    assert(sched_power_drained()); a->held_mutexes=0;
    a->state=TASK_SLEEPING; a->power_role=2; a->no_park_depth=1;
    assert(sched_power_drained() && power_task_may_run(a));
    a->power_role=0; a->no_park_depth=0; a->idle=true;
    assert(power_task_may_run(a)); a->idle=false; frozen=false;
    assert(power_task_may_run(a) && irq_enabled && !scheduler_lock);
    puts("PASS actual scheduler drain/admission: sleeping/preempted multi-owner, transaction, unpublished creator, dependent vfork, zombie/unused and worker roles");
}
'''
def main():
    drain = function((ROOT/'kernel/sched/sched.c').read_text(), 'bool sched_power_drained(')
    admission = function((ROOT/'kernel/power.c').read_text(), 'bool power_task_may_run(')
    with tempfile.TemporaryDirectory(prefix='coast-power-scheduler-') as temporary:
        source=Path(temporary)/'test.c'; binary=source.with_suffix('')
        source.write_text(HEADER+drain+admission+MAIN)
        subprocess.run(shlex.split(os.environ.get('HOSTCC','cc'))+[
            '-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer',str(source),'-o',str(binary)],check=True)
        env=os.environ.copy();env.setdefault('ASAN_OPTIONS','detect_leaks=0')
        subprocess.run([str(binary)],check=True,env=env,timeout=10)
if __name__=='__main__': main()
