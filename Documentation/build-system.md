# Build commands and optional arguments

Repository: https://github.com/camden-pina/ModernOS.git.
See [host setup](host-setup.md) for prerequisites. After setup:

```sh
make menuconfig
make iso
make run
```

`make iso` builds Coast: the kernel, desktop/accounts, Chromium, Code OSS,
GCC/G++, Bash, Python, Brine/makepkg, Qt, QEMU and its Chromium VM
viewer by default. Optional apps and their dependencies can be selected below. The output is
`build/coast/image/Coast.iso`.

The build verifies completed components and resumes interrupted stages. `make run` boots the existing ISO
in QEMU. Enter `gui` at the guest console to open the desktop.

## Build progress and interruption

Each stage shows its number, elapsed time and a live progress bar. Ninja,
CMake and Git percentages appear when the tool supplies a total. Otherwise,
the bar shows activity; it does not estimate how long the whole build will take.
The source sync also shows completed source repositories, downloaded Git bytes
and transfer speed. Recent tool output appears beside the bar. Redirected output
receives a status line every 30 seconds instead of terminal animation.

Full stdout and stderr are retained in `ISO_BUILD/logs/<stage>.log`, including
output from failed commands. To follow the source download in another terminal:

```sh
tail -f build/coast/logs/electron-sync.log
```

Press Ctrl+C in the build terminal to stop the current stage and its child
processes. Verified completed stages can be reused by the next `make iso`.
Electron source preparation resumes fetch, patches, hooks and overlays from
its own checkpoint. Partial archive downloads and compiler object trees are
reused where supported. Changed inputs invalidate dependent outputs; staging
and assembly retry from verified components. See [recovery and cleanup](repository-cleanup.md)
for phase granularity, pinned-source changes, full resets and component cleanup.

## Terminal configuration

`make menuconfig` opens the terminal wizard with the saved `.config`, or defaults
on the first run. It groups build resources, verification/features, source reuse,
package trust, VM resources, display/firmware, networking, audio, disks and extra
arguments, including optional apps and their runtime dependencies.

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
| `ISO_BUILD` | `build/coast` | Build directory for receipts, logs and `image/Coast.iso`. Changed inputs invalidate affected components automatically. |
| `APPS` | `all` | Apps to install: `all`, `none`, or a space/comma-separated list. Required runtime apps are included automatically. |
| `WITHOUT` | Empty | Omit apps and apps that require them; the build plan lists the reasons. |
| `ADD` | Empty | Add apps to the last ISO selection. Does not change `.config`. |
| `ISO_ARGS` | Empty | Additional builder arguments, listed below. |
| `DIAGNOSTICS` | `0` | Set to `1` to include Code OSS diagnostics. |
| `QUALIFY` | `0` | Set to `1` to enable diagnostics and applicable guest qualification suites after building. |
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
| `--apps LIST`, `--without LIST`, `--add LIST` | Equivalent app selection flags for direct builder use. |
| `--build-only`, `--target LIST` | Build selected components and dependencies without assembling an ISO (`make build COMPONENT=...`). |
| `--plan` | Print every planned stage as JSON without downloading or building. |
| `--qualify` | Include editor diagnostics and run applicable guest qualification suites after building; requires the extra test dependencies in [host setup](host-setup.md). |
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

## Optional applications

`make iso` includes every app unless the saved configuration or command-line
arguments select otherwise. Set `APPS`/`WITHOUT` in `make menuconfig` to persist a
selection. Command-line selections apply to that invocation only.

```sh
make iso WITHOUT='chromium codeoss'
make iso APPS='qt qemu'
make build COMPONENT=chromium
make iso ADD=chromium
make iso ADD='chromium qemu-viewer'
make iso ISO_ARGS=--plan
```

`APPS=none` builds the native core. App selection does not introduce named image
editions: every selection uses `make iso` and the same output path. To add an app
later, `make build COMPONENT=app` builds it and its dependencies without changing
the ISO; `make iso ADD=app` adds it to the last recorded ISO selection and builds
any missing prerequisites. Assembly creates a fresh image from component
receipts. It never modifies an old ISO in place. The selection remains available
after a component build or cleanup invalidates the image manifest. A full reset
can remove this generated selection; persist preferences in `.config`.

| App | Required installed apps |
| --- | --- |
| `chromium` | Shared Electron/Chromium engine, GNU/GUI libraries and display bridge (build components). |
| `codeoss` | `bash`, plus that same engine/GUI/bridge and the Code OSS application/modules. |
| `bash`, `gcc`, `python`, `qt`, `qemu` | No other optional app; their build prerequisites are still compiled. |
| `makepkg` | `gcc`, `python`, `bash`; package/signature/archive tools are included as runtime components. |
| `brine` | Brine package manager; `makepkg`, `python`, and their prerequisites. |
| `qemu-viewer` | `qemu`, `python`, `chromium`. |

`WITHOUT` excludes required apps: dependent apps are omitted and
the plan explains why. Explicit `ADD` restores its apps and their runtime
prerequisites even when saved `WITHOUT` settings omit them. For example, `WITHOUT=chromium` omits the browser and VM
viewer while retaining Code OSS and its shared engine. Omit both `chromium` and
`codeoss` to avoid building that engine. `WITHOUT=gcc` also omits makepkg and
Brine. Qt/QEMU can still require the host cross compiler to build;
that does not install GCC in the ISO.

The payload omits unselected app files and launcher entries. Its installed
package database describes the actual files and verifies dependency closure.
`make iso ISO_ARGS=--plan` reports selected/omitted apps, runtime dependencies,
shared engine consumers and build components. Guest qualification records suites
requiring omitted apps as skipped, rather than claiming they passed.

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
| `make clean` | Clean generated component/kernel/userland/image products; retain sources and downloads. Use the build's `ISO_BUILD`/`O`. |
| `make clean COMPONENT=qt` | Clean selected component/app products and invalidate dependent completion receipts. |
| `make clean ALL=1` | Full generated reset, including sources and download caches; preserve private/configured service state. |
| `make clean ALL=1 KEEP="sources downloads toolchains"` | Broad rebuild while retaining expensive prerequisites. |
| `make clean PREVIEW=1` | List removals, invalidations and affected apps without deleting products. |
| `make build COMPONENT=app` | Build an app and its runtime/build prerequisites, without assembling an ISO. |
| `make test-build-lifecycle test-optional-apps` | Check interruption/retry, cleanup protections, app dependencies and selected payloads. |
| `make userland-clean` | Remove generated `userland/bin`, `obj` and `runtime`. |
| `make help` | Show the short command reference. |
| `make test-build-config` | Check saved settings, Make overrides and argument handling without a source build or VM. |

Full/selective cleanup options, keep categories and recovery granularity are in
[recovery and cleanup](repository-cleanup.md). The narrower cache cleaner previews removals with `python3 -B tools/clean_cache.py`;
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
