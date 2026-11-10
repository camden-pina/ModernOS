# Genuine upstream runtime prerequisites, A1/A2

Latest: see `Documentation/CODEOSS-NODE-A2-HANDOFF-2026-10-25.md` for the mapping-hint fix and corrected, traced RWX blocker. A1’s preliminary quota attribution was incorrect; its immutable handoff is superseded by that diagnosis.

This is a bounded ABI milestone toward the requested VSCodium/Code OSS application. It does not replace the upstream workbench with a native editor. The old M3/M4 PieceTree preview is preserved separately and is not the requested port.

The separate probe image contains the unchanged official Node 24.18.0 Linux x64 binary and a pinned Debian glibc/libstdc++ sysroot. Node's version is the selected VS Code 1.140.0 source build pin; this standalone Node binary is not Electron's embedded runtime. Exact upstream SHA-256 receipts and Debian signature/package records accompany the release. Nothing was installed into the host. Debian maintainer scripts were not executed. The packages are a signed Bookworm point-release baseline, not a claim of current production security updates.

The original C files one directory above are guest diagnostics, not an editor implementation. Build each with the cached project compiler, for example:

```sh
build/native-musl/bin/musl-gcc -static -O2 -Wall -Wextra -Werror \
  ports/codeoss-app/mmap-file-guard-check.c -o /tmp/mmap-file-guard-check
```

Use these binaries only in an owned disposable Coast guest. `runtime-launch-check` accepts an absolute guest executable path, bounds its own child to 20 seconds, prints exec/wait status and reaps only that child. A zero launcher exit alone does not establish child success: inspect `RUNTIME_WAIT`.

Successful guest commands:

```sh
mmap-file-guard-check
epoll-capacity-check
runtime-launch-check /opt/codeoss-runtime/node --version
runtime-launch-check /opt/codeoss-runtime/node --jitless -e 'console.log(1+2)'
```

`--jitless` is an explicit interpreter diagnostic. It does not enable JIT, WebAssembly, Electron, GUI rendering or the workbench. No sandbox-disabling flags or guest access-control changes are used. Default Node JavaScript startup is still blocked; see the dated handoff.

From the checkout, run the tested guest harness against the separate ISO (creates a fresh output directory; no added disks or network):

```sh
python3 -B tools/test_codeoss_node_runtime.py \
  --iso build/codeoss-node-abi-a1-20261025/Coast-CodeOSS-Node-ABI-tested.iso \
  --output-root /tmp/coast-node-a1-checks --label node-4cpu --cpus 4
```

The expected unsupported default-JIT case is recorded separately from required supported regressions. The exact cached inputs, package script, build logs, tests and source archive are retained under the release's `reproduce/` and `provenance/`. No download happens in the guest test harness.
