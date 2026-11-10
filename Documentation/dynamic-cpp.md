# Shared C++ profile

`make dynamic-cpp` builds GCC14.2 libstdc++ and libgcc_s with the actual POSIX
thread backend, shared musl1.2.5 and separate C++ headers. This uses the pinned
GCC source in its own build/dynamic-cpp cache. Existing static GCC/G++ and the
large static browser runtime remain intact.

Use `g++-dynamic -std=c++17 source.cpp -o app` in the guest. Shared libraries use
`-fPIC -shared`, SONAME and RUNPATH as in the installed examples under
/usr/src/coast/dynamic-cpp. The ordinary g++ remains the static profile.
The driver selects shared libgcc_s for one exception/unwind ABI across library
boundaries. Runtime files are /usr/lib/libstdc++.so.6 and libgcc_s.so.1, with
headers under /usr/include/c++-dynamic/14.2.0. The ABI is this GCC14/musl build;
it does not promise arbitrary glibc binaries or foreign compiler runtimes.

Host probes cover practical CSV/file processing, vectors/strings/streams,
cross-library virtual calls, dynamic_cast/typeid RTTI, exceptions in both
directions and unwind cleanup, constructors/destructors, std::thread and
existing-thread TLS after dlopen, lookup failures, and concurrent exceptions.
The final candidate10 image passed the real ABI application, 128 concurrent
cross-DSO exceptions with cleanup checks, missing-DT_NEEDED startup diagnosis,
and native standalone/shared-library SDK compilation at both 1 and 4 CPUs
under KVM. The same image also passed dynamic C under TCG at 1 and 4 CPUs.
See `build/continuation-20261015-phase2/final-features-1792071157499214000/`
and the final validation record for exact input hashes and cleanup proof.

Run tools/test_dynamic_cpp.py --iso IMAGE --cpus 1 4. Runtime unloading and ASLR
remain outside the musl/ELF profile; dlclose retains code, and destructors are
verified at process/thread exit. No exception crossing a C function that lacks
unwind metadata is promised. Current evidence is under
build/continuation-20261015-phase2/ and build/dynamic-cpp/.
