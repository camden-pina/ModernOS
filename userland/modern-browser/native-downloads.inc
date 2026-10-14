/* Stream downloads to private files; publish only complete transfers with a
 * no-replace rename. Renderers receive neither paths nor file descriptors. */
static int native_begin_download(Request *request) {
    unsigned status = soup_message_get_status(request->message);
    if (!(request->packet.flags & WEB_RESOURCE_MAIN) || status < 200 || status >= 300)
        return 0;
    SoupMessageHeaders *headers = soup_message_get_response_headers(request->message);
    char *disposition = NULL;
    GHashTable *parameters = NULL;
    soup_message_headers_get_content_disposition(headers, &disposition, &parameters);
    const char *mime = soup_message_headers_get_content_type(headers, NULL);
    int attachment = disposition && !g_ascii_strcasecmp(disposition, "attachment");
    int document = !mime || g_str_has_prefix(mime, "text/") || g_str_has_prefix(mime, "image/") ||
                   !strcmp(mime, "application/xhtml+xml") || !strcmp(mime, "application/xml") ||
                   !strcmp(mime, "application/json");
    if (!attachment && document) {
        g_free(disposition);
        if (parameters)
            g_hash_table_unref(parameters);
        return 0;
    }

    GUri *uri = g_uri_parse(request->url, G_URI_FLAGS_ENCODED, NULL);
    char *base = g_path_get_basename(g_uri_get_path(uri)),
         *decoded = g_uri_unescape_string(base, NULL);
    const char *suggested = parameters ? g_hash_table_lookup(parameters, "filename") : NULL;
    if (!suggested || !*suggested)
        suggested = decoded ? decoded : base;
    char name[129];
    unsigned used = 0;
    for (; *suggested && used < sizeof(name) - 1; suggested++) {
        unsigned char c = *suggested;
        name[used++] = (g_ascii_isalnum(c) || c == '-' || c == '_' || c == '.') ? c : '_';
    }
    name[used] = 0;
    if (!used || !strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, "/"))
        strcpy(name, "download");
    const char *home = getenv("HOME");
    if (!home || *home != '/')
        home = "/home/user";
    char *directory = g_strdup_printf("%s/Downloads", home);
    int result = 0;
    if (mkdir(directory, 0755) < 0 && errno != EEXIST)
        result = -1;
    request->download_destination = g_strdup_printf("%s/%s", directory, name);
    request->download_temporary =
        g_strdup_printf("%s/.download-%d-%u.part", directory, getpid(), request->packet.id);
    if (!result)
        request->download_fd =
            open(request->download_temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (request->download_fd < 0)
        result = -1;
    g_free(disposition);
    if (parameters)
        g_hash_table_unref(parameters);
    g_uri_unref(uri);
    g_free(base);
    g_free(decoded);
    g_free(directory);
    return result ? result : 1;
}

static int native_write_download(Request *request, const void *data, size_t size) {
    if (size > 256 * 1024 * 1024 - request->download_bytes)
        return 0;
    const char *bytes = data;
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(request->download_fd, bytes + written, size - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return 0;
        written += n;
    }

    request->download_bytes += size;
    return 1;
}

static int native_finish_download(Request *request) {
    int error = fsync(request->download_fd) < 0;
    if (close(request->download_fd) < 0)
        error = 1;
    request->download_fd = -1;
    if (!error && syscall(SYS_renameat2, AT_FDCWD, request->download_temporary, AT_FDCWD,
                          request->download_destination, 1) < 0)
        error = 1;
    if (error)
        return 0;
    request->download_saved = 1;
    char *notice = g_strdup_printf("Download saved to %s", request->download_destination);
    native_queue_text(request->broker->native, BROWSER_STATUS, notice);
    g_free(notice);
    return 1;
}
