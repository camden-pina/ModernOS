/* Actual painters, with independent high-resolution coverage/alpha checks. */
#include "../userland/desktop/render.c"
Desktop desk;
static uint32_t actual[200 * 140], reference[200 * 140];
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      const char message[] =                                                   \
          "CLARITY FAIL line=" STRINGIFY(__LINE__) ": " #x "\n";               \
      write(2, message, sizeof(message) - 1);                                  \
      exit(1);                                                        \
    }                                                                          \
  } while (0)
static void surface(void) {
  desk.width = 200;
  desk.height = 140;
  desk.paint = desk.output = actual;
  desk.paint_stride = 200;
  desk.paint_x = desk.paint_y = 0;
  desk.clip = (Rect){0, 0, 200, 140};
  desk.paint_alpha = false;
  fill(desk.clip, 0xffffff);
}
static uint32_t alpha_reference(uint32_t f, uint32_t b, unsigned a) {
  uint32_t out = 0;
  for (unsigned s = 0; s < 24; s += 8)
    out |= (((((f >> s) & 255) * a + ((b >> s) & 255) * (255 - a)) + 127) / 255)
           << s;
  return out;
}
static unsigned levels(void) {
  bool used[256] = {0};
  unsigned n = 0;
  for (unsigned i = 0; i < 200 * 140; i++)
    used[(actual[i] >> 16) & 255] = true;
  for (unsigned i = 0; i < 256; i++)
    n += used[i];
  return n;
}
int main(void) {
  for (unsigned a = 0; a < 256; a++)
    for (unsigned k = 0; k < 32; k++) {
      uint32_t f = (k * 0x761a97u) & 0xffffff,
               b = (k * 0x71539du + 0xffff) & 0xffffff;
      CHECK((coverage_over(f, b, a) & 0xffffff) == alpha_reference(f, b, a));
      CHECK(coverage_over(0xffffff, 0xffffff, a) == 0xffffff);
    }
  /*64x64 area oracle is independent of the production16x16 cached mask.*/
  unsigned maximum_error = 0;
  for (int r = 1; r <= 32; r++)
    for (int y = 0; y < r; y++)
      for (int x = 0; x < r; x++) {
        unsigned count = 0;
        for (int sy = 0; sy < 64; sy++)
          for (int sx = 0; sx < 64; sx++) {
            int dx = r * 128 - (x * 128 + sx * 2 + 1),
                dy = r * 128 - (y * 128 + sy * 2 + 1);
            count += dx * dx + dy * dy <= r * r * 128 * 128;
          }
        unsigned truth = (count * 255 + 2048) / 4096,
                 c = corner_coverage(r, x, y);
        unsigned error = c > truth ? c - truth : truth - c;
        if (error > maximum_error)
          maximum_error = error;
        CHECK(error <= 8);
        CHECK(c == corner_coverage(r, y, x));
      }
  desk.scale = 100;
  desk.scale_index = 0;
  surface();
  blend((Rect){0, 0, 20, 20}, 0, 0);
  CHECK(actual[0] == 0xffffff);
  rounded((Rect){10, 10, 45, 30}, 8, 0);
  CHECK(levels() > 6);
  memcpy(reference, actual, sizeof(actual));
  surface();
  desk.clip = (Rect){18, 4, 17, 90};
  rounded((Rect){10, 10, 45, 30}, 8, 0);
  for (int y = 0; y < 140; y++)
    for (int x = 0; x < 200; x++)
      CHECK(actual[y * 200 + x] ==
            (contains(desk.clip, x, y) ? reference[y * 200 + x] : 0xffffff));
  for (int scale = 0; scale < 4; scale++) {
    const int scales[] = {100, 125, 150, 200};
    desk.scale = scales[scale];
    desk.scale_index = scale;
    surface();
    icon(20, 20, I_GLOBE, 15 * desk.scale / 100, 0);
    CHECK(levels() > 12);
    surface();
    icon(20, 20, I_GEAR, 80, 0);
    CHECK(levels() > 12);
    surface();
    desk.mouse_x = 30;
    desk.mouse_y = 30;
    desktop_cursor_draw();
    CHECK(levels() > 6);
    surface();
    desk.mouse_x = 199;
    desk.mouse_y = 139;
    desktop_cursor_draw();
  }
  desk.scale = 100;
  desk.scale_index = 0;
  surface();
  label(10, 10, "Coast", FONT_BODY, 0, 180);
  CHECK(levels() > 20);
  printf("PASS production coverage:64x64 area oracle max error=%u/255, exact "
         "all256 alpha values, clipping, icon analytic coverage/full80px "
         "cache, native glyph AA and cursor at four scales (ASan/UBSan).\n",
         maximum_error);
  return 0;
}
