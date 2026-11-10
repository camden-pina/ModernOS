# Current Coast package server

The portable server lives at `build/Coast-repository-server`, pointing to the selected qualified
source snapshot. Its public data includes 76 signed packages, thirteen source-recipe
Git repositories and their source archives. Run `readlink -f build/Coast-repository-server`
to identify the immutable snapshot. The
package producer and signing workspace are separate from this public snapshot.
The signing private key is not included in the snapshot.

The repository includes [QEMU 10.2.4](qemu.md), matching firmware, its seven
dependency libraries, Meson, the development SDK and corresponding sources.
QEMU, the libraries and Ninja in this snapshot were compiled inside Coast and
passed native installation, development and nested desktop boot checks.
`qemu-build` supplies the offline native build kit, and `coast-vm` opens a nested
Coast desktop in Chromium through the included noVNC source client.
In Coast, run `sudo pacman -Sy qemu` to refresh binary metadata and install it.
The qualified profile uses TCI software emulation and serial or VNC output.
`coast-build` adds the OS source kit, native build dependencies, offline toolchain/port
sources, Wayland SDK, NASM, xorriso and mtools. See [native rebuilds](source-builds.md).
The selected release versions are recorded in `repository.json` and
`OS-SOURCE.json` beside that symlink's target. The source kit has passed native
kernel, desktop and ISO builds; QEMU and its dependency kit have also passed a
fresh native build and nested graphical boot on the source-built glibc desktop.

The repository also supplies the relocatable POSIX GCC SDK, native-built CMake
and GN, and source-built Python and LLVM packages. Signed installation checks
cover C++ threads, Make/Ninja, CTest/CPack, GN, threaded Python subprocesses,
Clang/LLD and ThinLTO. The LLVM and Python packages were cross-built; their
compilers and subprocesses were exercised inside Coast.

GNU Bison and grep in this snapshot were also compiled inside Coast and tested
after signed installation. `bison-build` and `grep-build` contain their complete
offline source kits. The grep port falls back to real reads when Coast returns
`ENOSYS` for splice, so configure scripts can drain large producer pipes.
See [native build tools](native-build-tools.md).

## Use it on this Linux installation

The server runs as the current user's `coast-packages.service`. It is enabled
at user login and runs independently of the terminal or coding session. It does
not currently run before login or after the last user session is stopped;
systemd lingering has not been enabled.

- Host catalog: <http://127.0.0.1:8765>
- Coast VM endpoint: `http://10.0.2.2:8765`
- Listen address: **127.0.0.1 only**.
- Configuration: `~/.config/coast/repository-server.json`.
- User unit: `~/.config/systemd/user/coast-packages.service`.

No inbound LAN firewall rule, router forwarding, bridge or TAP interface is
needed for this setup. Other computers on the work network cannot reach this
loopback listener. It does not modify the drive layout, other installations,
partitions, bootloader or firmware settings.

```sh
systemctl --user status coast-packages.service
systemctl --user restart coast-packages.service
systemctl --user stop coast-packages.service
journalctl --user -u coast-packages.service -n 30
```

The selected Coast live image already defaults to the VM endpoint. Inside Coast:

```sh
coast server
coast refresh
coast search g
coast install bc
```

Repository files remain on the current Linux filesystem. Installed packages and
user files inside the live Coast VM remain in its RAM filesystem and reset on
shutdown. This server does not install Coast onto a physical partition.

## Move the server later

Copy the entire resolved `build/Coast-repository-server` directory, including
`repository.json`, `public/`, `web/`, `deployment/` and `serve_repository.py`, to
the other computer. Copy the directory itself, rather than only the
`Coast-repository-server` symlink. Python 3.11 or later is sufficient; the Coast
build workspace and private signing keys are unnecessary there.

For a local-only check after moving it, run from the copied directory:

```sh
python3 -I -B serve_repository.py --repository repository.json
```

When using a network you control, configure an explicit LAN URL and permitted
client subnet with the included deployment examples. See
[the deployment guide](local-repositories.md#host-on-another-computer) for the
LAN, TLS and service configuration. Those LAN instructions are not part of the
current work-network setup. Coast clients can select that server using
`coast server URL --check` followed by `coast server URL`; the pinned signing
key and signature requirements remain enforced.

If this workspace is moved, update the paths in the user service and run
`systemctl --user daemon-reload` before restarting it. For package updates,
export a new complete snapshot, select it through the server symlink and
restart the service. Do not edit signed files in a running snapshot.

## AUR workflow and current scope

The [AUR workflow](https://wiki.archlinux.org/title/Arch_User_Repository) is
Git recipe checkout, review of the PKGBUILD and accompanying files, a normal-user
makepkg build, and installation with pacman. Coast uses the actual upstream Git,
makepkg, fakeroot, GnuPG, pacman/libalpm and version-comparison tools for that
workflow. `base-devel` supplies the build dependencies. Root builds are refused;
installation and removal authenticate with Coast's real sudo and kernel
credentials. `--noconfirm` does not bypass the helper's source-review gate.

```sh
coast clone hello-coast
cd ~/Builds/hello-coast
cat PKGBUILD hello.c
makepkg --printsrcinfo
makepkg -si
```

Coast also provides its signed binary repository. Search, dependency resolution,
file ownership, transaction locks, version ordering and removal use the real
pacman database. Source updates use Git and the signed catalog; remote checkout
commits must match that catalog. Signatures identify the publisher and do not
make executable PKGBUILD code safe without review.

The separate Coast Community service runs the actual aurweb 6.5.1 application
on localhost port 8770, with SSH submissions on localhost port 2222. It supplies
the AUR v5 API, accounts, comments, votes, subscriptions, co-maintainers,
moderation and package requests. Current upstream orphan adoption requires
moderator approval. The Coast client queries that service and builds reviewed
community Git checkouts with upstream makepkg. The signed catalog's separate
version 1 API remains available for its trusted publisher recipes.

The public portable community bundle is `build/Coast-community-server` and
excludes credentials, the database, private mail and package signing secrets.
Its management tool supports start/stop/status and consistent private backup
and restore. Verification/reset mail stays in its local outbox. See
[the community guide](local-repositories.md#community-service).

The installed service is `coast-community.service`, enabled at user login.
Open <http://127.0.0.1:8770> on this host; the VM uses
`http://10.0.2.2:8770`. Both the web listener and SSH port 2222 bind only to
127.0.0.1. Its private configuration is
`~/.config/coast/community-server.json`, and persistent accounts, Git history,
SSH identity and local mail live under `~/.local/share/coast/community`.
Generated administrator credentials are in that directory's
`ADMIN-CREDENTIALS.txt`, readable only by this user. Change the initial
password through My Account. The `coastports` account maintains the seeded
`hello-coast`, `bc` and `ninja` recipes without administrator privileges.

```sh
systemctl --user status coast-community.service
systemctl --user restart coast-community.service
python3 -I -B build/Coast-community-server/manage.py \
  --config ~/.config/coast/community-server.json status
```

Always pass that configuration when using the installed bundle's management
tool; its bundled default is a portable example with a separate relative
state directory. For a consistent private backup, choose a new filename:

```sh
python3 -I -B build/Coast-community-server/manage.py \
  --config ~/.config/coast/community-server.json \
  backup ~/.local/share/coast/backups/community-YYYY-MM-DD.tar
```

Backups contain account secrets and SSH keys and remain mode 0600. A verified
backup is stored in `~/.local/share/coast/backups/`. Move the public bundle
and a trusted private backup separately when relocating the service, then
follow the bundle's restore instructions with an empty state directory.
Database, Git history and SSH identity survive a move. External email and a
physical connection between two computers have not been tested; local mail
and HTTPS behind a trusted loopback proxy have been tested. No work-network
ports or firewall rules were changed.

Native pacman downloads enforce the upstream UID/GID, NNP, Landlock ABI 3 and
seccomp policy. Coast still has its own kernel and musl ports; arbitrary Arch
binaries and recipes are not automatically compatible, and unimplemented Linux
facilities such as general chroot and locked secret memory are not advertised
as supported. See [the security notes](local-repositories.md#trust-and-local-service-boundaries).

The selected live images are `build/Coast-browser.iso` and `build/Coast-codeoss.iso`,
both pointing to the image with the current native build kernel and source-built
GNU application ABI libraries. Its qualification receipt is
`build/coast-source-glibc-r3/MANIFEST.json`; selection and test hashes are in
`build/source-glibc-image-selection.json`. It passes BIOS/UEFI boot, Chromium,
Code OSS, GNU runtime and installed QEMU checks. The compact Chromium image
remains available through the corresponding `-rollback.iso` links. Earlier images
also remain available at their recorded build paths. The top menu bar uses
the desktop's translucent blurred glass, and literal short package searches
prioritize package names: `coast search g` starts with `gcc`, followed by
`git`, without Qt matches from descriptions. `coast search g++` finds the
compiler package that provides that command.
