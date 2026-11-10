# Python and jq

Coast includes statically cross-compiled **CPython 3.14.10** and **jq 1.8.2**.
Use them in Terminal or at the physical console with ordinary account privileges:

```sh
python3 --version
python3 -c 'import json; print(json.dumps({"answer": 42}))'
python3 /home/user/Projects/script.py
printf '{"values":[1,2,3]}' | jq '.values | add'
```

`python` and `python3` select the installed `python3.14` interpreter. Its
source-only standard library is in `/usr/lib/python314.zip`; supported C
extension modules are linked into the interpreter. This includes JSON, regular
expressions, math, compression through zlib/gzip, SQLite, SSL, hashing, threading
and subprocess support. The port builds a separate **thread-safe OpenSSL 3.5.9**
instead of using the browser's single-threaded dependency profile. Certificate
verification uses `/etc/ssl`; it is enabled by default.

jq includes its bundled Oniguruma regular-expression engine and decimal-number
support. It operates on real stdin/stdout/files, supports shell pipelines and
returns its upstream parse-error and `-e` exit statuses.

The [additional program ports](additional-programs.md) add Ninja, pkgconf, m4,
gperf, calculators, GNU patch, rsync and compression tools with `make more-programs`.

## Build

The existing musl cross compiler, zlib, SQLite SDK, Python 3.14 host interpreter,
Perl, make and Bubblewrap are required:

```sh
make program-ports
make iso
```

The builder downloads only the pinned official Python/jq archives when missing
and verifies their published SHA-256 checksums. It uses the already-pinned
OpenSSL source archive. Compilation runs without a network, with read-only
workspace and source mounts; only the chosen output is writable. Configure uses
cross-compilation mode. Host Python runs the upstream code generators; target
executables are never used as host build tools or installation helpers.

`tools/build_program_ports.py --package python` or `--package jq` selects one
program. `--install-runtime` stages the built files for normal image generation.
The Code OSS image builder also accepts `--program-ports-build <BUILD.json>` and
checks program, recipe and fixture hashes before packaging. Build receipts,
module availability and logs remain under `build/program-ports-r1`. Licenses and
the corresponding source archives are included in the guest image.

## Current scope

The port retains Coast's existing filesystem, account, descriptor and network
restrictions. Installing an interpreter does not grant additional privileges.
Files on the boot RAM filesystem disappear on reboot. Socket/network use is
subject to the existing kernel policy; arbitrary Python networking and external
HTTPS are not established by the offline port checks.

The initial Python package excludes pip/ensurepip, Tk/IDLE, readline, ctypes,
curses, bz2/lzma/zstd and external database bindings. Standard-library `decimal`
uses its Python fallback. The basic interactive REPL is tested; use
`PYTHON_BASIC_REPL=1 python3` to select it explicitly. Third-party binary wheels,
in-guest extension builds and arbitrary Linux Python packages need separate
validation. This is a static musl source port, not glibc binary compatibility.
The build disables `os.sched_getaffinity`, whose libc declaration has no
implemented kernel operation; Python uses its upstream CPU-count fallback.
Signal handlers can request `SA_ONSTACK` with no alternate stack, which uses
the normal stack as [specified by Linux](https://man7.org/linux/man-pages/man2/sigaction.2.html).
Registering an alternate signal stack remains unsupported.

The profile launcher explicitly enrolls Bash in managed process-group handling
before exec. Native spawn already makes it a group leader, so Bash would
otherwise skip the enrollment call and background forks could inherit the
foreground. Pipeline siblings can join their existing group within the same
session and UID; parent/child target checks remain in force.

## Validation

`tools/test_program_ports.py` boots an owned, diskless and networkless guest.
It checks the ordinary UID, interpreter versions, Unicode paths, large-file
seeks, rename/symlinks, compressed archives, SQLite commit/rollback/reopen,
threads/futures, OpenSSL hashing and TLS initialization, subprocess pipes and
exit status, shebangs, termination, asyncio, jq regex/transforms/error codes,
system-write denial and the interactive console. The reusable workload is also
installed at `/usr/share/coast/program-ports/check.py`.

Python exposed the missing `FIOCLEX`/`FIONCLEX` ioctls: these now change only the
caller's owned descriptor flags, with invalid/O_PATH descriptors rejected.
The credential fixture compares those operations, including dup independence,
against the same ELF running on Linux.

Sources: [CPython release and checksum](https://www.python.org/downloads/release/python-31410/),
[jq release and checksum](https://github.com/jqlang/jq/releases/expanded_assets/jq-1.8.2).
