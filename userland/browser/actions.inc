/* Compiled into the upstream framebuffer frontend, which owns its browser
 * windows/navigation. Window decorations remain exclusively in ModernOS. */
void modernos_browser_action(unsigned action, const char *text) {
    struct gui_window *gw = input_window ? input_window : window_list;
    if (!gw) {
        return;
    }
    if (action == BROWSER_BACK) {
        if (browser_window_back_available(gw->bw)) {
            browser_window_history_back(gw->bw, false);
        }
    } else if (action == BROWSER_FORWARD) {
        if (browser_window_forward_available(gw->bw)) {
            browser_window_history_forward(gw->bw, false);
        }
    } else if (action == BROWSER_RELOAD) {
        browser_window_reload(gw->bw, true);
    } else if (action == BROWSER_STOP) {
        browser_window_stop(gw->bw);
    } else if (action == BROWSER_NAVIGATE) {
        nsurl *url;
        nserror error = nsurl_create(text, &url);
        if (error == NSERROR_OK) {
            error =
                browser_window_navigate(gw->bw, url, NULL, BW_NAVIGATE_HISTORY, NULL, NULL, NULL);
            nsurl_unref(url);
        }
        if (error != NSERROR_OK) {
            modernos_browser_text(BROWSER_NOTICE, messages_get_errorcode(error));
        }
    }
}
