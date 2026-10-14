/* Check the requesting document before looking in either content cache.
 * A local document remains usable, but a network document cannot obtain local
 * files that the desktop has granted to this browser process. */
bool fetch_can_access(const nsurl *url, const nsurl *referer) {
    if (referer == NULL)
        return true; /* desktop navigation */
    lwc_string *scheme = nsurl_get_component(url, NSURL_SCHEME);
    bool file = scheme && strcmp(lwc_string_data(scheme), "file") == 0;
    if (scheme)
        lwc_string_unref(scheme);
    if (!file)
        return true;
    scheme = nsurl_get_component(referer, NSURL_SCHEME);
    bool local = scheme && (strcmp(lwc_string_data(scheme), "file") == 0 ||
                            strcmp(lwc_string_data(scheme), "resource") == 0 ||
                            strcmp(lwc_string_data(scheme), "about") == 0);
    if (scheme)
        lwc_string_unref(scheme);
    if (local)
        return true;
    /* The resource: loader redirects the engine's built-in CSS to its
     * installed files while retaining the document's remote referrer. */
    static const char *const resources[] = {"default.css", "quirks.css", "internal.css", "user.css",
                                            "adblock.css"};
    for (unsigned i = 0; i < sizeof(resources) / sizeof(resources[0]); i++) {
        nsurl *resource = guit->fetch->get_resource_url(resources[i]);
        if (!resource)
            continue;
        bool match = nsurl_compare(url, resource, NSURL_COMPLETE);
        nsurl_unref(resource);
        if (match)
            return true;
    }

    return false;
}
