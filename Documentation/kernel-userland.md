# Kernel, storage, and userland

## Build and boot

From the repository root:

```sh
make iso
make run SMP=4
```

The output is `build/os.iso`. It contains the kernel, the Limine bootloader,
and an initramfs with `/bin/modernos-sh`, POSIX `/bin/sh`, `edit`, `gui`, upstream `nano`, `env`, `stty`,
`tty`, `reset`, `head`, `tail`, `wc`, `grep`, `find`, `hexdump`, and the
`hello`, `check`, `posix-check`, `spin`, `fault`, and `exec-test`
diagnostics. The ISO supports BIOS and UEFI. Limine's MP response starts
the available application processors; there is no separate kernel SMP switch.
`SMP=1` selects one virtual CPU in QEMU. The default is four.

Build dependencies are GNU make, GCC and binutils targeting x86-64, a host
x86-64 GCC, Python 3.12+, tar, curl, xorriso, and host ncurses tools `tic` and
`infocmp`. The Makefile selects `x86_64-elf-gcc` when installed
and otherwise uses the host GNU toolchain. Limine binary release 12.9.0 is
cached under `build/`; its first download needs network access. `make run`
uses QEMU's q35 machine and `firmware/ovmf/OVMF.fd`; override `OVMF` to use
another compatible firmware image. Serial input/output and USB keyboards
feed the same terminal. The framebuffer also displays console output.
`QEMU_EXTRA_ARGS` can supply additional emulator options for debugging.
The musl, ncurses and nano source archives are pinned and bundled under
`userland/vendor`; their normal builds do not download dependencies. See
`native-nano.md` for the build profile and source/license locations.
The native GCC/G++/binutils SDK and its source bundle are also built offline;
see `native-tools.md`. Ordinary QEMU runs default to 2 GiB; use `MEMORY=8G`
for a full native compiler source rebuild.

Attach disk images on independent NVMe controllers and a SATA port:

```sh
make run SMP=4 LINUX_DISK=/absolute/path/source.img FAT_DISK=/absolute/path/destination.img
make run SMP=4 SATA_DISK=/absolute/path/sata.img
```

These variables take raw disk images, either partitioned or containing a
filesystem directly. The destination image is writable. The integration
tests create their own images and never use host block devices.

## Files and commands

The root filesystem, `/bin`, and `/tmp` live in RAM. Their changes disappear
on reboot. Detected supported filesystems mount automatically under `/mnt`:

| Example path | Device |
| --- | --- |
| `/mnt/nvme0n1p1` | First partition of the first NVMe namespace |
| `/mnt/nvme1n1p1` | First partition on another NVMe controller |
| `/mnt/sata0p1` | First partition on the first SATA disk |
| `/mnt/sata0` | A SATA disk containing a filesystem without a partition table |

Run `lsblk`, `disks`, and `mounts` to find the actual names. GPT headers and partition
arrays are checked with CRC32; MBR primary and extended partitions are also
recognized. Mount paths depend on discovery order.

A typical copy-and-run workflow is:

```text
mounts
ls /mnt/nvme0n1p1
mkdir /mnt/nvme1n1p1/programs
cp /mnt/nvme0n1p1/hello /mnt/nvme1n1p1/programs/hello
/mnt/nvme1n1p1/programs/hello argument
cp /mnt/nvme0n1p1/hello /tmp/hello
run /tmp/hello another-argument
mv /mnt/nvme1n1p1/programs/hello /mnt/nvme1n1p1/programs/demo.elf
sync
```

`cp` preserves executable permissions. Paths containing spaces accept single
or double quotes. A trailing `&` starts an external program in the background.
Ctrl-C terminates the foreground child; the system shell stays available.
Backspace edits the current input line. Background processes can write output
but cannot read the foreground terminal.

| Commands | Purpose |
| --- | --- |
| `help [COMMAND\|CATEGORY]`, `COMMAND --help` | Categorized help, syntax, options, examples, and limits |
| `pwd`, `cd [PATH]`, `ls [-a] [-l] [PATH...]` | Navigation; detailed and hidden-file listings |
| `cat PATH...`, `stat PATH` | Inspect file contents and metadata |
| `cp SOURCE DEST`, `mv SOURCE DEST` | Copy or move a regular file |
| `mkdir [-p] PATH...`, `rmdir PATH...`, `rm [--] PATH...`, `touch PATH...` | Create and remove files or directories |
| `write PATH WORDS...`, `append PATH WORDS...`, `chmod OCTAL PATH` | Replace or append a line; change permissions |
| `edit [-r] FILE` | Create/edit a document or open a read-only view |
| `nano [OPTIONS] [FILE...]`, `vim [OPTIONS] [FILE...]` | Upstream editors on the console or desktop Terminal |
| `build-sh [SCRIPT\| -c COMMAND]`, `sh`, `ash`, `busybox`, `make` | Real POSIX scripts/pipes, upstream utilities, parallel build recipes; see [native-tools.md](native-tools.md) |
| `gcc`, `g++`, `cc`, `c++`, `cpp`, GNU binutils | Native static C/C++ compilation, preprocessing, assembly, linking, archives and ELF inspection |
| `bootstrap-toolchain`, `compiler-check` | Verified source rebuild workflow and compiler-facing ABI checks |
| `env [-i] [NAME=VALUE...] [PROGRAM [ARGS...]]` | Inspect the environment or launch with real environment changes |
| `stty [-a\|size\|raw\|cooked\|echo\|-echo]`, `tty`, `reset` | Inspect/configure terminal input, identify a terminal, restore normal input |
| `head [-n LINES] FILE`, `tail [-n LINES] FILE`, `wc FILE...` | Read portions of text; count lines, words, and bytes |
| `grep [-n] PATTERN FILE...`, `find [PATH]`, `hexdump FILE` | Literal text search, directory traversal, byte inspection |
| `run PROGRAM [ARGS...]`, `PROGRAM [ARGS...]` | Execute an ELF; bare names search `/bin` |
| `which PROGRAM...`, `jobs`, `history`, `echo`, `clear` | Program lookup and shell conveniences |
| `ps`, `cpus`, `uname [-a]`, `uptime` | Inspect tasks, CPUs, OS version, and elapsed boot time |
| `lsblk`, `disks`, `mounts`, `mount`, `umount DIRECTORY` | Inspect and manage storage mounts |
| `wait [PID]`, `kill [-SIGNAL] PID`, `sleep SECONDS`, `sync` | Wait, terminate, sleep, and flush storage |
| `gui [--info] [--stats] [--trace]` | Native desktop with Files, Text Editor, Terminal, Settings and workspaces |

`help` fits an overview on the screen. `help files`, `help text`, `help disks`,
`help system`, `help shell`, and `help programs` show command lists with
descriptions. `help COMMAND` and `COMMAND --help` show a full help page. upstream `--help` options pass through for the new native programs;
`help vim`/`help make` show the OS guide.
Incorrect arguments to a known command show its usage and example instead
of trying to execute a file with that command's name. Numeric parameters and
options are checked; unknown commands can suggest a nearby spelling.
Unfinished quotes/escapes and unsupported operators produce parse errors.
Overlong terminal lines are discarded completely rather than executing a
truncated command. The last 32 interactive lines are available through `history`.

Within a mount, `mv` can also rename directories. Across mounts it copies a
regular file and then removes the source. An ext source is read-only, so use
`cp` when copying from Linux. Existing destination names are not replaced by
rename. Open files and directories used as working directories cannot be
removed or renamed. The OS command console (`modernos-sh`) uses simple command
syntax. `sh`, `ash` and `build-sh` provide real POSIX scripts, pipelines,
redirection and wildcard expansion.

### Mount management

Automatic boot mounts remain available. A supported disk filesystem may be
unmounted and mounted at another empty RAM-root directory:

```text
lsblk
cd /
umount /mnt/nvme1n1p1
mkdir /mnt/documents
mount -t fat32 -o rw nvme1n1p1 /mnt/documents
edit /mnt/documents/notes.txt
sync
```

`mount` without arguments lists mounts. Syntax is
`mount [-t auto|fat32|ext|ext2|ext4] [-o ro|rw] DEVICE DIRECTORY`.
The `/dev/` prefix is optional for device identifiers; these identifiers do
not imply raw block-device files exist. A device can be mounted once; whole
disks containing discovered partitions cannot be mounted as overlapping
filesystems. The target must be an existing empty directory on the RAM root.
Read-only mounts reject all writes, and ext-family mounts always stay read-only.
`lsblk` shows byte capacity, logical sector size, filesystem mode, and mount path.

Unmount refuses open descriptors and working directories under the mount,
flushes metadata and the hardware write cache, then releases its context.
The RAM root cannot be unmounted. Forced/lazy unmount, bind mounts, remount,
disk formatting, and raw disk writes are not exposed.

### Documents

`edit FILE` creates a new document or opens an existing one. `edit -r FILE`
opens a read-only view. Existing read-only files automatically open that way.
The editor accepts ASCII documents up to 256 KiB and rejects binary contents.
It works on the framebuffer terminal and an ANSI-capable serial terminal;
USB arrows and navigation keys also work.

| Key | Action |
| --- | --- |
| Arrows, Home/End (also Ctrl-A/Ctrl-E), Page Up/Down | Move through the document |
| Enter, Tab, Backspace, Delete | Insert or remove text |
| Ctrl-F | Search literal text, wrapping once through the document |
| Ctrl-S | Save and flush |
| Ctrl-Q | Quit; a second Ctrl-Q confirms discarding unsaved edits |

Saving first writes and flushes `FILE.edit-save`, then moves the previous
document to `FILE.bak` and renames the staged document to the target.
The original is never truncated in place. An existing staging file is refused
to preserve recovery data. If a save fails, inspect the target, `.bak`, and
`.edit-save` with `cat`/`stat` before recovering them with `mv`. The editor
checks the original content before saving and refuses to overwrite a document
that changed since opening. This is not a transaction across concurrent
writers or power failures; FAT32 has no journal. RAM documents and their
backups disappear on reboot.

`head`/`tail` default to 10 lines and stream seekable files without loading
the whole file. `wc` counts newline bytes, whitespace-separated words, and
bytes. `grep` uses case-sensitive literal matching and accepts lines up to
64 KiB. `find` does not follow directory symlinks and caps traversal at
16 levels. These are native utilities, not GNU coreutils binaries.

### Native desktop

`gui` starts the shared desktop/window manager with Files focused on the real
Projects directory. It includes a cached atmospheric wallpaper, compact bottom
taskbar, searchable launcher, independently arranged workspaces, resizable
windows, actual file operations, a document editor, a native PTY-backed shell,
and functional Settings with shared light/dark appearance. No demo files or capacity values are substituted for
filesystem data. Super opens the launcher; Super+1/2/3 changes workspaces;
Alt+Tab switches windows; Ctrl+Alt+Escape returns to the console safely.

Files supports actual navigation, breadcrumbs/history, selection, sorting,
current-folder filename search, new/rename/copy/cut/paste/Trash/restore and
file metadata. Text documents open in Text Editor, with real staged saves,
read-only handling and unsaved-change confirmation. The console `edit` remains
available. RAM documents disappear on reboot; writable FAT32 mounts preserve
documents. Settings applies supported appearance, accent, wallpaper, scale and workspace
preferences and identifies whether saved configuration is on disk or in RAM.

Rendering uses cached decorated window surfaces/shadow masks and batched dirty
pixel rectangles. Dragging reuses unchanged contents and coalesces consecutive
motion reports without discarding button/key/wheel transitions. Focus changes
and popup closure damage their complete visible bounds.
Only the foreground task can acquire the display and read its input queue;
background programs cannot draw or steal input. Bounds, sizes, operations,
and user pointers are checked before rendering. Hardware framebuffer/MMIO
addresses and kernel memory are never mapped into userspace. A single owner
serializes presentation with the framebuffer console. Normal exit, a user
fault, image replacement, or a foreground handoff releases the display.
Returning to the shell resets canonical input and echo.

The shared components and native applications live in `userland/desktop/`.
Browser uses WebCore/JavaScriptCore and CPU Skia through a restricted Soup
broker and immutable-origin renderer. Local file associations retain NetSurf.
Bounded pixel/input channels connect both engines to the native desktop; see
[browser-network.md](browser-network.md). Wi-Fi remains unavailable. Native PCM
audio supports AC'97 and analog HDA, including an AW88399 speaker amplifier
driver for the identified Arrow Lake Lenovo PC; see
[browser-media-scroll.md](browser-media-scroll.md). Terminal has a real PTY and
bounded VT/ANSI screen; GNU nano and Vim work there and on the boot console.
Native fork/vfork, scripts and shell filters work; job control remains unsupported. Capacity/free-space reporting,
a general external GUI-client protocol, full Unicode shaping, accessibility, multiple user
sessions and a production desktop security model remain dependencies.
See [desktop.md](desktop.md) for source ownership, shortcuts, operation
semantics, limits, rendering and desktop validation.

## Filesystem and device support

* NVMe: PCIe controllers, active namespaces, NVM read/write/flush commands,
  DMA queues, PRP lists, and 512–4096-byte logical sectors. Transfers use
  serialized polling queues with timeouts. There is no hotplug, reset recovery,
  or interrupt-driven I/O yet.
* AHCI: SATA disks using LBA48 DMA reads/writes and cache flushes. ATAPI optical
  drives and legacy IDE controllers are not supported. PCI discovery requires
  an ACPI MCFG table exposing ECAM.
* ext2/ext4: read-only, clean filesystems with 1 KiB, 2 KiB, or 4 KiB blocks.
  Supports legacy indirect block maps, extent trees, sparse files, ordinary
  directories, and symlinks. Metadata checksums are checked on the superblock,
  group descriptors, inodes, and external extent nodes when enabled. Filesystems
  needing journal replay, bigalloc, encryption, inline data, and unsupported
  incompatible features are rejected. Unmount the Linux source cleanly before
  booting this kernel.
* FAT32: writable files, directories, truncation, rename, removal, mirrored FATs,
  and ASCII long filenames up to 255 characters. Non-ASCII long filenames fall
  back to their short alias. The FAT read-only attribute supplies permission
  emulation; Unix ownership is unavailable. Actual FAT timestamps are preserved
  with two-second modification and day-granularity access precision; Unix inode
  change time is unavailable. FAT12, FAT16, and
  exFAT are not supported. FAT32's per-file size limit is 4 GiB minus one byte.
  FAT32 has no journal; mutations are not atomic across a power failure.
* RAM filesystem: writable root and temporary storage, including initramfs
  executables, real symlinks and replacement of closed rename targets.
  Persistent destinations must be on a writable disk mount. The VFS provides
  actual /dev/null and /dev/zero character devices independently of terminals.

## Build a program

Inside ModernOS, the included GCC/G++ SDK can compile your own source directly:

```text
gcc -O2 -Wall /tmp/hello.c -o /tmp/hello
/tmp/hello
g++ -std=c++17 /tmp/hello.cpp -o /tmp/hello-cxx
build-sh -c 'make -j2'
```

The drivers default to static non-PIE executables. `/usr/include`, `/usr/lib`
and GCC's versioned include/runtime directories contain actual development
files, with libc, libgcc and libstdc++. Source examples and verified archives
live under `/usr/src/modernos`. See `native-tools.md` for rebuilding the compiler.
The following minimal-library workflow remains available for kernel-ABI tools
built on the host.

Programs must be static, little-endian x86-64 `ET_EXEC` ELFs using the supported
ABI. Dynamic linking, PIE, executable stacks, overlapping load pages,
and segments that are both writable and executable are rejected. A Linux
executable is usable only if its startup and syscall requirements fit this
subset; a general static glibc executable needs additional compatibility work.
Validated static `PT_TLS` templates are supported and initialized by upstream
musl from the ELF auxiliary vector. The shipped nano and `posix-check` use
static musl. Musl pthread clone/TLS, shared thread-group context and private-VM
futexes now support the separately ported JavaScriptCore component. Thread
groups stay on one CPU; this is a tested subset, not full POSIX threading.
See [modern-browser-port.md](modern-browser-port.md).

Use the included startup code, minimal C library, and linker script. For example,
create `userland/demo.c`:

```c
#include <libc.h>
int main(int argc, char **argv) {
    printf("demo pid=%ld argc=%d\n", call0(SYS_getpid), argc);
    if (argc > 1) puts(argv[1]);
    return 0;
}
```

Then run:

```sh
make -C userland bin/demo
make iso
```

All files in `userland/bin` are included in the initramfs, so `demo example`
runs the program. Add its name to `PROGRAMS` in `userland/Makefile` to retain
it in normal builds after cleaning. Alternatively, copy `userland/bin/demo`
onto the Linux source filesystem, mark it executable there, and use `cp`
inside the OS. The shipped `hello` verifies ring 3, arguments, and zeroed BSS.

## Syscall ABI

Entry uses the x86-64 `syscall` instruction; ordinary syscall return uses
`sysretq`. Interrupts and the first process entry use `iretq`. A plain `ret`
does not change privilege levels. `int 0x80` is not the user syscall interface.

Put the syscall number in RAX and arguments in RDI, RSI, RDX, R10, R8, and R9.
RAX returns the result or a negative errno. RCX and R11 are clobbered. The
provided `syscall6` wrapper handles these registers. Numbers and data layouts
are defined in `include/uapi/abi.h`.

| Syscalls | Supported behavior |
| --- | --- |
| `read`, `write`, `open`, `close`, `stat`, `lstat`, `fstat`, `lseek` | Real files, pipes, `/dev/tty` and PTYs; special streams reject seeking; `O_NOFOLLOW` checks the final component |
| `pipe`, `pipe2` | Two real pipe endpoints; `O_CLOEXEC` and `O_NONBLOCK`; EOF after the last writer closes, `-EPIPE` without readers |
| `openat`, `newfstatat`, `mkdirat`, `unlinkat`, `renameat`, `faccessat` | Real directory descriptors or `AT_FDCWD`; absolute paths ignore dirfd. `newfstatat` supports `AT_SYMLINK_NOFOLLOW`, `unlinkat` supports `AT_REMOVEDIR` |
| `getdents64`, `getcwd`, `chdir`, `fchdir`, `readlink`, `readlinkat` | Directory enumeration, working directories and reading real symlink targets |
| `pread64`, `pwrite64`, `readv`, `writev` | Positional I/O preserves file offset; vectors are validated before I/O, limited to 16 vectors and 16 MiB total |
| `dup`, `dup2`, `dup3`, `fcntl` | Shared file descriptions and offsets; per-descriptor close-on-exec flags; `F_DUPFD`, `F_DUPFD_CLOEXEC`, `F_GETFD`, `F_SETFD`, `F_GETFL`, `F_SETFL` (append/nonblocking), bounded `F_GETPIPE_SZ`/`F_SETPIPE_SZ` |
| `ioctl` | `TCGETS`, `TCSETS/W/F`, `TIOCGWINSZ`, `FIONREAD`; PTYs also support `TIOCSWINSZ`, `TCFLSH` and `OS_PTY_GET_FOREGROUND`; console settings require foreground ownership |
| `poll`, `ppoll`, `select`, `pselect6` | Actual readiness, EOF/HUP/invalid-fd reporting, waits and temporary signal masks; up to 128 descriptors, including sets across the 64-bit boundary |
| `access`, `fchmod`, `fsync`, `fdatasync` | Mode checks, descriptor permission changes, and durable disk flushes |
| `mkdir`, `rmdir`, `unlink`, `rename`, `chmod`, `sync`, `umask`, `truncate`, `ftruncate` | Writable mount mutations; creation honors umask; RAM/FAT resizing preserves offsets and zero-fills exposed bytes; closed RAM rename targets can be replaced; FAT occupied targets are refused |
| `symlink`, `symlinkat` | Create real RAM links; FAT32 returns `-ENOTSUP` |
| `utime`, `utimes`, `futimesat`, `utimensat` | Update actual RAM/FAT timestamps, including UTIME_NOW/OMIT; FAT precision/range limits apply |
| `renameat2` | Flags zero or RENAME_NOREPLACE; exchange/whiteout are unsupported; native desktop refuses replacement |
| `getitimer`, `setitimer`, `alarm` | Real cancellable/periodic SIGALRM deadlines; ITIMER_REAL only; not inherited by fork |
| `mount`, `umount2` | Supported device filesystems, optional `MS_RDONLY`, no mount data; unmount flags must be zero |
| `mmap`, `mprotect`, `munmap`, `brk` | Private anonymous memory, heap and regular-file snapshots; partial EOF pages zero-fill, whole pages past EOF fail; descriptor offsets are preserved; `PROT_NONE`/`MAP_FIXED` work; memfd MAP_SHARED uses coherent reference-counted physical aliases across processes/CPUs; ordinary file coherence and shared anonymous maps are unsupported; W^X stays enforced |
| `memfd_create` (319) | Anonymous descriptor-backed memory, checked resize/I/O/stat, seal/shrink/grow/write seals, coherent mappings and final lifetime; 128-MiB objects, per-group/global backing quotas; mapped shrink returns EBUSY |
| `socketpair` (53) | Bounded AF_UNIX stream/datagram/seqpacket IPC and SCM_RIGHTS/CLOEXEC/peek/truncation; actual descriptors/lifetime; named sockets and cyclic queued references are unsupported |
| `epoll_create/create1`, `epoll_ctl`, `epoll_wait/pwait` | Level/one-shot readiness, 128 watches per object, deadlines and temporary signal masks; watches follow descriptions and final external close; edge/nested modes are unsupported |
| `spawn_web` (409), `web_identity` (410) | Fresh ELF helper launch from a live threaded broker; explicit descriptors/environment and immutable renderer origin/domain; default-deny roles, quota and cross-origin resource boundaries; native WebCore browser integrated |
| `memory_usage` (411) | Calling process's actual current/peak mapped bytes and mapped-page limit; used by WebCore memory pressure handling |
| `mremap` (25) | Bounded in-place grow/shrink of uniformly protected pages; new pages zero-fill; occupied growth fails with rollback; moving mappings/aliases/flags are unsupported |
| `futex` (202) | Private-VM WAIT/WAKE, WAIT_BITSET/WAKE_BITSET, REQUEUE/CMP_REQUEUE; atomic compare/enqueue, signal interruption, relative monotonic and absolute monotonic/realtime deadlines; process-shared aliases and PI are unsupported |
| `getpid`, `getppid`, `gettid`, `sched_yield`, `nanosleep`, `clock_gettime`, `clock_getres`, `gettimeofday`, `time` | Identity, scheduling, interruptible sleep, `CLOCK_MONOTONIC` (1), firmware-derived UTC `CLOCK_REALTIME` (0), and sampled process/thread CPU clocks (2/3); realtime is `-E_NOTSUP` without a boot epoch |
| `uname`, `getuid`, `geteuid`, `getgid`, `getegid` | OS identity; single-user UID/GID 0, no credential isolation yet |
| `arch_prctl`, `set_tid_address` | `ARCH_SET_FS`/`ARCH_GET_FS`; user GS is reserved; a validated non-null clear-TID pointer is zeroed and its futex woken on thread exit |
| `getrlimit`, read-only self `prlimit64` (302) | Actual stack/data/address-space/descriptor bounds and zero core limit; limit writes and foreign queries are unsupported and denied in browser roles |
| `getrusage`, `times`, optional `wait4` usage | Sampled user/kernel CPU time, actual peak RSS, accumulated reaped descendants and 100-Hz ticks; other untracked resource fields remain zero |
| `socket` through `getsockopt`, `accept4` | Real IPv4 TCP/UDP, nonblocking mode, socket names/options, send/receive, vector I/O, shutdown and poll/select; unsupported families/options fail |
| `getrandom` (318), `/dev/random`, `/dev/urandom` | Checked RDSEED/RDRAND entropy; bounded retries and honest unavailable/exhausted results |
| `eventfd` (284), `eventfd2` (290) | Shared 64-bit counters/semaphore mode, nonblocking mode, poll readiness, close-on-exec and checked overflow/lifetime |
| `rt_sigaction`, `rt_sigprocmask`, `rt_sigpending`, `rt_sigreturn`, `kill`, `tkill`, `tgkill` | Shared group dispositions, per-thread masks/pending state, safe return and TGID/TID-checked directed signals; `SIGWINCH` on PTY resize; no stop/job-control signals; PID 1 is protected |
| `rt_sigsuspend`, `pause` | Sleep until a caught or terminating signal; atomic temporary mask, original mask restored after the handler, `EINTR` even with `SA_RESTART`; ignored signals do not end the wait |
| `fork`, `vfork`, `clone` | Private fork or parent-blocking VM-borrowed vfork; single-thread musl posix_spawn; musl pthread shared VM/FS/FILES/SIGHAND/THREAD flags, TLS/TID registration and clear-TID wakeup; thread groups stay on one CPU; multi-threaded fork/vfork is rejected |
| `execve`, `exit`, `exit_group`, `wait4` | Bounded argv/envp image replacement, static ELF or checked absolute-interpreter shebang scripts (four levels); per-thread exit, group exit/fatal termination and child reaping after final live worker; `WNOHANG`; status reports exit code or signal; multi-threaded exec/spawn is rejected |
| `spawn` (400) | `(path, argv, flags)` creates a process; flags 0 foreground, 1 background; inherits cwd, environment and pipe/PTY standard streams when present, otherwise gets terminal descriptors |
| `sysinfo` (401) | `(kind, buffer, capacity)` returns text for CPUs, processes, disks, mounts, or lsblk; kinds 0–4 |
| `display` (402), `input` (403) | Versioned ModernOS display/input API; see below |
| `spawn_io` (404) | `(path, argv, os_spawn_io*)` creates a background child with validated explicit stdin/stdout/stderr and optional cwd, retaining the caller's foreground/display ownership |
| `spawnve` (405) | `(path, argv, envp, flags)` creates a process with actual supplied environment strings; used by `env` |
| `pty` (406) | `(int pair[2], flags)` allocates master/slave descriptors; flags `O_NONBLOCK`/`O_CLOEXEC`; real line discipline, I/O, readiness and geometry |

The terminal implements `ICANON`, `ECHO`, `ISIG`, `ICRNL`, configurable
interrupt/erase/EOF characters, and raw `VMIN` 0 or 1 with `VTIME` 0.
Unsupported line disciplines and input/output/local flags return `-E_NOTSUP`.
Raw reads preserve control bytes; `O_NONBLOCK` returns `-E_AGAIN` when empty.
`TCSETSW` is immediate because console writes complete synchronously.
Kernel `os_termios` is the 36-byte Linux x86-64 kernel layout, not a host
libc `struct termios`. Kernel pointers are not used as public RAM inode or
device numbers.

### Display and input ABI v3

ABI 3 retains the original display operations and 32-byte input event layout.
See [desktop.md](desktop.md) for generic/QEMU backend details and synchronization limits.

`display(operation, pointer, bytes)` takes one of these operations:

| Operation | Arguments and result |
| --- | --- |
| `DISPLAY_INFO` | `os_display_info*`, exact struct size; returns ABI version, width/height, format 1 (packed XRGB8888), and rectangle limit |
| `DISPLAY_ACQUIRE` | pointer 0, bytes 0; foreground-only, one owner; busy acquisition returns `-EBUSY` |
| `DISPLAY_PRESENT` | `os_display_rect*`, exact struct size; tightly packed `width*height` pixels; rectangles must fit the screen and contain at most 65,536 pixels |
| `DISPLAY_PRESENT_BATCH` | 2064-byte batch with 1–128 rectangles, or legacy 528-byte batch with 1–32; private strided pixels; full-frame damage allowed; checked before drawing |
| `DISPLAY_CAPS` | 16-byte `os_display_caps`; actual backend, buffer count, page-flip/WC/large-video-page flags; no fabricated vsync capability |
| `DISPLAY_PRESENT_SWAP` | Same batch shape, plus a validated complete readable canvas; uses hidden-page upload/viewport flip on supported QEMU VGA, generic copying otherwise |
| `DISPLAY_RELEASE` | pointer 0, bytes 0; owner-only; clears/restores the console |

`input(os_input_event* buffer, capacity)` returns 1 through `capacity` events
or `-EAGAIN`. Capacity is 1–64. The owner-only ring retains 255 events and
reports `INPUT_OVERFLOW` with a dropped count when full. `INPUT_KEY` carries
a text/control byte (ANSI sequences represent navigation keys).
`INPUT_HID_KEY` carries USB usage in `code`, ASCII in `dx`, USB modifier bits
in `dy`, and down/up state in `buttons`, preserving raw releases and shortcuts.
`INPUT_POINTER` carries relative X/Y, wheel, button state, and connection or
driver-overrun flags. It is a custom API, not Linux evdev; arbitrary keymaps,
per-client input queues, and pointer acceleration are not
provided. Pixel and event structures use fixed-width types in `uapi/abi.h`.
The native desktop presents only damaged regions from a strided output surface.
Batches require stride at least the screen width and at most 8192 pixels, with
total damage at most twice the screen area. The batch path uses bounded row
copies/conversion without per-tile allocations. There is no userspace hardware
framebuffer mapping. Page flipping is currently implemented for verified QEMU
standard VGA only; there is no native Intel/NVIDIA modesetting or vsync driver.

### Standard streams and pipes

`os_spawn_io` contains `int32_t fd[3]`, a zero `uint32_t reserved`, and a
`uint64_t cwd` user string pointer (zero inherits cwd); its size is 24 bytes.
Descriptors must exist and support the requested read/write direction. Cwd
must resolve to a real directory. Child standard streams reference the existing
file descriptions; unselected descriptors are not inherited. The persistent
GUI shell uses this interface, and its native child commands inherit streams
0/1/2. Exiting a process closes endpoints before it becomes a zombie.

Pipe capacity defaults to 16 KiB. `F_SETPIPE_SZ` rounds up to a power of two
between 4 and 256 KiB; both endpoints observe the same capacity. Resizing
preserves wrapped queued data and refuses a shrink below the occupied bytes.
`F_GETPIPE_SZ` reports the actual capacity. Trusted browser code sizes its two
pixel pipes to 256 KiB; sandboxed page renderers cannot change pipe capacity.
Writes up to 4096 bytes are atomic relative to other
writers; larger writes may be partial/interleaved. Empty nonblocking reads and
writes without sufficient room return `-EAGAIN`. Reads return zero after all
writers close and remaining bytes drain. A missing reader returns `-EPIPE`;
automatic SIGPIPE delivery is not implemented. Blocking waits release locks
and use the existing scheduler. Pipe descriptors reject directory enumeration,
seeking, positional I/O, filesystem flushes and permission changes.

### Porting existing programs

Syscall numbers follow the upstream Linux x86-64 table for the implemented
subset. This expands the useful ABI for statically linked ports but does not
make arbitrary Linux executables runnable. Start with a small command-line
program, compile its sources for this target, and adapt its OS/library calls
to the supported interfaces. An actual POSIX libc port still needs proper
runtime behavior for every interface it uses. The supplied upstream static
musl build provides `errno`, allocation, stdio and TLS; the actual GNU nano
port is validated on this ABI. Other upstream programs must be tested
individually. Vim, BusyBox/ash and GNU make are validated ports; see
[native-tools.md](native-tools.md). IPv4 sockets, getrandom and eventfd support
network ports; see [browser-network.md](browser-network.md). Dynamic linking,
named Unix/IPv6 sockets, general coherent file mappings, process groups and credential
isolation remain unsupported. Musl pthreads/private-VM futexes have specific
tested limits described in [modern-browser-port.md](modern-browser-port.md).
No general Linux or POSIX compatibility is claimed.

`wait4` accepts a child PID or -1 and an optional checked Linux-layout resource-usage
buffer. It reports the reaped child's sampled user/kernel CPU time and accumulated
reaped descendants; untracked resource fields remain zero. Exit codes are
shifted left eight bits; termination signals occupy the low seven bits.
Unknown syscalls return `-ENOSYS`. `spawn`, `spawnve` and `spawn_io` inherit
the selected standard streams described above, not arbitrary descriptors.
The minimal native library returns negative errors directly; upstream musl
translates them into its real per-thread `errno`.

Signal actions use the Linux x86-64 layout and an 8-byte kernel mask.
Supported flags include `SA_RESTORER`, `SA_SIGINFO`, `SA_RESTART`, `SA_NODEFER`
and `SA_RESETHAND`; alternate signal stacks are unsupported. SA_SIGINFO
provides the signal number and context; asynchronous sender metadata is not populated. The kernel saves
authoritative registers/FPU state and restores these on `rt_sigreturn`, so
forged user context cannot choose kernel addresses, selectors or FPU controls.
Signal return uses IRETQ: a timer can interrupt application instructions with
live RCX/R11 values, which SYSRETQ would replace with the return address and
flags. Ordinary syscalls retain the faster SYSRETQ path and its defined register
clobbers. Both paths validate user addresses and sanitize flags.
`rt_sigsuspend`, `ppoll` and `pselect6` keep their temporary mask active through
handler delivery, with the original mask in the saved signal context. This lets
upstream ash wait for asynchronous jobs without a missed SIGCHLD or a busy loop.
Editing the user ucontext or using siglongjmp to bypass signal return is not
supported. Nesting is bounded to four handlers. Restart applies to interrupted
read/write/wait4; poll/select and nanosleep return `EINTR`, with sleep/select
remaining time where appropriate. User hardware faults use the appropriate
SIGFPE/SIGILL/SIGSEGV/SIGBUS/SIGTRAP handler when registered and unmasked;
otherwise they terminate with that signal. Synchronous faults cannot be ignored
or blocked to resume the same failed instruction indefinitely. Divide-by-zero,
illegal-instruction and page faults include the actual fault address and cause
(FPE_INTDIV, ILL_ILLOPN, SEGV_MAPERR/SEGV_ACCERR); other trap causes use SI_KERNEL.
Kernel faults still panic. A handler can exit or repair a mapping and return to
retry the instruction; editing the saved context remains unsupported.
Registered SIGCHLD notification and real SIGALRM timers work; automatic
SIGPIPE, real-time signal queues and job control are not implemented. Default SIGCHLD/SIGCONT/SIGWINCH are ignored.

Each process has private user mappings and shared supervisor-only kernel
mappings. The loader supplies a 16-byte-aligned System V initial stack with
argc, argv, actual bounded envp strings and a small auxiliary vector. Defaults
are PATH=/bin, HOME=/home/user, TERM=vt100, LANG=C.UTF-8, SHELL=/bin/sh and
USER=user. `env -i` supplies an explicitly empty environment. Child spawning
inherits its parent's saved image environment; `spawnve` and `execve` accept
explicit replacements. The OS command shell does not expand/export variables;
upstream ash/build-sh provide real shell variables and script execution.
User accesses are checked before the kernel copies data. Executable images
are limited to 128 MiB; individual heap/mapping requests to 512 MiB. The user
stack is 8 MiB with an unmapped guard. Paths remain limited to 511 bytes.
The kernel accepts 4096 argv entries, 512 environment entries and 128
descriptors. Each argv/env string may contain at most 32767 bytes; each vector
has a separate 128-KiB aggregate string budget. Packed environments and initial
stack tables use checked allocations, avoiding large kernel-stack arrays.
Fork eagerly clones at most 1 GiB of user mappings; RAM files are at most
256 MiB. These limits are checked before replacing or creating an image.
The interactive OS console still limits each line to 511 bytes; put long
commands in a script for `build-sh`. `#!` dispatch validates absolute interpreter
paths, optional arguments and a four-level recursion bound, reporting genuine
exec errors. Private forks are assigned to online CPUs at creation; VM-borrowed
vfork children and shared-VM thread groups stay on their parent's CPU.

## Scheduling and validation

Every CPU gets its own GDT, TSS, fault stacks, GS state, syscall MSRs, local
APIC timer, idle task, and FIFO run queue. Kernel and user tasks are preempted
at a 1 ms timer interval. New tasks are balanced across online CPUs and remain
pinned for their lifetime. Scheduling and page allocation are synchronized;
user context switches preserve integer registers, FS base, x87, MMX and SSE.
Kernel workers borrow a reference-counted user address space when possible,
avoiding unnecessary CR3 reloads, and retain live FPU state until another user
process is selected. Isolated video-copy assembly preserves its SIMD registers.
AVX and user FSGSBASE are disabled. Shared-address-space threads stay on one
CPU; there are no cross-CPU TLB shootdowns, live migration or scheduler
priorities. Limits are 64 CPUs and 128 total task slots. Exited nonleader thread
slots are reclaimed after leaving their kernel stack; process leaders remain
waitable until the group has no live threads. Orphans are adopted by the system
shell. PID 1 remains the terminal shell.

Run the integration suite with:

```sh
make test
make test-desktop
python3 tools/test_desktop.py --cpus 4 --storage
python3 tools/test_runtime.py --bios --cpus 1 --no-storage
```

The suite first checks the `make run` command expansion with optional disks
and paths containing spaces, then boots UEFI VMs with one and four CPUs. It checks ring-3
execution, syscall validation, user fault recovery, memory operations, SIMD
state preservation, repeated spawn/wait cleanup, and preemption of a busy
program. It also exercises help/errors, text tools, editor save/backup/discard,
display drawing and crash/exit cleanup, background input denial, *at/vector
I/O, descriptor flags, pipe endpoint lifetime and explicit child streams,
FS base, and mprotect. Desktop screenshots are saved
as `build/runtime-tests/*-desktop.ppm`. Two NVMe drives and an AHCI source exercise GPT discovery, ext2/ext4
reads, symlinks, FAT32 copies, long filenames, sparse input files, and directory
mutations, read-only manual mounts, busy-unmount checks, remounting, and
persistent editor documents. After stopping QEMU, it compares copied bytes on the host, runs
`fsck.fat -n`, and checks that the ext4 source image is unchanged. Logs and
fixtures are under `build/runtime-tests/`.

The runner prints `TEST` progress lines between stages. A guest `PASS` line
is followed by QEMU shutdown, host-side persistence checks, and the next CPU
configuration. Host tools run noninteractively with a 60-second timeout;
unexpected prompts or failures cannot wait indefinitely. Ctrl-C cancels a
run, and the next `make test` recreates its disposable disk images.

Validation uses QEMU; physical hardware compatibility and crash recovery have
not been established. The existing ACPI AML host hooks for power management
remain incomplete. The scheduler, storage drivers, and ABI are a working
foundation with the limits above, rather than full Linux compatibility.

Protocol references: [Limine protocol](https://github.com/Limine-Bootloader/limine-protocol/blob/trunk/PROTOCOL.md),
[Linux x86-64 syscall table](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl),
[Linux terminal ABI](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/termbits.h),
[Linux ioctl constants](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/ioctls.h),
[ext4 layout](https://www.kernel.org/doc/html/latest/filesystems/ext4/index.html),
and [ACPI system tables](https://uefi.org/specs/ACPI/6.6/05_ACPI_Software_Programming_Model.html).
