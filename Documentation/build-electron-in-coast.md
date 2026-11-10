# Building Electron yourself inside Coast

This is the manual native build procedure for Coast's pinned Electron 43.7.3
and companion Node. The complete Electron build inside Coast has not been
qualified. In particular, its Node/npm, Rust/bindgen and Chromium host-tool
workflows must pass the checks below before starting the long compilation.
The automatic `tools/build_electron_source.py` build path currently runs on
Linux and uses Bubblewrap.

1. **Prepare the exact sources on Linux.** From the Coast checkout, run:

   ```sh
   python3 -B tools/build_electron_source.py --sync \
     --output-root build/electron-source
   ```

   This downloads the pinned depot_tools/170-repository graph, applies the
   upstream patches and Coast overlays, and runs the source preparation hooks.
   Transfer `build/electron-source/checkout/` and the Coast source checkout to
   the VM. Include Git metadata, `node_modules`, downloaded compiler tools and
   sysroots. Keep any Git object alternates self-contained. For a Linux build
   instead, add `--build --jobs 4` to this command; it records the resulting
   `BUILD.json` automatically.

2. **Allocate build storage and install the native tools.** Plan for at least
   100 GB of workspace for the source graph and build. A Coast RAMFS workspace
   consumes guest RAM and disappears at shutdown. FAT32 cannot provide the
   symlink and permission behavior needed for the full source tree; the ext
   filesystem driver is currently read-only. Save source/results through your
   chosen transfer mechanism before shutting down.

   The usual 16–24 GB development VM is too small for a complete Electron
   source/build tree in RAMFS. Before attempting this build, provide enough
   guest RAM for the measured transferred tree, build outputs and running
   tools, or finish a writable POSIX filesystem port. Attaching a large disk
   alone does not supply writable POSIX storage on the current Coast image.

   Inside Coast:

   ```sh
   sudo pacman -Syy
   sudo pacman -Fyy
   brine install coast-build --noconfirm
   ```

   Required build tools include Python, Git, make, Ninja, GN, m4, Bison, gperf,
   NASM, Node/npm, Clang/LLD, Rust and bindgen. `coast-build` supplies the core
   native toolchain and source kits; it does not yet supply every Electron
   build-tool port. The [native tools guide](native-build-tools.md) gives the
   GCC/CMake/GN/LLVM source bootstrap commands and their qualification status.
   Chromium's GNU application sysroot and matching toolchain are needed for
   this build.

3. **Set the paths and check the build tools.** Adjust these example paths to
   where you put the checkouts. `COAST_NODE` must point to a Node runtime that
   passes the process-creation check in the next step.

   ```sh
   export COAST_SRC=/home/user/Builds/Coast
   export ELECTRON_SRC=/home/user/Builds/electron/checkout/src
   export COAST_NODE=/opt/electron-43.7.3/node
   export COAST_GN=/usr/bin/gn
   cd "$ELECTRON_SRC"
   python3 --version
   ninja --version
   m4 --version
   bison --version
   gperf --version
   nasm --version
   "$COAST_GN" --version
   third_party/llvm-build/Release+Asserts/bin/clang --version
   third_party/rust-toolchain/bin/rustc --version
   third_party/rust-toolchain/bin/bindgen --version
   "$COAST_NODE" --jitless --version
   ```

   A `--version` result is only a first check. The matching GNU loader and
   runtime libraries must also support compilation, subprocesses and loading
   native build-tool modules. If a tool fails here, finish that tool's Coast
   port before proceeding.

   Use the GN installed by Coast's signed `gn` package: it is built from the
   pinned GN revision and includes the qualified native subprocess port.

4. **Check Node subprocesses and create local wrappers.** Run:

   ```sh
   "$COAST_NODE" --jitless -e 'const c=require("child_process"); const r=c.spawnSync("/usr/bin/gcc",["--version"],{stdio:"inherit"}); if(r.error)throw r.error; process.exitCode=r.status??1'
   ```

   The current browser companion Node has a separate memory/security profile;
   its threaded build-tool subprocess behavior has not been qualified. A
   failure in this check requires a suitable native build Node. Keep the
   browser's sandbox and memory policy intact when implementing that port.

   Once that check passes:

   ```sh
   mkdir -p "$COAST_SRC/build/electron-native-tools/bin"
   cat > "$COAST_SRC/build/electron-native-tools/bin/node" <<'SH'
   #!/bin/sh
   exec "$COAST_NODE" --jitless "$@"
   SH
   cat > "$COAST_SRC/build/electron-native-tools/bin/npm" <<'SH'
   #!/bin/sh
   exec "$COAST_NODE" --jitless "$ELECTRON_SRC/third_party/electron_node/deps/npm/bin/npm-cli.js" "$@"
   SH
   cat > "$COAST_SRC/build/electron-native-tools/bin/gn" <<'SH'
   #!/bin/sh
   exec "$COAST_GN" "--root=$ELECTRON_SRC" "--root-pattern=$COAST_GN_ROOT_TARGET" "--root-target=$COAST_GN_ROOT_TARGET" "$@"
   SH
   chmod 755 "$COAST_SRC/build/electron-native-tools/bin/"*
   export PATH="$COAST_SRC/build/electron-native-tools/bin:$PATH"
   export PYTHONDONTWRITEBYTECODE=1
   export PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python
   export npm_config_cache="$COAST_SRC/build/electron-native-tools/npm-cache"
   export npm_config_update_notifier=false
   npm --version
   ```

   Restore the checksum-pinned optional Rollup host module:

   ```sh
   cd "$COAST_SRC"
   python3 -B -c 'import sys; from pathlib import Path; sys.path.insert(0,"tools"); from prepare_electron_host import prepare; print(prepare(Path("build/electron-native-tools")))'
   export NODE_PATH="$COAST_SRC/build/electron-native-tools/node_modules"
   ```

5. **Generate and build Electron using the checked-in Coast profile.** The
   profile includes jitless JavaScript, interpreted Wasm with explicit bounds
   checks, and disabled Node/context snapshot generation.

   ```sh
   cd "$ELECTRON_SRC"
   mkdir -p out/Coast-electron
   cp "$COAST_SRC/ports/electron/args.gn" out/Coast-electron/args.gn
   export COAST_GN_ROOT_TARGET=//electron:electron_app
   export ELECTRON_OUT_DIR=Coast-electron
   gn gen out/Coast-electron --script-executable=/usr/bin/python3
   ninja -C out/Coast-electron -j2 electron
   ```

   Start with two compilation jobs. Re-running Ninja resumes the build. Use
   an in-source `out/` directory here: some upstream generators assume that
   directory layout and write generated TypeScript files into `src/electron/`.

6. **Build the matching standalone Node.**

   ```sh
   mkdir -p out/Coast-node
   cp "$COAST_SRC/ports/electron/args.gn" out/Coast-node/args.gn
   export COAST_GN_ROOT_TARGET=//third_party/electron_node:node
   gn gen out/Coast-node --script-executable=/usr/bin/python3
   ninja -C out/Coast-node -j2 node
   ```

7. **Record the products and build a matching test kernel.** The recorder
   checks the pinned source overlays, GN arguments, ELF bounds and W^X, then
   hashes stripped copies of the binaries. It does not run Electron.

   ```sh
   cd "$COAST_SRC"
   python3 -B tools/record_electron_build.py --native \
     --checkout "$ELECTRON_SRC/.." \
     --electron-output "$ELECTRON_SRC/out/Coast-electron" \
     --node-output "$ELECTRON_SRC/out/Coast-node" \
     --output-root build/my-electron
   python3 -B tools/build_desktop_kernel.py \
     --runtime-build build/my-electron/BUILD.json \
     --output-root build/my-electron-kernel --jobs 4
   ```

   Coast's runtime admission uses exact binary hashes and canonical paths.
   This kernel step records the newly built pair without widening admission
   to arbitrary Electron binaries.

8. **Assemble Coast with the verified runtime.** Transfer the build products
   and their verified source checkout to the Linux build host, then use the
   normal OS pipeline:

   ```sh
   make iso ISO_ARGS='--electron-checkout /path/to/verified/checkout --runtime-build /path/to/my-electron/BUILD.json'
   make run
   ```

   The builder checks the Electron/Node pair, builds the matching kernel and
   assembles Coast with all desktop integrations. Use `ISO_ARGS=--qualify` with
   the same reuse arguments when guest qualification is required; see
   [build arguments](build-system.md).

   Build receipts store absolute paths. Preserve the recorded layout when
   transferring artifacts, or record the products on Linux with their new
   paths, omitting `--native`, before using them in the OS pipeline.

The source references for these steps are the pinned Electron
`docs/development/build-instructions-linux.md`, `build-instructions-gn.md`,
Coast's `ports/electron/sources.lock.json`, `ports/electron/args.gn`, and the
native builders in this repository. Native Electron compilation is left to
the user; it is not recorded as a passed Coast qualification.
