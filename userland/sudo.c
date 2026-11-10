/* Root-owned setuid sudo front end. Kernel policy authenticates the real
 * caller before this program selects credentials or executes a command. */
#include <libc.h>
#include <uapi/account.h>
static struct os_termios saved;
static volatile int restore;
static void erase(void *p,size_t n) {volatile unsigned char *b=p;while(n--)*b++=0;}
static void interrupted(int sig) {if(restore)ioctl(0,TCSETS,&saved);exit(128+sig);}
__attribute__((naked)) static void signal_return(void) {__asm__ volatile("mov $15,%rax\nsyscall\nud2");}
static int password(char out[128],int stdin_password) {
    int tty=!ioctl(0,TCGETS,&saved);
    if(!tty && !stdin_password) {const char *message="sudo: a terminal is required (or use -S)\n";write_all(2,message,strlen(message));return -E_NOTTY;}
    struct os_sigaction sa={.handler=(uintptr_t)interrupted,.flags=SA_RESTORER,.restorer=(uintptr_t)signal_return};
    for(unsigned sig=1;sig<=15;sig++)if(sig==1 || sig==2 || sig==15)syscall6(SYS_rt_sigaction,sig,(long)&sa,0,8,0,0);
    if(tty) {struct os_termios mode=saved;mode.lflag&=~ECHO;if(ioctl(0,TCSETS,&mode))return -E_IO;restore=1;}
    const char *prompt="[sudo] Password: ";write_all(2,prompt,strlen(prompt));
    unsigned used=0;int e=0;
    for(;;) {
        char c;long n=read(0,&c,1);
        if(n<=0) {e=n<0?n:-E_IO;break;}
        if(c=='\n' || c=='\r')break;
        if(used>=127) {e=-E_2BIG;continue;}
        out[used++]=c;
    }
    out[used]=0;
    if(restore) {ioctl(0,TCSETS,&saved);restore=0;write_all(2,"\n",1);}
    return e;
}
static int has_slash(const char *s) {while(*s)if(*s++=='/')return 1;return 0;}
static int target_id(const char *text,unsigned *out) {
 if(!strcmp(text,"root")){*out=0;return 0;}
 if(!strcmp(text,"user")){*out=1000;return 0;}
 if(!strncmp(text,"profile-",8)) {unsigned n=0;const char *p=text+8;if(!*p)return -1;for(;*p;p++){if(*p<'0' || *p>'9')return -1;n=n*10+*p-'0';if(n>31)return -1;}if(!n)return -1;*out=1000+n;return 0;}
 if(*text=='#')text++;
 if(!*text)return -1;
 uint64_t v=0;for(const char *p=text;*p;p++){if(*p<'0' || *p>'9')return -1;v=v*10+*p-'0';if(v>=UINT32_MAX)return -1;}
 *out=v;return 0;
}
static void decimal(char *out,unsigned v) {char d[16];unsigned n=0;do{d[n++]='0'+v%10;v/=10;}while(v);while(n)*out++=d[--n];*out=0;}
static void username(char *out,unsigned uid) {if(!uid)strcpy(out,"root");else if(uid==1000)strcpy(out,"user");else if(uid>1000 && uid<1032) {strcpy(out,"profile-");decimal(out+8,uid-1000);}else decimal(out,uid);}
int main(int argc,char **argv) {
    int at=1,noninteractive=0,stdin_password=0,validate=0,list=0,forget=0,shell=0;
    unsigned target=0,target_group=0;int group_set=0;
    for(;at<argc;at++) {
        const char *v=argv[at];if(v[0]!='-' || !v[1])break;
        if(!strcmp(v,"--")){at++;break;}
        if(!strcmp(v,"-n"))noninteractive=1;
        else if(!strcmp(v,"-S"))stdin_password=1;
        else if(!strcmp(v,"-k") || !strcmp(v,"-K"))forget=1;
        else if(!strcmp(v,"-v"))validate=1;
        else if(!strcmp(v,"-l"))list=1;
        else if(!strcmp(v,"-s") || !strcmp(v,"-i"))shell=!strcmp(v,"-i")?2:1;
        else if((!strcmp(v,"-u") || !strcmp(v,"-g")) && at+1<argc) {
            unsigned id;if(target_id(argv[at+1],&id)){puts("sudo: unknown target user or group.");return 2;}
            if(v[1]=='u')target=id;else {target_group=id;group_set=1;}at++;
        }
        else if(!strcmp(v,"-h") || !strcmp(v,"--help")) {
            puts("Usage: sudo [-n] [-S] [-k] [-u USER] [-g GROUP] [--] command [arguments]\n       sudo -v | -l | -k | -s | -i\nAdministrator access and password are configured in Settings > Profiles.");return 0;
        } else {puts("sudo: unsupported option (use sudo --help)");return 2;}
    }
    unsigned original_gid=call0(SYS_getgid);
    unsigned uid=call0(SYS_getuid);struct os_account_request q={.uid=uid};
    if(uid && call0(SYS_geteuid)!=0) {puts("sudo: effective uid is not 0; set-ID execution is disabled or sudo is not installed setuid root.");return 1;}
    if(forget && uid)call2(SYS_account,OS_ACCOUNT_FORGET,&q);
    if(forget && at==argc && !shell && !validate && !list)return 0;
    if(at==argc && !shell && !validate && !list){puts("sudo: specify a command (use sudo --help)");return 2;}
    int op=(validate || list)?OS_ACCOUNT_VALIDATE:OS_ACCOUNT_SUDO;
    int e=0;
    if(uid) {
        e=call2(SYS_account,OS_ACCOUNT_QUERY,&q);
        if(e || !(q.flags&OS_ACCOUNT_ADMIN) || !(q.flags&OS_ACCOUNT_PASSWORD)) {
            puts("sudo: administrator access and a profile password are required.");return 1;
        }
        e=call2(SYS_account,op,&q);
        if(e && !noninteractive) {
            for(int attempt=0;attempt<3;attempt++) {
                e=password(q.password,stdin_password);if(e)break;
                e=call2(SYS_account,op,&q);erase(q.password,sizeof(q.password));
                if(!e || e==-E_AGAIN || e==-E_PERM)break;
                puts("sudo: incorrect password.");
            }
        }
        if(e) {erase(&q,sizeof(q));puts(e==-E_AGAIN?"sudo: too many attempts; try again in 30 seconds.":"sudo: authentication required or access denied.");return 1;}
    }
    erase(&q,sizeof(q));
    if(validate)return 0;
    if(list){puts("Administrator policy: run commands as root. Authentication is required; timeout is set per profile.");return 0;}
    for(unsigned fd=3;fd<OS_FD_MAX;fd++)close(fd);
    char uid_text[32]="SUDO_UID=",gid_text[32]="SUDO_GID=",user_text[64]="SUDO_USER=";
    decimal(uid_text+9,uid);decimal(gid_text+9,original_gid);username(user_text+10,uid);
    char home[OS_PATH_MAX]="/root",home_env[OS_PATH_MAX+8]="HOME=",user_env[64]="USER=",logname_env[64]="LOGNAME=";
    if(target==1000)strcpy(home,"/home/user");
    else if(target) {if(target>1000 && target<1032){strcpy(home,"/home/profile-");decimal(home+14,target-1000);}else strcpy(home,"/");}
    strcpy(home_env+5,home);username(user_env+5,target);username(logname_env+8,target);
    char *env[]={"PATH=/usr/bin:/bin",home_env,user_env,logname_env,"SHELL=/usr/bin/linux-bash","TERM=xterm","LANG=C.UTF-8",uid_text,gid_text,user_text,NULL};
    if(shell==2 && call1(SYS_chdir,home)) {puts("sudo: target home directory is unavailable.");return 1;}
    unsigned gid=group_set?target_group:target;
    if(call2(SYS_setgroups,0,0) || call3(SYS_setresgid,gid,gid,gid) || call3(SYS_setresuid,target,target,target)) {puts("sudo: cannot set target credentials.");return 1;}
    char *shell_args[]={"/usr/bin/linux-bash",shell==2?"--login":"--noprofile","-i",NULL};
    char *command_args[argc+8];char **args=argv+at;
    if(shell && at<argc) {
        command_args[0]="/usr/bin/linux-bash";command_args[1]=shell==2?"--login":"--noprofile";
        command_args[2]="-c";command_args[3]="exec \"$@\"";command_args[4]="sudo-shell";
        int n=5;for(int i=at;i<argc;i++)command_args[n++]=argv[i];command_args[n]=NULL;args=command_args;
    } else if(shell)args=shell_args;
    char path[OS_PATH_MAX];const char *program=args[0];
    if(program[0]!='/' && !has_slash(program)) {
        const char *dirs[]={"/usr/bin/","/bin/"};int found=0;
        for(unsigned i=0;i<2;i++) {
            if(strlen(program)+strlen(dirs[i])>=sizeof(path))return 127;
            strcpy(path,dirs[i]);strcpy(path+strlen(path),program);struct os_stat st;
            if(!stat(path,&st) && (st.mode&0170000)==S_IFREG){found=1;break;}
        }
        if(!found){puts("sudo: command not found in /usr/bin or /bin");return 127;}program=path;
    }
    e=call3(SYS_execve,program,args,env);
    printf("sudo: %s: %s\n",program,error_text(e));return e==-E_NOENT?127:126;
}
