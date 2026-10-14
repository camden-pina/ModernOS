#define _GNU_SOURCE
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <webp/encode.h>
#include <webp/decode.h>
#include <jpeglib.h>
#include <libxml/parser.h>
#include <sqlite3.h>
#include <gcrypt.h>
#include <libpsl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL webcore-deps-check:%d: %s\n",__LINE__,#x); exit(1); } } while (0)

static void fonts(void) {
    const char *path = "/usr/share/fonts/TTF/DejaVuSans.ttf";
    FcConfig *config = FcConfigCreate();
    CHECK(config);
    CHECK(FcConfigAppFontAddFile(config,(const FcChar8 *)path));
    FcPattern *pattern = FcNameParse((const FcChar8 *)"DejaVu Sans:style=Book");
    CHECK(pattern);
    CHECK(FcConfigSubstitute(config,pattern,FcMatchPattern));
    FcDefaultSubstitute(pattern);
    FcResult result;
    FcPattern *match = FcFontMatch(config, pattern, &result);
    CHECK(match && result==FcResultMatch);
    FcChar8 *file;
    CHECK(FcPatternGetString(match,FC_FILE,0,&file)==FcResultMatch && !strcmp((char *)file,path));
    FT_Library library;
    FT_Face face;
    CHECK(!FT_Init_FreeType(&library));
    CHECK(!FT_New_Face(library,(const char *)file,0,&face) && !FT_Set_Pixel_Sizes(face,0,24));
    hb_font_t *font = hb_ft_font_create_referenced(face);
    hb_buffer_t *buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, "office", -1, 0, -1);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(font, buffer, NULL, 0);
    unsigned count;
    hb_glyph_info_t *glyph = hb_buffer_get_glyph_infos(buffer, &count);
    hb_glyph_position_t *position = hb_buffer_get_glyph_positions(buffer, NULL);
    CHECK(count>0 && count<6);
    long ink = 0, advance = 0;
    for (unsigned i = 0; i < count; i++) {
        CHECK(glyph[i].codepoint && !FT_Load_Glyph(face,glyph[i].codepoint,FT_LOAD_RENDER));
        advance += position[i].x_advance;
        for (unsigned y = 0; y < face->glyph->bitmap.rows; y++)
            for (unsigned x = 0; x < face->glyph->bitmap.width; x++)
                ink += face->glyph->bitmap.buffer[y * face->glyph->bitmap.pitch + x] != 0;
    }

    CHECK(advance>0 && ink>100);
    hb_buffer_clear_contents(buffer);
    hb_buffer_add_utf8(buffer, "\xd8\xa7\xd9\x84\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85", -1, 0, -1);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(font, buffer, NULL, 0);
    CHECK(hb_buffer_get_direction(buffer)==HB_DIRECTION_RTL);
    glyph = hb_buffer_get_glyph_infos(buffer, &count);
    CHECK(count>=3 && count<6);
    for (unsigned i = 0; i < count; i++)
        CHECK(glyph[i].codepoint && (!i || glyph[i-1].cluster>glyph[i].cluster));
    hb_buffer_destroy(buffer);
    hb_font_destroy(font);
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    FcPatternDestroy(match);
    FcPatternDestroy(pattern);
    FcConfigDestroy(config);
    puts("PASS Fontconfig matching, HarfBuzz ligatures/Arabic RTL and real FreeType raster ink");
}

static void images(void) {
    unsigned char rgba[16 * 16 * 4];
    for (unsigned i = 0; i < sizeof(rgba); i++)
        rgba[i] = (unsigned char)(i * 31 + 7);
    for (unsigned i = 3; i < sizeof(rgba); i += 4)
        rgba[i] = 255;
    uint8_t *encoded = NULL;
    size_t length = WebPEncodeLosslessRGBA(rgba, 16, 16, 16 * 4, &encoded);
    CHECK(length>0);
    int width, height;
    uint8_t *decoded = WebPDecodeRGBA(encoded, length, &width, &height);
    CHECK(decoded && width==16 && height==16 && !memcmp(decoded,rgba,sizeof(rgba)));
    WebPFree(decoded);
    WebPFree(encoded);
    struct jpeg_compress_struct c = {0};
    struct jpeg_error_mgr error;
    c.err = jpeg_std_error(&error);
    jpeg_create_compress(&c);
    unsigned char *jpeg = NULL;
    unsigned long size = 0;
    jpeg_mem_dest(&c, &jpeg, &size);
    c.image_width = 16;
    c.image_height = 16;
    c.input_components = 4;
    c.in_color_space = JCS_EXT_RGBA;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 95, TRUE);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < 16) {
        JSAMPROW row = rgba + c.next_scanline * 64;
        CHECK(jpeg_write_scanlines(&c,&row,1)==1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    CHECK(size>0);
    struct jpeg_decompress_struct d = {0};
    d.err = jpeg_std_error(&error);
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, jpeg, size);
    CHECK(jpeg_read_header(&d,TRUE)==JPEG_HEADER_OK);
    d.out_color_space = JCS_EXT_BGRA;
    CHECK(jpeg_start_decompress(&d));
    JDIMENSION left = 0, w = 16;
    jpeg_crop_scanline(&d, &left, &w);
    CHECK(w==16 && d.output_components==4);
    CHECK(jpeg_skip_scanlines(&d,2)==2);
    unsigned char line[64];
    while (d.output_scanline < d.output_height) {
        JSAMPROW row = line;
        CHECK(jpeg_read_scanlines(&d,&row,1)==1);
    }
    CHECK(jpeg_finish_decompress(&d));
    jpeg_destroy_decompress(&d);
    free(jpeg);
    puts("PASS genuine WebP lossless pixels and libjpeg-turbo BGRA/crop/skip decoding");
}

static void data(void) {
    const char xml[] = "<root><item id='7'>actual &amp; parsed</item></root>";
    xmlDocPtr doc = xmlReadMemory(xml, strlen(xml), "fixture.xml", NULL, XML_PARSE_NONET);
    CHECK(doc);
    xmlNode *item = xmlDocGetRootElement(doc)->children;
    xmlChar *value = xmlNodeGetContent(item);
    CHECK(value && !strcmp((char *)value,"actual & parsed"));
    xmlFree(value);
    xmlFreeDoc(doc);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(":memory:",&db)==SQLITE_OK);
    CHECK(sqlite3_exec(db,"CREATE TABLE t(v INTEGER UNIQUE);INSERT INTO t VALUES(11);BEGIN;INSERT INTO t VALUES(22);ROLLBACK;",NULL,NULL,NULL)==SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    CHECK(sqlite3_prepare_v2(db,"SELECT sum(v),count(*) FROM t",-1,&stmt,NULL)==SQLITE_OK);
    CHECK(sqlite3_step(stmt)==SQLITE_ROW && sqlite3_column_int(stmt,0)==11 && sqlite3_column_int(stmt,1)==1);
    CHECK(sqlite3_finalize(stmt)==SQLITE_OK && sqlite3_close(db)==SQLITE_OK);
    CHECK(!strcmp(psl_registrable_domain(psl_builtin(),"a.b.example.co.uk"),"example.co.uk"));
    CHECK(psl_is_public_suffix(psl_builtin(),"co.uk"));
    CHECK(gcry_check_version(NULL));
    gcry_control(GCRYCTL_DISABLE_SECMEM, 0);
    gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
    unsigned char digest[32];
    static const unsigned char expected[] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
                                             0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                             0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
                                             0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    gcry_md_hash_buffer(GCRY_MD_SHA256, digest, "abc", 3);
    CHECK(!memcmp(digest,expected,32));
    gcry_cipher_hd_t cipher;
    unsigned char zero[16] = {0}, out[16], tag[16];
    static const unsigned char ciphertext[] = {0x03, 0x88, 0xda, 0xce, 0x60, 0xb6, 0xa3, 0x92,
                                               0xf3, 0x28, 0xc2, 0xb9, 0x71, 0xb2, 0xfe, 0x78};
    static const unsigned char expected_tag[] = {0xab, 0x6e, 0x47, 0xd4, 0x2c, 0xec, 0x13, 0xbd,
                                                 0xf5, 0x3a, 0x67, 0xb2, 0x12, 0x57, 0xbd, 0xdf};
    CHECK(!gcry_cipher_open(&cipher,GCRY_CIPHER_AES128,GCRY_CIPHER_MODE_GCM,0));
    CHECK(!gcry_cipher_setkey(cipher,zero,16) && !gcry_cipher_setiv(cipher,zero,12));
    CHECK(!gcry_cipher_encrypt(cipher,out,16,zero,16) && !gcry_cipher_gettag(cipher,tag,16));
    CHECK(!memcmp(out,ciphertext,16) && !memcmp(tag,expected_tag,16));
    gcry_cipher_close(cipher);
    puts("PASS real XML, SQLite rollback, public suffixes and SHA256/AES-GCM vectors");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    fonts();
    images();
    data();
    puts("WEBCORE DEPENDENCIES CHECK PASS");
    return 0;
}
