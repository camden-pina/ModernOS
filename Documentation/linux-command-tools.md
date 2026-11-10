# Linux-numbered syscall profile for upstream command tools

`python3 tools/build_linux_compat.py --jobs 1` builds selected **unmodified upstream
application sources** with the packaged x86-64 Linux/musl static toolchain.
A source-only restoration first builds the project's existing compiler cache with
`python3 tools/build_gcc.py`; no host compiler installation is performed.
It preserves `__linux__`, uses official Linux UAPI headers and adds no Coast
application headers or application-specific source patches. This is a source
rebuild profile; it does not establish untouched distribution/glibc binaries.

The selected releases are GNU coreutils 9.7 (`cp`, `ln`, `stat`, `timeout`,
`mkfifo`, `mkdir`, `chmod`, `readlink`, `sleep`), GNU tar 1.35 and util-linux
2.41.1 (`flock`). They install below `/usr/lib/coast/linux-compat/` so the
established `/bin` and `/usr/bin` tools retain their existing identities.
GNU archives have recorded detached-signature verification; util-linux has
its official kernel.org archive checksum. Archives, signatures, notices,
Linux header hashes and license/exception texts are vendored. The builder
checks the archive and every extracted UAPI header before compilation.
`build/linux-compat/manifest-all.json` records exact ELF/source hashes and
flags. In-tree `semantic-check` and `abi-audit` are separately labeled fixtures.

The first actual guest run documented missing calls and directory-open
semantics. These failures selected the filesystem/lock work; default tar
ownership restoration then selected stored owner/group metadata. The profile
adds the following Linux x86-64 numbered operations:

| Number | Call | Implemented scope |
| --- | --- | --- |
| 73 | `flock` | Advisory shared/exclusive/unlock on mounted inodes, open-file-description ownership, dup/fork, last-close release, nonblocking and interruptible blocking waits |
| 86, 265 | `link`, `linkat` | Real RAMFS hard links, symlinks without implicit following, `AT_SYMLINK_FOLLOW`, directory descriptors and `AT_EMPTY_PATH`; directory links prohibited and cross-mount links rejected |
| 92, 93, 94, 260 | `chown`, `fchown`, `lchown`, `fchownat` | Stored 32-bit RAMFS uid/gid; `-1` leaves a field unchanged; descriptor/empty-path and no-follow handling; read-only mounts reject mutations |
| 137, 138 | `statfs`, `fstatfs` | RAMFS type, page size and name limit; unknown RAMFS capacities are zero as in Linux simple_statfs; persistent-filesystem statistics return unsupported |
| 230 | `clock_nanosleep` | Monotonic/realtime relative and absolute waits, validated times, interruptible waits and relative remainder; no CPU/alarm clock implementation |
| 268, 452 | `fchmodat`, `fchmodat2` | Basic permission bits, dirfds, no-follow/empty-path handling; unsupported symlink permissions and special mode bits produce errors |
| 332 | `statx` | 256-byte Linux layout, available basic metadata and explicit masks; no invented birth time, allocation count, mount ID or advanced attributes |
| 436 | `close_range` | Bounded close/CLOEXEC ranges; a unique table needs no copy for UNSHARE; shared-table UNSHARE returns unsupported |
| 439 | `faccessat2` | Dirfds, no-follow/empty paths and the established Coast permission/read-only checks; real/effective identity are both the existing UID/GID 0 |

`O_PATH` descriptors retain metadata identity without permitting data I/O,
seek, mmap, fsync or ordinary fd chmod/chown. They support duplication,
metadata queries and directory-relative operations. `readlinkat(fd, "")`
reads a retained symlink. RAMFS separates directory names from shared inode
identity; writes, ownership, timestamps and link counts are shared. Removing
or replacing a name preserves any open description until its last close.
EXT read-only metadata reports its stored owner/group and link count. FAT32
hard links/ownership and persistent statfs remain unsupported.

The process still runs as the existing single privileged identity. Stored
file ownership does not introduce login accounts or credential isolation.
Browser roles remain denied all new calls, with explicit negative tests.
Metadata, link mutations and descriptor tables follow the existing process
and VFS lock ordering. Blocking flock pins its description and drops locks
while sleeping, allowing a sibling to unlock or close/reuse a descriptor.

`tools/test_linux_compat.py` runs the actual upstream programs and in-tree
semantic fixtures against an explicit ISO. The semantic fixture exercises
link lifetime, shared data, rename aliases, ownership, statx masks/layout,
invalid pointers/flags, metadata descriptors, lock contention/fork/dup,
signals, clock deadlines and close-range behavior. An invalid-argument ABI
inventory only observes call presence; it is not a semantic compatibility pass.
Named FIFO creation remains an explicit unsupported probe. `sendfile`,
`copy_file_range`, vector-offset I/O, namespaces, arbitrary fcntl/OFD locks,
glibc and a general Linux ABI remain outside this bounded profile.
