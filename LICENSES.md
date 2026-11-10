# Licenses

Original Coast code is licensed under the [MIT License](LICENSE). This grant
applies to original project code; independently licensed third-party components
retain their existing terms and notices.

Third-party sources and data retain their own licenses and copyright notices:

- ACPICA: `lib/acpica/LICENSE.BSD-3-Clause` and `LICENSE.GPL-2.0-only`.
- Linux UAPI C headers: `ports/linux-uapi/licenses/` and per-file SPDX tags.
- Electron, Node, Chromium and dependency patches: upstream licenses referenced
  by `ports/electron/sources.lock.json`, plus `userland/electron-runtime/LICENSE`.
  The Chromium and V8 source overlays retain the BSD notices in
  `userland/electron-runtime/Chromium-LICENSE` and `V8-LICENSE`.
- Wayland protocols and keyboard map: `ports/wayland/COPYING`, embedded XML
  notices and `xkeyboard-config-COPYING`.
- QEMU and its libraries: upstream notices from the pinned source archives;
  SeaBIOS licensing accompanies the source-built firmware. noVNC, Ninja, pip
  and setuptools retain the licenses recorded in their source manifests.
- Code OSS/VSCodium: notices under `ports/codeoss/` and `ports/codeoss-app/`.
  The source-built proxy resolver stages its MIT license, third-party notices,
  the PAC runtime's Apache license and the Rust runtime's MIT/Apache licenses.
- CPython and its source patch in `ports/python/`: upstream Python licensing
  is retained; the builder and Python package install the upstream `LICENSE`.
- Public GUI sources/header bootstrap packages: the pins in `ports/chromium/` and
  `ports/codeoss-app/` retain Debian package copyright notices in the staged
  runtime and SDK. The desktop pipeline compiles runtime libraries from the
  pinned source archives and Debian patches. The separate binary bootstrap
  remains available for development. No compiled libraries are committed to Git.
- Native build tools: GNU grep and Bison retain GPL-3.0-or-later notices;
  Bison's generated parser skeletons retain their upstream exception. CMake
  and its bundled libraries retain their individual notices, and GN retains
  its BSD license. The builders install those notices with their runtimes.
- GNU application ABI libraries: the pinned Debian glibc source and Debian
  patch series retain glibc's LGPL/GPL terms and component notices. The source
  builder stages `COPYING`, `COPYING.LIB` and `LICENSES` with the runtime.
- LLVM/Clang/LLD: `ports/llvm/SOURCES.json` pins the upstream source under
  Apache-2.0 WITH LLVM-exception. Runtime packaging preserves project and
  bundled-component license files from that source archive.
- Other upstream ports: their pinned source archives and notices under
  `userland/vendor/licenses/`; build recipes stage the required runtime notices.
- Formatter: `lib/LICENSE.formatter`.

A package's license metadata describes its own payload. Source kits contain
multiple independently licensed components; they do not relicense those files.
