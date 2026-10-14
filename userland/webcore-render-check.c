#define _GNU_SOURCE
#include "include/web_process.h"
#include "include/browser_protocol.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL webcore-render-check:%d: %s (errno=%d)\n",__LINE__,#x,errno); exit(1); } } while (0)

_Static_assert(sizeof(struct browser_packet) == 40, "preserve browser surface protocol");
static atomic_int stop_worker;
static int diagnostic_fd = -1;
static void copy(int fd);

static void *worker(void *unused) {
    (void)unused;
    while (!atomic_load(&stop_worker))
        usleep(1000);
    return NULL;
}

static void exact(int fd, void *target, size_t size) {
    char *bytes = target;
    while (size) {
        ssize_t n = read(fd, bytes, size);
        if (n <= 0 && diagnostic_fd >= 0) {
            copy(diagnostic_fd);
            diagnostic_fd = -1;
        }
        CHECK(n>0);
        bytes += n;
        size -= n;
    }
}

static int launch(unsigned role, const char *path, char *const args[], int ipc, int output,
                  int errors, const char *origin) {
    struct os_spawn_web spec = {.role = role,
                                .ipc_fd = ipc,
                                .stdout_fd = output,
                                .stderr_fd = errors,
                                .origin = (uintptr_t)origin,
                                .home = role == OS_WEB_BROKER ? (uintptr_t)"/home/user" : 0};
    return syscall(MODERNOS_SPAWN_WEB, path, args, &spec);
}

static void copy(int fd) {
    char text[1024];
    ssize_t n;
    while ((n = read(fd, text, sizeof(text))) > 0)
        CHECK(write(1,text,n)==n);
    CHECK(n==0);
    close(fd);
}

static void wait_ok(int pid) {
    int status;
    CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
}

static void color(const uint32_t *pixels, unsigned x, unsigned y, uint32_t value) {
    uint32_t actual = pixels[y * 320 + x] & 0xffffff;
    if (actual != value)
        fprintf(stderr, "pixel (%u,%u): actual=%06x expected=%06x\n", x, y, actual, value);
    CHECK(actual==value);
}

static void frame(int fd, unsigned index) {
    struct browser_packet packet;
    exact(fd, &packet, sizeof(packet));
    CHECK(packet.magic==BROWSER_MAGIC && packet.type==BROWSER_FRAME && packet.x==0 && packet.y==0);
    CHECK(packet.width==320 && packet.height==240 && packet.canvas_width==320 && packet.canvas_height==240);
    CHECK(packet.bytes==320*240*4 && packet.bytes<=BROWSER_MAX_PIXELS*4);
    uint32_t *pixels = malloc(packet.bytes);
    CHECK(pixels);
    exact(fd, pixels, packet.bytes);
    color(pixels, 5, 5, 0xfafafa);
    color(pixels, 25, 25, 0xe02030);
    color(pixels, 75, 25, 0x2040e0);
    color(pixels, 225, 25, 0xd030c0);
    color(pixels, 145, 105, index ? 0x00cc44 : 0xffcc00);
    unsigned ink = 0;
    for (unsigned y = 170; y < 230; y++)
        for (unsigned x = 20; x < 310; x++) {
            uint32_t p = pixels[y * 320 + x];
            if (((p >> 16) & 255) < 100 && ((p >> 8) & 255) < 100 && (p & 255) < 100)
                ink++;
        }
    CHECK(ink>100);
    free(pixels);
}

static int broker(void) {
    struct os_web_identity own;
    CHECK(syscall(MODERNOS_WEB_IDENTITY,&own)==0 && own.role==OS_WEB_BROKER);
    const char *origins[] = {"https://alpha.example:443", "https://beta.example:443"};
    int ipc[2][2], frames[2][2], errors[2][2], pid[2];
    pthread_t live;
    CHECK(pthread_create(&live,NULL,worker,NULL)==0);
    for (unsigned i = 0; i < 2; i++) {
        CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,ipc[i])==0);
        CHECK(pipe(frames[i])==0 && pipe(errors[i])==0);
        char *args[] = {"web-renderer", "--fixture", (char *)origins[i], NULL};
        pid[i] = launch(OS_WEB_RENDERER, "/bin/web-renderer", args, ipc[i][1], frames[i][1],
                        errors[i][1], origins[i]);
        CHECK(pid[i]>0);
        close(ipc[i][1]);
        close(frames[i][1]);
        close(errors[i][1]);
    }

    CHECK(pid[0]!=pid[1]);
    for (unsigned i = 0; i < 2; i++) {
        diagnostic_fd = errors[i][0];
        struct os_web_identity identity;
        exact(ipc[i][0], &identity, sizeof(identity));
        CHECK(identity.process==(unsigned)pid[i] && identity.domain==own.domain && identity.role==OS_WEB_RENDERER);
        CHECK(!strcmp(identity.origin,origins[i]));
        frame(frames[i][0], 0);
        frame(frames[i][0], 1);
        copy(errors[i][0]);
        diagnostic_fd = -1;
        wait_ok(pid[i]);
        close(frames[i][0]);
        close(ipc[i][0]);
    }

    atomic_store(&stop_worker, 1);
    CHECK(pthread_join(live,NULL)==0);
    puts(
        "PASS genuine WebCore Grid/canvas/text pixels, promise DOM and mouse click in two immutable-origin renderer processes");
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(180);
    if (argc == 2 && !strcmp(argv[1], "--broker"))
        return broker();
    CHECK(argc==1);
    CHECK(mkdir("/home",0755)==0 || errno==EEXIST);
    CHECK(mkdir("/home/user",0700)==0 || errno==EEXIST);
    CHECK(mkdir("/home/user/.config",0700)==0 || errno==EEXIST);
    int ipc[2], output[2];
    CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,ipc)==0 && pipe(output)==0);
    char *args[] = {"webcore-render-check", "--broker", NULL};
    int pid = launch(OS_WEB_BROKER, "/bin/webcore-render-check", args, ipc[1], output[1], output[1],
                     NULL);
    CHECK(pid>0);
    close(ipc[1]);
    close(output[1]);
    copy(output[0]);
    wait_ok(pid);
    close(ipc[0]);
    puts("WEBCORE RENDER CHECK PASS (controlled fixture; production browser remains unfinished)");
    return 0;
}
