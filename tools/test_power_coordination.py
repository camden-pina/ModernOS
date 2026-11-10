#!/usr/bin/env python3
"""Actual coordinator/mutex C, ASan+UBSan; IRQ/IPI/scheduler/firmware/device mocks.
Host evidence does not establish APIC, interrupt register or hardware behavior.
"""
from pathlib import Path
import os, shlex, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
HEADER=r'''
#ifndef TEST_KERNEL_H
#define TEST_KERNEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <uapi/abi.h>
#define CONFIG_USERNS_TEST 1
#define min(a,b) ((a)<(b)?(a):(b))
typedef struct mutex mutex_t;
struct task_context { unsigned web_role; };
struct task { unsigned pid,held_mutexes,no_park_depth,power_role,vfork_parent;
 bool user,idle,browser_sandbox; struct task_context *context;
 mutex_t *waiting_mutex;
 void (*entry)(void *); void *argument; };
struct cpu_local { unsigned id,lapic_id,held_mutexes,no_park_depth,irq_depth;
 uintptr_t handoff; bool online; struct task *current;
 uint64_t park_seen_epoch,park_ack_epoch,resume_ack_epoch,commit_ack_epoch; };
typedef struct { unsigned value; } spinlock_t;
struct mutex { unsigned value; struct task *owner; unsigned owner_cpu; bool counted; };
uint64_t irq_save(void); void irq_restore(uint64_t);
static inline uint64_t spin_lock_irqsave(spinlock_t *l) {
 uint64_t f=irq_save(); while(__atomic_exchange_n(&l->value,1,__ATOMIC_ACQUIRE)){} return f; }
static inline void spin_unlock_irqrestore(spinlock_t *l,uint64_t f) {
 __atomic_store_n(&l->value,0,__ATOMIC_RELEASE); irq_restore(f); }
void mutex_lock(mutex_t *); void mutex_unlock(mutex_t *);
struct cpu_local *this_cpu(void); bool sched_ready(void); void sched_sleep(uint64_t);
unsigned sched_cpu_count(void); struct cpu_local *sched_cpu(unsigned);
bool sched_power_drained(void); uint64_t tsc_uptime_us(void);
struct task *task_allocate(const char *,bool,unsigned,int); void task_publish(struct task *);
bool apic_send_ipi(uint32_t,uint8_t);
void panic(const char *,...) __attribute__((noreturn));
int printk(const char *,...);
#define pr_info(...) printk(__VA_ARGS__)
#define pr_warn(...) printk(__VA_ARGS__)
#define pr_err(...) printk(__VA_ARGS__)
#define pr_notice(...) printk(__VA_ARGS__)
extern mutex_t vfs_mutex;
bool vfs_power_ready(void); int vfs_sync(void);
bool block_power_ready(void); int block_power_flush(void); int block_power_stop(void);
bool audio_power_ready(void); int audio_power_stop(void);
bool network_power_ready(void); int network_power_stop(void);
bool kernel_usb_power_ready(void); int kernel_usb_power_stop(void);
void host_halt(void) __attribute__((noreturn)); void host_cli(void);
#endif
'''
SOURCE=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <setjmp.h>
#include "test_kernel.h"
static _Thread_local bool host_if=true;
static _Thread_local unsigned cpu_id;
static struct cpu_local tc[4]; static struct task tt[8]; static struct task_context ctx[8];
static unsigned count=4;
static bool ready=true,drained=true,tel_idle=true,osl_idle=true,dev_ok=true,backend=true,vfs_ok=true;
static int flush_error,stop_error; static unsigned preps,masks,thaws,flushes,stops,finals;
static unsigned bad_prep,bad_mask,bad_restore,probes; static bool omit_ack,ipi_fail,release_unresponsive,late_ipi;
static pthread_t threads[4]; static jmp_buf terminal_jump,panic_jump;
static bool expect_panic; static uint64_t now;
#include "POWER_SOURCE"
struct boot_info boot_info;
/* Only instrument IRQ arrival precisely after exchange/release; actual
   primitives, accounting and software park loop are production code. */
static unsigned exchange_probe(unsigned *p,unsigned v,int order) {
 unsigned old=__atomic_exchange_n(p,v,order); if(!old){assert(!host_if);probes++;} return old; }
#define __atomic_exchange_n exchange_probe
/* Production code also stores waiting_mutex pointers. Instrument only the
   unsigned lock release, preserving the builtin's pointer/scalar types. */
#define __atomic_store_n(p,v,order) do { \
 __auto_type probe_p=(p); __typeof__(*probe_p) probe_v=(v); \
 __atomic_store_n(probe_p,probe_v,order); \
 if(__builtin_types_compatible_p(__typeof__(probe_p),unsigned *)) {assert(!host_if);probes++;} \
} while(0)
#include "SYNC_SOURCE"
#undef __atomic_exchange_n
#undef __atomic_store_n
mutex_t vfs_mutex;
uint64_t irq_save(void){bool f=host_if;host_if=false;return f?0x200:0;}
void irq_restore(uint64_t f){if(f&0x200)host_if=true;}
void host_cli(void){host_if=false;} void host_halt(void){longjmp(terminal_jump,1);}
struct cpu_local *this_cpu(void){return &tc[cpu_id];}
bool sched_ready(void){return ready;} unsigned sched_cpu_count(void){return count;}
struct cpu_local *sched_cpu(unsigned i){return i<count?&tc[i]:NULL;}
bool sched_power_drained(void){return drained;}
uint64_t tsc_uptime_us(void){return __atomic_load_n(&now,__ATOMIC_RELAXED);}
void sched_sleep(uint64_t us){
 __atomic_add_fetch(&now,us,__ATOMIC_RELAXED);
 if(command_phase(__atomic_load_n(&command,__ATOMIC_ACQUIRE))==PHASE_RELEASE) {
  /* Simulated timer safe points deliver the production RELEASE observer.
     Real parked helper threads alone acknowledge their own return path. */
  unsigned original=cpu_id; bool old_if=host_if;
  for(unsigned i=1;i<count;i++) if(!threads[i] && !(release_unresponsive&&i==3)) {
   cpu_id=i;host_if=false;power_ap_checkpoint();
  }
  cpu_id=original;host_if=old_if;
 }
 struct timespec d={0,10000};nanosleep(&d,NULL);
}
static void *park(void *arg){cpu_id=(uintptr_t)arg;host_if=false;power_ap_checkpoint();return NULL;}
static void *late_park(void *arg){
 while(command_phase(__atomic_load_n(&command,__ATOMIC_ACQUIRE))!=PHASE_RELEASE)usleep(10);
 return park(arg);
}
bool apic_send_ipi(uint32_t d,uint8_t v){assert(v==0xe1);if(ipi_fail)return false;
 if(omit_ack&&d==3){
  if(late_ipi)assert(!pthread_create(&threads[d],NULL,late_park,(void *)(uintptr_t)d));
  return true;
 }
 assert(!pthread_create(&threads[d],NULL,park,(void *)(uintptr_t)d));return true;}
struct task *task_allocate(const char *n,bool u,unsigned p,int c){(void)n;(void)u;(void)p;(void)c;return &tt[7];}
void task_publish(struct task *t){(void)t;}
void panic(const char *s,...){if(expect_panic)longjmp(panic_jump,1);fprintf(stderr,"panic %s\n",s);abort();}
int printk(const char *s,...){(void)s;return 0;}
bool acpi_power_available(unsigned a){return backend&&(a==1||a==2);}
uint32_t acpi_power_prepare(unsigned a){assert(host_if&&!cpu_id&&power_frozen()&&tel_idle&&a==action);preps++;return bad_prep;}
uint32_t acpi_power_disable_gpes(void){assert(host_if);masks++;return bad_mask;}
uint32_t acpi_power_restore_gpes(void){assert(host_if);thaws++;return bad_restore;}
uint32_t acpi_power_enter(unsigned a){assert(!host_if&&!cpu_id&&a==action&&cpus_ack(PHASE_COMMIT));assert(flushes&&stops==4);finals++;longjmp(terminal_jump,2);}
size_t acpi_power_info(char *o,size_t c){return snprintf(o,c,"firmware\n");}
bool acpi_os_power_idle(void){return osl_idle;} bool acpi_os_power_freeze(void){return osl_idle;}
void acpi_os_power_thaw(void){} void acpi_os_get_diagnostics(struct acpi_os_diagnostics *d){memset(d,0,sizeof(*d));}
void acpi_telemetry_freeze(uint64_t e){assert(e);} bool acpi_telemetry_idle(uint64_t e){assert(e);return tel_idle;}
void acpi_telemetry_thaw(uint64_t e){(void)e;} size_t acpi_telemetry_info(char *o,size_t c){return snprintf(o,c,"battery unknown\n");}
bool block_power_ready(void){return dev_ok;} bool audio_power_ready(void){return dev_ok;}
bool network_power_ready(void){return dev_ok;} bool kernel_usb_power_ready(void){return dev_ok;}
bool vfs_power_ready(void){return vfs_ok;} int vfs_sync(void){assert(cpus_ack(PHASE_RENDEZVOUS));flushes++;return flush_error;}
int block_power_flush(void){return flush_error;} int block_power_stop(void){stops++;return stop_error;}
int audio_power_stop(void){stops++;return stop_error;} int network_power_stop(void){stops++;return stop_error;}
int kernel_usb_power_stop(void){stops++;return stop_error;}
static void reset(void){
 memset(tc,0,sizeof(tc));memset(tt,0,sizeof(tt));memset(ctx,0,sizeof(ctx));cpu_id=0;host_if=true;
 for(unsigned i=0;i<4;i++){tc[i].id=tc[i].lapic_id=i;tc[i].online=true;tc[i].current=&tt[i];tt[i].context=&ctx[i];tt[i].user=true;tt[i].pid=i+1;}
 tt[0].power_role=POWER_ROLE_COORDINATOR;state=STATE_IDLE;command=epoch=request_id=0;started=true;firmware_present=true;
 test_mode=0;frozen=armed=canceled=prepared=gpes_disabled=false;control_lock.value=0;last_error=firmware_status=0;failure_stage[0]=0;count=4;
 drained=tel_idle=osl_idle=dev_ok=backend=vfs_ok=true;flush_error=stop_error=bad_prep=bad_mask=bad_restore=0;
 preps=masks=thaws=flushes=stops=finals=0;omit_ack=ipi_fail=release_unresponsive=late_ipi=false;memset(threads,0,sizeof(threads));now=0;
}
static void join(void){for(unsigned i=1;i<count;i++)if(threads[i])assert(!pthread_join(threads[i],NULL));}
static void request(unsigned a){assert(!power_request(&tt[1],a,0));assert(state==STATE_PENDING&&!armed);power_request_ready(tt[1].pid);assert(armed);}
int main(void){
 reset();cpu_id=1;mutex_t a={0},b={0};mutex_lock(&a);assert(host_if&&tc[1].held_mutexes==1&&tt[1].held_mutexes==1);
 tc[1].current=&tt[5];mutex_lock(&b);assert(tc[1].held_mutexes==2&&tt[5].held_mutexes==1);
 mutex_unlock(&b);assert(tc[1].held_mutexes==1);tc[1].current=&tt[1];mutex_unlock(&a);
 assert(!tc[1].held_mutexes&&!tt[1].held_mutexes&&probes==4);
 ready=false;mutex_lock(&a);mutex_unlock(&a);assert(!tc[1].held_mutexes);ready=true;
 expect_panic=true;if(!setjmp(panic_jump)){power_mutex_released();abort();}expect_panic=false;
 command=make_command(1,PHASE_RENDEZVOUS);host_if=false;power_checkpoint();assert(!tc[1].park_ack_epoch);host_if=true;tc[1].handoff=1;power_ap_checkpoint();assert(!tc[1].park_ack_epoch);
 tc[1].handoff=0;tc[1].irq_depth=1;power_ap_checkpoint();assert(!tc[1].park_ack_epoch);tc[1].irq_depth=0;
 tc[1].held_mutexes=1;power_ap_checkpoint();assert(!tc[1].park_ack_epoch);tc[1].held_mutexes=0;
 tc[1].no_park_depth=1;power_ap_checkpoint();assert(!tc[1].park_ack_epoch);tc[1].no_park_depth=0;
 assert(!pthread_create(&threads[1],NULL,park,(void*)1));while(!__atomic_load_n(&tc[1].park_ack_epoch,__ATOMIC_ACQUIRE))usleep(10);
 __atomic_store_n(&command,make_command(1,PHASE_RELEASE),__ATOMIC_RELEASE);assert(!pthread_join(threads[1],NULL));assert(tc[1].resume_ack_epoch==1);
 reset();assert(!power_request(&tt[1],1,0));power_request_ready(tt[2].pid);assert(!armed);
 uint64_t pending_time=pending_since;assert(!power_request(&tt[2],1,0));assert(request_id==1&&pending_since==pending_time&&!armed);
 now=POWER_PENDING_US*3;power_request_ready(tt[1].pid);assert(armed&&pending_since==now);
 pending_time=pending_since;now+=POWER_PENDING_US/2;
 assert(!power_request(&tt[2],1,0));power_request_ready(tt[2].pid);power_request_ready(tt[1].pid);
 assert(armed&&request_id==1&&pending_since==pending_time);
 assert(power_request(&tt[1],1,UINT64_C(1)<<32)==-E_INVAL);
 reset();request(1);state=STATE_FREEZING;assert(!power_request(&tt[1],3,0));
 assert(!begin_preparing()&&state==STATE_FREEZING);transition();assert(!preps&&!finals);
 reset();request(1);state=STATE_FREEZING;assert(begin_preparing());
 assert(power_request(&tt[1],3,0)==-E_BUSY&&!canceled);
 reset();request(1);assert(!power_request(&tt[2],1,0));assert(power_request(&tt[2],2,0)==-E_BUSY);
 assert(!power_request(&tt[1],3,0)&&canceled);assert(power_request(&tt[1],4,0)==-E_INVAL);
 tt[1].browser_sandbox=true;assert(power_request(&tt[1],1,0)==-E_PERM);tt[1].browser_sandbox=false;ctx[1].web_role=1;assert(power_request(&tt[1],1,0)==-E_PERM);
 reset();dev_ok=false;assert(power_request(&tt[1],1,0)==-E_NOTSUP);
 reset();request(1);drained=false;transition();assert(state==STATE_FAILED&&!frozen&&!preps&&!finals);
 reset();request(1);tel_idle=false;transition();assert(state==STATE_FAILED&&!frozen&&!preps&&!finals);
 reset();request(1);bad_prep=7;transition();assert(state==STATE_FAILED&&!frozen&&prepared&&!finals&&!stops);
 reset();request(1);osl_idle=false;transition();assert(state==STATE_FAILED&&!frozen&&thaws==1&&!finals);
 reset();request(1);omit_ack=true;transition();join();assert(state==STATE_FAILED&&!frozen&&!flushes&&!finals);
 for(unsigned i=1;i<count;i++)assert(tc[i].resume_ack_epoch==epoch);
 reset();request(1);flush_error=-E_IO;transition();join();assert(state==STATE_FAILED&&!frozen&&flushes==1&&!finals&&!stops);
 for(unsigned i=1;i<count;i++)assert(tc[i].resume_ack_epoch==epoch);
 reset();request(1);ipi_fail=true;transition();assert(state==STATE_FAILED&&!frozen&&!flushes&&!finals);
 reset();request(1);omit_ack=release_unresponsive=true;transition();join();
 assert(state==STATE_RECOVERING&&!frozen&&!flushes&&!finals&&tc[3].resume_ack_epoch!=epoch);
 assert(!strcmp(failure_stage,"release-unresponsive"));
 uint64_t blocked_epoch=epoch;assert(power_request(&tt[1],1,0)==-E_BUSY&&epoch==blocked_epoch);
 reset();request(1);omit_ack=late_ipi=true;transition();join();
 assert(state==STATE_FAILED&&!frozen&&!tc[3].park_ack_epoch&&tc[3].resume_ack_epoch==epoch);
 char text[8];unsigned before=preps;assert(power_info(text,sizeof(text))<sizeof(text));assert(preps==before&&text[7]==0);
 reset();count=1;request(1);int r=setjmp(terminal_jump);if(!r)transition();assert(r==2&&finals==1&&stops==4&&state==STATE_COMMITTED);
 reset();count=1;request(2);r=setjmp(terminal_jump);if(!r)transition();assert(r==2&&finals==1&&stops==4);
 puts("POWER HOST PASS: production coordinator/mutex accounting, reversible park/release, rollback and terminal ordering (mock hardware)");return 0;
}
'''
def main():
 with tempfile.TemporaryDirectory(prefix='coast-power-') as tmp:
  d=Path(tmp);(d/'test_kernel.h').write_text(HEADER)
  for name in ['sched.h','sync.h','panic.h','tsc.h','interrupts/lapic.h','drivers/block.h','drivers/audio.h','net/network.h','usb_input.h']:
   p=d/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(('#include_next <sched.h>\n' if name=='sched.h' else '')+'#include "test_kernel.h"\n')
  src=(ROOT/'kernel/power.c').read_text().replace('__asm__ volatile("cli; hlt" ::: "memory");','host_halt();').replace('__asm__ volatile("cli" ::: "memory");','host_cli();')
  (d/'power.c').write_text(src);(d/'test.c').write_text(SOURCE.replace('POWER_SOURCE',str(d/'power.c')).replace('SYNC_SOURCE',str(ROOT/'kernel/sched/sync.c')))
  cmd=shlex.split(os.environ.get('HOST_CC','cc'))+['-std=gnu11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-I'+str(d),'-idirafter',str(ROOT/'include'),str(d/'test.c'),'-o',str(d/'test')]
  subprocess.run(cmd,check=True)
  env=dict(os.environ);env['ASAN_OPTIONS']='detect_leaks=0:halt_on_error=1'
  subprocess.run([str(d/'test')],check=True,timeout=30,env=env)
if __name__=='__main__':main()
