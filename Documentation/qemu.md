# QEMU inside Coast

`make iso` includes QEMU, its dependency SDK, source-built x86 BIOS firmware,
noVNC and the `coast-vm` launcher. Start Coast with `make run` and
open Chromium to use the nested VM viewer.

Coast runs QEMU 10.2.4 with `qemu-system-x86_64`, `qemu-img`, `qemu-io`, QMP,
VNC, WebSockets and libslirp networking. The emulator, its seven library
dependencies, Ninja and boot firmware have been compiled inside Coast with
native GCC. Guest qualification checks boot, ABI and graphical input using the
rebuilt emulator.

The CPU backend is upstream TCI, which interprets TCG operations without
writable executable memory. Coast's W^X policy remains enabled. This is
software emulation and is considerably slower than KVM or a TCG JIT. Only
x86-64 system emulation is built. Use the serial console or Chromium viewer;
the port has no native SDL/GTK display backend or KVM interface.

## Run a nested VM

Copy the built `build/coast/image/Coast.iso` to a readable location inside Coast.
Allow enough RAM for both the outer desktop and the nested guest; for a
16 GiB nested guest, launch the outer VM with `make run MEMORY=32G`. Then run
inside Coast:

```sh
qemu-system-x86_64 --version
coast-vm --iso /home/user/Coast.iso --memory 16384
```

Open the printed URL in Chromium, normally
`http://127.0.0.1:6080/coast.html?port=5701`. Click the virtual screen to send
input. **Shell** sends Ctrl+Alt+Esc, **Desktop** sends `gui` and
**Ctrl+Alt+Del** sends those three keys together. Ctrl+C in the launching
terminal stops QEMU and the loopback HTTP server.

`coast-vm --help` describes memory, CPU, port, firmware and networking options.
Guest RAM is specified in MiB. Listeners bind to guest loopback. Networking is
off unless `--network` is supplied; the launcher attaches no data disk.

## Component development

The OS pipeline builds QEMU with its source-built POSIX musl compiler. A
component build can use that compiler receipt explicitly:

```sh
python3 -B tools/build_qemu.py --compiler-build build/coast/compiler/BUILD.json \
  --output-root build/my-qemu --jobs 4
```

The builder verifies versions and hashes from
`userland/vendor/qemu-sources.json`, including the QEMU release signature and
its pinned public key. It builds libffi, PCRE2, zlib, GLib, pixman, libslirp and
libucontext in a private prefix, then statically links QEMU. Host builds run
inside Bubblewrap and never execute target programs on Linux.

Eighteen BIOS, VGA BIOS and boot ROM files are built from the sources bundled
with the exact QEMU release. `BUILD.json` identifies those files and separately
lists optional release-supplied PXE/EFI ROMs. Git contains no firmware binaries.

For development inside Coast, obtain the signed `qemu-build` package from your
configured repository:

```sh
brine install qemu-build --noconfirm
mkdir -p ~/Builds/qemu
cd ~/Builds/qemu
tar -xzf /usr/src/coast/qemu-build/coast-qemu-10.2.4.tar.gz --strip-components=1
python3 -B tools/build_qemu.py --native --jobs 4
python3 -B ports/qemu/check.py --runtime build/qemu-native/runtime --sdk
```

The kit resolves GCC/G++, binutils, make, Python, pip, setuptools, Meson, Ninja,
pkgconf, patch, GnuPG and Linux UAPI headers. Its pinned upstream archives and
Ninja sources allow offline compilation after installation. The SDK lives at
`/usr/lib/coast/qemu-sdk`.

Native Ninja uses private sessions for headless jobs. Its cancellation check
verifies parallel jobs, separate process groups, SIGINT forwarding and worker
cleanup. A build-local Python hook handles unsupported optional xattrs for
Meson's copy operations without changing global Python or kernel policy.

## Qualification

`make iso ISO_ARGS=--qualify` checks the installed QEMU programs/SDK and runs
nested boot and USB-input checks through Chromium and WebSocket VNC. Its
qualification receipt records the exact tested artifacts and coverage.

`tools/test_qemu_native.py` checks package installation and native source builds;
`tools/test_qemu_viewer.py` checks browser-operated USB input.
`tools/test_large_builds.py` checks large mappings, sparse RAMFS files and
nested guest allocations. The desktop kernel permits 32 GiB native mappings
and 8 GiB RAMFS files while keeping budgeted application mappings capped at
512 MiB.

The installed `python3 /usr/share/coast/qemu/check.py --sdk` suite checks disk
conversion, qcow2 writes/rollback, coroutines, QMP, VNC, BIOS long-mode execution
and a native consumer of all seven SDK libraries. These checks do not establish
arbitrary Linux/Windows guest or real-hardware compatibility.

`tools/add_qemu_build_repository.py` prepares a signed build-kit snapshot.
After qualification, `tools/publish_native_qemu.py` checks exact input/artifact
hashes and publishes the Coast-built programs, libraries, Ninja and source
packages. Signing secrets remain outside public outputs.
