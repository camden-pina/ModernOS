# Coast

Coast is an experimental x86-64 operating system with its own kernel, native
desktop, shell, account system and Linux-compatible application ABI. Development
and guest validation use an x86-64 Linux host and QEMU.

The desktop browser is **Chromium**, through the ported Electron 43.7.3 /
Chromium 150.0.7871.250 runtime. NetSurf and the earlier WebCore browser have
been removed. Code OSS, the native editor and Terminal remain available.

The wider source image includes GCC/G++, binutils, make, Python, Git, Coast
Packages/makepkg, a Qt Widgets demo, QEMU and the Chromium VM viewer. Additional
packages and native build kits require a configured signed repository. See
[packages](Documentation/local-repositories.md), [QEMU](Documentation/qemu.md)
and [source builds](Documentation/source-builds.md).

## Source-only repository

Git contains source, patches, editable configuration, artwork, upstream notices
and checksum-pinned download manifests. Compilers, executable programs, object
files, firmware binaries, ISOs, downloaded archives and runtime staging are
ignored. Build products remain local under `build/` and `userland/runtime/`.
Private signing keys and machine configuration are excluded.

```sh
make source-check                 # audit files that Git would publish
make source-archive               # deterministic, offline kernel source kit
```

[Publication and cleanup](Documentation/repository-cleanup.md) describes the
checks, cache cleanup and GitHub workflow. [LICENSES.md](LICENSES.md) records
the current licensing status.

## Fresh checkout and host setup

Use an x86-64 Linux host, Python 3.12 or newer and a checkout path without
spaces. Run the following host setup yourself. This Ubuntu 24.04 example
installs the native-image build tools and graphical host emulator; other
distributions need their equivalent packages:

```sh
sudo apt-get update
sudo apt-get install -y build-essential clang lld git python3 curl ca-certificates \
  patch bzip2 xz-utils unzip zip file gnupg nasm mtools xorriso \
  qemu-system-x86 qemu-system-gui ovmf
```

Replace `OWNER/REPOSITORY` with the published GitHub repository:

```sh
git clone https://github.com/OWNER/REPOSITORY.git coast
cd coast
make source-check test-source-tree test-source-fetch test-desktop-assembly
python3 -B tools/test_package_configuration.py
```

For GitHub **Download ZIP**, extract it, enter the extracted directory and run
`git init` before those checks. The source inventory requires Git metadata or
the `SOURCE-MANIFEST.json` supplied by `make source-archive`; GitHub's ZIP has
neither. An exported Coast source archive already includes its inventory.
The first build needs internet access for checksum-pinned upstream downloads.
No ISO, developer cache, private key or running package server is required.

## Build the desktop

For the full Chromium/Code OSS desktop with Coast Packages, makepkg, Qt and
QEMU, first install the additional host build dependencies:

```sh
sudo apt-get install -y autoconf automake libtool bison flex gperf gettext texinfo \
  cmake ninja-build pkg-config bubblewrap podman uidmap slirp4netns fuse-overlayfs \
  ncurses-bin libncurses-dev libssl-dev libffi-dev libreadline-dev zlib1g-dev \
  libsqlite3-dev libbz2-dev liblzma-dev libgdbm-dev uuid-dev \
  libkrb5-dev libx11-dev libxkbfile-dev libsecret-1-dev lsb-release
```

Install Chromium's Linux prerequisites using the installer from the exact
Chromium revision in `ports/electron/sources.lock.json`. These are host package
installations; Coast's build command does not install them for you. The installer
below is standalone when ChromeOS font installation is disabled:

```sh
mkdir -p build/host-setup
curl --fail --location \
  https://raw.githubusercontent.com/chromium/chromium/f916f1b7136907af19d96b95b891533d091d932a/build/install-build-deps.py \
  --output build/host-setup/install-build-deps.py
printf '%s  %s\n' 22ea2aebdd09d5554029b55a63757d0d30a71272ebb3c5ee444aaeb822230920 \
  build/host-setup/install-build-deps.py | sha256sum --check
sudo python3 build/host-setup/install-build-deps.py --no-prompt --no-arm --no-chromeos-fonts
```

[Electron's Linux guide](https://www.electronjs.org/docs/latest/development/build-instructions-linux/)
describes the upstream requirements. Chromium alone calls for at least 100 GB
of free space; Coast also builds separate GUI, compiler, application and port
trees, so reserve additional space. Use ample RAM and reduce `DESKTOP_JOBS` if
necessary; the desktop VM itself uses 16 GiB. See
[Chromium's requirements](https://chromium.googlesource.com/chromium/src/+/main/docs/linux/build_instructions.md).

Run the build as your normal user. Podman must report `true`, and the Bubblewrap
check must exit successfully before building. If either fails, finish your
distribution's rootless container/user namespace setup first:

```sh
podman info --format '{{.Host.Security.Rootless}}'
bwrap --unshare-all --ro-bind / / --proc /proc --dev /dev -- /bin/true
make desktop-wide-iso DESKTOP_ARGS=--plan
make desktop-wide-iso DESKTOP_JOBS=4
```

The result is **`build/desktop-source-wide/image/Coast-desktop.iso`**. Logs are
under `build/desktop-source-wide/logs/`, with component receipts and `BUILD.json`
beside them. Rerun the same build command after an interruption; it checks and
reuses completed components. If source or build options change, select a fresh
`DESKTOP_BUILD=build/another-desktop` directory; the wider target appends `-wide`.

The pipeline builds Electron/Node, the GNU ABI and GUI libraries, VSCodium and
its native addons/Rust resolver, the kernel, native desktop, GCC/G++ SDK,
Bash/Readline, pacman/GnuPG, Python/SQLite, GNU makepkg tools/shared musl/fakeroot,
Qt Widgets/QPA, QEMU TCI/SDK/firmware and the noVNC viewer. It assembles a new
image and package ownership database without an inherited ISO. Pinned Rust
host tools, fonts and dictionaries are fetched into the build directory.
LLVM and the complete native browser/QEMU build kits remain separate packages.
See [source builds](Documentation/source-builds.md) for the precise coverage.

Two smaller alternatives use the same checkout:

```sh
make native-iso                  # build/native-image/Coast-native.iso
make desktop-iso DESKTOP_JOBS=4   # build/desktop-source/image/Coast-desktop.iso
```

Choose one image target. The native image needs only the first host setup and
contains the kernel, native desktop, BusyBox shell and account tools, without
Chromium, Code OSS or the wider ports. The core desktop needs the full host
setup but omits the wider integrations. `make -j4` builds only the kernel;
`make iso` is the separate development assembly described in the
[build guide](Documentation/build-system.md).

## Boot the image

On a graphical Linux host with access to `/dev/kvm`, boot the generated wide
image directly. This command uses BIOS and QEMU user networking, so it needs
no selected-image symlinks, external OVMF path or passt backend:

```sh
qemu-system-x86_64 -accel kvm -cpu host -machine q35 -smp 4 -m 16G \
  -cdrom build/desktop-source-wide/image/Coast-desktop.iso \
  -display gtk,gl=off -vga none -device VGA,vgamem_mb=32 \
  -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-mouse,bus=xhci.0 \
  -nic user,model=e1000e -serial stdio -no-reboot
```

For a core or native image, substitute its ISO path above; the native image can
use `-m 2G`. If KVM is unavailable, replace `-accel kvm -cpu host` with
`-accel tcg -cpu max` for slower software emulation. Enter `gui` at the Coast
console. Super+B opens Chromium, Super+T Terminal, and Super+E Files. Open
Code OSS, Coast Packages and Qt 6 Demo from Spotlight. Set the owner password
in Settings → Profiles before using sudo. See [accounts](Documentation/accounts.md).

For UEFI, the existing Make launcher accepts the new ISO explicitly:

```sh
make run-browser CHROMIUM_ISO=build/desktop-source-wide/image/Coast-desktop.iso \
  CHROMIUM_QEMU_NETWORK='-nic user,model=e1000e' QEMU_DISPLAY=gtk,gl=off
```

This launcher requires host OVMF, autodetected where supported; otherwise set
`OVMF=/absolute/path/to/OVMF.fd` to a suitable host firmware image. Bare
`make run-browser` and `make run-codeoss` refer to local selected images under
`build/`; a fresh clone does not contain those links. The launcher's default
passt networking needs a host QEMU that supports it; the override above works
with older host versions too.

Coast is a live system with RAM-backed storage. Save files to a supported
writable mount before shutdown; home files, passwords and package configuration
otherwise disappear with the VM.

## Build inside Coast

The wider image's package frontend starts without a server or trusted key.
It can list installed packages with `coast list` and show configuration with
`coast repo`. To download packages, obtain a repository URL, public signing
key and independently verified full fingerprint from your repository operator.
There is no public production endpoint bundled with this source checkout.
After setting the owner password, configure that identity inside Coast:

```sh
coast configure https://packages.example.org --key ./repository.asc --fingerprint FULL_40_CHARACTER_FINGERPRINT
coast server --check
sudo pacman -Syy
sudo pacman -Fyy
coast refresh
```

Replace the example URL, key path and fingerprint; the key must be accessible
inside the guest. Configuration imports only that verified public key.
[Source builds](Documentation/source-builds.md) also describes supplying public
repository configuration when assembling your own image. Hosting packages and
the separate community service is covered in
[Coast Packages](Documentation/local-repositories.md); their local server
bundles and private state are not shipped in Git.

Once your signed repository supplies the native build kits:

```sh
coast install coast-build --noconfirm
mkdir -p ~/Builds/coast
cd ~/Builds/coast
tar -xJf /usr/src/coast/os/coast-source.tar.xz --strip-components=1
python3 -B tools/build_desktop_kernel.py --jobs 4
python3 -B tools/build_native_iso.py --kernel build/desktop-kernel/vmlinux --jobs 4
```

The source kit contains all Coast sources, port patches and the exact lwIP
archive needed for an offline kernel build. The native rebuild produces `build/desktop-kernel/vmlinux` and
`build/native-image/Coast-native.iso`; it does not overwrite the running kernel.
Offline toolchain and port archives are installed under `/usr/src/coast/distfiles`.

QEMU and all seven of its libraries can also be compiled inside Coast. The
native integration test has rebuilt Coast's ISO and booted its graphical desktop
inside that source-built emulator. Install `qemu-build` for the offline build
kit and `coast-vm` for the Chromium VM viewer. Follow the complete
[build and nested boot commands](Documentation/qemu.md).

For a native build session, increase the host launch command to `-m 24G`.

NASM also has a reviewed native source recipe:

```sh
coast refresh
coast clone nasm
cd ~/Builds/nasm
makepkg --noconfirm
```

## Chromium and Code OSS

Chromium uses software rendering and the port's jitless
policy. Its owner-profile launch, sandbox, networking and clipboard limits are
documented in [the browser guide](Documentation/chromium-browser.md).

The standalone Electron source builder uses checked-in upstream commits,
complete patch sets, Coast policies and GN arguments:

```sh
python3 -B tools/build_electron_source.py --sync --build --jobs 4
```

This large build follows the upstream source graph and downloads its host build
tools. The source lock has been checked against all 170 original repositories.
The builder compiles Electron and its matching Node and records their exact
hashes; guest qualification requires a kernel built for that pair.
`make desktop-iso` connects this runtime build to the remaining desktop stages.
No historical `Documents/Codex` directory is needed by that source recipe.

The GUI library source builder downloads the pinned Debian sources and
patches automatically and builds them with rootless Podman:

```sh
python3 -B tools/build_gui_sources.py --jobs 4
```

Completed packages are checked and reused when the same command is rerun.
Downloaded build tools stay inside its container; exported GUI runtime
packages are compiled from source. Host setup remains a prerequisite.

The desktop build omits the optional Microsoft
sign-in broker binaries by default because they have no public source build.
To include them, run:

```sh
make desktop-iso DESKTOP_BUILD=build/desktop-with-auth DESKTOP_ARGS=--with-microsoft-auth
```

Code OSS's pinned upstream dependency installation
obtains those binaries automatically. The same option is available on
`tools/assemble_desktop.py` for explicit component assembly. The image receipt
records the choice and the remaining prebuilt application helpers; enabling
Microsoft sign-in does not make its broker source-built.

## Tests and cleanup

The wide image passed all 11 guest qualification phases, including actual
makepkg/fakeroot transactions, Qt input/clipboard and nested QEMU through
Chromium/noVNC. A separate source-only export rebuilt and booted the native
image in all four UEFI/BIOS CPU cases. The desktop run reused verified
source-built Electron/Node and GNU ABI receipts: a full empty Electron graph
build has **not** been verified end to end. The remaining prebuilt application
helpers and other limits are listed in [source builds](Documentation/source-builds.md).

To qualify your own wide build, choose `--qualify` on its initial build:

```sh
make desktop-wide-iso DESKTOP_JOBS=4 DESKTOP_ARGS=--qualify
```

Qualification also needs usable KVM, OVMF, GnuPG, Pillow, Tesseract and passt;
the host QEMU must support its native passt backend (QEMU 10.1 or newer; see
the [QEMU backend reference](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html)).
For Ubuntu, install `python3-pil tesseract-ocr passt` and provide the newer
host QEMU separately if needed. Qualification opts into editor diagnostics;
adding it to an existing build with different options requires a fresh
`DESKTOP_BUILD`. Reports stay under the build's `qualification/` directory and
bind every phase to the exact ISO. Ordinary interactive boot does not require
these additional test dependencies.

The smaller host checks are:

```sh
make source-check test-source-tree test-source-fetch
make test-desktop-assembly
python3 -B tools/test_package_configuration.py
make test-acpi-host test-power-host test-efi-loader test-boot-config
```

Guest suites include `tools/test_coast_source.py`, `tools/test_qemu.py` and
`tools/test_chromium_browser.py`. Their reports, screenshots and images stay
under `build/`. Host tests do not establish guest or hardware behavior. Cleanup
is optional: `python3 -B tools/clean_cache.py` previews known obsolete caches;
add `--apply` only to remove the listed generated paths.

Coast currently uses RAM-backed live-system storage unless files are copied to
a supported writable mount. Real-hardware support, arbitrary Linux applications
and every public website are outside the validated coverage. Kernel, process,
memory, networking, driver and filesystem sources are in `arch/`, `kernel/`,
`mm/`, `net/`, `drivers/` and `fs/`; applications and ports are in `userland/`
and `ports/`.
