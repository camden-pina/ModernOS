#ifndef COAST_SURFACE_DAMAGE_H
#define COAST_SURFACE_DAMAGE_H
#include <stdint.h>

struct surface_damage { int64_t x0,y0,x1,y1; int set; };
struct pixel_rect { unsigned x,y,width,height; };
static inline void surface_damage_add(struct surface_damage *d,int32_t x,int32_t y,
                                      int32_t width,int32_t height) {
    if(width<=0 || height<=0)return;
    int64_t x1=(int64_t)x+width,y1=(int64_t)y+height;
    if(!d->set){*d=(struct surface_damage){x,y,x1,y1,1};return;}
    if(x<d->x0)d->x0=x;
    if(y<d->y0)d->y0=y;
    if(x1>d->x1)d->x1=x1;
    if(y1>d->y1)d->y1=y1;
}
/* wl_surface v3 damage is surface-local. Interpret it only at commit, using
 * that commit's buffer scale, shadow offset and clipped physical viewport. */
static inline struct pixel_rect surface_damage_pixels(struct surface_damage d,int scale,
        int left,int top,unsigned width,unsigned height) {
    if(!d.set)return (struct pixel_rect){0};
    int64_t x0=d.x0*scale+left,y0=d.y0*scale+top;
    int64_t x1=d.x1*scale+left,y1=d.y1*scale+top;
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>width)x1=width;
    if(y1>height)y1=height;
    if(x1<=x0 || y1<=y0)return (struct pixel_rect){0};
    return (struct pixel_rect){x0,y0,x1-x0,y1-y0};
}
#endif
