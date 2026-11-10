#include "desktop.h"

/* Small, locally defined outline paths. Cached analytic coverage masks have the
   same stroke and proportions at every scale and require no external assets. */
typedef struct {
  Icon kind;
  int size, scale;
  bool used;
  uint8_t *mask;
  size_t bytes;
} IconCache;

static IconCache cache[160];
static int replacement;
static size_t cache_bytes;
static uint8_t *mask;
static int dim;

static void segment(float x1, float y1, float x2, float y2) {
  float dx = x2 - x1, dy = y2 - y1, length = dx * dx + dy * dy;
  float unit = 24.f / dim, radius = .65f * desk.scale / 100 * unit;
  float inner = radius - unit * .5f, outer = radius + unit * .5f;
  int left =
      MAX(0, (int)((x1 < x2 ? x1 : x2) / unit) - 3 * desk.scale / 100 - 2);
  int top =
      MAX(0, (int)((y1 < y2 ? y1 : y2) / unit) - 3 * desk.scale / 100 - 2);
  int right =
      MIN(dim, (int)((x1 > x2 ? x1 : x2) / unit) + 3 * desk.scale / 100 + 3);
  int bottom =
      MIN(dim, (int)((y1 > y2 ? y1 : y2) / unit) + 3 * desk.scale / 100 + 3);
  for (int y = top; y < bottom; y++)
    for (int x = left; x < right; x++) {
      float px = (x + .5f) * unit, py = (y + .5f) * unit;
      float t = length ? ((px - x1) * dx + (py - y1) * dy) / length : 0;
      t = t < 0 ? 0 : t > 1 ? 1 : t;
      float ax = px - x1 - t * dx, ay = py - y1 - t * dy,
            d2 = ax * ax + ay * ay;
      unsigned alpha = 0;
      if (d2 <= inner * inner)
        alpha = 255;
      else if (d2 < outer * outer) {
        float distance;
        __asm__("sqrtss %1,%0" : "=x"(distance) : "x"(d2));
        float coverage = (outer - distance) * 255 / unit;
        alpha = (unsigned)(coverage + .5f);
        if (alpha > 255)
          alpha = 255;
      }
      if (alpha > mask[y * dim + x])
        mask[y * dim + x] = alpha;
    }
}

static void box(float x, float y, float w, float h) {
  segment(x, y, x + w, y);
  segment(x + w, y, x + w, y + h);
  segment(x + w, y + h, x, y + h);
  segment(x, y + h, x, y);
}

static void circle(float x, float y, float r) {
  /* Higher-resolution original path; no low-resolution bitmap enlargement. */
  static const float a[33][2] = {
      {1.0000000f, 0.0000000f},   {0.9807853f, 0.1950903f},
      {0.9238795f, 0.3826834f},   {0.8314696f, 0.5555702f},
      {0.7071068f, 0.7071068f},   {0.5555702f, 0.8314696f},
      {0.3826834f, 0.9238795f},   {0.1950903f, 0.9807853f},
      {0.0000000f, 1.0000000f},   {-0.1950903f, 0.9807853f},
      {-0.3826834f, 0.9238795f},  {-0.5555702f, 0.8314696f},
      {-0.7071068f, 0.7071068f},  {-0.8314696f, 0.5555702f},
      {-0.9238795f, 0.3826834f},  {-0.9807853f, 0.1950903f},
      {-1.0000000f, 0.0000000f},  {-0.9807853f, -0.1950903f},
      {-0.9238795f, -0.3826834f}, {-0.8314696f, -0.5555702f},
      {-0.7071068f, -0.7071068f}, {-0.5555702f, -0.8314696f},
      {-0.3826834f, -0.9238795f}, {-0.1950903f, -0.9807853f},
      {-0.0000000f, -1.0000000f}, {0.1950903f, -0.9807853f},
      {0.3826834f, -0.9238795f},  {0.5555702f, -0.8314696f},
      {0.7071068f, -0.7071068f},  {0.8314696f, -0.5555702f},
      {0.9238795f, -0.3826834f},  {0.9807853f, -0.1950903f},
      {1.0000000f, -0.0000000f}};
  for (int i = 0; i < 32; i++)
    segment(x + a[i][0] * r, y + a[i][1] * r, x + a[i + 1][0] * r,
            y + a[i + 1][1] * r);
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

  case I_CODE:
    segment(8,5,2,12);segment(2,12,8,19);
    segment(16,5,22,12);segment(22,12,16,19);
    segment(14,3,10,21);
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

  case I_LIST:
    for (int y = 5; y <= 19; y += 7) {
      segment(3, y, 4, y);
      segment(8, y, 21, y);
    }
    break;

  case I_SORT:
    segment(4, 6, 20, 6);
    segment(4, 12, 15, 12);
    segment(4, 18, 10, 18);
    break;

  case I_MORE:
    for (int x = 5; x <= 19; x += 7)
      circle(x, 12, .8f);
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
  if (kind >= I_COUNT || size > ICON_MAX || size < 1) {
    return;
  }
  IconCache *c = NULL;
  for (unsigned i = 0; i < 160; i++) {
    if (cache[i].used && cache[i].kind == kind && cache[i].size == size &&
        cache[i].scale == desk.scale) {
      c = &cache[i];
      break;
    }
  }

  if (!c) {
    c = &cache[replacement++ % 160];
    cache_bytes -= c->bytes;
    deallocate(c->mask, c->bytes);
    size_t needed = (size_t)size * size;
    for (unsigned i = 0; i < 160 && cache_bytes + needed > 4 * 1024 * 1024; i++)
      if (&cache[i] != c && cache[i].used) {
        cache_bytes -= cache[i].bytes;
        deallocate(cache[i].mask, cache[i].bytes);
        memset(&cache[i], 0, sizeof(cache[i]));
      }
    memset(c, 0, sizeof(*c));
    c->bytes = (size_t)size * size;
    c->mask = allocate(c->bytes);
    if (!c->mask) {
      c->bytes = 0;
      return;
    }
    cache_bytes += c->bytes;
    memset(c->mask, 0, c->bytes);
    c->kind = kind;
    c->size = size;
    c->scale = desk.scale;
    c->used = true;
    mask = c->mask;
    dim = size;
    path(kind);
  }

  Rect visible = intersect((Rect){x, y, size, size}, desk.clip);
  for (int yy = visible.y; yy < visible.y + visible.h; yy++) {
    for (int xx = visible.x; xx < visible.x + visible.w; xx++) {
      unsigned a = c->mask[(yy - y) * size + xx - x];
      if (a) {
        blend((Rect){xx, yy, 1, 1}, color, a);
      }
    }
  }
}

void icon_cleanup(void) {
  cache_bytes = 0;
  for (unsigned i = 0; i < 160; i++) {
    deallocate(cache[i].mask, cache[i].bytes);
    memset(&cache[i], 0, sizeof(cache[i]));
  }
}
