/* Paint sibling positioned boxes in stacking order. */
static bool html_redraw_box_children(const html_content *html, struct box *box, int x_parent,
                                     int y_parent, const struct rect *clip, float scale,
                                     colour background, const struct redraw_context *ctx) {
    size_t count;
    struct modernos_paint_box *items;
    if (!modernos_order_children(box, &items, &count))
        return false;
    bool result = true;
    for (size_t i = 0; i < count; i++)
        if (!html_redraw_box(html, items[i].box,
                             x_parent + box->x - scrollbar_get_offset(box->scroll_x),
                             y_parent + box->y - scrollbar_get_offset(box->scroll_y), clip, scale,
                             background, ctx)) {
            result = false;
            break;
        }
    free(items);
    return result;
}
