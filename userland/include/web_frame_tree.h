#ifndef MODERNOS_WEB_FRAME_TREE_H
#define MODERNOS_WEB_FRAME_TREE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* A tree is structural metadata, never a new origin/process capability.
 * Records are preorder. Each fixed record is followed by name_bytes UTF-8
 * bytes and origin_bytes ASCII bytes, with no terminators or padding. Names
 * retain embedded NUL; origins never contain NUL. An owned snapshot contains
 * only the bound local root, its local descendants and existing remote leaves.
 * The broker exports a complete tree, marking nodes local to each recipient.
 */
#define WEB_FRAME_TREE_MAGIC 0x4d545246U
#define WEB_FRAME_TREE_VERSION 1U
#define WEB_FRAME_TREE_MAX_NODES 128U
#define WEB_FRAME_TREE_MAX_BYTES (1024U * 1024U)
#define WEB_FRAME_TREE_MAX_NAME 4096U
#define WEB_FRAME_TREE_MAX_ORIGIN 256U
#define WEB_FRAME_TREE_MAX_DEPTH 32U

enum { WEB_FRAME_TREE_LOCAL = 1U };
struct web_frame_tree_header {
    uint32_t magic, version, count, reserved;
    uint64_t root;
};

struct web_frame_tree_node {
    uint64_t frame, parent;
    uint32_t flags, name_bytes, origin_bytes, reserved;
};

struct web_frame_tree_span {
    struct web_frame_tree_node node;
    const unsigned char *name, *origin;
    uint32_t depth;
};

static inline int web_frame_tree_utf8(const unsigned char *text, size_t size) {
    for (size_t i = 0; i < size;) {
        unsigned char first = text[i++];
        if (first < 0x80)
            continue;
        unsigned need;
        uint32_t value, minimum;
        if (first >= 0xc2 && first <= 0xdf) {
            need = 1;
            value = first & 31;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            need = 2;
            value = first & 15;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            need = 3;
            value = first & 7;
            minimum = 0x10000;
        } else
            return 0;
        if (need > size - i)
            return 0;
        for (unsigned j = 0; j < need; j++) {
            unsigned char next = text[i++];
            if ((next & 0xc0) != 0x80)
                return 0;
            value = (value << 6) | (next & 63);
        }

        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return 0;
    }

    return 1;
}

/* Parse structural bounds independently of either browser or broker. Authority
 * and canonical-origin checks are mandatory at the broker after this parser.
 * Parent-before-child is insufficient: ancestors must also establish preorder,
 * so a late child cannot reopen a sibling subtree and change indexed order.
 */
static inline int web_frame_tree_decode(const void *data, size_t size,
                                        struct web_frame_tree_span spans[WEB_FRAME_TREE_MAX_NODES],
                                        uint32_t *count, uint64_t *root) {
    if (!data || size < sizeof(struct web_frame_tree_header) || size > WEB_FRAME_TREE_MAX_BYTES)
        return 0;
    const unsigned char *bytes = (const unsigned char *)data;
    struct web_frame_tree_header header;
    memcpy(&header, bytes, sizeof(header));
    if (header.magic != WEB_FRAME_TREE_MAGIC || header.version != WEB_FRAME_TREE_VERSION ||
        header.reserved || !header.root || !header.count || header.count > WEB_FRAME_TREE_MAX_NODES)
        return 0;
    uint64_t ancestors[WEB_FRAME_TREE_MAX_DEPTH + 1];
    unsigned ancestry = 0;
    size_t offset = sizeof(header);
    for (unsigned index = 0; index < header.count; index++) {
        if (size - offset < sizeof(struct web_frame_tree_node))
            return 0;
        struct web_frame_tree_span *span = &spans[index];
        memcpy(&span->node, bytes + offset, sizeof(span->node));
        offset += sizeof(span->node);
        struct web_frame_tree_node *node = &span->node;
        if (!node->frame || node->reserved || node->flags & ~WEB_FRAME_TREE_LOCAL ||
            node->name_bytes > WEB_FRAME_TREE_MAX_NAME || !node->origin_bytes ||
            node->origin_bytes > WEB_FRAME_TREE_MAX_ORIGIN ||
            (uint64_t)node->name_bytes + node->origin_bytes > size - offset)
            return 0;
        for (unsigned previous = 0; previous < index; previous++)
            if (spans[previous].node.frame == node->frame)
                return 0;
        span->depth = 0;
        if (!index) {
            if (node->frame != header.root || node->parent)
                return 0;
        } else {
            while (ancestry && ancestors[ancestry - 1] != node->parent)
                ancestry--;
            if (!ancestry || ancestry > WEB_FRAME_TREE_MAX_DEPTH)
                return 0;
            span->depth = ancestry;
        }

        ancestors[ancestry++] = node->frame;
        span->name = bytes + offset;
        offset += node->name_bytes;
        span->origin = bytes + offset;
        offset += node->origin_bytes;
        if (!web_frame_tree_utf8(span->name, node->name_bytes))
            return 0;
        for (unsigned j = 0; j < node->origin_bytes; j++)
            if (span->origin[j] < 0x21 || span->origin[j] > 0x7e)
                return 0;
    }

    if (offset != size)
        return 0;
    *count = header.count;
    *root = header.root;
    return 1;
}

#ifdef __cplusplus
static_assert(sizeof(web_frame_tree_header) == 24, "frame tree header v1");
static_assert(sizeof(web_frame_tree_node) == 32, "frame tree node v1");
#else
_Static_assert(sizeof(struct web_frame_tree_header) == 24, "frame tree header v1");
_Static_assert(sizeof(struct web_frame_tree_node) == 32, "frame tree node v1");
#endif
#endif
