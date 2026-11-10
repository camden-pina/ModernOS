/* Bounded software-output metadata and core regions. No new native authority.
 * Surface coordinates have zero offset and buffer scale1 or2. A clipped
 * fixed bitmap represents every observable region point within the bound.
 * Opaque regions are hints; copying the full buffer remains conservative.
 * The protocol checker retains its original fixed region dimensions. */
struct o1_region {struct output *output;uint8_t bits[REGION_BYTES];};
#include "region-bitmap.h"
static unsigned region_width(struct output *o) {return o->checker?LOGICAL_WIDTH:REGION_WIDTH;}
static unsigned region_height(struct output *o) {return o->checker?LOGICAL_HEIGHT:REGION_HEIGHT;}
static void output_forget_surface(struct output *o) {
    for(unsigned i=0;i<OUTPUT_LIMIT;i++)o->outputs[i].entered=0;
}
static void output_surface_state(struct output *o,int mapped) {
    if(!o->surface)return;
    for(unsigned i=0;i<OUTPUT_LIMIT;i++)if(o->outputs[i].resource) {
        if(mapped && !o->outputs[i].entered)wl_surface_send_enter(o->surface,o->outputs[i].resource);
        else if(!mapped && o->outputs[i].entered)wl_surface_send_leave(o->surface,o->outputs[i].resource);
        o->outputs[i].entered=mapped;
    }
}
static void output_destroyed(struct wl_resource *r) {
    struct output *o=wl_resource_get_user_data(r);
    for(unsigned i=0;i<OUTPUT_LIMIT;i++)if(o->outputs[i].resource==r) {
        o->outputs[i].resource=NULL;o->outputs[i].entered=0;return;
    }
}
static const struct wl_output_interface output_impl={.release=destroy_resource};
static void bind_output(struct wl_client *c,void *data,uint32_t version,uint32_t id) {
    struct output *o=data;unsigned i=0;
    while(i<OUTPUT_LIMIT && o->outputs[i].resource)i++;
    if(i==OUTPUT_LIMIT){wl_client_post_implementation_error(c,"O1 output binding bound");return;}
    struct wl_resource *r=wl_resource_create(c,&wl_output_interface,version>2?2:version,id);
    if(!r){wl_client_post_no_memory(c);return;}
    o->outputs[i].resource=r;wl_resource_set_implementation(r,&output_impl,o,output_destroyed);
    /* Physical millimeters unknown; this is an owned virtual output. The
       workbench follows the desktop's 120 Hz software presentation cadence;
       other clients retain the nominal 60 Hz mode. */
    wl_output_send_geometry(r,0,0,0,0,WL_OUTPUT_SUBPIXEL_UNKNOWN,"Coast","Owned virtual output",WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r,WL_OUTPUT_MODE_CURRENT|WL_OUTPUT_MODE_PREFERRED,
                       o->electron?o->display_width:o->width,o->electron?o->display_height:o->height,o->workbench && !o->browser?120000:60000);
    if(wl_resource_get_version(r)>=2){wl_output_send_scale(r,o->scale);wl_output_send_done(r);}
    if(o->surface && o->surface_has_buffer) {
        wl_surface_send_enter(o->surface,r);o->outputs[i].entered=1;
    }
}
static int output_init(struct output *o) {
    return wl_global_create(o->display,&wl_output_interface,2,o,bind_output)?0:-1;
}
static void region_destroyed(struct wl_resource *r) {
    struct o1_region *region=wl_resource_get_user_data(r);
    if(region->output->region_count)region->output->region_count--;else region->output->fatal=1;
    free(region);
}
static void region_rect(struct wl_resource *r,int32_t x,int32_t y,int32_t width,int32_t height,int add) {
    if(width<0 || height<0){fail(r,"negative region extent");return;}
    int64_t left=x,top=y,right=(int64_t)x+width,bottom=(int64_t)y+height;
    struct o1_region *region=wl_resource_get_user_data(r);
    unsigned bound_width=region_width(region->output),bound_height=region_height(region->output);
    left=left<0?0:left>bound_width?bound_width:left;right=right<0?0:right>bound_width?bound_width:right;
    top=top<0?0:top>bound_height?bound_height:top;bottom=bottom<0?0:bottom>bound_height?bound_height:bottom;
    for(int64_t row=top;row<bottom;row++)
        region_span(region->bits,(size_t)row*bound_width+left,(size_t)row*bound_width+right,add);
}
static void region_add(struct wl_client *c,struct wl_resource *r,int32_t x,int32_t y,int32_t w,int32_t h) {
    (void)c;region_rect(r,x,y,w,h,1);
}
static void region_subtract(struct wl_client *c,struct wl_resource *r,int32_t x,int32_t y,int32_t w,int32_t h) {
    (void)c;region_rect(r,x,y,w,h,0);
}
static const struct wl_region_interface region_impl={.destroy=destroy_resource,.add=region_add,.subtract=region_subtract};
static void create_region(struct wl_client *c,struct wl_resource *parent,uint32_t id) {
    struct output *o=wl_resource_get_user_data(parent);
    if(o->region_count==REGION_LIMIT){fail(parent,"region resource bound");return;}
    struct o1_region *data=calloc(1,sizeof(*data));
    if(!data){wl_client_post_no_memory(c);return;}
    struct wl_resource *r=wl_resource_create(c,&wl_region_interface,1,id);
    if(!r){free(data);wl_client_post_no_memory(c);return;}
    data->output=o;o->region_count++;wl_resource_set_implementation(r,&region_impl,data,region_destroyed);
}
static void regions_reset(struct output *o) {
    memset(o->input_current,255,REGION_BYTES);memset(o->input_pending,255,REGION_BYTES);
    memset(o->opaque_current,0,REGION_BYTES);memset(o->opaque_pending,0,REGION_BYTES);
    o->input_changed=o->opaque_changed=0;
}
static void surface_region(struct wl_client *c,struct wl_resource *r,struct wl_resource *region,int input) {
    struct output *o=wl_resource_get_user_data(r);uint8_t *target=input?o->input_pending:o->opaque_pending;
    if(r==o->cursor_surface)return;
    if(region) {
        if(wl_resource_get_client(region)!=c || !wl_resource_instance_of(region,&wl_region_interface,&region_impl)) {
            fail(r,"region object does not belong to this client/type");return;
        }
        struct o1_region *data=wl_resource_get_user_data(region);
        if(data->output!=o){fail(r,"foreign region owner");return;}
        memcpy(target,data->bits,REGION_BYTES);
    } else memset(target,input?255:0,REGION_BYTES);
    if(input)o->input_changed=1;else o->opaque_changed=1;
}
static void surface_input_region(struct wl_client *c,struct wl_resource *r,struct wl_resource *region) {surface_region(c,r,region,1);}
static void surface_opaque_region(struct wl_client *c,struct wl_resource *r,struct wl_resource *region) {surface_region(c,r,region,0);}
static unsigned region_area(const uint8_t *bits,unsigned bytes) {
    unsigned total=0;for(unsigned i=0;i<bytes;i++){uint8_t b=bits[i];while(b){total++;b&=b-1;}}return total;
}
static uint32_t region_hash(const uint8_t *bits,unsigned bytes) {
    uint32_t h=2166136261u;for(unsigned i=0;i<bytes;i++)h=(h^bits[i])*16777619u;return h;
}
static void regions_commit(struct output *o) {
    if(o->input_changed)memcpy(o->input_current,o->input_pending,REGION_BYTES);
    if(o->opaque_changed)memcpy(o->opaque_current,o->opaque_pending,REGION_BYTES);
    o->input_changed=o->opaque_changed=0;
    if(o->checker) {
        unsigned bytes=region_width(o)*region_height(o)/8;
        printf("O1_REGION_COMMIT input=%u opaque=%u input_hash=%08x opaque_hash=%08x\n",region_area(o->input_current,bytes),region_area(o->opaque_current,bytes),region_hash(o->input_current,bytes),region_hash(o->opaque_current,bytes));
    }
}
