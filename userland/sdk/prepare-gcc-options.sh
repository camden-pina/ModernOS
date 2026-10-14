#!/bin/sh
# Invalidate derived GCC tables when their real option generator changes.
set -eu
[ "$#" = 3 ] || {
    printf 'Usage: prepare-gcc-options BUILD_DIR TARGET AWK\n' >&2;
    exit 2;
}

case "$1" in
    /*) ;;
    *)
        printf 'BUILD_DIR must be absolute\n' >&2;
        exit 2
        ;;
esac

case "$2" in
    ''|*[!a-zA-Z0-9_-]*)
        printf 'Invalid target directory\n' >&2;
        exit 2
        ;;
esac

cd "$1"
target=$2
awk=$3
awk_key=$(sha256sum "$(command -v "$awk")")
awk_key=gnu-awk-options-v1-${awk_key%% *}
if [ ! -f .modernos-option-generator ] || [ "$(cat .modernos-option-generator)" != "$awk_key" ]; then
    rm -f gcc/optionlist gcc/options.h gcc/options.cc gcc/options-save.cc \
        gcc/options-urls.cc gcc/s-options gcc/s-options-h
    rm -rf "$target/libgcc" "$target/libstdc++-v3"
    printf '%s\n' "$awk_key" > .modernos-option-generator
fi
