#!/bin/sh
# Native static C/C++ toolchain rebuild. All commands execute inside Coast.
set -eu
export PATH=/usr/bin:/bin SHELL=/bin/sh CONFIG_SHELL=/bin/sh LC_ALL=C

usage() {
    printf '%s\n' 'Usage: bootstrap-toolchain [--work DIR] [--prefix DIR] [-j JOBS] [--prepare-only] [--posix-threads]' \
        'Extract bundled sources, build binutils/GCC/libgcc/libstdc++, and install' \
        'a separate static toolchain. Defaults: /tmp/toolchain, WORK/install, -j1.' \
        'Use a RAM workspace with sufficient memory (8 GiB recommended).' \
        'Re-running resumes completed extraction/configuration/build stages.' \
        '--prepare-only verifies/extracts sources without configuring or compiling.' \
        '--posix-threads enables C++ threads, mutexes and condition variables.'
}

work=/tmp/toolchain
prefix=
jobs=1
prepare_only=no
thread_model=single

while [ "$#" -gt 0 ]; do
    case "$1" in
        --help|-h)
            usage;
            exit 0
            ;;
        --work|--prefix|-j)
            [ "$#" -ge 2 ] || {
                usage >&2;
                exit 2;
            }
            case "$1" in
                --work) work=$2 ;;
                --prefix) prefix=$2 ;;
                -j) jobs=$2 ;;
            esac
            shift 2
            ;;
        --prepare-only)
            prepare_only=yes;
            shift
            ;;
        --posix-threads)
            thread_model=posix;
            shift
            ;;
        -j[0-9]*)
            jobs=${1#-j};
            shift
            ;;
        *)
            printf 'Unknown option: %s\n' "$1" >&2;
            usage >&2;
            exit 2
            ;;
    esac
done

case "$jobs" in
    ''|*[!0-9]*|0)
        printf 'JOBS must be a positive integer\n' >&2;
        exit 2
        ;;
esac

case "$work" in
    /*) ;;
    *)
        printf 'WORK must be an absolute path\n' >&2;
        exit 2
        ;;
esac
case "$work" in
    *[[:space:]]*)
        printf 'WORK must not contain whitespace\n' >&2;
        exit 2
        ;;
esac

[ -n "$prefix" ] || prefix=$work/install
case "$prefix" in
    /*) ;;
    *)
        printf 'PREFIX must be an absolute path\n' >&2;
        exit 2
        ;;
esac
case "$prefix" in
    /|/usr|/usr/|/bin|/bin/)
        printf 'Choose a separate installation prefix\n' >&2;
        exit 2
        ;;
esac

mkdir -p "$work/src" "$work/build" "$prefix"
work=$(cd "$work" && pwd)
prefix=$(cd "$prefix" && pwd)

case "$prefix" in
    /|/usr|/bin)
        printf 'Choose a separate installation prefix\n' >&2;
        exit 2
        ;;
esac
case "$prefix" in
    *[[:space:]]*)
        printf 'PREFIX must not contain whitespace\n' >&2;
        exit 2
        ;;
esac
case "$work" in
    /mnt/*)
        printf 'Build in RAM: writable FAT lacks source-tree symlinks/replacement\n' >&2;
        exit 2
        ;;
esac

bundle=/usr/src/coast
original_sources=no
if [ -f /usr/src/coast/distfiles/gcc-14.2.0.tar.gz ]; then
    bundle=/usr/src/coast/distfiles
    original_sources=yes
    cat > "$work/original-SHA256SUMS" <<'COAST_ORIGINAL_HASHES'
7d376d445f93126dc545e2c0086d0f647c3094aae081cdb78f42ce2bc25e7293  gcc-14.2.0.tar.gz
c44968b97cd86499efbc4b4ab7d98471f673e5414c554ef54afa930062dbbfcb  binutils-2.37.tar.gz
fd4829912cddd12f84181c3451cc752be224643e87fac497b69edddadc49b4f2  gmp-6.2.1.tar.xz
0c98a3f1732ff6ca4ea690552079da9c597872d30e96ec28414ee23c95558a7f  mpfr-4.1.0.tar.xz
17503d2c395dfcf106b622dc142683c1199431d095367c6aacba6eec30340459  mpc-1.2.1.tar.gz
a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4  musl-1.2.5.tar.gz
dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3  make-4.4.1.tar.gz
3dd430f0cd3b4428c6c3f6afc021b9cd3c1f8c93f7a688dc268ca428a90b4ac1  gawk-5.4.0.tar.xz
COAST_ORIGINAL_HASHES
    (cd "$bundle" && sha256sum -c "$work/original-SHA256SUMS")
else
    (cd "$bundle" && sha256sum -c SHA256SUMS)
fi

for package in gcc-14.2.0 binutils-2.37 gmp-6.2.1 mpfr-4.1.0 mpc-1.2.1 musl-1.2.5 make-4.4.1 gawk-5.4.0; do
    if [ ! -f "$work/src/$package/.coast-extracted" ]; then
        printf 'Extracting %s\n' "$package"
        rm -rf "$work/src/$package"
        archive=$package.tar.xz
        if [ "$original_sources" = yes ]; then
            case "$package" in
                gcc-14.2.0) archive=gcc-14.2.0.tar.gz ;;
                binutils-2.37) archive=binutils-2.37.tar.gz ;;
                gmp-6.2.1) archive=gmp-6.2.1.tar.xz ;;
                mpfr-4.1.0) archive=mpfr-4.1.0.tar.xz ;;
                mpc-1.2.1) archive=mpc-1.2.1.tar.gz ;;
                musl-1.2.5) archive=musl-1.2.5.tar.gz ;;
                make-4.4.1) archive=make-4.4.1.tar.gz ;;
                gawk-5.4.0) archive=gawk-5.4.0.tar.xz ;;
            esac
        fi
        case "$archive" in
            *.tar.gz) tar -xzf "$bundle/$archive" --no-same-owner -C "$work/src" ;;
            *.tar.xz) tar -xJf "$bundle/$archive" --no-same-owner -C "$work/src" ;;
        esac
        touch "$work/src/$package/.coast-extracted"
    fi
done

for name in gmp mpfr mpc; do
    case "$name" in
        gmp) version=6.2.1 ;;
        mpfr) version=4.1.0 ;;
        mpc) version=1.2.1 ;;
    esac
    [ -L "$work/src/gcc-14.2.0/$name" ] || ln -s "../$name-$version" "$work/src/gcc-14.2.0/$name"
done

[ "$prepare_only" = no ] || {
    printf 'Verified source workspace ready: %s/src\n' "$work";
    exit 0;
}

# GCC's option generator relies on GNU awk replacement escaping. BusyBox awk
# can silently omit every C++ language flag while exiting successfully.
export AWK=gawk
"$AWK" 'BEGIN { s="C++"; gsub("\\+", "\\+", s); if (!(" C++ " ~ (" " s " "))) exit 1 }' || {
    printf 'GNU awk is required for correct GCC option generation\n' >&2;
    exit 1;
}

stage() {
    printf '%s\n' "$1" > "$work/.coast-stage";
}

stage sources
flags='-O2 -g0 -fno-pie -fno-stack-protector -mno-avx -mno-avx2'
export CC=gcc CXX=g++ AR=ar RANLIB=ranlib NM=nm MAKEINFO=true
export CFLAGS="$flags -std=gnu11" CXXFLAGS="$flags -std=gnu++11" LDFLAGS='-static -no-pie'

# Host compiler flags must not override the individual libstdc++ source
# directories' C++98/11/17/20 modes.
export CFLAGS_FOR_TARGET="$flags" CXXFLAGS_FOR_TARGET="$flags"
export ac_cv_func_mmap=no ac_cv_func_madvise=no gcc_cv_func_mmap_file=no

target=x86_64-linux-musl
cd "$work/build"
mkdir -p binutils gcc
cd binutils

if [ ! -f .coast-configured ]; then
    "$work/src/binutils-2.37/configure" --build="$target" --host="$target" --target="$target" \
        --prefix="$prefix" --with-sysroot=/ --disable-nls --disable-werror \
        --disable-gdb --disable-gold --disable-shared --enable-static --without-debuginfod
    printf '%s\n' "$prefix" > .coast-configured
fi
[ "$(cat .coast-configured)" = "$prefix" ] || {
    printf 'Prefix changed; use a new workspace\n' >&2;
    exit 2;
}

make -j"$jobs"
make install
stage binutils

cd ../gcc
thread_option=--disable-threads
[ "$thread_model" = single ] || thread_option=--enable-threads=posix
configuration="$prefix threads=$thread_model"
if [ -f .coast-configured ] && [ "$thread_model" = single ] && [ "$(cat .coast-configured)" = "$prefix" ]; then
    printf '%s\n' "$configuration" > .coast-configured
fi
if [ ! -f .coast-configured ]; then
    "$work/src/gcc-14.2.0/configure" --build="$target" --host="$target" --target="$target" \
        --prefix="$prefix" --with-sysroot=/ --with-native-system-header-dir=/usr/include \
        --with-as="$prefix/bin/as" --with-ld="$prefix/bin/ld" \
        '--with-pkgversion=Coast native rebuild' '--with-specs=%{!shared:%{!r:-static -no-pie}}' \
        --disable-bootstrap --disable-multilib --disable-multiarch --disable-shared \
        --disable-nls --disable-libsanitizer --disable-libssp --disable-libquadmath \
        --disable-libgomp --disable-libvtv --disable-libcc1 --disable-plugin --disable-lto \
        --disable-libstdcxx-pch --disable-libstdcxx-verbose --disable-libstdcxx-backtrace \
        --disable-symvers "$thread_option" --disable-fixincludes --enable-languages=c,c++ \
        --enable-checking=release --enable-clocale=generic --without-isl --without-zstd
    printf '%s\n' "$configuration" > .coast-configured
fi
[ "$(cat .coast-configured)" = "$configuration" ] || {
    printf 'Prefix or thread model changed; use a new workspace\n' >&2;
    exit 2;
}

# Changing AWK does not itself invalidate GNU make's generated-table targets.
# Retire old derived tables/runtimes once, including a workspace first built
# with BusyBox awk. Never edit or substitute the upstream generator inputs.
options_helper=${0%/*}/prepare-gcc-options.sh
[ -f "$options_helper" ] || options_helper=/usr/src/coast/prepare-gcc-options.sh
/bin/sh "$options_helper" "$work/build/gcc" "$target" "$AWK"

# Command-line variables override target flags captured by an older configure.
# A resumed libstdc++ build must not retain its former global -std=gnu++11.
native_make() {
    make -j"$jobs" AWK="$AWK" CFLAGS_FOR_TARGET="$CFLAGS_FOR_TARGET" \
        CXXFLAGS_FOR_TARGET="$CXXFLAGS_FOR_TARGET -D_GNU_SOURCE" "$@"
}

native_make all-gcc

# Check the freshly built front ends before compiling their target runtimes.
# A version banner alone cannot catch silently malformed language flags.
for specification in 'gnu++98 199711L' 'gnu++11 201103L' 'gnu++17 201703L' 'gnu++20 202002L'; do
    set -- $specification
    printf '#if __cplusplus != %s\n#error incorrect C++ language mode\n#endif\nint main(){return 0;}\n' "$2" > "$work/option-check.cpp"
    ./gcc/xgcc -B./gcc/ -x c++ -std="$1" -nostdinc++ -fno-implicit-templates \
        -Werror -fsyntax-only "$work/option-check.cpp"
done

printf '#if __STDC_VERSION__ != 201112L\n#error incorrect C language mode\n#endif\nint main(void){return 0;}\n' > "$work/option-check.c"
./gcc/xgcc -B./gcc/ -std=c11 -Werror -fsyntax-only "$work/option-check.c"
stage gcc

native_make all-target-libgcc
stage libgcc

native_make all-target-libstdc++-v3
stage libstdcxx

native_make install-gcc install-target-libgcc install-target-libstdc++-v3

# Prove the new compiler and its runtime, rather than just an install command.
printf '#include <stdio.h>\nint main(void){puts("REBUILT_GCC_RUN_OK");return 0;}\n' > "$work/proof.c"
"$prefix/bin/gcc" "$work/proof.c" -o "$work/proof-c"
"$work/proof-c"

cat > "$work/proof.cpp" <<'EOF'
#include <string>
#include <vector>
#include <stdexcept>
#include <iostream>
int main() {
    std::vector<int> values{2, 3, 5};
    if (values.size() != 3 || values.at(2) != 5) return 1;
    try { throw std::runtime_error("REBUILT_GXX_RUN_OK"); }
    catch (const std::exception& error) {
        std::cout << std::string(error.what()) << std::endl;
        return 0;
    }
    return 1;
}
EOF

"$prefix/bin/g++" -std=c++17 "$work/proof.cpp" -o "$work/proof-cxx"
"$work/proof-cxx"

if [ "$thread_model" = posix ]; then
    cat > "$work/proof-threads.cpp" <<'EOF'
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdio>
int main() {
    std::mutex mutex; std::condition_variable condition; bool ready=false;
    std::thread worker([&] { std::lock_guard<std::mutex> lock(mutex); ready=true; condition.notify_one(); });
    std::unique_lock<std::mutex> lock(mutex); condition.wait(lock,[&]{return ready;}); lock.unlock(); worker.join();
    std::puts("REBUILT_CXX_THREADS_OK");
}
EOF
    "$prefix/bin/g++" -std=c++17 -pthread "$work/proof-threads.cpp" -o "$work/proof-threads"
    "$work/proof-threads"
fi

stage complete
printf 'Native toolchain rebuild succeeded: %s/bin/gcc\n' "$prefix"
