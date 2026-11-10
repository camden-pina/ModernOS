# Native compiler and build tools

Coast's normal-user build profile supports C++ threads, TLS and the
`posix_spawn` subprocess pattern used by native build tools. A temporary vfork
child shares its parent's memory until exec or exit; Coast freezes eligible
peer threads during that interval. Callers retry a transient `EAGAIN` while
peers reach a safe point. Ordinary multi-threaded fork remains restricted.
Browser tasks keep their separate memory budgets and sandbox rules.

## Qualified tools

QEMU, its seven libraries, Ninja, Bison and grep have been compiled from source
inside Coast and tested there. The selected signed package repository includes
those binaries and their offline source kits.

```sh
sudo pacman -Sy
sudo pacman -Fy
brine install bison --noconfirm
brine install grep --noconfirm
brine install bison-build --noconfirm
brine install grep-build --noconfirm
```

Each source kit installs below `/usr/src/coast/NAME-build/`. Extract its archive
into a user-owned build directory, then run its `tools/build_NAME.py --native`
and `ports/gnu-runtime/NAME_check.py` scripts. The signed source catalog also
provides `brine clone bison` and `brine clone grep` recipes for makepkg. Building
a recipe executes its PKGBUILD; review it before running makepkg.

GNU grep's port handles an unsupported splice operation by using upstream's
existing read fallback. Guest checks verify complete pipe drainage for both
matching and nonmatching input. This behavior matters for Autoconf's dependency
tracking probes. Bison checks generate a parser, compile it with native GCC,
and verify precedence, errors and the yacc wrapper with the installed m4.

## Threaded SDK, CMake and GN

The source builders provide GCC/G++ 14.2.0 with POSIX libstdc++, static CMake
4.4.5 with its bundled libraries, and GN at Chromium's pinned revision. Their
cross-built outputs have passed real Coast execution: parallel C++ threads,
futures, TLS, concurrent process creation, CMake compiler try-runs, Make and
Ninja builds, CTest, install, CPack and GN Python subprocesses. Binutils, GCC,
libgcc and POSIX libstdc++ have also been rebuilt from their original sources
inside Coast. The rebuilt compiler passed real thread, future, TLS and
exception checks. Native CMake/GN rebuilds and their Make/Ninja, CTest, CPack,
Python-helper and C++ consumer workflows also pass. The signed repository now
ships those native CMake/GN binaries, their offline source kits, and the
relocatable cross-built POSIX GCC SDK that was separately exercised in Coast.
The native GCC archive retains its original build prefix; it is not relocated
into `/usr`.

```sh
sudo pacman -Syy
sudo pacman -Fyy
brine install gcc --noconfirm
brine install ninja --noconfirm
brine install cmake --noconfirm
brine install gn --noconfirm
brine install llvm --noconfirm
```

The installed Ninja port is 1.13.2-4 or newer. Older images can already contain
an earlier Ninja that satisfies an unversioned dependency; the build source
kits require the qualified revision explicitly.

```sh
python3 -B tools/build_gcc.py --posix-threads \
  --output-root build/compiler-threaded \
  --runtime-root build/compiler-threaded/runtime
python3 -B tools/build_cmake.py --output-root build/cmake-threaded
python3 -B tools/build_gn.py --output-root build/gn-threaded
python3 -B tools/test_build_tools.py \
  --iso build/coast/image/Coast.iso \
  --repository build/brine-repository-server/repository.json \
  --gcc-build build/compiler-threaded/BUILD.json \
  --cmake-build build/cmake-threaded/BUILD.json \
  --gn-build build/gn-threaded/BUILD.json \
  --ninja /path/to/qualified/coast/ninja \
  --native-rebuild --output-root build/native-build-tools-test
```

Use an ISO containing the current native spawn and ELF TLS loader changes.
The test owns a 24 GiB, four-CPU VM, checks the signed package dependencies,
rebuilds binutils/GCC/libstdc++, then rebuilds CMake and GN with that compiler.
It exports build receipts and runtimes and stops its VM. Target programs run
inside Coast. Reports, source downloads and compiled files remain in `build/`.
The compiler archive can be reused with `--native-toolchain-archive` and
`--native-toolchain-report` when continuing the consumer builds. The harness
checks that the previous report proves the exact archive and compiler source
inputs, restores its original installation prefix, and tests it again.

CMake's current static profile disables OpenSSL; its local builds, tests and
packaging work offline. The source download helper uses curl and verifies
pinned SHA-256 hashes. See [full source rebuild coverage](source-builds.md)
for the remaining Chromium/Code OSS image work.

## GNU application ABI and LLVM

`tools/build_glibc.py` builds Debian's pinned glibc 2.36 source and patch series.
Its Coast patch uses non-executable stacks for ELF images that omit
`PT_GNU_STACK`, preserving the kernel's W^X rule. The resulting loader and
libraries have passed native GNU pthread/process/TLS tests, Clang C compilation,
and the Chromium and Code OSS desktop suites. These are cross-built libraries
tested in Coast; rebuilding glibc inside Coast still needs qualification.

`ports/llvm/SOURCES.json` pins Chromium's LLVM commit, and
`tools/build_llvm.py` builds static X86 Clang, LLD and archive/inspection tools.
On Linux it first builds matching host TableGen generators, then cross-compiles
the Coast tools without running them on Linux. `--native` performs the build in
Coast. `tools/test_llvm.py` and `ports/llvm/check.py` cover C/C++, threads,
subprocesses, LLD, ThinLTO and archive consumers. The cross-built LLVM tools
have passed those workflows in Coast, including signed package installation
and normal `/usr` C/C++ SDK discovery. LLVM source compilation inside Coast
remains unqualified. Its source-built zlib dependency enables LLD to read the
GCC SDK's compressed debug sections.

CPython's Coast source patch retries temporary `vfork` refusal while its GIL
remains released. It preserves the kernel's threaded-fork restriction instead
of falling back to that unsupported operation. The updated Python passed
128 subprocesses from four concurrent Python workers with parent PID, working
directory and child statuses checked, followed by LLVM's compiler workflows. Both the
runtime and patch are available in the signed Python package.

`tools/build_gnu_libgcc.py` builds the GNU ABI exception unwinder, static GCC
support libraries and start files from GCC 14.2.0. Its matching target metadata
and host generators also come from source, with pinned GMP, MPFR and MPC
prerequisites. `tools/build_gnu_cxx.py` builds the C++ library and headers against
that unwinder and the source-built glibc SDK. These headers stay separate from
the native musl SDK. A guest test verified both loaded DSO paths, then exercised
real threads, futures, TLS destructors and exceptions.

```sh
make glibc
make gnu-cxx
python3 -B tools/test_gnu_cxx.py --iso build/coast/image/Coast.iso \
  --repository build/brine-repository-server/repository.json \
  --glibc-build build/glibc/BUILD.json --build build/gnu-cxx/BUILD.json \
  --libgcc-build build/gnu-libgcc/BUILD.json --output-root build/gnu-cxx-test
```

Use an image containing that exact glibc build. `tools/test_gnu_native.py`
continues from a proven native GCC archive, rebuilds the GNU libraries inside
Coast, and runs their C++ consumer there. Native GNU library qualification is
pending; glibc remains a separately qualified cross-built bootstrap input.
The current native GCC bootstrap uses binutils 2.37. Its linker cannot consume
the GNU runtime's RELR sections, so native GNU library linking also needs a
linker with RELR support, such as the matching LLD or a newer GNU binutils port.
