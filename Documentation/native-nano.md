# Native GNU nano and terminal runtime

`nano` is the real upstream GNU nano 8.7 executable, compiled statically with
unmodified musl 1.2.5 and ncurses 6.5 sources. It runs in ring 3 on ModernOS,
uses the OS's VFS and terminal syscalls, and edits actual files. No host process,
Linux VM inside the OS, fake editor screen or simulated save is involved.

## Use

```text
nano /tmp/notes.txt
nano /mnt/nvme1n1p1/notes.txt
nano --help
env TERM=vt100 nano /tmp/notes.txt
```

Ctrl+O writes the document; Enter confirms the path. Ctrl+X exits; Ctrl+W
searches. Search/replace, selection, cut/paste, undo/redo and multiple buffers
use nano's upstream implementation. RAM files disappear on reboot. Save to
an actual writable FAT32 mount for persistent documents; `mounts`/`lsblk`
identify the available locations. Read-only and failed saves report the
underlying error. Nano's own save/backup behavior applies, separately from the
native `edit` and GUI Text Editor save implementations.

On the desktop, open Terminal from the taskbar or launcher and enter the same
command. Terminal starts the real `/bin/modernos-sh --terminal` over a master/slave PTY.
When nano disables canonical input, the window displays its actual VT output
and forwards ordinary text, Ctrl/Alt keys, arrows and function keys. Window
resizing changes the slave's real rows/columns and delivers SIGWINCH. A close
attempt while a terminal program is running asks for confirmation; Cancel
leaves the process running. Save work before confirming termination.

The console also supports the cursor movement, erase, reverse attributes and
scroll regions needed by nano. Console settings survive ordinary commands,
so `stty` has a real effect. Exiting/faulting a display owner restores normal
console input automatically.

| Command | Behavior |
| --- | --- |
| `env` | Print the actual initial environment |
| `env NAME=VALUE PROGRAM ...` | Run with real environment changes |
| `env -i NAME=VALUE PROGRAM ...` | Start with an explicitly empty environment |
| `stty -a`, `stty size` | Inspect actual line discipline or rows/columns |
| `stty raw`, `stty cooked`, `stty echo`, `stty -echo` | Change implemented terminal settings |
| `tty` | Identify the console or descriptor-based PTY; no fabricated `/dev/pts` name |
| `reset` | Restore canonical/echo/signal input and clear terminal state |
| `posix-check` | Exercise the real static-musl/runtime/syscall interfaces |

## Offline build and sources

Run `make iso`. The normal userland build invokes `tools/build_nano.py`, which
verifies the bundled archives against pinned SHA-256 hashes, builds musl and
ncurses, and links the upstream nano executable. A stamp caches successful
builds and is invalidated when the script, source hashes or build root change.
Build logs are under `build/vendor/`; output is `userland/bin/nano`, included
as `/bin/nano` in `build/os.iso`.

This requires a host x86-64 GCC/binutils, make, Python 3.12+, and ncurses host
tools `tic`/`infocmp`. Those host tools build the fallback terminal descriptions;
the OS does not need installed terminfo files. Source archives are already
bundled, so the native program build needs no network access. The first Limine
bootloader build may still download its existing pinned release.

Original complete sources and notices:

| Package | Bundled archive | Upstream source |
| --- | --- | --- |
| GNU nano 8.7 | `userland/vendor/nano-8.7.tar.xz` | [GNU nano source](https://www.nano-editor.org/dist/v8/nano-8.7.tar.xz) |
| musl 1.2.5 | `userland/vendor/musl-1.2.5.tar.gz` | [musl source](https://www.musl-libc.org/releases/musl-1.2.5.tar.gz) |
| ncurses 6.5 | `userland/vendor/ncurses-6.5.tar.gz` | [ncurses source](https://invisible-island.net/archives/ncurses/ncurses-6.5.tar.gz) |

GNU nano is GPLv3 or later; musl and ncurses have permissive licenses. Original
license files are kept in the archives and `userland/vendor/licenses/`, and
installed under `/usr/share/licenses/native-programs` in the boot image. The
bundled archives provide the corresponding source for the shipped executable.

The build disables Linux virtual-console extensions, libmagic, translations,
spell checking and nanorc configuration. Upstream's configuration switch also
disables syntax colors, formatter and linter integration. `nano --version`
reports these actual compiled options. UTF-8 file handling and the full editor
remain enabled. Embedded VT100 and xterm descriptions avoid nonexistent host
services; TERM defaults to vt100.

After this build, small additional native ports can use:

```sh
build/native-musl/bin/musl-gcc -O2 -static -no-pie \
  -fno-stack-protector -mno-avx -mno-avx2 program.c -o userland/bin/program
make iso
```

Compile static ET_EXEC images and validate every OS dependency. Nano's success
does not imply that arbitrary Linux binaries, glibc or threaded programs work.
See `kernel-userland.md` and `include/uapi/abi.h` for the actual ABI.

## Integrated source files

| Files | Responsibility |
| --- | --- |
| `include/uapi/abi.h`, `kernel/sched/syscall.c` | Shared syscall numbers/layouts, checked argument dispatch, polling, environment spawning and terminal interfaces |
| `kernel/sched/signals.c`, `kernel/sched/sched.c`, `include/sched.h` | Pending signals, protected saved contexts, interruptible waits, foreground ownership and cleanup |
| `fs/binfmt_elf.c`, `mm/user.c` | Initial argv/envp/auxv, validated static TLS, reserved guard pages and private mapping replacement |
| `drivers/tty/pty.c`, `include/pty.h`, `fs/vfs.c`, `include/fs/vfs.h` | Real PTY endpoints, line discipline, descriptor lifetime, I/O and readiness |
| `drivers/tty/tty.c`, `drivers/video/fbdev/core/fbcon.c`, `drivers/video/fbdev/core/display.c` | Console configuration, ANSI rendering and display-owner input restoration |
| `userland/desktop/terminal.c`, `terminal_screen.[ch]` | GUI PTY connection, actual VT screen, resize/key forwarding and running-program close confirmation |
| `userland/env.c`, `stty.c`, `tty.c`, `reset.c`, `sh.c`, `libc.c`, `crt0.S` | Native commands, command help, error reporting and initial environment access |
| `tools/build_nano.py`, `userland/Makefile`, root `Makefile`, `tools/mkinitramfs.py` | Verified offline upstream build, linking, application packaging and notices |
| `userland/posix-check.c`, `tools/test_nano.py` | Static-musl ABI checks and real console/GUI/storage integration tests |

Kernel companion paths are relative to the repository root. Userland companion
paths use the first file's directory. Desktop rendering and appearance
ownership are listed in `Documentation/desktop.md`.

## Implemented runtime and limits

The new Linux-numbered interfaces include signal actions/masks/return/pending,
poll/ppoll/select/pselect6, lstat/readlink/readlinkat, fchdir, umask,
ftruncate, getrlimit, gettimeofday/time/clock_getres and tkill/tgkill. The loader
supports bounded argv/envp and validated static TLS. PROT_NONE guard mappings,
MAP_FIXED replacement and real clear-TID storage are supported while retaining
W^X and process isolation. Signals restore kernel-owned register/FPU contexts;
they do not trust modified user signal frames.

ModernOS-specific `spawnve` (405) supplies an environment, and `pty` (406)
returns a master/slave descriptor pair. PTYs have 16-KiB input/output buffers,
canonical/raw input, echo, interrupt keys, nonblocking operation, poll readiness,
EOF/HUP, input/output flushing and actual window sizing. Overlong canonical
lines are discarded completely and report E2BIG to the reader; raw input uses
the bounded ring. The `OS_PTY_GET_FOREGROUND` ioctl returns the actual process
that owns input, rather than pretending Linux process groups exist.

PTY settings currently support ICANON/ECHO/ISIG/NOFLSH, ICRNL, OPOST/ONLCR,
VMIN 0/1 and VTIME 0. Unsupported flags/timing return ENOTSUP. Baud settings on
a virtual terminal do not configure physical hardware. TCSETSW is immediate;
there is no hardware output drain or flow-control protocol. The console supports
a narrower settings subset documented in `kernel-userland.md`.

Process fork/vfork and SIGCHLD now work; see [native-tools.md](native-tools.md)
for their semantics, signal timers, file resizing and utility ports.
Dynamic linking, pthreads/futexes, public PTY device paths, controlling
terminal/session APIs, job control, signal stacks, editable signal ucontexts,
automatic SIGPIPE and shared-address-space threads remain unavailable.
Hardware random devices and IPv4 sockets are now implemented; see
[browser-network.md](browser-network.md). For nano's external commands use
`env SHELL=/bin/ash nano FILE`, selecting the real POSIX shell.
Avoid its suspension command: the upstream prompt
mentions `fg`, but ModernOS cannot suspend/resume jobs and provides no `fg`.
The kernel rejects these operations. PTY/VT rendering is bounded to 240 columns × 100
rows. GUI glyphs cover Latin-1 with fallback for other characters; UTF-8 file
bytes remain intact. Keyboard input follows the existing US ASCII mapping.

## Validation

```sh
make test-native
python3 tools/test_nano.py --cpus 4 --storage
posix-check # at the ModernOS shell
```

These boot the real ISO under QEMU/OVMF. Nano tests type, save, exit and verify
actual documents on the console and in Terminal; resize the running editor;
cancel close; check read-only errors; compare exact FAT32 bytes; reboot to check
persistence; hash the read-only ext source; and run `fsck.fat -n` on the copied
test volume. Disk fixtures are generated under `build/runtime-tests/`; host
block devices are never used.

The separately linked static-musl check verifies initialized TLS, allocation,
memory guards, signal delivery/masks/pending/return, interruptible nanosleep,
SA_RESTART on a blocking pipe read, poll/select, malformed syscall arguments,
umask, truncation, PTY I/O/resize/limits/EOF and safe overlong-line handling.
Other existing kernel, storage and desktop regression checks remain in
`tools/test_runtime.py` and `tools/test_desktop.py`.
