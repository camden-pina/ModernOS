#ifndef MODERNOS_WEB_FRAME_PROTOCOL_H
#define MODERNOS_WEB_FRAME_PROTOCOL_H
#include <stdint.h>
#include "web_frame_tree.h"

#define WEB_FRAME_MAGIC 0x4d46524dU
#define WEB_FRAME_VERSION 5U
#define WEB_FRAME_MAX_METADATA 8192U
#define WEB_FRAME_MAX_BODY (16U * 1024U * 1024U)
#define WEB_FRAME_MAX_CHILDREN 8U

enum {
    WEB_FRAME_HELLO = 1,
    WEB_FRAME_CREATE,
    WEB_FRAME_CLOSE,
    WEB_FRAME_RESIZE,
    WEB_FRAME_MESSAGE,
    WEB_FRAME_SURFACE,
    WEB_FRAME_COMMITTED,
    WEB_FRAME_INPUT,
    WEB_FRAME_ACK,
    WEB_FRAME_FOCUS,
    WEB_FRAME_ACTIVATE,
    WEB_FRAME_CONSUME_ACTIVATION,
    WEB_FRAME_TREE
};

/* Frame identifiers belong to this browser's tree. The trusted broker checks
 * every edge before routing; a renderer never receives a foreign IPC socket.
 * CREATE metadata is URL, NUL, then the WebCore-computed HTTP referrer.
 * FOCUS names the sender's root in frame and its focused root/direct child in
 * source. Broker updates use the recipient root and its branch of the focus
 * path, or source=0 when focus is outside that recipient's subtree.
 * ACTIVATE carries the actual monotonic timestamp's double bits in source;
 * sender flag=1 identifies an opaque-origin local source. The broker selects
 * real ancestors and same-origin descendants. Incoming flag=1 identifies
 * the remote child in frame and updates only its actual local ancestors;
 * incoming flag=0 updates a same-origin recipient subtree. CONSUME_ACTIVATION clears
 * transient activation across the whole browser frame tree.
 * TREE has no metadata and a sealed bounded tree body. Renderer flag=0 exports
 * its actual owned subtree, with monotonic revision in source. Broker flag=1
 * exports canonical ancestry/siblings and acknowledges that recipient revision.
 * CREATE source identifies the actual owned local parent; process authority
 * remains the existing origin-bound CREATE/CLOSE channel. */
struct web_frame_packet {
    uint32_t magic, version, type, metadata_bytes, body_bytes, flags;
    /* INPUT with height=1 packs the trusted pointer's screen X/Y into source.
     * Message packets use source as a frame identifier. */
    uint64_t frame, source;
    int32_t x, y, width, height;
    uint32_t reserved[2];
};

#endif
