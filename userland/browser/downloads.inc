/* Actual bounded, exclusive downloads. Failed transfers remove their partial
 * file; completion is announced only after flush, close and no-replace rename.
 * The desktop presents notices and remains the sole window manager. */
struct gui_download_window {
    struct gui_download_window *next;
    download_context *context;
    int fd;
    char temporary[512], destination[512];
};

static unsigned download_sequence;
static struct gui_download_window *active_downloads;

static void modernos_download_remove(struct gui_download_window *download) {
    struct gui_download_window **at = &active_downloads;
    while (*at && *at != download) {
        at = &(*at)->next;
    }
    if (*at) {
        *at = download->next;
    }
}

static void modernos_download_cancel_all(void) {
    while (active_downloads) {
        struct gui_download_window *download = active_downloads;
        active_downloads = download->next;
        download_context_abort(download->context);
        close(download->fd);
        unlink(download->temporary);
        download_context_destroy(download->context);
        free(download);
    }
}

static struct gui_download_window *modernos_download_create(download_context *context,
                                                            struct gui_window *parent) {
    (void)parent;
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') {
        modernos_browser_text(BROWSER_NOTICE,
                              "Cannot download: HOME must be an absolute writable directory.");
        return NULL;
    }
    struct gui_download_window *download = calloc(1, sizeof(*download));
    if (!download) {
        return NULL;
    }
    char directory[512], name[129];
    const char *suggested = download_context_get_filename(context);
    if (!suggested || !suggested[0]) {
        suggested = "download";
    }
    unsigned used = 0;
    for (; *suggested && used < sizeof(name) - 1; suggested++) {
        unsigned char character = *suggested;
        name[used++] =
            ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
             (character >= '0' && character <= '9') || character == '-' || character == '_' ||
             character == '.')
                ? character
                : '_';
    }

    name[used] = 0;
    if (!used || !strcmp(name, ".") || !strcmp(name, "..")) {
        strcpy(name, "download");
    }
    int count = snprintf(directory, sizeof(directory), "%s/Downloads", home);
    if (count < 0 || count >= (int)sizeof(directory) ||
        (mkdir(directory, 0755) && errno != EEXIST)) {
        goto failure;
    }
    count =
        snprintf(download->destination, sizeof(download->destination), "%s/%s", directory, name);
    if (count < 0 || count >= (int)sizeof(download->destination)) {
        errno = ENAMETOOLONG;
        goto failure;
    }
    count = snprintf(download->temporary, sizeof(download->temporary), "%s/.download-%lu-%u.part",
                     directory, (unsigned long)getpid(), ++download_sequence);
    if (count < 0 || count >= (int)sizeof(download->temporary)) {
        errno = ENAMETOOLONG;
        goto failure;
    }
    download->fd = open(download->temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (download->fd < 0) {
        goto failure;
    }
    download->context = context;
    download->next = active_downloads;
    active_downloads = download;
    return download;
failure:
    modernos_browser_text(BROWSER_NOTICE, strerror(errno));
    free(download);
    return NULL;
}

static nserror modernos_download_data(struct gui_download_window *download, const char *data,
                                      unsigned size) {
    unsigned written = 0;
    while (written < size) {
        ssize_t count = write(download->fd, data + written, size - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return NSERROR_SAVE_FAILED;
        }
        written += count;
    }

    return NSERROR_OK;
}

static void modernos_download_error(struct gui_download_window *download, const char *message) {
    close(download->fd);
    unlink(download->temporary);
    modernos_browser_text(BROWSER_NOTICE, message ? message : "Download failed.");
    modernos_download_remove(download);
    download_context_destroy(download->context);
    free(download);
}

static void modernos_download_done(struct gui_download_window *download) {
    int error = fsync(download->fd) ? errno : 0;
    if (close(download->fd) && !error) {
        error = errno;
    }
    if (!error &&
        syscall(SYS_renameat2, AT_FDCWD, download->temporary, AT_FDCWD, download->destination, 1)) {
        error = errno;
    }
    char notice[640];
    if (error) {
        unlink(download->temporary);
        snprintf(notice, sizeof(notice),
                 "Download could not be saved: %s. Choose a different filename or move the "
                 "existing file.",
                 strerror(error));
    } else {
        snprintf(notice, sizeof(notice), "Download saved to %s", download->destination);
    }
    modernos_browser_text(error ? BROWSER_NOTICE : BROWSER_STATUS, notice);
    modernos_download_remove(download);
    download_context_destroy(download->context);
    free(download);
}

static struct gui_download_table modernos_download_table = {.create = modernos_download_create,
                                                            .data = modernos_download_data,
                                                            .error = modernos_download_error,
                                                            .done = modernos_download_done};
