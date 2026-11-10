#define PCRE2_CODE_UNIT_WIDTH 8
#include <glib.h>
#include <pixman.h>
#include <zlib.h>
#include <pcre2.h>
#include <ffi.h>
#include <libslirp.h>
#include <libucontext/libucontext.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int condition) {
    if (!condition) {
        fputs("QEMU_SDK_FAIL\n", stderr);
        exit(1);
    }
}

static int multiply(int left, int right) { return left * right; }

int main(void) {
    GHashTable *table = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(table, "coast", GINT_TO_POINTER(42));
    require(GPOINTER_TO_INT(g_hash_table_lookup(table, "coast")) == 42);
    g_hash_table_destroy(table);

    int error;
    PCRE2_SIZE offset;
    pcre2_code *regex = pcre2_compile((PCRE2_SPTR)"[A-Z]+", PCRE2_ZERO_TERMINATED,
                                     0, &error, &offset, NULL);
    require(regex != NULL);
    pcre2_match_data *match = pcre2_match_data_create_from_pattern(regex, NULL);
    require(pcre2_match(regex, (PCRE2_SPTR)"COAST42", 7, 0, 0, match, NULL) == 1);
    require(pcre2_get_ovector_pointer(match)[1] == 5);
    pcre2_match_data_free(match);
    pcre2_code_free(regex);

    pixman_image_t *source = pixman_image_create_bits(PIXMAN_a8r8g8b8, 2, 2, NULL, 0);
    pixman_image_t *dest = pixman_image_create_bits(PIXMAN_a8r8g8b8, 2, 2, NULL, 0);
    require(source && dest);
    for (int i = 0; i < 4; i++) pixman_image_get_data(source)[i] = 0xff123456;
    pixman_image_composite32(PIXMAN_OP_SRC, source, NULL, dest, 0, 0, 0, 0, 0, 0, 2, 2);
    for (int i = 0; i < 4; i++) require(pixman_image_get_data(dest)[i] == 0xff123456);
    pixman_image_unref(source);
    pixman_image_unref(dest);

    unsigned char compressed[128], restored[128];
    const char data[] = "Coast static QEMU dependency SDK";
    uLongf compressed_size = sizeof compressed, restored_size = sizeof restored;
    require(compress2(compressed, &compressed_size, (const Bytef *)data, sizeof data, 6) == Z_OK);
    require(uncompress(restored, &restored_size, compressed, compressed_size) == Z_OK);
    require(restored_size == sizeof data && memcmp(data, restored, sizeof data) == 0);

    ffi_cif cif;
    ffi_type *types[] = {&ffi_type_sint, &ffi_type_sint};
    int left = 6, right = 7;
    ffi_arg answer = 0;
    void *values[] = {&left, &right};
    require(ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2, &ffi_type_sint, types) == FFI_OK);
    ffi_call(&cif, FFI_FN(multiply), &answer, values);
    require(answer == 42);
    require(strcmp(slirp_version_string(), "4.9.3") == 0);
    libucontext_ucontext_t context;
    require(libucontext_getcontext(&context) == 0);
    puts("QEMU_SDK_PASS glib pcre2 pixman zlib ffi slirp ucontext");
    return 0;
}
