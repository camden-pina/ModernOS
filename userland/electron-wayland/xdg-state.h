/* Single-toplevel diagnostic state, using generated upstream xdg-shell v1 ABI. */
struct window_geometry { int32_t x,y,w,h; };
struct xdg_state {
    struct wl_resource *surface,*toplevel,*owner;
    uint32_t pending[8],acked_serial;
    unsigned pending_count,configures,acks,closes,pongs,bindings;
    int initial,configured,mapped,geometry_set,geometry_dirty,hints_dirty;
    int32_t min[2],max[2],pending_min[2],pending_max[2];
    struct window_geometry geometry,pending_geometry;
    char title[256],app_id[256];
};
