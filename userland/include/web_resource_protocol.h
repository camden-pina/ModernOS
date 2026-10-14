#ifndef MODERNOS_WEB_RESOURCE_PROTOCOL_H
#define MODERNOS_WEB_RESOURCE_PROTOCOL_H
#include <stdint.h>

/* Versioned broker resource messages on the origin-bound Unix channel.
 * Metadata is one bounded seqpacket; nonempty bodies are sealed memfds.
 * The native surface/input protocol remains the separate 40-byte ABI. */
#define WEB_RESOURCE_MAGIC 0x3152574dU
#define WEB_RESOURCE_VERSION 2U
#define WEB_RESOURCE_MAX_METADATA 32768U
#define WEB_RESOURCE_MAX_BODY (16U * 1024U * 1024U)
#define WEB_RESOURCE_MAX_ACTIVE 8U

enum {
    WEB_RESOURCE_REQUEST = 1,
    WEB_RESOURCE_RESPONSE,
    WEB_RESOURCE_ERROR,
    WEB_RESOURCE_CANCEL,
    WEB_SERVICE_REQUEST,
    WEB_SERVICE_RESPONSE
};

enum { WEB_RESOURCE_MAIN = 1U, WEB_RESOURCE_CREDENTIALS = 2U, WEB_RESOURCE_DOWNLOAD = 4U };
/* Services share the checked, sealed-body transport. Their URL determines the
 * authority; headers_bytes holds a binary storage key and body the value.
 * Request status is an operation; response status is an errno or zero. */
enum {
    WEB_SERVICE_COOKIE_GET = 1,
    WEB_SERVICE_COOKIE_SET,
    WEB_SERVICE_STORAGE_LENGTH,
    WEB_SERVICE_STORAGE_KEY,
    WEB_SERVICE_STORAGE_GET,
    WEB_SERVICE_STORAGE_SET,
    WEB_SERVICE_STORAGE_REMOVE,
    WEB_SERVICE_STORAGE_CLEAR,
    WEB_SERVICE_HISTORY_REQUEST,
    WEB_SERVICE_AUDIO_INFO,
    WEB_SERVICE_AUDIO_WRITE,
    WEB_SERVICE_AUDIO_RESET,
    WEB_SERVICE_FRAME_TREE
};

enum { WEB_SERVICE_SESSION = 1U, WEB_SERVICE_FOUND = 1U };
#define WEB_SERVICE_STORAGE_QUOTA (5U * 1024U * 1024U)

struct web_resource_packet {
    uint32_t magic, version, type, metadata_bytes, id, flags, status, body_bytes;
    uint32_t url_bytes, method_bytes, headers_bytes, error_bytes, metrics_bytes, reserved[3];
};

/* Response-only transport measurements follow the HTTP headers in metadata.
 * Times use CLOCK_MONOTONIC microseconds shared by the broker and renderer.
 * No HTTP headers, addresses, cookies or TLS secrets enter this record. */
enum { WEB_NETWORK_HTTP10 = 1, WEB_NETWORK_HTTP11, WEB_NETWORK_HTTP2 };
struct web_network_metrics {
    uint64_t fetch_start, dns_start, dns_end, connect_start, tls_start, connect_end;
    uint64_t request_start, response_start, response_end, encoded_body_bytes;
    uint32_t protocol, reserved;
};

#ifdef __cplusplus
static_assert(sizeof(web_resource_packet) == 64, "resource protocol v2");
static_assert(sizeof(web_network_metrics) == 88, "network metrics v2");
#else
_Static_assert(sizeof(struct web_resource_packet) == 64, "resource protocol v2");
_Static_assert(sizeof(struct web_network_metrics) == 88, "network metrics v2");
#endif
#endif
