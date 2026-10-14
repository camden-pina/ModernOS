These are complete, unmodified upstream source archives used by
`tools/build_nano.py`, `tools/build_native_ports.py`, `tools/build_gawk.py` and
`tools/build_gcc.py`. The archives are downloaded into this directory and
ignored by Git. `sources.json` preserves all 44 original filenames, upstream
HTTPS URLs and SHA-256 hashes; the hashes match the existing builders.
Matching package mirrors are recorded alongside the primary URLs. GNU mirror
selection and kernel.org's CDN are used for supported archives. Public mirrors
do not necessarily carry the exact GitHub-generated snapshots used by this build.
`SOURCE_MIRRORS` can point the fetcher at your own flat HTTPS archive cache; see
[`Documentation/build-system.md`](../../Documentation/build-system.md).
Run `make fetch-sources` to populate the full cache or `make check-sources`
to verify it offline. Kernel and userland builds also restore their required
sources automatically. Keep this cache when building offline or preserving
the source inputs for a particular image. License texts, configurations,
the CA bundle and this manifest remain in Git.

- GNU nano 8.7: https://www.nano-editor.org/dist/v8/nano-8.7.tar.xz
- musl 1.2.5: https://www.musl-libc.org/releases/musl-1.2.5.tar.gz
- Vim 9.2.0000: https://github.com/vim/vim/archive/refs/tags/v9.2.0000.tar.gz
- BusyBox 1.37.0: https://busybox.net/downloads/busybox-1.37.0.tar.bz2
- GNU make 4.4.1: https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz
- GNU awk 5.4.0: https://ftp.gnu.org/gnu/gawk/gawk-5.4.0.tar.xz
- ncurses 6.5: https://invisible-island.net/archives/ncurses/ncurses-6.5.tar.gz
- GCC 14.2.0: https://ftp.gnu.org/gnu/gcc/gcc-14.2.0/gcc-14.2.0.tar.gz
- Binutils 2.37: https://ftp.gnu.org/gnu/binutils/binutils-2.37.tar.gz
- GMP 6.2.1: https://ftp.gnu.org/gnu/gmp/gmp-6.2.1.tar.xz
- MPFR 4.1.0: https://ftp.gnu.org/gnu/mpfr/mpfr-4.1.0.tar.xz
- MPC 1.2.1: https://ftp.gnu.org/gnu/mpc/mpc-1.2.1.tar.gz

SHA-256 hashes are checked by the build script. GNU nano is GPLv3 or later;
musl and ncurses have permissive licenses. Vim retains the Vim license,
BusyBox GPLv2 and GNU make/GNU awk GPLv3 or later. Original notices are retained in
the archives, copied to `licenses/`, and installed in the boot initramfs.
The build disables Linux VT ioctls, external spelling, libmagic, translations,
and nanorc/color configuration. UTF-8 file handling, search/replace, undo/redo,
multiple buffers, and the actual curses editor remain upstream implementations.
Vim includes its full upstream runtime; threading/GUI/channel features are
disabled. Make uses /bin/ash and a pipe jobserver. BusyBox patch checks actual
replacement errors. These explicit source changes are in the verified builder.
Fork/vfork, SIGCHLD and SIGALRM interval timers are real kernel interfaces.
Use ash/build-sh for POSIX scripts; job suspension remains unsupported.
GNU awk is the default awk/gawk; GCC's native generators require its replacement
escaping semantics. Dynamic extensions, PMA and MPFR are explicitly disabled.
See `Documentation/native-nano.md` for supported operations and runtime limits.
GCC/binutils are configured as static x86-64 musl native tools with C/C++,
without threads, dynamic linking, LTO, plugins, multilib or PCH. Their sources
are unmodified; unavailable mmap/madvise paths use upstream malloc/read fallbacks.
GCC runtime exception and upstream math-library notices are copied into the
image. Full toolchain sources are additionally installed under
`/usr/src/modernos` as checksum-verified xz tar streams, with the unchanged
original archive hashes recorded separately. `userland/sdk/bootstrap-toolchain.sh`
builds into a separate native prefix and checks the rebuilt C/C++ executables.

See `Documentation/native-tools.md` for pinned hashes, build configuration, installed
commands, validated workflows and precise remaining ABI dependencies.

## Separate modern JavaScript engine component

`tools/build_javascriptcore.py` uses these unmodified sources. The desktop
Browser still uses NetSurf; this target builds the real WebKit JavaScriptCore
shell and supporting runtime, not WebCore or a complete browser. See
`Documentation/modern-browser-port.md` for configuration, guest tests and remaining work.

| Local archive | Upstream source | SHA-256 |
| --- | --- | --- |
| wpewebkit-2.54.0.tar.xz | [WPE WebKit 2.54.0](https://wpewebkit.org/releases/wpewebkit-2.54.0.tar.xz) | efa9bcc3cb891c2d88f50eec710d9ccee71cbdf1040420361eb98c17355eb452 |
| icu4c-78.1-sources.tgz | [ICU 78.1 release](https://github.com/unicode-org/icu/releases/tag/release-78.1) | 6217f58ca39b23127605cfc6c7e0d3475fe4b0d63157011383d716cb41617886 |
| ruby-3.4.7.tar.xz | [Ruby 3.4.7](https://cache.ruby-lang.org/pub/ruby/3.4/ruby-3.4.7.tar.xz) | db425a86f6e07546957578f4946cc700a91e7fd51115a86c56e096f30e0530c7 |

Ruby is a host generator only. ICU is source-built for host generators and the
target; its real data archive is statically linked. The component's separate
POSIX libstdc++ uses the already verified GCC 14.2.0 source and compiler, without
overwriting the existing single-thread SDK runtime. JavaScriptCore/WTF
copyright/permission notices and LGPL text are combined in
`licenses/javascriptcore-NOTICES.txt`; ICU's original notice is
`licenses/icu-LICENSE.txt`. Both are installed by mkinitramfs. The complete
corresponding source remains in these archives and the owned build script.
The new genuine CPU WebCore renderer also installs `webcore-NOTICES.txt` with
WebCore/PAL, bundled Skia/xdgmime notices and LGPL text. Its corresponding source
is the same pinned WPE archive plus `tools/build_webcore.py` and the owned
`userland/modern-browser/port/` adaptations.

Host binary tools cached in `build/modern-browser/host-tools/` were downloaded
from their primary releases. Original archives are retained separately in
`build/modern-browser/downloads/`:

| Cached archive | Upstream asset | SHA-256 |
| --- | --- | --- |
| modernos-cmake-4.4.4.tar.gz | [CMake Linux x86-64 4.4.4](https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-linux-x86_64.tar.gz) | e5bb807f7728cb60cd8b27ebc97a2edb469b68655f21e844a600c3575b76f5bb |
| modernos-ninja-1.13.2.zip | [Ninja Linux 1.13.2](https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-linux.zip) | 5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6 |

`tools/prepare_browser_host.py` downloads and verifies the pinned CMake, Ninja
and Meson archives automatically and builds Ruby from the pinned source when
needed. The JavaScriptCore and GLib builders invoke it, so `make iso` needs no
separate preparation or PATH changes. It installs only into the local build
cache; host system packages are installed separately. Preserve these caches
for offline builds and subsequent builds.

## WebKit GLib/Gio dependency layer

`tools/build_webkit_deps.py` builds genuine static target GLib/GObject/Gio,
libffi and PCRE2 into the **separate** `build/modern-browser/prefix`. Existing
target zlib/pkg-config data is reused from `build/web/deps`. This target does
not build WebCore or replace the desktop Browser. All source archives below
are retained unmodified and verified before extraction/build.

| Local archive | Primary upstream asset | SHA-256 |
| --- | --- | --- |
| glib-2.86.5.tar.xz | [GLib 2.86.5](https://download.gnome.org/sources/glib/2.86/glib-2.86.5.tar.xz) | ce85a947bb8b3c0204dbeff79aec39bcb46371c6fafb64ba5b8726c71e038d5f |
| libffi-3.5.2.tar.gz | [libffi 3.5.2](https://github.com/libffi/libffi/releases/download/v3.5.2/libffi-3.5.2.tar.gz) | f3a3082a23b37c293a4fcd1053147b371f2ff91fa7ea1b2a52e335676bac82dc |
| pcre2-10.46.tar.bz2 | [PCRE2 10.46](https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.46/pcre2-10.46.tar.bz2) | 15fbc5aba6beee0b17aecb04602ae39432393aba1ebd8e39b7cabf7db883299f |

The GLib digest also matches its published `.sha256sum`. `licenses/glib-LICENSE.txt`,
`libffi-LICENSE.txt` and `pcre2-LICENSE.txt` preserve their upstream license
texts and are installed by mkinitramfs. libffi's Linux executable static
trampolines are disabled because dynamic-library discovery/executable shared
aliasing are unavailable; real generic signal marshalling uses `ffi_call` and
passes guest tests. PCRE2 uses its genuine interpreter with JIT disabled.
Dynamic Gio module loading remains unavailable. A genuine statically registered
OpenSSL Gio TLS backend is now built/tested by the continuation below; full WPE
browser networking/integration remains unfinished.

Host-only Meson is extracted from `meson-1.10.2-py3-none-any.whl` under
`build/modern-browser/host-tools/meson-1.10.2`. The original wheel stays in
`build/modern-browser/downloads`; its SHA-256 matches the
[PyPI 1.10.2 metadata](https://pypi.org/pypi/meson/1.10.2/json):
`5f84ef186e6e788d9154db63620fc61b3ece69f643b94b43c8b9203c43d89b36`.
No host system packages or existing SDK files were replaced.

## Genuine WebCore dependencies (2026-10-10)

`tools/build_webcore_deps.py` builds the pinned HarfBuzz 11.2.1, Fontconfig
2.16.0 and WebP 1.6.0 archives already recorded in the handoff, plus the primary
upstream archives/URLs/SHA256 hashes in
`../modern-browser/dependency-sources.json`: libjpeg-turbo 3.2.0, libxml2 2.14.6,
Soup 3.8.0, glib-networking 2.80.1, libgpg-error 1.61, libgcrypt 1.12.4,
libtasn1 4.20.0, SQLite 3.54.0, libpsl 0.21.5 and nghttp2 1.65.0. Original
downloads, extracted sources, target caches and license notices are retained.
GNOME hashes match the published `.sha256sum` files; SQLite additionally matches
its published SHA3-256. Other primary HTTPS assets are pinned locally; no
signature verification is claimed. The libjpeg-turbo official release asset is
used, not GitHub's unsupported generated source archive.

These libraries are genuine static musl target code; the modern prefix preserves
the working NetSurf dependency prefix and SDK. glib-networking's packaging patch
builds its upstream OpenSSL sources as a static archive and explicitly registers
them with Gio. Guest checks cover trust and hostname rejection without bypasses.
HarfBuzz/Fontconfig/FreeType and WebP/libjpeg-turbo execute real text and image
operations in guests. WebCore compilation and frontend/resource integration
remain separate unfinished work. See `Documentation/AI-HANDOFF.md` for exact evidence.
