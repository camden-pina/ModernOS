#include <libc.h>
#include <uapi/account.h>
/* Invoked by the trusted desktop before accepting any terminal input. */
int main(int argc,char **argv) {
    if((argc!=4 && argc!=5) || call0(SYS_getuid)!=0)return 1;
    unsigned uid=number(argv[1],10);
    if(uid<OS_ACCOUNT_FIRST || uid>=OS_ACCOUNT_FIRST+OS_ACCOUNT_COUNT)return 1;
    int use_bash=!strcmp(argv[2],"bash"),use_qt=!strcmp(argv[2],"qt6");
    if(!use_bash && !use_qt && strcmp(argv[2],"native"))return 1;
    unsigned qt_scale=argc==5?(unsigned)number(argv[4],10):200;
    if((argc==5 && !use_qt) || qt_scale<50 || qt_scale>400)return 1;
    struct os_account_request q={.uid=uid};
    for(unsigned fd=3;fd<OS_FD_MAX;fd++)close(fd);
    int e=call2(SYS_account,OS_ACCOUNT_ENTER,&q);
    if(e) {printf("Session could not enter account: %s\n",error_text(e));return 1;}
    char home[OS_PATH_MAX+8],user[40],logname[40];
    if(strlen(argv[3])>OS_PATH_MAX-1)return 1;
    strcpy(home,"HOME=");strcpy(home+5,argv[3]);
    if(uid==OS_ACCOUNT_FIRST){strcpy(user,"USER=user");strcpy(logname,"LOGNAME=user");}
    else {char id[16];unsigned n=0,v=uid-OS_ACCOUNT_FIRST;do{id[n++]='0'+v%10;v/=10;}while(v);strcpy(user,"USER=profile-");char *p=user+13;while(n)*p++=id[--n];*p=0;strcpy(logname,"LOGNAME=");strcpy(logname+8,user+5);}
    char scale[32]="QT_SCALE_FACTOR=2.00";scale[16]='0'+qt_scale/100;scale[18]='0'+qt_scale/10%10;scale[19]='0'+qt_scale%10;
    char *env[]={home,user,logname,"PATH=/usr/bin:/bin","TERM=xterm","LANG=C.UTF-8",NULL,scale,NULL};
    char *native[]={"/bin/coast-sh","--terminal",NULL};
    char *bash[]={"/usr/bin/linux-bash","--noprofile","--norc","-i",NULL};
    char *qt[]={"/usr/bin/qt6-demo",NULL};
    env[6]=use_bash?"SHELL=/usr/bin/linux-bash":"SHELL=/bin/coast-sh";
    /* Native spawn already makes this process a group leader. Bash therefore
       skips its own setpgid(), so enroll its managed job-control semantics
       explicitly before it can fork foreground or background commands. */
    if(use_bash && (e=call2(SYS_setpgid,0,0))) {
        printf("Session job control failed: %s\n",error_text(e));return 1;
    }
    e=call3(SYS_execve,use_qt?qt[0]:use_bash?bash[0]:native[0],use_qt?qt:use_bash?bash:native,env);
    printf("Session shell failed: %s\n",error_text(e));return 1;
}
