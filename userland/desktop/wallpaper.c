#include "desktop.h"
#include <wallpaper_protocol.h>

#define PREVIEW_WIDTH 192
#define PREVIEW_HEIGHT 112

typedef struct {
  uint32_t *pixels;
  int width, height;
} Image;

static Image active, previews[WALLPAPER_COUNT], sources[WALLPAPER_COUNT];
static char source_paths[WALLPAPER_COUNT][OS_PATH_MAX];
static uint64_t source_used[WALLPAPER_COUNT], source_clock;
static uint32_t *spare;
static size_t spare_bytes;
static bool previews_requested, preview_failed[WALLPAPER_COUNT];
static char active_path[OS_PATH_MAX];
static bool generated;
static int generated_index, generated_mode, generated_background,
    generated_width, generated_height;
const char *const wallpaper_names[WALLPAPER_COUNT] = {
    "Coast", "Dusk", "Moss", "Alpine Lake", "Desert Sunset", "Choose image..."};
const uint32_t wallpaper_backgrounds[3] = {0x18252D, 0x000000, 0xF1F3F0};
static const char *const paths[] = {"/usr/share/wallpapers/alpine-lake.png",
                                    "/usr/share/wallpapers/desert-sunset.png"};

static void release_image(Image *image) {
  deallocate(image->pixels, (size_t)image->width * image->height * 4);
  memset(image, 0, sizeof(*image));
}

static int receive(int fd, void *buffer, size_t bytes) {
  unsigned char *p = buffer;
  while (bytes) {
    long n = read(fd, p, bytes);
    if (n == -E_INTR)
      continue;
    if (n <= 0)
      return n ? n : -E_IO;
    p += n;
    bytes -= n;
  }

  return 0;
}

static int load_image(const char *path, Image *image) {
  struct os_stat st;
  int error = stat(path, &st);
  if (error)

    return error;
  if ((st.mode & 0170000) != S_IFREG || st.size < 8 ||
      st.size > WALLPAPER_MAX_FILE)
    return -E_INVAL;
  int streams[2];
  error = call2(SYS_pipe2, streams, O_CLOEXEC);
  if (error)
    return error;
  int null = open("/dev/null", O_RDWR | O_CLOEXEC, 0);
  if (null < 0) {
    close(streams[0]);
    close(streams[1]);
    return null;
  }

  char *args[] = {"/bin/wallpaper-decode", (char *)path, NULL};
  struct os_spawn_io spec = {.fd = {null, streams[1], null},
                             .cwd = (uintptr_t)desk.home};
  int pid = call3(SYS_spawn_io, args[0], args, &spec);
  close(null);
  close(streams[1]);
  if (pid < 0) {
    close(streams[0]);
    return pid;
  }

  struct wallpaper_packet packet;
  error = receive(streams[0], &packet, sizeof(packet));
  if (!error) {
    if (packet.magic != WALLPAPER_MAGIC || packet.error > 0)
      error = -E_IO;
    else if (packet.error)
      error = packet.error;
    else if (!packet.width || !packet.height ||
             packet.width > WALLPAPER_MAX_EDGE ||
             packet.height > WALLPAPER_MAX_EDGE ||
             (uint64_t)packet.width * packet.height > WALLPAPER_MAX_PIXELS)
      error = -E_INVAL;
  }

  if (!error) {
    image->width = packet.width;
    image->height = packet.height;
    size_t bytes = (size_t)packet.width * packet.height * 4;
    image->pixels = allocate(bytes);
    error =
        image->pixels ? receive(streams[0], image->pixels, bytes) : -E_NOMEM;
  }

  close(streams[0]);
  if (error)
    call2(SYS_kill, pid, 9);

  int status = 0, waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited == -E_INTR);
  if (!error && (waited < 0 || status))
    error = -E_IO;

  if (error)
    release_image(image);
  return error;
}

static uint32_t color_mix(uint32_t a, uint32_t b, unsigned factor) {
  factor = MIN(factor, 255);
  unsigned inverse = 255 - factor;
  return ((((a & 0xff00ff) * inverse + (b & 0xff00ff) * factor + 0x800080) >>
           8) &
          0xff00ff) |
         ((((a & 0x00ff00) * inverse + (b & 0x00ff00) * factor + 0x008000) >>
           8) &
          0x00ff00);
}

static void procedural_rows(uint32_t *pixels, int width, int height, int index,
                            int begin, int rows) {
  uint32_t start = THEME_WALLPAPER_DARK, mid = THEME_WALLPAPER_MID,
           end = THEME_WALLPAPER_TEAL, glow = THEME_WALLPAPER_GLOW,
           lavender = 0x777C97;
  if (index == 1) {
    start = 0x30344E;
    mid = 0x414663;
    end = 0x344353;
    glow = 0x686D8A;
    lavender = 0x7C7894;
  }

  if (index == 2) {
    start = 0x273E41;
    mid = 0x314B4D;
    end = 0x2A4147;
    glow = 0x597E78;
    lavender = 0x78928D;
  }

  static int cached_width, columns[8192], curves[8192];
  if (cached_width != width) {
    for (int x = 0; x < width; x++) {
      columns[x] = (int64_t)x * 1024 / width;
      int64_t u = ((int64_t)x * 2 + 1) * 32768 / width, d = u - 380 * 64;
      curves[x] = 620 * 64 - u / 4 + d * d / (2300 * 64);
    }
    cached_width = width;
  }
  const int offsets[] = {-130, -20, 100, 255};
  const uint32_t coast[] = {0x829BC9, 0xEAA2AB, 0xD45C8A, 0x243E8C};
  uint32_t opaque[4];
  for (int i = 0; i < 4; i++)
    opaque[i] = color_mix(0, coast[i], 255);
  for (int y = begin; y < MIN(height, begin + rows); y++) {
    int v = (int64_t)y * 1024 / height;
    int64_t vy = ((int64_t)y * 2 + 1) * 32768 / height;
    uint32_t sky = color_mix(0xA8D9E9, 0x5088C3, vy / (64 * 6));
    unsigned shade = clampi((vy - 700 * 64) / (3 * 64), 0, 100);
    uint32_t shaded[4];
    for (int i = 0; i < 4; i++)
      shaded[i] = color_mix(opaque[i], 0x12285B, shade);
    uint32_t shaded_sky = color_mix(sky, 0x12285B, shade);
    for (int x = 0; x < width; x++) {
      int u = columns[x], t = (u + v) / 2;
      if (index == 0) {
        uint32_t c = shaded_sky;
        int64_t dy = vy - curves[x];
        for (int band = 3; band >= 0; band--) {
          int64_t distance = dy - offsets[band] * 64;
          if (distance <= -640)
            continue;
          if (distance >= 640)
            c = shaded[band];
          else
            c = color_mix(color_mix(band ? opaque[band - 1] : sky, coast[band],
                                    (distance + 640) * 255 / 1280),
                          0x12285B, shade);
          break;
        }
        pixels[(size_t)y * width + x] = c;
        continue;
      }
      uint32_t c = t < 512 ? color_mix(start, mid, t / 2)
                           : color_mix(mid, end, (t - 512) / 2);
      int dx = u - 880, dy = v + 60;
      unsigned d = (dx * dx + dy * dy) / 512;
      unsigned a = 50 * 1024 / (1024 + d * d / 128);
      c = color_mix(c, glow, a);

      dx = u + 200;
      dy = v - 960;
      d = (dx * dx + dy * dy) / 640;
      a = 37 * 1024 / (1024 + d * d / 180);
      c = color_mix(c, lavender, a);

      dx = u - 480;
      dy = v - 640;
      d = (dx * dx + dy * dy) / 360;
      a = 9 * 1024 / (1024 + d * d / 120);
      c = color_mix(c, 0x9CACAD, a);

      unsigned noise =
          ((unsigned)x * 1664525u + (unsigned)y * 1013904223u) >> 30;
      unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
      if (noise == 0) {
        r += r < 255;
        g += g < 255;
        b += b < 255;
      }
      pixels[(size_t)y * width + x] = (r << 16) | (g << 8) | b;
    }
  }
}

static void procedural(uint32_t *pixels, int width, int height, int index) {
  procedural_rows(pixels, width, height, index, 0, height);
}

static uint32_t over(uint32_t source, uint32_t background) {
  unsigned alpha = source >> 24;
  if (alpha == 255)
    return source & 0xffffff;
  uint32_t color = 0;
  for (unsigned shift = 0; shift <= 16; shift += 8) {
    unsigned a = (source >> shift) & 255, b = (background >> shift) & 255;
    color |= ((a * alpha + b * (255 - alpha) + 127) / 255) << shift;
  }
  return color;
}

static uint32_t interpolate(uint32_t a, uint32_t b, unsigned factor) {
  uint32_t color = 0;
  for (unsigned shift = 0; shift <= 16; shift += 8) {
    unsigned av = (a >> shift) & 255, bv = (b >> shift) & 255;
    color |= ((av * (65536 - factor) + bv * factor + 32768) >> 16) << shift;
  }
  return color;
}

static void compose_rows(uint32_t *out, int width, int height, Image *image,
                         int mode, uint32_t bg, int begin, int rows) {
  int iw = image->width, ih = image->height, dw = width, dh = height;
  bool wide = (int64_t)iw * height >= (int64_t)ih * width;
  if (mode == WALLPAPER_CENTER) {
    dw = iw;
    dh = ih;
  } else if (mode == WALLPAPER_FILL) {
    if (wide)
      dw = ((int64_t)iw * height + ih - 1) / ih;
    else
      dh = ((int64_t)ih * width + iw - 1) / iw;
  } else if (mode == WALLPAPER_FIT) {
    if (wide)
      dh = MAX(1, (int64_t)ih * width / iw);
    else
      dw = MAX(1, (int64_t)iw * height / ih);
  }

  int left = (width - dw) / 2, top = (height - dh) / 2;
  static int map_width, map_iw, map_dw, map_left, xsrc[8192], xnext[8192];
  static unsigned xfrac[8192];
  if (map_width != width || map_iw != iw || map_dw != dw || map_left != left) {
    for (int x = 0; x < width; x++) {
      int64_t fx = ((int64_t)(x - left) * 2 + 1) * iw * 32768 / dw - 32768;
      fx = MAX(0, MIN(fx, (int64_t)(iw - 1) * 65536));
      xsrc[x] = fx >> 16;
      xnext[x] = MIN(xsrc[x] + 1, iw - 1);
      xfrac[x] = fx & 65535;
    }
    map_width = width;
    map_iw = iw;
    map_dw = dw;
    map_left = left;
  }
  for (int y = begin; y < MIN(height, begin + rows); y++) {
    int64_t fy = ((int64_t)(y - top) * 2 + 1) * ih * 32768 / dh - 32768;
    fy = MAX(0, MIN(fy, (int64_t)(ih - 1) * 65536));
    int sy = fy >> 16, next_y = MIN(sy + 1, ih - 1);
    for (int x = 0; x < width; x++) {
      uint32_t color = bg;
      if (x >= left && x < left + dw && y >= top && y < top + dh) {
        int sx = xsrc[x], next_x = xnext[x];
        unsigned fx = xfrac[x];
        uint32_t a = over(image->pixels[(size_t)sy * iw + sx], bg),
                 b = over(image->pixels[(size_t)sy * iw + next_x], bg),
                 c = over(image->pixels[(size_t)next_y * iw + sx], bg),
                 d = over(image->pixels[(size_t)next_y * iw + next_x], bg);
        color = interpolate(interpolate(a, b, fx & 65535),
                            interpolate(c, d, fx & 65535), fy & 65535);
      }

      out[(size_t)y * width + x] = color;
    }
  }
}

static void compose(uint32_t *out, int width, int height, Image *image,
                    int mode, uint32_t bg) {
  compose_rows(out, width, height, image, mode, bg, 0, height);
}

static void make_preview(int index, Image *image) {
  Image *preview = &previews[index];
  if (!preview->pixels) {
    preview->width = PREVIEW_WIDTH;
    preview->height = PREVIEW_HEIGHT;
    preview->pixels = allocate(PREVIEW_WIDTH * PREVIEW_HEIGHT * 4);
  }
  if (!preview->pixels)
    return;
  if (index < 3)
    procedural(preview->pixels, PREVIEW_WIDTH, PREVIEW_HEIGHT, index);
  else
    compose(preview->pixels, PREVIEW_WIDTH, PREVIEW_HEIGHT, image,
            WALLPAPER_FILL, wallpaper_backgrounds[0]);
}

typedef struct {
  bool busy;
  int index, mode, background, row;
  Image *source;
  char path[OS_PATH_MAX];
  uint64_t started;
} WallpaperJob;
typedef struct {
  int fd, pid, index;
  bool preview;
  char path[OS_PATH_MAX];
  struct wallpaper_packet packet;
  size_t header, received;
  Image image;
} WallpaperDecoder;
static WallpaperJob job;
static WallpaperDecoder decoder = {.fd = -1};
static int retired_decoders[32];
static unsigned retired_count;
static uint64_t job_requests, job_commits, job_cancels, decoder_starts,
    cache_hits, tick_max_us;
static uint64_t wallpaper_time(void) {
  struct os_timespec t;
  return call2(SYS_clock_gettime, 1, &t)
             ? now_ms() * 1000
             : (uint64_t)t.sec * 1000000 + t.nsec / 1000;
}
static void notify_settings(void) {
  for (int i = 0; i < MAX_WINDOWS; i++)
    if (desk.windows[i].alive && desk.windows[i].app == APP_SETTINGS)
      invalidate_window(&desk.windows[i]);
}
static void reap_decoders(void) {
  for (unsigned i = 0; i < retired_count;) {
    int status;
    int p = waitpid(retired_decoders[i], &status, 1);
    if (p != 0 && p != -E_INTR)
      retired_decoders[i] = retired_decoders[--retired_count];
    else
      i++;
  }
}
static bool cancel_decoder(void) {
  if (!decoder.pid)
    return true;
  reap_decoders();
  if (retired_count == 32)
    return false;
  call2(SYS_kill, decoder.pid, 9);
  close(decoder.fd);
  retired_decoders[retired_count++] = decoder.pid;
  release_image(&decoder.image);
  memset(&decoder, 0, sizeof(decoder));
  decoder.fd = -1;
  job_cancels++;
  return true;
}
static void trim_sources(int keep) {
  int count = 0;
  for (int i = 3; i < WALLPAPER_COUNT; i++)
    count += sources[i].pixels != NULL;
  while (count > 2) {
    int oldest = -1;
    for (int i = 3; i < WALLPAPER_COUNT; i++)
      if (i != keep && i != desk.wallpaper_index && sources[i].pixels &&
          (oldest < 0 || source_used[i] < source_used[oldest]))
        oldest = i;
    if (oldest < 0)
      break;
    release_image(&sources[oldest]);
    source_paths[oldest][0] = 0;
    count--;
  }
}
static int start_decoder(int index, const char *path, bool preview) {
  int streams[2];
  long error = call2(SYS_pipe2, streams, O_CLOEXEC);
  if (error)
    return error;
  int null = open("/dev/null", O_RDWR | O_CLOEXEC, 0);
  if (null < 0) {
    close(streams[0]);
    close(streams[1]);
    return null;
  }
  if (call3(SYS_fcntl, streams[0], F_SETFL, O_NONBLOCK) < 0) {
    close(null);
    close(streams[0]);
    close(streams[1]);
    return -E_IO;
  }
  char *args[] = {"/bin/wallpaper-decode", (char *)path, NULL};
  struct os_spawn_io spec = {.fd = {null, streams[1], null},
                             .cwd = (uintptr_t)desk.home};
  int pid = call3(SYS_spawn_io, args[0], args, &spec);
  close(null);
  close(streams[1]);
  if (pid < 0) {
    close(streams[0]);
    return pid;
  }
  memset(&decoder, 0, sizeof(decoder));
  decoder.fd = streams[0];
  decoder.pid = pid;
  decoder.index = index;
  decoder.preview = preview;
  copy_text(decoder.path, sizeof(decoder.path), path);
  decoder_starts++;
  return 0;
}
static void wallpaper_failure(int error, bool preview, int index) {
  if (preview)
    preview_failed[index] = true;
  else {
    job.busy = false;
    copy_text(desk.wallpaper_status, sizeof(desk.wallpaper_status),
              "Wallpaper unavailable: ");
    append_text(desk.wallpaper_status, sizeof(desk.wallpaper_status),
                error_text(error));
  }
  if (desk.trace)
    printf("DESKTOP WALLPAPER_ERROR index=%d error=%d preview=%d\n", index,
           error, preview);
  cancel_decoder();
  notify_settings();
}
static void service_decoder(void) {
  if (!decoder.pid)
    return;
  bool preview = decoder.preview;
  int index = decoder.index;
  for (unsigned pass = 0; pass < 8; pass++) {
    if (decoder.header < sizeof(decoder.packet)) {
      long n = read(decoder.fd, (char *)&decoder.packet + decoder.header,
                    sizeof(decoder.packet) - decoder.header);
      if (n == -E_AGAIN || n == -E_INTR)
        return;
      if (n <= 0) {
        wallpaper_failure(n < 0 ? n : -E_IO, preview, index);
        return;
      }
      decoder.header += n;
      if (decoder.header < sizeof(decoder.packet))
        continue;
      struct wallpaper_packet *p = &decoder.packet;
      if (p->magic != WALLPAPER_MAGIC || p->error > 0) {
        wallpaper_failure(-E_IO, preview, index);
        return;
      }
      if (p->error) {
        wallpaper_failure(p->error, preview, index);
        return;
      }
      if (!p->width || !p->height || p->width > WALLPAPER_MAX_EDGE ||
          p->height > WALLPAPER_MAX_EDGE ||
          (uint64_t)p->width * p->height > WALLPAPER_MAX_PIXELS) {
        wallpaper_failure(-E_INVAL, preview, index);
        return;
      }
      decoder.image = (Image){.width = p->width, .height = p->height};
      decoder.image.pixels = allocate((size_t)p->width * p->height * 4);
      if (!decoder.image.pixels) {
        wallpaper_failure(-E_NOMEM, preview, index);
        return;
      }
    }
    size_t bytes = (size_t)decoder.image.width * decoder.image.height * 4;
    if (decoder.received < bytes) {
      long n = read(decoder.fd, (char *)decoder.image.pixels + decoder.received,
                    MIN(bytes - decoder.received, 65536));
      if (n == -E_AGAIN || n == -E_INTR)
        return;
      if (n <= 0) {
        wallpaper_failure(n < 0 ? n : -E_IO, preview, index);
        return;
      }
      decoder.received += n;
      if (decoder.received < bytes)
        continue;
    }
    int status = 0, result = waitpid(decoder.pid, &status, 1);
    if (!result || result == -E_INTR)
      return;
    if (result < 0 || status) {
      wallpaper_failure(-E_IO, preview, index);
      return;
    }
    close(decoder.fd);
    decoder.fd = -1;
    decoder.pid = 0;
    release_image(&sources[index]);
    sources[index] = decoder.image;
    memset(&decoder.image, 0, sizeof(decoder.image));
    copy_text(source_paths[index], sizeof(source_paths[index]), decoder.path);
    source_used[index] = ++source_clock;
    trim_sources(index);
    make_preview(index, &sources[index]);
    if (!preview && job.busy && job.index == index &&
        !strcmp(job.path, decoder.path)) {
      job.source = &sources[index];
      job.row = 0;
    }
    notify_settings();
    return;
  }
}
static int request_wallpaper(int index, const char *path) {
  if (!spare) {
    spare_bytes = (size_t)desk.width * desk.height * 4;
    spare = allocate(spare_bytes);
    if (!spare)
      return -E_NOMEM;
  }
  bool promote = decoder.pid && decoder.index == index &&
                 !strcmp(decoder.path, path ? path : "");
  if (!promote && !cancel_decoder())
    return -E_AGAIN;
  if (job.busy)
    job_cancels++;
  memset(&job, 0, sizeof(job));
  job.busy = true;
  job.index = index;
  job.mode = desk.wallpaper_mode;
  job.background = desk.wallpaper_background;
  job.started = wallpaper_time();
  job.row = 0;
  copy_text(job.path, sizeof(job.path), path ? path : "");
  job_requests++;
  if (index >= 3) {
    if (sources[index].pixels && !strcmp(source_paths[index], job.path)) {
      job.source = &sources[index];
      source_used[index] = ++source_clock;
      cache_hits++;
    } else if (promote)
      decoder.preview = false;
    else {
      int error = start_decoder(index, job.path, false);
      if (error) {
        job.busy = false;
        return error;
      }
    }
  }
  copy_text(desk.wallpaper_status, sizeof(desk.wallpaper_status), "Preparing ");
  append_text(desk.wallpaper_status, sizeof(desk.wallpaper_status),
              index == WALLPAPER_CUSTOM ? basename(job.path)
                                        : wallpaper_names[index]);
  append_text(desk.wallpaper_status, sizeof(desk.wallpaper_status), "...");
  if (desk.trace)
    printf("DESKTOP WALLPAPER_REQUEST index=%d mode=%d time=%lu\n", index,
           job.mode, now_ms());
  notify_settings();
  return 0;
}
int wallpaper_set(int index, const char *path) {
  if (index < 0 || index >= WALLPAPER_COUNT)
    return -E_INVAL;
  const char *target = index == WALLPAPER_CUSTOM ? path
                       : index >= 3              ? paths[index - 3]
                                                 : "";
  if (index >= 3) {
    if (!target || !*target || strlen(target) >= OS_PATH_MAX)
      return -E_INVAL;
    for (const char *p = target; *p; p++)
      if (*p == '\n' || *p == '\r')
        return -E_INVAL;
    struct os_stat st;
    int e = stat(target, &st);
    if (e)
      return e;
    if ((st.mode & 0170000) != S_IFREG || st.size < 8 ||
        st.size > WALLPAPER_MAX_FILE)
      return -E_INVAL;
  }
  if (desk.running)
    return request_wallpaper(index, target);
  /* Startup has no interactive desktop yet. Live changes always use the job. */
  if (index >= 3) {
    Image image = {0};
    int e = load_image(target, &image);
    if (e)
      return e;
    release_image(&sources[index]);
    sources[index] = image;
    copy_text(source_paths[index], sizeof(source_paths[index]), target);
    source_used[index] = ++source_clock;
    active = sources[index];
    copy_text(active_path, sizeof(active_path), target);
    if (index == WALLPAPER_CUSTOM)
      copy_text(desk.wallpaper_path, sizeof(desk.wallpaper_path), target);
    make_preview(index, &active);
  } else {
    memset(&active, 0, sizeof(active));
    active_path[0] = 0;
  }
  desk.wallpaper_index = index;
  desk.wallpaper_status[0] = 0;
  generated = false;
  return 0;
}
void wallpaper_generate(void) {
  if (desk.running) {
    if (job.busy) {
      if (job.mode != desk.wallpaper_mode ||
          job.background != desk.wallpaper_background) {
        int index = job.index;
        char path[OS_PATH_MAX];
        copy_text(path, sizeof(path), job.path);
        int e = request_wallpaper(index, path);
        if (e)
          wallpaper_failure(e, false, index);
      }
      return;
    }
    if (generated && generated_index == desk.wallpaper_index &&
        generated_mode == desk.wallpaper_mode &&
        generated_background == desk.wallpaper_background &&
        generated_width == desk.width && generated_height == desk.height)
      return;
    int e = wallpaper_set(desk.wallpaper_index, desk.wallpaper_path);
    if (e)
      wallpaper_failure(e, false, desk.wallpaper_index);
    return;
  }
  if (desk.wallpaper_index >= 3 && active.pixels)
    compose(desk.wallpaper, desk.width, desk.height, &active,
            desk.wallpaper_mode,
            wallpaper_backgrounds[desk.wallpaper_background]);
  else
    procedural(desk.wallpaper, desk.width, desk.height,
               MIN(desk.wallpaper_index, 2));
  generated = true;
  generated_index = desk.wallpaper_index;
  generated_mode = desk.wallpaper_mode;
  generated_background = desk.wallpaper_background;
  generated_width = desk.width;
  generated_height = desk.height;
  if (!spare) {
    spare_bytes = (size_t)desk.width * desk.height * 4;
    spare = allocate(spare_bytes);
  }
}
void wallpaper_prepare_previews(void) {
  previews_requested = true;
  memset(preview_failed, 0, sizeof(preview_failed));
}
bool wallpaper_busy(void) { return job.busy; }
bool wallpaper_preview_failed(int index) {
  return index >= 0 && index < WALLPAPER_COUNT && preview_failed[index];
}
bool wallpaper_preview_ready(int index) {
  return index >= 0 && index < WALLPAPER_COUNT && previews[index].pixels;
}
void wallpaper_tick(void) {
  uint64_t begin = wallpaper_time();
  reap_decoders();
  service_decoder();
  if (job.busy && (job.index < 3 || job.source)) {
    while (job.row < desk.height && wallpaper_time() - begin < 2000) {
      if (job.index < 3)
        procedural_rows(spare, desk.width, desk.height, job.index, job.row, 1);
      else
        compose_rows(spare, desk.width, desk.height, job.source, job.mode,
                     wallpaper_backgrounds[job.background], job.row, 1);
      job.row++;
    }
    if (job.row == desk.height) {
      uint32_t *old = desk.wallpaper;
      desk.wallpaper = spare;
      spare = old;
      desk.wallpaper_index = job.index;
      if (job.index == WALLPAPER_CUSTOM)
        copy_text(desk.wallpaper_path, sizeof(desk.wallpaper_path), job.path);
      if (job.source) {
        active = *job.source;
        copy_text(active_path, sizeof(active_path), job.path);
      } else {
        memset(&active, 0, sizeof(active));
        active_path[0] = 0;
      }
      generated = true;
      generated_index = job.index;
      generated_mode = job.mode;
      generated_background = job.background;
      generated_width = desk.width;
      generated_height = desk.height;
      job.busy = false;
      job_commits++;
      desk.wallpaper_status[0] = 0;
      desk.material_epoch++;
      compositor_damage((Rect){0, 0, desk.width, desk.height});
      notify_settings();
      if (desk.trace)
        printf("DESKTOP WALLPAPER_READY index=%d elapsed_us=%lu time=%lu\n",
               job.index, wallpaper_time() - job.started, now_ms());
    }
  } else if (!job.busy && !decoder.pid && previews_requested) {
    for (int i = 0; i < WALLPAPER_COUNT; i++) {
      if (previews[i].pixels || preview_failed[i] ||
          (i == WALLPAPER_CUSTOM && !desk.wallpaper_path[0]))
        continue;
      if (i < 3)
        make_preview(i, NULL);
      else {
        const char *path =
            i == WALLPAPER_CUSTOM ? desk.wallpaper_path : paths[i - 3];
        if (sources[i].pixels && !strcmp(source_paths[i], path))
          make_preview(i, &sources[i]);
        else {
          int e = start_decoder(i, path, true);
          if (e)
            preview_failed[i] = true;
        }
      }
      notify_settings();
      break;
    }
  }
  tick_max_us = MAX(tick_max_us, wallpaper_time() - begin);
}

void wallpaper_draw_preview(int index, Rect r) {
  Image *image = &previews[index];
  if (!image->pixels || empty(r))
    return;
  Rect clipped = intersect(r, desk.clip);
  int dw = r.w, dh = r.h;
  if ((int64_t)image->width * r.h >= (int64_t)image->height * r.w)
    dw = ((int64_t)image->width * r.h + image->height - 1) / image->height;
  else
    dh = ((int64_t)image->height * r.w + image->width - 1) / image->width;
  int left = r.x + (r.w - dw) / 2, top = r.y + (r.h - dh) / 2;
  for (int y = clipped.y; y < clipped.y + clipped.h; y++) {
    int sy = (y - top) * image->height / dh;
    uint32_t *out = paint_pixel(clipped.x, y);
    for (int x = clipped.x; x < clipped.x + clipped.w; x++)
      *out++ = image->pixels[(size_t)sy * image->width +
                             (x - left) * image->width / dw];
  }
}

void wallpaper_cleanup(void) {
  if (decoder.pid) {
    call2(SYS_kill, decoder.pid, 9);
    close(decoder.fd);
    int status;
    while (waitpid(decoder.pid, &status, 0) == -E_INTR) {
    }
    release_image(&decoder.image);
    decoder.pid = 0;
    decoder.fd = -1;
  }
  for (unsigned i = 0; i < retired_count; i++) {
    int status;
    while (waitpid(retired_decoders[i], &status, 0) == -E_INTR) {
    }
  }
  retired_count = 0;
  for (int i = 0; i < WALLPAPER_COUNT; i++) {
    release_image(&previews[i]);
    release_image(&sources[i]);
  }
  deallocate(spare, spare_bytes);
  spare = NULL;
  spare_bytes = 0;
  memset(&active, 0, sizeof(active));
  active_path[0] = 0;
  generated = false;
  if (desk.trace)
    printf(
        "WALLPAPER_STATS requests=%lu commits=%lu cancellations=%lu "
        "decoder_starts=%lu cache_hits=%lu max_tick_us=%lu live_children=%u\n",
        job_requests, job_commits, job_cancels, decoder_starts, cache_hits,
        tick_max_us, retired_count);
}
