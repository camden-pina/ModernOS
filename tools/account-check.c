/* Disposable guest fixture: never installed in a production image. */
#include <libc.h>
#include <uapi/account.h>
#include <uapi/window.h>
#define ADMIN (OS_ACCOUNT_FIRST+30)
#define STANDARD (OS_ACCOUNT_FIRST+31)
#define PASSWORD "disposable-QA-password"
static int failed;
#define CHECK(x) do {if(!(x)){printf("ACCOUNT_CHECK_FAIL line=%u\n",__LINE__);failed=1;}}while(0)
static int account(unsigned op,struct os_account_request *q) {return call2(SYS_account,op,q);}
static int enter(unsigned uid) {return call2(SYS_setgroups,0,0) || call3(SYS_setresgid,uid,uid,uid) || call3(SYS_setresuid,uid,uid,uid);}
static int auth(unsigned uid,const char *pw) {struct os_account_request q={.uid=uid};strcpy(q.password,pw);return account(OS_ACCOUNT_SUDO,&q);}
static int make_user(unsigned uid,unsigned admin) {
 struct os_account_request q={.uid=uid};int e=account(OS_ACCOUNT_ENSURE,&q);if(e)return e;
 q.actor=0;q.changes=7;q.flags=admin;q.timeout_minutes=0;strcpy(q.new_password,PASSWORD);return account(OS_ACCOUNT_UPDATE,&q);
}
static void probe(int test) {
 unsigned uid=test==1?STANDARD:ADMIN;
 if(enter(uid))exit(99);
 int e=0,fd;struct os_account_request q={.uid=uid};
 switch(test) {
 case 0:
  e=call0(SYS_getuid)!=ADMIN || call0(SYS_geteuid)!=ADMIN || call0(SYS_getgid)!=ADMIN;
  fd=open("/etc/account-qa-protected",O_RDONLY,0);e|=fd!=-E_ACCES;if(fd>=0)close(fd);
  fd=open("/etc/account-qa-write",O_WRONLY|O_CREAT,0600);e|=fd!=-E_ACCES;if(fd>=0)close(fd);
  fd=open("/etc/coast-accounts",O_RDONLY,0);e|=fd!=-E_ACCES;if(fd>=0)close(fd);
  fd=open("/tmp/account-qa-private/owned",O_WRONLY|O_CREAT,0600);e|=fd<0;if(fd>=0)close(fd);
  fd=open("/tmp/account-qa-other/secret",O_RDONLY,0);e|=fd!=-E_ACCES;if(fd>=0)close(fd);
  e|=call3(SYS_setresuid,0,0,0)!=-E_PERM;
  q.changes=OS_ACCOUNT_ROLE;q.flags=OS_ACCOUNT_ADMIN;e|=account(OS_ACCOUNT_UPDATE,&q)!=-E_PERM;
  e|=account(OS_ACCOUNT_ENSURE,&q)!=-E_PERM;
  e|=account(OS_ACCOUNT_CODE_LAUNCH,&q)!=-E_PERM;
  e|=call2(SYS_kill,1,0)!=-E_PERM;
  e|=call1(SYS_chroot,"/")!=-E_PERM;
  e|=call3(SYS_window,OS_WINDOW_SERVER_START,0,0)!=-E_PERM;
  struct os_window_info window={.version=OS_WINDOW_PIXEL_CLIENT_VERSION,.width=32,.height=32};
  strcpy(window.title,"Inactive account window");
  e|=call3(SYS_window,OS_WINDOW_CREATE,&window,sizeof(window))!=-E_PERM;
  q.uid=STANDARD;e|=account(OS_ACCOUNT_QUERY,&q)!=-E_PERM;
  break;
 case 1:e=auth(STANDARD,PASSWORD)!=-E_PERM || call0(SYS_getuid)!=STANDARD;break;
 case 2:e=auth(ADMIN,"wrong-password")!=-E_PERM || call0(SYS_getuid)!=ADMIN;break;
 case 3:
  /* Knowing the password alone cannot call the privileged sudo backend. */
  e=auth(ADMIN,PASSWORD)!=-E_PERM || call0(SYS_getuid)!=ADMIN;
  fd=open("/etc/account-qa-protected",O_RDONLY,0);e|=fd!=-E_ACCES;if(fd>=0)close(fd);break;
 case 4:
  e=syscall6(SYS_prctl,38,1,0,0,0,0)!=0;
  e|=auth(ADMIN,PASSWORD)!=-E_PERM || call0(SYS_getuid)!=ADMIN;
  e|=syscall6(SYS_prctl,38,0,0,0,0,0)!=-E_INVAL;
  break;
 case 5:
  q.actor=0;q.changes=OS_ACCOUNT_ROLE;q.flags=OS_ACCOUNT_ADMIN;
  e=account(OS_ACCOUNT_UPDATE,&q)!=-E_PERM;
  e|=account(OS_ACCOUNT_FILE_SCOPE,&q)!=-E_PERM;
  e|=account(OS_ACCOUNT_LOGIN,&q)!=-E_PERM;break;
 case 6: {
  q.uid=STANDARD;strcpy(q.password,PASSWORD);e=account(OS_ACCOUNT_SUDO,&q)!=-E_PERM;break;
 }
 case 7: {
  syscall6(SYS_prctl,38,1,0,0,0,0);
  char *av[]={"/tmp/account-check","--nnp",NULL};call3(SYS_execve,av[0],av,environ);e=1;break;
 }
 }
 exit(e?1:0);
}
static int run_probe(int n) {
 long pid=call0(SYS_fork);if(!pid)probe(n);int status=0;
 return pid<0 || waitpid(pid,&status,0)!=pid || status;
}
static int sudo_probe(unsigned uid,const char *pw,int nnp,int denied,const char *script) {
 int p[2];if(call1(SYS_pipe,p))return 1;
 long pid=call0(SYS_fork);
 if(!pid) {
  close(p[1]);call2(SYS_dup2,p[0],0);close(p[0]);if(enter(uid))exit(99);
  if(nnp)syscall6(SYS_prctl,38,1,0,0,0,0);
  char *av[]={"/bin/sudo","-S","/bin/ash","-c",(char *)script,NULL};
  char *env[]={"PATH=/tmp","HOME=/tmp","LD_PRELOAD=/tmp/evil.so","EVIL=1",NULL};
  call3(SYS_execve,av[0],av,env);exit(99);
 }
 close(p[0]);for(int i=0;i<3;i++){write_all(p[1],pw,strlen(pw));write_all(p[1],"\n",1);}close(p[1]);int status=0;
 if(pid<0 || waitpid(pid,&status,0)!=pid)return 1;
 return denied?status==0:status!=0;
}
int main(int argc,char **argv) {
 if(argc==2 && !strcmp(argv[1],"--nnp"))return auth(ADMIN,PASSWORD)!=-E_PERM;
 if(call0(SYS_getuid)!=0)return 77;
 struct os_account_request q={.uid=OS_ACCOUNT_FIRST};CHECK(!account(OS_ACCOUNT_ENSURE,&q));
 CHECK(!make_user(ADMIN,OS_ACCOUNT_ADMIN));CHECK(!make_user(STANDARD,0));
 int fd=open("/etc/account-qa-protected",O_WRONLY|O_CREAT,0600);CHECK(fd>=0);if(fd>=0)close(fd);
 call2(SYS_mkdir,"/tmp/account-qa-private",0700);call2(SYS_mkdir,"/tmp/account-qa-other",0700);
 fd=open("/tmp/account-qa-private",O_RDONLY|O_DIRECTORY,0);CHECK(fd>=0 && !call3(SYS_fchown,fd,ADMIN,ADMIN));if(fd>=0)close(fd);
 fd=open("/tmp/account-qa-other/secret",O_WRONLY|O_CREAT,0600);CHECK(fd>=0);if(fd>=0)close(fd);
 fd=open("/tmp/account-qa-other",O_RDONLY|O_DIRECTORY,0);CHECK(fd>=0 && !call3(SYS_fchown,fd,STANDARD,STANDARD));if(fd>=0)close(fd);
 for(int i=0;i<5;i++) {CHECK(!run_probe(i));if(!failed)printf("ACCOUNT_PROBE_PASS %d\n",i);}
 CHECK(!run_probe(5));CHECK(!run_probe(6));CHECK(!run_probe(7));
 const char *script="test $(/bin/id -u) = 0 && test \"$HOME\" = /root && test -z \"$LD_PRELOAD\" && test -z \"$EVIL\" && echo ELEVATED > /etc/account-sudo-command";
 CHECK(!sudo_probe(ADMIN,PASSWORD,0,0,script));
 CHECK(!sudo_probe(STANDARD,PASSWORD,0,1,script));
 CHECK(!sudo_probe(ADMIN,PASSWORD,1,1,script));
 CHECK(!sudo_probe(ADMIN,"wrong-password",0,1,script));
 CHECK(!sudo_probe(ADMIN,PASSWORD,0,0,script)); /* A success resets failed attempts. */
 CHECK(!sudo_probe(ADMIN,"wrong-password",0,1,script));
 CHECK(!sudo_probe(ADMIN,"wrong-password",0,1,script));
 CHECK(!sudo_probe(ADMIN,PASSWORD,0,1,script)); /* Five failures lock out even a correct password. */
 printf("ACCOUNT_CHECK_%s\n",failed?"FAIL":"PASS");return failed;
}
