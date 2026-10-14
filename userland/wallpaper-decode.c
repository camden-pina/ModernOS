/* Reuse the shipped static image libraries without linking them into the
   freestanding desktop. No shell, network, or display access is needed. */
#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <png.h>
#include <jpeglib.h>
#include "include/wallpaper_protocol.h"

static int dimensions(uint32_t w, uint32_t h) {
    return w && h && w <= WALLPAPER_MAX_EDGE && h <= WALLPAPER_MAX_EDGE &&
           (uint64_t)w * h <= WALLPAPER_MAX_PIXELS;
}

static uint32_t argb(const unsigned char *rgb, unsigned alpha) {
    return (alpha << 24) | ((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | rgb[2];
}

static int decode_png(FILE *source, struct wallpaper_packet *packet, uint32_t **out) {
    png_image image = {.version = PNG_IMAGE_VERSION};
    if (!png_image_begin_read_from_stdio(&image, source)) {
        png_image_free(&image);
        return -EINVAL;
    }
    int error = -EINVAL;
    unsigned char *pixels = NULL;
    if (!dimensions(image.width, image.height))
        goto done;
    packet->width = image.width;
    packet->height = image.height;
    image.format = PNG_FORMAT_RGBA;
    size_t count = (size_t)image.width * image.height;
    pixels = malloc(count * 4);
    if (!pixels) {
        error = -ENOMEM;
        goto done;
    }
    if (!png_image_finish_read(&image, NULL, pixels, 0, NULL))
        goto done;
    for (size_t i = 0; i < count; i++) {
        uint32_t color = argb(pixels + i * 4, pixels[i * 4 + 3]);
        memcpy(pixels + i * 4, &color, 4);
    }
    *out = (uint32_t *)pixels;
    pixels = NULL;
    error = 0;
done:
    free(pixels);
    png_image_free(&image);
    return error;
}

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
    int warnings;
} JpegError;

typedef struct {
    struct jpeg_decompress_struct image;
    JpegError error;
    int created;
    uint32_t *pixels;
    unsigned char *row;
} Jpeg;

static void jpeg_failed(j_common_ptr image) {
    JpegError *error = (JpegError *)image->err;
    longjmp(error->jump, 1);
}

static void jpeg_message(j_common_ptr image, int level) {
    if (level < 0)
        ((JpegError *)image->err)->warnings++;
}

static int decode_jpeg(FILE *source, struct wallpaper_packet *packet, uint32_t **out) {
    /* Heap state remains valid after libjpeg's error longjmp. */
    Jpeg *state = calloc(1, sizeof(*state));
    if (!state)
        return -ENOMEM;
    state->image.err = jpeg_std_error(&state->error.base);
    state->error.base.error_exit = jpeg_failed;
    state->error.base.emit_message = jpeg_message;
    int result = -EINVAL;
    if (setjmp(state->error.jump))
        goto done;
    state->created = 1;
    jpeg_create_decompress(&state->image);
    jpeg_stdio_src(&state->image, source);
    if (jpeg_read_header(&state->image, TRUE) != JPEG_HEADER_OK ||
        !dimensions(state->image.image_width, state->image.image_height))
        goto done;
    state->image.out_color_space = JCS_RGB;
    jpeg_start_decompress(&state->image);
    packet->width = state->image.output_width;
    packet->height = state->image.output_height;
    if (state->image.output_components != 3 || !dimensions(packet->width, packet->height))
        goto done;
    state->pixels = malloc((size_t)packet->width * packet->height * 4);
    state->row = malloc((size_t)packet->width * 3);
    if (!state->pixels || !state->row) {
        result = -ENOMEM;
        goto done;
    }
    while (state->image.output_scanline < packet->height) {
        uint32_t y = state->image.output_scanline;
        JSAMPROW row = state->row;
        if (jpeg_read_scanlines(&state->image, &row, 1) != 1)
            goto done;
        for (uint32_t x = 0; x < packet->width; x++)
            state->pixels[(size_t)y * packet->width + x] = argb(row + x * 3, 255);
    }

    jpeg_finish_decompress(&state->image);
    if (state->error.warnings)
        goto done;
    *out = state->pixels;
    state->pixels = NULL;
    result = 0;
done:
    if (state->created)
        jpeg_destroy_decompress(&state->image);
    free(state->pixels);
    free(state->row);
    free(state);
    return result;
}

static uint32_t little32(const unsigned char *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static unsigned little16(const unsigned char *p) {
    return p[0] | ((unsigned)p[1] << 8);
}

static int decode_bmp(FILE *source, off_t size, struct wallpaper_packet *packet, uint32_t **out) {
    unsigned char header[54];
    if (fread(header, 1, sizeof(header), source) != sizeof(header))
        return -EINVAL;
    uint32_t offset = little32(header + 10), dib = little32(header + 14);
    int32_t w = (int32_t)little32(header + 18), h = (int32_t)little32(header + 22);
    unsigned bits = little16(header + 28);
    if (dib < 40 || dib > 124 || offset < 14 + dib || w <= 0 || h == INT32_MIN ||
        little16(header + 26) != 1 || (bits != 24 && bits != 32) || little32(header + 30))
        return -EINVAL;
    uint32_t height = h < 0 ? -h : h;
    if (!dimensions(w, height))
        return -EINVAL;
    size_t stride = ((size_t)w * (bits / 8) + 3) & ~(size_t)3;
    if ((uint64_t)offset + stride * height > (uint64_t)size || fseek(source, offset, SEEK_SET))
        return -EINVAL;
    uint32_t *pixels = malloc((size_t)w * height * 4);
    unsigned char *row = malloc(stride);
    int result = -ENOMEM;
    if (!pixels || !row)
        goto done;
    result = -EINVAL;
    for (uint32_t y = 0; y < height; y++) {
        if (fread(row, 1, stride, source) != stride)
            goto done;
        uint32_t target_y = h < 0 ? y : height - y - 1;
        for (int32_t x = 0; x < w; x++) {
            unsigned char *p = row + x * (bits / 8);
            /* BI_RGB's fourth byte is reserved, not an alpha channel. */
            pixels[(size_t)target_y * w + x] =
                0xff000000U | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
        }
    }

    packet->width = w;
    packet->height = height;
    *out = pixels;
    pixels = NULL;
    result = 0;
done:
    free(pixels);
    free(row);
    return result;
}

static int send_all(const void *data, size_t bytes) {
    const unsigned char *p = data;
    while (bytes) {
        ssize_t n = write(STDOUT_FILENO, p, bytes);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        bytes -= n;
    }

    return 0;
}

int main(int argc, char **argv) {
    struct wallpaper_packet packet = {.magic = WALLPAPER_MAGIC, .error = -EINVAL};
    uint32_t *pixels = NULL;
    FILE *source = NULL;
    if (argc != 2)
        goto done;
    source = fopen(argv[1], "rb");
    if (!source) {
        packet.error = -errno;
        goto done;
    }
    struct stat st;
    if (fstat(fileno(source), &st)) {
        packet.error = -errno;
        goto done;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 8 || st.st_size > WALLPAPER_MAX_FILE)
        goto done;
    unsigned char signature[8];
    if (fread(signature, 1, sizeof(signature), source) != sizeof(signature))
        goto done;
    rewind(source);
    if (!png_sig_cmp(signature, 0, sizeof(signature)))
        packet.error = decode_png(source, &packet, &pixels);
    else if (signature[0] == 0xff && signature[1] == 0xd8)
        packet.error = decode_jpeg(source, &packet, &pixels);
    else if (signature[0] == 'B' && signature[1] == 'M')
        packet.error = decode_bmp(source, st.st_size, &packet, &pixels);
done:
    if (source)
        fclose(source);
    if (packet.error)
        packet.width = packet.height = 0;
    int failed = send_all(&packet, sizeof(packet));
    if (!failed && !packet.error)
        failed = send_all(pixels, (size_t)packet.width * packet.height * 4);
    free(pixels);
    return failed || packet.error ? 1 : 0;
}
