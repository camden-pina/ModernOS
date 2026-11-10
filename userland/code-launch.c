/* Static setuid entry for the fixed trusted Code OSS display bridge. The
 * editor itself always enters its existing UID-1000/NNP/seccomp sandbox. */
#include <libc.h>
#include <uapi/account.h>
static int enabled(const char *name) {
 size_t n=strlen(name);for(char **p=environ;p && *p;p++)if(!strncmp(*p,name,n) && !strcmp(*p+n,"=1"))return 1;return 0;
}
int main(int argc,char **argv) {
 if(argc==2 && !strcmp(argv[1],"--help")){puts("Usage: code [folder]\nOpen a folder in the sandboxed Code OSS editor.");return 0;}
 if(argc>2 || (argc==2 && argv[1][0]=='-')){puts("Usage: code [folder]");return 2;}
 unsigned real=call0(SYS_getuid);
 if(call0(SYS_geteuid)!=0 || (real && real!=OS_ACCOUNT_FIRST)){puts("code: the protected launcher is available in the owner profile.");return 1;}
 const char *folder=argc==2?argv[1]:".";struct os_stat st;
 /* Resolve the requested folder with the caller's filesystem identity. */
 call1(SYS_setfsgid,real);call1(SYS_setfsuid,real);
 int e=stat(folder,&st);
 call1(SYS_setfsuid,0);call1(SYS_setfsgid,0);
 if(e || (st.mode&0170000)!=S_IFDIR){printf("code: folder unavailable: %s\n",e?error_text(e):"not a directory");return 1;}
 struct os_account_request q={.uid=real};e=call2(SYS_account,OS_ACCOUNT_CODE_LAUNCH,&q);
 if(e){puts("code: launcher authorization denied.");return 1;}
 for(unsigned fd=3;fd<OS_FD_MAX;fd++)close(fd);
 char *env[]={"PATH=/usr/bin:/bin","HOME=/root","USER=user","LOGNAME=user","TERM=xterm","LANG=C.UTF-8",
  enabled("COAST_CODEOSS_QA")?"COAST_CODEOSS_QA=1":"COAST_CODEOSS_QA=0",
  enabled("COAST_USER_FILES_CHECK")?"COAST_USER_FILES_CHECK=1":"COAST_USER_FILES_CHECK=0",NULL};
 char *args[]={"code",(char *)folder,NULL};
 e=call3(SYS_execve,"/bin/codeoss-wayland",args,env);
 printf("code: display bridge failed: %s\n",error_text(e));return 126;
}
