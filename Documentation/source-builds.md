# Building Coast from source

Repository: https://github.com/camden-pina/ModernOS.git

Git contains Coast sources and pinned dependency recipes. The local Coast
package server also distributes an offline source kit and native development
dependencies. Build outputs, downloaded archives and signing keys are not Git
inputs.

## Rebuild coverage

| Component | Inputs in this tree | Verification |
| --- | --- | --- |
| Kernel and Chromium ABI profile | Kernel sources, Kbuild, `ports/chromium/kernel-flags.json`, pinned lwIP | Compiled and linked on Linux and inside a Coast guest from the repository source kit |
| Native desktop | `userland/desktop/`, libc and linker script | Linux and native Coast compilation; Chromium guest integration |
| Native desktop ISO | Kernel, desktop, BusyBox and Limine sources; `tools/build_native_iso.py` | Fresh image assembly; UEFI and BIOS kernel/shell/GUI checks |
| ISO/FAT tools | Pinned GNU xorriso 1.5.8.pl02 and mtools 4.0.49 | Static source cross-builds; native tests |
| NASM | Pinned 2.16.03 source, host/cross builder and native PKGBUILD | Host/cross builds, native assembler linked with GCC; native source-recipe test |
| Limine | Pinned 12.9.0 source and `tools/build_limine.py` | BIOS and x86-64 UEFI outputs compiled from source |
| Wayland display bridge | Pinned Wayland 1.21.0/libffi, XML protocols, keyboard map and bridge C | Static source build; actual Chromium browser suite |
| QEMU | `ports/qemu/`, exact dependency sources and builder | QEMU, seven libraries, Ninja and x86 BIOS compiled inside Coast; boots a native-built Coast ISO with ABI and desktop checks; Chromium VNC viewer passes real USB input |
| GNU Bison and grep | Pinned sources, port patches, native builders and offline source kits | Rebuilt inside Coast; signed package installation, parser compilation and complete pipe drainage checked |
| POSIX C++ SDK, CMake and GN | GCC source bootstrap, pinned CMake/GN and subprocess ports | Native GCC/POSIX C++, CMake and GN builds pass; signed SDK/tool upgrades pass real C++ thread, Make/Ninja, CTest and packaging consumers |
| Python and LLVM | CPython native-spawn patch, matching LLVM source and source-built zlib | Source cross-builds pass native execution and signed installation; 128 threaded Python children and C/C++, LLD, ThinLTO and archive workflows checked |
| GNU application ABI | Debian glibc sources/patches and GCC 14.2.0 GNU runtime builders | Source-built glibc, libgcc and libstdc++ execute in Coast; loaded DSO identity, threads, TLS and exceptions checked |
| GNU GUI libraries | `ports/chromium/gui-sources.json`, signed Debian source indexes, exact source/patch hashes and source-built GNU ABI receipts | Resumable rootless source builds; staged shared libraries, plugins and ELF dependency closure checked |
| Electron / standalone Node | 170 upstream repository pins, complete patches, Coast source overlays and GN args | Linux source builds and exact-pair kernel admission; guest runtime tests are separate. Full native Electron compilation is a user-run step |
| VSCodium / Code OSS application | Pinned VSCodium/VS Code source archives, upstream patches and NPM lockfiles | Fresh compilation/packaging; rebuilt C/C++ modules and real editor usability checks in a fresh assembly |
| Code OSS proxy resolver | Pinned Rust source, upstream Cargo lockfile, WABT source and local Rust host tools | Source-built native module linked to the matching GNU ABI; upstream embedded PAC wasm recorded separately |
| Bash/Readline terminal | Official Bash patches, Coast foreground-PTY port, pinned ncurses and the native base's compiled musl SDK | Static source build; pipelines, substitution, file writes and real USB job-control checks |
| Wider source desktop | Python, Coast Packages/makepkg, Qt, QEMU and noVNC source receipts | Fresh assembly passed all eleven guest suites, including actual package builds, Qt USB/clipboard and Chromium-operated nested Coast |
| Fresh Chromium/Code OSS ISO | `tools/build_desktop.py` and explicit component receipts | Fresh source-GUI assembly passed boot, Bash, native C/C++, signed packages, addons/SQLite/proxy, browser and editor guest suites |

A kernel rebuild is a complete native compilation. The desktop pipeline
connects the public component recipes without importing an older ISO. Plain
`make iso` includes BusyBox commands, Bash/Readline, native GCC/G++, standalone
pacman/GnuPG tools, Python, Coast Packages, the makepkg/fakeroot environment,
Qt and QEMU. `PROFILE=core` omits the Python/makepkg/Qt/QEMU integrations.
Additional development ports and full native browser build kits remain
separate components.

`tools/build_gui_sources.py` fetches the pinned Debian sources/patches for all
76 source packages supplying the 93 GUI runtime packages. Rootless Podman
contains their binary build tools; exported runtime libraries are compiled
from source. The builder verifies completed package receipts on restart and
cleans successful object trees inside its own container namespace.

`tools/prepare_gui_runtime.py --source-build PATH` stages a complete source
receipt with matching source-built glibc/libgcc/libstdc++, then checks the ELF
dependency closure, including optional plugins. Omitting `--source-build`
selects the explicitly recorded binary bootstrap path for development. The
desktop orchestrator requires the source-built path. Package scripts remain
unexecuted during staging, and no runtime library is committed to Git. Debian
build-time packaging checks run inside the rootless build container; Coast
guest qualification remains separate.

## Full desktop build on Linux

```sh
make iso
make run
```

The output is `build/desktop-source-wide/image/Coast-desktop.iso`. Host setup remains
a prerequisite: the native-image tools below, rootless Podman, Bubblewrap,
CMake, Ninja, pkg-config, ncurses host utilities, Autotools/Bison/Gettext tools and
Electron's upstream Linux build dependencies. The workflow installs no host
packages. It downloads pinned Rust host tools into its own output directory.
Its logs live in `build/desktop-source-wide/logs/`. The
[README](../README.md) supplies the short build sequence;
[host setup](host-setup.md) describes `make setup-host` and its checks.
The [command reference](build-system.md) explains all profiles and optional
arguments. Use `make iso ISO_ARGS=--plan` to inspect the complete build plan,
or `make iso ISO_BUILD=build/desktop-qa ISO_ARGS=--qualify` to build and qualify
a separate image.

Rerun the same command after interruption. Completed receipts, their source
identities and artifact hashes are checked before reuse. Changed build inputs
require a new `ISO_BUILD=build/another-desktop` directory, preserving the
previous outputs. An interrupted one-shot staging operation is retained under
that workflow's `failed/` before retrying. No selected image is overwritten.

The pipeline first syncs/builds Electron and Node, then builds the GNU ABI,
GUI libraries, VSCodium application, addon SDK/modules, Rust proxy resolver, admitted kernel,
native image, POSIX-threaded native GCC/G++ SDK, standalone package/archive/HTTPS
tools, Bash/Readline, Wayland bridge and pinned font/dictionary assets.
The GIO trash helper uses the matching application SDK, so it does not require
an old QEMU SDK. Assembly checks the exact Electron/Node kernel admission and
component hashes. It also checks every payload ELF's architecture, libraries
and GNU ABI requirements against the actual image. Microsoft sign-in is optional. To include its upstream proprietary brokers,
select `ISO_ARGS=--with-microsoft-auth` on a new build root. The default
source image omits these binaries; the receipt records the choice and the
remaining prebuilt application helpers.

`make iso` selects `--with-integrations` and defaults to
`build/desktop-source-wide/image/Coast-desktop.iso`. Six additional explicit
receipts supply Python/jq, makepkg support, Coast Packages, Qt, QEMU and its
source viewer. All use the source-built POSIX musl compiler; no legacy
`build/web`, `build/compiler` or installed runtime prefix is required.
Python builds its own pinned host interpreter when the host minor version
differs, then statically links the target with source-built threaded OpenSSL,
zlib and SQLite. `--bootstrap-host-python` exercises that bootstrap explicitly.
The static interpreter supplies the modules needed by the package and VM
workflows; dynamic-extension loading and ctypes are outside this build. Its
`PYTHON-MODULES.json` records unavailable extension modules.

The makepkg stage builds a separate, patched shared musl loader, GNU coreutils,
findutils, file, fakeroot, gawk, Git, sed, diffutils, grep, Ninja, pkgconf, m4,
patch and xz. Its dynamically linked archive tools allow fakeroot to record
package ownership while the kernel continues to enforce the real UID. The
initial libalpm database describes the exact files/aliases and dependencies in
the assembled image; it records source assembly as validation `none`, rather
than claiming an installation from signed packages.

Qt's host generators and target Core/Gui/Widgets libraries are built separately.
The native Coast QPA demo, source archive and notices enter the image; the
cross SDK remains in its component output. QEMU uses TCI, its static dependency
SDK and source-built x86 BIOS/VGA firmware. Its receipt separately identifies
optional release-supplied ROMs. noVNC and the `coast-vm` launcher are source files.
See [Qt](qt6.md) and [QEMU](qemu.md) for the supported application scope.

Fresh wider images start without a repository URL or signing identity.
`coast list` and `coast repo` work immediately. Obtain the public key and full
fingerprint from the repository operator, then configure it as the normal user:

```sh
coast configure https://packages.example.org --key ./repository.asc --fingerprint FULL_40_CHARACTER_FINGERPRINT
coast server --check
sudo pacman -Syy
sudo pacman -Fyy
coast refresh
```

The privileged helper rechecks the fingerprint, initializes only an empty
keyring, and preserves existing repository identities. `coast server URL`
changes an already trusted server while retaining its key. Image owners can
instead pass `--repository-config PATH` to the wider builder. This JSON contains
exactly `url`, `signing_fingerprint`, and `public_key` (absolute or relative to
the JSON file); the builder embeds public trust only. Community servers remain
an explicit `coast community URL` choice.

With a wider image, `--qualify` additionally checks source recipe cloning,
native makepkg/fakeroot build/install, signed frontend transactions, QEMU's
programs/SDK and guest CPU, Qt USB/clipboard workflows and the Chromium VNC
viewer operating a nested source-built Coast desktop. Pillow and Tesseract are
additional host requirements for those UI observations. Disposable fixture
keys and loopback servers are isolated from existing keys/services.

For local reuse of an existing source-built runtime:

```sh
python3 -B tools/build_desktop.py --output-root build/my-desktop \
  --electron-checkout /path/to/verified/checkout \
  --runtime-build /path/to/complete-runtime/BUILD.json --jobs 4
```

The runtime must include its default application loader and English locale.
`tools/package_electron_runtime.py` can record those resources from an older
source build without modifying its original receipt. `--components PATH`
accepts a local JSON map of component names to source-build receipt paths;
`--plan` shows all component names and commands. Each reused component is
verified. Neither option accepts an input ISO.

A genuinely empty Electron checkout/build still needs the upstream disk
budget; reusing an independently verified source build does not prove that
the entire pipeline was rebuilt from an empty checkout in that run. Guest
qualification is also separate from successful compilation/assembly.

`--qualify` includes opt-in editor diagnostics and runs the owned-VM boot,
Bash job-control, real native C/C++ compilation, signed pacman install/remove,
native-module/SQLite, Chromium and editor usability suites.
The boot suite checks UEFI and BIOS at one and four CPUs. QEMU/KVM, OVMF,
passt, GnuPG, Pillow and Tesseract are additional host prerequisites. The host
QEMU needs its native passt backend (QEMU 10.1 or newer). Select `--qualify`
on the initial build: it enables editor diagnostics and changes the build's
identity. Adding it to a build made with different options requires a fresh
output root. The aggregate qualification receipt
binds every report to the exact ISO hash and verifies that all owned VMs stop.
It does not change the selected development ISO. An already assembled QA image
can be checked with `tools/qualify_desktop.py --iso PATH --output-root PATH`.

The package CLI stage builds pacman/libalpm, GnuPG, libarchive, libseccomp,
curl/OpenSSL, zlib/zstd and GNU Make from pinned sources. It supplies the normal
`/etc/pacman.conf`; repository URLs and trusted signing keys are configured by
the image owner. It does not import the local development server's key or its
private signing identity. Qualification creates a separate disposable key and
performs real signature verification, installation and removal entirely in
the guest's RAM filesystem. The higher-level `coast` Python frontend and full
makepkg/fakeroot environment are not included in this stage.

Local validation on November 9 rebuilt all 76 GUI source packages, assembled
the core fresh image and passed all seven guest suites. The wider source
workflow then integrated Coast Packages/makepkg, Qt and QEMU and passed all
eleven aggregate guest suites on its final image. All 668 payload ELF checks
passed; the image contains 37 source-component package records. Its exact
qualification results are recorded in the same passoff. That run reused independently
verified source-built Electron/Node and GNU ABI receipts; it did not rebuild
the entire Electron graph from an empty checkout. A separate source-only export,
without ignored caches, rebuilt the kernel/native ISO and passed all four
UEFI/BIOS CPU cases. Exact hashes and report paths are in
[development status](AI-HANDOFF.md).

## Linux host

```sh
make -j4
python3 -B tools/build_desktop_kernel.py --output-root build/my-kernel --jobs 4
python3 -B tools/build_native_gui.py --output-root build/my-desktop
make nasm
make limine
make iso PROFILE=native
```

`make limine` compiles Limine's boot programs instead of downloading the
upstream binary bundle. It uses Clang/LLVM and mtools, and can build a separate
host NASM automatically. `make iso PROFILE=native` uses the GCC/binutils bootloader path
and compiles musl/BusyBox for the native shell tools. It requires mtools and
xorriso on Linux. This image is independent of existing ISOs and contains the
native desktop; the full Chromium/Code OSS runtime remains separate. Host OVMF firmware is discovered from the Linux
installation, or selected with `OVMF=/path/to/OVMF.fd`.

The `ports/linux-uapi/include/` files are C header sources from the original
linux-api-headers 1:7.1-1 snapshot. `SOURCE.json` records all file hashes and
`licenses/` preserves notices. QEMU, Qt and the display bridge use these headers
rather than a developer's installed `/usr/include/linux`.

## Offline native kernel build

```sh
sudo pacman -Sy
sudo pacman -Fy
coast install coast-build --noconfirm
mkdir -p ~/Builds/coast
cd ~/Builds/coast
tar -xJf /usr/src/coast/os/coast-source.tar.xz --strip-components=1
python3 -B tools/build_desktop_kernel.py --jobs 4
readelf -h build/desktop-kernel/vmlinux
python3 -B tools/build_native_iso.py --kernel build/desktop-kernel/vmlinux --jobs 4
```

`coast-source` contains a deterministic source archive with a SHA-256 inventory
and the exact lwIP source. Extract into the user's build directory; `/usr/src`
is package-owned. The running kernel is unaffected. This workflow uses native
Coast GCC, binutils, make and Python, without executing guest programs on Linux.

`coast-build` resolves the native build tools and Wayland SDK. The repository
also has `nasm-source` and `wayland-source` with their exact upstream archives.
`coast-distfiles` installs the verified core sources under `/usr/src/coast/distfiles`;
the fetch helper checks their hashes before copying them into a build checkout.
Its toolchain, port and engine source packages remain below the RAMFS per-file
limit. `coast-linux-uapi`, xorriso and mtools complete the native image prerequisites.
Source recipes are available for NASM, xorriso, mtools and Limine.
For a reviewed native NASM build, run `coast refresh`, `coast clone nasm`, then
`makepkg --noconfirm` in `~/Builds/nasm`.

```sh
PKG_CONFIG_PATH=/usr/lib/coast/wayland-sdk/lib/pkgconfig pkg-config --static --libs wayland-server
```

This SDK provides static Wayland client/server libraries, libffi, headers and
relocated pkg-config metadata. The QEMU SDK is documented separately in
[qemu.md](qemu.md).
The [native build tools guide](native-build-tools.md) covers Bison, grep and the
threaded C++/CMake/GN bootstrap.

## Build and run Coast inside its own emulator

`coast install qemu-build` installs the offline QEMU build kit and its native
build closure. `coast install coast-vm` supplies the source noVNC client and
Python launcher. The [QEMU guide](qemu.md) contains the complete native QEMU,
OS ISO and nested boot commands. The end-to-end qualification rebuilt both
QEMU and Coast in one VM, then ran the newly compiled desktop inside that QEMU.

The desktop kernel profile enables native mappings up to 32 GiB and RAMFS files
up to 8 GiB. Budgeted applications keep their 512 MiB per-mapping cap. Guest
checks cover a real mapping above 1 GiB, sparse files above 4 GiB and exact RAM
bytes above 1 GiB in a 2 GiB nested QEMU guest. A combined source-build VM should
have at least 20 GiB of RAM and four CPUs; the live filesystem consumes guest
RAM for its source and object files.

## Electron source graph

The [manual native Electron procedure](build-electron-in-coast.md) is provided
for users building Electron inside Coast themselves. Its prerequisite checks
and build commands are documented separately from the automatic Linux recipe;
an end-to-end Electron compilation inside Coast is not part of the automated
native validation.

```sh
python3 -B tools/build_electron_source.py --sync --build --jobs 4
```

The builder fetches pinned depot_tools and Electron, resolves upstream DEPS,
checks all 170 repository revisions, applies checksum-pinned patches, installs
Coast policies, and configures separate Electron and Node builds. Upstream
hooks supply the host build tools. No private historical JSON/configuration or
compiled Electron object cache is an input to this recipe. Follow the upstream
[Electron build prerequisites](https://www.electronjs.org/docs/latest/development/build-instructions-linux)
and [GN instructions](https://www.electronjs.org/docs/latest/development/build-instructions-gn).

The selected policies compile jitless JavaScript, explicit Wasm interpreter
bounds checks, the Coast platform adapters and the standalone Node CLI. The
checked-in Electron BUILD overlay also guards context-snapshot packaging targets
when snapshots are disabled, so an empty output tree does not need stale files.
The selected source-built pair has passed Coast runtime checks. A new pair
still requires exact-pair admission and its own guest qualification.
Chromium documents a minimum of 100 GB free disk space for a fresh checkout and
build. Keep sufficient space for that source graph, separate object trees and
linker outputs when choosing a build volume.
See the upstream [system requirements](https://chromium.googlesource.com/chromium/src/+/main/docs/linux/build_instructions.md#system-requirements).

For read-only verification of a preexisting patched checkout:

```sh
python3 -B tools/build_electron_source.py --checkout /path/to/checkout
python3 -B tools/build_electron_source.py --checkout /path/to/checkout \
  --configure-check --output-root build/graph-check
python3 -B tools/build_electron_source.py --checkout /path/to/checkout \
  --build --output-root build/clean-runtime --jobs 4
```

The second command mounts source read-only, overlays the checked-in policies,
and writes new GN graphs into the selected output directory. The third also
compiles into that directory without reusing old object files or executing the
target runtime on Linux. Pinned optional Rollup host tooling is fetched and
verified in the build directory; it is separate from the Coast runtime.
The pinned Chromium host Node and vendored npm sources supply local build-tool
wrappers, so a global npm installation is unnecessary. The builder also exposes
the two upstream repo-root TypeScript outputs through private mounts backed by
the output directory, and resolves original-fs generator inputs from their
source location. The original checkout stays unchanged.

## Code OSS application sources

`ports/codeoss-app/SOURCES.json` pins the VSCodium release source and its exact
upstream VS Code revision. Its `BUILD_SOURCEVERSION` is VSCodium's release-string
SHA-1, rather than an upstream Git commit. The source builder applies the pinned
VSCodium patches, uses the upstream NPM lockfiles and requests native addons
from source. Source preparation, dependency installation and compilation have
separate checkpoints so an interrupted download does not require patching twice.

```sh
python3 -B tools/build_codeoss_source.py --electron-checkout /path/to/checkout \
  --prepare --build --package-app --output-root build/codeoss-source-build
```

This builds the extensions and minimized desktop workbench, then packages its
production dependencies under `workspace/Coast-application-linux-x64/resources/app`.
The explicit Coast packaging task skips downloading and running Electron.
`BUILD.json` and `PACKAGED.json` distinguish compilation from packaging and guest
qualification. Packaging alone does not establish that every dependency was
compiled from source. The desktop pipeline replaces its C/C++ modules and Rust
proxy resolver in the next steps and excludes the optional Microsoft broker
binaries by default.

Use `tools/prepare_codeoss_sdk.py` with matching GNU ABI receipts and the staged
GUI runtime to prepare the isolated native-module SDK. It adds checksum-pinned
X11/Kerberos source headers and pkg-config metadata, without copying ELF objects
from development packages or borrowing host headers. Then:

```sh
python3 -B tools/build_codeoss_addons.py \
  --application-build build/codeoss-source-build --sdk build/codeoss-gnu-sdk \
  --electron-checkout /path/to/checkout --output-root build/codeoss-addons
```

This rebuilds separate copies of the pinned C/C++ modules using the matching
Electron headers, source-built GNU libraries and checked-in module versions.
It checks their required library and ABI versions. The Rust resolver is built
separately:

```sh
python3 -B tools/build_codeoss_rust.py \
  --application-build build/codeoss-source-build --sdk build/codeoss-gnu-sdk \
  --output-root build/codeoss-rust
```

`ports/codeoss-app/rust-addons.json` pins the resolver source, WABT and Rust host
tools. Cargo uses the upstream lockfile; source-built WABT translates the
upstream PAC wasm to C, with explicit memory bounds checks and no runtime JIT.
The embedded PAC wasm itself is an upstream build artifact, not rebuilt from
QuickJS source here. The remaining packaged native helpers are upstream
ripgrep and the sandbox helper. WASM and extension build artifacts also remain
explicit coverage limits. The image manifest lists remaining prebuilt ELF files.

Loading the modules in Coast, packaging them with rebuilt Electron and
qualifying the combined image remain separate steps. `make iso` runs
these source-build stages and the fresh assembler.

## Source-only publication and package snapshots

```sh
make source-check test-source-tree
make source-archive
python3 -B tools/add_source_repository.py --output-root build/new-source-repository
```

The publisher extends a verified existing repository using the existing private
signing key outside the public output. It signs new packages, pacman databases,
and the source catalog. Original packages stay available. Verify an unselected
candidate first:

```sh
python3 -B tools/test_coast_source.py \
  --repository build/new-source-repository/repository.json \
  --output-root build/new-source-repository-test --build-image --boot-rebuilt
```

The test boots an owned Coast VM, installs packages through signature checks,
rebuilds the kernel, desktop and NASM, and exercises native SDK and FAT consumers.
With `--build-image --boot-rebuilt`, it compiles Limine, creates the ISO inside
Coast, exports the result through read-only QEMU memory inspection, then boots
the image and checks its actual kernel/desktop. Reports remain
local in `build/`. Do not publish an existing source snapshot under a new hash
without building a new repository snapshot.
