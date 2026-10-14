# ModernOS

ModernOS is an experimental 64-bit operating system with its own monolithic
kernel, command shell and native desktop. It boots through Limine, GRUB 2 or
rEFInd and runs static x86-64 programs through its implemented syscall ABI.
Development and validation primarily use QEMU on an x86-64 Linux host.

The complete image includes:

- A desktop with Files, Text Editor, Terminal, Browser, Settings and workspaces.
- Native nano and Vim, BusyBox ash/utilities, GNU awk and GNU make.
- GCC/G++ 14.2.0, binutils, development headers and static C/C++ libraries.
- Git and curl, IPv4/DHCP/DNS, and a WebCore/JavaScriptCore browser with
  certificate-verified HTTPS. NetSurf handles local-file browsing.
- NVMe/SATA storage, read-only ext2/ext4, writable FAT32, USB input and audio.

ISO files and compiled programs are built locally. Source downloads, build
caches, disk fixtures and test results are excluded from Git.

## Host requirements

Use an **x86-64 Linux host**. The kernel can use a compatible host GCC/binutils
toolchain, an `x86_64-elf-` cross compiler, or Clang/LLVM. The complete userland
build also needs native host GCC and G++.

| Task | Required host tools |
| --- | --- |
| Kernel | GNU make, GCC/binutils or Clang/LLVM, Python 3.12+, tar, curl |
| Complete userland and ISO | Host GCC/G++, development headers, pkg-config, Perl, autoconf, automake, libtool, flex, bison, ncurses tools `tic`/`infocmp`, xorriso |
| GRUB or rEFInd ISO | clang, lld-link, mtools (`mformat`, `mcopy`) |
| GRUB | `grub-mkstandalone` or `grub2-mkstandalone` and matching `x86_64-efi` modules |
| rEFInd wallpaper | Pillow available to Python 3 |
| QEMU | `qemu-system-x86_64` with an SDL display/audio backend, and compatible OVMF firmware |
| Storage integration tests | dosfstools, e2fsprogs and mtools |

Install these tools with your Linux distribution's package manager. The build
does not install host system packages. The default QEMU firmware is included
at [`firmware/ovmf/OVMF.fd`](firmware/ovmf/OVMF.fd).

The build automatically prepares **CMake 4.4.4, Ninja 1.13.2, Meson 1.10.2
and Ruby 3.4.7** in its local cache, using pinned downloads and SHA-256 checks.
These tools compile the browser on your host; they are not installed into the
OS image. No manual browser tool setup, pip installation or PATH change is
needed. The [browser build tools guide](Documentation/browser-build-tools.md)
explains the cache and troubleshooting.

The first full build compiles large third-party projects, including GCC and
WebKit. Allow substantial build time, host memory and disk space. Port builders
choose their own parallelism, so `make -j4` does not limit every browser/compiler
sub-build. Subsequent builds reuse local caches.

## Get the source and build

```sh
git clone https://github.com/camden-pina/test.git modernos
cd modernos
```

For a kernel-only build:

```sh
make -j4
```

This creates `build/vmlinux`. A fresh kernel build automatically downloads
and verifies its pinned lwIP source. A kernel ELF alone does not contain the
userland or bootloader needed by the default QEMU launch.

For the complete OS, including browser support:

```sh
make iso
make run-image
```

`make iso` builds the kernel and userland, creates `build/initramfs.tar`, and
packages `build/os.iso`. It downloads and verifies missing sources and prepares
the browser tools automatically. The first build requires network access;
later builds reuse the caches. Limine is the default bootloader. `make run-image`
boots that existing image; `make run` rebuilds it before launching QEMU.

All 44 upstream source archives have pinned HTTPS URLs and SHA-256 hashes in
[`userland/vendor/sources.json`](userland/vendor/sources.json). Builds restore
missing sources and verify existing ones. To verify the source cache offline:

```sh
make check-sources
```

You can download the source archives ahead of time with `make fetch-sources`
and prepare the browser tools with `make browser-host`; both are optional.
Offline builds need cached host tools and the selected bootloader's
cache or installed tools. A checksum mismatch stops the build; preserve or
remove the reported corrupt archive, then fetch it again.

## Choose a bootloader

| Bootloader | Firmware path | Build command |
| --- | --- | --- |
| Limine | BIOS and x86-64 UEFI | `make iso BOOTLOADER=limine` |
| GRUB 2 | x86-64 UEFI | `make iso BOOTLOADER=grub` |
| rEFInd | x86-64 UEFI | `make iso BOOTLOADER=refind` |

Each command writes the selected image to `build/os.iso`. To retain separate
images, choose an output filename:

```sh
make iso BOOTLOADER=grub ISO_IMAGE=build/modernos-grub.iso
make run-image ISO_IMAGE=build/modernos-grub.iso
```

All three boot paths load the same kernel. GRUB and rEFInd launch the
OS-owned EFI loader in `arch/x86/boot/efi/`. Edit boot policy, framebuffer
resolution, kernel/module paths and timeout in
[`arch/x86/boot/boot.conf`](arch/x86/boot/boot.conf), which uses TOML.
`BOOT_CONFIG=` selects another shared configuration. Bootloader-specific
commands, appearance and power controls belong in the native templates:

- [Limine](scripts/bootloaders/limine.conf.in)
- [GRUB](scripts/bootloaders/grub.cfg.in)
- [rEFInd](scripts/bootloaders/refind.conf.in)

`BOOT_TEMPLATE=` selects a custom template for the chosen loader. Packaging
generates each loader's native configuration and `/boot/modernos.conf` for
the EFI stub. Unknown shared settings and missing template placeholders fail
with a clear error. See the build-system guide for the schema, native settings
and `BOOT_ASSETS=` for theme/icon files.
The native EFI images are unsigned.

Use `make iso-image BOOTLOADER=refind` to repackage an existing initramfs while
developing bootloader/kernel changes. For binary overrides, offline bootloader
caches and protocol details, see the
[build-system guide](Documentation/build-system.md#bootloader-selection-and-boot-policy).

## Run in QEMU

The default launch uses four virtual CPUs, 2 GiB of guest RAM, UEFI firmware,
USB keyboard/mouse, e1000e networking, AC97 audio and an SDL/OpenGL display.
It selects KVM when `/dev/kvm` is accessible and TCG otherwise.

```sh
make run-image SMP=4 MEMORY=2G
make run-image QEMU_ACCEL=tcg
make run-image QEMU_DISPLAY=sdl,gl=off
make run-image OVMF=/absolute/path/OVMF.fd
```

Click inside the QEMU window to capture the mouse; **Ctrl+Alt+G** releases it.
On Wayland, the launch uses SDL's native Wayland driver. Set
`QEMU_SDL_DRIVER=x11` when using the X11 backend. Host serial output is also
saved to `build/serial.log`.

## Use ModernOS

At the `os:` shell prompt, start with:

```text
help
help files
help text
ls /
ps
cpus
gui
```

`gui` opens the native desktop. Use **Ctrl+Alt+Escape** or the launcher's
**Return to console** action to exit. Files and Text Editor work with the
actual filesystem; Terminal runs a shell over a native pseudoterminal.
Settings provides wallpaper, scaling, light/dark appearance and workspaces.
Use **Save preferences** or **Ctrl+S** in Settings to save changes.

For editing and POSIX shell commands:

```text
nano /tmp/notes.txt
vim /tmp/notes.txt
build-sh
build-sh -c 'printf "b\na\na\n" | sort | uniq'
```

The initial `modernos-sh` console provides OS commands and basic command
syntax. `build-sh`, `sh` and `ash` provide POSIX scripts, variables, pipes,
redirection and wildcard expansion.

### Keep files across reboots

The root filesystem, `/tmp` and `/home/user` are in RAM. Save persistent
documents to a writable FAT32 mount. Attach raw disk images to QEMU with:

```sh
make run-image FAT_DISK=/absolute/path/data.img
make run-image LINUX_DISK=/absolute/path/source.img
make run-image SATA_DISK=/absolute/path/sata.img
```

ModernOS discovers supported filesystems and mounts them under `/mnt`.
Run `lsblk` and `mounts` inside the OS to find their actual names. FAT32 is
writable; ext2/ext4 are read-only. Files on an attached writable image can be
modified by guest applications. The [storage guide](Documentation/kernel-userland.md)
explains image layout, mount commands and filesystem limits.

### Build programs inside ModernOS

The complete image installs SDK examples under `/usr/src/modernos/examples`.
Copy them recursively, including their headers, before building:

```text
build-sh -c 'mkdir -p /tmp/example; cp -R /usr/src/modernos/examples/. /tmp/example/; cd /tmp/example; make -j2 check'
```

GCC/G++ produce static x86-64 executables using the included C/C++ libraries.
For the long native compiler source rebuild, use at least 8 GiB of guest RAM:

```sh
make run-image MEMORY=8G
```

Then run `bootstrap-toolchain -j4` inside ModernOS. See the
[native-tools guide](Documentation/native-tools.md) for compiler use, ABI
limits, checkpoints and exporting a rebuilt SDK.

## Test and develop

These host checks do not need a complete OS ISO:

```sh
make test-source-fetch test-browser-build test-boot-config test-efi-loader test-kernel-memory
```

Guest checks build an ISO and use QEMU:

```sh
make test
make test-desktop
make test-native
make test-compiler
make test-network
make test-native-browser
```

`make test` runs the core runtime, console-input and configure-shell checks;
it is a subset of the available tests. Logs, temporary images and recordings
stay under `build/`. See the
[test inventory](Documentation/repository-cleanup.md#test-scripts-and-shared-helpers)
for all scripts, shared fixtures and optional live website/display checks.

Useful development commands:

```sh
make O=out -j4
make V=1
make CROSS_COMPILE=x86_64-elf-
make LLVM=1
make clean
make O=out clean
make userland-clean
make help
```

`O=` selects the kernel/image output directory; use the same value for later
packaging, launches and cleaning. Userland and port caches remain shared.
`make clean` preserves shared downloads and test artifacts; `make userland-clean`
removes generated userland separately. See the
[cleanup guide](Documentation/repository-cleanup.md#deleting-local-artifacts)
before removing complete caches or checkpoints.

## Current limits

- Only x86-64 is supported. QEMU is the main validation environment; physical
  hardware compatibility depends on the implemented drivers.
- Graphics use software rendering and firmware framebuffers.
  Native Intel/NVIDIA modesetting and synchronized page flipping remain work
  in progress.
- Static executables use a documented syscall subset. Dynamic linking,
  full POSIX job control and arbitrary Linux application compatibility are
  outside the current supported ABI.
- Website compatibility remains incomplete, including some verification
  flows. Browser/media checks do not imply every website will work.

## Source layout and documentation

| Path | Purpose |
| --- | --- |
| `arch/x86/`, `init/` | CPU-specific code, boot adapters and kernel startup |
| `kernel/`, `mm/`, `fs/`, `net/`, `drivers/` | Kernel services, memory, filesystems, networking and hardware |
| `include/`, `lib/` | Kernel interfaces, memory/string helpers, fonts and LAI |
| `userland/` | Applications, desktop, browser ports and native SDK sources |
| `scripts/`, `tools/` | Build rules, source restoration, packaging and tests |
| `toolchain/` | Optional kernel cross-compiler build recipe |
| `firmware/`, `Documentation/` | QEMU firmware and detailed guides |

Read the [build-system guide](Documentation/build-system.md),
[desktop guide](Documentation/desktop.md),
[kernel and storage guide](Documentation/kernel-userland.md),
[native tools guide](Documentation/native-tools.md), and
[browser port guide](Documentation/modern-browser-port.md) for subsystem details.

Third-party license and attribution files are retained in
[`userland/vendor/licenses/`](userland/vendor/licenses/), the bundled LAI
tree and font assets. Their notices are included in built images where
configured by the packaging tools. The original OS code currently has no
top-level license declaration; third-party components retain their own terms.
