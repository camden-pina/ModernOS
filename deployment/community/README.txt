Coast Community — portable local server

This bundle runs the actual aurweb 6.5.1 application, MariaDB, Valkey, a
read-only anonymous Git/cgit interface, and SSH Git submissions in a rootless
container. Coast adds branding, trusted browser origins, per-request database
sessions, local mail delivery, a supervisor, and portable administration.

Requirements: Linux x86-64, rootless Podman with subordinate UID/GID mappings,
Python 3.11 or later, and enough space for the OCI image plus private state.
The image and upstream/Coast source are included. Running it needs no Arch
installation on the host and no access to the host's package signing key.

Start here, from this directory:

  python3 manage.py start
  python3 manage.py status

Open http://127.0.0.1:8770 . The default SSH port is 2222. Only localhost is
published. No firewall, router, disk-partition or system-boot changes occur.
server.json selects the container name, ports, public URL and state directory.
Relative state paths are relative to that configuration file, so moving the
whole bundle and state directory preserves the deployment.

The initial administrator login is generated on first start and written only
to state/ADMIN-CREDENTIALS.txt (mode 0600). Change its password in My Account.
Subsequent starts never reset that account's password. State also contains the
database, recipe Git repository, SSH host key, session secret and mail outbox.
It must remain private; do not publish it with the public bundle.

Local verification/reset email is kept in the private outbox; no message is
sent over the work network. The administrator can list/show local mail:

  python3 manage.py mail
  python3 manage.py mail MESSAGE-FILENAME.eml

Real account verification, password reset, SSH keys, comments, votes, flags,
subscriptions, co-maintainers, requests, orphaning, moderator-approved
adoption, deletion/merging and package-maintainer voting use upstream aurweb.
Recipe submissions never execute PKGBUILDs on this server. Community recipes
are not signed binary packages, and are not automatically trusted.

From a QEMU Coast VM on this host:

  coast community http://10.0.2.2:8770 --check
  coast community http://10.0.2.2:8770
  coast search --community partial-name
  coast show PACKAGE --community
  coast clone PACKAGE --community
  coast install PACKAGE --community

Search is literal and supports short partial names. A build runs as the
normal account, after source review. Installation authenticates through real
sudo/pacman. A normal PKGBUILD build can access that account's files, as in
the AUR workflow; review is not replaced by isolation. Arbitrary Arch glibc
packages are not automatically Coast-compatible.

For a consistent private backup, this stops the owned container, snapshots
the database and Git together with their numeric ownership, and restarts it:

  python3 manage.py backup /PRIVATE/PATH/coast-community-backup.tar

The backup includes credentials and keys and is mode 0600. Copy it securely.
To restore on another Linux host with rootless Podman, copy this bundle and
the backup, keep state empty, then:

  python3 manage.py load
  python3 manage.py restore /PRIVATE/PATH/coast-community-backup.tar
  python3 manage.py start

Restore only a trusted private backup. Existing state is never overwritten.
The SSH host identity is preserved; verify its fingerprint after a move.

For a real server on a network you control, use a trusted TLS reverse proxy
on that machine, set public_origin to its HTTPS origin and public_ssh_port
to its SSH forwarding port, and configure real email delivery before adding
remote users. Keep database/cache ports private. The app container remains
bound to localhost behind the reverse proxy; it trusts only its configured
Host and nginx. No LAN exposure is enabled by this bundle's default config.
No physical two-computer or target-laptop installation is claimed by VM tests.

Stop with python3 manage.py stop. A user systemd service can use:
ExecStart=/usr/bin/python3 /ABSOLUTE/BUNDLE/manage.py --config /PRIVATE/server.json serve
ExecStop=/usr/bin/python3 /ABSOLUTE/BUNDLE/manage.py --config /PRIVATE/server.json stop
User-login autostart works without enabling linger or changing host firewall
rules. On this work network, leave the default localhost binding unchanged.

Build provenance and checksums: bundle.json. Complete upstream aurweb source
and GPL-2.0-only COPYING are under source/. Coast's overlay and Containerfile
are included there as well. The base image and installed dependency versions
are recorded by the OCI image; their upstream distributions retain their own
licenses. Keep these sources and notices with copies of the bundle.
