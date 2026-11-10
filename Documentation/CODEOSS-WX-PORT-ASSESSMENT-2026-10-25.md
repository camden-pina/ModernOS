# Upstream V8/Electron port feasibility with Coast W^X intact

2026-10-25 04:54 UTC. Assessment only: no new security policy, runtime configuration, source build or sandbox exception was applied. A2's five-file installation was approved on its one normal retry; this assessment follows that resolved filesystem block.

The requested target remains the genuine upstream VSCodium/Code OSS application. The native PieceTree preview is not a substitute. Standalone Node 24.18.0 is a tested prerequisite, not Electron's embedded runtime.

## What is established

The unmodified Node binary executes version/interpreter tests but default execution requests RWX memory and fails under the unchanged kernel rule. Its first 512 MiB hint failure was an ABI placement defect, now fixed; its fallback allocation succeeded with page_limit=0. A1's quota explanation was corrected in the A2 handoff. Virtual-reservation work alone cannot solve the RWX request.

This is not only the initial large code cage: the exact upstream [MutablePageMetadata::GetCodeModificationPermission](https://raw.githubusercontent.com/nodejs/node/v24.18.0/deps/v8/src/heap/mutable-page-metadata.h) selects RWX outside jitless mode. [MemoryAllocator::SetPermissionsOnExecutableMemoryChunk](https://raw.githubusercontent.com/nodejs/node/v24.18.0/deps/v8/src/heap/memory-allocator.cc) uses either RWX recommit or that modification permission. The [code-range initialization](https://raw.githubusercontent.com/nodejs/node/v24.18.0/deps/v8/src/heap/code-range.cc) also precommits its normal code range. Disabling one code-range optimization or reducing a heap size is therefore not established as a W^X solution.

Static strings in the pinned, SHA-verified `codium` ELF identify `Chrome/150.0.7871.250 Electron/43.7.3`, matching the selected source build pin. This is static artifact evidence; that Electron executable has not run in the guest. The first attempted source URL used shell/common and returned 404; the correct exact-tag source is shell/browser, linked below. No unsupported version substitution was made.

## Feasible upstream configuration for a limited bring-up

Electron documents passing V8 flags on the startup command line. Its exact [43.7.3 JavascriptEnvironment::Initialize](https://raw.githubusercontent.com/electron/electron/v43.7.3/shell/browser/javascript_environment.cc) reads `--js-flags` before isolate initialization. That makes `--js-flags=--jitless` a justified configuration candidate for a genuine Electron main-process bring-up, preserving the kernel's W^X rule. It has not been executed on Coast. [Electron switch documentation](https://www.electronjs.org/docs/latest/api/command-line-switches#--js-flagsflags) independently describes startup flag propagation to the main process.

This would still be the upstream application/runtime, configured to use an interpreter. It would not establish default JIT support or full feature parity. Node's successful jitless tests do not prove Electron's renderer/utility/extension subprocesses inherit the mode, that bundled snapshots accept it, or that all workbench features work. [V8's explanation](https://v8.dev/blog/jitless) describes interpreter execution and its performance/feature tradeoffs. WebAssembly-dependent extensions, search/tokenization services, native addons and node-pty require explicit tests; do not label them supported from an arithmetic smoke test.

Before that configuration can be tested on the actual bundle, the ~216 MiB executable still needs an approved, finite application-image budget design (the existing budget remains 128 MiB). The ELF loader must retain all segment, overlap, bounds, W^X and permission validation. The complete cached official Electron resources, compatible DSOs and checked process setup are also required. A Node-mode Electron entry, if the actual packaging fuse permits it, is a smaller prerequisite than a window; it is not the editor. No manual ld.so loading is proposed to evade the image budget.

## Why PKU is not a strict-W^X shortcut

The exact upstream [RwxMemoryWriteScope documentation](https://raw.githubusercontent.com/nodejs/node/v24.18.0/deps/v8/src/common/code-memory-access.h) distinguishes Apple Silicon's per-thread writable-versus-executable behavior from Intel PKU. Intel PKU changes data read/write permissions while execution remains enabled; opening its write scope therefore does not preserve the current strict write-versus-execute invariant. Outside supported platforms that scope is a no-op.

Coast currently saves FXSAVE state, has no pkey allocation/protection syscalls or PKRU context/signal implementation, and rejects RWX mappings. Adding PKU APIs/CPU state alone would neither make the existing permission request pass nor supply strict W^X. That is a substantial security/context-switch design, not a compatibility checkbox. No PKU, host CPU/security or guest enforcement setting was changed.

Likewise, a simultaneous writable alias and executable alias of the same physical page is not automatically equivalent to strict W^X. Coast deliberately rejects executable memfd mappings. Enabling such aliases would cross that separate policy boundary and requires a reviewed security design and action-time approval; it must not be smuggled in as ordinary mapping correctness.

## Genuine source-port route and prerequisites

A runtime port could keep the upstream workbench while adapting V8's executable allocator and code-writing scopes to Coast's existing RW-to-RX transitions. It requires more than replacing one permission constant:

1. Remove RWX precommit/recommit assumptions and commit code pages only with allowed permissions.
2. Ensure no thread executes a page while it is writable; coordinate concurrent compilation, relocation, inline patching, GC, deoptimization and debugger/profiler code paths. Apply equivalent rules to Wasm/jump tables if enabled.
3. Preserve checked address/physical accounting, failure rollback, page lifetime, instruction-cache correctness and restoration after exceptions/signals. Test multiple isolates/workers, repeated teardown and resource exhaustion under the real guest limits.
4. Build/test the exact pinned upstream runtime with retained licenses and a reviewable small platform patch before attempting Electron/Chromium integration. This changes the upstream runtime, so it must be described as a source port rather than unmodified-binary compatibility.

Feasibility: a source adaptation is plausible, but no working x86-64 strict-W^X configuration was established in the inspected pinned code. Do not promise a small patch or a runnable IDE before implementing and verifying these conditions. No large source build was launched: free space was about 23 GiB before final release copying, while [Chromium's Linux build guidance](https://chromium.googlesource.com/chromium/src/+/main/docs/linux/build_instructions.md) requires substantially more (100 GB minimum in the checked guidance). Do not delete user data or change environments to force it.

## Recommended next bounded milestone

Prepare the finite executable-budget/application-process design for action-time review, then attempt actual pinned Electron startup in the explicitly labeled jitless configuration if that design is approved. Record the embedded runtime versions, snapshot/flag behavior and next failing API before undertaking graphical integration. Full workbench rendering additionally requires compatible GTK/NSS/other libraries, an actual display/Ozone/input path, and a sandbox-preserving multiprocess model. The separate strict-W^X V8 source-port investigation should begin with a build/storage/test plan, not a full Chromium checkout.

No `--no-sandbox`, RWX exception, assertion suppression, host settings change, custom editor fallback or paused allocator/trading work is part of this recommendation.
