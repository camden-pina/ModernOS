#include <libc.h>
#include <stdbool.h>

static char cwd[OS_PATH_MAX];
static char history[32][512];
static unsigned history_count;
static int background_request;
struct command_help {
    const char *name, *group, *usage, *summary, *details, *example;
    int min, max, external;
};

/* external: 0=builtin, 1=program, 2=foreground, 3=upstream help,
   4=upstream foreground, 5=upstream shell (scripts may run in background). */
static const struct command_help
    commands
        [] =
            {
                {"help", "shell", "help [COMMAND|CATEGORY]",
                 "Browse commands, syntax, and examples.",
                 "Categories: shell, files, text, disks, system, programs. Use COMMAND --help for its full page.",
                 "help mount", 0, 1, 0},
                {"pwd", "files", "pwd", "Print the current working directory.",
                 "Relative paths start here. / is the RAM root filesystem.", "pwd", 0, 0, 0},
                {"cd", "files", "cd [PATH]", "Change directory (default /).",
                 "Use .. for the parent. Quote names with spaces.", "cd /mnt/nvme1n1p1", 0, 1, 0},
                {"ls", "files", "ls [-a] [-l] [PATH...]",
                 "List files; -a includes hidden names, -l shows metadata.",
                 "Options may be combined as -al. With no path, list the current directory. Use -- before a path beginning with -.",
                 "ls -al /bin /mnt", 0, 63, 0},
                {"cat", "text", "cat PATH...", "Print file contents.",
                 "For large text files, head and tail show a shorter view.",
                 "cat \"/tmp/my document.txt\"", 1, 63, 0},
                {"cp", "files", "cp SOURCE DEST", "Copy one regular file and preserve its mode.",
                 "If DEST is a directory, use the source filename inside it. Existing files are overwritten. Recursive copies are not supported.",
                 "cp /mnt/nvme0n1p1/hello /tmp/hello", 2, 2, 0},
                {"mv", "files", "mv SOURCE DEST", "Move or rename a file or directory.",
                 "Existing destinations are refused. Between filesystems, regular files are copied then removed; a read-only source cannot be removed, so use cp.",
                 "mv /tmp/notes.txt /tmp/draft.txt", 2, 2, 0},
                {"mkdir", "files", "mkdir [-p] PATH...",
                 "Create directories; -p creates missing parents.",
                 "Without -p, the parent must exist. Permissions start at 0755.",
                 "mkdir -p /tmp/documents/drafts", 1, 63, 0},
                {"rmdir", "files", "rmdir PATH...", "Remove empty directories.",
                 "Directories used by a process or mounted filesystem remain busy.",
                 "rmdir /tmp/empty", 1, 63, 0},
                {"rm", "files", "rm [--] PATH...", "Remove regular files.",
                 "Directories need rmdir. Open files remain busy. Use -- for filenames beginning with -.",
                 "rm /tmp/old.txt", 1, 63, 0},
                {"touch", "files", "touch PATH...",
                 "Create empty files without truncating existing ones.",
                 "This console builtin only creates missing files. Use build-sh or busybox touch for real timestamp updates.",
                 "touch /tmp/notes.txt", 1, 63, 0},
                {"write", "text", "write PATH TEXT...", "Replace a file with one line of text.",
                 "Words are joined by spaces with a final newline. Existing contents are replaced; edit supports longer documents.",
                 "write /tmp/notes.txt \"First draft\"", 2, 63, 0},
                {"append", "text", "append PATH TEXT...", "Add a line to the end of a file.",
                 "Creates the file if missing. Existing bytes are preserved.",
                 "append /tmp/notes.txt \"Second paragraph\"", 2, 63, 0},
                {"edit", "text", "edit [-r] FILE",
                 "Open the document editor; -r opens a read-only view.",
                 "Arrows, Home/End, Page Up/Down, Delete, Enter, and Backspace edit text. Ctrl-S saves; Ctrl-Q quits with confirmation. Saves keep FILE.bak. Limit: 256 KiB ASCII text.",
                 "edit \"/tmp/my document.txt\"", 1, 2, 2},
                {"nano", "text", "nano [OPTIONS] [FILE...]", "Run native upstream GNU nano 8.7.",
                 "Works on the console and in the desktop Terminal through a real PTY. Ctrl-O writes, Ctrl-X exits, Ctrl-W searches. Use nano --help for upstream options. For shell commands use env SHELL=/bin/ash nano FILE. Process suspension/job control remains unsupported.",
                 "nano /mnt/nvme1n1p1/notes.txt", 0, 63, 4},
                {"vim", "text", "vim [OPTIONS] [FILE...]", "Run native upstream Vim 9.2.",
                 "i inserts text; Escape returns to normal mode; :w saves, :q quits, :wq does both. :help opens the bundled upstream manual. Syntax highlighting, undo, search and shell filters are supported. Job control and terminal-job windows are unavailable.",
                 "vim /tmp/notes.txt", 0, 63, 4},
                {"ash", "shell", "ash [SCRIPT [ARGS...] | -c COMMAND]",
                 "Run the upstream BusyBox POSIX shell.",
                 "Provides pipes, redirects, variables, globbing and scripts. Job control is disabled. Use build-sh to select full upstream text tools and configure-script defaults.",
                 "ash -c 'printf b\\n | sort'", 0, 63, 5},
                {"build-sh", "programs", "build-sh [SCRIPT [ARGS...] | -c COMMAND]",
                 "Open the native build environment.",
                 "Runs real ash with PATH=/usr/bin:/bin and SHELL/CONFIG_SHELL=/bin/ash. Native GCC/G++, binutils, musl development headers and static libraries are installed. Full text tools override the basic console tools.",
                 "build-sh -c 'gcc -O2 /tmp/hello.c -o /tmp/hello && /tmp/hello'",
                 0, OS_ARG_MAX - 1, 5},
                {"busybox", "programs", "busybox [APPLET [ARGS...]]",
                 "Run or list the upstream utility suite.",
                 "busybox --list lists compiled applets. busybox APPLET --help prints real options. Utilities act on the actual filesystem; unavailable OS features return errors.",
                 "busybox grep -En '^[A-Z]+' /tmp/notes.txt", 0, 63, 3},
                {"make", "programs", "make [OPTIONS] [TARGET...]",
                 "Run native upstream GNU make 4.4.1.",
                 "Reads actual makefiles, executes recipes with /bin/ash, uses real timestamps and supports -j parallel builds through a pipe jobserver. Use build-sh for the compiler and full text utilities.",
                 "make -j2", 0, OS_ARG_MAX - 1, 3},
                {"gcc", "programs", "gcc [OPTIONS] SOURCE... [-o OUTPUT]",
                 "Compile and link native C with GCC 14.2.0.",
                 "-c compiles an object; -O2 optimizes; -Wall enables warnings. Defaults to static x86-64 musl executables using actual ModernOS syscalls. Dynamic linking and pthreads are unavailable. Compiler diagnostics identify invalid options and source errors.",
                 "gcc -O2 -Wall /tmp/hello.c -o /tmp/hello", 0, OS_ARG_MAX - 1, 3},
                {"g++", "programs", "g++ [OPTIONS] SOURCE... [-o OUTPUT]",
                 "Compile and link native C++ with GCC 14.2.0.",
                 "Includes C++ headers, static libstdc++ and exceptions. Use g++ to link C++ objects. Threading and dynamic linking are unavailable.",
                 "g++ -std=c++17 /tmp/hello.cpp -o /tmp/hello", 0, OS_ARG_MAX - 1, 3},
                {"cc", "programs", "cc [OPTIONS] SOURCE... [-o OUTPUT]",
                 "Run the native GCC C compiler.",
                 "An executable alias of GCC. See help gcc for the target's supported runtime.",
                 "cc /tmp/hello.c -o /tmp/hello", 0, OS_ARG_MAX - 1, 3},
                {"c++", "programs", "c++ [OPTIONS] SOURCE... [-o OUTPUT]",
                 "Run the native GCC C++ compiler.",
                 "An executable alias of G++. See help g++ for the target's supported runtime.",
                 "c++ /tmp/hello.cpp -o /tmp/hello", 0, OS_ARG_MAX - 1, 3},
                {"cpp", "programs", "cpp [OPTIONS] [SOURCE [OUTPUT]]",
                 "Run the real C preprocessor.",
                 "Use -I for include directories and -D for definitions. GCC also supports -E preprocessing.",
                 "cpp /tmp/hello.c", 0, OS_ARG_MAX - 1, 3},
                {"as", "programs", "as [OPTIONS] SOURCE... -o OBJECT",
                 "Assemble native x86-64 instructions.",
                 "GNU assembler 2.37. Writes real relocatable ELF objects for the native linker.",
                 "as /tmp/start.s -o /tmp/start.o", 0, OS_ARG_MAX - 1, 3},
                {"ld", "programs", "ld [OPTIONS] OBJECT... -o OUTPUT",
                 "Link actual native ELF objects.",
                 "GNU linker 2.37. Prefer gcc/g++ to include the appropriate startup objects and static runtime. The kernel loads static ET_EXEC images; shared/dynamic images are unsupported.",
                 "ld --help", 0, OS_ARG_MAX - 1, 3},
                {"ar", "programs", "ar [OPTIONS] ARCHIVE [OBJECT...]",
                 "Create or inspect real object archives.",
                 "rcs creates/updates an indexed library; t lists members; x extracts. Use GCC/g++ to link the archive.",
                 "ar rcs /tmp/libsample.a /tmp/sample.o", 0, OS_ARG_MAX - 1, 3},
                {"ranlib", "programs", "ranlib [OPTIONS] ARCHIVE...",
                 "Index static object libraries.",
                 "Updates the archive symbol index using native GNU binutils.",
                 "ranlib /tmp/libsample.a", 0, OS_ARG_MAX - 1, 3},
                {"nm", "programs", "nm [OPTIONS] OBJECT...", "Inspect ELF symbols.",
                 "Use -u for undefined symbols and -C to demangle C++ names.", "nm -C /tmp/hello",
                 0, OS_ARG_MAX - 1, 3},
                {"readelf", "programs", "readelf [OPTIONS] ELF...",
                 "Inspect actual ELF headers and sections.",
                 "-h shows the header; -l shows load segments; -s shows symbols.",
                 "readelf -hl /tmp/hello", 0, OS_ARG_MAX - 1, 3},
                {"objdump", "programs", "objdump [OPTIONS] OBJECT...",
                 "Inspect/disassemble native objects.",
                 "-d disassembles executable sections; -h lists section headers.",
                 "objdump -d /tmp/hello", 0, OS_ARG_MAX - 1, 3},
                {"objcopy", "programs", "objcopy [OPTIONS] INPUT [OUTPUT]",
                 "Copy or transform real object files.",
                 "Supports format conversion and section/symbol options. Failed operations report errors.",
                 "objcopy --help", 0, OS_ARG_MAX - 1, 3},
                {"size", "programs", "size [OPTIONS] OBJECT...", "Report executable section sizes.",
                 "GNU binutils calculates text, data and BSS sizes from the actual file.",
                 "size /tmp/hello", 0, OS_ARG_MAX - 1, 3},
                {"strings", "programs", "strings [OPTIONS] FILE...",
                 "Find printable strings in real files.",
                 "-n LENGTH sets the minimum length; -a scans all data.", "strings /tmp/hello",
                 0, OS_ARG_MAX - 1, 3},
                {"strip", "programs", "strip [OPTIONS] OBJECT...",
                 "Remove debug data or symbols from binaries.",
                 "Modifies the specified files. --strip-debug retains ordinary symbols.",
                 "strip --strip-debug /tmp/hello", 0, OS_ARG_MAX - 1, 3},
                {"bootstrap-toolchain",
                 "programs", "bootstrap-toolchain [--work DIR] [--prefix DIR] [-j JOBS] [--prepare-only]",
                 "Rebuild the native GCC/binutils toolchain from bundled sources.",
                 "Uses actual native compilers and make, with verified sources under /usr/src/modernos. Build in a RAM workspace; 8 GiB memory is recommended. Installs a separate toolchain and runs C/C++ proofs. Use build-sh to invoke this script.",
                 "build-sh /usr/bin/bootstrap-toolchain -j4", 0, 7, 3},
                {"compiler-check", "programs", "compiler-check [--metadata | --directory]",
                 "Check compiler-facing runtime interfaces.",
                 "Tests large argv/environments, file mappings, CPU accounting, executable scripts and descriptors. --directory checks 8192 actual files, lookup, enumeration and rename. --metadata additionally creates and removes 327680 real files to check heap growth; allow time and sufficient RAM.",
                 "compiler-check", 0, 1, 1},
                {"build-check",
                 "programs", "build-check [--files DIRECTORY | --registers | --suspend | --faults]", "Validate process and build-tool syscalls.", "Checks fork/TLS/guards, SIGCHLD, timers, signal return, waits, vfork ordering, shared offsets, posix_spawn, and errors. --registers checks asynchronous RCX/R11 preservation; --suspend checks signal wait masks; --faults checks hardware fault delivery and metadata. --files checks resize, zero fill, timestamps and symlink capabilities on a writable directory.",
                 "build-check --files /tmp", 0, 2, 1},
                {"sed", "text", "sed [OPTIONS] SCRIPT [FILE...]",
                 "Transform lines with upstream sed.",
                 "Supports standard input and regular expressions. In-place replacement can fail where the filesystem cannot replace an existing rename target.",
                 "build-sh -c 'printf hello | sed s/hello/world/'", 0, 63, 3},
                {"awk", "text", "awk [OPTIONS] PROGRAM [FILE...]",
                 "Process fields and records with GNU awk.",
                 "Upstream GNU awk supports standard input, variables, arithmetic, regular expressions and GCC build generators. Dynamic extensions, persistent-memory allocation and arbitrary precision are disabled.",
                 "build-sh -c 'printf \"a 2\\\\n\" | awk \"{print \\\\$2}\"'", 0, OS_ARG_MAX - 1,
                 3},
                {"gawk", "text", "gawk [OPTIONS] PROGRAM [FILE...]",
                 "Run upstream GNU awk explicitly.",
                 "The awk and gawk commands use the same native GNU awk implementation. Use gawk --help for actual supported options.",
                 "gawk --version", 0, OS_ARG_MAX - 1, 3},
                {"sort", "text", "sort [OPTIONS] [FILE...]",
                 "Sort lines from files or standard input.",
                 "Use -n for numeric order, -r for reverse, -u for unique values.",
                 "build-sh -c 'printf \"b\\\\na\\\\n\" | sort'", 0, 63, 3},
                {"uniq", "text", "uniq [OPTIONS] [INPUT [OUTPUT]]",
                 "Remove adjacent duplicate lines.",
                 "Sort first when duplicates are not already adjacent.",
                 "build-sh -c 'sort /tmp/names.txt | uniq -c'", 0, 63, 3},
                {"tr", "text", "tr [OPTIONS] SET1 [SET2]",
                 "Translate or delete bytes from standard input.",
                 "Supports character classes, -d deletion and -s squeezing.",
                 "build-sh -c 'printf abc | tr a-z A-Z'", 0, 63, 3},
                {"cut", "text", "cut [OPTIONS] [FILE...]", "Select fields or byte ranges.",
                 "Use -d DELIMITER with -f FIELDS, or -b BYTES.", "cut -d : -f 1 /tmp/records.txt",
                 0, 63, 3},
                {"diff", "text", "diff [OPTIONS] FILE1 FILE2", "Compare actual file contents.",
                 "-u emits a unified patch; -r compares directories. Exit status 1 means differences.",
                 "diff -u /tmp/old.c /tmp/new.c", 0, 63, 3},
                {"patch", "text", "patch [OPTIONS] [ORIGINAL [PATCHFILE]]",
                 "Apply a patch to a source file.",
                 "Use -pN to strip leading path components. Back up valuable files before patching.",
                 "build-sh -c 'patch -p0 < /tmp/change.patch'", 0, 63, 3},
                {"tar", "files", "tar -c|-x|-t [-f ARCHIVE] [OPTIONS] [FILE...]",
                 "Create, extract or list upstream tar archives.",
                 "gzip/bzip2/xz decompression is available. Extract with --no-same-owner because ownership changes are unsupported. Requires a writable destination.",
                 "tar -xf /tmp/source.tar --no-same-owner", 0, 63, 3},
                {"gzip", "files", "gzip [OPTIONS] [FILE...]", "Compress files with gzip.",
                 "-d decompresses; -c writes to standard output and preserves the source.",
                 "build-sh -c 'gzip -c /tmp/notes.txt > /tmp/notes.gz'", 0, 63, 3},
                {"gunzip", "files", "gunzip [OPTIONS] [FILE...]", "Decompress gzip streams.",
                 "-c writes to standard output without deleting the archive.",
                 "gunzip -c /tmp/notes.gz", 0, 63, 3},
                {"bzip2", "files", "bzip2 [OPTIONS] [FILE...]",
                 "Compress or decompress bzip2 data.",
                 "Use -c to retain the input file and -d to decompress.",
                 "build-sh -c 'bzip2 -c /tmp/notes.txt > /tmp/notes.bz2'", 0, 63, 3},
                {"xzcat", "files", "xzcat [FILE...]", "Decompress xz data to standard output.",
                 "The port supplies xz decompression; an xz encoder is not enabled.",
                 "xzcat /tmp/source.tar.xz", 0, 63, 3},
                {"ln", "files", "ln [OPTIONS] TARGET LINK", "Create a real symbolic link with -s.",
                 "RAM supports symlinks, FAT32 does not; hard links are unsupported. Errors describe actual filesystem capability.",
                 "ln -s /tmp/notes.txt /tmp/notes-link", 0, 63, 3},
                {"readlink", "files", "readlink [-f] PATH",
                 "Read a real symlink or resolve a path.",
                 "Relative targets are resolved against their containing directory.",
                 "readlink /tmp/notes-link", 0, 63, 3},
                {"realpath", "files", "realpath PATH...", "Resolve canonical filesystem paths.",
                 "Requires actual resolvable entries.", "realpath /tmp/../bin/vim", 0, 63, 3},
                {"mktemp", "files", "mktemp [OPTIONS] [TEMPLATE]",
                 "Create a unique temporary file or directory.",
                 "Use -d for a directory. Files are created with exclusive checks.",
                 "mktemp /tmp/build.XXXXXX", 0, 63, 3},
                {"install", "files", "install [OPTIONS] SOURCE DEST",
                 "Copy files with installation permissions.",
                 "Use -d to create directories; unsupported ownership requests report errors.",
                 "install -m 755 /bin/hello /tmp/hello", 0, 63, 3},
                {"du", "files", "du [OPTIONS] [PATH...]",
                 "Estimate directory space from filesystem metadata.",
                 "Use -s for a total. Results reflect the backend stat data.",
                 "du -s /usr/share/vim", 0, 63, 3},
                {"date", "system", "date [OPTIONS] [+FORMAT]",
                 "Format the actual OS realtime clock.",
                 "The firmware-derived clock is UTC. Setting the system clock is unsupported.",
                 "date -u +%Y-%m-%d", 0, 63, 3},
                {"sha256sum", "text", "sha256sum [OPTIONS] [FILE...]",
                 "Hash actual file bytes with SHA-256.", "Use -c to check a saved checksum list.",
                 "sha256sum /bin/vim", 0, 63, 3},
                {"md5sum", "text", "md5sum [OPTIONS] [FILE...]",
                 "Hash files with MD5 for compatibility checks.",
                 "Use -c to verify a saved checksum list.", "md5sum /tmp/notes.txt", 0, 63, 3},
                {"xargs", "shell", "xargs [OPTIONS] [COMMAND [ARGS...]]",
                 "Build command arguments from standard input.",
                 "Use -0 with find -print0; -n limits each invocation. The kernel accepts 64 argv entries.",
                 "build-sh -c 'find /tmp -type f -print0 | xargs -0 -n 1 wc -c'", 0, 63, 3},
                {"getopt", "shell", "getopt [OPTIONS] -- ARGUMENTS...",
                 "Parse script options with upstream getopt.",
                 "Supports short and long options for native ash scripts.",
                 "getopt -o ab: -- -a -b value", 0, 63, 3},
                {"seq", "text", "seq [FIRST [INCREMENT]] LAST", "Generate a sequence of numbers.",
                 "Output can feed real shell pipelines.", "seq 1 5", 0, 63, 3},
                {"printf", "text", "printf FORMAT [ARGUMENT...]",
                 "Print formatted text and escaped characters.",
                 "For redirects and pipelines use ash/build-sh.", "printf \"hello\\\\n\"", 0, 63,
                 3},
                {"env", "programs", "env [-i] [NAME=VALUE...] [PROGRAM [ARGS...]]",
                 "Inspect or change a child's environment.",
                 "Variables are passed on the real ELF initial stack. -i clears inherited values. Bare program names search PATH; the kernel accepts at most 64 variables.",
                 "env TERM=vt100 nano /tmp/notes.txt", 0, 63, 2},
                {"stty", "system", "stty [-a|size|raw|cooked|echo|-echo]",
                 "Inspect or configure terminal input.",
                 "size reports the actual terminal geometry. Only implemented line discipline options are accepted.",
                 "stty size", 0, 1, 2},
                {"tty", "system", "tty", "Identify the current terminal.",
                 "Reports /dev/tty or a descriptor-based pseudoterminal session. PTY device paths are not exposed.",
                 "tty", 0, 0, 2},
                {"reset", "system", "reset", "Restore normal terminal input and clear the screen.",
                 "Restores canonical input, echo and signal keys through actual terminal ioctls.",
                 "reset", 0, 0, 2},
                {"head", "text", "head [-n LINES] FILE", "Print the first lines (default 10).",
                 "LINES is a nonnegative decimal count.", "head -n 5 /tmp/notes.txt", 1, 3, 1},
                {"tail", "text", "tail [-n LINES] FILE", "Print the last lines (default 10).",
                 "Reads a seekable regular file without loading the whole file.",
                 "tail -n 5 /tmp/notes.txt", 1, 3, 1},
                {"wc", "text", "wc FILE...", "Count lines, words, and bytes.",
                 "Lines count newline bytes. Words are separated by ASCII whitespace.",
                 "wc /tmp/notes.txt", 1, 63, 1},
                {"grep", "text", "grep [-n] PATTERN FILE...", "Find lines containing literal text.",
                 "-n prints line numbers. Matching is case-sensitive; regular expressions are not supported.",
                 "grep -n draft /tmp/notes.txt", 2, 63, 1},
                {"find", "files", "find [PATH]", "List a directory tree.",
                 "Defaults to the current directory. Does not follow directory symlinks. Recursion is bounded to 16 levels.",
                 "find /tmp/documents", 0, 1, 1},
                {"hexdump", "text", "hexdump FILE",
                 "Inspect bytes as hexadecimal and printable text.",
                 "Shows 16 bytes per line with hexadecimal offsets.", "hexdump /bin/hello", 1, 1,
                 1},
                {"chmod", "files", "chmod OCTAL PATH", "Set file permissions.",
                 "OCTAL must be 0 through 777 using only digits 0-7. FAT32 emulates write permission; ext mounts remain read-only.",
                 "chmod 755 /tmp/hello", 2, 2, 0},
                {"stat", "files", "stat PATH", "Show file type, size, mode, device, and inode.",
                 "Symlinks are followed. Unix ownership is unavailable on FAT32; its stored modification time has two-second precision.",
                 "stat /bin/sh", 1, 1, 0},
                {"lsblk", "disks", "lsblk", "List drives, partitions, sizes, and mount locations.",
                 "Device identifiers are accepted by mount. Raw disk read/write nodes are not exposed.",
                 "lsblk", 0, 0, 0},
                {"disks", "disks", "disks", "Show device sector counts and sector sizes.",
                 "NVMe and AHCI SATA discovery occurs at boot. Disk hotplug is not supported.",
                 "disks", 0, 0, 0},
                {"mount", "disks", "mount [-t TYPE] [-o ro|rw] DEVICE DIRECTORY",
                 "Mount a filesystem, or list mounts with no arguments.",
                 "TYPE: auto, fat32, ext, ext2, ext4. DIRECTORY must exist and be empty. DEVICE is a name from lsblk, optionally /dev/NAME. A device can be mounted once. ext filesystems are always read-only.",
                 "mount -o ro nvme1n1p1 /mnt/data", 0, 6, 0},
                {"umount", "disks", "umount DIRECTORY", "Flush and unmount a disk filesystem.",
                 "First cd outside the mount and close programs using it. Busy mounts and / are refused. Forced and lazy unmount are not supported.",
                 "umount /mnt/nvme1n1p1", 1, 1, 0},
                {"mounts", "disks", "mounts",
                 "List mount paths, filesystem types, and access modes.",
                 "RAM files disappear on reboot. Save persistent documents to a writable FAT32 mount.",
                 "mounts", 0, 0, 0},
                {"sync", "disks", "sync", "Flush filesystem metadata and disk write caches.",
                 "Editor saves and file copies also flush writes. FAT32 does not provide power-loss atomicity.",
                 "sync", 0, 0, 0},
                {"ps", "system", "ps", "List processes and kernel tasks.",
                 "Shows PID, parent PID, CPU, scheduling state, runtime ticks, and program name.",
                 "ps",
                 0, 0,
                 0},
                {"cpus", "system", "cpus", "Show online CPUs and scheduler counters.",
                 "Tasks are preemptive and pinned to the CPU selected at creation.", "cpus", 0, 0,
                 0},
                {"uname", "system", "uname [-a]",
                 "Print the OS name; -a includes version and architecture.",
                 "This OS provides a growing Linux-style syscall subset, not complete Linux binary compatibility.",
                 "uname -a", 0, 1, 0},
                {"uptime", "system", "uptime", "Print elapsed time since boot.",
                 "Uses the monotonic clock, independent of wall-clock time.", "uptime", 0, 0, 0},
                {"gui", "programs",
                 "gui [--info] [--stats] [--trace] [--browser-network-trace] [--browser-exception-trace]",
                 "Open the ModernOS desktop.",
                 "Files, Terminal, Browser, Text Editor and Settings use shared windows and real workspaces. Super opens the launcher; Ctrl+Alt+Escape returns safely to the console. --info reports display capabilities; --stats reports damage traffic on exit; --trace logs desktop actions. Browser trace flags enable explicit developer diagnostics.",
                 "gui", 0, 5, 2},
                {"git", "programs", "git COMMAND [OPTIONS]", "Manage real Git repositories.",
                 "Upstream Git supports local repositories and smart HTTP/HTTPS clone, fetch and push through the actual network backend. Configure your user.name and user.email before committing. Work in RAM; FAT lacks the replacement operations Git needs. SSH needs an SSH client, which is not installed. Run git COMMAND -h for options.",
                 "git init /home/user/Projects/repository", 1, OS_ARG_MAX - 1, 2},
                {"netinfo", "system", "netinfo",
                 "Show actual network link, DHCP address and packet counters.",
                 "IPv4 TCP/UDP and loopback are supported. The Intel e1000/e1000e driver supports the QEMU adapter. IPv6, Wi-Fi and unsupported NICs remain unavailable.",
                 "netinfo", 0, 0, 0},
                {"curl", "programs", "curl [OPTIONS] URL...",
                 "Transfer actual HTTP/HTTPS resources.",
                 "Uses the kernel's IPv4/DNS backend, OpenSSL and the bundled CA certificates. -I requests headers; -o FILE saves a response. Certificates and hostname are verified by default. Use curl --help for options.",
                 "curl -I https://www.netsurf-browser.org/", 0, OS_ARG_MAX - 1, 3},
                {"openssl", "programs", "openssl COMMAND [OPTIONS]",
                 "Use the native OpenSSL cryptographic tools.",
                 "Supports hashing, certificates, key generation and TLS using hardware-backed getrandom entropy. Use openssl help for the available commands.",
                 "openssl version", 0, OS_ARG_MAX - 1, 3},
                {"run", "programs", "run PROGRAM [ARGS...] [&]",
                 "Start a native static ELF program or executable script.",
                 "Bare names search /bin; paths run directly. Scripts require an absolute #! interpreter. & starts a background task. Ctrl-C terminates a foreground child. Supported ELF/syscall formats are required.",
                 "run hello example", 1, OS_ARG_MAX - 1, 1},
                {"which", "programs", "which PROGRAM...", "Locate executable programs.",
                 "Bare names search /bin. Paths containing / are checked directly.",
                 "which edit hello", 1, 63, 0},
                {"jobs", "shell", "jobs", "Show background jobs started by this shell.",
                 "Completed statuses stay available until wait consumes them.", "jobs", 0, 0, 0},
                {"wait", "shell", "wait [PID]",
                 "Wait for one background child and report its status.",
                 "Without PID, wait for any tracked job. PID must be greater than 1. Status is the raw wait status: exit code shifted by 8, or termination signal.",
                 "wait 12", 0, 1, 0},
                {"kill", "system", "kill [-SIGNAL] PID",
                 "Send a signal to a process (default signal 9).",
                 "Signal handlers, masks and return are supported. PID 1 is protected. Process groups, job suspension and stop signals are not implemented.",
                 "kill -15 12", 1, 2, 0},
                {"sleep", "system", "sleep SECONDS", "Pause for 0 through 86400 whole seconds.",
                 "SECONDS must be a nonnegative decimal integer.", "sleep 1", 1, 1, 0},
                {"echo", "shell", "echo [TEXT...]", "Print arguments separated by spaces.",
                 "Use single or double quotes to group words.", "echo \"Hello world\"", 0, 63, 0},
                {"clear", "shell", "clear", "Clear the terminal screen.",
                 "Uses ANSI escape sequences on the framebuffer and serial terminal.", "clear", 0,
                 0, 0},
                {"history", "shell", "history", "Show the last 32 interactive command lines.",
                 "History is kept in RAM by this shell; expansion with ! is not supported.",
                 "history", 0, 0, 0},
                {"exit", "shell", "exit", "Describe how to stop the system shell.",
                 "PID 1 stays available. An ordinary child sh -c COMMAND exits with the command status. Stop the VM through its controls.",
                 "exit", 0, 0, 0},
                {"sh", "shell", "sh [SCRIPT [ARGS...] | -c COMMAND]",
                 "Run the actual BusyBox POSIX shell.",
                 "Supports scripts, pipes, redirects, loops and variables. modernos-sh provides the desktop's OS command shell. Use build-sh to select full build utilities.",
                 "sh -c 'echo hello | sort'", 0, OS_ARG_MAX - 1, 5},
                {"modernos-sh", "shell", "modernos-sh [--terminal|-c COMMAND]",
                 "Open the ModernOS command console.",
                 "Provides help, disk and desktop commands. --terminal uses a real pseudoterminal. Child shells may exit; PID 1 stays available. Use build-sh or sh for POSIX scripts and redirection.",
                 "modernos-sh -c 'help gcc'", 0, 2, 1},
                {"hello", "programs", "hello [ARGS...]",
                 "Check ring-3 execution and argument passing.",
                 "A small native static executable.", "hello example", 0, 63, 1},
                {"check", "programs", "check [--tty-read|--tty-denied|--display-exit|--display-fault|--storage MOUNT]", "Run kernel ABI and isolation checks.", "Tests pointer validation, files, memory, scheduling, terminal and display access. --display-fault intentionally faults after acquiring the display; --storage checks busy unmount behavior.",
                 "check", 0, 2, 1},
                {"spin", "programs", "spin [ITERATIONS] [&]",
                 "Exercise scheduler preemption with a busy task.",
                 "Use kill and wait to stop and reap a background spin.", "spin 4000000000 &", 0, 1,
                 1},
                {"fault", "programs", "fault", "Exercise recovery from a user process fault.",
                 "Intentionally triggers a user fault. The system shell survives.", "fault", 0, 0,
                 1},
                {"exec-test", "programs", "exec-test", "Check executable image replacement.",
                 "Executes hello using execve.", "exec-test", 0, 0, 1},
                {"posix-check", "programs", "posix-check", "Check the native static-musl runtime.",
                 "Tests TLS, allocation, memory guards, signal handlers, poll/select, and real pseudoterminal input/output and resizing.",
                 "posix-check", 0, 0, 1}};

static const struct command_help *lookup_command(const char *name) {
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        if (!strcmp(name, commands[i].name))
            return &commands[i];
    return NULL;
}

static void help_page(const struct command_help *entry) {
    printf("Usage: %s\n%s\n%s\nExample: %s\n", entry->usage, entry->summary, entry->details,
           entry->example);
}

static int usage_error(const struct command_help *entry, const char *reason) {
    printf("%s: %s\n", entry->name, reason);
    help_page(entry);
    return 2;
}

static int help(const char *topic) {
    if (topic) {
        const struct command_help *entry = lookup_command(topic);
        if (entry) {
            help_page(entry);
            return 0;
        }
        int found = 0;
        for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
            if (!strcmp(topic, commands[i].group)) {
                printf("  %s - %s\n", commands[i].usage, commands[i].summary);
                found = 1;
            }
        if (!found) {
            printf("help: unknown command or category '%s'.\n", topic);
            puts("Categories: shell files text disks system programs");
            return 2;
        }
        puts("Use help COMMAND for options and examples.");
        return 0;
    }

    puts("ModernOS command guide: help COMMAND, help CATEGORY, or COMMAND --help");
    const char *groups[] = {"shell", "files", "text", "disks", "system", "programs"};
    for (unsigned g = 0; g < 6; g++) {
        printf("%s:", groups[g]);
        for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
            if (!strcmp(groups[g], commands[i].group))
                printf(" %s", commands[i].name);
        puts("");
    }

    puts("Paths: quote spaces; / and /tmp live in RAM; writable FAT32 mounts persist.");
    puts("Programs: NAME searches /bin; run PATH ARGS launches an ELF/script; & starts a job.");
    puts("Terminal: Backspace edits a line; Ctrl-C cancels; edit FILE opens the editor.");
    puts("Storage: lsblk -> mounts -> ls /mnt/DEVICE. Use sync before stopping.");
    puts("Example: edit /mnt/nvme1n1p1/notes.txt");
    puts(
        "For scripts, pipelines and redirection, start build-sh. Use gcc/g++ to compile programs.");
    return 0;
}

static struct job {
    int pid, status, finished;
} jobs[128];

static void remember_job(int pid) {
    for (unsigned i = 0; i < 128; i++)
        if (!jobs[i].pid) {
            jobs[i] = (struct job){pid, 0, 0};
            return;
        }
}

static void reap_jobs(void) {
    int status, pid;
    while ((pid = waitpid(-1, &status, 1)) > 0) {
        for (unsigned i = 0; i < 128; i++)
            if (jobs[i].pid == pid) {
                jobs[i].finished = 1;
                jobs[i].status = status;
                break;
            }
    }
}

static void error(const char *name, long code) {
    printf("%s: %s (%ld)\n", name, error_text(code), code);
}

static int tokenize(char *line, char **argv) {
    char *readp = line, *writep = line;
    int argc = 0;
    background_request = 0;
    while (*readp) {
        while (*readp == ' ' || *readp == '\t' || *readp == '\n')
            readp++;
        if (!*readp)
            break;
        if (argc == OS_ARG_MAX)
            return -E_2BIG;
        argv[argc++] = writep;
        int literal = 0;
        char quote = 0;

        while (*readp && (quote || (*readp != ' ' && *readp != '\t' && *readp != '\n'))) {
            char c = *readp++;
            if (!quote && (c == '|' || c == '<' || c == '>' || c == ';'))
                return -E_NOTSUP;
            if (c == '\\' && quote != '\'') {
                if (!*readp)
                    return -E_INVAL;
                literal = 1;
                *writep++ = *readp++;
                continue;
            }

            if (c == '\'' || c == '"') {
                literal = 1;
                if (quote == c) {
                    quote = 0;
                    continue;
                }
                if (!quote) {
                    quote = c;
                    continue;
                }
            }

            *writep++ = c;
        }

        if (quote)
            return -E_INVAL;
        if (*readp)
            readp++;
        *writep++ = 0;
        if (!literal && !strcmp(argv[argc - 1], "&")) {
            while (*readp == ' ' || *readp == '\t')
                readp++;
            if (*readp || argc == 1)
                return -E_NOTSUP;
            background_request = 1;
        }
    }

    argv[argc] = NULL;
    return argc;
}

static int ls(const char *path, int hidden, int detailed) {
    int fd = open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        error(path, fd);
        return 1;
    }
    uint8_t buffer[4096];
    long n;
    int failed = 0;
    while ((n = call3(SYS_getdents64, fd, buffer, sizeof(buffer))) > 0) {
        for (long off = 0; off < n;) {
            struct os_dirent *d = (void *)(buffer + off);
            if (d->reclen < 24 || d->reclen > n - off) {
                puts("ls: invalid directory response");
                close(fd);
                return 1;
            }
            if (hidden || d->name[0] != '.') {
                if (detailed) {
                    struct os_stat st;
                    long e = syscall6(SYS_newfstatat, fd, (long)d->name, (long)&st, 0, 0, 0);
                    if (e < 0) {
                        error(d->name, e);
                        failed = 1;
                    } else
                        printf("%o %ld ", st.mode & 0777, (long)st.size);
                }

                printf("%s%s\n", d->name, d->type == 4 ? "/" : d->type == 10 ? "@" : "");
            }

            off += d->reclen;
        }
    }

    if (n < 0) {
        error(path, n);
        failed = 1;
    }
    close(fd);
    return failed;
}

static int cat(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        error(path, fd);
        return 1;
    }
    char buffer[4096];
    long n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        long written = 0;
        while (written < n) {
            long part = write(1, buffer + written, n - written);
            if (part <= 0) {
                error("cat", part ? part : -E_IO);
                close(fd);
                return 1;
            }
            written += part;
        }
    }

    if (n < 0)
        error(path, n);
    close(fd);
    return n < 0 ? 1 : 0;
}

static int destination(const char *source, const char *target, char *out) {
    struct os_stat st;
    if (!stat(target, &st) && (st.mode & 0170000) == S_IFDIR) {
        const char *base = source;
        for (const char *p = source; *p; p++)
            if (*p == '/')
                base = p + 1;
        size_t n = strlen(target), b = strlen(base);
        if (!b || n + b + 2 > OS_PATH_MAX)
            return -E_NAMETOOLONG;
        strcpy(out, target);
        out[n++] = '/';
        strcpy(out + n, base);
    } else {
        if (strlen(target) >= OS_PATH_MAX)
            return -E_NAMETOOLONG;
        strcpy(out, target);
    }
    return 0;
}

static int copy_file(const char *source, const char *target) {
    char path[OS_PATH_MAX];
    int e = destination(source, target, path);
    if (e)

        return e;

    int input = open(source, O_RDONLY, 0);
    if (input < 0)
        return input;

    struct os_stat src, dst;
    e = fstat(input, &src);
    if (e || (src.mode & 0170000) != S_IFREG) {
        close(input);
        return e ? e : -E_ISDIR;
    }
    if (!stat(path, &dst) && dst.dev == src.dev && dst.ino == src.ino) {
        close(input);
        return -E_INVAL;
    }

    int output = open(path, O_CREAT | O_WRONLY | O_TRUNC, src.mode & 0777);
    if (output < 0) {
        close(input);
        return output;
    }

    char buffer[4096];
    long n, total = 0;
    while ((n = read(input, buffer, sizeof(buffer))) > 0) {
        long written = 0;
        while (written < n) {
            long part = write(output, buffer + written, n - written);
            if (part <= 0) {
                e = part ? part : -E_NOSPC;
                break;
            }
            written += part;
        }

        if (e)
            break;
        total += n;
    }

    if (!e && n < 0)
        e = n;
    if (!e && total != src.size)
        e = -E_IO;

    close(output);
    close(input);
    if (!e) {
        long mode = call2(SYS_chmod, path, src.mode & 0777);
        if (mode < 0 && mode != -E_NOTSUP)
            e = mode;
    }

    if (!e)
        e = call0(SYS_sync);
    return e;
}

static void info(unsigned kind) {
    char buffer[8192];
    long n = call3(SYS_sysinfo, kind, buffer, sizeof(buffer));
    if (n < 0)
        error("sysinfo", n);
    else
        write(1, buffer, n);
}

static int mkdir_parents(const char *path) {
    char copy[OS_PATH_MAX];
    if (strlen(path) >= sizeof(copy))
        return -E_NAMETOOLONG;
    strcpy(copy, path);
    for (char *p = copy + 1;; p++)
        if (*p == '/' || !*p) {
            char saved = *p;
            *p = 0;
            if (*copy) {
                long e = call2(SYS_mkdir, copy, 0755);
                if (e && e != -E_EXIST)
                    return e;
                struct os_stat st;
                e = stat(copy, &st);
                if (e)
                    return e;
                if ((st.mode & 0170000) != S_IFDIR)
                    return -E_NOTDIR;
            }

            *p = saved;
            if (!saved)
                break;
        }
    return 0;
}

static int program_path(const char *program, char *path) {
    int slash = 0;
    for (const char *p = program; *p; p++)
        if (*p == '/')
            slash = 1;
    size_t n = strlen(program);
    if (!n)
        return -E_NOENT;
    if (n + (slash ? 1 : 6) > OS_PATH_MAX)
        return -E_NAMETOOLONG;
    if (slash)
        strcpy(path, program);
    else {
        strcpy(path, "/bin/");
        strcpy(path + 5, program);
    }
    return 0;
}

static void suggest(const char *name) {
    /* Bounded edit distance for short command names. */
    size_t n = strlen(name);
    if (n > 32)
        return;
    const char *best = NULL;
    unsigned score = 3;
    for (unsigned c = 0; c < sizeof(commands) / sizeof(commands[0]); c++) {
        const char *candidate = commands[c].name;
        size_t m = strlen(candidate);
        unsigned row[33], next[33];
        if (m > 32)
            continue;
        for (unsigned j = 0; j <= m; j++)
            row[j] = j;
        for (unsigned i = 1; i <= n; i++) {
            next[0] = i;
            for (unsigned j = 1; j <= m; j++) {
                unsigned a = row[j] + 1, b = next[j - 1] + 1,
                         d = row[j - 1] + (name[i - 1] != candidate[j - 1]);
                next[j] = a < b ? a : b;
                if (d < next[j])
                    next[j] = d;
            }

            memcpy(row, next, (m + 1) * sizeof(row[0]));
        }

        if (row[m] < score) {
            score = row[m];
            best = candidate;
        }
    }

    if (best)
        printf("Did you mean '%s'? See help %s.\n", best, best);
}

/* Match the shell's invocation mode without granting it terminal ownership.
   A script/-c command can run in the background; -i and stdin scripts still
   need the foreground terminal. The kernel enforces access for descendants. */
static bool shell_has_script(int argc, char **argv) {
    bool interactive = false, from_stdin = false;
    for (int i = 1; i < argc; i++) {
        const char *option = argv[i];
        if (!strcmp(option, "--")) {
            return !interactive && !from_stdin && i + 1 < argc && strcmp(argv[i + 1], "-");
        }
        if ((option[0] != '-' && option[0] != '+') || !option[1]) {
            return !interactive && !from_stdin && strcmp(option, "-");
        }
        if (option[1] == '-') {
            return false;
        }
        bool enable = option[0] == '-', command = false, named_option = false;
        for (const char *flag = option + 1; *flag; flag++) {
            if (*flag == 'i')
                interactive = enable;
            if (*flag == 's')
                from_stdin = enable;
            if (*flag == 'c' && enable)
                command = true;
            if (*flag == 'o')
                named_option = true;
        }

        if (command) {
            return !interactive && i + 1 < argc;
        }
        if (named_option && i + 1 < argc) {
            if (!strcmp(argv[i + 1], "interactive"))
                interactive = enable;
            i++;
        }
    }

    return false;
}

static int command(int argc, char **argv) {
    if (!argc)
        return 0;
    const char *name = argv[0];
    long e = 0;
    const struct command_help *entry = lookup_command(name);
    int background = background_request;
    if (background)
        argv[--argc] = NULL;
    if (entry) {
        if (entry->external < 3 && argc == 2 &&
            (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
            help_page(entry);
            return 0;
        }
        if (background && entry->external != 1 && entry->external != 3 &&
            !(entry->external == 5 && shell_has_script(argc, argv)))
            return usage_error(entry, "this command requires the foreground terminal");
        if (argc - 1 < entry->min || argc - 1 > entry->max)
            return usage_error(entry, "incorrect number of arguments");
    }

    if (!strcmp(name, "help"))
        return help(argc > 1 ? argv[1] : NULL);
    if (!strcmp(name, "pwd")) {
        e = call2(SYS_getcwd, cwd, sizeof(cwd));

        if (e < 0) {
            error(name, e);
            return 1;
        }
        puts(cwd);
        return 0;
    }

    if (!strcmp(name, "cd"))
        e = call1(SYS_chdir, argc > 1 ? argv[1] : "/");
    else if (!strcmp(name, "ls")) {
        int hidden = 0, detailed = 0, first = 1;
        for (; first < argc && argv[first][0] == '-'; first++) {
            if (!strcmp(argv[first], "--")) {
                first++;
                break;
            }
            for (char *p = argv[first] + 1; *p; p++) {
                if (*p == 'a')
                    hidden = 1;
                else if (*p == 'l')
                    detailed = 1;
                else
                    return usage_error(entry, "unknown option; supported options are -a and -l");
            }
        }

        int failed = 0;
        if (first == argc)
            failed = ls(".", hidden, detailed);
        for (int i = first; i < argc; i++) {
            if (argc - first > 1)
                printf("%s:\n", argv[i]);
            failed |= ls(argv[i], hidden, detailed);
        }
        return failed;
    } else if (!strcmp(name, "cat")) {
        int failed = 0;
        for (int i = 1; i < argc; i++)
            failed |= cat(argv[i]);
        return failed;
    } else if (!strcmp(name, "cp")) {
        if (argv[1][0] == '-')
            return usage_error(
                entry, "options are not supported; prefix a filename beginning with - with ./");
        e = copy_file(argv[1], argv[2]);
    } else if (!strcmp(name, "mv")) {
        char path[OS_PATH_MAX];
        e = destination(argv[1], argv[2], path);
        if (!e)
            e = rename_noreplace(argv[1], path);
        if (e == -E_XDEV) {
            e = copy_file(argv[1], path);
            if (!e)
                e = call1(SYS_unlink, argv[1]);
        }
    } else if (!strcmp(name, "mkdir") || !strcmp(name, "rmdir") || !strcmp(name, "rm") ||
               !strcmp(name, "touch")) {
        int first = 1, parents = !strcmp(name, "mkdir") && !strcmp(argv[1], "-p"), failed = 0;
        if (parents || !strcmp(argv[1], "--"))
            first++;
        else if (argv[1][0] == '-')
            return usage_error(entry,
                               "unsupported option; use -- before filenames beginning with -");
        if (first == argc)
            return usage_error(entry, "missing path");
        for (int i = first; i < argc; i++) {
            if (!strcmp(name, "mkdir"))
                e = parents ? mkdir_parents(argv[i]) : call2(SYS_mkdir, argv[i], 0755);
            else if (!strcmp(name, "rmdir"))
                e = call1(SYS_rmdir, argv[i]);
            else if (!strcmp(name, "rm"))
                e = call1(SYS_unlink, argv[i]);
            else {
                int fd = open(argv[i], O_CREAT | O_WRONLY, 0644);
                e = fd < 0 ? fd : close(fd);
            }
            if (e < 0) {
                error(argv[i], e);
                failed = 1;
            }
        }

        return failed;
    } else if (!strcmp(name, "write") || !strcmp(name, "append")) {
        int fd = open(argv[1], O_CREAT | O_WRONLY | (!strcmp(name, "append") ? O_APPEND : O_TRUNC),
                      0644);
        if (fd < 0)
            e = fd;
        else {
            for (int i = 2; i < argc && !e; i++) {
                if (i > 2)
                    e = write_all(fd, " ", 1);
                if (!e)
                    e = write_all(fd, argv[i], strlen(argv[i]));
            }

            if (!e)
                e = write_all(fd, "\n", 1);
            if (!e)
                e = fsync(fd);
            close(fd);
        }
    } else if (!strcmp(name, "chmod")) {
        long mode = number(argv[1], 8);
        if (mode < 0 || mode > 0777)
            return usage_error(entry, "mode must be octal digits in the range 0 through 777");
        e = call2(SYS_chmod, argv[2], mode);
    } else if (!strcmp(name, "stat")) {
        struct os_stat st;
        e = stat(argv[1], &st);
        if (!e)
            printf("%s type=%s size=%ld mode=%o device=%lu inode=%lu\n", argv[1],
                   (st.mode & 0170000) == S_IFDIR   ? "directory"
                   : (st.mode & 0170000) == S_IFCHR ? "device"
                                                    : "file",
                   (long)st.size, st.mode & 0777, (unsigned long)st.dev, (unsigned long)st.ino);
    } else if (!strcmp(name, "mount")) {
        if (argc == 1) {
            info(INFO_MOUNTS);
            return 0;
        }
        int i = 1;
        unsigned flags = 0;
        const char *type = "auto";
        while (i < argc && argv[i][0] == '-') {
            if (!strcmp(argv[i], "-t") && i + 1 < argc) {
                type = argv[i + 1];
                i += 2;
            } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
                if (!strcmp(argv[i + 1], "ro"))
                    flags = MS_RDONLY;
                else if (!strcmp(argv[i + 1], "rw"))
                    flags = 0;
                else
                    return usage_error(entry, "mount options must be ro or rw");
                i += 2;
            } else
                return usage_error(entry, "unknown option or missing option value");
        }

        if (argc - i != 2)
            return usage_error(entry, "expected a device name and an existing empty directory");
        e = syscall6(SYS_mount, (long)argv[i], (long)argv[i + 1], (long)type, flags, 0, 0);
        if (!e) {
            puts("Mounted filesystem:");
            info(INFO_MOUNTS);
        }
    } else if (!strcmp(name, "umount"))
        e = call2(SYS_umount2, argv[1], 0);
    else if (!strcmp(name, "ps")) {
        info(INFO_PROCESSES);
        return 0;
    } else if (!strcmp(name, "cpus")) {
        info(INFO_CPUS);
        return 0;
    } else if (!strcmp(name, "disks")) {
        info(INFO_DISKS);
        return 0;
    } else if (!strcmp(name, "lsblk")) {
        info(INFO_LSBLK);
        return 0;
    } else if (!strcmp(name, "netinfo")) {
        info(INFO_NETWORK);
        return 0;
    } else if (!strcmp(name, "mounts")) {
        info(INFO_MOUNTS);
        return 0;
    } else if (!strcmp(name, "sync"))
        e = call0(SYS_sync);
    else if (!strcmp(name, "uname")) {
        if (argc > 1 && strcmp(argv[1], "-a"))
            return usage_error(entry, "only -a is supported");
        struct os_utsname os;
        e = call1(SYS_uname, &os);
        if (!e) {
            if (argc > 1)
                printf("%s %s %s %s %s\n", os.sysname, os.nodename, os.release, os.version,
                       os.machine);
            else
                puts(os.sysname);
        }
    } else if (!strcmp(name, "uptime")) {
        struct os_timespec ts;
        e = call2(SYS_clock_gettime, 1, &ts);
        if (!e)
            printf("up %ld days %ld hours %ld minutes %ld seconds\n", (long)ts.sec / 86400,
                   (long)ts.sec / 3600 % 24, (long)ts.sec / 60 % 60, (long)ts.sec % 60);
    } else if (!strcmp(name, "which")) {
        int failed = 0;
        for (int i = 1; i < argc; i++) {
            char path[OS_PATH_MAX];
            e = program_path(argv[i], path);
            if (!e)
                e = call2(SYS_access, path, 1);
            if (e) {
                error(argv[i], e);
                failed = 1;
            } else
                puts(path);
        }

        return failed;
    } else if (!strcmp(name, "kill")) {
        long signal = 9, pid;
        if (argc == 3) {
            if (argv[1][0] != '-')
                return usage_error(entry, "signal must have the form -NUMBER (for example -15)");
            signal = number(argv[1] + 1, 10);
        }
        pid = number(argv[argc - 1], 10);
        if (pid <= 1 || pid > INT32_MAX || signal < 0 || signal > 64)
            return usage_error(
                entry, "PID must be greater than 1; signal must be an integer from 0 through 64");
        e = call2(SYS_kill, pid, signal);
    } else if (!strcmp(name, "jobs")) {
        reap_jobs();
        int found = 0;
        for (unsigned i = 0; i < 128; i++)
            if (jobs[i].pid) {
                found = 1;
                printf("pid %d %s status=%d\n", jobs[i].pid, jobs[i].finished ? "done" : "running",
                       jobs[i].status);
            }
        if (!found)
            puts("No background jobs.");
        return 0;
    } else if (!strcmp(name, "wait")) {
        int status = 0;
        long pid = argc > 1 ? number(argv[1], 10) : -1;
        if (argc > 1 && (pid <= 1 || pid > INT32_MAX))
            return usage_error(entry, "PID must be a decimal integer greater than 1");
        struct job *job = NULL;
        for (unsigned i = 0; i < 128; i++)
            if (jobs[i].pid && (pid == -1 || jobs[i].pid == pid)) {
                job = &jobs[i];
                break;
            }
        if (job && job->finished) {
            e = job->pid;
            status = job->status;
        } else
            e = waitpid(job ? job->pid : pid, &status, 0);
        if (e > 0) {
            printf("pid %ld status=%d\n", e, status);
            if (job)
                memset(job, 0, sizeof(*job));
            e = 0;
        }
    } else if (!strcmp(name, "sleep")) {
        long seconds = number(argv[1], 10);
        if (seconds < 0 || seconds > 86400)
            return usage_error(entry, "SECONDS must be a decimal integer from 0 through 86400");
        struct os_timespec ts = {seconds, 0};
        e = call2(SYS_nanosleep, &ts, 0);
    } else if (!strcmp(name, "echo")) {
        for (int i = 1; i < argc; i++)
            printf("%s%s", i == 1 ? "" : " ", argv[i]);
        puts("");
        return 0;
    } else if (!strcmp(name, "clear")) {
        write_all(1, "\033[2J\033[H", 7);
        return 0;
    } else if (!strcmp(name, "history")) {
        unsigned start = history_count > 32 ? history_count - 32 : 0;
        for (unsigned i = start; i < history_count; i++)
            printf("%u %s\n", i + 1, history[i % 32]);
        return 0;
    } else if (!strcmp(name, "exit")) {
        if (call0(SYS_getpid) != 1)
            exit(0);
        puts("The system shell remains active. Use the emulator controls to stop.");
        return 0;
    } else {
        int first = !strcmp(name, "run") ? 1 : 0;
        if (argc <= first)
            return usage_error(lookup_command("run"), "missing program name");
        char path[OS_PATH_MAX];
        const char *program = argv[first];
        e = program_path(program, path);
        if (e) {
            error(program, e);
            return 1;
        }
        int pid = spawn(path, argv + first, background);
        if (pid < 0) {
            if (pid == -E_NOENT && !entry) {
                printf("%s: command not found. Type help or which NAME.\n", program);
                suggest(program);
            } else {
                error(program, pid);
                if (pid == -E_NOEXEC)
                    puts(
                        "Programs must be static x86-64 ET_EXEC ELFs using the supported syscall ABI.");
            }

            return 127;
        }

        if (background) {
            remember_job(pid);
            printf("started pid %d\n", pid);
        } else {
            int status;
            int result = waitpid(pid, &status, 0);
            if (result < 0) {
                error("wait", result);
                return 1;
            }
            if (status & 127) {
                printf("pid %d terminated by signal %d\n", pid, status & 127);
                return 128 + (status & 127);
            }
            if (status >> 8) {
                printf("pid %d exited %d\n", pid, status >> 8);
                return status >> 8;
            }
        }

        return 0;
    }

    if (e < 0) {
        error(name, e);
        if (e == -E_ROFS)
            puts("Choose a writable FAT32 mount from mounts, or /tmp for temporary files.");
        if (e == -E_BUSY && !strcmp(name, "umount"))
            puts("Change directory outside this mount and close processes or files using it.");
        if (e == -E_NOENT && !strcmp(name, "mount"))
            puts("Use lsblk for device names; create an empty target directory with mkdir.");
        if ((e == -E_INVAL || e == -E_NOTSUP) && entry)
            help_page(entry);
        return 1;
    }

    return 0;
}

static int parse_error(int code) {
    if (code == -E_2BIG)
        puts(
            "parse: input exceeds the 511-byte console line limit or the argument limit. Use build-sh SCRIPT for long build commands.");
    else
        puts(
            "parse: unmatched quote, unfinished escape, or unsupported shell operator. Close quotes and escape literal operators.");
    return 2;
}

int main(int argc, char **argv) {
    int terminal = argc == 2 && !strcmp(argv[1], "--terminal");
    if (argc > 1 && !terminal) {
        if (argc != 3 || strcmp(argv[1], "-c")) {
            help_page(lookup_command("modernos-sh"));
            return 2;
        }
        char *words[OS_ARG_MAX + 1];
        int n = tokenize(argv[2], words);
        return n < 0 ? parse_error(n) : command(n, words);
    }

    puts("ModernOS userland shell. Type help, help text, or help COMMAND.");

    for (;;) {
        reap_jobs();
        long e = call2(SYS_getcwd, cwd, sizeof(cwd));
        if (e < 0) {
            error("pwd", e);
            strcpy(cwd, "?");
        }

        printf("os:%s$ ", cwd);
        char line[512], *words[OS_ARG_MAX + 1];
        int length = read_line(0, line, sizeof(line));
        if (length < 0) {
            parse_error(length);
            continue;
        }
        if (!length && terminal)
            return 0;
        if (!length || !line[0])
            continue;

        strcpy(history[history_count++ % 32], line);
        int count = tokenize(line, words);
        if (count < 0)
            parse_error(count);
        else
            command(count, words);
    }
}
