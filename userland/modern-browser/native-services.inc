/* Broker-owned cookies and binary UTF-16 Web Storage. Local storage is shared
 * in a private SQLite database; session storage stays in this window. */
typedef struct {
    GBytes *key, *value;
} NativeStorageEntry;

static void native_storage_entry_free(void *data) {
    NativeStorageEntry *entry = data;
    g_bytes_unref(entry->key);
    g_bytes_unref(entry->value);
    g_free(entry);
}

static void native_service_reply(Broker *broker, const struct web_resource_packet *request,
                                 const char *url, unsigned error, unsigned flags, const void *body,
                                 size_t size) {
    struct web_resource_packet reply;
    init(&reply, WEB_SERVICE_RESPONSE, request->id);
    reply.status = error;
    reply.flags = flags;
    reply.url_bytes = reply.metadata_bytes = strlen(url);
    reply.body_bytes = size;
    int fd = size ? web_resource_body(body, size) : -1;
    if (size && fd < 0) {
        reply.status = ENOMEM;
        reply.flags = 0;
        reply.body_bytes = 0;
    }
    if (web_resource_send(broker->channel, &reply, url, fd) < 0)
        broker->broken = 1;
    if (fd >= 0)
        close(fd);
}

static int native_sql_prepare(NativeBrowser *view, const char *sql, const char *origin,
                              sqlite3_stmt **statement) {
    if (!view->storage || sqlite3_prepare_v2(view->storage, sql, -1, statement, NULL) != SQLITE_OK)
        return 0;
    return sqlite3_bind_text(*statement, 1, origin, -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

static int native_local_storage(NativeBrowser *view, const struct web_resource_packet *request,
                                const char *url, const void *key, size_t key_size,
                                const void *value, size_t value_size) {
    Broker *broker = &view->broker;
    sqlite3_stmt *statement = NULL;
    unsigned error = EIO, flags = 0;
    GBytes *result = NULL;
    int operation = request->status, transaction = 0;
    const char *sql = NULL;
    if (operation == WEB_SERVICE_STORAGE_LENGTH)
        sql = "SELECT count(*) FROM storage WHERE origin=?1";
    else if (operation == WEB_SERVICE_STORAGE_KEY)
        sql = "SELECT k FROM storage WHERE origin=?1 ORDER BY rowid LIMIT 1 OFFSET ?2";
    else if (operation == WEB_SERVICE_STORAGE_GET)
        sql = "SELECT v FROM storage WHERE origin=?1 AND k=?2";
    else {
        if (!view->storage ||
            sqlite3_exec(view->storage, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
            goto done;
        transaction = 1;
        if (operation == WEB_SERVICE_STORAGE_SET) {
            if (!native_sql_prepare(
                    view,
                    "SELECT coalesce(sum(length(k)+length(v)),0) FROM storage WHERE origin=?1",
                    broker->origin, &statement))
                goto done;
            if (sqlite3_step(statement) != SQLITE_ROW)
                goto done;
            sqlite3_int64 total = sqlite3_column_int64(statement, 0);
            sqlite3_finalize(statement);
            statement = NULL;
            if (!native_sql_prepare(
                    view, "SELECT length(k)+length(v) FROM storage WHERE origin=?1 AND k=?2",
                    broker->origin, &statement) ||
                sqlite3_bind_blob(statement, 2, key, key_size, SQLITE_TRANSIENT) != SQLITE_OK)
                goto done;
            if (sqlite3_step(statement) == SQLITE_ROW)
                total -= sqlite3_column_int64(statement, 0);
            sqlite3_finalize(statement);
            statement = NULL;
            if (total + key_size + value_size > WEB_SERVICE_STORAGE_QUOTA) {
                error = ENOSPC;
                goto done;
            }
            sql =
                "INSERT INTO storage(origin,k,v) VALUES(?1,?2,?3) ON CONFLICT(origin,k) DO UPDATE SET v=excluded.v";
        } else if (operation == WEB_SERVICE_STORAGE_REMOVE)
            sql = "DELETE FROM storage WHERE origin=?1 AND k=?2";
        else
            sql = "DELETE FROM storage WHERE origin=?1";
    }

    if (!native_sql_prepare(view, sql, broker->origin, &statement))
        goto done;
    if (operation == WEB_SERVICE_STORAGE_KEY) {
        uint32_t index;
        memcpy(&index, value, sizeof(index));
        if (sqlite3_bind_int64(statement, 2, index) != SQLITE_OK)
            goto done;
    } else if (operation == WEB_SERVICE_STORAGE_GET || operation == WEB_SERVICE_STORAGE_SET ||
               operation == WEB_SERVICE_STORAGE_REMOVE) {
        if (sqlite3_bind_blob(statement, 2, key ? key : "", key_size, SQLITE_TRANSIENT) !=
            SQLITE_OK)
            goto done;
        if (operation == WEB_SERVICE_STORAGE_SET &&
            sqlite3_bind_blob(statement, 3, value ? value : "", value_size, SQLITE_TRANSIENT) !=
                SQLITE_OK)
            goto done;
    }

    int step = sqlite3_step(statement);
    if (step != SQLITE_ROW && step != SQLITE_DONE)
        goto done;
    if (operation == WEB_SERVICE_STORAGE_LENGTH && step == SQLITE_ROW) {
        uint32_t count = sqlite3_column_int64(statement, 0);
        result = g_bytes_new(&count, sizeof(count));
        flags = WEB_SERVICE_FOUND;
    } else if ((operation == WEB_SERVICE_STORAGE_KEY || operation == WEB_SERVICE_STORAGE_GET) &&
               step == SQLITE_ROW) {
        result = g_bytes_new(sqlite3_column_blob(statement, 0), sqlite3_column_bytes(statement, 0));
        flags = WEB_SERVICE_FOUND;
    }
    error = 0;
done:
    if (statement)
        sqlite3_finalize(statement);
    if (transaction) {
        if (error)
            sqlite3_exec(view->storage, "ROLLBACK", NULL, NULL, NULL);
        else if (sqlite3_exec(view->storage, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_exec(view->storage, "ROLLBACK", NULL, NULL, NULL);
            error = EIO;
        }
    }

    gsize size = 0;
    const void *bytes = result ? g_bytes_get_data(result, &size) : NULL;
    native_service_reply(broker, request, url, error, flags, bytes, size);
    if (result)
        g_bytes_unref(result);
    return 1;
}

static size_t native_session_bytes(NativeBrowser *view) {
    size_t total = 0;
    GHashTableIter iterator;
    void *data;
    g_hash_table_iter_init(&iterator, view->session_storage);
    while (g_hash_table_iter_next(&iterator, NULL, &data)) {
        GPtrArray *entries = data;
        for (unsigned i = 0; i < entries->len; i++) {
            NativeStorageEntry *entry = g_ptr_array_index(entries, i);
            total += g_bytes_get_size(entry->key) + g_bytes_get_size(entry->value);
        }
    }

    return total;
}

static int native_session_storage(NativeBrowser *view, const struct web_resource_packet *request,
                                  const char *url, const void *key, size_t key_size,
                                  const void *value, size_t value_size) {
    Broker *broker = &view->broker;
    GPtrArray *entries = g_hash_table_lookup(view->session_storage, broker->origin);
    unsigned error = 0, flags = 0, index = 0;
    GBytes *key_bytes = g_bytes_new(key, key_size), *result = NULL;
    NativeStorageEntry *found = NULL;
    size_t used = 0;
    for (unsigned i = 0; entries && i < entries->len; i++) {
        NativeStorageEntry *entry = g_ptr_array_index(entries, i);
        used += g_bytes_get_size(entry->key) + g_bytes_get_size(entry->value);
        if (g_bytes_equal(entry->key, key_bytes)) {
            found = entry;
            index = i;
        }
    }

    switch (request->status) {
    case WEB_SERVICE_STORAGE_LENGTH: {
        uint32_t count = entries ? entries->len : 0;
        result = g_bytes_new(&count, sizeof(count));
        flags = WEB_SERVICE_FOUND;
        break;
    }
    case WEB_SERVICE_STORAGE_KEY: {
        uint32_t n;
        memcpy(&n, value, sizeof(n));
        if (entries && n < entries->len) {
            result = g_bytes_ref(((NativeStorageEntry *)g_ptr_array_index(entries, n))->key);
            flags = WEB_SERVICE_FOUND;
        }
        break;
    }
    case WEB_SERVICE_STORAGE_GET:
        if (found) {
            result = g_bytes_ref(found->value);
            flags = WEB_SERVICE_FOUND;
        }
        break;
    case WEB_SERVICE_STORAGE_SET: {
        size_t old = found ? g_bytes_get_size(found->key) + g_bytes_get_size(found->value) : 0;
        if (used - old + key_size + value_size > WEB_SERVICE_STORAGE_QUOTA ||
            native_session_bytes(view) - old + key_size + value_size > 32 * 1024 * 1024 ||
            (!entries && g_hash_table_size(view->session_storage) >= 128)) {
            error = ENOSPC;
            break;
        }

        if (!entries) {
            entries = g_ptr_array_new_with_free_func(native_storage_entry_free);
            g_hash_table_insert(view->session_storage, g_strdup(broker->origin), entries);
        }
        if (found) {
            g_bytes_unref(found->value);
            found->value = g_bytes_new(value, value_size);
        } else {
            NativeStorageEntry *entry = g_new0(NativeStorageEntry, 1);
            entry->key = g_bytes_ref(key_bytes);
            entry->value = g_bytes_new(value, value_size);
            g_ptr_array_add(entries, entry);
        }
        break;
    }
    case WEB_SERVICE_STORAGE_REMOVE:
        if (found)
            g_ptr_array_remove_index(entries, index);
        break;
    case WEB_SERVICE_STORAGE_CLEAR:
        if (entries)
            g_ptr_array_set_size(entries, 0);
        break;
    }
    gsize size = 0;
    const void *bytes = result ? g_bytes_get_data(result, &size) : NULL;
    native_service_reply(broker, request, url, error, flags, bytes, size);
    if (result)
        g_bytes_unref(result);
    g_bytes_unref(key_bytes);
    return 1;
}

static int native_service(Broker *broker, const struct web_resource_packet *request,
                          const char *metadata, int fd) {
    if (request->method_bytes || request->error_bytes || request->status < WEB_SERVICE_COOKIE_GET ||
        request->status > WEB_SERVICE_FRAME_TREE || request->flags & ~WEB_SERVICE_SESSION ||
        memchr(metadata, 0, request->url_bytes) || request->body_bytes > WEB_SERVICE_STORAGE_QUOTA)
        return 0;
    unsigned operation = request->status;
    if (operation >= WEB_SERVICE_AUDIO_INFO && operation <= WEB_SERVICE_AUDIO_RESET &&
        (request->flags || request->headers_bytes ||
         (operation == WEB_SERVICE_AUDIO_WRITE ? (!request->body_bytes || request->body_bytes % 4 ||
                                                  request->body_bytes > OS_AUDIO_MAX_FRAMES * 4)
                                               : request->body_bytes)))
        return 0;
    if ((operation <= WEB_SERVICE_COOKIE_SET &&
         (request->flags || request->headers_bytes || request->body_bytes > 4096)) ||
        (operation == WEB_SERVICE_STORAGE_KEY &&
         (request->headers_bytes || request->body_bytes != 4)) ||
        ((operation == WEB_SERVICE_STORAGE_LENGTH || operation == WEB_SERVICE_STORAGE_CLEAR) &&
         (request->headers_bytes || request->body_bytes)) ||
        (operation == WEB_SERVICE_HISTORY_REQUEST &&
         (request->flags || request->headers_bytes || request->body_bytes)) ||
        (operation == WEB_SERVICE_FRAME_TREE &&
         (request->flags || request->headers_bytes || request->body_bytes)) ||
        ((operation == WEB_SERVICE_STORAGE_GET || operation == WEB_SERVICE_STORAGE_REMOVE ||
          operation == WEB_SERVICE_COOKIE_GET) &&
         request->body_bytes))
        return 0;
    char *url = g_strndup(metadata, request->url_bytes), *origin = origin_for(url);
    if (!origin || strcmp(origin, broker->origin)) {
        native_service_reply(broker, request, url, EACCES, 0, NULL, 0);
        g_free(url);
        g_free(origin);
        return 1;
    }

    g_free(origin);
    const void *value = NULL;
    void *mapping = MAP_FAILED;
    if (fd >= 0) {
        mapping = mmap(NULL, request->body_bytes, PROT_READ, MAP_SHARED, fd, 0);
        if (mapping == MAP_FAILED) {
            native_service_reply(broker, request, url, ENOMEM, 0, NULL, 0);
            g_free(url);
            return 1;
        }
        value = mapping;
    }

    NativeBrowser *view = broker->native;
    if (operation == WEB_SERVICE_FRAME_TREE) {
        GBytes *tree = native_tree_export(broker->native);
        gsize size = 0;
        const void *bytes = tree ? g_bytes_get_data(tree, &size) : NULL;
        native_service_reply(broker, request, url, tree ? 0 : EAGAIN, tree ? WEB_SERVICE_FOUND : 0,
                             bytes, size);
        if (tree)
            g_bytes_unref(tree);
    } else if (operation >= WEB_SERVICE_AUDIO_INFO) {
        struct os_audio_info info = {0};
        unsigned command = operation == WEB_SERVICE_AUDIO_INFO    ? OS_AUDIO_INFO
                           : operation == WEB_SERVICE_AUDIO_WRITE ? OS_AUDIO_WRITE
                                                                  : OS_AUDIO_RESET;
        long result = syscall(MODERNOS_AUDIO, command, command == OS_AUDIO_INFO ? &info : value,
                              command == OS_AUDIO_WRITE ? request->body_bytes / 4 : 0);
        native_service_reply(broker, request, url, result < 0 ? errno : 0, 0,
                             !result && command == OS_AUDIO_INFO ? &info : NULL,
                             !result && command == OS_AUDIO_INFO ? sizeof(info) : 0);
    } else if (operation == WEB_SERVICE_HISTORY_REQUEST) {
        NativeHistory *entry = view->restoring && view->count ? &view->history[view->cursor] : NULL;
        if (entry && !strcmp(entry->url, url) && entry->metadata && entry->body && entry->method &&
            strcmp(entry->method, "GET")) {
            uint32_t lengths[2] = {strlen(entry->method),
                                   entry->content_type ? strlen(entry->content_type) : 0};
            GByteArray *record = g_byte_array_new();
            g_byte_array_append(record, (void *)lengths, sizeof(lengths));
            g_byte_array_append(record, (void *)entry->method, lengths[0]);
            if (lengths[1])
                g_byte_array_append(record, (void *)entry->content_type, lengths[1]);
            if (entry->request_body) {
                gsize size;
                const void *bytes = g_bytes_get_data(entry->request_body, &size);
                g_byte_array_append(record, bytes, size);
            }
            native_service_reply(broker, request, url, 0, WEB_SERVICE_FOUND, record->data,
                                 record->len);
            g_byte_array_unref(record);
        } else
            native_service_reply(broker, request, url, ENOENT, 0, NULL, 0);
    } else if (operation <= WEB_SERVICE_COOKIE_SET) {
        GUri *uri = g_uri_parse(url, G_URI_FLAGS_ENCODED, NULL);
        char *cookies = NULL;
        GUri *first_party = g_uri_parse(native_first_party(broker), G_URI_FLAGS_ENCODED, NULL);
        GUri *cookie_site = native_cookie_site(broker);
        if (operation == WEB_SERVICE_COOKIE_GET) {
            GSList *visible = soup_cookie_jar_get_cookie_list_with_same_site_info(
                view->cookies, uri, first_party, cookie_site, FALSE, TRUE, FALSE);
            if (visible)
                cookies = soup_cookies_to_cookie_header(visible);
            soup_cookies_free(visible);
        } else if (!request->body_bytes || !memchr(value, 0, request->body_bytes)) {
            char *text = g_strndup(value ? value : "", request->body_bytes);
            SoupCookie *cookie = soup_cookie_parse(text, uri);
            g_free(text);
            if (cookie) {
                int allowed = !soup_cookie_get_http_only(cookie) &&
                              !g_str_has_prefix(soup_cookie_get_name(cookie), "__Http-") &&
                              !g_str_has_prefix(soup_cookie_get_name(cookie), "__Host-Http-");
                GSList *existing = soup_cookie_jar_all_cookies(view->cookies);
                for (GSList *item = existing; allowed && item; item = item->next) {
                    SoupCookie *old = item->data;
                    if (soup_cookie_get_http_only(old) &&
                        !strcmp(soup_cookie_get_name(old), soup_cookie_get_name(cookie)) &&
                        !strcmp(soup_cookie_get_domain(old), soup_cookie_get_domain(cookie)) &&
                        !strcmp(soup_cookie_get_path(old), soup_cookie_get_path(cookie)))
                        allowed = 0;
                }

                soup_cookies_free(existing);
                if (allowed && first_party)
                    soup_cookie_jar_add_cookie_full(view->cookies, cookie, uri, first_party);
                else
                    soup_cookie_free(cookie);
            }
        }

        native_service_reply(broker, request, url, 0, cookies ? WEB_SERVICE_FOUND : 0, cookies,
                             cookies ? strlen(cookies) : 0);
        if (cookie_site)
            g_uri_unref(cookie_site);
        if (first_party)
            g_uri_unref(first_party);
        g_free(cookies);
        g_uri_unref(uri);
    } else {
        const void *key = metadata + request->url_bytes;
        if (request->headers_bytes % 2 ||
            (operation == WEB_SERVICE_STORAGE_SET && request->body_bytes % 2)) {
            if (mapping != MAP_FAILED)
                munmap(mapping, request->body_bytes);
            g_free(url);
            return 0;
        }

        if (request->flags & WEB_SERVICE_SESSION)
            native_session_storage(view, request, url, key, request->headers_bytes, value,
                                   request->body_bytes);
        else
            native_local_storage(view, request, url, key, request->headers_bytes, value,
                                 request->body_bytes);
    }

    if (mapping != MAP_FAILED)
        munmap(mapping, request->body_bytes);
    g_free(url);
    return 1;
}
