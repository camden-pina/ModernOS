/* The broker owns the frame graph. Each child has its own origin-bound
 * renderer, network channel and raster pipe. Cross-origin payloads are copied
 * into broker-owned sealed files; a foreign renderer's capability never passes
 * through to another renderer. */
static NativeBrowser *native_child(NativeBrowser *view, uint64_t frame) {
    for (unsigned i = 0; i < WEB_FRAME_MAX_CHILDREN; i++)
        if (view->children[i] && view->children[i]->frame_id == frame)
            return view->children[i];
    return NULL;
}

static unsigned native_frame_count(NativeBrowser *view) {
    unsigned count = 0;
    for (unsigned i = 0; i < WEB_FRAME_MAX_CHILDREN; i++)
        if (view->children[i])
            count += 1 + native_frame_count(view->children[i]);
    return count;
}

static void native_destroy_child(NativeBrowser *child) {
    native_stop_renderer(child);
    g_free(child->pending);
    g_free(child->current);
    g_free(child->requested);
    g_free(child->referrer);
    for (unsigned i = 0; i < child->count; i++)
        native_history_clear(&child->history[i]);
    g_clear_pointer(&child->writing, g_bytes_unref);
    while (!g_queue_is_empty(&child->output))
        g_bytes_unref(g_queue_pop_head(&child->output));
    g_free(child);
}

static void native_destroy_children(NativeBrowser *view) {
    for (unsigned i = 0; i < WEB_FRAME_MAX_CHILDREN; i++) {
        NativeBrowser *child = view->children[i];
        view->children[i] = NULL;
        if (child)
            native_destroy_child(child);
    }
}

static int native_frame_forward(NativeBrowser *destination, struct web_frame_packet packet,
                                const char *metadata, int body) {
    if (destination->broker.channel < 0 || destination->broker.broken)
        return 1;
    int copy = -1;
    if (packet.body_bytes) {
        void *mapped = mmap(NULL, packet.body_bytes, PROT_READ, MAP_SHARED, body, 0);
        if (mapped == MAP_FAILED)
            return 0;
        copy = web_resource_body(mapped, packet.body_bytes);
        munmap(mapped, packet.body_bytes);
        if (copy < 0)
            return 0;
    }

    if (web_frame_send(destination->broker.channel, packet, metadata, copy) < 0)
        destination->broker.broken = 1;
    if (copy >= 0)
        close(copy);
    return 1;
}

#include "native-frame-tree.inc"

static void native_focus_path(NativeBrowser *view, NativeBrowser *focused) {
    uint64_t target = view == focused ? view->root_frame : 0;
    for (NativeBrowser *branch = focused; branch && branch->parent; branch = branch->parent)
        if (branch->parent == view) {
            target = branch->frame_id;
            break;
        }
    view->focused_frame = target;
    if (view->root_frame)
        native_frame_forward(view,
                             (struct web_frame_packet){.type = WEB_FRAME_FOCUS,
                                                       .frame = view->root_frame,
                                                       .source = target},
                             NULL, -1);
    for (unsigned i = 0; i < WEB_FRAME_MAX_CHILDREN; i++)
        if (view->children[i])
            native_focus_path(view->children[i], focused);
}

static void native_focus(NativeBrowser *focused) {
    NativeBrowser *root = focused;
    while (root->parent)
        root = root->parent;
    native_focus_path(root, focused);
}

static void native_activation_forward(NativeBrowser *view, unsigned type, uint64_t timestamp) {
    if (view->root_frame)
        native_frame_forward(
            view,
            (struct web_frame_packet){.type = type, .frame = view->root_frame, .source = timestamp},
            NULL, -1);
}

static void native_activate_descendants(NativeBrowser *view, const char *origin,
                                        uint64_t timestamp) {
    for (unsigned i = 0; i < WEB_FRAME_MAX_CHILDREN; i++)
        if (view->children[i]) {
            NativeBrowser *child = view->children[i];
            if (child->broker.origin && !strcmp(child->broker.origin, origin))
                native_activation_forward(child, WEB_FRAME_ACTIVATE, timestamp);
            native_activate_descendants(child, origin, timestamp);
        }
}

static void native_consume_activations(NativeBrowser *view, NativeBrowser *sender) {
    if (view != sender)
        native_activation_forward(view, WEB_FRAME_CONSUME_ACTIVATION, 0);
    for (unsigned i = 0; i < WEB_FRAME_MAX_CHILDREN; i++)
        if (view->children[i])
            native_consume_activations(view->children[i], sender);
}

static void native_send_surface(NativeBrowser *child) {
    if (!child->parent || !child->surface_dirty || child->awaiting_surface || !child->surface)
        return;
    if (child->parent->broker.broken || child->parent->broker.channel < 0)
        return;
    gsize size;
    const void *bytes = g_bytes_get_data(child->surface, &size);
    int fd = web_resource_body(bytes, size);
    if (fd < 0) {
        child->quit = 1;
        return;
    }
    struct web_frame_packet packet = {.type = WEB_FRAME_SURFACE,
                                      .body_bytes = size,
                                      .frame = child->frame_id,
                                      .width = child->surface_width,
                                      .height = child->surface_height};
    if (web_frame_send(child->parent->broker.channel, packet, NULL, fd) < 0)
        child->parent->broker.broken = 1;
    else {
        child->awaiting_surface = 1;
        child->surface_dirty = 0;
    }
    close(fd);
}

static void native_child_surface(NativeBrowser *child, const struct browser_packet *packet,
                                 const void *body) {
    if (packet->type == BROWSER_FRAME) {
        g_clear_pointer(&child->surface, g_bytes_unref);
        child->surface = g_bytes_new(body, packet->bytes);
        child->surface_width = packet->width;
        child->surface_height = packet->height;
    } else if (packet->type == BROWSER_PRESENT && child->surface &&
               packet->canvas_width == (unsigned)child->surface_width &&
               packet->canvas_height == (unsigned)child->surface_height) {
        child->surface_dirty = 1;
        native_send_surface(child);
    }
}

static int native_frame_control(Broker *broker, const struct web_frame_packet *packet,
                                const char *metadata, int body) {
    NativeBrowser *view = broker->native;
    if (packet->type == WEB_FRAME_HELLO) {
        if (packet->metadata_bytes || packet->body_bytes || packet->flags || packet->source ||
            packet->x || packet->y || packet->width || packet->height || view->root_frame)
            return 0;
        if (view->parent ? packet->frame != view->frame_id
                         : (packet->frame >> 32) != (unsigned)view->pid)
            return 0;
        view->root_frame = packet->frame;
        // A new child starts without focus. A focused child navigation keeps
        // the parent-owned focus path, including across renderer replacement.
        if (!view->parent || view->parent->focused_frame == view->frame_id)
            native_focus(view);
        return 1;
    }

    if (!view->root_frame)
        return 0;
    if (packet->type == WEB_FRAME_TREE) {
        if (packet->frame != view->root_frame || packet->metadata_bytes || !packet->body_bytes ||
            packet->body_bytes > WEB_FRAME_TREE_MAX_BYTES || packet->flags || !packet->source ||
            packet->source <= view->tree_revision || packet->x || packet->y || packet->width ||
            packet->height)
            return 0;
        void *bytes = mmap(NULL, packet->body_bytes, PROT_READ, MAP_SHARED, body, 0);
        if (bytes == MAP_FAILED)
            return 0;
        int valid = native_tree_accept(view, bytes, packet->body_bytes, packet->source);
        munmap(bytes, packet->body_bytes);
        return valid;
    }

    if (packet->type == WEB_FRAME_ACTIVATE || packet->type == WEB_FRAME_CONSUME_ACTIVATION) {
        if (packet->frame != view->root_frame || packet->metadata_bytes || packet->body_bytes ||
            packet->flags > 1 || packet->x || packet->y || packet->width || packet->height)
            return 0;
        if (packet->type == WEB_FRAME_CONSUME_ACTIVATION) {
            if (packet->source || packet->flags)
                return 0;
            NativeBrowser *root = view;
            while (root->parent)
                root = root->parent;
            native_consume_activations(root, view);
        } else {
            // All guest processes share CLOCK_MONOTONIC. Preserve WebCore's
            // original timestamp, never replace it with the IPC receipt time.
            double seconds;
            memcpy(&seconds, &packet->source, sizeof(seconds));
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
                return 0;
            double current = (double)now.tv_sec + (double)now.tv_nsec / 1e9;
            if (!isfinite(seconds) || seconds < 0 || seconds > current + 0.001)
                return 0;
            for (NativeBrowser *branch = view; branch->parent; branch = branch->parent)
                native_frame_forward(branch->parent,
                                     (struct web_frame_packet){.type = WEB_FRAME_ACTIVATE,
                                                               .flags = 1,
                                                               .frame = branch->frame_id,
                                                               .source = packet->source},
                                     NULL, -1);
            if (!packet->flags)
                native_activate_descendants(view, broker->origin, packet->source);
        }

        return 1;
    }

    if (packet->type == WEB_FRAME_FOCUS) {
        if (packet->frame != view->root_frame || packet->metadata_bytes || packet->body_bytes ||
            packet->flags || packet->x || packet->y || packet->width || packet->height)
            return 0;
        NativeBrowser *focused =
            packet->source == view->root_frame ? view : native_child(view, packet->source);
        // Late focus notifications for a removed child are harmless. A frame
        // identifier outside this renderer's immediate branch is never routed.
        if (!focused)
            return 1;
        native_focus(focused);
        return 1;
    }

    if (packet->type == WEB_FRAME_CREATE) {
        if (!packet->metadata_bytes || packet->metadata_bytes > WEB_FRAME_MAX_METADATA ||
            packet->body_bytes || packet->flags > UINT16_MAX || packet->x || packet->y ||
            packet->width < 1 || packet->height < 1 || packet->width > 4096 ||
            packet->height > 4096 ||
            (uint64_t)packet->width * packet->height > WEB_FRAME_MAX_BODY / 4 ||
            (packet->frame >> 32) != (unsigned)view->pid)
            return 0;
        NativeFrameNode *owner = native_tree_node(view, packet->source);
        if (!owner || !owner->local)
            return 0;
        const char *separator = memchr(metadata, 0, packet->metadata_bytes);
        if (!separator || separator == metadata || separator - metadata > 4096 ||
            memchr(separator + 1, 0, metadata + packet->metadata_bytes - separator - 1))
            return 0;
        char *url = g_strndup(metadata, separator - metadata), *origin = origin_for(url);
        if (!origin) {
            g_free(url);
            return 0;
        }
        g_free(origin);
        char *referrer =
            g_strndup(separator + 1, metadata + packet->metadata_bytes - separator - 1);
        int valid = 1;
        for (const char *p = referrer; *p; p++)
            if ((unsigned char)*p < 32 || (unsigned char)*p == 127)
                valid = 0;
        if (*referrer) {
            char *source_origin = origin_for(referrer);
            valid &= source_origin && !strcmp(source_origin, broker->origin);
            g_free(source_origin);
        }
        if (!valid) {
            g_free(url);
            g_free(referrer);
            return 0;
        }
        NativeBrowser *child = native_child(view, packet->frame);
        if (!child) {
            NativeBrowser *root = view;
            unsigned depth = 1;
            while (root->parent) {
                root = root->parent;
                depth++;
            }
            if (depth > 3 || native_frame_count(root) >= WEB_FRAME_MAX_CHILDREN) {
                fprintf(stderr, "Embedded frame quota exceeded\n");
                g_free(url);
                g_free(referrer);
                return 1;
            }

            unsigned slot = 0;
            while (slot < WEB_FRAME_MAX_CHILDREN && view->children[slot])
                slot++;
            if (slot == WEB_FRAME_MAX_CHILDREN) {
                g_free(url);
                g_free(referrer);
                return 1;
            }
            child = g_new0(NativeBrowser, 1);
            child->parent = view;
            child->frame_id = packet->frame;
            child->broker.session = broker->session;
            child->broker.channel = -1;
            child->broker.native = child;
            child->frames = -1;
            child->errors = -1;
            child->storage = view->storage;
            child->cookies = view->cookies;
            child->session_storage = view->session_storage;
            view->children[slot] = child;
        }

        // Parent-initiated navigation creates a new document with the parent's
        // referrer policy, even when the target reuses the child's origin.
        if (child->pid)
            native_stop_renderer(child);
        g_free(child->referrer);
        child->referrer = referrer;
        child->width = packet->width;
        child->height = packet->height;
        child->sandbox = packet->flags;
        child->screen_width = view->screen_width;
        child->screen_height = view->screen_height;
        child->available_height = view->available_height;
        child->window_x = view->window_x;
        child->window_y = view->window_y;
        child->window_width = view->window_width;
        child->window_height = view->window_height;
        g_free(child->pending);
        child->pending = url;
        child->hops = 0;
        return 1;
    }

    if (packet->type == WEB_FRAME_CLOSE || packet->type == WEB_FRAME_ACK ||
        packet->type == WEB_FRAME_RESIZE) {
        if (packet->metadata_bytes || packet->body_bytes || packet->flags || packet->source ||
            packet->x || packet->y)
            return 0;
        if (packet->type != WEB_FRAME_RESIZE && (packet->width || packet->height))
            return 0;
        if (packet->type == WEB_FRAME_RESIZE &&
            (packet->width < 1 || packet->height < 1 || packet->width > 4096 ||
             packet->height > 4096 ||
             (uint64_t)packet->width * packet->height > WEB_FRAME_MAX_BODY / 4))
            return 0;
        NativeBrowser *child = native_child(view, packet->frame);
        if (!child)
            return 1;
        if (packet->type == WEB_FRAME_CLOSE) {
            if (view->focused_frame == child->frame_id)
                native_focus(view);
            for (unsigned i = 0; i < WEB_FRAME_MAX_CHILDREN; i++)
                if (view->children[i] == child)
                    view->children[i] = NULL;
            native_destroy_child(child);
        } else if (packet->type == WEB_FRAME_ACK) {
            child->awaiting_surface = 0;
            native_send_surface(child);
        } else {
            child->width = packet->width;
            child->height = packet->height;
            struct browser_packet resize = {
                .type = BROWSER_RESIZE, .width = child->width, .height = child->height};
            if (child->broker.channel >= 0 &&
                browser_control_send(child->broker.channel, resize, NULL) < 0)
                child->broker.broken = 1;
        }

        return 1;
    }

    if (packet->type == WEB_FRAME_COMMITTED) {
        if (!view->parent || packet->frame != view->frame_id || !packet->metadata_bytes ||
            packet->metadata_bytes > 256 || packet->body_bytes || packet->flags > 2 ||
            packet->source || packet->x || packet->y || packet->width || packet->height ||
            memchr(metadata, 0, packet->metadata_bytes))
            return 0;
        char *claimed = g_strndup(metadata, packet->metadata_bytes),
             *canonical = origin_for(claimed);
        int valid = !strcmp(claimed, "null") || (canonical && !strcmp(canonical, broker->origin));
        if (valid) {
            g_free(view->committed_origin);
            view->committed_origin = g_strdup(claimed);
        }
        g_free(canonical);
        g_free(claimed);
        return valid && native_frame_forward(view->parent, *packet, metadata, -1) &&
               native_tree_broadcast(native_tree_root(view));
    }

    if (packet->type == WEB_FRAME_MESSAGE) {
        if (!packet->body_bytes || !packet->metadata_bytes || packet->metadata_bytes > 514 ||
            packet->flags || packet->x || packet->y || packet->width || packet->height)
            return 0;
        const char *separator = memchr(metadata, 0, packet->metadata_bytes);
        if (!separator || separator == metadata ||
            separator == metadata + packet->metadata_bytes - 1 ||
            memchr(separator + 1, 0, metadata + packet->metadata_bytes - separator - 1))
            return 0;
        char *from = g_strndup(metadata, separator - metadata),
             *to = g_strndup(separator + 1, metadata + packet->metadata_bytes - separator - 1);
        char *source_origin = origin_for(from), *target_origin = origin_for(to);
        NativeFrameNode *source_node = native_tree_node(view, packet->source);
        int valid =
            source_node && source_node->local && !strcmp(from, source_node->origin) &&
            (!strcmp(from, "null") || (source_origin && !strcmp(source_origin, broker->origin))) &&
            (!strcmp(to, "*") || target_origin);
        g_free(from);
        g_free(to);
        g_free(source_origin);
        g_free(target_origin);
        if (!valid)
            return 0;
        NativeFrameNode *destination_node = NULL;
        NativeBrowser *destination =
            native_tree_owner(native_tree_root(view), packet->frame, &destination_node);
        if (!destination)
            return 1; /* Removed frames cannot receive late messages. */
        /* Seqpacket order installs a newly known source mirror before dispatch.
         * Only an actual local target in this tree acquires a message; neither
         * structural metadata nor a guessed ID grants a foreign channel. */
        return native_tree_send(destination) &&
               native_frame_forward(destination, *packet, metadata, body);
    }

    if (packet->type == WEB_FRAME_INPUT) {
        if (packet->metadata_bytes || packet->body_bytes ||
            (packet->height != 1 && packet->height != 2) ||
            packet->flags > (packet->height == 1 ? 7U : 1U) ||
            (packet->height == 1 && (packet->width < -32 || packet->width > 32 ||
                                     (packet->source >> 32) >= (unsigned)view->screen_width ||
                                     (uint32_t)packet->source >= (unsigned)view->screen_height)) ||
            (packet->height == 2 &&
             (packet->source || packet->width || packet->y || packet->x < 0 || packet->x > 65535)))
            return 0;
        NativeBrowser *child = native_child(view, packet->frame);
        if (!child)
            return 1;
        return native_frame_forward(child, *packet, NULL, -1);
    }

    return 0;
}

static void native_tick_children(NativeBrowser *view) {
    for (unsigned i = 0; i < WEB_FRAME_MAX_CHILDREN; i++) {
        NativeBrowser *child = view->children[i];
        if (!child)
            continue;
        native_read_frames(child);
        native_read_errors(child);
        native_send_surface(child);
        native_tick_children(child);
        if (child->pending && !child->quit) {
            char *url = child->pending;
            child->pending = NULL;
            if (native_start_renderer(child, url, 0) < 0) {
                fprintf(stderr, "Embedded renderer start: %s\n", strerror(errno));
                child->quit = 1;
            }
            g_free(url);
        } else if (child->pid) {
            int status = 0;
            int exited = waitpid(child->pid, &status, WNOHANG);
            if (exited == child->pid) {
                fprintf(stderr, "Embedded renderer exit: status=%d\n", status);
                child->pid = 0;
                child->quit = 1;
            }
        }

        if (child->quit || child->broker.broken) {
            view->children[i] = NULL;
            native_destroy_child(child);
        }
    }
}
