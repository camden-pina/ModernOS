# Kernel, storage and userland

Coast has a monolithic x86-64 kernel, native userspace programs and a bounded
Linux-compatible application ABI. Source is published at
https://github.com/camden-pina/ModernOS.git.

## Build and boot

```sh
make setup-host
make menuconfig
make iso
make run
```

Coast combines the kernel, native desktop/account tools, Chromium, Code OSS
and the package/development environment. `make -j4` builds only the
kernel at `build/vmlinux`. See [source builds](source-builds.md) and
[the build system](build-system.md) for output roots, source components and
cross/native builds.

Limine provides BIOS and x86-64 UEFI boot. Boot adapters provide the kernel with
memory, framebuffer, ACPI and module information. SMP, interrupts, timers,
physical memory, virtual memory, process scheduling and drivers initialize
before the interactive console/desktop session.

## Processes and executable ABI

The loader accepts supported little-endian x86-64 ELF64 static executables,
PIE and bounded dynamic interpreter/shared-library layouts. It validates
headers, mappings, executable stack requests, interpreter paths and memory
permissions. Writable/executable load pages are rejected. The native musl C/C++
profile and the source-built GNU ABI profile serve different ported programs;
Electron/Node admission is tied to the exact built runtime pair.

Implemented Linux-numbered operations cover the tested file, process, memory,
thread, signal, time, pipe, socket, polling and synchronization workflows.
Unsupported calls return an error. Threads use TLS, futexes and the process
signal/exit model. The runtime profiles add the narrowly implemented
namespace, credential, seccomp, memory and process contracts required by the
ported applications. Those profiles do not establish general Linux binary
compatibility. See [executable compatibility](binary-compatibility.md),
[dynamic linking](dynamic-linking.md), [shared C++](dynamic-cpp.md),
[Linux tools](linux-command-tools.md) and [accounts](accounts.md).

Kernel interfaces and limits are implemented in `kernel/sched/`, `mm/`,
`fs/binfmt_elf.c` and the public headers under `include/uapi/`.

## Credentials and application authority

The console and Terminal use ordinary account credentials. Profiles have
private homes, UID/GID values, administrator/standard roles and authenticated
sudo. Kernel filesystem permission checks enforce ownership, mode bits,
sticky directories and set-ID execution. `no_new_privs` and the runtime's
syscall sandbox constrain child processes.

The protected native desktop is a trusted UI broker. File/window/clipboard
operations authorize the active profile and the appropriate native window.
Applications receive bounded window, input and clipboard capabilities rather
than control over peer applications. Chromium/Code OSS run in the owner
profile with their specific process namespace and renderer restrictions.

## Filesystems and storage

The live root is RAM-backed. Initramfs modules supply native programs, ported
runtimes, configuration, data and notices. Save persistent work to a supported
writable mount before stopping the VM.

| Facility | Supported scope |
| --- | --- |
| RAM filesystem | Live files, directories, ownership, permissions and the tested file/mapping operations |
| FAT32 | Supported writable mounted storage |
| ext2/ext4 | Supported read-only access |
| NVMe / SATA AHCI | Implemented block-device discovery and read/write/flush paths |
| VFS | File/directory descriptors, metadata, directory enumeration, mounts and permission checks |
| IPC | Pipes, Unix sockets, eventfd, epoll, memfd and the implemented shared-memory contracts |
| Notifications/locks | The inotify and record-lock operations required by the tested ports |

Unsupported operations fail explicitly; an implemented syscall number alone
is not evidence of complete Linux semantics. Disks reports the real inventory
without offering erase, formatting or partition-edit operations.

## Networking, audio, power and input

The network stack provides the supported Ethernet device paths, IPv4,
DHCP/DNS, TCP/UDP and loopback sockets. Native curl/Git and Chromium exercise
real HTTP/TLS networking; QEMU user networking and passt supply host-side VM
connectivity. The guest network test checks exact transfer data, certificate
trust/hostname rejection and Git clone/fetch/push.

ACPICA provides ACPI runtime services, interrupts/events and the implemented
power/device consumers. Shutdown/reset are coordinated with the desktop;
see [ACPICA](acpica.md) and [power](system-power.md). Hardware-specific audio
and device support is bounded by the corresponding drivers and validation.
USB input and the framebuffer/window service support the native desktop,
Chromium, Code OSS, Qt and the nested VM viewer.

## Native development and packages

The full image includes GCC/G++, binutils, GNU Make, Bash/Readline, pacman,
GnuPG, Python/SQLite, Git, makepkg/fakeroot support, Qt Widgets and QEMU.
A local package ownership database describes the assembled files. Signed
package installation requires an explicitly configured repository identity;
see [Brine](local-repositories.md).

The `coast-build` and `qemu-build` packages provide additional offline native
build kits from a configured repository. The native integration workflows
compile Coast's kernel/desktop/ISO inside Coast and boot the rebuilt ISO under
source-built QEMU. See [QEMU](qemu.md) and [native tools](native-tools.md).

Guest tests operate disposable QEMU instances and record their ISO hashes,
checks and owned-VM cleanup. Host tests establish their specific fixture
behavior; they do not establish real-hardware or arbitrary-application support.
