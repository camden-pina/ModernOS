#include "../userland/desktop/wallpaper.c"
#include "wallpaper_r3_reference.h"
Desktop desk;
extern void *malloc(size_t);
extern void free(void *);
int clampi(int64_t v, int low, int high) {
  return v < low ? low : v > high ? high : v;
}
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      const char msg[] = "WALLPAPER ORACLE FAIL: " #x "\n";                    \
      write(2, msg, sizeof(msg) - 1);                                          \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
int main(void) {
  const int sizes[][2] = {{17, 13}, {192, 112}, {640, 480}, {2560, 1600}};
  unsigned cases = 0;
  for (unsigned n = 0; n < 4; n++) {
    int width = sizes[n][0], height = sizes[n][1];
    size_t bytes = (size_t)width * height * 4;
    uint32_t *reference = malloc(bytes), *actual = malloc(bytes + 64);
    CHECK(reference && actual);
    for (int kind = 0; kind < (n == 3 ? 1 : 3); kind++) {
      memset(actual, 0xCD, bytes + 64);
      reference_procedural(reference, width, height, kind);
      for (int y = 0; y < height; y += 17)
        procedural_rows(actual, width, height, kind, y, 17);
      CHECK(!memcmp(actual, reference, bytes));
      for (int k = 0; k < 16; k++)
        CHECK(actual[width * height + k] == 0xcdcdcdcd);
      cases++;
    }
    free(reference);
    free(actual);
  }
  uint32_t image[37 * 29];
  for (int i = 0; i < 37 * 29; i++)
    image[i] = (i % 5 == 0 ? 0x80000000 : 0xff000000) | (i * 991u & 0xffffff);
  Image source = {image, 37, 29};
  uint32_t reference[181 * 113], actual[181 * 113 + 16];
  for (int mode = 0; mode < 4; mode++)
    for (int bg = 0; bg < 3; bg++) {
      memset(actual, 0xCD, sizeof(actual));
      reference_compose(reference, 181, 113, &source, mode,
                        wallpaper_backgrounds[bg]);
      for (int y = 0; y < 113; y += 11)
        compose_rows(actual, 181, 113, &source, mode, wallpaper_backgrounds[bg],
                     y, 11);
      CHECK(!memcmp(actual, reference, sizeof(reference)));
      for (int k = 0; k < 16; k++)
        CHECK(actual[181 * 113 + k] == 0xcdcdcdcd);
      cases++;
    }
  printf("PASS actual wallpaper row optimizations: %u pixel-exact r3 oracle "
         "comparisons, procedural fields through2560x1600, all image "
         "placement/background modes, transparency and chunk guards.\n",
         cases);
}
