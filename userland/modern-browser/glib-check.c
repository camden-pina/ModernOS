#define _GNU_SOURCE
#include <gio/gio.h>
#include <gio/gunixfdmessage.h>
#include <gio/gunixfdlist.h>
#include <gio/gunixinputstream.h>
#include <glib-unix.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL glib-check:%d: %s (errno=%d)\n",__LINE__,#x,errno); _exit(1); } } while (0)

static GPrivate local = G_PRIVATE_INIT(NULL);
static GMainLoop *loop;
static GMainContext *context;
static GThread *owner;
static gint callbacks;

static gboolean finish(gpointer argument) {
    CHECK(argument==context && g_thread_self()==owner && g_main_context_is_owner(context));
    callbacks++;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static gpointer producer(gpointer argument) {
    CHECK(g_private_get(&local)==NULL);
    g_private_set(&local, argument);
    g_usleep(3000);
    CHECK(g_private_get(&local)==argument);
    g_main_context_invoke_full(context, G_PRIORITY_DEFAULT, finish, context, NULL);
    return argument;
}

static gboolean expired(gpointer argument) {
    (void)argument;
    CHECK(0 && "event loop deadline");
    return G_SOURCE_REMOVE;
}

static void event_loop(void) {
    owner = g_thread_self();
    context = g_main_context_new();
    loop = g_main_loop_new(context, FALSE);
    CHECK(g_main_context_acquire(context));
    GSource *timeout = g_timeout_source_new(5000);
    g_source_set_callback(timeout, expired, NULL, NULL);
    g_source_attach(timeout, context);
    GThread *thread = g_thread_new("glib-producer", producer, context);
    CHECK(thread);
    g_main_loop_run(loop);
    CHECK(g_thread_join(thread)==context && callbacks==1 && !g_private_get(&local));
    g_source_destroy(timeout);
    g_source_unref(timeout);
    g_main_context_release(context);
    g_main_loop_unref(loop);
    g_main_context_unref(context);
    puts("PASS GLib thread/TLS and real cross-thread main-context wakeup/dispatch");
}

static gint calculate(GObject *object, gint a, gint b, gpointer argument) {
    CHECK(G_IS_OBJECT(object));
    return a * b + GPOINTER_TO_INT(argument);
}

static void objects_regex(void) {
    guint signal = g_signal_new("kernel-ffi-calc", G_TYPE_OBJECT, G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                g_cclosure_marshal_generic, G_TYPE_INT, 2, G_TYPE_INT, G_TYPE_INT);
    CHECK(signal);
    GObject *object = g_object_new(G_TYPE_OBJECT, NULL);
    g_signal_connect(object, "kernel-ffi-calc", G_CALLBACK(calculate), GINT_TO_POINTER(5));
    gint value = 0;
    g_signal_emit(object, signal, 0, 7, 6, &value);
    CHECK(value==47);
    g_object_unref(object);
    GError *error = NULL;
    GRegex *regex = g_regex_new("(\\p{L}+)=(\\d+)", 0, 0, &error);
    CHECK(regex && !error);
    GMatchInfo *match = NULL;
    CHECK(g_regex_match(regex,"café=42",0,&match));
    gchar *text = g_match_info_fetch(match, 1);
    CHECK(!strcmp(text,"café"));
    g_free(text);
    text = g_match_info_fetch(match, 2);
    CHECK(!strcmp(text,"42"));
    g_free(text);
    g_match_info_free(match);
    g_regex_unref(regex);
    puts("PASS GObject generic libffi signal marshalling and genuine PCRE2 Unicode regex");
}

static void sockets(void) {
    int stream[2];
    CHECK(pipe(stream)==0);
    GInputStream *input = g_unix_input_stream_new(stream[0], TRUE);
    CHECK(g_pollable_input_stream_can_poll(G_POLLABLE_INPUT_STREAM(input)));
    CHECK(write(stream[1],"pollable",8)==8);
    char pipe_data[8];
    GError *pipe_error = NULL;
    CHECK(g_pollable_input_stream_is_readable(G_POLLABLE_INPUT_STREAM(input)));
    CHECK(g_pollable_input_stream_read_nonblocking(G_POLLABLE_INPUT_STREAM(input),pipe_data,8,NULL,&pipe_error)==8 && !pipe_error);
    CHECK(!memcmp(pipe_data,"pollable",8));
    close(stream[1]);
    g_object_unref(input);
    int pair[2];
    CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair)==0);
    GError *error = NULL;
    GSocket *a = g_socket_new_from_fd(pair[0], &error);
    CHECK(a && !error);
    GSocket *b = g_socket_new_from_fd(pair[1], &error);
    CHECK(b && !error);
    CHECK(g_socket_get_family(a)==G_SOCKET_FAMILY_UNIX && g_socket_get_socket_type(a)==G_SOCKET_TYPE_SEQPACKET);
    int fd = memfd_create("gio-rights", MFD_CLOEXEC);
    CHECK(fd>=0 && ftruncate(fd,4096)==0);
    uint32_t *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(map!=MAP_FAILED);
    map[0] = 0x7351;
    GUnixFDList *list = g_unix_fd_list_new();
    CHECK(g_unix_fd_list_append(list,fd,&error)==0 && !error);
    GSocketControlMessage *control = g_unix_fd_message_new_with_fd_list(list);
    GOutputVector out = {"WebKit-Gio-IPC", 14};
    CHECK(g_socket_send_message(a,NULL,&out,1,&control,1,0,NULL,&error)==14 && !error);
    g_object_unref(control);
    g_object_unref(list);
    close(fd);
    CHECK(g_socket_condition_check(b,G_IO_IN)&G_IO_IN);
    char data[32] = {0};
    GInputVector in = {data, sizeof(data)};
    GSocketControlMessage **controls = NULL;
    gint count = 0, flags = 0;
    CHECK(g_socket_receive_message(b,NULL,&in,1,&controls,&count,&flags,NULL,&error)==14 && !error && count==1);
    CHECK(!strcmp(data,"WebKit-Gio-IPC") && G_IS_UNIX_FD_MESSAGE(controls[0]));
    gint n = 0;
    gint *received = g_unix_fd_message_steal_fds(G_UNIX_FD_MESSAGE(controls[0]), &n);
    CHECK(n==1);
    uint32_t word = 0;
    CHECK(pread(received[0],&word,4,0)==4 && word==map[0]);
    word = 0x9827;
    CHECK(pwrite(received[0],&word,4,4)==4 && map[1]==word);
    close(received[0]);
    g_free(received);
    g_object_unref(controls[0]);
    g_free(controls);
    CHECK(munmap(map,4096)==0);
    g_object_unref(a);
    g_object_unref(b);
    puts("PASS genuine Gio Unix sockets/control messages and shared-memory descriptor coherence");
}

static void read_complete(GObject *object, GAsyncResult *result, gpointer argument) {
    CHECK(argument==loop);
    GError *error = NULL;
    GBytes *bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(object), result, &error);
    CHECK(bytes && !error);
    gsize length = 0;
    const char *text = g_bytes_get_data(bytes, &length);
    CHECK(length==11 && !memcmp(text,"async-bytes",11));
    g_bytes_unref(bytes);
    g_main_loop_quit(loop);
}

static void async_io(void) {
    loop = g_main_loop_new(NULL, FALSE);
    GInputStream *input = g_memory_input_stream_new_from_data("async-bytes", 11, NULL);
    g_input_stream_read_bytes_async(input, 32, G_PRIORITY_DEFAULT, NULL, read_complete, loop);
    guint timeout = g_timeout_add(5000, expired, NULL);
    g_main_loop_run(loop);
    g_source_remove(timeout);
    g_object_unref(input);
    g_main_loop_unref(loop);
    puts("PASS real Gio asynchronous task completion on its main loop");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(25);
    event_loop();
    objects_regex();
    sockets();
    async_io();
    puts("GLIB CHECK PASS");
    return 0;
}
