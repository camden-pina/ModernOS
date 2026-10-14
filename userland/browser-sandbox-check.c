#include <libc.h>

#define CHECK(e) do { if (!(e)) { printf("SANDBOX FAIL line %u: %s\n",__LINE__,#e); return 1; } } while (0)

static int put(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd>=0);
    CHECK(!write_all(fd,value,strlen(value)));
    CHECK(!close(fd));
    return 0;
}

static int directory(const char *path) {
    long e = call2(SYS_mkdir, path, 0700);
    CHECK(!e || e==-E_EXIST);
    return 0;
}

static int child(int local) {
    struct os_stat st;
    CHECK(fstat(127,&st)==-E_BADF);
    CHECK(open("/home/user/sandbox-secret",O_RDONLY,0)==-E_ACCES);
    CHECK(open("/home/user",O_RDONLY|O_DIRECTORY,0)==-E_ACCES);
    CHECK(open("/etc/ssl/cert.pem",O_WRONLY,0)==-E_ACCES);
    CHECK(open("/usr/share/netsurf/escape",O_RDONLY,0)==-E_ACCES);
    CHECK(open("/home/user/Downloads/../../sandbox-secret",O_RDONLY,0)==-E_ACCES);
    CHECK(open("/home/user/Downloads/escape/sandbox-secret",O_RDONLY,0)==-E_ACCES);
    CHECK(open("/home/user/Downloads/escape/new-secret",O_CREAT|O_WRONLY,0600)==-E_ACCES);
    CHECK(call2(SYS_mkdir,"/home/user/Downloads/escape/new-dir",0700)==-E_ACCES);
    CHECK(open("/home/user/Downloads/secret-link",O_RDWR|O_TRUNC,0600)==-E_ACCES);
    CHECK(call2(SYS_truncate,"/home/user/Downloads/secret-link",0)==-E_ACCES);
    CHECK(call1(SYS_unlink,"/home/user/sandbox-secret")==-E_ACCES);
    CHECK(call1(SYS_unlink,"/dev/null")==-E_ACCES);
    CHECK(call2(SYS_chmod,"/home/user/sandbox-secret",0777)==-E_PERM);
    CHECK(call2(SYS_symlink,"/home/user/sandbox-secret","/home/user/Downloads/link")==-E_PERM);
    CHECK(call0(SYS_fork)==-E_PERM && call0(SYS_vfork)==-E_PERM);
    CHECK(syscall6(SYS_clone,17,0,0,0,0,0)==-E_PERM);
    char *args[] = {"/bin/hello", NULL};
    CHECK(call3(SYS_execve,args[0],args,0)==-E_PERM);
    CHECK(spawn(args[0],args,1)==-E_PERM);
    CHECK(call3(SYS_spawn_browser,args[0],args,0)==-E_PERM);
    CHECK(call2(SYS_browser_grant,call0(SYS_getpid),"/home/user/sandbox-secret")==-E_PERM);
    CHECK(call2(SYS_kill,call0(SYS_getppid),9)==-E_PERM);
    CHECK(call3(SYS_display,DISPLAY_ACQUIRE,0,0)==-E_PERM);
    CHECK(call2(SYS_input,0,0)==-E_PERM);
    CHECK(syscall6(SYS_mount,0,0,0,0,0,0)==-E_PERM);
    CHECK(call0(9999)==-E_PERM);
    int socket = call3(SYS_socket, 2, 1, 0);
    CHECK(socket>=0);
    struct {
        uint16_t family, port;
        uint32_t address;
        uint8_t rest[8];
    } bound = {.family = 2, .port = 256};
    CHECK(call3(SYS_bind,socket,&bound,sizeof(bound))==-E_PERM);
    bound.port = 0;
    CHECK(!call3(SYS_bind,socket,&bound,sizeof(bound)));
    CHECK(call2(SYS_listen,socket,1)==-E_PERM);
    CHECK(call3(SYS_accept,socket,0,0)==-E_PERM);
    CHECK(!close(socket));
    uint8_t random[32];
    CHECK(call3(SYS_getrandom,random,sizeof(random),0)==sizeof(random));
    int ca = open("/etc/ssl/cert.pem", O_RDONLY, 0);
    CHECK(ca>=0);
    CHECK(!close(ca));
    int asset = open("/usr/share/netsurf/modernos.html", O_RDONLY, 0);
    CHECK(asset>=0);
    CHECK(!close(asset));
    CHECK(!directory("/home/user/.config/netsurf"));
    int fd = open("/home/user/.config/netsurf/SandboxTest", O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd>=0 && write(fd,"cookie",6)==6);
    CHECK(!close(fd));
    CHECK(!call1(SYS_unlink,"/home/user/.config/netsurf/SandboxTest"));
    fd = open("/home/user/Downloads/.sandbox.part", O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd>=0 && write(fd,"download",8)==8);
    CHECK(!fsync(fd));
    CHECK(!close(fd));
    CHECK(call2(SYS_rename,"/home/user/Downloads/.sandbox.part","/home/user/Downloads/escape/replaced")==-E_ACCES);
    CHECK(!rename_noreplace("/home/user/Downloads/.sandbox.part","/home/user/Downloads/sandbox-result"));
    CHECK(!call1(SYS_unlink,"/home/user/Downloads/sandbox-result"));
    if (!local)
        CHECK(open("/home/user/SandboxDocs/page.html",O_RDONLY,0)==-E_ACCES);
    if (local) {
        fd = open("/home/user/SandboxDocs/page.html", O_RDONLY, 0);
        CHECK(fd>=0);
        CHECK(!close(fd));
        CHECK(open("/home/user/SandboxDocs/escape",O_RDONLY,0)==-E_ACCES);
        CHECK(open("/home/user/SandboxDocs/page.html",O_WRONLY,0)==-E_ACCES);
    }

    const long size = 300L * 1024 * 1024;
    uint64_t limit[2];
    CHECK(!call2(SYS_getrlimit,9,limit));
    CHECK(limit[0]==512L*1024*1024 && limit[1]==limit[0]);
    long memory = syscall6(SYS_mmap, 0, size, 3, 0x22, -1, 0);
    CHECK(memory>0);
    CHECK(syscall6(SYS_mmap,0,size,3,0x22,-1,0)==-E_NOMEM);
    CHECK(!call2(SYS_munmap,memory,size));
    memory = syscall6(SYS_mmap, 0, size, 3, 0x22, -1, 0);
    CHECK(memory>0);
    CHECK(!call2(SYS_munmap,memory,size));
    puts("SANDBOX_CHILD_READY");
    char command;
    CHECK(read(0,&command,1)==1);
    if (command == 'g') {
        fd = open("/home/user/SandboxOther/page.html", O_RDONLY, 0);
        CHECK(fd>=0);
        CHECK(!close(fd));
        CHECK(open("/home/user/SandboxDocs/page.html",O_RDONLY,0)==-E_ACCES);
    }

    puts("SANDBOX CHILD PASS: filesystem, symlinks, descriptors, syscall policy, memory limit");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1)
        return child(!strcmp(argv[1], "--local"));
    CHECK(!directory("/home"));
    CHECK(!directory("/home/user"));
    CHECK(!directory("/home/user/Downloads"));
    CHECK(!directory("/home/user/SandboxDocs"));
    CHECK(!directory("/home/user/SandboxOther"));
    CHECK(!put("/home/user/sandbox-secret","PRIVATE_DATA"));
    CHECK(!put("/home/user/SandboxDocs/page.html","<title>Selected document</title>"));
    CHECK(!put("/home/user/SandboxOther/page.html","<title>Another document</title>"));
    CHECK(!call2(SYS_symlink,"/home/user","/home/user/Downloads/escape"));
    CHECK(!call2(SYS_symlink,"/home/user/sandbox-secret","/home/user/Downloads/secret-link"));
    CHECK(!call2(SYS_symlink,"/home/user/sandbox-secret","/home/user/SandboxDocs/escape"));
    CHECK(!call2(SYS_symlink,"/home/user/sandbox-secret","/usr/share/netsurf/escape"));
    CHECK(!call2(SYS_symlink,"/home/user","/home/sandbox-user"));
    int secret = open("/home/user/sandbox-secret", O_RDONLY, 0);
    CHECK(secret>=0);
    CHECK(call2(SYS_dup2,secret,127)==127);
    CHECK(!close(secret));
    for (int local = 0; local < 2; local++) {
        int in[2], out[2];
        CHECK(!call2(SYS_pipe2,in,O_CLOEXEC));
        CHECK(!call2(SYS_pipe2,out,O_CLOEXEC));
        char *args[] = {"/bin/browser-sandbox-check", local ? "--local" : "--child", NULL};
        struct os_spawn_browser spec = {
            .io = {.fd = {in[0], out[1], out[1]},
                   .cwd = (uintptr_t)(local ? "/home/sandbox-user" : "/home/user")},
            .local_file = local ? (uintptr_t)"/home/user/SandboxDocs/page.html" : 0};
        int pid = call3(SYS_spawn_browser, args[0], args, &spec);
        CHECK(pid>0);
        CHECK(!close(in[0]) && !close(out[1]));
        char line[256];
        while (read_line(out[0], line, sizeof(line)) > 0) {
            puts(line);
            if (!strcmp(line, "SANDBOX_CHILD_READY")) {
                if (local)
                    CHECK(!call2(SYS_browser_grant,pid,"/home/user/SandboxOther/page.html"));
                CHECK(write(in[1],local?"g":"n",1)==1);
            }
        }

        CHECK(!close(in[1]) && !close(out[0]));
        int status = -1;
        CHECK(waitpid(pid,&status,0)==pid && !status);
    }

    CHECK(!close(127));
    char value[32];
    secret = open("/home/user/sandbox-secret", O_RDONLY, 0);
    CHECK(secret>=0);
    CHECK(read(secret,value,sizeof(value))==12 && !memcmp(value,"PRIVATE_DATA",12));
    CHECK(!close(secret));
    CHECK(!call1(SYS_unlink,"/usr/share/netsurf/escape"));
    puts("BROWSER SANDBOX CHECK PASS");
    return 0;
}
