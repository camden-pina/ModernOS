#define _GNU_SOURCE
#include "../include/web_process.h"
#include "../include/browser_protocol.h"
#include "../include/audio_protocol.h"
#include "web-resource-io.h"
#include "web-frame-io.h"
#include "browser-control.h"
#include <libsoup/soup.h>
#include <glib-unix.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sqlite3.h>

/* Development broker with genuine async HTTP(S) resource delivery. The
 * --check entry point verifies pages; native navigation/frontend work remains. */
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL web-broker:%d: %s (errno=%d)\n",__LINE__,#x,errno); exit(1); } } while (0)

void g_io_openssl_load(GIOModule *module);
typedef struct Broker Broker;
typedef struct Request Request;
typedef struct NativeBrowser NativeBrowser;
static int native_renderer_control(Broker *, const struct browser_packet *, const char *);
static int native_frame_control(Broker *, const struct web_frame_packet *, const char *, int);
static const char *native_first_party(Broker *);
static GUri *native_cookie_site(Broker *);
static void native_queue_text(NativeBrowser *, unsigned, const char *);
static void native_remember_response(Request *, const struct web_resource_packet *, const char *,
                                     const void *);
static int native_restore_response(Broker *, const struct web_resource_packet *, const char *,
                                   const char *);
static int native_service(Broker *, const struct web_resource_packet *, const char *, int);
static int native_begin_download(Request *);
static int native_write_download(Request *, const void *, size_t);
static int native_finish_download(Request *);
struct Request {
    Broker *broker;
    struct web_resource_packet packet;
    char *url;
    SoupMessage *message;
    GInputStream *stream;
    GCancellable *cancel;
    GByteArray *body;
    int started, cancelled;
    int download_fd, download_saved;
    size_t download_bytes;
    char *download_temporary, *download_destination;
    GBytes *request_body;
};

struct Broker {
    SoupSession *session;
    char *origin;
    int channel, broken;
    guint source;
    GHashTable *requests;
    GQueue queued;
    unsigned active, received;
    NativeBrowser *native;
};

struct Reader {
    int frames, errors, pid;
    atomic_int done;
};

static int network_trace;
static int exception_trace;

/* Opt-in transport diagnostics contain cookie names and attributes only.
 * Never emit credentials, cookie values or challenge request/response bodies. */
static void trace_request_headers(SoupMessage *message, gpointer data) {
    Request *request = data;
    const char *header =
        soup_message_headers_get_one(soup_message_get_request_headers(message), "Cookie");
    GString *names = g_string_new(NULL);
    if (header) {
        char **parts = g_strsplit(header, ";", -1);
        for (unsigned i = 0; parts[i]; i++) {
            char *part = g_strstrip(parts[i]), *end = strchr(part, '=');
            if (!end || end - part > 256)
                continue;
            if (names->len)
                g_string_append_c(names, ',');
            g_string_append_len(names, part, end - part);
        }

        g_strfreev(parts);
    }

    fprintf(
        stderr,
        "HTTP request headers: id=%u method=%s credentials=%u top_navigation=%u cookie_names=[%s] %s\n",
        request->packet.id, soup_message_get_method(message),
        !!(request->packet.flags & WEB_RESOURCE_CREDENTIALS),
        soup_message_get_is_top_level_navigation(message), names->str, request->url);
    SoupMessageHeaders *headers = soup_message_get_request_headers(message);
    const char *agent = soup_message_headers_get_one(headers, "User-Agent"),
               *mode = soup_message_headers_get_one(headers, "Sec-Fetch-Mode"),
               *site = soup_message_headers_get_one(headers, "Sec-Fetch-Site"),
               *dest = soup_message_headers_get_one(headers, "Sec-Fetch-Dest");
    fprintf(
        stderr,
        "HTTP identity: id=%u user_agent=[%s] fetch_mode=[%s] fetch_site=[%s] fetch_dest=[%s]\n",
        request->packet.id, agent ? agent : "", mode ? mode : "", site ? site : "",
        dest ? dest : "");
    /* Compare complete headers privately, not just cookie names. The
     * message-specific feature lookup also respects omitted credentials. */
    SoupCookieJar *jar = SOUP_COOKIE_JAR(soup_session_get_feature_for_message(
        request->broker->session, SOUP_TYPE_COOKIE_JAR, message));
    const char *method = soup_message_get_method(message);
    int safe = !strcmp(method, "GET") || !strcmp(method, "HEAD") || !strcmp(method, "OPTIONS");
    GSList *selected =
        jar ? soup_cookie_jar_get_cookie_list_with_same_site_info(
                  jar, soup_message_get_uri(message), soup_message_get_first_party(message),
                  soup_message_get_site_for_cookies(message), TRUE, safe,
                  soup_message_get_is_top_level_navigation(message))
            : NULL;
    char *expected = selected ? soup_cookies_to_cookie_header(selected) : NULL;
    fprintf(stderr, "HTTP cookie integrity: id=%u matches_jar=%u selected_count=%u %s\n",
            request->packet.id, !strcmp(header ? header : "", expected ? expected : ""),
            g_slist_length(selected), request->url);
    g_free(expected);
    soup_cookies_free(selected);
    g_string_free(names, TRUE);
}

static void trace_response_headers(Request *request) {
    SoupMessage *message = request->message;
    SoupMessageHeaders *headers = soup_message_get_response_headers(message);
    const char *mitigated = soup_message_headers_get_one(headers, "cf-mitigated"),
               *ray = soup_message_headers_get_one(headers, "cf-ray"),
               *date = soup_message_headers_get_one(headers, "Date");
    GDateTime *server_time = date ? soup_date_time_new_from_http_string(date) : NULL;
    char *skew = server_time
                     ? g_strdup_printf("%" G_GINT64_FORMAT, g_get_real_time() / G_USEC_PER_SEC -
                                                                g_date_time_to_unix(server_time))
                     : g_strdup("unknown");
    GEnumClass *tls = g_type_class_ref(G_TYPE_TLS_PROTOCOL_VERSION);
    GEnumValue *version = g_enum_get_value(tls, soup_message_get_tls_protocol_version(message));
    SoupHTTPVersion http = soup_message_get_http_version(message);
    fprintf(
        stderr,
        "HTTP response diagnostics: id=%u status=%u http=[%s] tls=[%s] certificate_errors=%u cf_mitigated=[%.256s] cf_ray=[%.128s] clock_skew_seconds=%s %s\n",
        request->packet.id, soup_message_get_status(message),
        http == SOUP_HTTP_2_0   ? "h2"
        : http == SOUP_HTTP_1_1 ? "http/1.1"
                                : "http/1.0",
        version ? version->value_nick : "unknown",
        (unsigned)soup_message_get_tls_peer_certificate_errors(message), mitigated ? mitigated : "",
        ray ? ray : "", skew, request->url);
    g_type_class_unref(tls);
    g_free(skew);
    if (server_time)
        g_date_time_unref(server_time);
}

static void trace_cookie_change(SoupCookieJar *jar, SoupCookie *old, SoupCookie *cookie,
                                gpointer data) {
    (void)jar;
    (void)data;
    SoupCookie *entry = cookie ? cookie : old;
    fprintf(
        stderr, "HTTP cookie %s: name=%s domain=%s path=%s secure=%u http_only=%u same_site=%u\n",
        cookie ? "stored" : "removed", soup_cookie_get_name(entry), soup_cookie_get_domain(entry),
        soup_cookie_get_path(entry), soup_cookie_get_secure(entry),
        soup_cookie_get_http_only(entry), soup_cookie_get_same_site_policy(entry));
}

static char *origin_for(const char *url) {
    GError *error = NULL;
    GUri *uri = g_uri_parse(url, G_URI_FLAGS_ENCODED, &error);
    if (!uri) {
        g_clear_error(&error);
        return NULL;
    }

    const char *scheme = g_uri_get_scheme(uri), *host = g_uri_get_host(uri);
    char *result = NULL, *ascii = NULL, *lower = NULL;
    if (!scheme || !host || g_uri_get_userinfo(uri) ||
        (g_ascii_strcasecmp(scheme, "http") && g_ascii_strcasecmp(scheme, "https")))
        goto done;

    ascii = g_hostname_to_ascii(host);
    if (!ascii)
        goto done;
    lower = g_ascii_strdown(ascii, -1);
    size_t length = strlen(lower);
    if (length && lower[length - 1] == '.')
        lower[--length] = 0;

    unsigned label = 0;
    for (size_t i = 0; i < length; i++) {
        char c = lower[i];
        if (c == '.') {
            if (!label || lower[i - 1] == '-')
                goto done;
            label = 0;
        } else if (g_ascii_islower(c) || g_ascii_isdigit(c) || c == '-') {
            if ((!label && c == '-') || ++label > 63)
                goto done;
        } else
            goto done;
    }

    if (!label || lower[length - 1] == '-')
        goto done;

    int port = g_uri_get_port(uri);
    if (port < 0)
        port = !g_ascii_strcasecmp(scheme, "https") ? 443 : 80;
    if (port < 1 || port > 65535)
        goto done;

    result = g_strdup_printf("%s://%s:%d", !g_ascii_strcasecmp(scheme, "https") ? "https" : "http",
                             lower, port);
    if (strlen(result) >= 256)
        g_clear_pointer(&result, g_free);

done:
    g_free(ascii);
    g_free(lower);
    g_uri_unref(uri);
    return result;
}

static void init(struct web_resource_packet *p, unsigned type, uint32_t id) {
    memset(p, 0, sizeof(*p));
    p->magic = WEB_RESOURCE_MAGIC;
    p->version = WEB_RESOURCE_VERSION;
    p->type = type;
    p->id = id;
}

static void send_error(Broker *broker, uint32_t id, const char *url, const char *reason) {
    fprintf(stderr, "Resource error: %s: %s\n", reason, url);
    struct web_resource_packet packet;
    init(&packet, WEB_RESOURCE_ERROR, id);
    packet.url_bytes = strlen(url);
    packet.error_bytes = strlen(reason);
    if (packet.error_bytes > 4096)
        packet.error_bytes = 4096;
    packet.metadata_bytes = packet.url_bytes + packet.error_bytes;
    char *metadata = g_malloc(packet.metadata_bytes);
    memcpy(metadata, url, packet.url_bytes);
    memcpy(metadata + packet.url_bytes, reason, packet.error_bytes);
    if (web_resource_send(broker->channel, &packet, metadata, -1) < 0)
        broker->broken = 1;
    g_free(metadata);
}

static void start_queued(Broker *broker);

static void release(Request *request) {
    Broker *broker = request->broker;
    g_hash_table_remove(broker->requests, GUINT_TO_POINTER(request->packet.id));
    if (request->started)
        --broker->active;
    g_clear_object(&request->message);
    g_clear_object(&request->stream);
    g_clear_object(&request->cancel);
    if (request->download_fd >= 0)
        close(request->download_fd);
    if (request->download_temporary && !request->download_saved)
        unlink(request->download_temporary);
    g_free(request->download_temporary);
    g_free(request->download_destination);
    g_clear_pointer(&request->request_body, g_bytes_unref);
    g_byte_array_unref(request->body);
    g_free(request->url);
    g_free(request);
    start_queued(broker);
}

static void failed(Request *request, const char *message) {
    if (!request->cancelled && !request->broker->broken)
        send_error(request->broker, request->packet.id, request->url, message);
    release(request);
}

static void completed(Request *request) {
    Broker *broker = request->broker;
    struct web_resource_packet packet;
    init(&packet, WEB_RESOURCE_RESPONSE, request->packet.id);
    packet.url_bytes = strlen(request->url);
    packet.body_bytes = request->body->len;
    packet.status = soup_message_get_status(request->message);

    GString *metadata = g_string_new_len(request->url, packet.url_bytes);
    SoupMessageHeadersIter iterator;
    const char *name, *value;
    soup_message_headers_iter_init(&iterator, soup_message_get_response_headers(request->message));
    while (soup_message_headers_iter_next(&iterator, &name, &value)) {
        /* Soup consumed these into the trusted jar; response cookies never
         * enter a renderer's transport or caches, including HttpOnly values. */
        if (!g_ascii_strcasecmp(name, "Set-Cookie") || !g_ascii_strcasecmp(name, "Set-Cookie2"))
            continue;
        g_string_append_len(metadata, name, strlen(name) + 1);
        g_string_append_len(metadata, value, strlen(value) + 1);
        if (metadata->len > WEB_RESOURCE_MAX_METADATA) {
            g_string_free(metadata, TRUE);
            failed(request, "Response headers exceed broker quota");
            return;
        }
    }

    packet.headers_bytes = metadata->len - packet.url_bytes;
    SoupMessageMetrics *observed = soup_message_get_metrics(request->message);
    if (observed) {
        struct web_network_metrics metrics = {
            .fetch_start = soup_message_metrics_get_fetch_start(observed),
            .dns_start = soup_message_metrics_get_dns_start(observed),
            .dns_end = soup_message_metrics_get_dns_end(observed),
            .connect_start = soup_message_metrics_get_connect_start(observed),
            .tls_start = soup_message_metrics_get_tls_start(observed),
            .connect_end = soup_message_metrics_get_connect_end(observed),
            .request_start = soup_message_metrics_get_request_start(observed),
            .response_start = soup_message_metrics_get_response_start(observed),
            .response_end = soup_message_metrics_get_response_end(observed),
            .encoded_body_bytes = soup_message_metrics_get_response_body_bytes_received(observed)};
        switch (soup_message_get_http_version(request->message)) {
        case SOUP_HTTP_1_0:
            metrics.protocol = WEB_NETWORK_HTTP10;
            break;
        case SOUP_HTTP_1_1:
            metrics.protocol = WEB_NETWORK_HTTP11;
            break;
        case SOUP_HTTP_2_0:
            metrics.protocol = WEB_NETWORK_HTTP2;
            break;
        }
        if (metadata->len + sizeof(metrics) > WEB_RESOURCE_MAX_METADATA) {
            g_string_free(metadata, TRUE);
            failed(request, "Response metrics exceed broker quota");
            return;
        }
        packet.metrics_bytes = sizeof(metrics);
        g_string_append_len(metadata, (const char *)&metrics, sizeof(metrics));
    }

    packet.metadata_bytes = metadata->len;
    if (request->download_destination) {
        if (!native_finish_download(request)) {
            g_string_free(metadata, TRUE);
            failed(
                request,
                "Download could not be saved; the destination already exists or is not writable.");
            return;
        }

        packet.flags = WEB_RESOURCE_DOWNLOAD;
    }

    int body = packet.body_bytes ? web_resource_body(request->body->data, packet.body_bytes) : -1;
    if (packet.body_bytes && body < 0) {
        g_string_free(metadata, TRUE);
        failed(request, "Response body allocation failed");
        return;
    }

    if (web_resource_send(broker->channel, &packet, metadata->str, body) < 0)
        broker->broken = 1;
    if (broker->native && !(packet.flags & WEB_RESOURCE_DOWNLOAD) &&
        (request->packet.flags & WEB_RESOURCE_MAIN) && packet.status >= 200 && packet.status < 300)
        native_remember_response(request, &packet, metadata->str, request->body->data);

    if (body >= 0)
        close(body);
    g_string_free(metadata, TRUE);

    ++broker->received;
    fprintf(broker->native ? stderr : stdout, "HTTP resource: status=%u bytes=%u %s\n",
            packet.status, packet.body_bytes, request->url);
    release(request);
}

static void read_chunk(GObject *source, GAsyncResult *result, gpointer data) {
    Request *request = data;
    GError *error = NULL;
    GBytes *bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, &error);
    if (!bytes) {
        failed(request, error->message);
        g_error_free(error);
        return;
    }

    gsize size;
    const void *body = g_bytes_get_data(bytes, &size);
    if (request->download_destination) {
        if (!size) {
            g_bytes_unref(bytes);
            completed(request);
            return;
        }
        if (!native_write_download(request, body, size)) {
            g_bytes_unref(bytes);
            failed(request, "Download could not be saved or exceeds the 256 MiB file limit.");
            return;
        }
        g_bytes_unref(bytes);
        g_input_stream_read_bytes_async(request->stream, 65536, G_PRIORITY_DEFAULT, request->cancel,
                                        read_chunk, request);
        return;
    }

    if (size > WEB_RESOURCE_MAX_BODY - request->body->len) {
        g_bytes_unref(bytes);
        failed(request, "Response body exceeds broker quota");
        return;
    }
    if (!size) {
        g_bytes_unref(bytes);
        completed(request);
        return;
    }

    g_byte_array_append(request->body, body, size);
    g_bytes_unref(bytes);
    g_input_stream_read_bytes_async(request->stream, 65536, G_PRIORITY_DEFAULT, request->cancel,
                                    read_chunk, request);
}

static void got_headers(GObject *source, GAsyncResult *result, gpointer data) {
    Request *request = data;
    GError *error = NULL;
    request->stream = soup_session_send_finish(SOUP_SESSION(source), result, &error);
    if (!request->stream) {
        failed(request, error->message);
        g_error_free(error);
        return;
    }

    if (network_trace) {
        trace_response_headers(request);
        GSList *cookies = soup_cookies_from_response(request->message);
        for (GSList *item = cookies; item; item = item->next) {
            SoupCookie *cookie = item->data;
            fprintf(
                stderr,
                "HTTP response cookie: id=%u status=%u name=%s domain=%s secure=%u http_only=%u\n",
                request->packet.id, soup_message_get_status(request->message),
                soup_cookie_get_name(cookie), soup_cookie_get_domain(cookie),
                soup_cookie_get_secure(cookie), soup_cookie_get_http_only(cookie));
        }

        soup_cookies_free(cookies);
    }

    if (request->broker->native && native_begin_download(request) < 0) {
        failed(request, "Download could not be saved in Downloads.");
        return;
    }

    g_input_stream_read_bytes_async(request->stream, 65536, G_PRIORITY_DEFAULT, request->cancel,
                                    read_chunk, request);
}

static void start_queued(Broker *broker) {
    while (!broker->broken && broker->active < WEB_RESOURCE_MAX_ACTIVE &&
           !g_queue_is_empty(&broker->queued)) {
        Request *request = g_queue_pop_head(&broker->queued);
        request->started = 1;
        ++broker->active;
        soup_session_send_async(broker->session, request->message, G_PRIORITY_DEFAULT,
                                request->cancel, got_headers, request);
    }
}

static int token(const char *name) {
    if (!*name)
        return 0;
    for (; *name; name++)
        if (!g_ascii_isalnum(*name) && !strchr("!#$%&'*+-.^_`|~", *name))
            return 0;
    return 1;
}

static int headers(Request *request, const char *bytes, size_t count, const char *request_origin) {
    const char *end = bytes + count;
    while (bytes < end) {
        const char *value = memchr(bytes, 0, end - bytes);
        const char *next = value ? memchr(value + 1, 0, end - value - 1) : NULL;
        if (!next || !token(bytes) || value - bytes > 256)
            return 0;
        for (const char *p = value + 1; p < next; p++)
            if (((unsigned char)*p < 32 && *p != '\t') || (unsigned char)*p == 127)
                return 0;
        if (!g_ascii_strcasecmp(bytes, "Origin") && strcmp(value + 1, request->broker->origin) &&
            strcmp(value + 1, "null")) {
            char *claimed = origin_for(value + 1);
            int matches = claimed && !strcmp(claimed, request->broker->origin);
            g_free(claimed);
            if (!matches)
                return 0;
        }

        /* Network framing and HTTP cookies belong to Soup. Renderer-supplied
         * credentials cannot choose another origin's private cookie state. */
        if (g_ascii_strcasecmp(bytes, "Host") && g_ascii_strcasecmp(bytes, "Connection") &&
            g_ascii_strcasecmp(bytes, "Content-Length") &&
            g_ascii_strcasecmp(bytes, "Transfer-Encoding") && g_ascii_strcasecmp(bytes, "Cookie") &&
            g_ascii_strncasecmp(bytes, "Proxy-", 6) &&
            (g_ascii_strcasecmp(bytes, "Authorization") ||
             !strcmp(request_origin, request->broker->origin)))
            soup_message_headers_append(soup_message_get_request_headers(request->message), bytes,
                                        value + 1);
        bytes = next + 1;
    }

    return 1;
}

static gboolean receive(gint fd, GIOCondition condition, gpointer data) {
    (void)condition;
    Broker *broker = data;
    for (unsigned n = 0; n < 32; n++) {
        uint32_t magic = 0;
        ssize_t peek = recv(fd, &magic, sizeof(magic), MSG_PEEK | MSG_DONTWAIT);
        if (peek < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (peek != (ssize_t)sizeof(magic)) {
            broker->broken = 1;
            break;
        }
        if (magic == WEB_FRAME_MAGIC && broker->native) {
            struct web_frame_packet input = {0};
            char text[WEB_FRAME_MAX_METADATA];
            int body = -1;
            int received = web_frame_receive(fd, &input, text, &body, MSG_DONTWAIT);
            int valid = received == 1 && native_frame_control(broker, &input, text, body);
            if (body >= 0)
                close(body);
            if (!valid) {
                broker->broken = 1;
                break;
            }
            continue;
        }

        if (magic == BROWSER_MAGIC && broker->native) {
            struct browser_packet input = {0};
            char text[BROWSER_MAX_TEXT + 1];
            if (browser_control_receive(fd, &input, text) != 1 ||
                !native_renderer_control(broker, &input, text)) {
                broker->broken = 1;
                break;
            }
            continue;
        }

        struct web_resource_packet packet;
        char metadata[WEB_RESOURCE_MAX_METADATA];
        int body = -1;
        int result = web_resource_receive(fd, &packet, metadata, &body, MSG_DONTWAIT);
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (result <= 0) {
            broker->broken = 1;
            break;
        }
        if (packet.type == WEB_SERVICE_REQUEST && broker->native) {
            int valid = native_service(broker, &packet, metadata, body);
            if (body >= 0)
                close(body);
            if (!valid) {
                broker->broken = 1;
                break;
            }
            continue;
        }

        if (packet.type == WEB_RESOURCE_CANCEL && !packet.body_bytes && !packet.method_bytes &&
            !packet.headers_bytes && !packet.error_bytes && !packet.status) {
            Request *request = g_hash_table_lookup(broker->requests, GUINT_TO_POINTER(packet.id));
            if (request) {
                request->cancelled = 1;
                if (request->started)
                    g_cancellable_cancel(request->cancel);
                else {
                    g_queue_remove(&broker->queued, request);
                    release(request);
                }
            }

            continue;
        }

        if (packet.type != WEB_RESOURCE_REQUEST || !packet.method_bytes || packet.error_bytes ||
            packet.status || packet.flags & WEB_RESOURCE_DOWNLOAD ||
            g_hash_table_contains(broker->requests, GUINT_TO_POINTER(packet.id)) ||
            memchr(metadata, 0, packet.url_bytes + packet.method_bytes)) {
            if (body >= 0)
                close(body);
            broker->broken = 1;
            break;
        }

        char *url = g_strndup(metadata, packet.url_bytes),
             *method = g_strndup(metadata + packet.url_bytes, packet.method_bytes);
        char *origin = origin_for(url);
        int valid = origin && token(method) && strcmp(method, "CONNECT") &&
                    strcmp(method, "TRACE") &&
                    (!(packet.flags & WEB_RESOURCE_MAIN) || !strcmp(origin, broker->origin));
        if (!valid || g_hash_table_size(broker->requests) >= 128) {
            send_error(broker, packet.id, url,
                       valid ? "Broker request quota exceeded"
                             : "URL/method/origin denied by broker");
            g_free(origin);
            g_free(url);
            g_free(method);
            if (body >= 0)
                close(body);
            continue;
        }

        if (broker->native && native_restore_response(broker, &packet, url, method)) {
            g_free(origin);
            g_free(url);
            g_free(method);
            if (body >= 0)
                close(body);
            continue;
        }

        Request *request = g_new0(Request, 1);
        request->broker = broker;
        request->packet = packet;
        request->url = url;
        request->download_fd = -1;
        request->message = soup_message_new(method, url);
        request->body = g_byte_array_new();
        request->cancel = g_cancellable_new();
        g_free(method);
        g_hash_table_insert(broker->requests, GUINT_TO_POINTER(packet.id), request);
        if (!request->message ||
            !headers(request, metadata + packet.url_bytes + packet.method_bytes,
                     packet.headers_bytes, origin)) {
            if (body >= 0)
                close(body);
            g_free(origin);
            failed(request, "Invalid resource request headers");
            continue;
        }

        if (!(packet.flags & WEB_RESOURCE_CREDENTIALS))
            soup_message_disable_feature(request->message, SOUP_TYPE_COOKIE_JAR);
        GUri *first_party = g_uri_parse(native_first_party(broker), G_URI_FLAGS_ENCODED, NULL);
        if (first_party) {
            soup_message_set_first_party(request->message, first_party);
            g_uri_unref(first_party);
        }
        GUri *cookie_site = native_cookie_site(broker);
        soup_message_set_site_for_cookies(request->message, cookie_site);
        if (cookie_site)
            g_uri_unref(cookie_site);
        soup_message_set_is_top_level_navigation(
            request->message, !!(packet.flags & WEB_RESOURCE_MAIN) &&
                                  !strcmp(native_first_party(broker), broker->origin));
        if (network_trace)
            g_signal_connect(request->message, "wrote-headers", G_CALLBACK(trace_request_headers),
                             request);
        g_free(origin);
        soup_message_add_flags(request->message,
                               SOUP_MESSAGE_NO_REDIRECT | SOUP_MESSAGE_COLLECT_METRICS);
        if (body >= 0) {
            void *mapped = mmap(NULL, packet.body_bytes, PROT_READ, MAP_SHARED, body, 0);
            if (mapped == MAP_FAILED) {
                close(body);
                failed(request, "Request body mapping failed");
                continue;
            }
            GBytes *bytes = g_bytes_new(mapped, packet.body_bytes);
            if ((packet.flags & WEB_RESOURCE_MAIN) && packet.body_bytes <= 4 * 1024 * 1024)
                request->request_body = g_bytes_ref(bytes);
            munmap(mapped, packet.body_bytes);
            close(body);
            const char *content_type = soup_message_headers_get_one(
                soup_message_get_request_headers(request->message), "Content-Type");
            soup_message_set_request_body_from_bytes(request->message, content_type, bytes);
            g_bytes_unref(bytes);
        }

        g_queue_push_tail(&broker->queued, request);
        start_queued(broker);
    }

    return broker->broken ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

static void copy(int fd) {
    char buffer[2048];
    ssize_t n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0)
        CHECK(write(1,buffer,n)==n);
    CHECK(n==0);
    close(fd);
}

static void exact(struct Reader *reader, void *buffer, size_t size) {
    char *cursor = buffer;
    while (size) {
        ssize_t n = read(reader->frames, cursor, size);
        if (n <= 0) {
            copy(reader->errors);
            reader->errors = -1;
        }
        CHECK(n>0);
        cursor += n;
        size -= n;
    }
}

static void color(const uint32_t *pixels, unsigned x, unsigned y, uint32_t expected) {
    uint32_t actual = pixels[y * 320 + x] & 0xffffff;
    if (actual != expected)
        fprintf(stderr, "network pixel (%u,%u): actual=%06x expected=%06x\n", x, y, actual,
                expected);
    CHECK(actual==expected);
}

static void *read_frames(void *data) {
    struct Reader *reader = data;
    for (unsigned index = 0; index < 5; index++) {
        struct browser_packet p;
        exact(reader, &p, sizeof(p));
        CHECK(p.magic==BROWSER_MAGIC && p.type==BROWSER_FRAME && !p.x && !p.y &&
            p.width==320 && p.height==240 && p.canvas_width==320 && p.canvas_height==240 && p.bytes==320*240*4);
        uint32_t *pixels = malloc(p.bytes);
        CHECK(pixels);
        exact(reader, pixels, p.bytes);
        if (index == 3) {
            unsigned green = 0;
            for (unsigned i = 0; i < 320 * 240; i++)
                if ((pixels[i] & 0xffffff) == 0x11aa33)
                    ++green;
            CHECK(green>=1000);
            free(pixels);
            continue;
        }

        if (index == 4) {
            color(pixels, 5, 5, 0x1188bb);
            free(pixels);
            continue;
        }
        color(pixels, 5, 5, 0xfafafa);
        color(pixels, 25, 25, 0xe02030);
        color(pixels, 75, 25, 0x2040e0);
        color(pixels, 225, 25, 0xd030c0);
        color(pixels, 225, 85, 0x00ccee);
        color(pixels, 145, 105, index == 2 ? 0x1122ee : index ? 0x00cc44 : 0xffcc00);
        unsigned ink = 0;
        for (unsigned y = 170; y < 230; y++)
            for (unsigned x = 20; x < 310; x++) {
                uint32_t p = pixels[y * 320 + x];
                if (((p >> 16) & 255) < 100 && ((p >> 8) & 255) < 100 && (p & 255) < 100)
                    ++ink;
            }
        CHECK(ink>100);
        free(pixels);
    }

    copy(reader->errors);
    close(reader->frames);
    int status;
    CHECK(waitpid(reader->pid,&status,0)==reader->pid && WIFEXITED(status) && !WEXITSTATUS(status));
    atomic_store(&reader->done, 1);
    return NULL;
}

static void trust_fixture(SoupSession *session, const char *url) {
    CHECK(!strncmp(url,"http://10.0.2.2:",16));
    SoupMessage *message = soup_message_new("GET", url);
    CHECK(message);
    GError *error = NULL;
    GBytes *bytes = soup_session_send_and_read(session, message, NULL, &error);
    if (error)
        fprintf(stderr, "Fixture CA fetch: %s\n", error->message);
    CHECK(bytes && !error && soup_message_get_status(message)==200);
    gsize size;
    const char *data = g_bytes_get_data(bytes, &size);
    CHECK(size>0 && size<1024*1024);
    CHECK(mkdir("/home/user/.config/webkit",0700)==0 || errno==EEXIST);
    const char *path = "/home/user/.config/webkit/web-resource-fixture-ca.pem";
    CHECK(g_file_set_contents(path,data,size,&error));
    GTlsDatabase *database = g_tls_file_database_new(path, &error);
    CHECK(database && !error);
    g_object_set(session, "tls-database", database, NULL);
    g_object_unref(database);
    CHECK(unlink(path)==0);
    g_bytes_unref(bytes);
    g_object_unref(message);
}

static void run_page(SoupSession *session, const char *url) {
    Broker broker = {.session = session, .origin = origin_for(url)};
    CHECK(broker.origin);
    broker.requests = g_hash_table_new(g_direct_hash, g_direct_equal);
    int channel[2], frames[2], errors[2];
    CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,channel)==0 && pipe(frames)==0 && pipe(errors)==0);
    char *args[] = {"web-renderer", "--network-check", (char *)url, NULL};
    struct os_spawn_web spec = {.role = OS_WEB_RENDERER,
                                .ipc_fd = channel[1],
                                .stdout_fd = frames[1],
                                .stderr_fd = errors[1],
                                .origin = (uintptr_t)broker.origin};
    int pid = syscall(MODERNOS_SPAWN_WEB, "/bin/web-renderer", args, &spec);
    CHECK(pid>0);
    close(channel[1]);
    close(frames[1]);
    close(errors[1]);
    broker.channel = channel[0];
    struct os_web_identity identity;
    CHECK(read(channel[0],&identity,sizeof(identity))==(ssize_t)sizeof(identity));
    struct os_web_identity own;
    CHECK(syscall(MODERNOS_WEB_IDENTITY,&own)==0);
    CHECK(identity.role==OS_WEB_RENDERER && identity.process==(unsigned)pid && identity.domain==own.domain &&
        !strcmp(identity.origin,broker.origin));
    broker.source = g_unix_fd_add(broker.channel, (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR),
                                  receive, &broker);
    struct Reader reader = {.frames = frames[0], .errors = errors[0], .pid = pid};
    pthread_t thread;
    CHECK(pthread_create(&thread,NULL,read_frames,&reader)==0);
    while (!atomic_load(&reader.done)) {
        for (unsigned i = 0; i < 32 && g_main_context_iteration(NULL, FALSE); i++) {
        }
        usleep(1000);
    }
    CHECK(pthread_join(thread,NULL)==0 && broker.received>=5);
    if (!broker.broken)
        g_source_remove(broker.source);
    close(broker.channel);
    soup_session_abort(session);
    while (g_hash_table_size(broker.requests))
        g_main_context_iteration(NULL, TRUE);
    g_hash_table_unref(broker.requests);
    printf(
        "PASS real brokered HTML/CSS/JS/fetch/PNG pixels and click, editing/wheel/form navigation in renderer pid=%d origin=%s\n",
        pid, broker.origin);
    g_free(broker.origin);
}

#include "native-broker.inc"

static int broker_main(int argc, char **argv) {
    struct os_web_identity identity;
    CHECK(syscall(MODERNOS_WEB_IDENTITY,&identity)==0 && identity.role==OS_WEB_BROKER);
    g_io_openssl_load(NULL);
    CHECK(g_tls_backend_supports_tls(g_tls_backend_get_default()));
    SoupSession *session = soup_session_new_with_options("timeout", 20, "max-conns", 8, NULL);
    GError *error = NULL;
    GTlsDatabase *database = g_tls_file_database_new("/etc/ssl/cert.pem", &error);
    CHECK(database && !error);
    g_object_set(session, "tls-database", database, NULL);
    g_object_unref(database);
    int end = argc;
    if (argc >= 5 && !strcmp(argv[argc - 2], "--fixture-ca")) {
        trust_fixture(session, argv[argc - 1]);
        end -= 2;
    }
    CHECK(end>2);
    for (int i = 2; i < end; i++)
        run_page(session, argv[i]);
    g_object_unref(session);
    puts(
        "WEBCORE NETWORK PAGE CHECK PASS (development embedder; desktop/public-site support unfinished)");
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 2 && !strcmp(argv[1], "--native"))
        return native_broker_main(argc, argv);
    alarm(180);
    CHECK(argc>=3 && (!strcmp(argv[1],"--check") || !strcmp(argv[1],"--broker-check")));
    if (!strcmp(argv[1], "--broker-check"))
        return broker_main(argc, argv);
    CHECK(mkdir("/home",0755)==0 || errno==EEXIST);
    CHECK(mkdir("/home/user",0700)==0 || errno==EEXIST);
    CHECK(mkdir("/home/user/.config",0700)==0 || errno==EEXIST);
    int channel[2], output[2];
    CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,channel)==0 && pipe(output)==0);
    char **args = calloc(argc + 1, sizeof(*args));
    CHECK(args);
    for (int i = 0; i < argc; i++)
        args[i] = argv[i];
    args[0] = "web-broker";
    args[1] = "--broker-check";
    struct os_spawn_web spec = {.role = OS_WEB_BROKER,
                                .ipc_fd = channel[1],
                                .stdout_fd = output[1],
                                .stderr_fd = output[1],
                                .home = (uintptr_t)"/home/user"};
    int pid = syscall(MODERNOS_SPAWN_WEB, "/bin/web-broker", args, &spec);
    CHECK(pid>0);
    close(channel[1]);
    close(output[1]);
    free(args);
    copy(output[0]);
    close(channel[0]);
    int status;
    CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
    return 0;
}
