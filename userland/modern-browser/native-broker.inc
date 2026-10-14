/* Trusted native navigation and bounded, nonblocking surface relay. Each
 * renderer remains bound to one immutable origin for its entire lifetime. */
typedef struct {
    char *url, *metadata;
    struct web_resource_packet response;
    GBytes *body;
    int pid, token;
    char *method, *content_type;
    GBytes *request_body;
} NativeHistory;

typedef struct {
    uint64_t frame, parent;
    int local;
    GBytes *name;
    char *origin;
} NativeFrameNode;

struct NativeBrowser {
    Broker broker;
    int pid, frames, errors, width, height, quit;
    int screen_width, screen_height, available_height;
    int window_x, window_y, window_width, window_height;
    char *pending, *current, *requested, *referrer;
    NativeHistory history[128], candidate;
    unsigned count, cursor, hops, key_modifiers;
    int traversing, restoring, pending_client_navigation;
    struct browser_packet input;
    char input_text[BROWSER_MAX_TEXT + 1];
    size_t input_used, input_text_used;
    GByteArray *incoming;
    size_t expected;
    GQueue output;
    GBytes *writing;
    size_t written, queued_bytes;
    GHashTable *session_storage;
    sqlite3 *storage;
    SoupCookieJar *cookies;
    NativeBrowser *parent, *children[WEB_FRAME_MAX_CHILDREN];
    uint64_t frame_id, root_frame, focused_frame;
    unsigned sandbox;
    int awaiting_surface, surface_dirty;
    GBytes *surface;
    int surface_width, surface_height;
    NativeFrameNode *tree;
    unsigned tree_count;
    uint64_t tree_revision, tree_export_revision;
    GBytes *tree_export;
    GHashTable *tree_names;
    char *committed_origin, *tree_url_origin;
};

static GBytes *native_tree_export(NativeBrowser *);
static void native_tree_clear(NativeBrowser *);
#include "native-services.inc"
#include "native-downloads.inc"

static int native_command(NativeBrowser *, const struct browser_packet *, const char *);
static void native_destroy_children(NativeBrowser *);
static void native_child_surface(NativeBrowser *, const struct browser_packet *, const void *);
static void native_tick_children(NativeBrowser *);

static const char *native_first_party(Broker *broker) {
    NativeBrowser *view = broker->native;
    if (!view)
        return broker->origin;
    while (view->parent)
        view = view->parent;
    return view->broker.origin;
}

/* A document beneath a foreign site has an opaque cookie site, even when
 * it fetches its top document's origin with credentials. Derive this from
 * the trusted frame graph rather than a renderer-supplied first-party URL. */
static GUri *native_cookie_site(Broker *broker) {
    GUri *site = g_uri_parse(native_first_party(broker), G_URI_FLAGS_ENCODED, NULL);
    if (!site)
        return NULL;
    const char *root_host = g_uri_get_host(site), *root_scheme = g_uri_get_scheme(site);
    const char *root_domain = root_host ? soup_tld_get_base_domain(root_host, NULL) : NULL;
    if (!root_host || !root_scheme) {
        g_uri_unref(site);
        return NULL;
    }
    if (!root_domain)
        root_domain = root_host;
    for (NativeBrowser *view = broker->native; view; view = view->parent) {
        GUri *uri = g_uri_parse(view->broker.origin, G_URI_FLAGS_ENCODED, NULL);
        const char *host = uri ? g_uri_get_host(uri) : NULL,
                   *scheme = uri ? g_uri_get_scheme(uri) : NULL;
        const char *domain = host ? soup_tld_get_base_domain(host, NULL) : NULL;
        if (!domain)
            domain = host;
        int same = domain && scheme && !g_ascii_strcasecmp(domain, root_domain) &&
                   !g_ascii_strcasecmp(scheme, root_scheme);
        if (uri)
            g_uri_unref(uri);
        if (!same) {
            g_uri_unref(site);
            return NULL;
        }
    }

    return site;
}

static void native_history_clear(NativeHistory *entry) {
    g_free(entry->url);
    g_free(entry->metadata);
    g_clear_pointer(&entry->body, g_bytes_unref);
    g_free(entry->method);
    g_free(entry->content_type);
    g_clear_pointer(&entry->request_body, g_bytes_unref);
    memset(entry, 0, sizeof(*entry));
}

static void native_remember_response(Request *request, const struct web_resource_packet *response,
                                     const char *metadata, const void *body) {
    Broker *broker = request->broker;
    NativeBrowser *view = broker->native;
    native_history_clear(&view->candidate);
    view->candidate.url = g_strndup(metadata, response->url_bytes);
    view->candidate.method = g_strdup(soup_message_get_method(request->message));
    view->candidate.content_type = g_strdup(soup_message_headers_get_one(
        soup_message_get_request_headers(request->message), "Content-Type"));
    /* A bounded in-memory document cache restores POST results without
     * changing their method or submitting a form again during Back/Forward. */
    if (response->body_bytes > 4 * 1024 * 1024 || request->packet.body_bytes > 4 * 1024 * 1024)
        return;
    size_t total = response->body_bytes +
                   (request->request_body ? g_bytes_get_size(request->request_body) : 0);
    for (unsigned i = 0; i < view->count; i++)
        total +=
            view->history[i].response.body_bytes +
            (view->history[i].request_body ? g_bytes_get_size(view->history[i].request_body) : 0);
    for (unsigned i = 0; total > 16 * 1024 * 1024 && i < view->count; i++) {
        NativeHistory *entry = &view->history[i];
        total -= entry->response.body_bytes +
                 (entry->request_body ? g_bytes_get_size(entry->request_body) : 0);
        entry->response.body_bytes = 0;
        g_clear_pointer(&entry->body, g_bytes_unref);
        g_clear_pointer(&entry->metadata, g_free);
        g_clear_pointer(&entry->request_body, g_bytes_unref);
    }

    view->candidate.response = *response;
    view->candidate.metadata = g_memdup2(metadata, response->metadata_bytes);
    view->candidate.body = g_bytes_new(body, response->body_bytes);
    if (request->request_body)
        view->candidate.request_body = g_bytes_ref(request->request_body);
}

static int native_restore_response(Broker *broker, const struct web_resource_packet *request,
                                   const char *url, const char *method) {
    NativeBrowser *view = broker->native;
    if (!view->restoring || !(request->flags & WEB_RESOURCE_MAIN))
        return 0;
    NativeHistory *entry = &view->history[view->cursor];
    if (strcmp(entry->url, url))
        return 0;
    view->restoring = 0;
    if (!entry->metadata || !entry->body) {
        if (entry->method && strcmp(entry->method, "GET")) {
            send_error(broker, request->id, url,
                       "This form result is no longer cached. Submit the form again to reopen it.");
            return 1;
        }
        return 0;
    }

    if (entry->method && strcmp(method, entry->method)) {
        send_error(broker, request->id, url,
                   "History request method does not match its saved document.");
        return 1;
    }
    struct web_resource_packet packet = entry->response;
    packet.id = request->id;
    /* A replay has no new network transaction. Never report the previous
     * navigation's monotonic measurements as the current load's timings. */
    packet.metadata_bytes -= packet.metrics_bytes;
    packet.metrics_bytes = 0;
    gsize size;
    const void *bytes = g_bytes_get_data(entry->body, &size);
    int fd = size ? web_resource_body(bytes, size) : -1;
    if (size && fd < 0) {
        send_error(broker, request->id, url, "History document allocation failed");
        return 1;
    }
    if (web_resource_send(broker->channel, &packet, entry->metadata, fd) < 0)
        broker->broken = 1;
    if (fd >= 0)
        close(fd);
    ++broker->received;
    fprintf(stderr, "History document: status=%u bytes=%u %s\n", packet.status, packet.body_bytes,
            url);
    return 1;
}

static unsigned native_history_flags(NativeBrowser *view) {
    return (view->count && view->cursor ? BROWSER_CAN_BACK : 0) |
           (view->count && view->cursor + 1 < view->count ? BROWSER_CAN_FORWARD : 0);
}

static int native_packet_valid(const struct browser_packet *p) {
    if (p->magic != BROWSER_MAGIC)
        return 0;
    if (p->type == BROWSER_FRAME)
        return browser_surface_rect_valid(p) && (uint64_t)p->width * p->height * 4 == p->bytes;
    if (p->type == BROWSER_COPY)
        return browser_surface_rect_valid(p) && !p->flags &&
               p->bytes == sizeof(struct browser_copy_source);
    if (p->type == BROWSER_PRESENT)
        return browser_surface_present_valid(p);
    if (p->type == BROWSER_STATE)
        return !p->bytes && !(p->flags & ~7u);
    return (p->type == BROWSER_TITLE || p->type == BROWSER_URL || p->type == BROWSER_NOTICE ||
            p->type == BROWSER_STATUS) &&
           p->bytes <= BROWSER_MAX_TEXT;
}

static void native_queue_text(NativeBrowser *view, unsigned type, const char *text) {
    if (view->parent)
        return;
    struct browser_packet p = {
        .magic = BROWSER_MAGIC, .type = type, .bytes = text ? (uint32_t)strlen(text) : 0};
    GByteArray *buffer = g_byte_array_sized_new(sizeof(p) + p.bytes);
    g_byte_array_append(buffer, (void *)&p, sizeof(p));
    if (p.bytes)
        g_byte_array_append(buffer, (void *)text, p.bytes);
    view->queued_bytes += buffer->len;
    g_queue_push_tail(&view->output, g_byte_array_free_to_bytes(buffer));
}

static void native_flush(NativeBrowser *view) {
    size_t budget = 4 * 1024 * 1024;
    while (budget && !view->quit) {
        if (!view->writing) {
            view->writing = g_queue_pop_head(&view->output);
            view->written = 0;
            if (!view->writing)
                return;
        }

        gsize size;
        const char *bytes = g_bytes_get_data(view->writing, &size);
        ssize_t n = write(1, bytes + view->written, MIN(budget, size - view->written));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (n <= 0) {
            view->quit = 1;
            return;
        }
        view->written += n;
        budget -= n;
        view->queued_bytes -= n;
        if (view->written == size)
            g_clear_pointer(&view->writing, g_bytes_unref);
    }
}

static void native_read_frames(NativeBrowser *view) {
    size_t budget = 4 * 1024 * 1024;
    while (view->frames >= 0 && budget && view->queued_bytes < 8 * 1024 * 1024) {
        if (!view->incoming) {
            view->incoming = g_byte_array_new();
            view->expected = sizeof(struct browser_packet);
        }
        size_t remaining = view->expected - view->incoming->len;
        unsigned char bytes[65536];
        ssize_t n = read(view->frames, bytes, MIN(sizeof(bytes), MIN(remaining, budget)));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (n <= 0) {
            close(view->frames);
            view->frames = -1;
            return;
        }
        budget -= n;
        g_byte_array_append(view->incoming, bytes, n);
        if (view->incoming->len != view->expected)
            continue;
        struct browser_packet packet;
        memcpy(&packet, view->incoming->data, sizeof(packet));
        int valid = native_packet_valid(&packet);
        if (view->parent && (packet.type == BROWSER_FRAME || packet.type == BROWSER_PRESENT)) {
            valid =
                packet.magic == BROWSER_MAGIC && !packet.x && !packet.y && !packet.flags &&
                packet.canvas_width > 0 && packet.canvas_height > 0 &&
                packet.canvas_width <= 4096 && packet.canvas_height <= 4096 &&
                (uint64_t)packet.canvas_width * packet.canvas_height <= WEB_FRAME_MAX_BODY / 4 &&
                (packet.type == BROWSER_PRESENT
                     ? !packet.bytes && !packet.width && !packet.height
                     : packet.width == (int)packet.canvas_width &&
                           packet.height == (int)packet.canvas_height &&
                           packet.bytes == (uint64_t)packet.width * packet.height * 4);
        }

        if (!valid || (view->parent && packet.type == BROWSER_COPY)) {
            view->quit = 1;
            return;
        }
        if (view->expected == sizeof(packet) && packet.bytes) {
            view->expected += packet.bytes;
            continue;
        }
        if (packet.type == BROWSER_COPY) {
            struct browser_copy_source source;
            memcpy(&source, view->incoming->data + sizeof(packet), sizeof(source));
            if (!browser_surface_copy_valid(&packet, &source)) {
                view->quit = 1;
                return;
            }
        }

        if (view->parent) {
            native_child_surface(view, &packet, view->incoming->data + sizeof(packet));
            g_clear_pointer(&view->incoming, g_byte_array_unref);
        } else {
            view->queued_bytes += view->incoming->len;
            g_queue_push_tail(&view->output, g_byte_array_free_to_bytes(view->incoming));
            view->incoming = NULL;
        }
    }
}

static void native_read_errors(NativeBrowser *view) {
    char bytes[4096];
    for (unsigned i = 0; view->errors >= 0 && i < 4; i++) {
        ssize_t n = read(view->errors, bytes, sizeof(bytes));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (n <= 0) {
            close(view->errors);
            view->errors = -1;
            return;
        }
        ssize_t written = 0;
        while (written < n) {
            ssize_t count = write(2, bytes + written, n - written);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return;
            written += count;
        }
    }
}

static void native_update_history(NativeBrowser *view, const char *url, int token, unsigned flags) {
    if (flags & BROWSER_HISTORY_TRAVERSE) {
        for (unsigned i = 0; i < view->count; i++)
            if (view->history[i].pid == view->pid && view->history[i].token == token) {
                view->cursor = i;
                view->traversing = 1;
                break;
            }
    }

    if ((flags & BROWSER_HISTORY_REPLACE) && view->count) {
        g_free(view->history[view->cursor].url);
        view->history[view->cursor].url = g_strdup(url);
        /* A replaced URL still has the live WebCore HistoryItem, but no
         * network response was fetched for its replacement address. */
        g_clear_pointer(&view->history[view->cursor].metadata, g_free);
        g_clear_pointer(&view->history[view->cursor].body, g_bytes_unref);
        g_clear_pointer(&view->history[view->cursor].request_body, g_bytes_unref);
        view->history[view->cursor].response.body_bytes = 0;
    }

    if (view->traversing) {
        view->traversing = 0;
        view->history[view->cursor].pid = view->pid;
        view->history[view->cursor].token = token;
        return;
    }

    if (!view->count || strcmp(view->history[view->cursor].url, url) ||
        (!(flags & BROWSER_HISTORY_REPLACE) && token &&
         view->history[view->cursor].pid == view->pid &&
         view->history[view->cursor].token != token)) {
        while (view->count && view->cursor + 1 < view->count)
            native_history_clear(&view->history[--view->count]);
        if (view->count == 128) {
            native_history_clear(&view->history[0]);
            memmove(view->history, view->history + 1, 127 * sizeof(NativeHistory));
            --view->count;
        }
        memset(&view->history[view->count], 0, sizeof(NativeHistory));
        view->history[view->count++].url = g_strdup(url);
        view->cursor = view->count - 1;
    }

    if (view->candidate.url && !strcmp(view->candidate.url, url)) {
        native_history_clear(&view->history[view->cursor]);
        view->history[view->cursor] = view->candidate;
        memset(&view->candidate, 0, sizeof(view->candidate));
    }
    view->history[view->cursor].pid = view->pid;
    view->history[view->cursor].token = token;
}

static int native_renderer_control(Broker *broker, const struct browser_packet *p,
                                   const char *text) {
    NativeBrowser *view = broker->native;
    if (view->parent && (p->type == BROWSER_BACK || p->type == BROWSER_FORWARD))
        return 1;
    if (p->type == BROWSER_BACK || p->type == BROWSER_FORWARD) {
        if (p->bytes || p->x < 1 || p->x > 128 || p->y || p->width || p->height ||
            p->canvas_width || p->canvas_height || p->flags)
            return 0;
        return native_command(view, p, NULL);
    }

    if ((p->type != BROWSER_NAVIGATE && p->type != BROWSER_URL) || !p->bytes || p->x < 0 || p->y ||
        p->width || p->height || p->canvas_width || p->canvas_height ||
        p->flags & ~(BROWSER_HISTORY_TRAVERSE | BROWSER_HISTORY_REPLACE) ||
        (p->type == BROWSER_NAVIGATE && (p->x || p->flags)))
        return 0;
    char *origin = origin_for(text);
    if (!origin)
        return 0;
    if (p->type == BROWSER_URL) {
        if (strcmp(origin, broker->origin)) {
            g_free(origin);
            return 0;
        }
        const char *url = !strcmp(origin, "https://modernos.invalid:443") ? "about:blank" : text;
        g_free(view->current);
        view->current = g_strdup(url);
        g_free(view->requested);
        view->requested = g_strdup(url);
        native_update_history(view, url, p->x, p->flags);
        struct browser_packet state = {.type = BROWSER_STATE, .flags = native_history_flags(view)};
        if (browser_control_send(broker->channel, state, NULL) < 0)
            broker->broken = 1;
    } else if (++view->hops <= 20) {
        g_free(view->pending);
        view->pending = g_strdup(text);
        view->pending_client_navigation = 0;
    } else
        native_queue_text(view, BROWSER_NOTICE, "Too many cross-origin redirects.");
    g_free(origin);
    return 1;
}

static void native_stop_renderer(NativeBrowser *view) {
    native_destroy_children(view);
    native_tree_clear(view);
    view->tree_revision = 0;
    view->tree_export_revision = 0;
    Broker *broker = &view->broker;
    if (broker->channel >= 0) {
        if (broker->source && !broker->broken)
            g_source_remove(broker->source);
        broker->source = 0;
        broker->broken = 1;
        close(broker->channel);
        broker->channel = -1;
    }

    if (view->pid) {
        /* Kernel authorization is restricted to this broker's own renderer. */
        if (kill(view->pid, 9) < 0 && errno != ESRCH)
            fprintf(stderr, "Renderer cleanup: %s\n", strerror(errno));
        int status;
        struct rusage usage = {0};
        int reaped;
        do {
            reaped = wait4(view->pid, &status, 0, &usage);
        } while (reaped < 0 && errno == EINTR);
        if (reaped == view->pid)
            fprintf(stderr, "Renderer closed: status=%d peak-memory=%ld KiB\n", status,
                    usage.ru_maxrss);
        view->pid = 0;
    }

    if (view->frames >= 0) {
        close(view->frames);
        view->frames = -1;
    }
    native_read_errors(view);
    if (view->errors >= 0) {
        close(view->errors);
        view->errors = -1;
    }
    g_clear_pointer(&view->incoming, g_byte_array_unref);
    g_clear_pointer(&view->surface, g_bytes_unref);
    view->awaiting_surface = 0;
    view->surface_dirty = 0;
    view->root_frame = 0;
    native_queue_text(view, BROWSER_CANCEL_PAINT, NULL);
    native_history_clear(&view->candidate);
    /* Cancellation callbacks release every request before reusing the broker. */
    if (broker->requests) {
        GList *values = g_hash_table_get_values(broker->requests);
        for (GList *item = values; item; item = item->next) {
            Request *request = item->data;
            request->cancelled = 1;
            if (request->started)
                g_cancellable_cancel(request->cancel);
        }

        g_list_free(values);
        while (!g_queue_is_empty(&broker->queued))
            release(g_queue_pop_head(&broker->queued));
        while (g_hash_table_size(broker->requests))
            g_main_context_iteration(NULL, TRUE);
        g_clear_pointer(&broker->requests, g_hash_table_unref);
    }

    g_clear_pointer(&broker->origin, g_free);
    broker->active = 0;
}

static int native_start_renderer(NativeBrowser *view, const char *url, int client_navigation) {
    g_free(view->requested);
    view->requested = g_strdup(url);
    char *origin = origin_for(!strcmp(url, "about:blank") ? "https://modernos.invalid/" : url);
    if (strcmp(url, "about:blank") && !view->restoring && origin && view->pid &&
        !view->broker.broken && view->broker.origin && !strcmp(origin, view->broker.origin)) {
        struct browser_packet packet = {.type = BROWSER_NAVIGATE,
                                        .flags =
                                            client_navigation ? BROWSER_NAVIGATION_FROM_CLIENT : 0};
        int result = browser_control_send(view->broker.channel, packet, url);
        g_free(origin);
        return result;
    }

    g_free(origin);
    native_stop_renderer(view);
    Broker *broker = &view->broker;
    broker->origin = origin_for(!strcmp(url, "about:blank") ? "https://modernos.invalid/" : url);
    if (!broker->origin) {
        native_queue_text(view, BROWSER_NOTICE, "Enter a valid HTTP or HTTPS address.");
        return 0;
    }
    broker->requests = g_hash_table_new(g_direct_hash, g_direct_equal);
    broker->broken = 0;
    int channel[2], frames[2], errors[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channel) < 0)
        return -1;
    if (pipe2(frames, O_CLOEXEC) < 0) {
        close(channel[0]);
        close(channel[1]);
        return -1;
    }
    if (pipe2(errors, O_CLOEXEC) < 0) {
        close(channel[0]);
        close(channel[1]);
        close(frames[0]);
        close(frames[1]);
        return -1;
    }

    if (fcntl(frames[0], F_SETPIPE_SZ, 256 * 1024) < 0) {
        close(channel[0]);
        close(channel[1]);
        close(frames[0]);
        close(frames[1]);
        close(errors[0]);
        close(errors[1]);
        return -1;
    }

    char width[24], height[24];
    snprintf(width, sizeof(width), "%d", view->width);
    snprintf(height, sizeof(height), "%d", view->height);
    char frame[32], parent[32], sandbox[24];
    snprintf(frame, sizeof(frame), "%llu", (unsigned long long)view->frame_id);
    snprintf(parent, sizeof(parent), "%llu",
             (unsigned long long)(view->parent ? view->parent->root_frame : 0));
    snprintf(sandbox, sizeof(sandbox), "%u", view->sandbox);
    char screen_width[24], screen_height[24], available_height[24];
    snprintf(screen_width, sizeof(screen_width), "%d", view->screen_width);
    snprintf(screen_height, sizeof(screen_height), "%d", view->screen_height);
    snprintf(available_height, sizeof(available_height), "%d", view->available_height);
    char window_x[24], window_y[24], window_width[24], window_height[24];
    snprintf(window_x, sizeof(window_x), "%d", view->window_x);
    snprintf(window_y, sizeof(window_y), "%d", view->window_y);
    snprintf(window_width, sizeof(window_width), "%d", view->window_width);
    snprintf(window_height, sizeof(window_height), "%d", view->window_height);
    char *top_args[] = {"web-renderer", "--view",     (char *)url,   width,
                        height,         screen_width, screen_height, available_height,
                        window_x,       window_y,     window_width,  window_height,
                        NULL,           NULL,         NULL};
    unsigned top_argc = 12;
    if (client_navigation && !view->restoring)
        top_args[top_argc++] = "--client-navigation";
    if (exception_trace)
        top_args[top_argc++] = "--exception-trace";
    char *child_args[] = {"web-renderer",
                          "--subframe",
                          (char *)url,
                          width,
                          height,
                          frame,
                          parent,
                          view->parent ? view->parent->broker.origin : NULL,
                          sandbox,
                          view->referrer ? view->referrer : "",
                          screen_width,
                          screen_height,
                          available_height,
                          window_x,
                          window_y,
                          window_width,
                          window_height,
                          (char *)native_first_party(broker),
                          exception_trace ? "--exception-trace" : NULL,
                          NULL};
    char **args = view->parent ? child_args : top_args;
    struct os_spawn_web spec = {.role = OS_WEB_RENDERER,
                                .ipc_fd = channel[1],
                                .stdout_fd = frames[1],
                                .stderr_fd = errors[1],
                                .origin = (uintptr_t)broker->origin};
    int pid = syscall(MODERNOS_SPAWN_WEB, "/bin/web-renderer", args, &spec);
    close(channel[1]);
    close(frames[1]);
    close(errors[1]);
    if (pid < 0) {
        close(channel[0]);
        close(frames[0]);
        close(errors[0]);
        return -1;
    }

    view->pid = pid;
    view->frames = frames[0];
    view->errors = errors[0];
    broker->channel = channel[0];
    fcntl(view->frames, F_SETFL, O_NONBLOCK);
    fcntl(view->errors, F_SETFL, O_NONBLOCK);
    struct os_web_identity identity, own;
    if (read(broker->channel, &identity, sizeof(identity)) != (ssize_t)sizeof(identity) ||
        syscall(MODERNOS_WEB_IDENTITY, &own) < 0 || identity.role != OS_WEB_RENDERER ||
        identity.process != (unsigned)pid || identity.domain != own.domain ||
        strcmp(identity.origin, broker->origin))
        return -1;
    broker->source = g_unix_fd_add(broker->channel, (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR),
                                   receive, broker);
    const int modifier_keys[] = {304, 306, 308};
    for (unsigned i = 0; i < 3; i++) {
        struct browser_packet key = {.type = BROWSER_KEY,
                                     .x = modifier_keys[i],
                                     .flags = !!(view->key_modifiers & (1u << i))};
        if (browser_control_send(broker->channel, key, NULL) < 0)
            return -1;
    }

    struct browser_packet state = {.type = BROWSER_STATE, .flags = native_history_flags(view)};
    if (browser_control_send(broker->channel, state, NULL) < 0)
        return -1;
    fprintf(stderr, "Native WebKit renderer pid=%d origin=%s\n", pid, broker->origin);
    return 0;
}

#include "native-frames.inc"

static int native_command(NativeBrowser *view, const struct browser_packet *p, const char *text) {
    if (p->magic != BROWSER_MAGIC || p->bytes > BROWSER_MAX_TEXT)
        return 0;
    if (p->type == BROWSER_SCREEN) {
        if (p->bytes || p->x < -32768 || p->x > 32768 || p->y < -32768 || p->y > 32768 ||
            p->width < 1 || p->height < 1 || p->width > 32768 || p->height > 32768 || !p->flags ||
            p->flags > (unsigned)p->height || !p->canvas_width || !p->canvas_height ||
            p->canvas_width > 32768 || p->canvas_height > 32768)
            return 0;
        view->screen_width = p->width;
        view->screen_height = p->height;
        view->available_height = p->flags;
        view->window_x = p->x;
        view->window_y = p->y;
        view->window_width = p->canvas_width;
        view->window_height = p->canvas_height;
        if (view->pid && browser_control_send(view->broker.channel, *p, NULL) < 0)
            view->broker.broken = 1;
        for (unsigned i = 0; i < WEB_FRAME_MAX_CHILDREN; i++)
            if (view->children[i])
                native_command(view->children[i], p, NULL);
        return 1;
    }

    if (p->type == BROWSER_POINTER) {
        if (p->canvas_width >= (unsigned)view->screen_width ||
            p->canvas_height >= (unsigned)view->screen_height)
            return 0;
    } else if (p->canvas_width || p->canvas_height)
        return 0;
    if (p->type == BROWSER_KEY) {
        if (p->bytes || (p->flags & ~1u))
            return 0;
        const int modifier_keys[] = {304, 306, 308};
        for (unsigned i = 0; i < 3; i++)
            if (p->x == modifier_keys[i]) {
                if (p->flags)
                    view->key_modifiers |= 1u << i;
                else
                    view->key_modifiers &= ~(1u << i);
            }
    }

    if (p->type == BROWSER_QUIT) {
        view->quit = 1;
        return !p->bytes;
    }
    if (p->type == BROWSER_NAVIGATE || p->type == BROWSER_BACK || p->type == BROWSER_FORWARD) {
        const char *url = text;
        if (p->type != BROWSER_NAVIGATE) {
            if (p->bytes || p->x < 0 || p->x > 128)
                return 0;
            unsigned steps = p->x ? p->x : 1;
            if (!view->count || (p->type == BROWSER_BACK ? steps > view->cursor
                                                         : steps >= view->count - view->cursor))
                return 1;
            if (p->type == BROWSER_BACK)
                view->cursor -= steps;
            else
                view->cursor += steps;
            view->traversing = 1;
            view->restoring = 1;
            url = view->history[view->cursor].url;
            if (view->history[view->cursor].pid == view->pid && view->history[view->cursor].token &&
                !view->broker.broken) {
                struct browser_packet back = *p;
                back.x = view->history[view->cursor].token;
                if (browser_control_send(view->broker.channel, back, NULL) < 0)
                    view->broker.broken = 1;
                view->restoring = 0;
                return 1;
            }
        } else {
            if (!p->bytes || memchr(text, 0, p->bytes))
                return 0;
            view->traversing = 0;
            view->restoring = 0;
        }

        g_free(view->pending);
        view->pending = g_strdup(url);
        view->hops = 0;
        view->pending_client_navigation = p->type == BROWSER_NAVIGATE;
        return 1;
    }

    if (p->bytes || p->type < BROWSER_KEY || p->type > BROWSER_STOP)
        return 0;
    if (p->type == BROWSER_RELOAD && view->current && !strcmp(view->current, "about:blank")) {
        g_free(view->pending);
        view->pending = g_strdup("about:blank");
        view->restoring = 0;
        view->traversing = 0;
        view->hops = 0;
        view->pending_client_navigation = 1;
        return 1;
    }

    if (p->type == BROWSER_RELOAD && (!view->pid || view->broker.broken)) {
        if (view->requested) {
            g_free(view->pending);
            view->pending = g_strdup(view->requested);
            view->restoring = 0;
            view->traversing = 0;
            view->hops = 0;
            view->pending_client_navigation = 1;
        }

        return 1;
    }

    if (p->type == BROWSER_RESIZE) {
        if (p->width < 300 || p->height < 200 || p->width > 4096 || p->height > 4096 ||
            (uint64_t)p->width * p->height > BROWSER_MAX_PIXELS)
            return 0;
        view->width = p->width;
        view->height = p->height;
    }

    if (view->pid && browser_control_send(view->broker.channel, *p, NULL) < 0)
        view->broker.broken = 1;
    return 1;
}

static void native_read_commands(NativeBrowser *view) {
    for (unsigned i = 0; i < 64; i++) {
        void *buffer;
        size_t size;
        if (view->input_used < sizeof(view->input)) {
            buffer = (char *)&view->input + view->input_used;
            size = sizeof(view->input) - view->input_used;
        } else {
            buffer = view->input_text + view->input_text_used;
            size = view->input.bytes - view->input_text_used;
        }
        if (size) {
            ssize_t n = read(0, buffer, size);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return;
            if (n <= 0) {
                view->quit = 1;
                return;
            }
            if (view->input_used < sizeof(view->input))
                view->input_used += n;
            else
                view->input_text_used += n;
        }

        if (view->input_used != sizeof(view->input))
            continue;
        if (view->input.magic != BROWSER_MAGIC || view->input.bytes > BROWSER_MAX_TEXT) {
            view->quit = 1;
            return;
        }
        if (view->input_text_used != view->input.bytes)
            continue;
        view->input_text[view->input.bytes] = 0;
        if (!native_command(view, &view->input, view->input_text)) {
            view->quit = 1;
            return;
        }
        view->input_used = view->input_text_used = 0;
    }
}

static int native_broker_main(int argc, char **argv) {
    while (argc > 1) {
        if (!strcmp(argv[argc - 1], "--network-trace"))
            network_trace = 1;
        else if (!strcmp(argv[argc - 1], "--exception-trace"))
            exception_trace = 1;
        else
            break;
        argc--;
    }

    CHECK(argc==5 || argc==8 || argc==12);
    struct os_web_identity identity;
    CHECK(syscall(MODERNOS_WEB_IDENTITY,&identity)==0 && identity.role==OS_WEB_BROKER);
    g_io_openssl_load(NULL);
    CHECK(g_tls_backend_supports_tls(g_tls_backend_get_default()));
    SoupSession *session = soup_session_new_with_options("timeout", 30, "max-conns", 8, NULL);
    GError *error = NULL;
    GTlsDatabase *database = g_tls_file_database_new("/etc/ssl/cert.pem", &error);
    CHECK(database && !error);
    g_object_set(session, "tls-database", database, NULL);
    g_object_unref(database);
    NativeBrowser view = {.broker = {.session = session, .channel = -1},
                          .frames = -1,
                          .errors = -1,
                          .width = atoi(argv[3]),
                          .height = atoi(argv[4])};
    view.screen_width = argc >= 8 ? atoi(argv[5]) : view.width;
    view.screen_height = argc >= 8 ? atoi(argv[6]) : view.height;
    view.available_height = argc >= 8 ? atoi(argv[7]) : view.height;
    view.window_x = argc == 12 ? atoi(argv[8]) : 0;
    view.window_y = argc == 12 ? atoi(argv[9]) : 0;
    view.window_width = argc == 12 ? atoi(argv[10]) : view.width;
    view.window_height = argc == 12 ? atoi(argv[11]) : view.height;
    CHECK(view.screen_width>0 && view.screen_height>0 && view.screen_width<=32768 && view.screen_height<=32768
        && view.available_height>0 && view.available_height<=view.screen_height);
    CHECK(view.window_x>=-32768 && view.window_x<=32768 && view.window_y>=-32768 && view.window_y<=32768
        && view.window_width>0 && view.window_width<=32768 && view.window_height>0 && view.window_height<=32768);
    const char *home = getenv("HOME");
    if (!home || *home != '/')
        home = "/home/user";
    char *config = g_strdup_printf("%s/.config/webkit", home);
    mkdir(config, 0700);
    char *cookie_file = g_strdup_printf("%s/cookies.txt", config),
         *storage_file = g_strdup_printf("%s/storage.sqlite", config);
    view.cookies = soup_cookie_jar_text_new(cookie_file, FALSE);
    soup_cookie_jar_set_accept_policy(view.cookies, SOUP_COOKIE_JAR_ACCEPT_NO_THIRD_PARTY);
    if (network_trace)
        g_signal_connect(view.cookies, "changed", G_CALLBACK(trace_cookie_change), NULL);
    soup_session_add_feature(session, SOUP_SESSION_FEATURE(view.cookies));
    view.session_storage =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    /* SQLite's actual directory-lock VFS works on RAMFS and FAT without
     * pretending the kernel implements POSIX advisory record locks. */
    int storage_error = sqlite3_open_v2(storage_file, &view.storage,
                                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "unix-dotfile");
    if (!storage_error)
        sqlite3_busy_timeout(view.storage, 1000);
    if (storage_error ||
        sqlite3_exec(
            view.storage,
            "PRAGMA temp_store=MEMORY;PRAGMA max_page_count=16384;CREATE TABLE IF NOT EXISTS storage(origin TEXT NOT NULL,k BLOB NOT NULL,v BLOB NOT NULL,PRIMARY KEY(origin,k))",
            NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "Local storage could not open: %s\n",
                view.storage ? sqlite3_errmsg(view.storage) : "allocation failure");
        if (view.storage)
            sqlite3_close(view.storage);
        view.storage = NULL;
    }

    g_free(config);
    g_free(cookie_file);
    g_free(storage_file);
    view.broker.native = &view;
    view.pending = g_strdup(argv[2]);
    view.pending_client_navigation = 1;
    CHECK(view.width>=300 && view.height>=200 && view.width<=4096 && view.height<=4096);
    CHECK(fcntl(0,F_SETFL,fcntl(0,F_GETFL)|O_NONBLOCK)==0 && fcntl(1,F_SETFL,fcntl(1,F_GETFL)|O_NONBLOCK)==0);
    while (!view.quit) {
        native_read_commands(&view);
        for (unsigned i = 0; i < 32 && g_main_context_iteration(NULL, FALSE); i++) {
        }
        native_read_frames(&view);
        native_read_errors(&view);
        native_tick_children(&view);
        native_flush(&view);
        if (view.pending && !view.quit) {
            char *url = view.pending;
            view.pending = NULL;
            int client_navigation = view.pending_client_navigation;
            view.pending_client_navigation = 0;
            if (native_start_renderer(&view, url, client_navigation) < 0) {
                fprintf(stderr, "Renderer start: %s\n", strerror(errno));
                native_queue_text(&view, BROWSER_NOTICE,
                                  "The WebKit page process could not start.");
                native_stop_renderer(&view);
            }

            g_free(url);
        } else if (view.pid) {
            int status = 0;
            struct rusage usage = {0};
            int exited = wait4(view.pid, &status, WNOHANG, &usage);
            if (exited == view.pid) {
                fprintf(stderr, "Renderer exit: status=%d peak-memory=%ld KiB\n", status,
                        usage.ru_maxrss);
                view.pid = 0;
                native_read_frames(&view);
                native_stop_renderer(&view);
                if (status)
                    native_queue_text(&view, BROWSER_NOTICE,
                                      "The WebKit page process stopped. Reload the page to retry.");
            }
        }

        usleep(1000);
    }

    native_stop_renderer(&view);
    g_free(view.pending);
    g_free(view.current);
    g_free(view.requested);
    for (unsigned i = 0; i < view.count; i++)
        native_history_clear(&view.history[i]);
    native_history_clear(&view.candidate);
    g_clear_pointer(&view.writing, g_bytes_unref);
    while (!g_queue_is_empty(&view.output))
        g_bytes_unref(g_queue_pop_head(&view.output));
    g_hash_table_unref(view.session_storage);
    if (view.storage)
        sqlite3_close(view.storage);
    g_object_unref(view.cookies);
    g_object_unref(session);
    return 0;
}
