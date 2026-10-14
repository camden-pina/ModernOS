#define _GNU_SOURCE
#include "../include/web_process.h"
#include <libsoup/soup.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL web-soup-check:%d: %s (errno=%d)\n",__LINE__,#x,errno); exit(1); } } while (0)

void g_io_openssl_load(GIOModule *module);

static GBytes *fetch(SoupSession *session, const char *base, const char *path, unsigned status,
                     int reject) {
    char *url = g_strconcat(base, path, NULL);
    SoupMessage *message = soup_message_new("GET", url);
    CHECK(message);
    GError *error = NULL;
    GBytes *body = soup_session_send_and_read(session, message, NULL, &error);
    if (reject) {
        CHECK(!body && error && g_error_matches(error,G_TLS_ERROR,G_TLS_ERROR_BAD_CERTIFICATE));
        printf("PASS rejected certificate: %s (%s)\n", url, error->message);
        g_error_free(error);
    } else {
        if (error)
            fprintf(stderr, "Soup error: %s: %s\n", url, error->message);
        CHECK(body && !error && soup_message_get_status(message)==status);
    }
    g_object_unref(message);
    g_free(url);
    return body;
}

static void payload(SoupSession *session, const char *base, const char *path) {
    GBytes *body = fetch(session, base, path, 200, 0);
    gsize size;
    const unsigned char *data = g_bytes_get_data(body, &size);
    CHECK(size==190123);
    for (gsize i = 0; i < size; i++)
        CHECK(data[i]==(unsigned char)(i*31+7));
    g_bytes_unref(body);
}

static int broker(char **args) {
    struct os_web_identity identity;
    CHECK(syscall(MODERNOS_WEB_IDENTITY,&identity)==0 && identity.role==OS_WEB_BROKER);
    errno = 0;
    CHECK(open("/tmp/soup-escape",O_WRONLY|O_CREAT,0600)==-1 && errno==EACCES);
    g_io_openssl_load(NULL);
    CHECK(g_tls_backend_supports_tls(g_tls_backend_get_default()));
    CHECK(strstr(G_OBJECT_TYPE_NAME(g_tls_backend_get_default()),"Openssl"));
    SoupSession *session = soup_session_new();
    g_object_set(session, "timeout", 20, NULL);
    payload(session, args[0], "large.bin");
    payload(session, args[0], "chunked");
    payload(session, args[0], "redirect");
    GBytes *failed = fetch(session, args[0], "fail", 503, 0);
    g_bytes_unref(failed);
    puts("PASS real Soup broker HTTP bytes, chunks, redirect and explicit HTTP failure");
    fetch(session, args[1], "page.html", 0, 1);
    GBytes *ca = fetch(session, args[0], "ca.pem", 200, 0);
    gsize length;
    const char *bytes = g_bytes_get_data(ca, &length);
    const char *directory = "/home/user/.config/webkit";
    CHECK(mkdir(directory,0700)==0 || errno==EEXIST);
    const char *path = "/home/user/.config/webkit/soup-fixture-ca.pem";
    GError *error = NULL;
    CHECK(g_file_set_contents(path,bytes,length,&error));
    g_bytes_unref(ca);
    GTlsDatabase *database = g_tls_file_database_new(path, &error);
    if (error)
        fprintf(stderr, "TLS database: %s\n", error->message);
    CHECK(database && !error);
    g_object_set(session, "tls-database", database, NULL);
    g_object_unref(database);
    payload(session, args[1], "large.bin");
    fetch(session, args[2], "page.html", 0, 1);
    fetch(session, args[3], "page.html", 0, 1);
    soup_session_abort(session);
    g_object_unref(session);
    CHECK(unlink(path)==0);
    puts(
        "PASS genuine statically registered OpenSSL Gio TLS backend, trusted body, wrong-host and untrusted rejection");
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(140);
    CHECK(argc==5 || (argc==6 && !strcmp(argv[1],"--broker")));
    if (argc == 6)
        return broker(argv + 2);
    CHECK(mkdir("/home",0755)==0 || errno==EEXIST);
    CHECK(mkdir("/home/user",0700)==0 || errno==EEXIST);
    CHECK(mkdir("/home/user/.config",0700)==0 || errno==EEXIST);
    int ipc[2], output[2];
    CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,ipc)==0 && pipe(output)==0);
    char *args[] = {"web-soup-check", "--broker", argv[1], argv[2], argv[3], argv[4], NULL};
    struct os_spawn_web spec = {.role = OS_WEB_BROKER,
                                .ipc_fd = ipc[1],
                                .stdout_fd = output[1],
                                .stderr_fd = output[1],
                                .home = (uintptr_t)"/home/user"};
    int pid = syscall(MODERNOS_SPAWN_WEB, "/bin/web-soup-check", args, &spec);
    CHECK(pid>0);
    close(ipc[1]);
    close(output[1]);
    char text[1024];
    ssize_t n;
    while ((n = read(output[0], text, sizeof(text))) > 0)
        CHECK(write(1,text,n)==n);
    CHECK(n==0);
    close(output[0]);
    close(ipc[0]);
    int status;
    CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
    puts("SOUP BROKER CHECK PASS");
    return 0;
}
