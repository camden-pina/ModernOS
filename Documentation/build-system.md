# Build commands and optional arguments

Repository: https://github.com/camden-pina/ModernOS.git.
See [host setup](host-setup.md) for prerequisites. After setup:

```sh
make menuconfig
make iso
make run
```

`make iso` builds Coast: the kernel, desktop/accounts, Chromium, Code OSS,
GCC/G++, Bash, Python, Coast Packages/makepkg, Qt, QEMU and its Chromium VM
viewer. Every integration is part of the build. The output is
`build/coast/image/Coast.iso`.

The build resumes verified completed stages. `make run` boots the existing ISO
in QEMU. Run `make iso` again after source changes, then enter `gui` at the guest
console to open the desktop.

## Terminal configuration

`make menuconfig` opens the terminal wizard with the saved `.config`, or defaults
on the first run. It groups build resources, verification/features, source reuse,
package trust, VM resources, display/firmware, networking, audio, disks and extra
arguments. Every OS component stays included.

Use arrow keys to move, Enter to edit, Space to toggle/cycle and Tab to move
between groups. `?` shows help and `/` searches option names, labels and help.
`S` or F2 saves, `L` or F3 loads another file, `D` restores defaults and `Q` exits.
Text fields support Ctrl+U to clear and Esc to cancel. Exiting with unsaved
changes offers save, discard or cancel. Save and exit before starting the build.

Make automatically reads `.config` on later invocations. Reopening the wizard
reloads it. Select a named settings file consistently with:

```sh
make menuconfig CONFIG=workstation.config
make iso CONFIG=workstation.config
make run CONFIG=workstation.config
```

Command-line assignments override saved settings for that invocation, such as
`make iso JOBS=2` or `make run MEMORY=24G`. They do not modify the saved file.
Without a saved configuration, the normal defaults apply.

`make config` provides a plain terminal questionnaire: Enter keeps a value,
`?` shows help, `-` clears a field and `q` cancels. `make defconfig` writes the
defaults without interaction; it also accepts `CONFIG=path`. Saving changed
settings preserves the previous file as `<filename>.old`.

The file uses validated TOML, with sections matching the wizard's groups.
Missing options receive defaults; empty optional paths select automatic values.
Build directories use simple paths without spaces, as checked by the wizard for
the component tools. The default `.config`, root `*.config` files and backups
stay out of Git. Inspection without editing is available with
`python3 -B tools/build_config.py show --config .config`.

## Build arguments

Pass optional settings as `NAME=value` after the command:

```sh
make iso JOBS=2
```

| Argument | Default | Meaning |
| --- | --- | --- |
| `JOBS` | `4` | Component build parallelism, from 1 to 24. Use this to control pipeline workers. |
| `ISO_BUILD` | `build/coast` | Build directory for receipts, logs and `image/Coast.iso`. Use a fresh directory when changing recorded build inputs or diagnostics. |
| `ISO_ARGS` | Empty | Additional builder arguments, listed below. |
| `DIAGNOSTICS` | `0` | Set to `1` to include Code OSS diagnostics. |
| `QUALIFY` | `0` | Set to `1` to enable diagnostics and all guest qualification suites after building. |
| `WITH_MICROSOFT_AUTH` | `0` | Set to `1` to include upstream proprietary Microsoft sign-in helpers. |
| `ELECTRON_CHECKOUT` | Automatic | Verified source checkout to reuse read-only. |
| `RUNTIME_BUILD` | Automatic | Matching verified Electron/Node receipt; requires `ELECTRON_CHECKOUT`. |
| `COMPONENTS` | Empty | JSON map of verified component receipts. |
| `PACKAGE_CONFIG` | Empty | Public package repository trust JSON to embed. |
| `O` | `build` | Kernel output directory and parent of the default `ISO_BUILD`. Shared downloads and some toolchains still use the repository's `build/`. `KBUILD_OUTPUT` is the equivalent environment variable; `O` takes precedence. |
| `SOURCE_MIRRORS` | Empty | Space-separated HTTPS mirrors tried before the base source manifest's upstream URLs, with the same checksum checks. Individual ports also use their own pinned manifests. |

Keep the same `ISO_BUILD` and `O` values for `make iso` and `make run`.

| `ISO_ARGS` option | Meaning |
| --- | --- |
| `--plan` | Print every planned stage as JSON without downloading or building. |
| `--qualify` | Include editor diagnostics and run all guest qualification suites after building; requires the extra test dependencies in [host setup](host-setup.md). |
| `--diagnostics` | Include opt-in Code OSS diagnostics without running qualification. |
| `--electron-checkout PATH` | Reuse a verified Electron source checkout read-only. |
| `--runtime-build PATH` | Reuse a verified, matching Electron/Node receipt; also requires `--electron-checkout`. |
| `--components PATH` | JSON map of component names to verified source-build receipts; see [source builds](source-builds.md). |
| `--repository-config PATH` | Embed an explicit public package repository trust configuration. |
| `--with-microsoft-auth` | Include upstream proprietary Microsoft sign-in helpers. |
| `--help` | Show the builder's complete option help without building. |

For example, `make iso ISO_ARGS=--plan` prints the plan, and
`make iso ISO_BUILD=build/qa ISO_ARGS=--qualify` builds Coast with qualification.
Quote multiple arguments as `ISO_ARGS='--diagnostics --with-microsoft-auth'`.

## Run arguments

The VM uses BIOS, USB keyboard/mouse, e1000e user networking, four CPUs and
16 GiB RAM. KVM is selected when `/dev/kvm` is accessible; otherwise QEMU uses
software emulation.

```sh
make run MEMORY=24G SMP=4
make run QEMU_ACCEL=kvm
make run FIRMWARE=uefi OVMF=/path/to/OVMF.fd
make -n run
```

`make -n run` prints the launch command without starting a VM.

| Argument | Default | Meaning |
| --- | --- | --- |
| `ISO_BUILD`, `O` | Same as the build | Locate the built Coast ISO. |
| `ISO_IMAGE` | `build/coast/image/Coast.iso` | Explicit path to the Coast ISO to boot. This launch setting does not rename the build output. |
| `MEMORY` | `16G` | Guest RAM, in QEMU size syntax. |
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
| `make setup-host` | Explicitly install Ubuntu/Debian prerequisites. `HOST_ARGS` forwards helper arguments; use `HOST_ARGS=--help` for its options. For inspection without installation, run `python3 -B tools/setup_host.py --print` or `--check` directly. |
| `make fetch-sources` | Download and verify the base pinned source archives; accepts `SOURCE_MIRRORS`. Component builders fetch their own sources as needed. |
| `make check-sources` | Verify cached base sources without downloading. |
| `make source-check` | Audit publication inputs for generated binaries, downloads and secrets. |
| `make source-archive` | Export `O/coast-source.tar.xz`, including the source inventory and lwIP for an offline kernel build. |
| `make clean` | Clean the kernel output, initramfs, ISO file and staging directory. Component caches remain available. Use the same output arguments as the build. |
| `make userland-clean` | Remove generated `userland/bin`, `obj` and `runtime`. |
| `make help` | Show the short command reference. |
| `make test-build-config` | Check saved settings, Make overrides and argument handling without a source build or VM. |

The cache cleaner previews removals with `python3 -B tools/clean_cache.py`;
`--apply` performs them. See [cache cleanup](repository-cleanup.md) for its
protection rules and options.

## Component development

`make` alone builds the kernel at `O/vmlinux` using Coast's Kbuild-style system.
`make -j4` sets kernel compiler parallelism, `V=1` prints compiler commands,
`CROSS_COMPILE=prefix-` chooses a GCC cross compiler and `LLVM=1` selects
Clang/LLVM. `make desktop-kernel` builds the kernel ABI settings from
`ports/chromium/kernel-flags.json`. The OS pipeline uses each component's
recorded toolchain. Kernel/userland regression checks manage their own fixtures.

Component-specific recipes are described in [source builds](source-builds.md),
[Qt](qt6.md), [QEMU](qemu.md) and the other port guides. Use each tool's `--help`
for its complete argument list.

See [source builds](source-builds.md) for receipts, package coverage and remaining
prebuilt inputs. The qualified desktop uses verified source-built Electron/Node
components; an empty full Electron graph build has not yet been verified end
to end.
