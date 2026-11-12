# Coast OS

Coast OS is an experimental x86-64 operating system with its own kernel, native
desktop, account system and Linux-compatible application ABI. It runs Chromium,
Code OSS and native development tools inside Coast.

[GitHub repository](https://github.com/camden-pina/ModernOS)

![Coast desktop with the native Files application](Documentation/images/desktop.png)

## Build and run

Use an **x86-64 Linux host with Git and Python 3.12+**. On Ubuntu 24.04, run:

```sh
git clone https://github.com/camden-pina/ModernOS.git coast
cd coast
python3 -B tools/setup_host.py --install
make menuconfig
make iso
make run
```

The setup helper explicitly installs host dependencies with sudo, including the
checksum-verified Chromium prerequisite installer. Run it once as your normal
user. The build fetches pinned upstream sources and uses rootless Podman and
Bubblewrap. [Host setup](Documentation/host-setup.md) covers other distributions,
required namespaces and inspecting the setup commands before installation.

The ISO is **`build/coast/image/Coast.iso`**. Live progress shows the current
stage, elapsed time, tool percentages and source download sizes; full logs are
saved under `build/coast/logs/`. Rerun `make iso` to resume
verified components and continue interrupted source preparation. Pinned sources use shallow Git checkouts to avoid downloading
upstream history. Electron/Chromium is still a large
source build: reserve more than 100 GB for it plus space for the other build
trees. The desktop VM uses 16 GiB RAM. `make run` uses KVM when available and
GTK with host OpenGL; Coast's desktop still renders
in software through emulated VGA at a 1920×1080 default boot resolution. [Run options](Documentation/build-system.md#run-arguments)
cover alternate host display backends.

`make iso` defaults to the complete desktop: the kernel, native desktop and account
tools, Chromium, Code OSS, GCC/G++, Python, Brine/makepkg, Qt Widgets,
QEMU and the Chromium VM viewer. The wizard can omit optional apps and explains
their dependencies. `make run` boots that ISO without rebuilding.
The proprietary Microsoft sign-in broker is unavailable until its GTK/WebKit
libraries are built for Coast; leave `WITH_MICROSOFT_AUTH=0` (the default).

For example, omit Chromium and add it later:

```sh
make iso WITHOUT=chromium
make build COMPONENT=chromium
make iso ADD=chromium
```

Code OSS still needs the shared Electron/Chromium engine. Omitting Chromium
also omits the Chromium-based VM viewer; add `qemu-viewer` later to restore it.
[App selection and dependencies](Documentation/build-system.md#optional-applications)
cover this and other choices. [Recovery and cleanup](Documentation/repository-cleanup.md)
describe `make clean`, per-component cleanup, full resets and KEEP options.
[Optional arguments](Documentation/build-system.md) cover build parallelism,
output directories, VM settings and developer workflows.

The terminal wizard saves build and VM choices in `.config`. Reopen it with
`make menuconfig`; later Make commands load the saved settings automatically.
`make iso` also works with defaults before a configuration is saved.

Coast boots directly to the desktop. Press **Ctrl+Alt+G** to use the physical
console and press it again to return to the desktop. The owner account is
UID 1000 with sudo access; its initial password is `root`. Change it with
`passwd` after boot if you want a different password for that session. Switching
to the console closes desktop windows after any unsaved-work prompts.

For GitHub Download ZIP, extract it and run `git init` in the extracted folder
before building. A Coast source archive already includes its source inventory.

## Use Coast

Super+B opens Chromium when installed, Super+T opens Terminal and Super+E opens
Files. Super+Space opens Spotlight; use it to launch Code OSS, Brine, Qt 6 Demo
and the other installed applications.

Coast uses **RAM-backed live storage**. Copy work to a supported writable mount
before shutdown to keep files, passwords and configuration.

**Brine** is Coast OS's package manager. It installs signed packages and builds
reviewed source recipes with makepkg/fakeroot. To download signed
packages, obtain a server URL, public key and independently verified full
fingerprint from your repository operator, then run inside Coast:

```sh
brine configure https://packages.example.org --key ./repository.asc --fingerprint FULL_40_CHARACTER_FINGERPRINT
brine server --check
brine refresh
```

The example URL is a placeholder for your package service. Repository hosting
and community setup are described in [Brine](Documentation/local-repositories.md).
A source build requires no package server or private signing key.

## Development

Git contains source, patches, editable configuration, artwork, license notices
and pinned download manifests. Generated programs, ISOs, downloads and private
state stay in ignored local directories.

```sh
make source-check test-source-tree test-source-fetch test-desktop-assembly
make test-build-lifecycle test-optional-apps
python3 -B tools/test_package_configuration.py
```

An integrated `make iso JOBS=16` run with qualification enabled passed all 26
stages and 11 guest qualification phases, including signed package transactions, Brine, Qt
input/clipboard and nested QEMU through Chromium/noVNC. Source-export
components were also compiled inside Coast; the resulting native ISO passed
UEFI/BIOS boot checks at one and four CPUs. This build reused verified
source-built Electron/Node components; a build of their full graph from an
empty checkout remains unverified. [Source builds](Documentation/source-builds.md)
records the precise coverage and remaining prebuilt inputs.

See [the build guide](Documentation/build-system.md), [desktop](Documentation/desktop.md),
[browser](Documentation/chromium-browser.md), [accounts](Documentation/accounts.md),
[Qt](Documentation/qt6.md), [QEMU](Documentation/qemu.md),
[contributing](CONTRIBUTING.md) and [licenses](LICENSES.md).
[Cache cleanup](Documentation/repository-cleanup.md) explains how to inspect
and remove regenerable local caches.
