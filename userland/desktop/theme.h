#ifndef MODERNOS_DESKTOP_THEME_H
#define MODERNOS_DESKTOP_THEME_H
#include <stdint.h>

uint32_t theme_color(uint32_t light);
/* Shared desktop tokens. Accent overrides are owned by Settings. */
enum {
    THEME_WALLPAPER_DARK = 0x293A53,
    THEME_WALLPAPER_MID = 0x30445C,
    THEME_WALLPAPER_TEAL = 0x29434C,
    THEME_WALLPAPER_GLOW = 0x506D77,
    THEME_TASKBAR = 0x202E3B,
    THEME_WINDOW = 0xFFFFFF,
    THEME_TOOLBAR = 0xF9FAF9,
    THEME_SIDEBAR = 0xF5F7F6,
    THEME_ACCENT = 0x347966,
    THEME_ACCENT_HOVER = 0x286957,
    THEME_SELECTION = 0xDCEBE6,
    THEME_SELECTION_TEXT = 0x286B58,
    THEME_PRIMARY_TEXT = 0x2D3D44,
    THEME_TEXT = 0x3C4B50,
    THEME_SECONDARY_TEXT = 0x8E9D9F,
    THEME_MUTED_TEXT = 0xA3ADAF,
    THEME_BORDER = 0xE8ECEB,
    THEME_DIVIDER = 0xEDF0EF,
    THEME_ACTIVE = 0x8BC8A8,
    THEME_WINDOW_BORDER = 0xDBE3E3,
    THEME_SHADOW = 0x121E2F
};

/* Semantic colors remain exact in light mode; one shared palette owns dark mode. */
#define THEME_WINDOW theme_color(0xFFFFFF)
#define THEME_TOOLBAR theme_color(0xF9FAF9)
#define THEME_SIDEBAR theme_color(0xF5F7F6)
#define THEME_PRIMARY_TEXT theme_color(0x2D3D44)
#define THEME_TEXT theme_color(0x3C4B50)
#define THEME_SECONDARY_TEXT theme_color(0x8E9D9F)
#define THEME_MUTED_TEXT theme_color(0xA3ADAF)
#define THEME_BORDER theme_color(0xE8ECEB)
#define THEME_DIVIDER theme_color(0xEDF0EF)
#define THEME_WINDOW_BORDER theme_color(0xDBE3E3)
#define THEME_SHADOW theme_color(0x121E2F)
#define THEME_ON_ACCENT 0xFFFFFF
#endif
