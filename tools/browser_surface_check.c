#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../userland/include/browser_protocol.h"

enum { WIDTH = 640, HEIGHT = 400, PIXELS = WIDTH * HEIGHT };
static uint32_t guarded[PIXELS + 2], original[PIXELS], expected[PIXELS];
static unsigned live_allocations;

static void *paint_alloc(size_t bytes) {
    void *p = malloc(bytes);
    if (p)
        ++live_allocations;
    return p;
}

static void paint_free(void *p, size_t bytes) {
    (void)bytes;
    assert(live_allocations);
    --live_allocations;
    free(p);
}

static void *paint_oom(size_t bytes) {
    (void)bytes;
    return NULL;
}

static struct browser_packet packet(int x, int y, int width, int height) {
    return (struct browser_packet){.magic = BROWSER_MAGIC,
                                   .type = BROWSER_COPY,
                                   .bytes = sizeof(struct browser_copy_source),
                                   .x = x,
                                   .y = y,
                                   .width = width,
                                   .height = height,
                                   .canvas_width = WIDTH,
                                   .canvas_height = HEIGHT};
}

static void reset(void) {
    guarded[0] = guarded[PIXELS + 1] = 0x5a19beef;
    for (unsigned i = 0; i < PIXELS; i++)
        original[i] = i * 0x91a35u + 7;
    memcpy(guarded + 1, original, sizeof(original));
    memcpy(expected, original, sizeof(original));
}

static void check_copy(int x, int y, int sx, int sy, int width, int height) {
    reset();
    struct browser_packet p = packet(x, y, width, height);
    struct browser_copy_source source = {sx, sy};
    for (int row = 0; row < height; row++)
        memcpy(expected + (y + row) * WIDTH + x, original + (sy + row) * WIDTH + sx, width * 4);
    assert(browser_surface_copy(guarded+1,WIDTH,HEIGHT,&p,&source));
    assert(!memcmp(guarded+1,expected,sizeof(expected)));
    assert(guarded[0]==0x5a19beef && guarded[PIXELS+1]==0x5a19beef);
}

static void check_rejected(struct browser_packet p, struct browser_copy_source source) {
    reset();
    assert(!browser_surface_copy(guarded+1,WIDTH,HEIGHT,&p,&source));
    assert(!memcmp(guarded+1,original,sizeof(original)));
    assert(guarded[0]==0x5a19beef && guarded[PIXELS+1]==0x5a19beef);
}

static void check_paint(void) {
    reset();
    struct browser_paint paint = {0};
    struct browser_packet present = {.magic = BROWSER_MAGIC,
                                     .type = BROWSER_PRESENT,
                                     .canvas_width = WIDTH,
                                     .canvas_height = HEIGHT};
    struct browser_packet copy = packet(0, 0, WIDTH, HEIGHT - 100);
    struct browser_copy_source source = {0, 100};
    assert(browser_surface_paint_queue(&paint,WIDTH,HEIGHT,1,&copy,&source,paint_alloc));
    for (int y = 0; y < HEIGHT - 100; ++y)
        memcpy(expected + y * WIDTH, original + (y + 100) * WIDTH, WIDTH * 4);
    uint32_t color[20 * 10];
    for (unsigned i = 0; i < 200; ++i)
        color[i] = 0xff1177bb;
    struct browser_packet patch = {.magic = BROWSER_MAGIC,
                                   .type = BROWSER_FRAME,
                                   .bytes = sizeof(color),
                                   .x = 20,
                                   .y = 30,
                                   .width = 20,
                                   .height = 10,
                                   .canvas_width = WIDTH,
                                   .canvas_height = HEIGHT};
    assert(browser_surface_paint_queue(&paint,WIDTH,HEIGHT,1,&patch,color,paint_alloc));
    for (int y = 0; y < 10; ++y)
        memcpy(expected + (30 + y) * WIDTH + 20, color + y * 20, 20 * 4);
    assert(!memcmp(guarded+1,original,sizeof(original))); /* still displayed */
    struct browser_packet bad = present;
    bad.canvas_width = WIDTH + 1;
    assert(!browser_surface_paint_apply(guarded+1,WIDTH,HEIGHT,&paint,&bad));
    assert(!memcmp(guarded+1,original,sizeof(original)));
    assert(browser_surface_paint_apply(guarded+1,WIDTH,HEIGHT,&paint,&present));
    assert(!memcmp(guarded+1,expected,sizeof(expected)));
    assert(guarded[0]==0x5a19beef && guarded[PIXELS+1]==0x5a19beef);
    browser_surface_paint_cancel(&paint, paint_free);
    assert(!live_allocations && !paint.count && !paint.bytes);
    assert(!browser_surface_paint_apply(guarded+1,WIDTH,HEIGHT,&paint,&present));
    reset();
    assert(browser_surface_paint_queue(&paint,WIDTH,HEIGHT,1,&patch,color,paint_alloc));
    unsigned count = paint.count;
    size_t bytes = paint.bytes;
    bad = copy;
    bad.bytes = 7;
    unsigned char short_source[7] = {0};
    assert(!browser_surface_paint_queue(&paint,WIDTH,HEIGHT,1,&bad,short_source,paint_alloc));
    source.y = INT_MAX;
    assert(!browser_surface_paint_queue(&paint,WIDTH,HEIGHT,1,&copy,&source,paint_alloc));
    assert(!browser_surface_paint_queue(&paint,WIDTH,HEIGHT,1,&patch,color,paint_oom));
    bad = patch;
    bad.canvas_width = WIDTH + 1;
    assert(!browser_surface_paint_queue(&paint,WIDTH,HEIGHT,1,&bad,color,paint_alloc));
    assert(paint.count==count && paint.bytes==bytes);
    browser_surface_paint_cancel(&paint, paint_free);
    assert(!live_allocations && !memcmp(guarded+1,original,sizeof(original)));
    assert(!browser_surface_paint_queue(&paint,0,0,0,&patch,color,paint_alloc));
    struct browser_packet full = {.magic = BROWSER_MAGIC,
                                  .type = BROWSER_FRAME,
                                  .bytes = sizeof(original),
                                  .width = WIDTH,
                                  .height = HEIGHT,
                                  .canvas_width = WIDTH,
                                  .canvas_height = HEIGHT};
    assert(browser_surface_paint_queue(&paint,0,0,0,&full,original,paint_alloc));
    assert(browser_surface_paint_apply(guarded+1,WIDTH,HEIGHT,&paint,&present));
    browser_surface_paint_cancel(&paint, paint_free);
    for (unsigned i = 0; i < BROWSER_PAINT_MAX_PACKETS; ++i)
        assert(browser_surface_paint_queue(&paint,WIDTH,HEIGHT,1,&patch,color,paint_alloc));
    assert(!browser_surface_paint_queue(&paint,WIDTH,HEIGHT,1,&patch,color,paint_alloc));
    browser_surface_paint_cancel(&paint, paint_free);
    assert(!live_allocations);
    while (browser_surface_paint_queue(&paint, WIDTH, HEIGHT, 1, &full, original, paint_alloc))
        assert(paint.bytes<=BROWSER_PAINT_MAX_BYTES);
    assert(paint.count && paint.count<BROWSER_PAINT_MAX_PACKETS);
    assert(!memcmp(guarded+1,original,sizeof(original)));
    browser_surface_paint_cancel(&paint, paint_free);
    assert(!live_allocations);
    struct browser_packet cancel = {.magic = BROWSER_MAGIC, .type = BROWSER_CANCEL_PAINT};
    assert(browser_surface_cancel_valid(&cancel));
    cancel.bytes = 1;
    assert(!browser_surface_cancel_valid(&cancel));
    bad = present;
    bad.flags = 1;
    assert(!browser_surface_present_valid(&bad));
    puts(
        "PASS atomic browser paints: unchanged before present, copy/damage commit, cancellation, malformed payloads, resize initialization, allocation failure and packet/byte limits");
}

int main(void) {
    _Static_assert(sizeof(struct browser_packet) == 40, "preserve wire header");
    _Static_assert(sizeof(struct browser_copy_source) == 8, "preserve copy payload");
    check_copy(0, 100, 0, 0, WIDTH, 300);
    check_copy(0, 0, 0, 100, WIDTH, 300);
    check_copy(0, 0, 100, 0, 540, HEIGHT);
    check_copy(100, 0, 0, 0, 540, HEIGHT);
    check_copy(20, 30, 19, 29, 300, 250);
    check_copy(19, 29, 20, 30, 300, 250);
    check_copy(10, 10, 10, 10, WIDTH - 20, HEIGHT - 20);
    check_copy(WIDTH - 1, HEIGHT - 1, 0, 0, 1, 1);
    struct browser_packet base = packet(0, 0, 300, 200), bad;
    struct browser_copy_source source = {0, 0};
    bad = base;
    bad.magic = 0;
    check_rejected(bad, source);
    bad = base;
    bad.type = BROWSER_FRAME;
    check_rejected(bad, source);
    bad = base;
    bad.bytes = 7;
    check_rejected(bad, source);
    bad = base;
    bad.flags = 1;
    check_rejected(bad, source);
    bad = base;
    bad.x = -1;
    check_rejected(bad, source);
    bad = base;
    bad.y = INT_MAX;
    check_rejected(bad, source);
    bad = base;
    bad.width = 0;
    check_rejected(bad, source);
    bad = base;
    bad.height = -1;
    check_rejected(bad, source);
    bad = base;
    bad.width = INT_MAX;
    check_rejected(bad, source);
    bad = base;
    bad.canvas_width = UINT_MAX;
    check_rejected(bad, source);
    bad = base;
    bad.canvas_height = HEIGHT + 1;
    check_rejected(bad, source);
    check_rejected(base, (struct browser_copy_source){-1, 0});
    check_rejected(base, (struct browser_copy_source){0, INT_MAX});
    check_rejected(base, (struct browser_copy_source){WIDTH - 299, 0});
    check_rejected(base, (struct browser_copy_source){0, HEIGHT - 199});
    assert(!browser_surface_copy(NULL,WIDTH,HEIGHT,&base,&source));
    check_paint();
    puts(
        "PASS retained browser canvas: overlapping copies, edge pixels, bounds/overflow/size rejection and unchanged rejected surfaces");
}
