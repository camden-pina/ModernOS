#ifndef _VIDEO_FBCON_H
#define _VIDEO_FBCON_H

#include <boot/info.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <uapi/abi.h>

void fbcon_init(struct boot_framebuffer *fb);
void fbcon_clear(void);
void fbcon_putc(char c);
bool fbcon_display_info(struct os_display_info *info);
void fbcon_graphics(bool enabled);
void fbcon_present(const struct os_display_rect *rect, const uint32_t *pixels);
struct address_space;
bool fbcon_present_batch(const struct address_space *space, const struct os_display_batch *batch);
bool fbcon_upload_batch(const struct address_space *space, const struct os_display_batch *batch,
                        void *target);
void fbcon_winsize(struct os_winsize *size);

#endif /* _VIDEO_FBCON_H */
