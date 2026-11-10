# Contributing

Repository: https://github.com/camden-pina/ModernOS.git

Keep generated programs, downloaded archives and private signing state out of
Git. Run `make source-check` before committing; it checks tracked files as well
as new files, including files force-added despite `.gitignore`.

Add upstream URLs and SHA-256 pins with new dependencies. Keep port patches and
configuration in this tree, put downloads and build outputs in ignored caches,
and preserve upstream license notices. Build recipes should not depend on a
particular developer's home directory.

Build the kernel with `make -j4` and the desktop with
`make -C userland bin/gui -j4`. Run checks appropriate to the change; the GitHub
workflow runs source-publication checks, host services, boot configuration,
and fresh kernel/desktop compilation. Guest changes also need the relevant
QEMU integration check. State whether a result came from a host fixture or an
actual Coast guest.

See `Documentation/source-builds.md` for the native rebuild and full desktop
bootstrap paths, and `LICENSES.md` for current licensing status.
