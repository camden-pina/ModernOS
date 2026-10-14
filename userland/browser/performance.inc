/* A per-document clock backed by the OS monotonic timer. These are the basic
 * High Resolution Time operations; resource/navigation timing is not exposed.
 * Millisecond precision also avoids exposing a fine-grained timing channel. */
static duk_ret_t modernos_performance_now(duk_context *ctx) {
    duk_push_this(ctx);
    duk_push_current_function(ctx);
    duk_get_prop_string(ctx, -1, MAGIC(PerformanceOwner));
    if (!duk_strict_equals(ctx, -3, -1))
        return duk_error(ctx, DUK_ERR_TYPE_ERROR,
                         "performance.now requires its Performance receiver");
    duk_get_prop_string(ctx, -2, MAGIC(PerformanceOrigin));
    double origin = duk_get_number(ctx, -1);
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return duk_error(ctx, DUK_ERR_ERROR, "Monotonic clock unavailable");
    double elapsed = (double)now.tv_sec * 1000 + (double)(now.tv_nsec / 1000000) - origin;
    duk_push_number(ctx, elapsed < 0 ? 0 : elapsed);
    return 1;
}

static void modernos_performance_install(duk_context *ctx, duk_idx_t window) {
    window = duk_normalize_index(ctx, window);
    struct timespec mono, wall;
    if (clock_gettime(CLOCK_MONOTONIC, &mono) || clock_gettime(CLOCK_REALTIME, &wall)) {
        duk_error(ctx, DUK_ERR_ERROR, "Document clock unavailable");
        return;
    }
    duk_idx_t performance = duk_push_object(ctx);
    duk_push_string(ctx, "timeOrigin");
    duk_push_number(ctx, (double)wall.tv_sec * 1000 + (double)(wall.tv_nsec / 1000000));
    duk_def_prop(ctx, performance,
                 DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_HAVE_ENUMERABLE |
                     DUK_DEFPROP_ENUMERABLE | DUK_DEFPROP_HAVE_CONFIGURABLE |
                     DUK_DEFPROP_CONFIGURABLE);
    duk_push_c_function(ctx, modernos_performance_now, 0);
    duk_dup(ctx, performance);
    duk_put_prop_string(ctx, -2, MAGIC(PerformanceOwner));
    duk_push_number(ctx, (double)mono.tv_sec * 1000 + (double)(mono.tv_nsec / 1000000));
    duk_put_prop_string(ctx, -2, MAGIC(PerformanceOrigin));
    duk_put_prop_string(ctx, performance, "now");
    duk_push_string(ctx, "performance");
    duk_insert(ctx, -2);
    duk_def_prop(ctx, window,
                 DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_HAVE_ENUMERABLE |
                     DUK_DEFPROP_ENUMERABLE | DUK_DEFPROP_HAVE_CONFIGURABLE |
                     DUK_DEFPROP_CONFIGURABLE);
    const char *const aliases[] = {"globalThis", "self"};
    for (unsigned i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        duk_push_string(ctx, aliases[i]);
        duk_dup(ctx, window);
        duk_def_prop(ctx, window,
                     DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_WRITABLE |
                         DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_ENUMERABLE |
                         DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE);
    }
}
