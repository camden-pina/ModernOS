#include "desktop.h"

/* Physical-pixel coverage, cached per radius. Only corner pixels are sampled;
   straight edges and interiors retain fast spans. No canvas supersampling. */
static uint8_t corners[33][32 * 32];
static bool ready[33];
static unsigned sample_corner(int radius, int x, int y) {
  int r = radius * 32, count = 0;
  for (int sy = 0; sy < 16; sy++)
    for (int sx = 0; sx < 16; sx++) {
      int dx = r - (x * 32 + sx * 2 + 1);
      int dy = r - (y * 32 + sy * 2 + 1);
      count += dx * dx + dy * dy <= r * r;
    }
  return (count * 255 + 128) / 256;
}
unsigned corner_coverage(int radius, int x, int y) {
  if (x < 0 || y < 0)
    return 0;
  if (radius <= 0 || x >= radius || y >= radius)
    return 255;
  if (radius > 32)
    return sample_corner(radius, x, y);
  if (!ready[radius]) {
    for (int row = 0; row < radius; row++)
      for (int col = 0; col < radius; col++)
        corners[radius][row * 32 + col] = sample_corner(radius, col, row);
    ready[radius] = true;
  }
  return corners[radius][y * 32 + x];
}
unsigned rect_coverage(Rect r, int radius, int x, int y) {
  if (!contains(r, x, y))
    return 0;
  radius = MIN(radius, MIN(r.w, r.h) / 2);
  return corner_coverage(radius, MIN(x - r.x, r.x + r.w - 1 - x),
                         MIN(y - r.y, r.y + r.h - 1 - y));
}
