# Linux host setup

Clone `https://github.com/camden-pina/ModernOS.git` into a path without spaces.
Use x86-64 Linux and Python 3.12 or newer. Ubuntu 24.04 is the documented package
installation example; Ubuntu/Debian hosts with the required Python version use
the same helper. The build itself runs as your normal user.

```sh
python3 -B tools/setup_host.py --install
make menuconfig
make iso
make run
```

`python3 -B tools/setup_host.py --install` is an explicit host package installation.
The `make setup-host` shortcut is available when GNU make is already installed. The helper uses sudo
for apt and the pinned Chromium prerequisite installer. It does not change host
security policy, add repositories or install a global Python/Node/Rust environment.
The build fetches its pinned Node, Rust and other upstream tools into `build/`.

`make menuconfig` opens the terminal build wizard. Save and exit to write
`.config`; `make iso` and `make run` then use those settings. Reopening the wizard
loads the saved values. `make config` provides a plain terminal questionnaire.
See [configuration and optional arguments](build-system.md).

Inspect the package commands without installing anything:

```sh
python3 -B tools/setup_host.py --print
```

## Dependencies

The build needs GNU make, GCC/G++, binutils, Git, Python, curl, tar,
patch, gzip/bzip2/xz, mtools and xorriso. The helper installs NASM; the
Limine builder can also bootstrap a local host NASM when needed. Host QEMU
supplies the graphical VM frontend; `make run` uses BIOS, USB keyboard/mouse
and user networking.

The build also uses Autotools, Bison, Flex, gperf, Gettext,
Texinfo, CMake, Ninja, pkg-config, ncurses tools, rootless Podman, Bubblewrap and
development headers for OpenSSL, libffi, Readline, zlib, SQLite, bzip2, xz,
gdbm, UUID, Kerberos, X11, xkbfile and libsecret. The exact apt package lists
are in `tools/setup_host.py`.

Chromium prerequisites come from its installer at the exact revision in
`ports/electron/sources.lock.json`. The helper verifies SHA-256 before running
the downloaded file, disables ChromeOS font installation and uses the
standalone installer. See [Electron's Linux guide](https://www.electronjs.org/docs/latest/development/build-instructions-linux/)
and [Chromium's Linux requirements](https://chromium.googlesource.com/chromium/src/+/main/docs/linux/build_instructions.md).
Chromium alone needs at least 100 GB free disk; reserve additional room for
Coast's GUI, compiler, application and port builds. The desktop guest uses
16 GiB RAM. `JOBS=2` reduces build parallelism.

On another distribution, install equivalent packages yourself and use:

```sh
python3 -B tools/setup_host.py --check
```

This command checks installed tools and exercises rootless Podman/Bubblewrap.
It installs nothing.

## Rootless containers and virtual machines

Podman must run without sudo and report a rootless host. Bubblewrap needs the
user namespaces allowed by your distribution's configuration:

```sh
podman info --format '{{.Host.Security.Rootless}}'
bwrap --unshare-all --ro-bind / / --proc /proc --dev /dev -- /bin/true
```

If a check fails after installing packages, finish your distribution's rootless
container setup, including subordinate UID/GID ranges and applicable AppArmor
policy. Log in again when group or account mappings change.

`make run` automatically selects KVM when `/dev/kvm` is accessible.
Otherwise it uses TCG. The default BIOS launch needs no external OVMF file or
passt backend. VM settings are explicit:

```sh
make run MEMORY=24G SMP=4
make -n run
```

The [command reference](build-system.md) explains build/output options,
firmware, disks, networking and the other optional Make arguments.

The complete guest qualification suite additionally needs KVM, host OVMF,
GnuPG, Pillow, Tesseract and passt. Its native QEMU passt backend requires
QEMU 10.1 or newer. On Ubuntu the optional test packages are `python3-pil`,
`tesseract-ocr` and `passt`; provide a newer host QEMU if needed. Use
`OVMF=/absolute/path/to/OVMF.fd` when automatic firmware discovery is unsuitable.
Choose `ISO_ARGS=--qualify` on the initial build; it enables editor test
diagnostics and is part of the build's identity.
