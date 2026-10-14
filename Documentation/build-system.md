# Kernel layout and build system

ModernOS builds a monolithic x86-64 ELF kernel booted by Limine, GRUB 2 or
rEFInd. The source layout
and recursive build follow the conventions described in the Linux
[kernel Makefiles guide](https://docs.kernel.org/kbuild/makefiles.html) and
[Kbuild command reference](https://docs.kernel.org/kbuild/kbuild.html).
This project implements the subset needed by its existing kernel: explicit
`obj-y` lists, directory archives, dependency tracking, and separate output
directories. It has no Kconfig configuration step or loadable module support.
AArch64 support has been removed; `ARCH=x86` and `ARCH=x86_64` both select the
x86-64 kernel.

## Source ownership

| Path | Contents |
| --- | --- |
| `Makefile` | Public build commands, output/toolchain selection, final link, ISO packaging |
| `Kbuild` | Ordered list of kernel component directories |
| `arch/x86/` | Architecture flags, CPU and interrupt code, page tables, linker script, boot configuration/background and native EFI stub |
| `arch/x86/include/asm/` | Architecture headers; existing include spellings are kept through the architecture include path |
| `init/` | Shared boot handoff validation and kernel startup |
| `kernel/` | Logging, panic handling, scheduling, processes, signals, synchronization and syscall dispatch |
| `mm/` | Physical allocation, virtual memory, user memory and allocation bitmaps |
| `fs/` | VFS, filesystem implementations, descriptor facilities and ELF loading |
| `net/` | Network integration, Unix sockets and the lwIP source manifest |
| `drivers/` | ACPI, audio, block storage, PCI, clocks, input, NICs, terminals, USB and video |
| `lib/` | Kernel strings, formatting, fonts and the bundled LAI interpreter |
| `include/` | Shared kernel headers, subsystem interfaces and `uapi/` ABI definitions |
| `scripts/` | Shared recursive build/clean rules, bootloader recipes and port/emulator/test targets |
| `userland/` | Native programs, application resources and their separate build |
| `tools/` | Host builders, packaging tools and validation programs |
| `toolchain/` | Cross compiler build recipe and preserved compiler installation/cache |
| `firmware/ovmf/OVMF.fd` | Default UEFI firmware for QEMU |
| `Documentation/` | Build, kernel, userland and application documentation |

Existing binaries, old kernel outputs and miscellaneous generated files were
preserved under `build/legacy-artifacts/`. Existing large download, compiler,
browser and library caches remain under `build/`.

## Commands

Run these from the repository root:

```sh
make -j4                         # Kernel only: build/vmlinux
make kernel                      # Same kernel build
make vmlinux                     # Same kernel build
make O=out -j4                   # Kernel output in out/vmlinux
make O=. -j4                     # Kernel objects and vmlinux in the source tree
make V=1                         # Show full build commands
make CROSS_COMPILE=x86_64-elf-    # Select GCC, ld and ar with this prefix
make LLVM=1                      # Select clang, ld.lld and llvm-ar
make iso                         # Build userland, initramfs and build/os.iso
make iso BOOTLOADER=grub         # Install GRUB 2 into an x86-64 UEFI ISO
make iso BOOTLOADER=refind       # Install rEFInd into an x86-64 UEFI ISO
make iso-image BOOTLOADER=grub   # Package the existing initramfs; no userland build
make O=out iso                   # Package out/os.iso
make run SMP=4                   # Rebuild the ISO and boot it in QEMU
make run BOOTLOADER=refind SMP=4 # Rebuild and boot the selected loader
make run-image                   # Boot the existing selected ISO
make test-boot-config            # Host config/selection/staging checks
make fetch-sources               # Download and verify pinned upstream sources
make check-sources               # Verify local sources without downloading
make test-source-fetch           # Host source-cache failure and integrity checks
make help                        # Show the common build commands
```

The kernel needs GNU make, an x86-64 GCC/binutils toolchain or the LLVM tools,
Python 3.12+ and tar. A compatible host GCC/binutils toolchain also works. The
Makefile uses `toolchain/opt/cross/bin` when present and automatically selects
`x86_64-elf-` when that compiler is available. `CC`, `LD`, `AR`, `KCFLAGS`,
`KCPPFLAGS` and `KAFLAGS` can be supplied explicitly. ISO builds also need the
userland dependencies listed in the [README](../README.md), curl and xorriso;
QEMU runs need `qemu-system-x86_64` and OVMF. `OVMF=/path/to/firmware` selects
another firmware image.
Upstream source archives are local caches rather than Git files. The first
kernel build downloads only lwIP; userland restores the full source set before
building its upstream ports. `userland/vendor/sources.json` records their
original filenames, HTTPS URLs and SHA-256 hashes, matching the existing port
builders. `make fetch-sources` prepares the cache, and `make check-sources`
checks it offline. Downloads require curl; a failed transfer or checksum never
installs an incomplete archive. Keep the prepared source and bootloader caches
for offline builds.
Stalled downloads abort after 30 seconds below 1 KiB/s; each transfer has a
600-second limit. When another source is available, a network/HTTP failure
immediately advances to that URL. The final URL retains two retries within a
120-second retry window, plus an IPv4 fallback for connection failures.
Package mirrors are recorded in `sources.json`; every archive must match the
same pinned SHA-256 regardless of the server. Existing corrupt cache files and
downloaded files with a wrong checksum fail the build instead of being installed.

To use your own HTTPS mirror for all source archives and browser host tools:

```sh
make iso SOURCE_MIRRORS="https://mirror.example.org/modernos"
make fetch-sources SOURCE_MIRRORS="https://mirror-a.example.org/modernos https://mirror-b.example.org/modernos"
python3 tools/fetch_sources.py busybox-1.37.0.tar.bz2 --mirror https://mirror.example.org/modernos
```

Each mirror uses a flat directory of the cached archive filenames, such as
`modernos/busybox-1.37.0.tar.bz2`. Configured mirrors are tried first, followed
by the primary URL and package mirrors. Keep the cached files byte-for-byte
identical; repacked sources will fail verification. `SOURCE_MIRRORS` can also be
exported in the environment; repeatable `--mirror` options override it for the
fetcher CLI. Browser host-tool archives use the names in
`tools/prepare_browser_host.py` (including the `modernos-` prefixes), and are
cached separately under `build/modern-browser/downloads/`.
The optional cross-toolchain build and rEFInd preparation use the same fetcher
and `SOURCE_MIRRORS`; rEFInd's cached filename is `refind-bin-0.14.2.zip`.
Some pinned snapshots have no verified public mirror. A project mirror can
serve these exact cached bytes as well.

The full browser automatically prepares its pinned CMake/Ninja/Meson/Ruby
cache. `make iso` needs no separate browser setup step. `make browser-host`
can prepare just those tools in advance; see [Browser build tools](browser-build-tools.md)
for their purpose, cache locations and failure logs.
Existing test checkpoint manifests accept the relocated firmware path when its
saved checksum still matches; their files and other compatibility checks stay
unchanged.

`O=` selects the kernel output directory and takes precedence over the
`KBUILD_OUTPUT` environment variable. Its default is `build/`. Kernel objects,
dependency files, command records, archives and `vmlinux` go there, alongside
the initramfs and ISO staging/output files. A generated output-directory
Makefile also permits `make -C out` after the first `make O=out` invocation.
Use the same `O=` value for subsequent builds, packaging, runs and cleaning.
Makefile test targets pass the selected ISO and initramfs through the
`MODERNOS_ISO` and `MODERNOS_INITRAMFS` environment variables. Explicit test
`--iso` arguments still take precedence, and direct script invocations retain
the default `build/` paths when these variables are absent.

Userland binaries/objects/runtime files and third-party preparation caches
remain shared in `userland/` and `build/`, even with `O=out`. In particular,
lwIP is prepared into `build/lwip/`; its kernel objects go into the selected
output directory. The existing port builders and integration tools may also
use shared `build/` paths. Separate `O=` directories isolate kernel builds,
but do not create independent userland or third-party workspaces.

The host kernel filename is `vmlinux`. ISO packaging copies it and the
initramfs to the paths specified in `arch/x86/boot/boot.conf`, currently
`/boot/kernel.elf` and `/boot/initramfs.tar`.

## Bootloader selection and boot policy

`BOOTLOADER=limine` is the default and preserves Limine's BIOS and x86-64 UEFI
support. `BOOTLOADER=grub` and `BOOTLOADER=refind` build x86-64 UEFI images.
The selector is checked immediately; unsupported values fail with the valid
choices. The top-level Makefile includes only the selected recipe from
`scripts/bootloaders/`. Each ISO build replaces its staging tree, so changing
the selector in the same `O=` directory removes the previous loader's files.
All choices use the same `vmlinux`, initramfs and kernel subsystems.

Limine passes its protocol to a kernel adapter. GRUB and rEFInd load the
OS-owned PE32+ EFI stub at `/EFI/ModernOS/modernos.efi`, which reads the ELF
kernel and modules using UEFI filesystem services and prepares the shared
kernel handoff. GRUB uses its documented
[`chainloader`](https://www.gnu.org/software/grub/manual/grub/html_node/chainloader.html)
command; rEFInd uses a manual
[loader stanza](https://www.rodsbooks.com/refind/configfile.html).
This follows the separation illustrated by Linux's
[EFI stub](https://docs.kernel.org/admin-guide/efi-stub.html): boot managers
start an OS-owned EFI entry, and the kernel consumes normalized boot data.
ModernOS's ELF/stub interface does not implement Linux's `bzImage` protocol
or Multiboot2. Neither alternative embeds or invokes Limine.

`include/boot/info.h` defines the versioned shared handoff. The Limine adapter
is `arch/x86/boot/limine.c`; the firmware adapter is
`arch/x86/boot/efi/loader.c`, with its assembly transition in `transition.S`.
`init/boot.c` validates either handoff before `init/main.c` starts the kernel.
Memory management, VFS, graphics, ACPI and SMP consume this shared interface.
An additional protocol belongs in its own adapter, which supplies the same
memory map, framebuffer, modules, firmware tables and command line; it does
not require another kernel implementation. New boot-manager packaging belongs
in a separate `scripts/bootloaders/` recipe and config generator.

`arch/x86/boot/boot.conf` is the shared source of boot policy, written in TOML.
`BOOT_CONFIG=` selects another file. Its supported settings are:

| Table | Settings |
| --- | --- |
| Top level | Required `version = 1` |
| `[boot]` | Required `title` and `kernel`; optional `resolution` and `cmdline` |
| `[[boot.modules]]` | Required `path`, optional `string`; one table per module |
| `[menu]` | Optional `timeout`, `resolution` and `wallpaper` |

Paths are absolute paths on the boot volume. The menu resolution falls back
to the boot resolution when omitted. Empty resolutions allow automatic mode
selection; empty or omitted wallpaper disables the shared background image.
Menu timeout is an integer from -1 to 9999: -1 waits indefinitely, 0 boots
immediately, and positive values count seconds. Packaging maps these meanings
to each loader's syntax, including rEFInd's reversed 0/-1 meanings.
Unknown keys, incorrect types, duplicate payload paths and unrepresentable
settings fail with a clear error. For example, GRUB/rEFInd require a 32-bit
kernel framebuffer, and rEFInd cannot select a 16/24-bit menu depth.

Native settings live in three editable templates:

| Template | Generated file inside the image |
| --- | --- |
| `scripts/bootloaders/limine.conf.in` | `/boot/limine/limine.conf` |
| `scripts/bootloaders/grub.cfg.in` | `/boot/grub/grub.cfg` |
| `scripts/bootloaders/refind.conf.in` | `/EFI/BOOT/refind.conf` |

The selected template is rendered using the shared values. Native commands
are preserved in that loader's syntax and apply only to that loader. Wallpaper
scaling, power-control labels, themes, icons, menu behavior and other advanced
settings belong here. Limine can use `wallpaper_style: tiled`, GRUB can use
`set timeout_style=hidden`, and rEFInd can use `big_icon_size 128` without
requiring equivalent settings in the other templates. The native loader
interprets these commands; packaging validates the shared schema and template
placeholders, not every native command or hardware capability. See the
[Limine configuration reference](https://github.com/Limine-Bootloader/Limine/blob/v12.x/CONFIG.md),
[GRUB configuration reference](https://www.gnu.org/software/grub/manual/grub/grub.html),
and [rEFInd configuration reference](https://www.rodsbooks.com/refind/configfile.html).

Templates use `{{BOOT_ENTRY}}` exactly once, `{{TIMEOUT}}`,
`{{MENU_RESOLUTION}}`, `{{WALLPAPER}}` and an optional `{{HEADER}}`.
`{{BOOT_ENTRY}}` expands to the OS entry, and `{{TIMEOUT}}` to a native timeout
value. `{{MENU_RESOLUTION}}` and `{{WALLPAPER}}` expand to complete native
directives or empty text.
Removing a placeholder needed by a shared setting, misspelling one or omitting
the boot entry fails before old staging is replaced. The default templates
preserve the existing OS entry, resolution, background scaling and power
controls. Add native settings alongside the generated settings; intentional
native overrides follow the selected loader's own precedence rules.

To maintain a separate GRUB customization, copy its template, add native
commands and select it when building:

```sh
cp scripts/bootloaders/grub.cfg.in my-grub.cfg.in
make iso BOOTLOADER=grub BOOT_TEMPLATE=my-grub.cfg.in
```

`BOOT_ASSETS=` copies additional files needed by native templates into the
image. It is a space-separated list of `GUEST_PATH=HOST_PATH` mappings, such as:

```sh
make iso BOOTLOADER=grub BOOT_TEMPLATE=my-grub.cfg.in \
  BOOT_ASSETS='/boot/themes/custom/theme.txt=themes/custom/theme.txt /boot/themes/custom/background.png=themes/custom/background.png'
```

In the template, a matching GRUB theme can use
`set theme="/boot/themes/custom/theme.txt"`. Use host filenames without spaces
in Make's mapping lists. Missing assets, conflicting destinations and inputs
inside the disposable staging tree fail before staging is cleared. rEFInd's
bundled icons are copied without overwriting explicitly supplied icon files.
Use consistent directory casing, including `/boot` and `/EFI/BOOT`, because
UEFI FAT volumes cannot contain separate directories distinguished by case.
The shared wallpaper is found under its guest path in the source tree,
beside `BOOT_CONFIG`, or in `arch/x86/boot/`, using the configured basename.
Its default source is `arch/x86/boot/boot-background.jpg`, packaged for all
loaders at `/boot/boot-background.jpg`. rEFInd packaging also generates its
PNG banner from this shared image.

The generated `/boot/modernos.conf` contains only the OS loading settings.
GRUB/rEFInd kernel arguments come from its generated `cmdline` field; configure
them through `[boot].cmdline` in the shared file.
The kernel continues to receive normalized boot information through
`boot_info`; it does not read the templates or TOML file. The requested kernel
resolution falls back to an available firmware mode when the OS stub cannot
find an exact match. Existing Limine-syntax `BOOT_CONFIG=` files must be
converted to TOML; native Limine additions move into its template.

The native EFI configuration contains `kernel=/boot/kernel.elf`,
`resolution=2560x1600x32`, `cmdline=...`, then repeated `module=/path` and
`module_string=...` pairs. The module whose string is `initramfs` is copied from
`INITRAMFS=` (default `$(O)/initramfs.tar`). Further modules default to their
guest path under the repository; override any module source with
`BOOT_MODULES='/boot/extra.bin=/host/extra.bin'`. Same-volume absolute paths
and `boot():/` paths are supported. The EFI stub accepts at most 32 modules
and a 64 KiB config, with ASCII paths/command lines/module strings limited
to 511 bytes. Native EFI requires four-level x86-64 paging and a 32-bit GOP
framebuffer, and limits each loaded kernel/module file to 1 GiB. Unsupported
paths, framebuffer depths, oversized files and ambiguous multiple kernel
entries fail during packaging.

GRUB/rEFInd ISOs contain a FAT32 EFI El Torito image with the selected manager,
the OS stub, kernel, modules and configuration. Firmware can therefore read
the OS payload without an ISO9660 filesystem driver. The FAT image is also
exposed as an EFI partition for a raw-written USB ISO. Boot files are retained
in the ISO filesystem as well; this makes the resulting image larger than
the Limine image. These builds need `mformat`/`mcopy` (mtools), `clang` and
`lld-link`, in addition to xorriso and the normal build dependencies.

GRUB needs `grub-mkstandalone` (or `grub2-mkstandalone`) and matching
`x86_64-efi` modules. Set `GRUB_MKSTANDALONE=/path/to/grub-mkstandalone` and
`GRUB_EFI_DIR=/path/to/grub/x86_64-efi` for a locally extracted toolset.
An extracted toolset in `build/grub-host/usr/` is detected automatically.
`GRUB_FONT=` selects an optional Unicode font; the packaged/system GRUB font
enables the graphical menu and configured background. Without it, the menu
uses firmware text output and the OS stub still requests the configured mode.

rEFInd defaults to official release 0.14.2. Its archive is downloaded into
`build/refind-bin-0.14.2.zip`, checked against a pinned SHA-256, and extracted
locally. `REFIND_EFI=/path/to/refind_x64.efi` uses an existing binary;
an adjacent `icons/` directory is packaged when present. `REFIND_CACHE=` and
`REFIND_DIR=` select an alternate download cache or existing extracted release.
Pillow (`python-pillow` on Arch,
`python3-pil` on Debian) converts the shared JPEG wallpaper to rEFInd's PNG
banner format. Cached bootloader assets and explicit tool/binary overrides
permit offline packaging; no build command installs packages or changes the
host firmware's boot entries. The EFI images are unsigned.

For a boot regression test, package one ISO for each selector with
`make iso-image BOOTLOADER=... ISO_IMAGE=build/bootloader-tests/NAME.iso`,
then run:

```sh
python3 tools/test_bootloaders.py \
  --limine build/bootloader-tests/limine.iso \
  --grub build/bootloader-tests/grub.iso \
  --refind build/bootloader-tests/refind.iso --cpus 1 4 --bios
```

`--bios` also exercises Limine's legacy firmware path. The host-only
`make test-boot-config` checks the TOML schema, portable defaults, native
template selection, asset staging, invalid selectors and replacement of the
staging tree when selectors change. Invalid templates and asset conflicts
are checked without destroying an existing staging tree.

## Recursive and incremental builds

The top Makefile includes `arch/x86/Makefile` for architecture flags and the
linker script. `scripts/Makefile.build` reads root `Kbuild`, then each listed
directory's `Makefile`. Each directory explicitly declares its objects and
child directories through `obj-y`; adding a source file alone does not add it
to the kernel. The declared order determines archive and final link order.

Each directory creates a thin `built-in.a` archive, which references its
compiled objects. Child archives are combined up to the output root's
`built-in.a`, and the final link produces `vmlinux` with
`arch/x86/kernel/vmlinux.lds`. These archives depend on their object files and
are not standalone distribution artifacts.

C and preprocessed assembly compilation write hidden `.d` files for header
dependencies. Hidden `.cmd` files record the compile/archive commands, and
`vmlinux.cmd` records the linker command. Unchanged sources and commands reuse
existing outputs; changing compiler options rebuilds affected objects and
archives. Raw assembly includes need explicit Makefile prerequisites, as
shown for `arch/x86/kernel/kernel.inc`.

## Adding sources

Place a source in the subsystem that owns its behavior, then add its object
to that directory's Makefile:

```make
obj-y += new_driver.o
```

For a new child directory, add `obj-y += child/` in its parent and create
`child/Makefile` with the child's object list. Each object must have one
matching `.c`, `.s`, `.S` or `.asm` source; ambiguous stems are rejected.
Use `ccflags-y` or `asflags-y` for options confined to that directory.
lwIP is the special case: `net/lwip/Makefile` lists the pinned upstream source
objects, and the shared rules compile them from `build/lwip/src`.

## Cleaning

```sh
make clean                       # Clean generated kernel/images in build/
make O=out clean                 # Clean generated kernel/images in out/
make userland-clean              # Explicitly clean shared generated userland
```

Kernel cleaning follows the declared source directories and removes generated
objects, dependencies, command files, archives, `vmlinux`, the native EFI stub,
the selected ISO, initramfs and ISO staging tree. It retains source files, the generated output
wrapper, archived previous artifacts and shared third-party caches. Cleaning
userland is a separate command because those outputs are shared across kernel
output directories.
