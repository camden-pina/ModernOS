/* Validated structural snapshots. Existing CREATE/CLOSE owns process authority;
 * a tree record never creates a process, accepts a new origin, or grants IPC. */
static NativeBrowser *native_tree_root(NativeBrowser *view) {
    while (view->parent)
        view = view->parent;
    return view;
}

static void native_tree_clear(NativeBrowser *view) {
    for (unsigned i = 0; i < view->tree_count; i++) {
        g_bytes_unref(view->tree[i].name);
        g_free(view->tree[i].origin);
    }
    g_free(view->tree);
    view->tree = NULL;
    view->tree_count = 0;
    g_clear_pointer(&view->committed_origin, g_free);
    g_clear_pointer(&view->tree_url_origin, g_free);
    g_clear_pointer(&view->tree_export, g_bytes_unref);
    if (!view->parent)
        g_clear_pointer(&view->tree_names, g_hash_table_unref);
}

static NativeFrameNode *native_tree_node(NativeBrowser *view, uint64_t id) {
    for (unsigned i = 0; i < view->tree_count; i++)
        if (view->tree[i].frame == id)
            return &view->tree[i];
    return NULL;
}

static NativeBrowser *native_tree_owner(NativeBrowser *view, uint64_t id, NativeFrameNode **node) {
    NativeFrameNode *found = native_tree_node(view, id);
    if (found && found->local) {
        *node = found;
        return view;
    }
    /* During bootstrap the inherited root is bound by CREATE before HELLO. */
    if (id == (view->root_frame ? view->root_frame : view->frame_id)) {
        *node = NULL;
        return view;
    }
    for (unsigned i = 0; i < WEB_FRAME_MAX_CHILDREN; i++)
        if (view->children[i]) {
            NativeBrowser *owner = native_tree_owner(view->children[i], id, node);
            if (owner)
                return owner;
        }
    return NULL;
}

static char *native_tree_canonical_origin(const char *url) {
    if (!strcmp(url, "null"))
        return g_strdup(url);
    char *origin = origin_for(url);
    if (!origin)
        return NULL;
    /* Broker capabilities include explicit default ports; WebCore's standard
     * serialized origins omit them. The wire representation follows WebCore,
     * while authority comparisons still normalize through origin_for(). */
    char *port = strrchr(origin, ':');
    if (port && ((!strncmp(origin, "https://", 8) && !strcmp(port, ":443")) ||
                 (!strncmp(origin, "http://", 7) && !strcmp(port, ":80"))))
        *port = 0;
    return origin;
}

static const char *native_tree_origin(NativeBrowser *view) {
    if (view->committed_origin)
        return view->committed_origin;
    /* WebCore SandboxFlag::Origin is pinned to bit 2. Before commitment the
     * broker must not turn a sandboxed opaque document into its URL origin. */
    if (view->sandbox & 4U)
        return "null";
    const char *url = view->broker.origin ? view->broker.origin : view->pending;
    if (!view->tree_url_origin && url)
        view->tree_url_origin = native_tree_canonical_origin(url);
    return view->tree_url_origin;
}

static GBytes *native_tree_name(NativeBrowser *root, const NativeFrameNode *node) {
    GBytes *name = root->tree_names ? g_hash_table_lookup(root->tree_names, &node->frame) : NULL;
    return name ? name : node->name;
}

static int native_tree_append_record(GByteArray *bytes, uint32_t *count, uint64_t id,
                                     uint64_t parent, int local, GBytes *name, const char *origin) {
    if (*count >= WEB_FRAME_TREE_MAX_NODES || !origin)
        return 0;
    gsize name_size = 0;
    const void *name_bytes = name ? g_bytes_get_data(name, &name_size) : NULL;
    size_t origin_size = strlen(origin);
    if (name_size > WEB_FRAME_TREE_MAX_NAME || !origin_size ||
        origin_size > WEB_FRAME_TREE_MAX_ORIGIN ||
        bytes->len + sizeof(struct web_frame_tree_node) + name_size + origin_size >
            WEB_FRAME_TREE_MAX_BYTES)
        return 0;
    struct web_frame_tree_node node = {.frame = id,
                                       .parent = parent,
                                       .flags = local ? WEB_FRAME_TREE_LOCAL : 0,
                                       .name_bytes = name_size,
                                       .origin_bytes = origin_size};
    g_byte_array_append(bytes, (const void *)&node, sizeof(node));
    if (name_size)
        g_byte_array_append(bytes, name_bytes, name_size);
    g_byte_array_append(bytes, (const void *)origin, origin_size);
    (*count)++;
    return 1;
}

static int native_tree_append_view(NativeBrowser *view, NativeBrowser *recipient,
                                   NativeBrowser *root, uint64_t parent, GBytes *fallback_name,
                                   GByteArray *bytes, uint32_t *count, unsigned depth) {
    if (depth > WEB_FRAME_TREE_MAX_DEPTH)
        return 0;
    uint64_t bound = view->root_frame ? view->root_frame : view->frame_id;
    if (!bound)
        return 0;
    if (!view->tree_count) {
        return native_tree_append_record(bytes, count, bound, parent, view == recipient,
                                         fallback_name, native_tree_origin(view));
    }
    for (unsigned i = 0; i < view->tree_count; i++) {
        NativeFrameNode *node = &view->tree[i];
        uint64_t actual_parent = i ? node->parent : parent;
        GBytes *name = native_tree_name(root, node);
        if (node->local) {
            if (!native_tree_append_record(bytes, count, node->frame, actual_parent,
                                           view == recipient, name, node->origin))
                return 0;
        } else {
            NativeBrowser *child = native_child(view, node->frame);
            /* A removed child may still occur in an in-flight owned snapshot. */
            if (child && !native_tree_append_view(child, recipient, root, actual_parent, name,
                                                  bytes, count, depth + 1))
                return 0;
        }
    }

    return 1;
}

static GBytes *native_tree_export(NativeBrowser *recipient) {
    NativeBrowser *root = native_tree_root(recipient);
    if (!root->root_frame)
        return NULL;
    struct web_frame_tree_header header = {WEB_FRAME_TREE_MAGIC, WEB_FRAME_TREE_VERSION, 0, 0,
                                           root->root_frame};
    GByteArray *bytes = g_byte_array_sized_new(4096);
    g_byte_array_append(bytes, (const void *)&header, sizeof(header));
    if (!native_tree_append_view(root, recipient, root, 0, NULL, bytes, &header.count, 0)) {
        g_byte_array_unref(bytes);
        return NULL;
    }
    memcpy(bytes->data, &header, sizeof(header));
    struct web_frame_tree_span spans[WEB_FRAME_TREE_MAX_NODES];
    uint32_t count;
    uint64_t tree_root;
    if (!web_frame_tree_decode(bytes->data, bytes->len, spans, &count, &tree_root)) {
        g_byte_array_unref(bytes);
        return NULL;
    }
    return g_byte_array_free_to_bytes(bytes);
}

static int native_tree_send(NativeBrowser *view) {
    if (!view->root_frame || view->broker.channel < 0 || view->broker.broken)
        return 1;
    GBytes *bytes = native_tree_export(view);
    if (!bytes)
        return 0;
    if (!view->tree_export || !g_bytes_equal(view->tree_export, bytes) ||
        view->tree_export_revision != view->tree_revision) {
        gsize size;
        const void *data = g_bytes_get_data(bytes, &size);
        int body = web_resource_body(data, size);
        if (body < 0) {
            g_bytes_unref(bytes);
            return 0;
        }
        int sent = native_frame_forward(view,
                                        (struct web_frame_packet){.type = WEB_FRAME_TREE,
                                                                  .flags = 1,
                                                                  .frame = view->root_frame,
                                                                  .source = view->tree_revision,
                                                                  .body_bytes = size},
                                        NULL, body);
        close(body);
        if (!sent) {
            g_bytes_unref(bytes);
            return 0;
        }
        g_clear_pointer(&view->tree_export, g_bytes_unref);
        view->tree_export = g_bytes_ref(bytes);
        view->tree_export_revision = view->tree_revision;
    }

    g_bytes_unref(bytes);
    return 1;
}

static int native_tree_broadcast(NativeBrowser *view) {
    if (!native_tree_send(view))
        return 0;
    for (unsigned i = 0; i < WEB_FRAME_MAX_CHILDREN; i++)
        if (view->children[i] && !native_tree_broadcast(view->children[i]))
            return 0;
    return 1;
}

static int native_tree_accept(NativeBrowser *view, const void *bytes, size_t size,
                              uint64_t revision) {
    struct web_frame_tree_span spans[WEB_FRAME_TREE_MAX_NODES];
    uint32_t count;
    uint64_t root;
    if (!web_frame_tree_decode(bytes, size, spans, &count, &root) || root != view->root_frame ||
        !(spans[0].node.flags & WEB_FRAME_TREE_LOCAL))
        return 0;
    NativeFrameNode *nodes = g_new0(NativeFrameNode, count);
    int valid = 1;
    for (unsigned i = 0; i < count && valid; i++) {
        const struct web_frame_tree_span *span = &spans[i];
        const struct web_frame_tree_node *record = &span->node;
        NativeFrameNode *node = &nodes[i];
        node->frame = record->frame;
        node->parent = record->parent;
        node->local = !!(record->flags & WEB_FRAME_TREE_LOCAL);
        node->name = g_bytes_new(span->name, record->name_bytes);
        node->origin = g_strndup((const char *)span->origin, record->origin_bytes);
        char *canonical = native_tree_canonical_origin(node->origin);
        valid = canonical && !strcmp(canonical, node->origin);
        g_free(canonical);
        if (node->local) {
            char *bound = native_tree_canonical_origin(view->broker.origin);
            valid &=
                (node->frame == view->root_frame || (node->frame >> 32) == (unsigned)view->pid) &&
                (!strcmp(node->origin, "null") || (bound && !strcmp(node->origin, bound))) &&
                native_child(view, node->frame) == NULL;
            g_free(bound);
        } else {
            NativeFrameNode *previous = native_tree_node(view, node->frame);
            valid &= native_child(view, node->frame) != NULL || (previous && !previous->local);
        }
        if (i) {
            NativeFrameNode *parent = NULL;
            for (unsigned j = 0; j < i; j++)
                if (nodes[j].frame == node->parent)
                    parent = &nodes[j];
            valid &= parent && parent->local;
        }
    }

    if (!valid) {
        for (unsigned i = 0; i < count; i++) {
            if (nodes[i].name)
                g_bytes_unref(nodes[i].name);
            g_free(nodes[i].origin);
        }
        g_free(nodes);
        return 0;
    }

    NativeBrowser *top = native_tree_root(view);
    if (!top->tree_names)
        top->tree_names = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
                                                (GDestroyNotify)g_bytes_unref);
    for (unsigned i = 0; i < count; i++) {
        NativeFrameNode *previous = native_tree_node(view, nodes[i].frame);
        /* A stale name echo cannot undo a newer update from another renderer.
         * First isolated-root snapshots already contain the bootstrap name. */
        int initial_root_echo = i == 0 && view->parent && !previous &&
                                g_hash_table_contains(top->tree_names, &nodes[i].frame);
        if (!initial_root_echo && (!previous || !g_bytes_equal(previous->name, nodes[i].name))) {
            uint64_t *id = g_new(uint64_t, 1);
            *id = nodes[i].frame;
            g_hash_table_replace(top->tree_names, id, g_bytes_ref(nodes[i].name));
        }
    }

    for (unsigned i = 0; i < view->tree_count; i++) {
        g_bytes_unref(view->tree[i].name);
        g_free(view->tree[i].origin);
    }
    g_free(view->tree);
    view->tree = nodes;
    view->tree_count = count;
    view->tree_revision = revision;
    g_free(view->committed_origin);
    view->committed_origin = g_strdup(nodes[0].origin);
    GHashTableIter names;
    void *key;
    g_hash_table_iter_init(&names, top->tree_names);
    while (g_hash_table_iter_next(&names, &key, NULL)) {
        NativeFrameNode *owned = NULL;
        if (!native_tree_owner(top, *(uint64_t *)key, &owned))
            g_hash_table_iter_remove(&names);
    }
    return native_tree_broadcast(top);
}
