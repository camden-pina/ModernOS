# Build commands and optional arguments

Repository: https://github.com/camden-pina/ModernOS.git.
See [host setup](host-setup.md) for prerequisites. After setup:

```sh
make iso
make run
```

`make iso` builds the complete Coast desktop from pinned sources: kernel,
native desktop/accounts, Chromium, Code OSS, GCC/G++, Bash, Python,
Coast Packages/makepkg, Qt, QEMU and its Chromium VM viewer. It resumes verified
completed stages. `make run` boots the corresponding existing ISO in QEMU;
run `make iso` again when you want to rebuild it. Enter `gui` at the guest console.

## Build arguments

Pass options as `NAME=value` after the command. These examples are alternatives:

```sh
make iso JOBS=2
make iso PROFILE=native
make iso PROFILE=core
make iso ISO_BUILD=build/my-desktop
make iso ISO_ARGS=--plan
```

| Argument | Default | Meaning |
| --- | --- | --- |
| `PROFILE` | `full` | `full` builds the complete desktop. `core` builds Chromium/Code OSS, native GCC/G++, Bash and package tools, omitting the Python/makepkg/Qt/QEMU integrations. `native` builds the small native desktop without Electron. `staged` packages the development userland staging area described below. |
| `JOBS` | `4` | Parallelism for the source pipeline, including nested component builds; use 1–24 for full/core. Use this instead of `make -jN` to control pipeline workers. |
| `ISO_BUILD` | Profile dependent; see below | Directory for the source pipeline's receipts, logs and image. Use a new directory when changing inputs or diagnostics. Applies to full/core/native. |
| `ISO_ARGS` | Empty | Additional arguments to the selected full/core or native Python builder, listed below. |
| `O` | `build` | Kernel output directory and parent of the default `ISO_BUILD`. Shared downloads and some toolchains still use the repository's `build/`. `KBUILD_OUTPUT` is the equivalent environment variable; `O` takes precedence. |
| `SOURCE_MIRRORS` | Empty | Space-separated HTTPS mirrors tried before the base source manifest's upstream URLs, with the same checksum verification. Individual ports also use their own pinned manifests. |

| Profile | Default ISO |
| --- | --- |
| `full` | `build/desktop-source-wide/image/Coast-desktop.iso` |
| `core` | `build/desktop-source/image/Coast-desktop.iso` |
| `native` | `build/native-image/Coast-native.iso` |
| `staged` | `build/os.iso` |

For full/core, the image is `ISO_BUILD/image/Coast-desktop.iso`; for native it is
`ISO_BUILD/Coast-native.iso`. Keep the same `PROFILE`, `ISO_BUILD` and `O` values
when running the image:

```sh
make iso PROFILE=native JOBS=2
make run PROFILE=native
```

Full/core `ISO_ARGS` options:

| Option | Meaning |
| --- | --- |
| `--plan` | Print every planned stage as JSON without downloading or building. |
| `--qualify` | Include editor diagnostics and run the profile's guest qualification suites after building; requires the extra host test dependencies in [host setup](host-setup.md). |
| `--diagnostics` | Include opt-in Code OSS diagnostics without running qualification. |
| `--electron-checkout PATH` | Reuse a verified Electron source checkout read-only. |
| `--runtime-build PATH` | Reuse a verified, matching Electron/Node receipt; also requires `--electron-checkout`. |
| `--components PATH` | JSON map of component names to verified source-build receipts; see [source builds](source-builds.md). |
| `--repository-config PATH` | Explicit public package repository trust configuration; requires `PROFILE=full`. |
| `--with-microsoft-auth` | Include upstream proprietary Microsoft sign-in helpers. |
| `--help` | Show the builder's complete option help without building. |

For example, `make iso ISO_BUILD=build/desktop-qa ISO_ARGS=--qualify` builds and
qualifies the full image. Quote multiple options as
`ISO_ARGS='--diagnostics --with-microsoft-auth'`. Full already enables
`--with-integrations`; this argument is unnecessary on the default command.
The native builder accepts `--kernel PATH` to reuse a kernel from this checkout,
and `--help`. `ISO_ARGS=--plan` applies only to full/core; use
`make -n iso PROFILE=native` to print the native command without executing it.

## Run arguments

The default VM uses BIOS, USB keyboard/mouse, e1000e user networking, four CPUs
and 16 GiB RAM. Native/staged default to 2 GiB. KVM is selected when `/dev/kvm`
is accessible; otherwise QEMU uses software emulation. An explicit `ISO_IMAGE`
selects an existing image to boot; it does not rename full/core/native build outputs.

```sh
make run MEMORY=24G SMP=4
make run QEMU_ACCEL=kvm
make run ISO_IMAGE=build/my-desktop/image/Coast-desktop.iso
make run FIRMWARE=uefi OVMF=/path/to/OVMF.fd
make -n run
```

`make -n run` prints the launch command without starting a VM.

| Argument | Default | Meaning |
| --- | --- | --- |
| `PROFILE`, `ISO_BUILD`, `O` | Same as the build | Select the profile's image and resource defaults. |
| `ISO_IMAGE` | Profile's ISO path | Override the image to boot. |
| `MEMORY` | `16G` for full/core; `2G` for native/staged | Guest RAM, in QEMU size syntax. |
| `SMP` | `4` | Guest CPU count. |
| `QEMU_ACCEL` | `kvm` when accessible, otherwise `tcg` | Accelerator; explicit KVM fails if unavailable. |
| `QEMU_CPU` | `host` with KVM, otherwise `max` | QEMU CPU model. |
| `QEMU_DISPLAY` | `gtk,gl=off` | Display backend, such as `sdl,gl=on` or `none`. |
| `QEMU_SDL_DRIVER` | Inherited `SDL_VIDEODRIVER`, otherwise `wayland` when available | SDL video backend. |
| `QEMU_GTK_BACKEND` | Inherited `GDK_BACKEND` | GTK display backend. |
| `FIRMWARE` | `bios` | `bios` uses QEMU's built-in BIOS; `uefi` uses OVMF. |
| `OVMF` | Discovered host firmware | UEFI firmware file used with `FIRMWARE=uefi`. |
| `QEMU_VIDEO` | VGA with 32 MiB video memory, 120 Hz EDID | Full video device arguments. |
| `QEMU_NETWORK` | `-nic user,model=e1000e` | Full network arguments; use `'-nic none'` to disable networking. |
| `QEMU_AUDIO` | SDL audio and AC97 | Full audio arguments; set `QEMU_AUDIO=` to disable the sound device. |
| `LINUX_DISK`, `FAT_DISK` | Empty | Paths to raw disk images attached as NVMe devices. |
| `SATA_DISK` | Empty | Raw disk image attached as SATA. |
| `QEMU_EXTRA_ARGS` | Empty | Additional QEMU arguments appended to the command. |

Serial output is also recorded in `O/serial.log`. Disk arguments attach the
specified images with write access; use disposable images for testing.

## Host setup, sources and maintenance

| Command | Optional arguments / behavior |
| --- | --- |
| `make setup-host` | Explicitly install Ubuntu/Debian prerequisites. `PROFILE=native` selects the smaller prerequisite set. `HOST_ARGS` forwards helper arguments; use `HOST_ARGS=--help` for its options. For inspection without installation, run `python3 -B tools/setup_host.py --print` or `--check` directly; both accept `--native`. |
| `make fetch-sources` | Download and verify the base pinned source archives; accepts `SOURCE_MIRRORS`. Component builders fetch their own sources as needed. |
| `make check-sources` | Verify cached base sources without downloading. |
| `make source-check` | Audit publication inputs for generated binaries, downloads and secrets. |
| `make source-archive` | Export `O/coast-source.tar.xz`, including the source inventory and lwIP for an offline kernel build. |
| `make clean` | Clean the selected kernel output, initramfs, ISO file and staging directory. It does not remove the source pipeline's component caches. Use the same profile/output arguments as the build. |
| `make userland-clean` | Remove shared generated `userland/bin`, `obj` and `runtime`. |
| `make help` | Show the short command reference. |

The cache cleaner previews removals with `python3 -B tools/clean_cache.py`;
`--apply` performs them. See [cache cleanup](repository-cleanup.md) for its
protection rules and options. Component-specific development commands remain
in their [source-build](source-builds.md), [Qt](qt6.md), [QEMU](qemu.md),
[program-port](program-ports.md) and other port guides; use each tool's `--help`
for its complete argument list.

## Kernel and staged userland development

`make` alone builds only the kernel at `O/vmlinux` using Coast's Kbuild-style
system. `make -j4` sets kernel compiler parallelism, `V=1` prints compiler
commands, `CROSS_COMPILE=prefix-` chooses a GCC cross compiler and `LLVM=1`
selects Clang/LLVM. `make desktop-kernel` builds the Chromium ABI feature profile
from `ports/chromium/kernel-flags.json`. These settings apply to the kernel and
staged workflows; source-pipeline components use their own recorded toolchains.

For development ports that install files into `userland/bin` and
`userland/runtime`, package that staging area with:

```sh
make iso PROFILE=staged
make run PROFILE=staged
```

The staged profile builds the in-tree kernel/userland and initramfs. It supports
these additional arguments:

| Argument | Meaning |
| --- | --- |
| `REPACK=1` | Reuse the existing initramfs instead of rebuilding userland/initramfs. |
| `ISO_IMAGE` | Output ISO filename, default `O/os.iso`. |
| `BOOTLOADER` | `limine` (default, BIOS/UEFI), `grub` (UEFI), or `refind` (UEFI). GRUB/rEFInd require their host tooling. |
| `BOOT_CONFIG` | Shared TOML boot policy, default `arch/x86/boot/boot.conf`. |
| `BOOT_TEMPLATE` | Native loader template, defaulting to the selected loader's template in `scripts/bootloaders/`. |
| `BOOT_MODULES`, `BOOT_ASSETS` | Space-separated `GUEST_PATH=HOST_PATH` file mappings for the loader. |
| `LIMINE_BUILD`, `LIMINE_DIR` | Limine source output root and bootloader binary directory; defaults under `build/limine-source`. |
| `AUDIO_FIRMWARE` | Optional AW88399 firmware blob; default `/lib/firmware/aw88399_acf.bin` when present. |
| `XORRISO`, `MFORMAT`, `MCOPY` | Host ISO/FAT tool commands. |

For example, `make iso PROFILE=staged BOOTLOADER=grub ISO_IMAGE=build/coast-grub.iso`
selects GRUB. Boot that image with `make run PROFILE=staged
ISO_IMAGE=build/coast-grub.iso FIRMWARE=uefi`. Bootloader overrides apply to
staged builds; full/core/native use their source-built Limine recipe.
Existing in-tree guest test targets select staged assembly automatically.

See [source builds](source-builds.md) for receipts, package coverage and remaining
prebuilt inputs. The qualified full desktop uses verified source-built
Electron/Node components; an empty full Electron graph build has not yet been
verified end to end.
