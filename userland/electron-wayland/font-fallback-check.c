#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <sys/utsname.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
int main(int argc,char **argv) {
    struct utsname os;if(uname(&os)||strcmp(os.sysname,"Coast"))return 77;
    if(getuid()!=1000||!FcInit())return 1;
    const unsigned chars[]={0x41,0x3ba,0x65e5,0x672c,0x2615};
    FT_Library library;if(FT_Init_FreeType(&library))return 2;
    int failed=0;
    int bulk=argc==2&&!strcmp(argv[1],"--bulk");
    for(unsigned i=0;i<sizeof(chars)/sizeof(chars[0]);i++) {
        FcPattern *query=FcPatternCreate();FcCharSet *charset=FcCharSetCreate();FcCharSetAddChar(charset,chars[i]);
        FcPatternAddString(query,FC_FAMILY,(const FcChar8*)"monospace");FcPatternAddCharSet(query,FC_CHARSET,charset);
        FcConfigSubstitute(NULL,query,FcMatchPattern);FcDefaultSubstitute(query);
        FcResult result;FcPattern *match=FcFontMatch(NULL,query,&result);FcChar8 *file=NULL,*family=NULL;int index=0;FcCharSet *coverage=NULL;
        int covered=match&&FcPatternGetCharSet(match,FC_CHARSET,0,&coverage)==FcResultMatch&&FcCharSetHasChar(coverage,chars[i]);
        if(match){FcPatternGetString(match,FC_FILE,0,&file);FcPatternGetString(match,FC_FAMILY,0,&family);FcPatternGetInteger(match,FC_INDEX,0,&index);}
        FT_Face face=NULL;int error=file?FT_New_Face(library,(const char*)file,index,&face):-1;unsigned glyph=0;
        if(!error){glyph=FT_Get_Char_Index(face,chars[i]);error=FT_Set_Pixel_Sizes(face,0,18);if(error&&face->num_fixed_sizes)error=FT_Select_Size(face,0);if(!error)error=FT_Load_Char(face,chars[i],FT_LOAD_RENDER|FT_LOAD_COLOR);}
        printf("FONT_FALLBACK U+%04X family=%s file=%s index=%d coverage=%d glyph=%u raster_error=%d\n",chars[i],family?(char*)family:"missing",file?(char*)file:"missing",index,covered,glyph,error);
        if(bulk&&file) {
            FILE *stream=fopen((const char*)file,"rb");long size=0;size_t got=0;int read_error=0;
            if(stream&&!fseek(stream,0,SEEK_END)&&(size=ftell(stream))>0&&size<=128*1024*1024) {
                rewind(stream);void *data=malloc(size);
                if(data){errno=0;got=fread(data,1,size,stream);read_error=errno;free(data);}
            }
            printf("FONT_BULK_READ file=%s wanted=%ld got=%zu errno=%d\n",file,size,got,read_error);
            failed|=size<=0||got!=(size_t)size;
            if(stream)fclose(stream);
        }
        failed|=!covered||!glyph||error;
        if(face)FT_Done_Face(face);
        if(match)FcPatternDestroy(match);
        FcPatternDestroy(query);FcCharSetDestroy(charset);
    }
    FT_Done_FreeType(library);FcFini();
    if(!failed)puts("FONT_FALLBACK_PASS Greek CJK coffee actual_fontconfig_freetype_raster");
    return failed;
}
