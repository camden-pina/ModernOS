# Build recovery, cleanup and source publication

Repository: https://github.com/camden-pina/ModernOS.git. Development uses `main`.
Generated files belong in ignored build directories; Git holds source, pinned
manifests, patches, configuration templates, artwork and license notices.

## Resume an interrupted build

Press Ctrl+C in the build terminal. The orchestrator stops its stage process
group and retains logs/checkpoints. Run the same command again:

```sh
make iso
```

Keep the same `CONFIG`, `ISO_BUILD`, `O` and app arguments when resuming a
custom build. Completed components are verified against their source recipes,
dependency receipts and artifact hashes. Changed or damaged components are
rebuilt with their dependent outputs invalidated. `STATE.json` records component
status; completion receipts are written atomically. Editing settings does not
require a new output directory. If the pinned Electron source lock/policies
change, use `make clean COMPONENT=sources` before resyncing that managed graph;
other component caches remain available.

Recovery has different granularity for different work:

| Work | Recovery |
| --- | --- |
| Pinned archive downloads | Retain partial bytes, request an HTTP range on retry, and publish only after checksum verification. Servers without range support restart that archive. |
| Electron sources | Resume fetch, patches, hooks and overlays from `electron/SYNC.json`. Completed repositories remain cached. An interrupted individual Git pack may need downloading again. Managed checkouts are disposable; keep source edits in the repository's port patches. |
| Archive extraction | Extract into a temporary tree, then publish complete source directories. A retry removes abandoned extraction scratch directories. |
| Make/Ninja and compiler stages | Reuse their object trees when recorded inputs match. A failed command reruns; individual install/configure steps may repeat. |
| GUI source packages | Verify container ownership/mounts and reuse completed package receipts. The interrupted package resumes or rebuilds using its own build tools. |
| SDK/module staging and ISO assembly | Restart the affected staging operation from verified component inputs. Downloaded archives are moved to a checksum-addressed cache before a component reset. |
| Guest qualification | Reuse passed phases only when the ISO, manifest, test sources and reports still match. Rerun unfinished phases. Omitted-app suites are recorded as skipped. |
| Cleanup | Journal the plan before deleting anything. Rerun the cleanup command after interruption; missing products are harmless. |

This preserves completed work; it does not checkpoint an individual compiler
process, retain every interrupted network transfer, or resume a VM mid-test.
A source change during an unfinished component can restart that component.
A force-killed process cannot finish its checkpoint. Recovery rechecks outputs
and retries the affected operation instead of trusting a partial receipt.

## Cleanup commands

Stop the build with Ctrl+C before cleaning. Builds, `make run` and cleanup share
a project lock. Cleanup also checks for manual/older build processes and VMs
that use this project. It refuses jobs the cleanup would affect and never kills unrelated jobs.
A manual VM using protected, retained images can continue running.
Run cleanup and build commands separately.

```sh
make clean PREVIEW=1
make clean
make clean COMPONENT=qt
make clean COMPONENT='cache logs failed'
make clean ALL=1 KEEP='sources downloads toolchains images'
make clean ALL=1
```

`make clean` removes component products, the generated kernel and userland
outputs, ISO assembly and qualification outputs. It retains source trees and
downloaded archives. It also drops build objects beneath a preserved Electron
checkout. No app selection is changed.

`COMPONENT` accepts space/comma-separated names. Cleaning a component removes
its products and invalidates dependent completion receipts, including the ISO
and qualification when those depend on it. Dependent products can remain on
disk until the next build replaces them. The ISO dependency graph uses its
recorded app selection: cleaning an unused app leaves that ISO's receipt valid.
`make iso ADD=app` can still use the saved selection after a component cleanup.

| Components | What they cover |
| --- | --- |
| `chromium`, `codeoss`, `bash`, `gcc`, `python`, `brine`, `makepkg`, `qt`, `qemu`, `qemu-viewer` | App products. Shared components are shared: cleaning Chromium includes Electron and the display bridge, so an ISO containing Code OSS needs reassembly too. Prerequisite compiler/source caches are not recursively deleted. |
| `sources` | Managed Electron checkout, depot_tools and sync receipts; invalidates the runtime and all source consumers. |
| `electron` | Electron/Node products and compiler object trees; retains its managed checkout. |
| `glibc`, `libgcc`, `cxx`, `gui-sources`, `gui-runtime`, `application`, `sdk`, `addons`, `rust`, `bridge`, `assets`, `program`, `frontend`, `viewer` | Individual pipeline components; names also appear in `make iso ISO_ARGS=--plan`. |
| `kernel`, `native`, `compiler` | Kernel admission/build, native boot/base build, or native compiler. `kernel` also cleans Kbuild output. |
| `image`, `qualification` | ISO assembly or guest verification outputs. |
| `kernel-tree`, `userland` | Selected Kbuild output, or generated `userland/bin`, `obj`, `runtime`. |
| `cache`, `downloads`, `logs`, `failed`, `tests`, `containers` | Python/npm/linker caches, abandoned Git pack/lock and receipt/extraction scratch files, pinned downloads, component logs, failed staging, qualification fixtures, or the managed GUI build container. Container cleanup resets its environment receipt; KEEP=toolchains retains that container. |

Explicit `sources` and `downloads` cleanup removes that category's cache.
`brine` selects the frontend and package backend; `packages` selects the backend
tools alone. `program` selects the
Python/jq build component. The native core and build-time dependencies can
remain necessary even when an app is omitted from the ISO.

`ALL=1` cannot be combined with `COMPONENT`. It resets generated project storage, including caches/downloads,
registered build roots, Kbuild outputs and userland outputs. This is the full
rebuild command. `KEEP` can retain selected categories:

| `KEEP` category | Retained |
| --- | --- |
| `sources` | Managed checkouts, upstream/workspace directories and versioned source trees. In-source objects may remain; Electron's known object trees are cleaned separately. |
| `downloads` | Archive/vendor/distfile caches and resumable archive partials. |
| `toolchains` | Compiler/GNU/GUI SDKs, musl/host tools, recognized completed Electron/Node build caches and their source/host dependencies. |
| `images` | Existing ISO files. Invalidated manifests still require rebuilding before qualification. |
| `logs` | Logs and log directories. |
| `tests` | Qualification/test-fixture data; invalidated completion receipts are cleared. |
| `none` | Disable the normal source/download retention. |

Use `KEEP='sources downloads toolchains'` for a broad rebuild that retains the
expensive build prerequisites. `ALL=1` has no implicit KEEP categories.
Cleanup shows activity and elapsed time while planning/removing files.
`PREVIEW=1` shows the exact paths and invalidated components without removing
products. Keep the same `ISO_BUILD` and `O` as the build for selective cleanup.

Cleanup always protects tracked/new public sources, `.git`, saved root
configuration files and registered wizard settings/backups under build/, signing keys, credential stores, `.env` files and private
service state. Selected image/rollback and service symlinks under `build/` and
their targets are protected. Symlinks are never followed for deletion. Unchanged private-key/configuration
fixtures in upstream sources can be removed only when their bytes match a
pinned Git revision or checksum-verified source archive. Actual private edits
retain the enclosing source tree and its preparation receipts intact; use a fresh output directory for a
strict rebuild if a protected tree contains in-source objects. External
OS output roots require an `OWNER.json` for this source tree; external Kbuild
outputs are registered during preparation. Unregistered existing external roots
are refused. Cleanup records remain in `build/cleanup-records/`. The standalone
`tools/clean_project.py --json` flag prints an applied record for automation.

The narrower `python3 -B tools/clean_cache.py` previews cache pruning while
retaining component products; `--apply` performs it. `--intermediates` additionally
prunes verified, unreferenced Electron/Node object/debug caches. It retains the
same publication/private-state boundaries. Inspect disk use with
`du -h --max-depth=1 .` and free space with `df -h .`.

## Publish sources

```sh
make source-check test-source-tree
make source-archive
git status --short
```

The audit includes tracked and new files, even force-added ignored files. It
rejects compiled binaries, generated paths, archive downloads, unsafe links,
oversized files and private-key/token patterns without printing their contents.
The deterministic `build/coast-source.tar.xz` includes an inventory and pinned
lwIP for offline kernel builds. The README screenshot comes from a Coast VM.
See [contributing](../CONTRIBUTING.md) and [licenses](../LICENSES.md).
