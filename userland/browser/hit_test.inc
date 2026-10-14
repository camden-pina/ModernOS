static void modernos_box_position(const html_content *html, struct box *box, int *bx, int *by) {
    box_coords(box, bx, by);
    int sx, sy;
    modernos_fixed_offset(html, box, &sx, &sy);
    *bx += sx;
    *by += sy;
}

/* Follow the same order as painting, from the foremost child back. The
 * returned box and its ancestors are the single path that receives input. */
static struct box *modernos_pick_box(const html_content *html, struct box *box, int x, int y,
                                     unsigned depth, unsigned *budget) {
    if (depth > 512 || !*budget)
        return NULL;
    (*budget)--;
    int bx, by;
    modernos_box_position(html, box, &bx, &by);
    bool physical = false;
    bool contains = box_contains_point(&html->unit_len_ctx, box, x - bx, y - by, &physical);
    if (box->style && box->parent) {
        /* Match the padding/overflow clipping used by the painter. Visible
         * overflow is traversed even outside the parent's physical bounds. */
        if (css_computed_overflow_x(box->style) != CSS_OVERFLOW_VISIBLE &&
            (x < bx || x >= bx + box->padding[LEFT] + box->width + box->padding[RIGHT]))
            return NULL;
        if (css_computed_overflow_y(box->style) != CSS_OVERFLOW_VISIBLE &&
            (y < by || y >= by + box->padding[TOP] + box->height + box->padding[BOTTOM]))
            return NULL;
        css_computed_clip_rect rect;
        if (!contains && css_computed_position(box->style) == CSS_POSITION_ABSOLUTE &&
            css_computed_clip(box->style, &rect) == CSS_CLIP_RECT)
            return NULL;
    }

    size_t count;
    struct modernos_paint_box *items;
    if (!modernos_order_children(box, &items, &count))
        return NULL;
    struct box *picked = NULL;
    for (size_t i = count; i > 0 && !picked; i--)
        picked = modernos_pick_box(html, items[i - 1].box, x, y, depth + 1, budget);
    free(items);
    if (picked)
        return picked;
    return contains && physical && box_visible(box) ? box : NULL;
}

struct box *box_at_point(const html_content *html, struct box *box, const int x, const int y,
                         int *box_x, int *box_y) {
    unsigned budget = 65536;
    struct box *picked = modernos_pick_box(html, html->layout, x, y, 0, &budget);
    if (!picked || picked == box)
        return NULL;
    /* Iterate only through ancestors of the visible target. This prevents a
     * covered link or control from retaining the previous branch's action. */
    while (picked && picked->parent != box)
        picked = picked->parent;
    if (!picked)
        return NULL;
    modernos_box_position(html, picked, box_x, box_y);
    *box_x -= scrollbar_get_offset(picked->scroll_x);
    *box_y -= scrollbar_get_offset(picked->scroll_y);
    return picked;
}
