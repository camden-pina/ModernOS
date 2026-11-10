# Coast Packages

Coast Packages is the local package service for Coast. Its client command
is `coast`. The Arch User Repository inspired the reviewed source-recipe workflow;
this service is not named AUR and does not connect to Arch's repositories.
Upstream pacman/libalpm, makepkg, repo-add, GnuPG and Git provide the actual
package transactions, source builds, version ordering and signatures.
The separate Coast Community service runs upstream aurweb 6.5.1, including its
real accounts, SSH submissions, package discussions, moderation and AUR v5 API.
The signed binary publisher and community submission service have separate trust.

## Community service

The portable community bundle is `build/Coast-community-server`. It contains a
rootless OCI image, the upstream GPL source, Coast's overlay, and `manage.py`.
It requires Linux x86-64, rootless Podman and Python 3.11 or later. The default
website is `http://127.0.0.1:8770`; the VM reaches it at `http://10.0.2.2:8770`.
SSH Git submissions use port 2222. All current host listeners remain loopback.

```sh
python3 build/Coast-community-server/manage.py start
python3 build/Coast-community-server/manage.py status
coast community --check
coast search --community partial-name
coast show PACKAGE --community
coast clone PACKAGE --community
coast install PACKAGE --community
coast update PACKAGE --community
```

`coast search` combines signed packages, signed recipes and community recipes,
with the same literal ranking. `--repo` selects signed binaries; `--source`
selects recipes; `--community` selects community submissions. The v5 API's
two-character minimum does not prevent Coast's one-character search: the client
reads the bounded public metadata index for short queries. `coast search g`
puts GCC before build metapackages and ignores incidental `g` characters in Qt
descriptions. Installation always requires exact package names.

`coast community URL --check` checks the API without changing settings.
`coast community URL` authenticates a change to root-owned configuration;
`coast community off` disables community queries. Changing that URL preserves
the signed binary origin and publisher fingerprint. Signed binaries and the
signed source catalog take precedence unless `--community` is explicit.
Community metadata and Git are untrusted input; a submission never grants
publisher signing authority. The client disables Git hooks, external protocols
and global Git configuration, validates recipe review files, and shows terminal
control characters visibly. `--noconfirm` cannot bypass recipe review.

Registration, email verification and password reset use the actual upstream
workflow. This local deployment puts outgoing messages in a private `.eml`
outbox and sends no external email. The operator can use `manage.py mail` to
list messages and `manage.py mail FILENAME.eml` to read a verification/reset
message. Initial administrator credentials are generated under the private
state directory in `ADMIN-CREDENTIALS.txt`, mode 0600. Normal startups preserve
password changes. The website supports SSH keys, comments, votes, subscriptions,
flags, co-maintainers, package requests, disowning, moderator-approved orphan
adoption, moderation and package-maintainer voting.

The public bundle excludes private state and the package signing key. Keep the
database, Git store, SSH host key, session secret and local mail outbox private.
`manage.py backup /PRIVATE/backup.tar` stops the owned container, archives the
database and Git consistently with their numeric ownership, and restarts it.
The backup is mode 0600 and contains secrets. Copy the public bundle and this
private backup separately to another host, then restore into empty state with
`manage.py restore /PRIVATE/backup.tar`. Existing state is never overwritten.
The bundle README describes host names, TLS reverse proxies and ports for a
future controlled network. No firewall or work-network port changes are needed
for the current VM setup.

## Start the host service

Run this on the host computer, outside the VM:

```sh
make serve-packages
```

The development catalog is available at `http://127.0.0.1:8765`. By default,
the server binds to host loopback. With the standard QEMU user network, the VM reaches that same
service at `http://10.0.2.2:8765`. The host serves signed binary packages,
signed metadata, Git recipe repositories and checksum-pinned source archives.
It does not install host software, run recipes, accept uploads, or expose the
repository's private signing key. No host administrator privileges are needed.

Keep this terminal running while using the repository. In another terminal:

```sh
make run ISO_IMAGE=build/Coast-codeoss.iso SMP=2
```

The VM needs its usual user network (`-nic user,model=e1000e`). No disk is
needed. This remains a live image: package and user-file changes stay in RAM
and reset when the VM shuts down. Persistent installations belong to the
future installer and are not a requirement for this live package service.

## Host on another computer

The server also supports ordinary IPv4 Ethernet/Wi-Fi networks on its host.
Coast itself needs a supported Ethernet adapter; it has no Wi-Fi driver yet.
The package protocol does not depend on QEMU. `10.0.2.2` is only the default
development address. A real client uses the repository computer's LAN address
or a DNS name that resolves to it.

Export a portable snapshot on the build computer:

```sh
make export-packages REPOSITORY_EXPORT=build/coast-repository-server
```

Copy that entire output directory to the repository computer, for example to
`/srv/coast-packages`. It contains signed packages, public keys, source archives,
static Git origins, the catalog page and the Python server. Its manifest uses
relative paths, so the build workspace is unnecessary on the other computer.
It contains neither the package signing private key nor a requirement to build
or execute Coast programs on the server. Python 3.11 or later is required.

For a LAN server at the example address `192.168.1.50`, run there:

```sh
python3 -B /srv/coast-packages/serve_repository.py \
  --repository /srv/coast-packages/repository.json \
  --bind 0.0.0.0 --port 8765 --public-url http://192.168.1.50:8765 \
  --allow-client 192.168.1.0/24
```

Replace the address and network with the actual ones. Permit inbound TCP8765
in that computer's firewall for the client network. No router port forwarding
is needed on the same LAN. The address in `--public-url` is also the allowed
HTTP Host authority; arbitrary Host values and cross-site browser requests
remain rejected. Repeating `--allow-client` enables more client subnets. Without
that option all clients that can reach the listener may read its public files.

On Coast, as the normal user:

```sh
coast server http://192.168.1.50:8765 --check
coast server http://192.168.1.50:8765
coast refresh
coast install bc
```

The first command checks connectivity and verifies the signed database and
source catalog against the key already installed in Coast. The second repeats
verification, then authenticates through real sudo to update the client,
pacman and makepkg settings together. It never imports a key from the network
or disables signature checks. `coast server` without an address shows the
current server and pinned signing fingerprint. `coast repo` remains available.
Configuration files are staged under a lock, replaced individually with atomic
renames and restored on an ordinary write failure; this is not a guarantee of
a cross-file transaction surviving sudden power loss.

For a repository on an untrusted or routed network, use HTTPS. Provide a
certificate whose IP/DNS subject matches the client URL and whose issuing CA
the Coast system trusts. The normal certificate-chain and hostname checks
remain enabled. The listener accepts `--tls-cert CHAIN.pem --tls-key KEY.pem`;
set `--public-url https://packages.example:8765` accordingly. TLS private keys
stay outside the public snapshot. Signed packages protect integrity over HTTP,
but HTTP does not provide server authentication, privacy or replay protection.
HTTPS complements package signatures; it does not replace them.

The exported `deployment/` directory includes JSON configuration examples and
an optional Linux systemd service. To enable it on a Linux repository computer:

```sh
sudo cp /srv/coast-packages/deployment/server.example.json /etc/coast-packages-server.json
# Edit the JSON with the actual server address and allowed client subnet.
sudo cp /srv/coast-packages/deployment/coast-packages.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now coast-packages.service
```

Keep the snapshot root-owned, readable, and not writable by the service. The
service runs as a dynamic unprivileged account with a read-only filesystem view.
For TLS, use `server-tls.example.json` and place `tls-credentials.conf` at
`/etc/systemd/system/coast-packages.service.d/tls-credentials.conf`; systemd
then supplies the certificate and private TLS key as service credentials.
A non-systemd server can run the same Python command under its service manager.
To publish an update, export a fresh complete snapshot and restart the service
against it. Live mutation is refused by the server's artifact identity checks.

## Physical installation status

Network package delivery and installation to a permanent system disk are
separate capabilities. Coast currently boots a RAM root from its initramfs.
The ext filesystem driver is read-only. FAT32 can store ordinary files, but
does not supply the ownership, permissions and symlink semantics required for
a secure installed root. There is no permanent-system installer yet. Copying
the live image to a drive does not make package changes persist.

A physical installation needs a writable root filesystem with real ownership
and permissions, a disk-root boot path, an installer/bootloader setup and
reboot/recovery tests on disposable disks. Physical NIC and storage support
also need verification for the target machine. Current Ethernet drivers cover
selected Intel e1000/e1000e and I225/I226/82576 devices; QEMU qualification does
not by itself prove a particular motherboard's physical PHY works.

For a different local port, start the server with `--port PORT`, then change
`/etc/coast/packages.json` and `/etc/pacman.conf` inside the disposable VM to
use the corresponding `10.0.2.2:PORT` address. The default configuration points
to port 8765. A standalone offline image can instead be built with
`--repository-mode embedded`; hosting is the default.

## Find, install and remove programs

In the VM's Terminal:

```sh
coast search g                    # GCC/G++, Git, Gawk and other partial matches
coast search g++                  # literal plus signs, case-insensitive
coast search --repo editor        # signed binary packages only
coast search --source ninja       # reviewed source recipes only
coast search --json g             # structured results, commands and installed versions
coast info gcc
coast repo                        # configured server and trusted key fingerprint
coast list                        # installed packages
coast install bc                  # signed binary package; sudo authenticates installation
coast install g++                 # exact provided alias for the gcc package
coast remove bc
coast remove --unused bc           # also remove dependencies that are no longer needed
coast upgrade                     # upstream pacman -Syu
coast refresh                     # verify and refresh the source catalog as your normal user
```

Search matches partial package names, executable names, provided aliases and
descriptions. Multiple terms must all match, and exact names/commands appear
first. Package names rank ahead of command aliases and descriptive metadata;
shorter matching names appear before longer names at the same rank. One-letter
queries match name/command/alias prefixes, so `coast search g` starts with GCC
and omits unrelated graphical packages. Description matching starts at three
characters; useful name substrings and literal names such as `g++` still work.
Results distinguish `coast/` signed packages from `recipes/` source
recipes. The default image includes signed bootstrap metadata, so discovery
works before contacting the host; installing a package that is not in the
VM's cache requires the host service. `coast refresh` verifies the catalog before
atomically selecting its new cached generation. A failed refresh keeps the
previous generation intact. Installation and removal require exact names,
never whichever partial search result appears first.

The repository contains 36 packages: GCC/G++, binutils, Make, Gawk, Git, Nano,
Vim, Python, jq, Ninja, pkgconf, m4, patch, gperf, bc/dc, Zstandard, LZ4, XZ,
rsync, the Qt 6 demo, GNU grep, sed, diffutils, Lua, SQLite, the package tools
and their metapackages. GCC owns its compiler drivers and C/C++ headers; G++
is part of that package. `coast-base` represents the kernel, desktop,
musl, shared compiler runtime and other essential base tools. `base-devel`
depends on the tools needed for native source builds.

The upstream commands remain available:

```sh
pacman -Ss calculator            # upstream regular-expression search
pacman -Si bc
pacman -Q
pacman -Qo /usr/bin/bc
pacman -F usr/bin/g++
sudo pacman -Fy                  # refresh the signed executable/file index
sudo pacman -S bc ninja
sudo pacman -Rns bc
pacman -Qkk ninja
sudo pacman -Sc                  # remove old VM package-cache entries
```

Dependencies, ownership, transaction locks, conflicts, configuration backups
and version constraints use the real `/var/lib/pacman/local` database and
libalpm. `pacman` and `coast-base` remain in `HoldPkg`. The helper refuses an
invalid/missing signed command index instead of silently returning incomplete
results. Refresh that index with `sudo pacman -Fy` after repository updates.

## Review and build source recipes

Binary installation is the default. To build from source instead:

```sh
coast refresh
coast info --source hello-coast
coast show hello-coast          # verifies the recipe file against the signed catalog
coast clone hello-coast         # real Git over HTTP; verifies the expected Git commit
cd ~/Builds/hello-coast
cat PKGBUILD                     # inspect source, patches and install scripts too
makepkg --printsrcinfo
makepkg -si                      # normal-user build; sudo installs the result
```

`coast build NAME` builds without installation. `coast install --source NAME`
combines checkout, explicit review and upstream `makepkg --syncdeps --install`.
`coast update NAME` fetches the Git repository, verifies its head against the
signed catalog and merges only a fast-forward. `coast checkupdates` compares
installed versions with recipe versions using upstream vercmp. `coast rpc`
provides version-1 source-catalog search/info JSON. The seed recipes are a
native example, GNU bc and Ninja. Their source archives are served by the host
and verified by makepkg against pinned SHA-256 checksums.

Review is required even with `--noconfirm`. `--reviewed` is an explicit assertion
that the caller has already reviewed the checkout, for automation. Recipes are
executable shell code with access to the building user's files; the signature
identifies the repository publisher and does not make a recipe safe. Root
builds are refused. Fakeroot produces archive ownership metadata while the
kernel still enforces the real normal-user UID and filesystem permissions.
Only the explicit installation step authenticates through sudo and pacman.

Coast uses musl and its own kernel ABI. Arbitrary Arch glibc binaries and
unported recipes are not automatically compatible. User checkouts remain
editable, real Git repositories; inspect and port dependencies before building.

## Trust and local-service boundaries

Binary packages, the binary database and the file database require trusted
OpenPGP signatures (`SigLevel = Required`). The signed source catalog is checked
against the exact fingerprint in root-owned `/etc/coast/packages.json`.
Recipe Git commits and displayed recipe-file SHA-256 hashes come from that
verified catalog. The image contains public trust data, never the host signing
secret. The server serves only a verified public-artifact allowlist; it does
not browse the host filesystem or execute Git/PKGBUILDs in response to requests.
HTTP byte ranges support package-download resumption. Browser requests use
same-origin policy, a restrictive content-security policy and local Host checks.

Transport is local HTTP through QEMU's host gateway. Integrity and publisher
trust come from the pinned signatures and source checksums. There is no public
Internet repository or remote publishing endpoint. Source review and package
installation happen in the VM, and the server makes no privileged host changes.

The downloader uses upstream libalpm's internal curl path and actual download
sandbox: real UID/GID 990 (`alpm`), cleared supplementary groups, `no_new_privs`,
Landlock ABI 3 and libseccomp 2.6.1. The filesystem policy permits reading the
system and writing only the download cache; execution is denied. The kernel
enforces rules by inode, checks symlink targets and rename/hardlink permissions,
and preserves restrictions across fork, clone, native spawn and exec. Ordinary
seccomp supports classic BPF, thread synchronization, ALLOW/ERRNO/TRAP and kill
actions. Unsupported actions/flags are refused; this does not advertise newer
Landlock ABIs, seccomp notification/tracing, or every Linux kernel facility.
Native general chroot and locked secret-key memory remain platform work;
unsupported setup does not report fake success.
GnuPG's insecure-memory warning remains visible where it is emitted. The
private repository signing key stays exclusively on the host.

The native archive tools include OpenSSL digest support, so makepkg records real
SHA-256 hashes in `.MTREE` and `pacman -Qkk` can verify files built inside Coast.
Normal source builds retain the account's ordinary filesystem access, as in
the AUR's makepkg workflow. Download confinement does not make a malicious
PKGBUILD safe to run; review it and its sources before building.

`LocalFileSigLevel = Optional` permits reviewed, explicitly installed local
builds to be unsigned, as required by `makepkg -si`. A present invalid signature
is rejected. Package install scripts can run with root privileges during an
authorized transaction. The initial repository supplies no custom privileged
hooks. Existing account, browser/editor namespace and pathname Unix-socket
permission boundaries remain in force. The makepkg Bash port uses upstream
Bash's descriptor-duplication fallback because native `/dev/fd` is not complete.
`CheckSpace` stays off for the live RAM filesystem's unknown capacity, as with
Linux ramfs. None of this claims complete Linux ABI or security equivalence.

## Publish a new local build

Host builds run confined, with read-only sources, a selected writable output
and no network during compilation. They use cross mode; target programs and
PKGBUILDs never run on the host. Known host generators build/sign package
archives, create Git repositories and update Git's static HTTP discovery files.
The host server is a read-only consumer of those completed artifacts.

```sh
make local-repository LOCAL_REPOSITORY_OUT=build/local-repository-new
python3 -B tools/serve_repository.py \
  --repository build/local-repository-new/BUILD.json
```

The default signing directory is `build/coast-signing-keys`. It must be private (mode 0700) and outside the publication
output. Preserve it when clearing caches. `BUILD.json` records verified public
files for the server and installed files/metadata for the image builder.
Hosted images contain installed programs, public trust and bootstrap metadata;
the package mirror, Git origins and recipe distfiles stay on the host.
Corresponding upstream sources, licenses and port recipes still accompany the
image for auditing and license compliance. Add/review a recipe and its source
checksums, port its native dependencies, then publish to a new unused output.
Restart the server with that completed manifest to expose the new snapshot.

Validation covers the real HTTP server, path/signature boundaries, native VM
HTTP/Git access, package install/remove/restore, normal-user source builds and
exact restored compiler execution. All test VMs are owned disposable guests;
no host package database, raw drive or system account is changed.

## Coast system identity

The kernel and hostname identify as `Coast` and `coast`. Native preferences,
account policy, SDK headers, shared files and port sources use `coast` paths.
The base package is `coast-base`; the source helper is `coast-packages`.
The current editor image is `build/Coast-codeoss.iso`, and its host repository
is selected by `build/Coast-packages`. Previous qualified images and immutable
upstream build receipts remain available for rollback and provenance.
