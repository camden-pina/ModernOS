#include "desktop.h"

/* Small, locally defined outline paths. Cached 2x2 coverage masks have the
   same stroke and proportions at every scale and require no external assets. */
typedef struct {
    Icon kind;
    int size;
    bool used;
    uint8_t mask[40 * 40];
} IconCache;

static IconCache cache[160];
static int replacement;
static uint8_t *mask;
static int dim;

static void segment(float x1, float y1, float x2, float y2) {
    float dx = x2 - x1, dy = y2 - y1, length = dx * dx + dy * dy;
    float radius = .8f * desk.scale / 100 * 24 / dim;
    for (int y = 0; y < dim; y++) {
        for (int x = 0; x < dim; x++) {
            unsigned coverage = 0;
            for (int sy = 0; sy < 2; sy++) {
                for (int sx = 0; sx < 2; sx++) {
                    float px = (x + (sx + .5f) / 2) * 24 / dim,
                          py = (y + (sy + .5f) / 2) * 24 / dim;
                    float t = length ? ((px - x1) * dx + (py - y1) * dy) / length : 0;
                    if (t < 0) {
                        t = 0;
                    }
                    if (t > 1) {
                        t = 1;
                    }
                    float ax = px - x1 - t * dx, ay = py - y1 - t * dy;
                    coverage += ax * ax + ay * ay <= radius * radius;
                }
            }

            unsigned a = coverage * 255 / 4;
            if (a > mask[y * 40 + x]) {
                mask[y * 40 + x] = a;
            }
        }
    }
}

static void box(float x, float y, float w, float h) {
    segment(x, y, x + w, y);
    segment(x + w, y, x + w, y + h);
    segment(x + w, y + h, x, y + h);
    segment(x, y + h, x, y);
}

static void circle(float x, float y, float r) {
    /* A 16-segment circle avoids a libm dependency. */
    static const float a[17][2] = {{1, 0},  {.924f, .383f},   {.707f, .707f},   {.383f, .924f},
                                   {0, 1},  {-.383f, .924f},  {-.707f, .707f},  {-.924f, .383f},
                                   {-1, 0}, {-.924f, -.383f}, {-.707f, -.707f}, {-.383f, -.924f},
                                   {0, -1}, {.383f, -.924f},  {.707f, -.707f},  {.924f, -.383f},
                                   {1, 0}};
    for (int i = 0; i < 16; i++) {
        segment(x + a[i][0] * r, y + a[i][1] * r, x + a[i + 1][0] * r, y + a[i + 1][1] * r);
    }
}

static void path(Icon k) {
    switch (k) {
    case I_FOLDER:
        segment(3, 6, 9, 6);
        segment(9, 6, 11, 8);
        segment(11, 8, 21, 8);
        segment(21, 8, 21, 19);
        segment(21, 19, 3, 19);
        segment(3, 19, 3, 6);
        break;

    case I_HOME:
        segment(3, 11, 12, 4);
        segment(12, 4, 21, 11);
        segment(5, 10, 5, 20);
        segment(5, 20, 19, 20);
        segment(19, 20, 19, 10);
        box(10, 14, 4, 6);
        break;

    case I_CLOCK:
        circle(12, 12, 8);
        segment(12, 7, 12, 12);
        segment(12, 12, 16, 14);
        break;

    case I_DOWNLOAD:
        segment(12, 3, 12, 15);
        segment(8, 11, 12, 15);
        segment(12, 15, 16, 11);
        segment(4, 16, 4, 20);
        segment(4, 20, 20, 20);
        segment(20, 20, 20, 16);
        break;

    case I_IMAGE:
        box(3, 4, 18, 16);
        circle(8, 9, 1.5f);
        segment(4, 18, 10, 12);
        segment(10, 12, 14, 16);
        segment(14, 16, 17, 13);
        segment(17, 13, 21, 17);
        break;

    case I_TRASH:
        segment(3, 6, 21, 6);
        segment(6, 6, 7, 20);
        segment(7, 20, 17, 20);
        segment(17, 20, 18, 6);
        box(9, 3, 6, 3);
        segment(10, 10, 10, 16);
        segment(14, 10, 14, 16);
        break;

    case I_DRIVE:
        box(3, 10, 18, 10);
        segment(3, 10, 6, 4);
        segment(6, 4, 18, 4);
        segment(18, 4, 21, 10);
        segment(6, 16, 6, 16);
        segment(10, 16, 10, 16);
        break;

    case I_BACK:
        segment(15, 5, 8, 12);
        segment(8, 12, 15, 19);
        break;

    case I_FORWARD:
        segment(9, 5, 16, 12);
        segment(16, 12, 9, 19);
        break;

    case I_UP:
        segment(5, 15, 12, 8);
        segment(12, 8, 19, 15);
        break;

    case I_SEARCH:
        circle(10, 10, 6);
        segment(15, 15, 21, 21);
        break;

    case I_MINUS:
        segment(6, 12, 18, 12);
        break;

    case I_MAX:
        box(6, 6, 12, 12);
        break;

    case I_CLOSE:
        segment(7, 7, 17, 17);
        segment(17, 7, 7, 17);
        break;

    case I_LAYOUT:
    case I_GRID:
        box(3, 3, 6, 6);
        box(15, 3, 6, 6);
        box(3, 15, 6, 6);
        box(15, 15, 6, 6);
        break;

    case I_PLUS:
        segment(12, 5, 12, 19);
        segment(5, 12, 19, 12);
        break;

    case I_TERMINAL:
        box(2, 4, 20, 16);
        segment(6, 8, 10, 12);
        segment(10, 12, 6, 16);
        segment(13, 16, 17, 16);
        break;

    case I_GLOBE:
        circle(12, 12, 9);
        segment(3, 12, 21, 12);
        segment(12, 3, 8, 8);
        segment(8, 8, 8, 16);
        segment(8, 16, 12, 21);
        segment(12, 3, 16, 8);
        segment(16, 8, 16, 16);
        segment(16, 16, 12, 21);
        break;

    case I_GEAR:
        circle(12, 12, 3);
        box(9, 2, 6, 3);
        box(9, 19, 6, 3);
        box(2, 9, 3, 6);
        box(19, 9, 3, 6);
        segment(5, 5, 9, 4);
        segment(15, 4, 19, 5);
        segment(19, 5, 20, 9);
        segment(20, 15, 19, 19);
        segment(19, 19, 15, 20);
        segment(9, 20, 5, 19);
        segment(5, 19, 4, 15);
        segment(4, 9, 5, 5);
        break;

    case I_WIFI:
        segment(2, 8, 6, 5);
        segment(6, 5, 12, 4);
        segment(12, 4, 18, 5);
        segment(18, 5, 22, 8);
        segment(5, 12, 8, 10);
        segment(8, 10, 16, 10);
        segment(16, 10, 19, 12);
        segment(9, 16, 12, 14);
        segment(12, 14, 15, 16);
        segment(12, 20, 12, 20);
        break;

    case I_SOUND:
        box(3, 9, 4, 6);
        segment(7, 9, 12, 5);
        segment(12, 5, 12, 19);
        segment(12, 19, 7, 15);
        segment(16, 8, 18, 11);
        segment(18, 11, 18, 13);
        segment(18, 13, 16, 16);
        segment(19, 5, 22, 10);
        segment(22, 10, 22, 14);
        segment(22, 14, 19, 19);
        break;

    case I_DOC:
        segment(5, 3, 14, 3);
        segment(14, 3, 19, 8);
        segment(19, 8, 19, 21);
        segment(19, 21, 5, 21);
        segment(5, 21, 5, 3);
        segment(14, 3, 14, 8);
        segment(14, 8, 19, 8);
        segment(8, 12, 16, 12);
        segment(8, 16, 14, 16);
        break;

    case I_COPY:
        box(8, 8, 12, 13);
        segment(15, 3, 3, 3);
        segment(3, 3, 3, 16);
        break;

    case I_CUT:
        circle(6, 6, 3);
        circle(6, 18, 3);
        segment(8, 8, 21, 21);
        segment(8, 16, 21, 3);
        break;

    case I_PASTE:
        box(5, 5, 14, 16);
        box(9, 2, 6, 5);
        break;

    case I_RENAME:
        segment(4, 18, 4, 21);
        segment(4, 21, 8, 20);
        segment(8, 20, 21, 7);
        segment(21, 7, 17, 3);
        segment(17, 3, 4, 18);
        segment(14, 6, 18, 10);
        break;

    case I_INFO:
        circle(12, 12, 9);
        segment(12, 7, 12, 7);
        segment(12, 11, 12, 17);
        break;

    case I_CHECK:
        segment(4, 12, 10, 18);
        segment(10, 18, 21, 5);
        break;

    case I_RELOAD:
        segment(20, 10, 20, 4);
        segment(20, 4, 14, 4);
        segment(20, 8, 17, 5);
        segment(17, 5, 12, 4);
        segment(12, 4, 7, 6);
        segment(7, 6, 4, 10);
        segment(4, 10, 4, 14);
        segment(4, 14, 7, 18);
        segment(7, 18, 12, 20);
        segment(12, 20, 17, 18);
        segment(17, 18, 20, 14);
        break;

    case I_RESTORE:
        box(4, 8, 12, 12);
        segment(8, 4, 20, 4);
        segment(20, 4, 20, 16);
        break;

    case I_SAVE:
        box(4, 3, 16, 18);
        box(8, 3, 8, 6);
        box(8, 14, 8, 7);
        break;

    case I_WORKSPACE:
        box(3, 5, 18, 14);
        segment(11, 5, 11, 19);
        break;
    default:
        break;
    }
}

void icon(int x, int y, Icon kind, int size, uint32_t color) {
    if (kind >= I_COUNT || size > 40 || size < 1) {
        return;
    }
    IconCache *c = NULL;
    for (unsigned i = 0; i < 160; i++) {
        if (cache[i].used && cache[i].kind == kind && cache[i].size == size) {
            c = &cache[i];
            break;
        }
    }

    if (!c) {
        c = &cache[replacement++ % 160];
        memset(c, 0, sizeof(*c));
        c->kind = kind;
        c->size = size;
        c->used = true;
        mask = c->mask;
        dim = size;
        path(kind);
    }

    Rect visible = intersect((Rect){x, y, size, size}, desk.clip);
    for (int yy = visible.y; yy < visible.y + visible.h; yy++) {
        for (int xx = visible.x; xx < visible.x + visible.w; xx++) {
            unsigned a = c->mask[(yy - y) * 40 + xx - x];
            if (a) {
                blend((Rect){xx, yy, 1, 1}, color, a);
            }
        }
    }
}
