# Build system

Coast uses a small Kbuild-style GNU make build. `Kbuild` and directory Makefiles
declare kernel objects explicitly; `scripts/Makefile.build` compiles and archives
them into the selected output directory. The native base image and the full
Chromium/Code OSS image have different assembly paths. See
[source-builds.md](source-builds.md) for their current rebuild coverage.
The [README](../README.md) has fresh-checkout host setup and build/boot commands,
including the wider Coast Packages/makepkg, Qt and QEMU image.

## Commands

```sh
make -j4
make O=build/debug V=1
make desktop-kernel
make -C userland bin/gui -j4
make native-iso
make iso
make run-image
```

`make` builds `build/vmlinux`. `O=...` selects a kernel/image output directory;
shared upstream sources and toolchains remain under `build/`. `V=1` prints full
compiler commands. `CROSS_COMPILE=prefix-` selects a GCC cross compiler;
`LLVM=1` selects Clang/LLVM. `make desktop-kernel` uses the checked-in Chromium
ABI feature profile in `ports/chromium/kernel-flags.json`.

`make native-iso` builds a smaller desktop image at
`build/native-image/Coast-native.iso` without an existing ISO. It compiles musl,
patched BusyBox and Limine as well as the kernel/native desktop. Linux needs
NASM, mtools, xorriso and the standard gzip/bzip2/xz tools. The same recipe runs
inside Coast with `coast-build` installed. Chromium/Code OSS are separate ports.

`make iso` builds native userland, creates an initramfs and packages the selected
bootloader into `build/os.iso`. It builds the native base, without an automatic
Electron/Code OSS bootstrap. `make iso-image` packages existing kernel/initramfs
inputs. `make run-image` boots an existing image; `make run` first rebuilds it.
The complete browser/editor image has independent source-build targets:

```sh
make desktop-iso DESKTOP_JOBS=4
make desktop-wide-iso DESKTOP_JOBS=4
make run-browser CHROMIUM_ISO=build/desktop-source-wide/image/Coast-desktop.iso \
  CHROMIUM_QEMU_NETWORK='-nic user,model=e1000e' QEMU_DISPLAY=gtk,gl=off
```

Choose the core or wider image; their output paths are documented in the README.
The UEFI launch command needs host OVMF. The README also supplies a BIOS launch
command that works without selected-image links or an external firmware path.

The older `make chromium-browser CHROMIUM_BUILD=build/my-browser` overlay
assembler requires the selected existing Code OSS base ISO and
verified runtime dependencies. It compiles the current kernel, native desktop
and static source-built Wayland bridge, then removes retired browser files from
the inherited initramfs modules. `--bridge-build` selects a bridge build receipt.
For the independent full Electron source graph, use
`tools/build_electron_source.py`. The qualified source desktop reused verified
Electron/Node source builds; an empty full Electron graph build has not been
verified end to end. See [source-builds.md](source-builds.md) for the evidence.

## Downloads

```sh
make fetch-sources
make check-sources
make iso SOURCE_MIRRORS="https://mirror.example.org/coast"
```

The base source manifest pins archive names, HTTPS locations and SHA-256 hashes.
A configured mirror is tried first with the same checksums. Separate port
builders have their own pinned manifests. Source downloads and host tools are
cached locally and never committed. Standalone JavaScriptCore uses the host
bootstrap in `tools/prepare_browser_host.py`; it is retained for the text-core
editor/SDK, without the retired WebCore browser frontend.

## Boot managers and firmware

```sh
make iso BOOTLOADER=limine
make iso BOOTLOADER=grub ISO_IMAGE=build/coast-grub.iso
make iso BOOTLOADER=refind ISO_IMAGE=build/coast-refind.iso
```

Limine is the default. `tools/build_limine.py` compiles its pinned source release
for BIOS and x86-64 UEFI. It needs Clang/LLVM and mtools; it builds a local host
NASM if necessary. GRUB and rEFInd need their host tooling and the native EFI
loader. `BOOT_CONFIG` selects the TOML boot policy, `BOOT_TEMPLATE` selects the
native boot-manager template, and `BOOT_MODULES`/`BOOT_ASSETS` supply guest-to-host
file mappings.

QEMU uses host OVMF, discovered by `tools/host_firmware.py`. Override it with
`OVMF=/path/to/OVMF.fd`. No compiled firmware is part of the source repository.
`QEMU_ACCEL`, `QEMU_DISPLAY`, `SMP`, `MEMORY` and `ISO_IMAGE` select VM settings.
The browser/editor targets use two CPUs and 16 GiB by default.

## Source publication and cleaning

```sh
make source-check test-source-tree
make source-archive
python3 -B tools/clean_cache.py --apply
```

Source archives contain a file/hash inventory and the exact lwIP source for an
offline kernel build. `make clean` cleans the selected kernel/image output.
`make userland-clean` removes generated `userland/bin`, `obj` and `runtime`.
The cache cleaner removes known retired/failed candidates while preserving
selected images, rollback targets, service bundles, toolchains and signing keys.
The audit, Git ignore rules and GitHub checks are documented in
[repository-cleanup.md](repository-cleanup.md).
