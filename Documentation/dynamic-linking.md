# ELF dynamic linking

Coast runs the existing musl 1.2.5 userspace ABI with an optional shared
libc and its upstream dynamic loader. Existing programs and the native GCC
default remain static. The shared runtime is built from the same verified
vendored musl archive in an independent `build/dynamic-linker/` cache.

## Build and use

```sh
make dynamic-linker
```

Inside Coast, compile a position-independent C executable or shared library:

```sh
gcc-dynamic /tmp/hello.c -o /tmp/hello
/tmp/hello
gcc-dynamic -fPIC -shared /tmp/example.c -Wl,-soname,libexample.so -o /tmp/libexample.so
gcc-dynamic /tmp/main.c -L/tmp -lexample -Wl,-rpath,'$ORIGIN' -o /tmp/main
ldd /tmp/main
```

`gcc-dynamic` uses the installed native GCC, startup objects and linker. Its
external specs select PIE and shared musl for that invocation. The ordinary
`gcc`/`g++` defaults remain `-static -no-pie`. The optional `g++-dynamic` shared POSIX C++ profile is documented in
[Shared C++](dynamic-cpp.md); the original C SDK remains supported. Small source
examples and a Makefile live at `/usr/src/coast/dynamic`.

The runtime installs `/usr/lib/libc.so`, `/lib/ld-musl-x86_64.so.1`, and
`gcc-dynamic`/`ldd` under both `/bin` and `/usr/bin`. The existing `/lib` alias
to `/usr/lib` is retained. musl supplies dependency traversal, symbol lookup,
relocations, initialization/finalization, `dlopen`, `dlsym`, and TLS.
Its normal search rules apply: an explicit library pathname; `LD_LIBRARY_PATH`
for this single-user, non-set-id ABI; the dependent object's RPATH/RUNPATH;
and the configured/default system paths. The optional musl path file is
`/etc/ld-musl-x86_64.path`, with `/lib:/usr/local/lib:/usr/lib` as the default.

For a main executable, musl normally determines `$ORIGIN` through
`/proc/self/exe`. When that link is absent, this port uses the absolute
kernel `AT_EXECFN` value and `realpath` to resolve symlinks. An arbitrary
`argv[0]` cannot select the search directory. Secure execution still disables
this fallback. If the executable path becomes unresolvable before dependency
loading, the fallback cannot establish its origin and reports missing
dependencies; no synthetic proc link is created.

## Kernel contract

The kernel accepts little-endian x86-64 ELF64 `ET_EXEC` and `ET_DYN`, a
maximum of 64 program headers and 128 MiB of load pages per image. A dynamic
main executable has one absolute, terminated `PT_INTERP` pathname. The
interpreter must be executable `ET_DYN` and cannot have its own interpreter.
Both images are validated before the old process address space is replaced.
Load segments must be readable, must not overlap at page granularity, and
cannot be writable and executable simultaneously. Executable stacks and
text relocations are rejected. Dynamic, TLS and RELRO records must describe
contained load-segment data; dynamic vectors have a bounded terminator.

PIE and interpreter load biases are deterministic (`0x40000000` and
`0x200000000`). There is no ASLR. The initial stack contains argv, envp,
`AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, the main image's `AT_ENTRY`,
the interpreter's `AT_BASE`, `AT_EXECFN`, and identity/security values matching
the current `get*id` syscalls. `AT_RANDOM` contains 16 fresh hardware-entropy
bytes when the existing random provider supplies them. On a CPU without an
available hardware source, that entry is omitted; musl then retains its
non-random stack-canary fallback. No timer/address seed is presented as entropy.

The musl integration changes only the DSO mapping boundary. It reserves
anonymous `PROT_NONE` pages, maps each segment's actual file bytes, and maps
BSS with zero-backed anonymous pages. This preserves Coast's existing
eager `MAP_PRIVATE` snapshots and rejection of file mappings beyond EOF.
Relocatable DSOs reserve with a null address hint, so multiple libraries
linked with the same nonzero first virtual address can coexist. The patch
checks the x86-64 header, segment/file bounds, W^X, executable stacks and
dynamic/TLS/RELRO metadata before mapping. The upstream relocation engine
still owns symbol and relocation semantics; this is not a hardened validator
for every deliberately corrupt ELF symbol, hash or version table.

Relocations run on writable data pages. musl then applies RELRO with
`mprotect(PROT_READ)`. Executable pages remain read/execute, writable pages
remain non-executable, and kernel `mmap`/`mprotect` continue to reject W+X.
Existing sandbox read restrictions apply to interpreter/library opens and
file mappings; dynamic linking does not grant new filesystem capabilities.

This profile does not imply compatibility with arbitrary Linux/glibc binaries,
foreign machines, executable-stack/text-relocation DSOs, overlapping LOADs,
namespaces (`dlmopen`), GNU IFUNC
extensions, or runtime unloading. musl's `dlclose` retains loaded code; library
destructors execute during normal process exit. TLS and pthread behavior use
the kernel's existing thread and signal ABI.

## Verification

```sh
make test-dynamic-linker-host
python3 tools/test_dynamic_linker.py --cpus 1 4 --iso build/coast/image/Coast.iso
```

Build the shared runtime before running the host checks. A second host
harness compiles the actual patched musl ORIGIN helper with its real 512-byte
caller buffer, covering missing proc links, access refusal, secure execution,
canonical symlinks, argv0 independence and length/allocation errors.

The kernel host harness compiles the actual ELF loader with mocked file and VM
boundaries under AddressSanitizer/UndefinedBehaviorSanitizer. Synthetic files
exercise static execution, dynamic `ET_EXEC`, PIE, direct loader entry,
auxiliary data, segment permissions, invalid metadata/interpreters and cleanup
after read/allocation failures. It does not execute guest page tables.

The guest suite runs real shared objects and the installed native compiler.
It checks transitive `DT_NEEDED`, `$ORIGIN` RUNPATH, environment search,
constructors/destructors, PLT/data/copy/relative relocations, exported and weak
symbols, large BSS, `dlopen`/`dlsym`, startup/runtime failures, TLS in an already
running pthread, two nonzero-address DSOs, and native PIE/shared-library builds.
Forked probes require actual faults on text/RELRO writes and execution of
non-executable data, and require errors for W+X mappings/protection changes.
The suite fails if it sees a success marker without the expected exit status.

Build logs and the runtime/fixture hash manifest are under
`build/dynamic-linker/`; guest serial logs use the existing
`build/runtime-tests/*-dynamic-linker.serial.log` convention.

## Verified snapshot (2026-10-15)

The actual kernel-loader host harness passed 27 ASan/UBSan cases. The actual
patched musl ORIGIN-helper harness passed 11 cases, including its 512-byte
caller and failure paths. The combined candidate03 image passed the complete
dynamic guest suite on TCG with 1 and 4 CPUs and KVM with 4 CPUs, including
native compilation, existing-thread TLS, executable-symlink/argv0 handling,
actual RELRO faults, NX and rejected W+X operations.

The exact image/kernel/libc hashes and per-run cleanup assertions are recorded
in `build/dynamic-linker/validation-20261015.json`; guest logs and admission
records use the `candidate03-*` prefix in the same directory. Earlier
`candidate01-*` and `candidate02-*` logs preserve the resolved ORIGIN failures.
