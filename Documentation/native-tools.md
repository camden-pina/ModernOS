# Native compilers, editors and build utilities

ModernOS ships GCC/G++ 14.2.0, GNU binutils 2.37, Vim 9.2.0000, BusyBox 1.37.0,
GNU make 4.4.1 and GNU awk 5.4.0,
linked statically with the existing musl 1.2.5/ncurses 6.5 toolchain. They execute
in native ring-3 processes and use the real OS filesystem, pipes and terminals.
The ISO includes Vim's full runtime: `:help`, syntax definitions, filetype
detection, plugins and manuals under `/usr/share/vim/vim92`.
The compiler SDK includes real C/C++ headers, startup objects, static musl,
libgcc and libstdc++ libraries. Native compiler executables are under `/usr/bin`,
with correct-prefix launch aliases under `/bin` for the OS console.

## Editing

```text
vim /tmp/notes.txt
vim /mnt/nvme1n1p1/notes.txt
vim --version
help vim
```

Press `i` to insert, Escape for normal mode, `u` to undo, `/text` to search,
`:w` to save, `:q` to quit, or `:wq` for both. `:help` opens the actual upstream
manual. Default `/usr/share/vim/vimrc` enables syntax/filetype support and sets
the real POSIX shell to `/bin/ash`. Shell filters such as `:%!sort` fork real
processes through pipes. The normal editor, split windows, undo, search and
Vimscript are upstream implementations. GUI/gVim, terminal-job windows,
channels, X/Wayland, clipboard services, NLS, ACLs and extended attributes
are disabled. Vim's timeout support uses its existing SIGALRM/setitimer branch;
its pthread-based SIGEV_THREAD timer branch is disabled for this target.

The desktop Terminal forwards actual VT output and raw input to Vim, and
delivers SIGWINCH when resized. Global desktop shortcuts retain their ordinary
meaning. Save work before confirming termination of a terminal program.
RAM documents disappear on reboot; writable FAT32 mounts retain documents.
Existing GUI light/dark Settings apply to the terminal and desktop.

## Shells and commands

`build-sh` starts the real BusyBox ash shell with:

```text
PATH=/usr/bin:/bin
SHELL=/bin/ash
CONFIG_SHELL=/bin/ash
```

It accepts interactive commands, script paths and `-c COMMAND`. Pipes,
redirection, variables, globbing, loops, functions and script conditionals
come from upstream ash. Job control is disabled because process groups and
session APIs are absent. From the OS console, script paths and `-c` invocations
of sh/ash/build-sh can run with `&`; interactive and stdin shells require the
foreground terminal. Background descendants retain kernel input/display
access restrictions. The console's PID-1 shell and desktop shell retain
their OS commands (`mount`, `lsblk`, `gui`, etc.). Their basic commands keep
their existing syntax. `build-sh` selects full upstream grep/egrep/fgrep,
head/tail/wc, find, hexdump and stat in `/usr/bin`. For individual commands,
`busybox APPLET ...` always reaches the upstream implementation.

```text
build-sh
build-sh -c 'printf "b\na\na\n" | sort | uniq'
build-sh ./configure --prefix=/usr
build-sh -c 'make -j2'
busybox grep -En '^[A-Z]+' /tmp/notes.txt
busybox --list
busybox tar --help
```

Compiled applets include ash, awk, sed, grep/egrep/fgrep, head, tail, wc, find,
diff, patch, sort, uniq, tr, cut, cmp, tee, xargs, printf, expr, test, seq,
basename, dirname, readlink, realpath, ls, cp, mv, mkdir, rmdir, rm, cat, echo,
pwd, chmod, touch, ln, install, mktemp, date, getopt, du, od, hexdump, stat,
cksum, md5sum, sha256sum, tar, gzip/gunzip/zcat, bzip2/bunzip2/bzcat,
xz/unxz/xzcat, sleep, uname, true, false and yes. Xz is decompression only.
`busybox --list` is authoritative; options and invalid-argument messages come
from the actual program. `help COMMAND` gives an OS usage page for major tools.

`awk` and `gawk` run the actual statically linked GNU awk 5.4.0. BusyBox's awk
remains explicitly accessible as `busybox awk`. GCC's option generators require
GNU replacement escaping: BusyBox silently removes the backslashes needed to
recognize `C++`/`ObjC++`, producing unusable C++ option flags. The native rebuild
explicitly selects GNU awk and checks this behavior before configuring. Dynamic
extensions, persistent-memory allocation and MPFR arithmetic are disabled;
ordinary programs, regex, arrays, input/output and generator scripts are upstream.

`/bin/sh` is now the real POSIX ash implementation. `/bin/modernos-sh` retains
the OS command console and desktop-terminal interface. `#!` scripts execute
through their actual interpreter, with checked recursion and argument handling.

`make` uses `/bin/ash` for recipes and real filesystem modification times for
dependencies. `-j` uses upstream's pipe jobserver, with native child processes
and exit statuses. Named FIFO jobservers are disabled. There is no simulated
compiler or recipe success: failed commands stop the recipe.

## Compile your own programs

```text
gcc -O2 -Wall /tmp/hello.c -o /tmp/hello
/tmp/hello
g++ -std=c++17 /tmp/example.cpp -o /tmp/example
gcc -c /tmp/library.c -o /tmp/library.o
ar rcs /tmp/libexample.a /tmp/library.o
gcc /tmp/main.c /tmp/libexample.a -o /tmp/example
```

`cc`/`c++` are GCC/G++ aliases. `cpp`, `as`, `ld`, `ar`, `ranlib`, `nm`,
`objcopy`, `objdump`, `readelf`, `size`, `strings` and `strip` are actual GNU tools.
Prefer the compiler driver for linking so startup objects/runtime libraries
are included. Drivers default to `-static -no-pie`: the loader accepts static
ET_EXEC, validated static TLS and W^X-separated load segments. C++ containers,
exceptions and the static standard library are included; pthreads are disabled.

Examples are installed at `/usr/src/modernos/examples`:

```text
build-sh -c 'mkdir /tmp/example; cp -R /usr/src/modernos/examples/. /tmp/example/; cd /tmp/example; make -j2 check'
```

`make check` also compiles and runs the ABI example. Copy the examples
recursively to retain its `include/memory_usage.h` protocol header.

The target triplet is `x86_64-linux-musl`, selecting x86-64 ELF and musl's
Linux-numbered syscall wrappers. ModernOS implements the explicitly documented
subset itself; no Linux kernel/service is installed or assumed. Unsupported
interfaces still return real errors. Arbitrary Linux/glibc, dynamic, shared-memory
or threaded software needs further porting. `-shared`, PIE and `-flto` output
are outside the supported native execution profile.

## Implemented kernel dependencies

* `fork`, `clone(SIGCHLD)` and `vfork`/`clone(CLONE_VM|CLONE_VFORK|SIGCHLD)`
  create real processes. Fork copies private mappings, including inaccessible
  guard pages; descriptors share offsets and endpoint references. Vfork blocks
  the parent until exec/exit and rejects VM mutation in the borrowed child.
  Static musl `posix_spawn`, pipes, redirection and executable replacement work.
* Registered SIGCHLD handlers receive pending child-exit notifications.
  `getitimer`, `setitimer(ITIMER_REAL)` and `alarm` supply real, cancellable
  SIGALRM deadlines and periodic timers. Timers preserve masks/pending delivery,
  coalesce missed periods and are not inherited by fork. CPU-time timers and
  pthread timer callbacks are unsupported.
* `truncate`/`ftruncate` grow and shrink RAM and FAT32 files, preserving offsets
  and zero-filling exposed bytes. `utime`, `utimes`, `futimesat` and `utimensat`
  update available timestamps, with UTIME_NOW/OMIT and checked pointers.
  RAM has whole-second timestamps. FAT32 stores two-second modification times
  and day-granularity access dates within 1980–2107; inode-change time is unknown.
* RAM supports real symlink creation through `symlink`/`symlinkat`. FAT32 reports
  ENOTSUP. Hard links remain unsupported.
* Closed RAM targets can be replaced by standard `rename`/`renameat`.
  `renameat2(RENAME_NOREPLACE)` preserves the desktop and console's safe refusal
  of occupied targets. Busy/open files remain protected. FAT32 refuses occupied
  rename targets: `sed -i` and replacement patches there report failure. The
  BusyBox patch port checks rename errors and identifies its saved temporary
  edited file, rather than silently reporting success. Work on a RAM source
  tree or write a separate output file on FAT32.
* Real `/dev/null` discards writes and returns EOF, and `/dev/zero` reads zeros.
  They are distinct from terminals, support readiness, and reject positional I/O.
* Compiler-sized inputs use an 8-MiB guarded stack, 4096 argv entries, 512
  environment entries and 128 descriptors. Each string is at most 32767 bytes;
  argv/envp each have a 128-KiB aggregate string budget. Kernel vectors and packed
  environments allocate only the actual measured strings and pointer counts,
  avoiding large fixed scratch buffers on each short exec. String copying validates page-sized
  spans, stopping at a terminator before an inaccessible following page.
* Individual ELF images are limited to 128 MiB, heap/mapping requests to
  512 MiB and RAM files to 256 MiB. Fork eagerly clones at most 1 GiB of mapped
  user pages, without copy-on-write. Private child processes are balanced across
  CPUs at creation; borrowed vfork children retain parent affinity. GNU make uses
  its fork recipe path so `-j` jobs can execute on multiple CPUs.
* `select`/`pselect6` support the full 128-descriptor range. Kernel shebang dispatch
  supports absolute interpreters, one optional argument and four script levels,
  reporting malformed, missing and recursive interpreters accurately. USTAR
  initramfs parsing supports full-width names/prefixes and real symlink entries.
* Background children release console/PTY input when redirecting or closing
  their inherited stdin. Transfer to the live parent is synchronized with exit
  and reparenting, so a detached build cannot strand the command console.
  Canonical input/editing state remains with the terminal during foreground
  transfers: partially typed text survives a fork/redirection/reap handoff.
  Explicit termios flush or mode reset still discards buffered input.
* The kernel heap indexes free blocks rather than scanning all live source
  inodes. It starts with 128 MiB and grows through checked physical-page arenas
  when needed, up to 1 GiB and 256 arenas. Freed blocks are reused; growth arenas
  stay allocated. Metadata stress checks create/remove 327680 real files and
  execute programs between batches. Physical pages use indexed, compressed bitmaps over usable firmware regions;
  interleaved file/process pages no longer exhaust a runtime extent table.
* RAM directories retain their real entry lists and public inode/offset
  semantics, with a bounded hash index for large directories and a checked
  sequential enumeration cache. Mutations invalidate the cache; failed cache
  allocations fall back to list lookup. Kernel memory clearing uses exact
  integer-width stores, including before FPU initialization.

## Reproducible offline build

`make iso` invokes `tools/build_nano.py` for musl/ncurses, then
`tools/build_native_ports.py` for editors/utilities, `tools/build_gawk.py` for
GNU awk, `tools/build_gcc.py` for
the static compiler SDK and `tools/package_sdk.py` for corresponding sources.
Bundled complete archives are
verified against SHA-256 before extraction. The build caches successful outputs,
installs binaries, copies Vim runtime and retains upstream notices in the image.
Generated runtime directories are ignored by Git and recreated by the build.
Host GCC/G++, binutils, GNU make, Python 3.12+, xz and ncurses host tools are needed.
The native port build downloads nothing.

| Package | Source archive | SHA-256 |
| --- | --- | --- |
| Vim 9.2.0000 | [Upstream tag](https://github.com/vim/vim/archive/refs/tags/v9.2.0000.tar.gz) | `875875fb5988af3db0726bef9b048a559a9563aa0ecca8240e82057e8e5941c3` |
| BusyBox 1.37.0 | [Official source](https://busybox.net/downloads/busybox-1.37.0.tar.bz2) | `3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4` |
| GNU make 4.4.1 | [GNU source](https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz) | `dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3` |
| GNU awk 5.4.0 | [GNU source](https://ftp.gnu.org/gnu/gawk/gawk-5.4.0.tar.xz) | `3dd430f0cd3b4428c6c3f6afc021b9cd3c1f8c93f7a688dc268ca428a90b4ac1` |
| GCC 14.2.0 | [GNU source](https://ftp.gnu.org/gnu/gcc/gcc-14.2.0/gcc-14.2.0.tar.gz) | `7d376d445f93126dc545e2c0086d0f647c3094aae081cdb78f42ce2bc25e7293` |
| Binutils 2.37 | [GNU source](https://ftp.gnu.org/gnu/binutils/binutils-2.37.tar.gz) | `c44968b97cd86499efbc4b4ab7d98471f673e5414c554ef54afa930062dbbfcb` |
| GMP 6.2.1 | [GNU source](https://ftp.gnu.org/gnu/gmp/gmp-6.2.1.tar.xz) | `fd4829912cddd12f84181c3451cc752be224643e87fac497b69edddadc49b4f2` |
| MPFR 4.1.0 | [GNU source](https://ftp.gnu.org/gnu/mpfr/mpfr-4.1.0.tar.xz) | `0c98a3f1732ff6ca4ea690552079da9c597872d30e96ec28414ee23c95558a7f` |
| MPC 1.2.1 | [GNU source](https://ftp.gnu.org/gnu/mpc/mpc-1.2.1.tar.gz) | `17503d2c395dfcf106b622dc142683c1199431d095367c6aacba6eec30340459` |

Source changes are explicit in the builder: make's default shell is `/bin/ash`,
and BusyBox patch checks failed replacements. Vim uses build configuration only.
Original sources and licenses remain bundled; notices are installed at
`/usr/share/licenses/native-programs`. Vim retains its own license;
BusyBox uses GPLv2 and GNU make/GNU awk GPLv3 or later.
GCC/binutils use GPLv3-or-later notices; GCC runtime exception and GMP/MPFR/MPC
notices are preserved. Complete source archives remain unmodified. GCC/binutils
ports use target/build configuration rather than undisclosed source patches.
GCC disables bootstrap/multilib/shared libraries/LTO/plugins/threads/PCH,
optional sanitizer/OpenMP libraries and NLS. Its build selects the upstream
read/malloc fallback instead of mmap/madvise optimizations; the kernel also
supports the private file mappings described below.
A source-built musl cross seed builds static native C/C++ tools and runtimes;
build-machine executable aliases are never copied into ModernOS.

## Rebuild GCC inside ModernOS

Start QEMU with sufficient memory (`make run MEMORY=8G`) and run:

```text
bootstrap-toolchain --help
bootstrap-toolchain --work /tmp/toolchain --prefix /tmp/toolchain/install -j4
```

The script verifies `/usr/src/modernos/SHA256SUMS`, extracts GCC/binutils and
GMP/MPFR/MPC, configures the actual static target, builds native binutils,
GCC/G++, libgcc and libstdc++, then compiles/runs C and C++ proofs using the new
drivers. It installs into a separate prefix, preserves the running compiler,
resumes successful stages and stops on any failed operation. This is a source
rebuild using the installed compiler; GCC's three-stage comparison bootstrap
is disabled. `--prepare-only` verifies/extracts sources without compiling.
The bundle also includes musl, GNU make and GNU awk source for further native ports.
The freshly built GCC front ends must pass C11 and C++98/11/17/20 language-mode
checks before building their runtime libraries. Host `-std=gnu++11` flags are
kept separate from runtime flags so each libstdc++ directory selects its own
required standard. Final proofs compile and run C plus C++17 containers,
exceptions and standard output using the rebuilt compiler and runtime.
The native GNU awk binary digest identifies the option-generator cache.
An older workspace without that identity regenerates its derived option tables
and target runtime builds; subsequent resumes retain the matching cache.
Explicit make variables also replace target flags saved by older configuration.
The generator regression uses GCC's upstream make recipes to reproduce the
BusyBox error, invokes the same packaged cache-preparation helper as the rebuild,
checks the repaired tables and verifies that a matching cache remains reusable.
Compressed archives are recompressed tar streams with unchanged upstream files;
the original archive hashes are recorded in `UPSTREAM-SHA256SUMS`.

Use a RAM workspace because FAT32 lacks source-tree symlinks and replacement
rename. Building requires substantial RAM and CPU time; RAM sources/artifacts
disappear on reboot. Copy finished executables to a writable mount to retain
them. QEMU defaults to 2 GiB for ordinary boot/run; source rebuilding
uses the larger explicit VM memory setting above.

To preserve the rebuilt toolchain on an existing writable FAT mount, archive
the installation rather than the source/build trees. For the default prefix:

```text
build-sh -c 'tar -czf /mnt/nvme1n1p1/native-toolchain.tar.gz -C /tmp/toolchain/install .'
sync
```

After reboot, restore it into the same RAM prefix, then use the explicit driver
or put that `bin` directory first in the build shell's `PATH`:

```text
build-sh -c 'mkdir -p /tmp/toolchain/install; tar -xzf /mnt/nvme1n1p1/native-toolchain.tar.gz -C /tmp/toolchain/install; /tmp/toolchain/install/bin/gcc --version'
```

Substitute the actual writable mount. The archive depends on ModernOS's bundled
musl headers/libraries and preserves the configured assembler/linker prefix;
it is not a standalone replacement for a different OS's toolchain.

The long integration test `tools/test_toolchain_rebuild.py` freezes its ISO and
saves the real QEMU RAM/device/process state at build stages, every 15 minutes,
and on completion/failure. It uses structured local file migration, requires
QEMU 8.2 or newer, and refuses incompatible ISO, firmware, CPU/memory or QEMU
configurations. Disk fixtures are excluded because their state is not captured.
For example, recover a previously saved 8-CPU build with:

```text
python3 -u tools/test_toolchain_rebuild.py --cpus 8 --memory 8G --resume build/runtime-tests/native-rebuild-gawk.state
```

`--restart` reruns the checked native rebuild script after a failed build;
resuming a build still running needs no restart. Checkpoint files are local test
artifacts; this does not provide production filesystem persistence. Actual
source files, generators and rebuilt binaries still execute entirely in ModernOS.

`make test-rebuilt-toolchain REBUILD_CHECKPOINT=build/runtime-tests/native-rebuild-gawk.state`
restores a completed checkpoint, compiles/runs the examples and compiler ABI
probe with explicit rebuilt-driver paths, and verifies C++17 exceptions/containers
and C++20 `span`. Runtime archive paths must resolve inside the rebuilt prefix.
It checks static ELF permissions, creates the SDK archive in
the guest, then exports the exact RAMFS bytes through QEMU's read-only memory
monitor. The host SHA-256 must equal the guest digest. It retains
`build/native-toolchain.tar.gz`, its digest/manifest, the real build log and
compiled proof binaries. This optional diagnostic/export test needs GDB and
the exact checkpoint boot kernel's debug information; it neither patches guest
memory nor replaces guest filesystem operations with host execution.

The completed 8-CPU/8-GiB UEFI run rebuilt binutils, GCC/G++, libgcc and libstdc++
entirely inside ModernOS, in approximately 4.5 hours under TCG. The exported
build log contains both `REBUILT_GCC_RUN_OK` and `REBUILT_GXX_RUN_OK`.
Restored-guest validation then compiled and executed C, C++17 exceptions and
containers, and C++20 `span` with the new drivers and their own rebuilt runtime
archives. All five proof executables passed the static ELF checks.
The verified export is 65,494,016 bytes, with SHA-256
`2acc2f572a4293bb42bb21dd0fd77c19e21205ca62ba8bd7c73db4b95a20edac`.
This digest identifies that recorded archive; a later rebuild/export can have
different archive timestamps. Its machine configuration, compiler banner,
runtime paths and digest are recorded in `build/native-toolchain.tar.gz.json`.
Validation records are `build/native-rebuild-verified.log`,
`build/rebuilt-toolchain-export-marked.log` and
`build/native-rebuilt-proofs/rebuild.log`.
The rebuild/export harness associates shell output with unique completion
markers and checks the actual exit status, so a saved or delayed console prompt
cannot be mistaken for a completed command.

Private file-backed mmap now snapshots readable regular files, preserves
file offsets and survives descriptor closure/unlink. Partial EOF pages are
zero-filled; whole pages beyond EOF are rejected because mappings do not retain
the backing-file metadata needed to fault those pages with SIGBUS.
MAP_SHARED/coherent file mappings remain unsupported. CPU clocks, times,
getrusage and wait4 expose actual sampled user/kernel CPU time; untracked
resource fields remain zero. IPv4 sockets, hardware getrandom, random devices
and eventfd support Git/curl/NetSurf; see [browser-network.md](browser-network.md).

Remaining ABI gaps include dynamic linking, shared mmap, mknod/FIFOs,
hard links, unlinking open files, replacing occupied FAT targets,
controlling sessions/process groups, suspension, pthreads/futexes,
editable signal ucontexts, IPv6, Unix sockets and unsupported network drivers. Use `build-sh SCRIPT`
for the explicit build environment; kernel execution supports static ELF and
bounded shebang scripts.
Writable ext filesystems and FAT power-loss transactions are also absent.
No general Linux/POSIX compatibility is claimed.

## Integrated source map

| Sources | Responsibility |
| --- | --- |
| `tools/build_native_ports.py`, `tools/build_gawk.py`, `userland/vendor/busybox-modernos.config`, bundled archives/licenses | Verified upstream builds, target configuration and explicit source patches |
| `tools/build_gcc.py`, `tools/package_sdk.py`, `userland/sdk/`, `sdk-tool.c` | Source-built static compiler/binutils/SDK, verified source bundle, native rebuild and correct-prefix launch aliases |
| `userland/build-sh.c`, `applet.c`, `runtime/usr/share/vim/vimrc`, `sh.c`, `env.c` | Build environment, real multicall execution, editor defaults, help and checked limits |
| `userland/Makefile`, root `Makefile`, `tools/mkinitramfs.py` | Build registration, executable aliases, runtime/notice packaging and ISO |
| `kernel/sched/process.c`, `fs/binfmt_elf.c`, `mm/user.c`, `include/mm/user.h` | Private fork, vfork lifetime, inherited descriptors/env/TLS, eager mapping cloning and guard preservation |
| `kernel/sched/sched.c`, `include/sched.h`, `kernel/sched/syscall.c`, `include/uapi/abi.h` | Process/timer dispatch, real SIGCHLD/SIGALRM, checked syscall layouts, lazy kernel VM/FPU context |
| `kernel/sched/signals.c`, `arch/x86/kernel/interrupts/interrupt.c`, `arch/x86/kernel/interrupts/interrupt_helper.s`, `mm/user.c` | Checked signal frames, asynchronous IRET return, atomic signal waits, real user fault delivery and page-fault classification |
| `fs/file_time.c`, `include/fs/file_time.h`, `fs/ramfs.c`, `fs/fat32.c`, `fs/vfs.c`, `include/fs/vfs.h` | Actual timestamps, resize/zero fill, RAM symlinks/replacement, device streams and filesystem capability boundaries |
| `userland/build-check.c`, `compiler-check.c`, `check.c`, `tools/test_native_ports.py`, `tools/test_compiler.py`, `tools/test_gcc_generators.py`, `tools/test_console_input.py` | Process/FPU/timer/device/file/large-input checks and booted native-tool/compiler/input/generator integration tests |
| `tools/test_toolchain_rebuild.py`, `tools/test_rebuilt_toolchain.py`, `tools/guest_ramfs.py`, `tools/test_runtime.py` | Native source-build verification, checked command completion, guest checkpoints and read-only export of actual compiled artifacts |

Framebuffer/compositor changes are mapped separately in
[desktop.md](desktop.md#source-ownership). All implementations are integrated
into the normal source tree and build; the generated binaries/runtime are build
outputs, not substitutes for the bundled upstream sources.

## Validation

```sh
make test-ports
make test-compiler
make test-configure-shell
python3 tools/test_toolchain_rebuild.py --cpus 4 --memory 8G
python3 tools/test_native_ports.py --cpus 4 --storage --dark
```

The tests boot the real ISO, exercise process/guard/signal/timer/device/file
interfaces, shell pipelines and regex tools, patching, compression, parallel,
recursive and incremental make recipes, and default Vim console/GUI editing. They verify undo,
bundled help, shell filters, saved bytes, resize and close cancellation. Storage
checks export exact FAT documents, reboot, verify the read-only source hash and
run `fsck.fat -n`. Existing runtime, desktop, nano and dark-mode checks remain.
The configure-shell regression preserves live RCX/R11 across timer-delivered
handlers, then runs the unmodified binutils Autoconf shell probe sequentially
and in four parallel subprocesses, checking every child status. The source
rebuild test runs the process/signal and compiler ABI checks before starting
the build, so register corruption cannot silently invalidate feature detection.
Its signal-wait checks exercise `rt_sigsuspend`, `pause`, `ppoll` and `pselect`,
including pending and delayed delivery, masks during and after handlers,
ignored timer signals and invalid arguments. These waits follow the
[Linux signal-suspension contract](https://man7.org/linux/man-pages/man2/sigsuspend.2.html)
used by the bundled musl and ash ports.
The same test compiles/runs the unmodified binutils `intl/configure`
divide-by-zero probe. Hardware fault checks validate real SIGFPE/SIGILL
handlers and siginfo, retry a protected write after the SIGSEGV handler fixes
its mapping, and ensure a blocked synchronous fault terminates. The full rebuild
keeps intentional configure fault diagnostics in its log and judges completion
by the build status and execution of both rebuilt-compiler proofs.
