#include <libc.h>

#define CHECK(expr) do { if (!(expr)) { puts("FAIL: " #expr); return 1; } } while (0)

static uint8_t zero[8192];

static int copy_check(void) {
    long source = syscall6(SYS_mmap, 0, 12288, 3, 0x22, -1, 0);
    long destination = syscall6(SYS_mmap, 0, 12288, 3, 0x22, -1, 0);
    CHECK(source>0 && destination>0);
    uint8_t *s = (void *)source, *d = (void *)destination;
    CHECK(!call3(SYS_mprotect,source+8192,4096,0));
    CHECK(!call3(SYS_mprotect,destination+8192,4096,0));
    for (unsigned i = 0; i < 8192; i++)
        s[i] = (uint8_t)(i * 37 + 1);
    for (unsigned a = 0; a < 16; a++)
        for (unsigned b = 0; b < 16; b++)
            for (unsigned n = 0; n <= 160; n++) {
                memset(d, 0x9b, 192);
                CHECK(memcpy(d+a,s+b,n)==d+a);
                for (unsigned i = 0; i < 192; i++)
                    CHECK(d[i]==(i>=a && i-a<n?s[b+i-a]:0x9b));
            }
    const unsigned lengths[] = {0,  1,  7,  8,   15,  16,  17,  31,  32,   33,
                                63, 64, 65, 127, 128, 129, 255, 256, 4095, 4096};
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(*lengths); i++) {
        unsigned n = lengths[i];
        for (unsigned shift = 0; shift < 16; shift++) {
            CHECK(memcpy(d+8192-n-shift,s+8192-n,n)==d+8192-n-shift);
            CHECK(!memcmp(d+8192-n-shift,s+8192-n,n));
            CHECK(memcpy(d+8192-n,s+8192-n-shift,n)==d+8192-n);
            CHECK(!memcmp(d+8192-n,s+8192-n-shift,n));
        }
    }

    CHECK(!call2(SYS_munmap,source,12288) && !call2(SYS_munmap,destination,12288));
    puts("LIBC COPY CHECK PASS: all alignments, lengths and inaccessible page boundaries");
    return 0;
}

int main(int argc, char **argv) {
    CHECK(argc>=1 && argv[0] && !argv[argc]);
    if (argc == 2 && !strcmp(argv[1], "--child"))
        return 27;
    if (argc == 2 && !strcmp(argv[1], "--copy"))
        return copy_check();
    if (argc == 2 && !strcmp(argv[1], "--pipe-child")) {
        char in[8];
        CHECK(read(0,in,sizeof(in))==5);
        CHECK(!memcmp(in,"hello",5));
        CHECK(write(1,"world",5)==5);
        CHECK(write(2,"!",1)==1);
        return 23;
    }

    if (argc == 2 && !strcmp(argv[1], "--tty-denied")) {
        char c;
        CHECK(read(0,&c,1)==-E_IO);
        struct os_termios mode;
        CHECK(!ioctl(0,TCGETS,&mode));
        CHECK(ioctl(0,TCSETS,&mode)==-E_PERM);
        CHECK(call3(SYS_display,DISPLAY_ACQUIRE,0,0)==-E_PERM);

        struct os_input_event event;
        CHECK(call2(SYS_input,&event,1)==-E_PERM);
        return 0;
    }

    if (argc == 2 && !strcmp(argv[1], "--tty-read")) {
        char input[64];
        puts("TTY_READ_READY");
        long n = read(0, input, sizeof(input) - 1);
        CHECK(n>=0);
        input[n] = 0;
        printf("TTY_READ_OK: %s", input);
        return 0;
    }

    if (argc == 2 && (!strcmp(argv[1], "--display-exit") || !strcmp(argv[1], "--display-fault"))) {

        CHECK(!call3(SYS_display,DISPLAY_ACQUIRE,0,0));
        struct os_termios mode;
        CHECK(!ioctl(0,TCGETS,&mode));
        mode.lflag = 0;
        mode.cc[VMIN] = 0;
        CHECK(!ioctl(0,TCSETSF,&mode));
        if (!strcmp(argv[1], "--display-fault"))
            __asm__ volatile("ud2");
        return 0; /* Intentionally leave display ownership and raw mode active. */
    }

    if (argc == 3 && !strcmp(argv[1], "--storage")) {
        char path[OS_PATH_MAX];
        strcpy(path, argv[2]);
        size_t n = strlen(path);
        CHECK(n+16<sizeof(path));
        strcpy(path + n, "/busy-test");
        int held = open(path, O_CREAT | O_RDWR, 0644);
        CHECK(held>=0);
        CHECK(call2(SYS_umount2,argv[2],0)==-E_BUSY);
        CHECK(call2(SYS_umount2,argv[2],1)==-E_INVAL);
        CHECK(!close(held));
        CHECK(!call1(SYS_unlink,path));
        CHECK(!call1(SYS_chdir,argv[2]));
        CHECK(call2(SYS_umount2,argv[2],0)==-E_BUSY);
        CHECK(!call1(SYS_chdir,"/"));
        puts("STORAGE SAFETY PASS");
        return 0;
    }

    unsigned cs;
    __asm__ volatile("mov %%cs,%0" : "=r"(cs));
    CHECK((cs&3)==3);
    CHECK(!copy_check());
    for (unsigned i = 0; i < sizeof(zero); i++)
        CHECK(!zero[i]);

    CHECK(call3(SYS_write,1,1,16)==-E_FAULT);
    CHECK(call3(SYS_read,0,0xffffffff80000000ULL,4)==-E_FAULT);
    CHECK(call0(9999)==-E_NOSYS);
    CHECK(call0(SYS_getpid)>1);
    CHECK(call0(SYS_getppid)==1);

    struct os_utsname os;
    CHECK(!call1(SYS_uname,&os));
    CHECK(!strcmp(os.sysname,"ModernOS"));
    CHECK(call1(SYS_uname,1)==-E_FAULT);
    CHECK(call0(SYS_gettid)==call0(SYS_getpid));
    CHECK(call0(SYS_getuid)==0);

    uint64_t fsbase = 123;
    CHECK(!call2(SYS_arch_prctl,0x1003,&fsbase));
    CHECK(!fsbase);
    CHECK(call2(SYS_arch_prctl,0x1002,0xffffffff80000000ULL)==-E_PERM);
    CHECK(!call2(SYS_arch_prctl,0x1002,0x12340000));
    CHECK(!call2(SYS_arch_prctl,0x1003,&fsbase));
    CHECK(fsbase==0x12340000);
    CHECK(!call2(SYS_arch_prctl,0x1002,0));

    int fd = open("/tmp/syscall-test", O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd>=0);
    CHECK(write(fd,"abcdef",6)==6);
    CHECK(lseek(fd,2,0)==2);
    char data[8] = {0};
    CHECK(read(fd,data,3)==3);
    CHECK(!memcmp(data,"cde",3));
    struct os_stat st;
    CHECK(!fstat(fd,&st));
    CHECK(st.size==6);

    CHECK(syscall6(SYS_pread64,fd,(long)data,2,0,0,0)==2);
    CHECK(!memcmp(data,"ab",2));
    CHECK(lseek(fd,0,1)==5);
    CHECK(syscall6(SYS_pwrite64,fd,(long)"XY",2,0,0,0)==2);
    CHECK(lseek(fd,0,1)==5);
    CHECK(syscall6(SYS_pread64,fd,(long)data,1,-1,0,0)==-E_INVAL);

    int duplicate = call1(SYS_dup, fd);
    CHECK(duplicate>=0);
    CHECK(lseek(duplicate,1,0)==1);
    CHECK(lseek(fd,0,1)==1);
    CHECK(!call3(SYS_fcntl,fd,F_SETFD,FD_CLOEXEC));
    CHECK(call2(SYS_fcntl,duplicate,F_GETFD)==0);
    CHECK(call2(SYS_fcntl,fd,F_GETFD)==FD_CLOEXEC);
    CHECK(!close(duplicate));
    CHECK(call3(SYS_dup3,fd,fd,O_CLOEXEC)==-E_INVAL);
    CHECK(!call3(SYS_fcntl,fd,F_SETFD,0));

    struct os_iovec vectors[2] = {{(uintptr_t)"12", 2}, {(uintptr_t)"34", 2}};
    CHECK(lseek(fd,0,0)==0);
    CHECK(call3(SYS_writev,fd,vectors,2)==4);
    vectors[0] = (struct os_iovec){(uintptr_t)data, 2};
    vectors[1] = (struct os_iovec){(uintptr_t)data + 2, 2};
    CHECK(lseek(fd,0,0)==0);
    CHECK(call3(SYS_readv,fd,vectors,2)==4);
    CHECK(!memcmp(data,"1234",4));
    vectors[1].base = 1;
    CHECK(call3(SYS_writev,fd,vectors,2)==-E_FAULT);
    CHECK(call3(SYS_writev,fd,vectors,17)==-E_INVAL);

    CHECK(!fsync(fd));
    CHECK(ioctl(fd,TCGETS,data)==-E_NOTTY);
    CHECK(!call2(SYS_fchmod,fd,0600));
    CHECK(!close(fd));
    CHECK(!call1(SYS_unlink,"/tmp/syscall-test"));

    int dir = open("/tmp", O_RDONLY | O_DIRECTORY, 0);
    CHECK(dir>=0);
    fd = openat(dir, "at-test", O_CREAT | O_RDWR, 0600);
    CHECK(fd>=0);
    CHECK(write(fd,"at",2)==2);
    CHECK(!syscall6(SYS_newfstatat,dir,(long)"at-test",(long)&st,0,0,0));
    CHECK(st.size==2);
    CHECK(!close(fd));
    CHECK(!call3(SYS_unlinkat,dir,"at-test",0));
    CHECK(!close(dir));
    CHECK(openat(99,"relative",O_RDONLY,0)==-E_BADF);

    int initial_readonly = open("/tmp/created-readonly", O_WRONLY | O_CREAT | O_EXCL, 0444);
    CHECK(initial_readonly>=0);
    CHECK(write(initial_readonly,"initialized",11)==11);
    CHECK(!close(initial_readonly));
    CHECK(open("/tmp/created-readonly",O_WRONLY,0)==-E_ACCES);
    CHECK(!call1(SYS_unlink,"/tmp/created-readonly"));

    int pipefd[2];
    CHECK(call2(SYS_pipe2,1,0)==-E_FAULT);
    CHECK(call2(SYS_pipe2,pipefd,O_APPEND)==-E_INVAL);
    CHECK(!call2(SYS_pipe2,pipefd,O_CLOEXEC|O_NONBLOCK));
    CHECK(call2(SYS_fcntl,pipefd[0],F_GETFD)==FD_CLOEXEC);
    CHECK(read(pipefd[0],data,sizeof(data))==-E_AGAIN);
    CHECK(write(pipefd[1],"pipe",4)==4);
    CHECK(read(pipefd[0],data,sizeof(data))==4);
    CHECK(!memcmp(data,"pipe",4));
    CHECK(lseek(pipefd[0],0,0)==-E_SPIPE);
    CHECK(!fstat(pipefd[0],&st));
    CHECK((st.mode&0170000)==S_IFIFO);
    CHECK(call3(SYS_getdents64,pipefd[0],zero,sizeof(zero))==-E_NOTDIR);
    int pipe_copy = call1(SYS_dup, pipefd[1]);
    CHECK(pipe_copy>=0);
    CHECK(!close(pipefd[1]));
    CHECK(read(pipefd[0],data,1)==-E_AGAIN);
    CHECK(!close(pipe_copy));
    CHECK(read(pipefd[0],data,1)==0);
    CHECK(!close(pipefd[0]));
    CHECK(!call2(SYS_pipe2,pipefd,0));
    CHECK(!close(pipefd[0]));
    CHECK(write(pipefd[1],"x",1)==-E_PIPE);
    CHECK(!close(pipefd[1]));

    int input_pipe[2], output_pipe[2];
    CHECK(!call2(SYS_pipe2,input_pipe,O_CLOEXEC));
    CHECK(!call2(SYS_pipe2,output_pipe,O_CLOEXEC));
    char *pipe_args[] = {"/bin/check", "--pipe-child", NULL};
    struct os_spawn_io streams = {.fd = {input_pipe[0], output_pipe[1], output_pipe[1]},
                                  .cwd = (uintptr_t)"/tmp"};
    CHECK(call3(SYS_spawn_io,"/bin/check",pipe_args,1)==-E_FAULT);
    int pipe_pid = call3(SYS_spawn_io, "/bin/check", pipe_args, &streams);
    CHECK(pipe_pid>0);
    CHECK(!close(input_pipe[0]));
    CHECK(!close(output_pipe[1]));
    CHECK(write(input_pipe[1],"hello",5)==5);
    CHECK(!close(input_pipe[1]));
    size_t received = 0;
    long got;
    while ((got = read(output_pipe[0], data + received, sizeof(data) - received)) > 0)
        received += got;
    CHECK(!got && received==6);
    CHECK(!memcmp(data,"world!",6));
    CHECK(!close(output_pipe[0]));
    int pipe_status = 0;
    CHECK(waitpid(pipe_pid,&pipe_status,0)==pipe_pid);
    CHECK(pipe_status==(23<<8));
    CHECK(call2(SYS_umount2,"/",0)==-E_BUSY);

    struct os_termios terminal;
    CHECK(!ioctl(0,TCGETS,&terminal));
    CHECK((terminal.lflag&(ICANON|ECHO))==(ICANON|ECHO));
    CHECK(ioctl(0,TCGETS,(void *)1)==-E_FAULT);
    struct os_winsize size;
    CHECK(!ioctl(0,TIOCGWINSZ,&size));
    CHECK(size.rows && size.cols);

    long old = call1(SYS_brk, 0);
    CHECK(old>0);
    CHECK(call1(SYS_brk,old+8192)==old+8192);
    *(volatile char *)old = 42;
    CHECK(call1(SYS_brk,old)==old);

    long mem = syscall6(SYS_mmap, 0, 8192, 3, 0x22, -1, 0);
    CHECK(mem>0);
    CHECK(!*(volatile char *)mem);
    *(volatile char *)mem = 77;
    CHECK(!call3(SYS_mprotect,mem,8192,1));
    CHECK(call3(SYS_read,0,mem,1)==-E_FAULT);
    CHECK(!call3(SYS_mprotect,mem,8192,3));
    CHECK(call3(SYS_mprotect,mem,8192,7)==-E_INVAL);
    CHECK(!call2(SYS_munmap,mem,8192));
    CHECK(call3(SYS_write,1,mem,1)==-E_FAULT);
    CHECK(syscall6(SYS_mmap,0,4096,7,0x22,-1,0)==-E_INVAL);

    struct os_display_info display;
    CHECK(!call3(SYS_display,DISPLAY_INFO,&display,sizeof(display)));
    CHECK(display.version==OS_DISPLAY_VERSION && display.width && display.height);
    CHECK(call3(SYS_display,DISPLAY_INFO,1,sizeof(display))==-E_FAULT);
    struct os_display_caps caps;
    CHECK(!call3(SYS_display,DISPLAY_CAPS,&caps,sizeof(caps)));
    CHECK(caps.buffers>=1 && caps.backend<=DISPLAY_BACKEND_QEMU_VBE && !caps.reserved);
    CHECK(!(caps.flags&DISPLAY_CAP_PAGE_FLIP) || caps.buffers>=2);
    CHECK(call3(SYS_display,DISPLAY_CAPS,1,sizeof(caps))==-E_FAULT);
    CHECK(!call3(SYS_display,DISPLAY_ACQUIRE,0,0));
    CHECK(call3(SYS_display,DISPLAY_ACQUIRE,0,0)==-E_BUSY);
    uint32_t pixel = 0x123456;
    struct os_display_rect rect = {0, 0, 1, 1, (uintptr_t)&pixel};
    CHECK(!call3(SYS_display,DISPLAY_PRESENT,&rect,sizeof(rect)));
    rect.x = display.width;
    CHECK(call3(SYS_display,DISPLAY_PRESENT,&rect,sizeof(rect))==-E_INVAL);
    rect.x = 0;
    rect.pixels = 1;
    CHECK(call3(SYS_display,DISPLAY_PRESENT,&rect,sizeof(rect))==-E_FAULT);
    rect.width = UINT32_MAX;
    CHECK(call3(SYS_display,DISPLAY_PRESENT,&rect,sizeof(rect))==-E_INVAL);

    struct os_display_batch batch = {
        .pixels = (uintptr_t)&pixel, .stride = display.width, .count = 1, .rects = {{0, 0, 1, 1}}};
    CHECK(!call3(SYS_display,DISPLAY_PRESENT_BATCH,&batch,sizeof(batch)));
    CHECK(!call3(SYS_display,DISPLAY_PRESENT_BATCH,&batch,OS_DISPLAY_LEGACY_BATCH_SIZE));
    batch.count = 33;
    CHECK(call3(SYS_display,DISPLAY_PRESENT_BATCH,&batch,OS_DISPLAY_LEGACY_BATCH_SIZE)==-E_INVAL);
    batch.count = OS_DISPLAY_MAX_RECTS + 1;
    CHECK(call3(SYS_display,DISPLAY_PRESENT_BATCH,&batch,sizeof(batch))==-E_INVAL);
    batch.count = 1;
    batch.rects[0].x = display.width;
    CHECK(call3(SYS_display,DISPLAY_PRESENT_BATCH,&batch,sizeof(batch))==-E_INVAL);
    batch.rects[0].x = 0;
    batch.pixels = 1;
    CHECK(call3(SYS_display,DISPLAY_PRESENT_BATCH,&batch,sizeof(batch))==-E_FAULT);
    batch.pixels = UINT64_MAX;
    batch.rects[0].x = 1;
    CHECK(call3(SYS_display,DISPLAY_PRESENT_BATCH,&batch,sizeof(batch))==-E_FAULT);

    size_t canvas_size = (size_t)display.width * display.height * 4;
    long canvas = syscall6(SYS_mmap, 0, canvas_size, 3, 0x22, -1, 0);
    CHECK(canvas>0);
    batch.pixels = canvas;
    batch.rects[0].x = 0;

    const uint64_t simd_expected[2] = {0x1122334455667788ULL, 0x8877665544332211ULL};
    uint64_t simd_observed[4];
    register long display_number __asm__("rax") = SYS_display;
    register long display_op __asm__("rdi") = DISPLAY_PRESENT_SWAP;
    register long display_arg __asm__("rsi") = (long)&batch;
    register long display_size __asm__("rdx") = sizeof(batch);
    __asm__ volatile("movdqu %[in],%%xmm0; movdqu %[in],%%xmm3; syscall; "
                     "movdqu %%xmm0,%[out0]; movdqu %%xmm3,%[out3]"
                     : "+a"(display_number), "+D"(display_op), "+S"(display_arg),
                       "+d"(display_size), [out0] "=m"(*(uint64_t (*)[2])simd_observed),
                       [out3] "=m"(*(uint64_t (*)[2])(simd_observed + 2))
                     : [in] "m"(simd_expected)
                     : "rcx", "r11", "xmm0", "xmm3", "memory", "cc");
    CHECK(!display_number && !memcmp(simd_expected,simd_observed,16) &&
        !memcmp(simd_expected,simd_observed+2,16));
    CHECK(!call3(SYS_mprotect,canvas+4096,4096,0));
    CHECK(call3(SYS_display,DISPLAY_PRESENT_SWAP,&batch,sizeof(batch))==-E_FAULT);
    CHECK(!call3(SYS_mprotect,canvas+4096,4096,3));
    CHECK(!call3(SYS_display,DISPLAY_PRESENT_SWAP,&batch,sizeof(batch)));
    CHECK(!call2(SYS_munmap,canvas,canvas_size));
    CHECK(call3(SYS_display,DISPLAY_PRESENT_SWAP,&batch,sizeof(batch))==-E_FAULT);
    struct os_input_event event;
    CHECK(call2(SYS_input,&event,1)==-E_AGAIN);
    CHECK(call2(SYS_input,1,1)==-E_FAULT);
    CHECK(call2(SYS_input,&event,65)==-E_INVAL);
    CHECK(!call3(SYS_display,DISPLAY_RELEASE,0,0));
    CHECK(call2(SYS_input,&event,1)==-E_PERM);

    struct os_timespec start, end, delay = {0, 30000000};
    CHECK(!call2(SYS_clock_gettime,1,&start));
    CHECK(!call2(SYS_nanosleep,&delay,0));
    CHECK(!call2(SYS_clock_gettime,1,&end));
    CHECK((end.sec-start.sec)*1000000000+end.nsec-start.nsec>=30000000);

    const uint64_t expected[2] = {0x0123456789abcdefULL, 0xfedcba9876543210ULL};
    uint64_t observed[2];
    register long nr __asm__("rax") = SYS_sched_yield;
    __asm__ volatile("movdqu %[in],%%xmm15; syscall; movdqu %%xmm15,%[out]"
                     : [out] "=m"(observed), "+a"(nr)
                     : [in] "m"(expected)
                     : "rcx", "r11", "xmm15", "memory", "cc");
    CHECK(!memcmp(expected,observed,sizeof(expected)));

    for (unsigned i = 0; i < 24; i++) {
        char *args[] = {"/bin/check", "--child", NULL};
        int pid = spawn(args[0], args, 1), status = 0;
        CHECK(pid>0);
        CHECK(waitpid(pid,&status,0)==pid);
        CHECK(status==(27<<8));
    }

    puts(
        "USERLAND CHECK PASS: ring3, pointers, file/at/vector I/O, dup, FS base, terminal, display, W^X, scheduling");
    return 0;
}
