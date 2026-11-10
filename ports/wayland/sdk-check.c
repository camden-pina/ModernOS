#include <stdio.h>
#include <wayland-server.h>

int main(void)
{
    struct wl_display *display = wl_display_create();
    if (!display)
        return 1;
    wl_display_destroy(display);
    puts("COAST_WAYLAND_SDK_PASS 1.21.0");
    return 0;
}
