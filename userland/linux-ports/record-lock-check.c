#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL record locks:%d %s errno=%d\n",__LINE__,#x,errno);exit(1);} } while(0)
#define ERROR(x,e) do {errno=0;CHECK((x)==-1 && errno==(e));} while(0)
static int fd;
static pid_t owner;
static char path[256], other[256];
static const char *self;
static struct flock query(int f,short type,off_t start,off_t len) {
 struct flock l={.l_type=type,.l_whence=SEEK_SET,.l_start=start,.l_len=len};CHECK(!fcntl(f,F_GETLK,&l));return l;
}
static int lock_at(int f,short type,short whence,off_t start,off_t len) {
 struct flock l={.l_type=type,.l_whence=whence,.l_start=start,.l_len=len};return fcntl(f,F_SETLK,&l);
}
static void child_check(void (*test)(void)) {
 pid_t p=fork();CHECK(p>=0);if(!p){test();_exit(0);}int status;CHECK(waitpid(p,&status,0)==p && WIFEXITED(status) && WEXITSTATUS(status)==0);
}
static void no_conflict(void) { struct flock l=query(fd,F_WRLCK,0,0);CHECK(l.l_type==F_UNLCK); }
static void conflict(void) {
 struct flock l=query(fd,F_RDLCK,12,1);CHECK(l.l_type==F_WRLCK && l.l_whence==SEEK_SET && l.l_start==10 && l.l_len==10 && l.l_pid==owner);
 ERROR(lock_at(fd,F_WRLCK,SEEK_SET,12,1),EAGAIN);ERROR(lock_at(fd,F_RDLCK,SEEK_SET,12,1),EAGAIN);
 CHECK(!lock_at(fd,F_WRLCK,SEEK_SET,20,5));CHECK(!close(fd));
 fd=open(path,O_RDWR);CHECK(fd>=0);l=query(fd,F_WRLCK,12,1);CHECK(l.l_type==F_WRLCK && l.l_pid==owner);
}
static void split(void) {
 struct flock l=query(fd,F_WRLCK,11,1);CHECK(l.l_type==F_WRLCK && l.l_start==10 && l.l_len==3);
 l=query(fd,F_WRLCK,15,1);CHECK(l.l_type==F_UNLCK);
 l=query(fd,F_WRLCK,18,1);CHECK(l.l_type==F_WRLCK && l.l_start==17 && l.l_len==3);
}
static void shared(void) {
 struct flock l=query(fd,F_RDLCK,15,1);CHECK(l.l_type==F_UNLCK);
 CHECK(!lock_at(fd,F_RDLCK,SEEK_SET,15,1));l=query(fd,F_WRLCK,15,1);CHECK(l.l_type==F_RDLCK && l.l_start==13 && l.l_len==4);
 ERROR(lock_at(fd,F_RDLCK,SEEK_SET,11,1),EAGAIN);
}
static void relative(void) {
 struct flock l=query(fd,F_WRLCK,25,1);CHECK(l.l_type==F_WRLCK && l.l_start==25 && l.l_len==5);
 l=query(fd,F_WRLCK,90,1);CHECK(l.l_type==F_WRLCK && l.l_start==90 && l.l_len==0);
}
static void bsd_independent(void) {
 CHECK(!lock_at(fd,F_WRLCK,SEEK_SET,0,1));int f=open(path,O_RDWR);CHECK(f>=0);ERROR(flock(f,LOCK_EX|LOCK_NB),EAGAIN);CHECK(!close(f));
}
static void *same_process(void *unused) {
 (void)unused;int f=open(path,O_RDWR);CHECK(f>=0);struct flock l=query(f,F_WRLCK,0,1);CHECK(l.l_type==F_UNLCK);
 CHECK(!lock_at(f,F_RDLCK,SEEK_SET,0,1));CHECK(!close(f));return NULL;
}
static void exec_lifetime(int cloexec) {
 int ready[2],done[2];CHECK(!pipe(ready) && !pipe(done));pid_t p=fork();CHECK(p>=0);
 if(!p){
  close(ready[0]);close(done[1]);int f=open(path,O_RDWR);CHECK(f>=0);CHECK(!lock_at(f,F_WRLCK,SEEK_SET,0,1));
  if(cloexec)CHECK(open(path,O_RDONLY|O_CLOEXEC)>=0);
  char a[24],b[24];snprintf(a,sizeof(a),"%d",ready[1]);snprintf(b,sizeof(b),"%d",done[0]);
  execl(self,self,"--exec-hold",a,b,(char*)NULL);_exit(120);
 }
 close(ready[1]);close(done[0]);char c;CHECK(read(ready[0],&c,1)==1 && c=='R');
 struct flock l=query(fd,F_WRLCK,0,1);CHECK(cloexec ? l.l_type==F_UNLCK : l.l_type==F_WRLCK && l.l_pid==p);
 CHECK(write(done[1],"D",1)==1);close(done[1]);close(ready[0]);int status;CHECK(waitpid(p,&status,0)==p && WIFEXITED(status) && !WEXITSTATUS(status));no_conflict();
}
static void capacity_conflict(void) {
 struct flock l=query(fd,F_WRLCK,1,1);CHECK(l.l_type==F_WRLCK && l.l_start==0 && l.l_len==3 && l.l_pid==owner);
}
int main(int argc,char **argv) {
 if(argc==4 && !strcmp(argv[1],"--exec-hold")){char c;CHECK(write(atoi(argv[2]),"R",1)==1);CHECK(read(atoi(argv[3]),&c,1)==1);return 0;}
 int coast=argc==2 && !strcmp(argv[1],"--coast");self=argv[0];owner=getpid();
 char dir[]="/tmp/record-lock-check-XXXXXX";CHECK(mkdtemp(dir));snprintf(path,sizeof(path),"%s/data",dir);snprintf(other,sizeof(other),"%s/other",dir);
 fd=open(path,O_CREAT|O_RDWR|O_TRUNC,0600);CHECK(fd>=0);char bytes[100]={0};CHECK(write(fd,bytes,sizeof(bytes))==sizeof(bytes));
 CHECK(!lock_at(fd,F_WRLCK,SEEK_SET,10,10));no_conflict();child_check(conflict);
 CHECK(!lock_at(fd,F_UNLCK,SEEK_SET,13,4));child_check(split);
 CHECK(!lock_at(fd,F_WRLCK,SEEK_SET,13,4));child_check(conflict);
 CHECK(!lock_at(fd,F_RDLCK,SEEK_SET,13,4));child_check(shared);CHECK(!lock_at(fd,F_UNLCK,SEEK_SET,0,0));
 puts("RECORD_LOCK_CASE ranges-shared-conflicts-merge-split PASS");
 CHECK(lseek(fd,40,SEEK_SET)==40);CHECK(!lock_at(fd,F_WRLCK,SEEK_CUR,-10,-5));CHECK(!lock_at(fd,F_WRLCK,SEEK_END,-10,0));child_check(relative);CHECK(!lock_at(fd,F_UNLCK,SEEK_SET,0,0));
 ERROR(lock_at(fd,9,SEEK_SET,0,1),EINVAL);ERROR(lock_at(fd,F_WRLCK,9,0,1),EINVAL);ERROR(lock_at(fd,F_WRLCK,SEEK_SET,-1,1),EINVAL);
 ERROR(lock_at(fd,F_WRLCK,SEEK_SET,INT64_MAX,2),EOVERFLOW);ERROR(lock_at(fd,F_WRLCK,SEEK_SET,0,INT64_MIN),EINVAL);
 ERROR(lock_at(-1,F_WRLCK,SEEK_SET,0,1),EBADF);int ro=open(path,O_RDONLY),wo=open(path,O_WRONLY),op=open(path,O_PATH);CHECK(ro>=0 && wo>=0 && op>=0);
 ERROR(lock_at(ro,F_WRLCK,SEEK_SET,0,1),EBADF);ERROR(lock_at(wo,F_RDLCK,SEEK_SET,0,1),EBADF);ERROR(lock_at(op,F_RDLCK,SEEK_SET,0,1),EBADF);close(ro);close(wo);close(op);
 ERROR(syscall(SYS_fcntl,fd,F_GETLK,(void*)1),EFAULT);ERROR(syscall(SYS_fcntl,fd,F_SETLK,(void*)1),EFAULT);
 struct flock *q=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(q!=MAP_FAILED);*q=(struct flock){.l_type=F_WRLCK,.l_whence=SEEK_SET,.l_len=1};CHECK(!mprotect(q,4096,PROT_READ));
 ERROR(syscall(SYS_fcntl,fd,F_GETLK,q),EFAULT);CHECK(!fcntl(fd,F_SETLK,q));CHECK(!munmap(q,4096));CHECK(!lock_at(fd,F_UNLCK,SEEK_SET,0,0));
 puts("RECORD_LOCK_CASE whence-negative-length-errors-pointers PASS");
 CHECK(!lock_at(fd,F_WRLCK,SEEK_SET,0,1));int extra=open(path,O_RDONLY);CHECK(extra>=0 && !close(extra));child_check(no_conflict);
 CHECK(!lock_at(fd,F_WRLCK,SEEK_SET,0,1));extra=dup(fd);CHECK(extra>=0 && !close(extra));child_check(no_conflict);
 CHECK(!lock_at(fd,F_WRLCK,SEEK_SET,0,1));int unrelated=open(other,O_CREAT|O_RDWR,0600);extra=open(path,O_RDONLY);CHECK(unrelated>=0 && extra>=0 && dup2(unrelated,extra)==extra);child_check(no_conflict);close(extra);close(unrelated);
 CHECK(!lock_at(fd,F_WRLCK,SEEK_SET,0,1));extra=open(path,O_RDONLY);CHECK(extra>=0 && !syscall(SYS_close_range,extra,extra,0));child_check(no_conflict);
 exec_lifetime(0);exec_lifetime(1);
 puts("RECORD_LOCK_CASE any-close-dup2-close-range-fork-exec-exit PASS");
 CHECK(!lock_at(fd,F_WRLCK,SEEK_SET,0,1));pthread_t thread;CHECK(!pthread_create(&thread,NULL,same_process,NULL));CHECK(!pthread_join(thread,NULL));child_check(no_conflict);
 CHECK(!flock(fd,LOCK_EX));child_check(bsd_independent);CHECK(!flock(fd,LOCK_UN));
 puts("RECORD_LOCK_CASE thread-owner-bsd-independence PASS");
 if(coast){
  for(int i=0;i<256;i++)CHECK(!lock_at(fd,F_WRLCK,SEEK_SET,i*5,3));
  ERROR(lock_at(fd,F_WRLCK,SEEK_SET,2000,1),ENOLCK);ERROR(lock_at(fd,F_UNLCK,SEEK_SET,1,1),ENOLCK);child_check(capacity_conflict);
  CHECK(!lock_at(fd,F_UNLCK,SEEK_SET,0,0));CHECK(!lock_at(fd,F_WRLCK,SEEK_SET,0,1));CHECK(!lock_at(fd,F_UNLCK,SEEK_SET,0,0));
  struct flock l={.l_type=F_WRLCK,.l_whence=SEEK_SET,.l_len=1};ERROR(fcntl(fd,F_SETLKW,&l),ENOTSUP);ERROR(fcntl(fd,F_OFD_SETLK,&l),ENOTSUP);
  puts("RECORD_LOCK_CASE bounded-capacity-atomic-failure-explicit-unsupported PASS");
 }
 CHECK(!close(fd));unlink(path);unlink(other);CHECK(!rmdir(dir));puts("RECORD_LOCK_CHECK_PASS");return 0;
}
