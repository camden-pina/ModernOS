# Executable compatibility

Coast accepts little-endian x86-64 ELF64 `ET_EXEC` programs and `ET_DYN`
PIE/shared objects. Static and dynamic describe linkage; each program must also
fit the implemented OS ABI. Supporting both does not establish that most ELF
files or most Linux programs run.

The kernel validates an absolute `PT_INTERP` pathname and loads a bounded
`ET_DYN` interpreter without its own interpreter. The shipped interpreter is
`/lib/ld-musl-x86_64.so.1` from pinned musl 1.2.5. Shared C uses that libc;
shared C++ uses GCC 14.2 libstdc++/libgcc_s with the POSIX thread backend and
its matching headers/exception ABI. Default native `gcc`/`g++` still select
the static profile. See [dynamic linking](dynamic-linking.md) and
[shared C++](dynamic-cpp.md) for loader, TLS and SDK contracts.

Linux x86-64 syscall numbers are implemented as a subset. Unknown numbers
return `ENOSYS`; unsupported operations/flags can return an error even for an
implemented number. Tests cover the supplied musl applications, rebuilds of
upstream tools, and the project's dynamic C/C++ ELF fixtures. They do not
establish an untouched Ubuntu/Fedora/Arch/glibc executable corpus. No such
corpus was run in this continuation.

A binary requiring the glibc interpreter, glibc libraries/symbol versions,
foreign C++ runtimes, X11/Wayland, absent devices or other unimplemented OS
services needs further work. Other architectures, ELF32 and big-endian files
are rejected. AVX and user FSGSBASE are disabled. The loader rejects executable
stacks and writable/executable load pages; it has bounded headers/mappings.
Dynamic load biases are deterministic, with no ASLR. musl `dlclose` retains
loaded code; general unloading, glibc `dlmopen` and IFUNC compatibility are
not provided. These bounds are described in the loader guide.

Source anchors: `fs/binfmt_elf.c`, `kernel/sched/syscall.c`,
`tools/build_dynamic_linker.py`, `tools/build_dynamic_cpp.py` and
`tools/test_dynamic_{linker,cpp}.py`. Final-image execution reports, when
complete, are recorded under `build/continuation-20261015-phase2/`.

The additional bounded syscall profile and independently rebuilt GNU/util-linux
corpus are described in [Linux command tools](linux-command-tools.md). Their
exact final guest results belong to the Phase3 evidence, separately from the
preserved Phase2 validation. Presence of a syscall number alone is not proof
that a program's required semantics work.
