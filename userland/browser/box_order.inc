/* Shared by painting and pointer hit testing. Equal stacking levels retain
 * document order; float placeholders are represented by float_children. */
struct modernos_paint_box {
    struct box *box;
    int group, z;
    unsigned order;
};

static struct modernos_paint_box modernos_paint_order(struct box *box, unsigned order,
                                                      bool floating) {
    struct modernos_paint_box item = {.box = box, .group = floating ? 2 : 1, .order = order};
    if (box->style && (css_computed_position(box->style) != CSS_POSITION_STATIC ||
                       (box->parent &&
                        (box->parent->type == BOX_FLEX || box->parent->type == BOX_INLINE_FLEX)))) {
        int32_t z = 0;
        if (css_computed_z_index(box->style, &z) == CSS_Z_INDEX_SET)
            item.z = z;
        item.group = item.z < 0 ? 0 : item.z > 0 ? 4 : 3;
    }

    return item;
}

static int modernos_paint_compare(const void *a, const void *b) {
    const struct modernos_paint_box *x = a, *y = b;
    if (x->group != y->group)
        return (x->group > y->group) - (x->group < y->group);
    if (x->z != y->z)
        return (x->z > y->z) - (x->z < y->z);
    return (x->order > y->order) - (x->order < y->order);
}

static bool modernos_order_children(struct box *box, struct modernos_paint_box **result,
                                    size_t *count) {
    *count = 0;
    *result = NULL;
    for (struct box *c = box->children; c; c = c->next)
        if (c->type != BOX_FLOAT_LEFT && c->type != BOX_FLOAT_RIGHT)
            (*count)++;
    for (struct box *c = box->float_children; c; c = c->next_float)
        (*count)++;
    if (!*count)
        return true;
    if (*count > 65536)
        return false;
    struct modernos_paint_box *items = malloc(*count * sizeof(*items));
    if (!items)
        return false;
    unsigned used = 0;
    for (struct box *c = box->children; c; c = c->next)
        if (c->type != BOX_FLOAT_LEFT && c->type != BOX_FLOAT_RIGHT) {
            items[used] = modernos_paint_order(c, used, false);
            used++;
        }
    for (struct box *c = box->float_children; c; c = c->next_float) {
        items[used] = modernos_paint_order(c, used, true);
        used++;
    }
    qsort(items, *count, sizeof(*items), modernos_paint_compare);
    *result = items;
    return true;
}

static void modernos_fixed_offset(const html_content *html, const struct box *box, int *sx,
                                  int *sy) {
    *sx = *sy = 0;
    if (!html->bw)
        return;
    for (const struct box *p = box; p; p = p->parent) {
        if (p->style && css_computed_position(p->style) == CSS_POSITION_FIXED) {
            if (html->bw->window)
                guit->window->get_scroll(html->bw->window, sx, sy);
            else {
                *sx = scrollbar_get_offset(html->bw->scroll_x);
                *sy = scrollbar_get_offset(html->bw->scroll_y);
            }
            return;
        }
    }
}
