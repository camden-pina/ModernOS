# QEMU inside Coast

Coast runs QEMU 10.2.4, including `qemu-system-x86_64`, `qemu-img`, `qemu-io`,
QMP, VNC, WebSockets and libslirp networking. The emulator, all seven library
dependencies, Ninja and the BIOS used by nested Coast have been compiled inside
Coast with native GCC. That emulator booted a Coast ISO compiled in the same VM,
passed its userland ABI checks and ran its graphical desktop.

The CPU backend is upstream TCI, which interprets TCG operations without
writable executable memory. Coast's W^X policy remains enabled. This is software
emulation and is considerably slower than KVM or a TCG JIT. Only x86-64 system
emulation is built. There is no native SDL/GTK display backend or KVM interface;
use the serial console or the Chromium viewer below.

## Install and run

With the local signed Coast repository running on the outer Linux host:

```sh
sudo pacman -Sy
sudo pacman -Fy
coast install coast-vm --noconfirm
qemu-system-x86_64 --version
coast-vm --iso /home/user/Builds/coast/build/native-image/Coast-native.iso
```

Open the URL printed by `coast-vm` in Chromium, normally
`http://127.0.0.1:6080/coast.html?port=5701`. Wait for **Shell ready**, click the
virtual screen and type `gui`. **Shell** sends Ctrl+Alt+Esc to return from the
nested desktop to its console. Ctrl+C in the launcher's terminal stops its QEMU
child and closes the local listeners.

The viewer uses source JavaScript from noVNC 1.6.0 and Coast's Python launcher.
HTTP, VNC and WebSocket listeners bind to loopback. Nested networking is disabled
by default; add `--network` for QEMU user networking. `--memory 2048` selects
2 GiB of nested RAM. The large-build desktop kernel supports this; older kernels
with a 512 MiB mapping cap require `--memory 384`. The launcher chooses its
default and checks the maximum against the running kernel's allocation limit.

For a bounded serial and desktop integration run:

```sh
python3 /usr/share/coast/qemu/boot_coast.py \
  --iso /home/user/Builds/coast/build/native-image/Coast-native.iso --gui
```

This boots the ISO, runs its ABI check, opens the actual desktop, saves a
framebuffer and closes it using an emulated USB shortcut. Reports go to
`/tmp/coast-nested` unless `--output-root` selects another path.

## Rebuild QEMU and Coast inside Coast

Use an outer VM with at least 20 GiB of RAM and four vCPUs for the combined build
and nested desktop checks. The live filesystem is RAM-backed: copy source
changes, images and receipts to a supported writable mount before shutdown.

```sh
coast install coast-build --noconfirm
coast install qemu-build --noconfirm
mkdir -p ~/Builds/qemu ~/Builds/coast
cd ~/Builds/qemu
tar -xzf /usr/src/coast/qemu-build/coast-qemu-10.2.4.tar.gz --strip-components=1
python3 -B tools/build_qemu.py --native --jobs 4
python3 -B ports/qemu/check.py --runtime build/qemu-native/runtime --sdk

cd ~/Builds/coast
tar -xJf /usr/src/coast/os/coast-source.tar.xz --strip-components=1
python3 -B tools/build_native_iso.py --jobs 4

cd ~/Builds/qemu
python3 -B ports/qemu/boot_coast.py \
  --iso ../coast/build/native-image/Coast-native.iso \
  --qemu ./build/qemu-native/runtime/usr/bin/qemu-system-x86_64 \
  --firmware ./build/qemu-native/runtime/usr/share/qemu --gui
```

To use the newly compiled emulator interactively without replacing installed
packages:

```sh
coast-vm --iso ../coast/build/native-image/Coast-native.iso \
  --qemu ./build/qemu-native/runtime/usr/bin/qemu-system-x86_64 \
  --firmware ./build/qemu-native/runtime/usr/share/qemu
```

The signed build kit resolves GCC/G++, binutils, make, Python, pip, setuptools,
Meson, Ninja, pkgconf, patch, GnuPG and Linux UAPI headers. Matching upstream
archives are installed under `/usr/src/coast/qemu/vendor`; the build kit also
contains pinned Ninja sources. Configuration and compilation work offline after
package installation. It builds libffi, PCRE2, zlib, GLib, pixman, libslirp and
libucontext in a private prefix, then statically links QEMU. The SDK installs
under `/usr/lib/coast/qemu-sdk`, separate from the other development libraries.

Native Ninja uses private sessions for headless jobs. Its cancellation check
verifies two parallel jobs, separate process groups, SIGINT forwarding and
complete worker cleanup. A build-local Python compatibility hook translates
unsupported optional xattrs for Meson's copy operations. It does not change
global Python behavior or kernel permission checks.

Eighteen BIOS, VGA BIOS and boot ROM files are built from the sources bundled
with the exact QEMU release. `BUILD.json` identifies those files and separately
lists optional release-supplied PXE/EFI ROMs. Git contains no firmware binaries.

The OS recipe above builds the native kernel, desktop, BusyBox, account tools
and source-built Limine BIOS/UEFI boot programs. It produces
`build/native-image/Coast-native.iso`. Full Chromium/Code OSS source rebuild
coverage is tracked separately in [source-builds.md](source-builds.md).

## Fresh source desktop

`make iso` includes QEMU, its dependency SDK, source-built x86 BIOS
firmware, source noVNC and `coast-vm` without importing a prior image or package
server. The builder takes an explicit source-built POSIX musl compiler:

```sh
python3 -B tools/build_qemu.py --compiler-build build/my-desktop/compiler/BUILD.json \
  --output-root build/my-qemu --jobs 4
```

`make iso ISO_ARGS=--qualify` checks the installed emulator/SDK and operates
a nested native Coast desktop through Chromium, WebSocket VNC and USB input.
This complements the native-in-Coast rebuild workflow above. The fresh image
contains the runtime and SDK; the larger native build kit remains separately
installable from a configured signed repository.

## Linux build and qualification

```sh
python3 -B tools/build_gcc.py
make qemu
```

This cross-build uses the pinned musl toolchain, Bubblewrap, Meson host tooling
and checked-in Linux UAPI headers. It never executes target programs on Linux.
Dependency versions and SHA-256 hashes are in `userland/vendor/qemu-sources.json`;
the QEMU release signature is verified using its pinned upstream public key.

The native integration harness starts an owned, diskless Coast VM, installs
signed packages, performs both source builds and boots the resulting ISO inside
the resulting emulator:

```sh
python3 -B tools/test_qemu_native.py \
  --repository build/Coast-repository-server/repository.json \
  --output-root build/qemu/my-native-check
```

`tools/test_qemu_viewer.py` exercises actual USB input through Chromium,
WebSocket VNC and the nested USB keyboard. `tools/test_large_builds.py` checks
large mappings, sparse RAMFS files and a real 2 GiB nested guest. The desktop
build profile permits 32 GiB native mappings and 8 GiB RAMFS files while keeping
budgeted application mappings capped at 512 MiB.

The installed `python3 /usr/share/coast/qemu/check.py --sdk` suite checks disk
conversion, qcow2 writes and rollback, coroutines, QMP, VNC, BIOS long-mode guest
execution and a native consumer of all seven SDK libraries. These tests do not
establish arbitrary Linux/Windows guest or real-hardware compatibility.

`tools/add_qemu_build_repository.py` creates a new signed build-kit candidate.
After native qualification, `tools/publish_native_qemu.py` checks the exact
input and artifact hashes, publishes the Coast-built programs/libraries/Ninja,
updates versioned source packages and replaces recipes in a new snapshot. The
existing repository is preserved and signing secrets remain outside public
outputs. Qualify package installation before selecting the new snapshot.
