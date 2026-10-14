/* SVG path plotter for NetSurf's native framebuffer frontend.
 * Coordinates stay in double precision through affine transforms and curve
 * subdivision. Filling all contours together preserves non-zero winding and
 * holes; an open contour is closed for fill, but not for stroke. */
#define MODERNOS_PATH_EDGES 32768

struct modernos_point {
    double x, y;
};

struct modernos_edge {
    struct modernos_point a, b;
    bool stroke;
};

struct modernos_path {
    struct modernos_edge *edges;
    size_t count, capacity;
};

struct modernos_crossing {
    double x;
    int winding;
};

static bool modernos_edge_add(struct modernos_path *path, struct modernos_point a,
                              struct modernos_point b, bool stroke) {
    if (a.x == b.x && a.y == b.y)
        return true;
    if (path->count == MODERNOS_PATH_EDGES)
        return false;
    if (path->count == path->capacity) {
        size_t capacity = path->capacity ? path->capacity * 2 : 64;
        void *memory = realloc(path->edges, capacity * sizeof(*path->edges));
        if (!memory)
            return false;
        path->edges = memory;
        path->capacity = capacity;
    }

    path->edges[path->count++] = (struct modernos_edge){a, b, stroke};
    return true;
}

static struct modernos_point modernos_mid(struct modernos_point a, struct modernos_point b) {
    return (struct modernos_point){(a.x + b.x) * 0.5, (a.y + b.y) * 0.5};
}

static bool modernos_curve(struct modernos_path *path, struct modernos_point a,
                           struct modernos_point b, struct modernos_point c,
                           struct modernos_point d, unsigned depth) {
    /* Distance from controls to the line segment, including collinear curves
       that double back. Bounding subdivision also handles coincident ends. */
    double dx = d.x - a.x, dy = d.y - a.y, length = dx * dx + dy * dy;
    bool flat = true;
    struct modernos_point control[2] = {b, c};
    for (unsigned i = 0; i < 2; i++) {
        double t = length ? ((control[i].x - a.x) * dx + (control[i].y - a.y) * dy) / length : 0;
        if (t < 0)
            t = 0;
        if (t > 1)
            t = 1;
        double x = control[i].x - a.x - t * dx, y = control[i].y - a.y - t * dy;
        if (x * x + y * y > 0.0625)
            flat = false;
    }

    if (flat || depth == 12)
        return modernos_edge_add(path, a, d, true);
    struct modernos_point ab = modernos_mid(a, b), bc = modernos_mid(b, c), cd = modernos_mid(c, d);
    struct modernos_point abc = modernos_mid(ab, bc), bcd = modernos_mid(bc, cd),
                          mid = modernos_mid(abc, bcd);
    return modernos_curve(path, a, ab, abc, mid, depth + 1) &&
           modernos_curve(path, mid, bcd, cd, d, depth + 1);
}

static bool modernos_transform(const float *p, const float t[6], struct modernos_point *out) {
    out->x = (double)t[0] * p[0] + (double)t[2] * p[1] + t[4];
    out->y = (double)t[1] * p[0] + (double)t[3] * p[1] + t[5];
    return isfinite(out->x) && isfinite(out->y) && fabs(out->x) <= 1000000 &&
           fabs(out->y) <= 1000000;
}

static int modernos_crossing_compare(const void *a, const void *b) {
    double x = ((const struct modernos_crossing *)a)->x,
           y = ((const struct modernos_crossing *)b)->x;
    return (x > y) - (x < y);
}

static nserror modernos_plot_path(const struct redraw_context *ctx, const plot_style_t *style,
                                  const float *p, unsigned n, const float t[6]) {
    (void)ctx;
    if (!n)
        return NSERROR_OK;
    if (!p || !t || n > MODERNOS_PATH_EDGES * 7)
        return NSERROR_INVALID;
    for (unsigned i = 0; i < 6; i++)
        if (!isfinite(t[i]))
            return NSERROR_INVALID;
    struct modernos_path path = {0};
    struct modernos_point start = {0}, at = {0};
    bool contour = false;
    nserror error = NSERROR_INVALID;
    for (unsigned i = 0; i < n;) {
        float command = p[i++];
        unsigned coordinates = command == PLOTTER_PATH_BEZIER                                   ? 6
                               : (command == PLOTTER_PATH_MOVE || command == PLOTTER_PATH_LINE) ? 2
                                                                                                : 0;
        if (coordinates > n - i || (!contour && command != PLOTTER_PATH_MOVE))
            goto finish;
        struct modernos_point point[3];
        for (unsigned j = 0; j < coordinates / 2; j++)
            if (!modernos_transform(p + i + j * 2, t, &point[j]))
                goto finish;
        i += coordinates;
        if (command == PLOTTER_PATH_MOVE) {
            if (contour && !modernos_edge_add(&path, at, start, false))
                goto finish;
            start = at = point[0];
            contour = true;
        } else if (command == PLOTTER_PATH_LINE) {
            if (!modernos_edge_add(&path, at, point[0], true))
                goto finish;
            at = point[0];
        } else if (command == PLOTTER_PATH_BEZIER) {
            if (!modernos_curve(&path, at, point[0], point[1], point[2], 0))
                goto finish;
            at = point[2];
        } else if (command == PLOTTER_PATH_CLOSE) {
            if (!modernos_edge_add(&path, at, start, true))
                goto finish;
            at = start;
        } else
            goto finish;
    }

    if (contour && !modernos_edge_add(&path, at, start, false))
        goto finish;
    nsfb_bbox_t clip;
    if (!nsfb_plot_get_clip(nsfb, &clip))
        goto finish;
    if ((uint64_t)path.count * (unsigned)(clip.y1 - clip.y0) > 64000000)
        goto finish;
    if (style->fill_colour != NS_TRANSPARENT && path.count) {
        struct modernos_crossing *crossings = malloc(path.count * sizeof(*crossings));
        if (!crossings) {
            error = NSERROR_NOMEM;
            goto finish;
        }
        for (int y = clip.y0; y < clip.y1; y++) {
            size_t count = 0;
            double scan = y + 0.5;
            for (size_t i = 0; i < path.count; i++) {
                struct modernos_edge *e = &path.edges[i];
                if ((e->a.y <= scan && e->b.y > scan) || (e->b.y <= scan && e->a.y > scan))
                    crossings[count++] = (struct modernos_crossing){
                        e->a.x + (scan - e->a.y) * (e->b.x - e->a.x) / (e->b.y - e->a.y),
                        e->b.y > e->a.y ? 1 : -1};
            }

            qsort(crossings, count, sizeof(*crossings), modernos_crossing_compare);
            int winding = 0;
            double previous = 0;
            for (size_t i = 0; i < count; i++) {
                double x = crossings[i].x;
                if (winding && x > previous) {
                    int left = (int)ceil(previous - 0.5), right = (int)ceil(x - 0.5);
                    if (left < clip.x0)
                        left = clip.x0;
                    if (right > clip.x1)
                        right = clip.x1;
                    if (right > left) {
                        nsfb_bbox_t span = {left, y, right, y + 1};
                        nsfb_plot_rectangle_fill(nsfb, &span, style->fill_colour);
                    }
                }

                winding += crossings[i].winding;
                previous = x;
            }
        }

        free(crossings);
    }

    if (style->stroke_colour != NS_TRANSPARENT && style->stroke_width > 0) {
        double scale = (hypot(t[0], t[1]) + hypot(t[2], t[3])) * 0.5;
        double width = plot_style_fixed_to_double(style->stroke_width) * scale;
        if (!isfinite(width) || width > 4096)
            goto finish;
        width = fmax(1, width);
        for (size_t i = 0; i < path.count; i++)
            if (path.edges[i].stroke) {
                struct modernos_edge *e = &path.edges[i];
                double dx = e->b.x - e->a.x, dy = e->b.y - e->a.y, length = hypot(dx, dy);
                if (!length)
                    continue;
                double nx = -dy * width / (2 * length), ny = dx * width / (2 * length);
                double ax = nx, ay = ny, bx = nx, by = ny;
                /* Intersect adjacent stroke edges for a miter join, with SVG's
               default limit of four stroke half-widths. */
                for (unsigned side = 0; side < 2; side++) {
                    size_t neighbor = side ? i + 1 : i ? i - 1 : path.count;
                    if (neighbor >= path.count || !path.edges[neighbor].stroke)
                        continue;
                    struct modernos_edge *other = &path.edges[neighbor];
                    struct modernos_point end = side ? e->b : e->a,
                                          join = side ? other->a : other->b;
                    if (end.x != join.x || end.y != join.y)
                        continue;
                    double ox = other->b.x - other->a.x, oy = other->b.y - other->a.y,
                           ol = hypot(ox, oy);
                    if (!ol)
                        continue;
                    double mx = nx - oy * width / (2 * ol), my = ny + ox * width / (2 * ol);
                    double den = nx * mx + ny * my;
                    if (den <= 0.000001)
                        continue;
                    mx *= width * width / (4 * den);
                    my *= width * width / (4 * den);
                    if (hypot(mx, my) > 2 * width)
                        continue;
                    if (side) {
                        bx = mx;
                        by = my;
                    } else {
                        ax = mx;
                        ay = my;
                    }
                }

                int polygon[8] = {(int)round(e->a.x + ax), (int)round(e->a.y + ay),
                                  (int)round(e->b.x + bx), (int)round(e->b.y + by),
                                  (int)round(e->b.x - bx), (int)round(e->b.y - by),
                                  (int)round(e->a.x - ax), (int)round(e->a.y - ay)};
                nsfb_plot_polygon(nsfb, polygon, 4, style->stroke_colour);
            }
    }

    error = NSERROR_OK;
finish:
    free(path.edges);
    return error;
}
