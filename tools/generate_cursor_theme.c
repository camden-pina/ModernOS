/* Export the desktop's own antialiased paths as standard Wayland Xcursors. */
#include "../userland/desktop/cursor.c"
Desktop desk;
int S(int n) { return (n*desk.scale+50)/100; }
static unsigned integer(const char *s) { unsigned n=0;while(*s>='0'&&*s<='9')n=n*10+(*s++-'0');return n; }
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    unsigned kind=integer(argv[1]),scale=integer(argv[2]);
    if(kind>=C_NATIVE || (scale!=100 && scale!=200))return 2;
    desk.scale=scale;desk.scale_index=scale==200?3:0;
    Cursor *c=prepare(kind);
    uint32_t header[]={36,0xfffd0002,24*scale/100,1,c->width,c->height,c->hot_x,c->hot_y,0};
    if(write(1,header,sizeof(header))!=(long)sizeof(header))return 1;
    for(int y=0;y<c->height;y++)for(int x=0;x<c->width;x++) {
        unsigned a=c->outer[y*64+x],b=c->inner[y*64+x];
        /* Same white foreground and charcoal outline as the native pointer,
           premultiplied for the Wayland cursor surface. */
        unsigned alpha=b+(a*(255-b)+127)/255;
        unsigned channel=(245*b+28*a*(255-b)/255+127)/255;
        uint32_t pixel=(alpha<<24)|(channel<<16)|(channel<<8)|channel;
        if(write(1,&pixel,4)!=4)return 1;
    }
    return 0;
}
