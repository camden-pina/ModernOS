/* Authoritative command/help list; define SHELL_COMMAND before including. */
SHELL_COMMAND("help", "shell", "help [COMMAND|CATEGORY]",
                 "Browse commands, syntax, and examples.",
                 "Categories: shell, files, text, disks, system, programs. Use COMMAND --help for its full page.",
                 "help mount", 0, 1, 0)
SHELL_COMMAND("pwd", "files", "pwd", "Print the current working directory.",
                 "Relative paths start here. / is the RAM root filesystem.", "pwd", 0, 0, 0)
SHELL_COMMAND("cd", "files", "cd [PATH|-]", "Change directory (default /).",
                 "Use .. for the parent and - to return to the previous directory. A failed cd preserves the previous directory. Quote names with spaces.", "cd -", 0, 1, 0)
SHELL_COMMAND("ls", "files", "ls [-a] [-l] [PATH...]",
                 "List files; -a includes hidden names, -l shows metadata.",
                 "Options may be combined as -al. With no path, list the current directory. Use -- before a path beginning with -.",
                 "ls -al /bin /mnt", 0, 63, 0)
SHELL_COMMAND("cat", "text", "cat PATH...", "Print file contents.",
                 "For large text files, head and tail show a shorter view.",
                 "cat \"/tmp/my document.txt\"", 1, 63, 0)
SHELL_COMMAND("cp", "files", "cp SOURCE DEST", "Copy one regular file and preserve its mode.",
                 "If DEST is a directory, use the source filename inside it. Existing files are overwritten. Recursive copies are not supported.",
                 "cp /mnt/nvme0n1p1/hello /tmp/hello", 2, 2, 0)
SHELL_COMMAND("mv", "files", "mv SOURCE DEST", "Move or rename a file or directory.",
                 "Existing destinations are refused. Between filesystems, regular files are copied then removed; a read-only source cannot be removed, so use cp.",
                 "mv /tmp/notes.txt /tmp/draft.txt", 2, 2, 0)
SHELL_COMMAND("mkdir", "files", "mkdir [-p] PATH...",
                 "Create directories; -p creates missing parents.",
                 "Without -p, the parent must exist. Permissions start at 0755.",
                 "mkdir -p /tmp/documents/drafts", 1, 63, 0)
SHELL_COMMAND("rmdir", "files", "rmdir PATH...", "Remove empty directories.",
                 "Directories used by a process or mounted filesystem remain busy.",
                 "rmdir /tmp/empty", 1, 63, 0)
SHELL_COMMAND("rm", "files", "rm [--] PATH...", "Remove regular files.",
                 "Directories need rmdir. Open files remain busy. Use -- for filenames beginning with -.",
                 "rm /tmp/old.txt", 1, 63, 0)
SHELL_COMMAND("touch", "files", "touch PATH...",
                 "Create empty files without truncating existing ones.",
                 "This console builtin only creates missing files. Use build-sh or busybox touch for real timestamp updates.",
                 "touch /tmp/notes.txt", 1, 63, 0)
SHELL_COMMAND("write", "text", "write PATH TEXT...", "Replace a file with one line of text.",
                 "Words are joined by spaces with a final newline. Existing contents are replaced; edit supports longer documents.",
                 "write /tmp/notes.txt \"First draft\"", 2, 63, 0)
SHELL_COMMAND("append", "text", "append PATH TEXT...", "Add a line to the end of a file.",
                 "Creates the file if missing. Existing bytes are preserved.",
                 "append /tmp/notes.txt \"Second paragraph\"", 2, 63, 0)
SHELL_COMMAND("edit", "text", "edit [-r] FILE",
                 "Open the document editor; -r opens a read-only view.",
                 "Arrows, Home/End, Page Up/Down, Delete, Enter, and Backspace edit text. Ctrl-S saves; Ctrl-Q quits with confirmation. Saves keep FILE.bak. Limit: 256 KiB ASCII text.",
                 "edit \"/tmp/my document.txt\"", 1, 2, 2)
SHELL_COMMAND("less", "text", "less [OPTIONS] [FILE...]", "Browse files or standard input one screen at a time.",
                 "Space/Page Down advances; b/Page Up goes back; g/G jumps to the start/end; /pattern searches; n repeats; q quits. -N shows line numbers; -S truncates long lines. Input is limited to 100000 lines.",
                 "less -N /tmp/notes.txt", 0, 63, 4)
SHELL_COMMAND("nl", "text", "nl [OPTIONS] [FILE...]", "Add line numbers to text.",
                 "Defaults to numbering nonempty lines. -ba numbers every line; -s sets a separator; -w sets number width.",
                 "nl -ba /tmp/notes.txt", 0, 63, 3)
SHELL_COMMAND("tac", "text", "tac [FILE...]", "Print input lines in reverse order.",
                 "Reads files or standard input. Input is buffered in memory; choose a bounded input for large logs.",
                 "tac /tmp/notes.txt", 0, 63, 3)
SHELL_COMMAND("rev", "text", "rev [FILE...]", "Reverse the bytes on each input line.",
                 "Reads files or standard input and preserves line endings. This build is byte-oriented.",
                 "rev /tmp/notes.txt", 0, 63, 3)
SHELL_COMMAND("paste", "text", "paste [-d LIST] [-s] [FILE...]", "Join corresponding lines from text files.",
                 "The default separator is a tab. -d chooses delimiters; -s combines lines from one file at a time. Use - for standard input.",
                 "paste -d : /tmp/names /tmp/values", 0, 63, 3)
SHELL_COMMAND("comm", "text", "comm [-123] FILE1 FILE2", "Compare two sorted text files.",
                 "Output columns are lines unique to FILE1, unique to FILE2, and common to both. -1/-2/-3 suppress columns; -12 prints common lines.",
                 "comm -12 /tmp/list-a /tmp/list-b", 0, 63, 3)
SHELL_COMMAND("fold", "text", "fold [-bs] [-w WIDTH] [FILE...]", "Wrap long text lines to a chosen width.",
                 "The default is 80 columns. -s breaks at spaces; -b counts bytes. Reads standard input when no file is given.",
                 "fold -s -w 72 /tmp/notes.txt", 0, 63, 3)
SHELL_COMMAND("base64", "text", "base64 [-d] [-w COL] [FILE]", "Encode or decode binary data as Base64.",
                 "-d decodes. -w sets output line width; -w 0 disables wrapping. Use build-sh for redirection to a file.",
                 "base64 /tmp/data.bin", 0, 63, 3)
SHELL_COMMAND("split", "files", "split [OPTIONS] [INPUT [PREFIX]]", "Split a file into smaller pieces.",
                 "-b N splits by bytes; -l N splits by lines. Output names use PREFIX plus letter suffixes. Existing matching names are overwritten; choose a fresh prefix.",
                 "split -b 1m /tmp/archive /tmp/part.", 0, 63, 3)
SHELL_COMMAND("nano", "text", "nano [OPTIONS] [FILE...]", "Run native upstream GNU nano 8.7.",
                 "Works on the console and in the desktop Terminal through a real PTY. Ctrl-O writes, Ctrl-X exits, Ctrl-W searches. Use nano --help for upstream options. For shell commands use env SHELL=/bin/ash nano FILE. Process suspension/job control remains unsupported.",
                 "nano /mnt/nvme1n1p1/notes.txt", 0, 63, 4)
SHELL_COMMAND("vim", "text", "vim [OPTIONS] [FILE...]", "Run native upstream Vim 9.2.",
                 "i inserts text; Escape returns to normal mode; :w saves, :q quits, :wq does both. :help opens the bundled upstream manual. Syntax highlighting, undo, search and shell filters are supported. Job control and terminal-job windows are unavailable.",
                 "vim /tmp/notes.txt", 0, 63, 4)
SHELL_COMMAND("ash", "shell", "ash [SCRIPT [ARGS...] | -c COMMAND]",
                 "Run the upstream BusyBox POSIX shell.",
                 "Provides pipes, redirects, variables, globbing and scripts. Job control is disabled. Use build-sh to select full upstream text tools and configure-script defaults.",
                 "ash -c 'printf b\\n | sort'", 0, 63, 5)
SHELL_COMMAND("build-sh", "programs", "build-sh [SCRIPT [ARGS...] | -c COMMAND]",
                 "Open the native build environment.",
                 "Runs real ash with PATH=/usr/bin:/bin and SHELL/CONFIG_SHELL=/bin/ash. Native GCC/G++, binutils, musl development headers and static libraries are installed. Full text tools override the basic console tools.",
                 "build-sh -c 'gcc -O2 /tmp/hello.c -o /tmp/hello && /tmp/hello'",
                 0, OS_ARG_MAX - 1, 5)
SHELL_COMMAND("busybox", "programs", "busybox [APPLET [ARGS...]]",
                 "Run or list the upstream utility suite.",
                 "busybox --list lists compiled applets. busybox APPLET --help prints real options. Utilities act on the actual filesystem; unavailable OS features return errors.",
                 "busybox grep -En '^[A-Z]+' /tmp/notes.txt", 0, 63, 3)
SHELL_COMMAND("make", "programs", "make [OPTIONS] [TARGET...]",
                 "Run native upstream GNU make 4.4.1.",
                 "Reads actual makefiles, executes recipes with /bin/ash, uses real timestamps and supports -j parallel builds through a pipe jobserver. Use build-sh for the compiler and full text utilities.",
                 "make -j2", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("gcc", "programs", "gcc [OPTIONS] SOURCE... [-o OUTPUT]",
                 "Compile and link native C with GCC 14.2.0.",
                 "-c compiles an object; -O2 optimizes; -Wall enables warnings. Defaults to static x86-64 musl executables using actual Coast syscalls. Use gcc-dynamic for musl PIE/shared C libraries; C pthreads use the existing musl/kernel thread ABI. Compiler diagnostics identify invalid options and source errors.",
                 "gcc -O2 -Wall /tmp/hello.c -o /tmp/hello", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("gcc-dynamic", "programs", "gcc-dynamic [OPTIONS] SOURCE... [-o OUTPUT]",
                 "Compile native PIE and shared C libraries with musl.",
                 "Uses the installed GCC and /lib/ld-musl-x86_64.so.1. Add -fPIC -shared for a library; use -Wl,-rpath for dependency search. Ordinary gcc stays static. Examples: /usr/src/coast/dynamic.",
                 "gcc-dynamic /tmp/hello.c -o /tmp/hello", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("ldd", "programs", "ldd PROGRAM", "List actual musl ELF dependencies.",
                 "The musl loader resolves the program's DT_NEEDED entries using its environment, RUNPATH and system paths. Missing libraries report errors.",
                 "ldd /tmp/hello", 1, 1, 3)
SHELL_COMMAND("dynamic-driver-check", "programs", "dynamic-driver-check",
                 "Check real dynamic ELF execution and protections.",
                 "Runs PIE and dynamic ET_EXEC fixtures, transitive dependencies, dlopen/dlsym, TLS/pthreads, malformed-image errors, RELRO, NX and W^X probes.",
                 "dynamic-driver-check", 0, 0, 1)
SHELL_COMMAND("g++", "programs", "g++ [OPTIONS] SOURCE... [-o OUTPUT]",
                 "Compile and link native C++ with GCC 14.2.0.",
                 "Includes C++ headers, static libstdc++ and exceptions. Use g++ to link C++ objects. This SDK installs a static C++ runtime; gcc-dynamic supplies the shared C profile.",
                 "g++ -std=c++17 /tmp/hello.cpp -o /tmp/hello", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("cc", "programs", "cc [OPTIONS] SOURCE... [-o OUTPUT]",
                 "Run the native GCC C compiler.",
                 "An executable alias of GCC. See help gcc for the target's supported runtime.",
                 "cc /tmp/hello.c -o /tmp/hello", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("c++", "programs", "c++ [OPTIONS] SOURCE... [-o OUTPUT]",
                 "Run the native GCC C++ compiler.",
                 "An executable alias of G++. See help g++ for the target's supported runtime.",
                 "c++ /tmp/hello.cpp -o /tmp/hello", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("cpp", "programs", "cpp [OPTIONS] [SOURCE [OUTPUT]]",
                 "Run the real C preprocessor.",
                 "Use -I for include directories and -D for definitions. GCC also supports -E preprocessing.",
                 "cpp /tmp/hello.c", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("as", "programs", "as [OPTIONS] SOURCE... -o OBJECT",
                 "Assemble native x86-64 instructions.",
                 "GNU assembler 2.37. Writes real relocatable ELF objects for the native linker.",
                 "as /tmp/start.s -o /tmp/start.o", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("ld", "programs", "ld [OPTIONS] OBJECT... -o OUTPUT",
                 "Link actual native ELF objects.",
                 "GNU linker 2.37. Prefer gcc/g++ for static programs or gcc-dynamic for musl PIE/shared C libraries, with the appropriate startup objects and runtime. Load segments must obey W^X.",
                 "ld --help", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("ar", "programs", "ar [OPTIONS] ARCHIVE [OBJECT...]",
                 "Create or inspect real object archives.",
                 "rcs creates/updates an indexed library; t lists members; x extracts. Use GCC/g++ to link the archive.",
                 "ar rcs /tmp/libsample.a /tmp/sample.o", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("ranlib", "programs", "ranlib [OPTIONS] ARCHIVE...",
                 "Index static object libraries.",
                 "Updates the archive symbol index using native GNU binutils.",
                 "ranlib /tmp/libsample.a", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("nm", "programs", "nm [OPTIONS] OBJECT...", "Inspect ELF symbols.",
                 "Use -u for undefined symbols and -C to demangle C++ names.", "nm -C /tmp/hello",
                 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("readelf", "programs", "readelf [OPTIONS] ELF...",
                 "Inspect actual ELF headers and sections.",
                 "-h shows the header; -l shows load segments; -s shows symbols.",
                 "readelf -hl /tmp/hello", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("objdump", "programs", "objdump [OPTIONS] OBJECT...",
                 "Inspect/disassemble native objects.",
                 "-d disassembles executable sections; -h lists section headers.",
                 "objdump -d /tmp/hello", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("objcopy", "programs", "objcopy [OPTIONS] INPUT [OUTPUT]",
                 "Copy or transform real object files.",
                 "Supports format conversion and section/symbol options. Failed operations report errors.",
                 "objcopy --help", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("size", "programs", "size [OPTIONS] OBJECT...", "Report executable section sizes.",
                 "GNU binutils calculates text, data and BSS sizes from the actual file.",
                 "size /tmp/hello", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("strings", "programs", "strings [OPTIONS] FILE...",
                 "Find printable strings in real files.",
                 "-n LENGTH sets the minimum length; -a scans all data.", "strings /tmp/hello",
                 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("strip", "programs", "strip [OPTIONS] OBJECT...",
                 "Remove debug data or symbols from binaries.",
                 "Modifies the specified files. --strip-debug retains ordinary symbols.",
                 "strip --strip-debug /tmp/hello", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("bootstrap-toolchain",
                 "programs", "bootstrap-toolchain [--work DIR] [--prefix DIR] [-j JOBS] [--prepare-only]",
                 "Rebuild the native GCC/binutils toolchain from bundled sources.",
                 "Uses actual native compilers and make, with verified sources under /usr/src/coast. Build in a RAM workspace; 8 GiB memory is recommended. Installs a separate toolchain and runs C/C++ proofs. Use build-sh to invoke this script.",
                 "build-sh /usr/bin/bootstrap-toolchain -j4", 0, 7, 3)
SHELL_COMMAND("compiler-check", "programs", "compiler-check [--metadata | --directory]",
                 "Check compiler-facing runtime interfaces.",
                 "Tests large argv/environments, file mappings, CPU accounting, executable scripts and descriptors. --directory checks 8192 actual files, lookup, enumeration and rename. --metadata additionally creates and removes 327680 real files to check heap growth; allow time and sufficient RAM.",
                 "compiler-check", 0, 1, 1)
SHELL_COMMAND("build-check",
                 "programs", "build-check [--files DIRECTORY | --registers | --suspend | --faults]", "Validate process and build-tool syscalls.", "Checks fork/TLS/guards, SIGCHLD, timers, signal return, waits, vfork ordering, shared offsets, posix_spawn, and errors. --registers checks asynchronous RCX/R11 preservation; --suspend checks signal wait masks; --faults checks hardware fault delivery and metadata. --files checks resize, zero fill, timestamps and symlink capabilities on a writable directory.",
                 "build-check --files /tmp", 0, 2, 1)
SHELL_COMMAND("sed", "text", "sed [OPTIONS] SCRIPT [FILE...]",
                 "Transform lines with upstream sed.",
                 "Supports standard input and regular expressions. In-place replacement can fail where the filesystem cannot replace an existing rename target.",
                 "build-sh -c 'printf hello | sed s/hello/world/'", 0, 63, 3)
SHELL_COMMAND("awk", "text", "awk [OPTIONS] PROGRAM [FILE...]",
                 "Process fields and records with GNU awk.",
                 "Upstream GNU awk supports standard input, variables, arithmetic, regular expressions and GCC build generators. Dynamic extensions, persistent-memory allocation and arbitrary precision are disabled.",
                 "build-sh -c 'printf \"a 2\\\\n\" | awk \"{print \\\\$2}\"'", 0, OS_ARG_MAX - 1,
                 3)
SHELL_COMMAND("gawk", "text", "gawk [OPTIONS] PROGRAM [FILE...]",
                 "Run upstream GNU awk explicitly.",
                 "The awk and gawk commands use the same native GNU awk implementation. Use gawk --help for actual supported options.",
                 "gawk --version", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("sort", "text", "sort [OPTIONS] [FILE...]",
                 "Sort lines from files or standard input.",
                 "Use -n for numeric order, -r for reverse, -u for unique values.",
                 "build-sh -c 'printf \"b\\\\na\\\\n\" | sort'", 0, 63, 3)
SHELL_COMMAND("uniq", "text", "uniq [OPTIONS] [INPUT [OUTPUT]]",
                 "Remove adjacent duplicate lines.",
                 "Sort first when duplicates are not already adjacent.",
                 "build-sh -c 'sort /tmp/names.txt | uniq -c'", 0, 63, 3)
SHELL_COMMAND("tr", "text", "tr [OPTIONS] SET1 [SET2]",
                 "Translate or delete bytes from standard input.",
                 "Supports character classes, -d deletion and -s squeezing.",
                 "build-sh -c 'printf abc | tr a-z A-Z'", 0, 63, 3)
SHELL_COMMAND("cut", "text", "cut [OPTIONS] [FILE...]", "Select fields or byte ranges.",
                 "Use -d DELIMITER with -f FIELDS, or -b BYTES.", "cut -d : -f 1 /tmp/records.txt",
                 0, 63, 3)
SHELL_COMMAND("diff", "text", "diff [OPTIONS] FILE1 FILE2", "Compare actual file contents.",
                 "-u emits a unified patch; -r compares directories. Exit status 1 means differences.",
                 "diff -u /tmp/old.c /tmp/new.c", 0, 63, 3)
SHELL_COMMAND("patch", "text", "patch [OPTIONS] [ORIGINAL [PATCHFILE]]",
                 "Apply a patch to a source file.",
                 "Use -pN to strip leading path components. Back up valuable files before patching.",
                 "build-sh -c 'patch -p0 < /tmp/change.patch'", 0, 63, 3)
SHELL_COMMAND("tar", "files", "tar -c|-x|-t [-f ARCHIVE] [OPTIONS] [FILE...]",
                 "Create, extract or list upstream tar archives.",
                 "gzip/bzip2/xz decompression is available. Extract with --no-same-owner because ownership changes are unsupported. Requires a writable destination.",
                 "tar -xf /tmp/source.tar --no-same-owner", 0, 63, 3)
SHELL_COMMAND("gzip", "files", "gzip [OPTIONS] [FILE...]", "Compress files with gzip.",
                 "-d decompresses; -c writes to standard output and preserves the source.",
                 "build-sh -c 'gzip -c /tmp/notes.txt > /tmp/notes.gz'", 0, 63, 3)
SHELL_COMMAND("gunzip", "files", "gunzip [OPTIONS] [FILE...]", "Decompress gzip streams.",
                 "-c writes to standard output without deleting the archive.",
                 "gunzip -c /tmp/notes.gz", 0, 63, 3)
SHELL_COMMAND("bzip2", "files", "bzip2 [OPTIONS] [FILE...]",
                 "Compress or decompress bzip2 data.",
                 "Use -c to retain the input file and -d to decompress.",
                 "build-sh -c 'bzip2 -c /tmp/notes.txt > /tmp/notes.bz2'", 0, 63, 3)
SHELL_COMMAND("xzcat", "files", "xzcat [FILE...]", "Decompress xz data to standard output.",
                 "The port supplies xz decompression; an xz encoder is not enabled.",
                 "xzcat /tmp/source.tar.xz", 0, 63, 3)
SHELL_COMMAND("ln", "files", "ln [OPTIONS] TARGET LINK", "Create a real symbolic link with -s.",
                 "RAM supports symlinks, FAT32 does not; hard links are unsupported. Errors describe actual filesystem capability.",
                 "ln -s /tmp/notes.txt /tmp/notes-link", 0, 63, 3)
SHELL_COMMAND("readlink", "files", "readlink [-f] PATH",
                 "Read a real symlink or resolve a path.",
                 "Relative targets are resolved against their containing directory.",
                 "readlink /tmp/notes-link", 0, 63, 3)
SHELL_COMMAND("realpath", "files", "realpath PATH...", "Resolve canonical filesystem paths.",
                 "Requires actual resolvable entries.", "realpath /tmp/../bin/vim", 0, 63, 3)
SHELL_COMMAND("mktemp", "files", "mktemp [OPTIONS] [TEMPLATE]",
                 "Create a unique temporary file or directory.",
                 "Use -d for a directory. Files are created with exclusive checks.",
                 "mktemp /tmp/build.XXXXXX", 0, 63, 3)
SHELL_COMMAND("install", "files", "install [OPTIONS] SOURCE DEST",
                 "Copy files with installation permissions.",
                 "Use -d to create directories; unsupported ownership requests report errors.",
                 "install -m 755 /bin/hello /tmp/hello", 0, 63, 3)
SHELL_COMMAND("du", "files", "du [OPTIONS] [PATH...]",
                 "Estimate directory space from filesystem metadata.",
                 "Use -s for a total. Results reflect the backend stat data.",
                 "du -s /usr/share/vim", 0, 63, 3)
SHELL_COMMAND("date", "system", "date [OPTIONS] [+FORMAT]",
                 "Format the actual OS realtime clock.",
                 "The firmware-derived clock is UTC. Setting the system clock is unsupported.",
                 "date -u +%Y-%m-%d", 0, 63, 3)
SHELL_COMMAND("sha256sum", "text", "sha256sum [OPTIONS] [FILE...]",
                 "Hash actual file bytes with SHA-256.", "Use -c to check a saved checksum list.",
                 "sha256sum /bin/vim", 0, 63, 3)
SHELL_COMMAND("md5sum", "text", "md5sum [OPTIONS] [FILE...]",
                 "Hash files with MD5 for compatibility checks.",
                 "Use -c to verify a saved checksum list.", "md5sum /tmp/notes.txt", 0, 63, 3)
SHELL_COMMAND("xargs", "shell", "xargs [OPTIONS] [COMMAND [ARGS...]]",
                 "Build command arguments from standard input.",
                 "Use -0 with find -print0; -n limits each invocation. The kernel accepts 64 argv entries.",
                 "build-sh -c 'find /tmp -type f -print0 | xargs -0 -n 1 wc -c'", 0, 63, 3)
SHELL_COMMAND("getopt", "shell", "getopt [OPTIONS] -- ARGUMENTS...",
                 "Parse script options with upstream getopt.",
                 "Supports short and long options for native ash scripts.",
                 "getopt -o ab: -- -a -b value", 0, 63, 3)
SHELL_COMMAND("seq", "text", "seq [FIRST [INCREMENT]] LAST", "Generate a sequence of numbers.",
                 "Output can feed real shell pipelines.", "seq 1 5", 0, 63, 3)
SHELL_COMMAND("printf", "text", "printf FORMAT [ARGUMENT...]",
                 "Print formatted text and escaped characters.",
                 "For redirects and pipelines use ash/build-sh.", "printf \"hello\\\\n\"", 0, 63,
                 3)
SHELL_COMMAND("env", "programs", "env [-i] [NAME=VALUE...] [PROGRAM [ARGS...]]",
                 "Inspect or change a child's environment.",
                 "Variables are passed on the real ELF initial stack. -i clears inherited values. Bare program names search PATH; the kernel accepts at most 64 variables.",
                 "env TERM=vt100 nano /tmp/notes.txt", 0, 63, 2)
SHELL_COMMAND("stty", "system", "stty [-a|size|raw|cooked|echo|-echo]",
                 "Inspect or configure terminal input.",
                 "size reports the actual terminal geometry. Only implemented line discipline options are accepted.",
                 "stty size", 0, 1, 2)
SHELL_COMMAND("tty", "system", "tty", "Identify the current terminal.",
                 "Reports /dev/tty or a descriptor-based pseudoterminal session. PTY device paths are not exposed.",
                 "tty", 0, 0, 2)
SHELL_COMMAND("reset", "system", "reset", "Restore normal terminal input and clear the screen.",
                 "Restores canonical input, echo and signal keys through actual terminal ioctls.",
                 "reset", 0, 0, 2)
SHELL_COMMAND("head", "text", "head [-n LINES] FILE", "Print the first lines (default 10).",
                 "LINES is a nonnegative decimal count.", "head -n 5 /tmp/notes.txt", 1, 3, 1)
SHELL_COMMAND("tail", "text", "tail [-n LINES] FILE", "Print the last lines (default 10).",
                 "Reads a seekable regular file without loading the whole file.",
                 "tail -n 5 /tmp/notes.txt", 1, 3, 1)
SHELL_COMMAND("wc", "text", "wc FILE...", "Count lines, words, and bytes.",
                 "Lines count newline bytes. Words are separated by ASCII whitespace.",
                 "wc /tmp/notes.txt", 1, 63, 1)
SHELL_COMMAND("grep", "text", "grep [-n] PATTERN FILE...", "Find lines containing literal text.",
                 "-n prints line numbers. Matching is case-sensitive; regular expressions are not supported.",
                 "grep -n draft /tmp/notes.txt", 2, 63, 1)
SHELL_COMMAND("find", "files", "find [PATH]", "List a directory tree.",
                 "Defaults to the current directory. Does not follow directory symlinks. Recursion is bounded to 16 levels.",
                 "find /tmp/documents", 0, 1, 1)
SHELL_COMMAND("hexdump", "text", "hexdump FILE",
                 "Inspect bytes as hexadecimal and printable text.",
                 "Shows 16 bytes per line with hexadecimal offsets.", "hexdump /bin/hello", 1, 1,
                 1)
SHELL_COMMAND("chmod", "files", "chmod OCTAL PATH", "Set file permissions.",
                 "OCTAL must be 0 through 7777 using only digits 0-7. Includes set-user-ID, set-group-ID and sticky bits; kernel ownership checks apply.",
                 "chmod 755 /tmp/hello", 2, 2, 0)
SHELL_COMMAND("stat", "files", "stat PATH", "Show file type, size, mode, device, and inode.",
                 "Symlinks are followed. Unix ownership is unavailable on FAT32; its stored modification time has two-second precision.",
                 "stat /bin/sh", 1, 1, 0)
SHELL_COMMAND("lsblk", "disks", "lsblk", "List drives, partitions, sizes, and mount locations.",
                 "Device identifiers are accepted by mount. Raw disk read/write nodes are not exposed.",
                 "lsblk", 0, 0, 0)
SHELL_COMMAND("disks", "disks", "disks", "Show device sector counts and sector sizes.",
                 "NVMe and AHCI SATA discovery occurs at boot. Disk hotplug is not supported.",
                 "disks", 0, 0, 0)
SHELL_COMMAND("mount", "disks", "mount [-t TYPE] [-o ro|rw] DEVICE DIRECTORY",
                 "Mount a filesystem, or list mounts with no arguments.",
                 "TYPE: auto, fat32, ext, ext2, ext4. DIRECTORY must exist and be empty. DEVICE is a name from lsblk, optionally /dev/NAME. A device can be mounted once. ext filesystems are always read-only.",
                 "mount -o ro nvme1n1p1 /mnt/data", 0, 6, 0)
SHELL_COMMAND("umount", "disks", "umount DIRECTORY", "Flush and unmount a disk filesystem.",
                 "First cd outside the mount and close programs using it. Busy mounts and / are refused. Forced and lazy unmount are not supported.",
                 "umount /mnt/nvme1n1p1", 1, 1, 0)
SHELL_COMMAND("mounts", "disks", "mounts",
                 "List mount paths, filesystem types, and access modes.",
                 "RAM files disappear on reboot. Save persistent documents to a writable FAT32 mount.",
                 "mounts", 0, 0, 0)
SHELL_COMMAND("sync", "disks", "sync", "Flush filesystem metadata and disk write caches.",
                 "Editor saves and file copies also flush writes. FAT32 does not provide power-loss atomicity.",
                 "sync", 0, 0, 0)
SHELL_COMMAND("ps", "system", "ps", "List processes and kernel tasks.",
                 "Shows PID, parent PID, CPU, scheduling state, runtime ticks, and program name.",
                 "ps",
                 0, 0,
                 0)
SHELL_COMMAND("cpus", "system", "cpus", "Show online CPUs and scheduler counters.",
                 "Tasks are preemptive and pinned to the CPU selected at creation.", "cpus", 0, 0,
                 0)
SHELL_COMMAND("acpi", "system", "acpi", "Show cached ACPI runtime and power-device status.",
                 "Reports actual firmware, adapters, batteries, stale data and platform limitations. Reading the cache does not evaluate AML.",
                 "acpi", 0, 0, 0)
SHELL_COMMAND("battery", "system", "battery", "Show the cached ACPI adapter and battery report.",
                 "Uses the same snapshot as acpi and System & Power. Missing devices, unknown values and unsupported firmware methods remain explicit.",
                 "battery", 0, 0, 0)
SHELL_COMMAND("power", "system", "power [cancel]", "Show coordinated system-power status.",
                 "Includes backend readiness, request ID, stage and last error. cancel asks to stop a pending reversible request; a late cancellation can fail.",
                 "power cancel", 0, 1, 0)
SHELL_COMMAND("poweroff", "system", "poweroff", "Request coordinated system shutdown.",
                 "Save application work first. An accepted asynchronous request is still pending; it is not proof the machine powered off. Use power to inspect progress or failure.",
                 "poweroff", 0, 0, 0)
SHELL_COMMAND("reboot", "system", "reboot", "Request a coordinated system restart.",
                 "Save application work first. An accepted asynchronous request is still pending; it is not proof the machine restarted. Use power to inspect progress or failure. reset only resets terminal settings.",
                 "reboot", 0, 0, 0)
SHELL_COMMAND("uname", "system", "uname [-a]",
                 "Print the OS name; -a includes version and architecture.",
                 "This OS provides a growing Linux-style syscall subset, not complete Linux binary compatibility.",
                 "uname -a", 0, 1, 0)
SHELL_COMMAND("uptime", "system", "uptime", "Print elapsed time since boot.",
                 "Uses the monotonic clock, independent of wall-clock time.", "uptime", 0, 0, 0)
SHELL_COMMAND("doom", "programs", "doom [--info] [--trace] [--frames N] [ENGINE OPTIONS]",
                 "Play Doomgeneric with the complete Freedoom Phase 2 campaign.",
                 "The desktop Terminal opens a managed software window; use --console for exclusive console mode. WASD moves/strafe; arrows or mouse turn; Ctrl/mouse1 fires; E/Space uses; Shift runs. Escape opens the menu; Ctrl+Alt+Escape quits. Native stereo sound effects and procedural MUS/MIDI music are supported. Saves/config: /home/user/.doom (RAM). Use doom --help for options.",
                 "doom -warp 1 -skill 2", 0, OS_ARG_MAX - 1, 4)
SHELL_COMMAND("gui", "programs",
                 "gui [--info] [--stats] [--trace] [--browser-network-trace] [--browser-exception-trace]",
                 "Open the Coast desktop.",
                 "Files, Terminal, Browser, Text Editor and Settings use shared windows and real workspaces. Super opens the launcher; Ctrl+Alt+Escape returns safely to the console. --info reports display capabilities; --stats reports damage traffic on exit; --trace logs desktop actions. Browser trace flags enable explicit developer diagnostics.",
                 "gui", 0, 5, 2)
SHELL_COMMAND("git", "programs", "git COMMAND [OPTIONS]", "Manage real Git repositories.",
                 "Upstream Git supports local repositories and smart HTTP/HTTPS clone, fetch and push through the actual network backend. Configure your user.name and user.email before committing. Work in RAM; FAT lacks the replacement operations Git needs. SSH needs an SSH client, which is not installed. Run git COMMAND -h for options.",
                 "git init /home/user/Projects/repository", 1, OS_ARG_MAX - 1, 2)
SHELL_COMMAND("netinfo", "system", "netinfo",
                 "Show actual network link, DHCP address and packet counters.",
                 "IPv4 TCP/UDP and loopback are supported. The Intel e1000/e1000e driver supports the QEMU adapter. IPv6, Wi-Fi and unsupported NICs remain unavailable.",
                 "netinfo", 0, 0, 0)
SHELL_COMMAND("curl", "programs", "curl [OPTIONS] URL...",
                 "Transfer actual HTTP/HTTPS resources.",
                 "Uses the kernel's IPv4/DNS backend, OpenSSL and the bundled CA certificates. -I requests headers; -o FILE saves a response. Certificates and hostname are verified by default. Use curl --help for options.",
                 "curl -I https://www.qemu.org/", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("openssl", "programs", "openssl COMMAND [OPTIONS]",
                 "Use the native OpenSSL cryptographic tools.",
                 "Supports hashing, certificates, key generation and TLS using hardware-backed getrandom entropy. Use openssl help for the available commands.",
                 "openssl version", 0, OS_ARG_MAX - 1, 3)
SHELL_COMMAND("run", "programs", "run PROGRAM [ARGS...] [&]",
                 "Start a native static ELF program or executable script.",
                 "Bare names search /bin; paths run directly. Scripts require an absolute #! interpreter. & starts a background task. Ctrl-C terminates a foreground child. Supported ELF/syscall formats are required.",
                 "run hello example", 1, OS_ARG_MAX - 1, 1)
SHELL_COMMAND("which", "programs", "which PROGRAM...", "Locate executable programs.",
                 "Bare names search /bin. Paths containing / are checked directly.",
                 "which edit hello", 1, 63, 0)
SHELL_COMMAND("jobs", "shell", "jobs", "Show background jobs started by this shell.",
                 "Completed statuses stay available until wait consumes them.", "jobs", 0, 0, 0)
SHELL_COMMAND("wait", "shell", "wait [PID]",
                 "Wait for one background child and report its status.",
                 "Without PID, wait for any tracked job. PID must be greater than 1. Status is the raw wait status: exit code shifted by 8, or termination signal.",
                 "wait 12", 0, 1, 0)
SHELL_COMMAND("kill", "system", "kill [-SIGNAL] PID",
                 "Send a signal to a process (default signal 9).",
                 "Signal handlers, masks and return are supported. PID 1 is protected. Process groups, job suspension and stop signals are not implemented.",
                 "kill -15 12", 1, 2, 0)
SHELL_COMMAND("sleep", "system", "sleep SECONDS", "Pause for 0 through 86400 whole seconds.",
                 "SECONDS must be a nonnegative decimal integer.", "sleep 1", 1, 1, 0)
SHELL_COMMAND("echo", "shell", "echo [TEXT...]", "Print arguments separated by spaces.",
                 "Use single or double quotes to group words.", "echo \"Hello world\"", 0, 63, 0)
SHELL_COMMAND("clear", "shell", "clear", "Clear the terminal screen.",
                 "Uses ANSI escape sequences on the framebuffer and serial terminal.", "clear", 0,
                 0, 0)
SHELL_COMMAND("history", "shell", "history", "Show the last 32 interactive command lines.",
                 "History is kept in RAM by this shell; expansion with ! is not supported.",
                 "history", 0, 0, 0)
SHELL_COMMAND("exit", "shell", "exit", "Describe how to stop the system shell.",
                 "PID 1 stays available. An ordinary child sh -c COMMAND exits with the command status. Stop the VM through its controls.",
                 "exit", 0, 0, 0)
SHELL_COMMAND("sh", "shell", "sh [SCRIPT [ARGS...] | -c COMMAND]",
                 "Run the actual BusyBox POSIX shell.",
                 "Supports scripts, pipes, redirects, loops and variables. coast-sh provides the desktop's OS command shell. Use build-sh to select full build utilities.",
                 "sh -c 'echo hello | sort'", 0, OS_ARG_MAX - 1, 5)
SHELL_COMMAND("coast-sh", "shell", "coast-sh [--terminal|-c COMMAND]",
                 "Open the Coast command console.",
                 "Provides help, disk and desktop commands. --terminal uses a real pseudoterminal. Child shells may exit; PID 1 stays available. Use build-sh or sh for POSIX scripts and redirection.",
                 "coast-sh -c 'help gcc'", 0, 2, 1)
SHELL_COMMAND("hello", "programs", "hello [ARGS...]",
                 "Check ring-3 execution and argument passing.",
                 "A small native static executable.", "hello example", 0, 63, 1)
SHELL_COMMAND("check", "programs", "check [--tty-read|--tty-denied|--display-exit|--display-fault|--storage MOUNT]", "Run kernel ABI and isolation checks.", "Tests pointer validation, files, memory, scheduling, terminal and display access. --display-fault intentionally faults after acquiring the display; --storage checks busy unmount behavior.",
                 "check", 0, 2, 1)
SHELL_COMMAND("spin", "programs", "spin [ITERATIONS] [&]",
                 "Exercise scheduler preemption with a busy task.",
                 "Use kill and wait to stop and reap a background spin.", "spin 4000000000 &", 0, 1,
                 1)
SHELL_COMMAND("fault", "programs", "fault", "Exercise recovery from a user process fault.",
                 "Intentionally triggers a user fault. The system shell survives.", "fault", 0, 0,
                 1)
SHELL_COMMAND("exec-test", "programs", "exec-test", "Check executable image replacement.",
                 "Executes hello using execve.", "exec-test", 0, 0, 1)
SHELL_COMMAND("posix-check", "programs", "posix-check", "Check the native static-musl runtime.",
                 "Tests TLS, allocation, memory guards, signal handlers, poll/select, and real pseudoterminal input/output and resizing.",
                 "posix-check", 0, 0, 1)
