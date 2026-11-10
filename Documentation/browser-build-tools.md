# Browser build tools

`make iso` prepares the browser build tools automatically on an x86-64 Linux
host. This page is background and troubleshooting information; users do not
need to follow a separate setup procedure to include the browser in the ISO.

## What these tools do

| Tool | Purpose |
| --- | --- |
| CMake 4.4.4 | Configures JavaScriptCore and WebCore and generates build rules |
| Ninja 1.13.2 | Executes those build rules and the Meson-generated rules |
| Meson 1.10.2 | Configures GLib/Gio and other browser libraries |
| Ruby 3.4.7 | Runs WebKit's source-code generators |

These programs run on the build computer. They are not browser add-ons or
programs required inside Coast, and they are not packaged into the ISO.
The kernel-only build does not need them.

## Automatic preparation

The JavaScriptCore and GLib builders call
[`tools/prepare_browser_host.py`](../tools/prepare_browser_host.py). It fetches
CMake, Ninja and Meson from pinned upstream URLs, checks their SHA-256 hashes,
and extracts them locally. Ruby is compiled from the pinned source archive in
[`userland/vendor/sources.json`](../userland/vendor/sources.json). Downloads use
the same verified cache handling as other source inputs.

The build selects these cached tools directly. No system installation of
them, pip, unzip or PATH editing is required. Normal host requirements such
as GCC/G++, GNU make, Python and curl still apply. Ruby's YAML/Psych generator
also needs libyaml development headers, and NetSurf's host resource tools need
libpng and zlib development headers. On Debian/Ubuntu these are `libyaml-dev`,
`libpng-dev` and `zlib1g-dev`; install them before building. See the
[README](../README.md#host-requirements).

Parallel builders share a preparation lock. Once prepared, tools are reused
offline. An incomplete extraction is repaired from its verified archive.
The userland Makefile orders the compiler, JavaScriptCore, browser libraries,
broker and renderer so parallel builds wait for their prerequisites.

You can prepare the tools ahead of time, without compiling the browser:

```sh
make browser-host
```

This command is optional. The normal complete build remains:

```sh
make iso
make run-image
```

## Memory-constrained builds

For hosts where a single large C++ compilation is killed for memory pressure,
an opt-in mode reduces the size of generated translation units:

```sh
COAST_LOW_MEMORY_BUILD=1 COAST_PORT_JOBS=1 \
  COAST_JSC_JOBS=1 COAST_WEBCORE_JOBS=1 make -j1 iso
```

This retains the same renderer features, ABI and Release optimization flags.
GCC collects its internal heap more frequently. In the isolated WebCore source
copy, the four typed-array implementation files are partitioned into complete
one-function translation units, preserving all 30 host functions and the
constructor helper. WebCore uses two-source bundles instead of the dense
64-source JavaScript binding bundles. JavaScriptCore's original
`bytecompiler/BytecodeGenerator.cpp` and `bytecompiler/NodesCodegen.cpp` are
compiled separately with `@no-unify`, retaining every source-body byte and
the same O3 flags. Their combined three-file bundle exceeded measured host
memory in both tool-owned and native builds. The remaining bytecompiler
bundle contains `ProfileTypeBytecodeFlag.cpp`. The original `heap/Heap.cpp`
also compiles separately after its eight-file bundle exceeded native host
memory. Removing it repacks heap bundles 4–9; heap bundles 1–3 and every other
JavaScriptCore/WTF bundle retain their contents. All source-body bytes and O3
flags are retained. These are checked source-list changes, not a global
JavaScriptCore bundle-size reduction.
The separate JSCOnly source tree is not structurally changed.
Only the renderer link also receives GNU ld's `--no-keep-memory`, which rereads
input symbol tables instead of retaining them in memory. This can reduce linker
memory use at the expense of link time; see the
[GNU ld option documentation](https://sourceware.org/binutils/docs/ld/Options.html).

On measured tight hosts, QEMU guest tests and compiler work had to be serialized
to retain enough memory headroom. Run compiler/linker work and QEMU tests one at
a time, stopping or finishing one before starting the other. `-j1` limits build
parallelism but does not limit independently launched QEMU processes.

The mode accepts only `0` or `1`; unset or `0` selects the normal build.
Switch modes only after every active builder has stopped. Normal mode restores
the original four source bodies and source list from the verified pinned
archive; affected translation units may need rebuilding. This is a peak-memory
reduction, not a guarantee that every build stage fits a particular RAM limit.
It can increase compilation time and object count. See
[GCC's collector parameters](https://gcc.gnu.org/onlinedocs/gcc-14.2.0/gcc/Optimize-Options.html)
for the compiler-memory tradeoff.

## Explicit Clang19 renderer builds

The default renderer compiler remains GCC. An optional, separately cached
Clang19 build can help when even individual GCC translation units exceed the
host's memory headroom:

```sh
COAST_WEBCORE_COMPILER=clang19 COAST_LOW_MEMORY_BUILD=1 \
  COAST_PORT_JOBS=1 COAST_JSC_JOBS=1 COAST_WEBCORE_JOBS=1 make -j1 iso
```

`COAST_WEBCORE_COMPILER` accepts only `gcc` or `clang19`; unset selects GCC.
Install Clang19 from its official vendor/distribution packages and expose
`clang-19` on `PATH`, including its matching Clang resource headers and host
shared libraries. This mode does not install a compiler automatically. It also
requires the existing musl sysroot, GCC14.2 cross-toolchain and POSIX-threaded
libstdc++/libgcc prepared by the normal browser dependencies.
The runtime preflight currently requires a Linux x86_64 build host. It executes
a Linux-musl host binary and does not replace Coast guest validation.

Both C and C++ use named Clang wrappers with an explicit `x86_64-linux-musl`
target, musl sysroot, GCC installation, GNU target linker and POSIX C++ headers
and runtime archives. Host loader paths are scoped to those wrappers; inherited
host include/library overrides are cleared. The custom POSIX `libgcc.a`
contains the unwinder, so `--unwindlib=none` suppresses a second implicit unwind
library rather than removing the existing unwinding implementation. The mode
first statically links and runs the existing C++ thread/condition-variable/
exception test, verifies its ELF target and static linkage, and then lets CMake
perform its normal compiler checks.

Clang explicitly uses `USE_HEADER_MAPS=OFF`, matching the upstream WPE/GTK
default. The pinned WPE release archive omits `Tools/Scripts/hmaptool`; the
custom Coast port name would otherwise enable Clang header maps by default.
The build retains ordinary target include directories and forwarded headers.
This changes header lookup setup, not renderer features or compiler checks.

Clang uses `webcore-llint-clang19/` (or `webcore-clang19/` for the explicit C-loop
interpreter), `webcore-clang19-toolchain.cmake`, its own runtime include, named
wrappers and separate logs under `build/modern-browser/`. Compiler identity is
recorded in the selected Clang cache. If its resolved path or reported version changes,
preserve or rename the old Clang cache before configuring a fresh one. GCC
objects are never copied into the Clang cache, and the existing GCC cache and
standalone JSCOnly build retain their own compiler settings. Shared static
dependencies can still be reused.

`COAST_WEBCORE_GENERATOR` accepts only `ninja` (the unchanged default) or
`makefiles`. The latter requires explicit `COAST_WEBCORE_COMPILER=clang19`
and a working GNU `make` on `PATH`; the resolved executable is passed as
`CMAKE_MAKE_PROGRAM` to CMake's official `Unix Makefiles` generator. For example:

```sh
COAST_WEBCORE_COMPILER=clang19 COAST_WEBCORE_GENERATOR=makefiles \
  COAST_LOW_MEMORY_BUILD=1 COAST_PORT_JOBS=1 COAST_JSC_JOBS=1 \
  COAST_WEBCORE_JOBS=1 make -j1 iso
```

This uses `webcore-llint-clang19-makefiles/` (or
`webcore-clang19-makefiles/` for C-loop), its own
`webcore-clang19-makefiles-toolchain.cmake` and runtime include, and matching
`webcore-llint-clang19-makefiles-{configure,build}.log` logs. Existing GCC and
Clang/Ninja caches and logs are retained. Nothing is copied between object
caches; each generator tracks its own genuine dependencies and successful
builds. C/C++ wrappers and the static runtime preflight are shared because
their compiler and runtime arguments are identical.

On one constrained host the 127 MiB Ninja graph retained about 146 MiB while
compiling, and a translation unit killed under Ninja passed unchanged alone
at about 633 MiB compiler RSS. The Makefiles option tests whether CMake's
hierarchical makefiles reduce that persistent graph overhead; it is not a
guarantee of lower peak memory or a completed renderer build. See the official
[Unix Makefiles generator](https://cmake.org/cmake/help/latest/generator/Unix%20Makefiles.html)
and [CMAKE_MAKE_PROGRAM](https://cmake.org/cmake/help/latest/variable/CMAKE_MAKE_PROGRAM.html)
documentation. Keep all build job counts at one on such hosts.

For low-memory Clang19/Makefiles builds only, after real configuration the
Python builder replaces itself with a small waiting shell for the identical
`cmake --build` command. This frees the persistent Python parent's memory
without changing compilation flags, environment, working directory, logs or
dependency tracking. A failed build keeps its log, reports the real CMake
status and exits unsuccessfully; the renderer is not installed. Only after
successful completion does the shell replace itself with the shared Python
finalizer, which installs the selected renderer and retains the one-time
original backup. Other compiler/generator/memory modes keep the normal build
path. CMake, make and supervisor memory still contribute to peak usage.

Renderer feature selections, Release/O3 optimization and the selected
low-memory source policy stay the same. The selected renderer replaces
`userland/bin/web-renderer` only after a successful link. The existing Makefile
rechecks the selected cache on every renderer invocation. Keep preparation,
compilation and guest tests serialized because the generated WebCore source
tree and installed renderer destination are shared.

See the official [Clang toolchain documentation](https://clang.llvm.org/docs/Toolchain.html)
and [cross-compilation guide](https://clang.llvm.org/docs/CrossCompilation.html)
for target, sysroot, library and runtime selection.

## Cache and troubleshooting

All host-tool outputs live in ignored `build/modern-browser/` directories:

| Path | Contents |
| --- | --- |
| `downloads/` | Verified CMake, Ninja and Meson archives |
| `host-tools/cmake-4.4.4-linux-x86_64/` | CMake executable and modules |
| `host-tools/ninja` | Ninja executable |
| `host-tools/meson-1.10.2/` | Meson Python package |
| `host-tools/ruby/` | Ruby executable and standard library |
| `ruby-3.4.7/`, `ruby-host/` | Ruby source and compilation outputs |

Ruby preparation writes `browser-host-ruby-configure.log`,
`browser-host-ruby-build.log` and `browser-host-ruby-install.log` there.
A failing step reports the relevant log. Fix the reported host dependency
and rerun `make iso`; successful preparation is reused.
Ruby cache readiness loads ERB, JSON and YAML and parses a YAML document, so a
Ruby installation without a working YAML/Psych extension is not reused.
If Ruby was built before libyaml headers were installed, stop active builders
and clear only `host-tools/ruby/` and `ruby-host/` in that cache before retrying;
this preserves the verified source/download archives.

A checksum mismatch stops preparation and preserves the suspect archive.
Move or remove the reported archive, then rerun the build to fetch it again.
Downloads use configured mirrors first, then the primary URL and available
package mirrors; the final source retains retries and an IPv4 connection fallback.
`SOURCE_MIRRORS` also applies to these host tools. Serve their exact cached archive
filenames (including the `coast-` prefixes) from a flat HTTPS directory; see
[mirror configuration](build-system.md#commands). Initial downloads need network
access; deleting the cache requires fetching or preparing it again.

`make clean` preserves shared port and host-tool caches. These artifacts do
not belong in GitHub. Pins and upstream links are recorded in the helper and
[`userland/vendor/README.md`](../userland/vendor/README.md).

Builders create their generated output directories automatically. The GLib TLS
and WebCore patchers restore missing or partial source backups from verified
pinned archives. Dependency stages check their installed libraries and metadata
before reusing a completion stamp, and reinstall missing outputs on the next build.

Run `make test-browser-build` for host bootstrap, cold builds, patch-backup
recovery, dependency-cache recovery and parallel Make ordering checks. GitHub
Actions runs these and the source-fetch checks on fresh checkouts for pushes
and pull requests.
