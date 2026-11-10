# Upstream shell, text, scripting and database ports

This bounded profile rebuilds upstream source for x86-64 Linux/musl 1.2.5. It
uses the existing GCC 14.2 toolchain and Linux headers. It does not establish
compatibility with untouched distribution executables or glibc.

| Source | Version | Installed commands | License |
| --- | --- | --- | --- |
| GNU Bash | 5.3 plus official patches 001–020 (5.3.20) | linux-bash | GPL-3.0-or-later |
| GNU grep | 3.12 | linux-grep | GPL-3.0-or-later |
| GNU sed | 4.10 | linux-sed | GPL-3.0-or-later |
| GNU diffutils | 3.12 | linux-diff, linux-cmp, linux-diff3, linux-sdiff | GPL-3.0-or-later |
| Lua | 5.5.1 | linux-lua, linux-luac | MIT |
| SQLite | 3.54.0 | linux-sqlite3 | Public domain dedication |

The `linux-*` aliases point into `/usr/lib/coast/linux-ports/`. Existing
native names retain their identities. Headers and PIC static libraries are
under that prefix's `sdk/include` and `sdk/lib`. The runtime also includes
licenses, source archives and official Bash patches. The full source delivery
contains the build scripts, signatures and verification records.

`userland/vendor/linux-ports-sources.json` records official URLs, versions,
checksums and signature verification. GNU archives and Bash patches were
verified with the official GNU keyring. Lua's published SHA-256 and SQLite's
published SHA3-256 and archive size were checked. Only Bash's official patches
modify an upstream application source; no Coast application patches are
applied to these six projects.

## Build and use

Restore the existing compiler first if its generated cache is absent, then run:

```sh
python3 tools/build_gcc.py
python3 tools/build_linux_ports.py --jobs 1
```

The builder refuses active QEMU guests and permits one host job. It installs
into runtime staging and never installs host packages. `--package NAME`
selects one project; `--checks-only` rebuilds the in-tree verification fixtures
after the ports and toolchain exist. Normal image generation builds native
programs from their current sources. The task's final verification used an
isolated tree; the working user's generated binaries and ISO were preserved.

Examples inside Coast:

```sh
linux-bash --noprofile --norc -c 'a=(red blue); echo "${a[1]}"'
linux-grep -r -n -E 'warning|error' /tmp/project
linux-sed -i.bak 's/before/after/g' /tmp/project/file
linux-diff -u /tmp/old /tmp/new
linux-lua -e 'print(6*7)'
linux-sqlite3 /tmp/example.db 'CREATE TABLE t(n); INSERT INTO t VALUES(42);'
```

For native compilation using the new libraries:

```sh
p=/usr/lib/coast/linux-ports/sdk
gcc-dynamic -std=c11 -I"$p/include" -I"$p/include/lua5.5" \
  /usr/src/coast/linux-ports/examples/library-check.c \
  -L"$p/lib" -llua -lsqlite3 -lm -ldl -pthread -o /tmp/library-check
mkdir -p /tmp/linux-ports-work
/tmp/library-check
```

The same example is tested with the installed static-default `gcc` as well as
`gcc-dynamic`. These are PIC archives, not newly provided shared Lua/SQLite
DSOs. The shared consumer uses the existing shared musl ABI.

Bash is built without Readline; GNU grep has no PCRE2 backend. GNU message
catalogs are disabled. Lua's language, file/process I/O and bytecode workflows
are tested; arbitrary extension modules are not established. SQLite is built
with thread safety, FTS5 and math functions. The tested filesystem profile uses
DELETE rollback journals. WAL, crash recovery under arbitrary power loss,
network filesystems and physical hardware remain unverified.

## Actual compatibility failures and fixes

The initial guest baseline exposed `SQLITE_IOERR_LOCK` (extended code 3850),
with `fcntl` returning `ENOTSUP`. The kernel now implements nonblocking POSIX
record locks through Linux-numbered `fcntl` 72, commands `F_GETLK` and `F_SETLK`:

- Real shared/read and exclusive/write ranges, relative offsets, negative
  lengths, zero length to EOF, splitting, conversion and merging.
- Process/TGID ownership, shared by threads, absent after fork and preserved
  across exec unless a descriptor for that inode is closed.
- Closing **any** descriptor for an inode releases that process's locks on it,
  including dup2 replacement, close-range, CLOEXEC and final process exit.
  Internal VFS references and syscall pins do not trigger descriptor-close
  semantics. BSD flock remains independent.
- Foreign-owner conflicts return `EAGAIN`; invalid pointers, modes, ranges and
  overflow return errors. The global mounted-regular-file inventory holds at
  most 256 segments. Full-table and split failures return `ENOLCK` atomically.
- Browser roles are denied the new lock commands; their existing executable
  allowlist remains intact. New assertions extend the established capability
  fixture, refreshed only in the task's isolated image.

Blocking `F_SETLKW`, OFD locks, mandatory locks and non-regular-file record
locks remain explicitly unsupported. This is a documented subset, not a claim
of complete Linux locking support. Semantics follow the Linux man-pages
[record-locking interface](https://man7.org/linux/man-pages/man2/F_GETLK.2const.html)
and the kernel's [independent BSD/POSIX lock policy](https://docs.kernel.org/filesystems/locks.html).

A second reproducer found a native SDK linker issue: GNU ld 2.37 misrelaxes a
newer `PUSH ...@GOTPCRELX` instruction into a call. A guest-built Lua/SQLite
consumer consequently hit musl's invalid-free guard. A controlled assembly
reproducer and guest debugger capture identify the instruction transformation.
Both new PIC archives now use `-Wa,-mrelax-relocations=no`, producing standard
GOTPCREL records supported by the packaged linker. The upstream sources and
kernel allocator are unchanged. Native static and shared consumers exercise
the repaired integration.

Diffutils 3.12's generated `strcasecmp` configure probe attempts target
execution even in cross mode. Its cached result is supplied only after an
exhaustive byte comparison program, built against the actual pinned target
libc, passes on Linux. Source inspection and locale results are recorded;
this does not promise ISO-8859-1 locale support.

## Verification

`tools/test_linux_ports.py` checks versions and aliases and runs 19 workflows:
Bash arrays/functions/pipelines/background wait, grep recursion/NUL records,
sed backups/multiline edits, diff/cmp exit statuses, diff3/sdiff, Lua coroutines,
70 KiB file seek/rename/process I/O, bytecode, SQLite JSON/FTS5, file-backed
commit/rollback/reopen, concurrent writers/readers through CLI and C API,
native static/shared SDK consumers, record-lock lifetime/error/capacity
semantics and browser denial checks.

The SQLite CLI correctly returns exit 1 for a locked database; its C API
returns `SQLITE_BUSY` 5. Both are checked against actual Linux oracles.

`tools/test_linux_ports_storage.py` uses only disposable FAT images. It writes
a 128 KiB SQL payload, commits and rolls back transactions, closes the guest
after sync, then reopens the database in a fresh guest. It verifies every
exported byte with SHA-256, checks database integrity, rejects writes on a
read-only remount, and compares the complete extracted database payload with
a host reader. This is synced fresh-guest persistence, not a power-loss claim.

The final validation manifest records actual one/four-CPU results and existing
GUI, storage, dynamic C/C++, threads, Doom, power and host regression gates.
Only completed gates are reported passed; baseline failures remain in the
validation evidence.
