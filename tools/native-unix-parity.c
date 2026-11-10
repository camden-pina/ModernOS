/* Same static ELF runs only in disposable Linux and Coast guests. */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <grp.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trace(const char *name, long result) {
    printf("UNIX %s %ld\n", name, result); fflush(stdout);
}
static long result(long value) { return value < 0 ? -errno : value; }
static void drop(unsigned uid) {
    if (setgroups(0, NULL) || setgid(uid) || setuid(uid)) _exit(90);
}
static struct sockaddr_un address(const char *path) {
    struct sockaddr_un a = {.sun_family=AF_UNIX};
    strcpy(a.sun_path, path); return a;
}
static int listener(const char *path) {
    int s=socket(AF_UNIX, SOCK_STREAM, 0); struct sockaddr_un a=address(path);
    if(s<0 || bind(s,(void *)&a,sizeof(a)) || listen(s,4)) _exit(91);
    return s;
}
static int connect_path(const char *path) {
    int s=socket(AF_UNIX, SOCK_STREAM, 0); struct sockaddr_un a=address(path);
    long r=result(connect(s,(void *)&a,sizeof(a))); close(s); return r;
}
static void probe(unsigned n) {
    if(n==0) {
        drop(1000); umask(0077);
        int s=listener("/tmp/unix-owner/live"); struct stat st;
        stat("/tmp/unix-owner/live",&st);
        trace("socket-mode",st.st_mode&0777);trace("socket-owner",st.st_uid==1000 && st.st_gid==1000);
        pid_t server=getpid(),pid=fork();
        if(!pid) {
            int c=socket(AF_UNIX,SOCK_STREAM,0);struct sockaddr_un a=address("/tmp/unix-owner/live");
            if(connect(c,(void *)&a,sizeof(a)))_exit(92);
            struct ucred peer={0};socklen_t len=sizeof(peer);
            char ok=!getsockopt(c,SOL_SOCKET,SO_PEERCRED,&peer,&len) && peer.pid==server && peer.uid==1000 && peer.gid==1000;
            if(write(c,&ok,1)!=1 || read(c,&ok,1)!=1 || ok!='Y')_exit(93);
            close(c);_exit(0);
        }
        int c=accept(s,NULL,NULL);struct ucred peer={0};socklen_t len=sizeof(peer);char ok=0;
        trace("server-peer",!getsockopt(c,SOL_SOCKET,SO_PEERCRED,&peer,&len) && peer.pid==pid && peer.uid==1000 && peer.gid==1000);
        trace("client-peer",read(c,&ok,1)==1 && ok==1);
        write(c,"Y",1);close(c);close(s);int status=0;waitpid(pid,&status,0);
        trace("exchange",WIFEXITED(status) && WEXITSTATUS(status)==0);
    } else if(n==1) {
        umask(0);int s=listener("/tmp/unix-owner/foreign");chmod("/tmp/unix-owner/foreign",0777);
        pid_t pid=fork();if(!pid){drop(1001);trace("private-directory",connect_path("/tmp/unix-owner/foreign"));_exit(0);}
        waitpid(pid,NULL,0);close(s);
    } else if(n==2) {
        umask(0);int s=listener("/tmp/unix-public/readonly");chown("/tmp/unix-public/readonly",1000,1000);chmod("/tmp/unix-public/readonly",0600);
        pid_t pid=fork();if(!pid){drop(1001);trace("socket-write-dac",connect_path("/tmp/unix-public/readonly"));_exit(0);}
        waitpid(pid,NULL,0);close(s);
    } else if(n==3) {
        drop(1000);int s=socket(AF_UNIX,SOCK_STREAM,0);struct sockaddr_un a=address("/tmp/unix-root/no");
        trace("bind-directory-dac",result(bind(s,(void *)&a,sizeof(a))));close(s);
    } else if(n==4) {
        umask(0);int s=listener("/tmp/unix-owner/symlink");chmod("/tmp/unix-owner/symlink",0777);
        symlink("/tmp/unix-owner","/tmp/unix-public/alias");
        pid_t pid=fork();if(!pid){drop(1001);trace("canonical-directory-dac",connect_path("/tmp/unix-public/alias/symlink"));_exit(0);}
        waitpid(pid,NULL,0);close(s);
    } else if(n==5) {
        drop(1000);int a=listener("/tmp/unix-owner/rebind");unlink("/tmp/unix-owner/rebind");
        int b=listener("/tmp/unix-owner/rebind");
        trace("unlink-rebind",connect_path("/tmp/unix-owner/rebind"));
        int accepted=accept(b,NULL,NULL);trace("new-inode-listener",accepted>=0);close(accepted);close(a);close(b);
    } else if(n==6) {
        drop(1000);int s=socket(AF_UNIX,SOCK_STREAM,0);struct sockaddr_un a=address("/tmp/unix-owner/stale");
        bind(s,(void *)&a,sizeof(a));close(s);
        trace("closed-socket",connect_path("/tmp/unix-owner/stale"));
    } else if(n==7) {
        drop(1000);pid_t pid=fork();
        if(!pid){pid_t id=getpid();trace("daemon-setsid",setsid()==id);trace("session-id",getsid(0)==id);trace("group-id",getpgrp()==id);trace("leader-denied",result(setsid()));_exit(0);}
        waitpid(pid,NULL,0);
    } else if(n==8) {
        drop(1000);int s=listener("/tmp/unix-owner/permission");chmod("/tmp/unix-owner/permission",0000);
        trace("owner-no-write",connect_path("/tmp/unix-owner/permission"));close(s);
    } else if(n==9) {
        umask(0);int s=listener("/tmp/unix-public/link-target");chmod("/tmp/unix-public/link-target",0777);
        symlink("/tmp/unix-public/link-target","/tmp/unix-public/leaf");
        pid_t pid=fork();if(!pid){drop(1000);trace("leaf-symlink",connect_path("/tmp/unix-public/leaf"));_exit(0);}
        waitpid(pid,NULL,0);close(s);
    } else if(n==10) {
        umask(0);int s=listener("/tmp/unix-public/root-listener");chmod("/tmp/unix-public/root-listener",0777);
        pid_t server=getpid(),pid=fork();
        if(!pid) {
            drop(1001);int c=socket(AF_UNIX,SOCK_STREAM,0);struct sockaddr_un a=address("/tmp/unix-public/root-listener");
            if(connect(c,(void *)&a,sizeof(a)))_exit(94);
            struct ucred peer={0};socklen_t len=sizeof(peer);char ok=!getsockopt(c,SOL_SOCKET,SO_PEERCRED,&peer,&len) && peer.pid==server && peer.uid==0 && peer.gid==0;
            write(c,&ok,1);close(c);_exit(0);
        }
        if(seteuid(1000))_exit(95);
        int c=accept(s,NULL,NULL);struct ucred peer={0};socklen_t len=sizeof(peer);char ok=0;
        trace("cross-uid-peer",!getsockopt(c,SOL_SOCKET,SO_PEERCRED,&peer,&len) && peer.pid==pid && peer.uid==1001 && peer.gid==1001);
        trace("listen-credential-snapshot",read(c,&ok,1)==1 && ok==1);
        close(c);close(s);waitpid(pid,NULL,0);
    } else if(n==11) {
        int s=listener("/tmp/unix-root/inherited");int c=socket(AF_UNIX,SOCK_STREAM,0);struct sockaddr_un a=address("/tmp/unix-root/inherited");
        if(connect(c,(void *)&a,sizeof(a)))_exit(96);
        int accepted=accept(s,NULL,NULL);pid_t pid=fork();
        if(!pid){drop(1001);char value='Z';_exit(write(c,&value,1)==1?0:97);}
        char value=0;trace("inherited-fd-authority",read(accepted,&value,1)==1 && value=='Z');
        waitpid(pid,NULL,0);close(c);close(accepted);close(s);
    }
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    mkdir("/tmp",01777);chmod("/tmp",01777);
    mkdir("/tmp/unix-owner",0700);chown("/tmp/unix-owner",1000,1000);
    mkdir("/tmp/unix-root",0755);mkdir("/tmp/unix-public",0777);chmod("/tmp/unix-public",0777);
    for(unsigned n=0;n<12;n++) {
        pid_t pid=fork();if(!pid){probe(n);_exit(0);}int status=0;waitpid(pid,&status,0);
        trace("case-exit",WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status));
    }
    puts("NATIVE_UNIX_PARITY_DONE");fflush(stdout);
    if(getpid()==1)for(;;)pause();
    return 0;
}
