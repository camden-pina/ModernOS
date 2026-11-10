/* Run only in disposable guests. The identical static ELF runs on Linux and
 * Coast; every destructive credential operation is isolated by fork. */
#include <libc.h>
static const char *self;
static void trace(const char *name,long result) {printf("CRED %s %ld\n",name,result);}
static void ids(const char *name) {
 unsigned u[3]={0},g[3]={0};call3(SYS_getresuid,u,u+1,u+2);call3(SYS_getresgid,g,g+1,g+2);
 printf("CRED %s uid=%u,%u,%u gid=%u,%u,%u fs=%ld,%ld\n",name,u[0],u[1],u[2],g[0],g[1],g[2],call1(SYS_setfsuid,-1),call1(SYS_setfsgid,-1));
}
static void aux(void) {
 char **e=environ;while(*e)e++;uintptr_t *a=(uintptr_t *)(e+1);
 unsigned uid=0,euid=0,gid=0,egid=0,secure=0;
 for(;a[0];a+=2){if(a[0]==11)uid=a[1];if(a[0]==12)euid=a[1];if(a[0]==13)gid=a[1];if(a[0]==14)egid=a[1];if(a[0]==23)secure=a[1];}
 printf("CRED aux uid=%u euid=%u gid=%u egid=%u secure=%u\n",uid,euid,gid,egid,secure);
}
static int copy(const char *path,unsigned mode) {
 int in=open(self,O_RDONLY,0),out=open(path,O_CREAT|O_TRUNC|O_WRONLY,0755);if(in<0 || out<0)return -1;
 char b[4096];long n;while((n=read(in,b,sizeof(b)))>0)if(write_all(out,b,n))return -1;
 close(in);close(out);return call2(SYS_chmod,path,mode);
}
static void execute(const char *path,const char *operation) {char *av[]={(char *)path,(char *)operation,NULL};call3(SYS_execve,path,av,environ);trace("exec-error",-1);exit(99);}
static void probe(int n) {
 switch(n) {
 case 0:
  trace("setresgid",call3(SYS_setresgid,21,22,23));trace("setresuid",call3(SYS_setresuid,101,102,103));ids("res");
  trace("saved-swap",call3(SYS_setresuid,-1,103,-1));ids("saved");trace("forbidden-root",call1(SYS_setuid,0));break;
 case 1:
  trace("setuid-drop",call1(SYS_setuid,42));ids("permanent");trace("regain",call1(SYS_setuid,0));break;
 case 2:
  trace("effective-drop",call3(SYS_setresuid,-1,42,-1));ids("temporary");trace("effective-regain",call3(SYS_setresuid,-1,0,-1));ids("restored");break;
 case 3:
  trace("reuid",call2(SYS_setreuid,42,43));ids("re");trace("re-saved",call3(SYS_setresuid,-1,43,-1));trace("re-regain",call1(SYS_setuid,0));break;
 case 4: {
  unsigned groups[]={42,7,11},got[4]={0};trace("groups-set",call2(SYS_setgroups,3,groups));long count=call2(SYS_getgroups,4,got);
  printf("CRED groups count=%ld values=%u,%u,%u\n",count,got[0],got[1],got[2]);
  call1(SYS_setgid,42);call1(SYS_setuid,42);int fd=open("/tmp/credential-group",O_RDONLY,0);trace("supplementary-read",fd>=0?0:fd);if(fd>=0)close(fd);
  trace("groups-forbidden",call2(SYS_setgroups,0,0));break;
 }
 case 5: {
  trace("fsuid-old",call1(SYS_setfsuid,42));int fd=open("/tmp/credential-secret",O_RDONLY,0);trace("fsuid-read",fd>=0?0:fd);if(fd>=0)close(fd);
  trace("fsuid-restore",call1(SYS_setfsuid,0));break;
 }
 case 6:case 7:
  call2(SYS_setgroups,0,0);call1(SYS_setgid,42);call1(SYS_setuid,42);
  if(n==7)trace("nnp",syscall6(SYS_prctl,38,1,0,0,0,0));
  execute("/tmp/credential-suid","--executed");break;
 case 8:
  call2(SYS_setgroups,0,0);call1(SYS_setgid,42);call1(SYS_setuid,42);execute("/tmp/credential-suid","--drop-exec");break;
 case 9: {
  call1(SYS_setgid,42);call1(SYS_setuid,42);int fd=open("/tmp/credential-write",O_WRONLY,0);trace("unprivileged-write",fd<0?fd:write(fd,"x",1));if(fd>=0)close(fd);
  struct os_stat st={0};stat("/tmp/credential-write",&st);trace("write-setid-bits",st.mode&06000);break;
 }
 case 10: {
  trace("root-chown",call3(SYS_chown,"/tmp/credential-chown",42,42));struct os_stat st={0};stat("/tmp/credential-chown",&st);trace("chown-setid-bits",st.mode&06000);break;
 }
 case 11:
  call1(SYS_setgid,42);call1(SYS_setuid,42);execute("/tmp/credential-script","--executed");break;
 case 12: {
  uint32_t first=0,word=9;
  for(unsigned op=0;op<=4;op++) {
   uint32_t encoded=(op<<28)|(2u<<24)|(3u<<12)|10;
   long e=syscall6(SYS_futex,(long)&first,5|128,0,0,(long)&word,encoded);
   printf("CRED futex-op op=%u result=%ld word=%u\n",op,e,word);
  }
  word=0x80000001;uint32_t encoded=(11u<<28)|(2u<<24)|(31u<<12);
  long e=syscall6(SYS_futex,(long)&first,5|128,0,0,(long)&word,encoded);
  printf("CRED futex-shift result=%ld word=%u\n",e,word);
  word=3;encoded=(1u<<28)|(5u<<24)|(0xfffu<<12)|0xfff;
  e=syscall6(SYS_futex,(long)&first,5|128,0,0,(long)&word,encoded);
  printf("CRED futex-signed result=%ld word=%u\n",e,word);
  trace("futex-bad-op",syscall6(SYS_futex,(long)&first,5|128,0,0,(long)&word,7u<<28));
  trace("futex-bad-alignment",syscall6(SYS_futex,(long)&first,5|128,0,0,(long)((char *)&word+1),0));
  trace("futex-bad-memory",syscall6(SYS_futex,(long)&first,5|128,0,0,1UL<<48,0));
  break;
 }
 case 13: {
  call1(SYS_setuid,42);int fd=open("/tmp/credential-write",O_RDONLY,0),alias=call1(SYS_dup,fd);
  trace("fioclex",call3(SYS_ioctl,fd,0x5451,1UL<<48));trace("fioclex-fd",call3(SYS_fcntl,fd,F_GETFD,0));
  trace("fioclex-dup",call3(SYS_fcntl,alias,F_GETFD,0));trace("fionclex",call3(SYS_ioctl,fd,0x5450,0));
  trace("fionclex-fd",call3(SYS_fcntl,fd,F_GETFD,0));trace("fioclex-invalid",call3(SYS_ioctl,-1,0x5451,0));
  int path=open("/tmp/credential-write",010000000,0);trace("fioclex-opath",call3(SYS_ioctl,path,0x5451,0));
  if(path>=0)close(path);
  if(fd>=0)close(fd);
  if(alias>=0)close(alias);
  int pipe[2],one=1,zero=0;long e=call2(SYS_pipe2,pipe,0);trace("fionbio-pipe-create",e);
  if(!e){trace("fionbio-pipe-set",call3(SYS_ioctl,pipe[0],0x5421,&one));trace("fionbio-pipe-flag",call3(SYS_fcntl,pipe[0],F_GETFL,0)&O_NONBLOCK);trace("fionbio-pipe-clear",call3(SYS_ioctl,pipe[0],0x5421,&zero));trace("fionbio-pipe-cleared",call3(SYS_fcntl,pipe[0],F_GETFL,0)&O_NONBLOCK);close(pipe[0]);close(pipe[1]);}
  break;
 }
 case 14: {
  call1(SYS_setuid,42);
  const char *devices[]={"/dev/null","/dev/zero"};
  for(unsigned i=0;i<2;i++) {
   int fd=open(devices[i],O_RDWR,0),alias=call1(SYS_dup,fd);
   printf("CRED seek-device device=%u open=%ld\n",i,fd>=0?0L:(long)fd);
   for(unsigned origin=0;origin<=4;origin++) {
    long offset=origin==0?-1:origin==1?INT64_MAX:origin==2?INT64_MIN:12345;
    printf("CRED seek-device device=%u origin=%u result=%ld alias=%ld\n",i,origin,call3(SYS_lseek,fd,offset,origin),call3(SYS_lseek,alias,0,1));
   }
   printf("CRED seek-device device=%u invalid=%ld negative-origin=%ld\n",i,call3(SYS_lseek,fd,0,5),call3(SYS_lseek,fd,0,-1));
   int path=open(devices[i],010000000,0);
   printf("CRED seek-device device=%u opath=%ld\n",i,call3(SYS_lseek,path,0,0));
   if(path>=0)close(path);
   if(fd>=0)close(fd);
   if(alias>=0)close(alias);
  }
  trace("seek-invalid",call3(SYS_lseek,-1,0,0));
  int pipe[2];if(!call2(SYS_pipe2,pipe,0)){trace("seek-pipe",call3(SYS_lseek,pipe[0],0,0));close(pipe[0]);close(pipe[1]);}
  break;
 }
 }
 exit(0);
}
int main(int argc,char **argv) {
 if(argc>=2) {
  if(!strcmp(argv[1],"--drop-exec")){call3(SYS_setresuid,-1,42,-1);execute("/tmp/credential-plain","--plain");}
  if(!strcmp(argv[1],"--plain")){ids("plain-exec");trace("exec-regain",call3(SYS_setresuid,-1,0,-1));return 0;}
  ids("setid-exec");aux();return 0;
 }
 self=argv[0];if(call0(SYS_getuid)){trace("requires-root",1);return 77;}
 call2(SYS_mkdir,"/tmp",01777);call2(SYS_chmod,"/tmp",01777);call1(SYS_umask,0);
 if(copy("/tmp/credential-plain",0755) || copy("/tmp/credential-suid",06755))return 99;
 const char *paths[]={"/tmp/credential-secret","/tmp/credential-group","/tmp/credential-write","/tmp/credential-chown","/tmp/credential-script"};
 for(unsigned i=0;i<5;i++){int fd=open(paths[i],O_CREAT|O_TRUNC|O_WRONLY,0600);if(fd<0)return 99;if(i==4){const char *s="#!/tmp/credential-plain\n";write_all(fd,s,strlen(s));}close(fd);}
 call3(SYS_chown,paths[1],0,7);call2(SYS_chmod,paths[1],0640);call2(SYS_chmod,paths[2],06777);call2(SYS_chmod,paths[3],06755);call2(SYS_chmod,paths[4],06755);
 ids("root");
 for(int i=0;i<15;i++){printf("CRED probe %d\n",i);long pid=call0(SYS_fork);if(!pid)probe(i);int status=0;if(pid<0 || waitpid(pid,&status,0)!=pid || status){trace("child-failure",i);return 1;}}
 puts("CREDENTIAL_PARITY_DONE");return 0;
}
