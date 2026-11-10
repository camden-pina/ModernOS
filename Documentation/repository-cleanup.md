# Source publication and cache cleanup

This working tree is initialized as a Git repository on `main`. No remote,
commit or push is created by the preparation workflow. The source inventory
contains no compiled programs, firmware binaries, disk images, downloaded source
archives or private signing material.

## Git inputs

Keep kernel/application sources, Kbuild and Makefiles, port patches, pinned
upstream manifests, editable configuration, artwork and required notices.
`.gitignore` excludes all of `build/`, `userland/bin/`, `userland/obj/`,
`userland/runtime/`, toolchain outputs, upstream archive caches, Python caches,
images, compiled objects/libraries and signing secrets.

The Vim configuration input lives at `userland/config/vimrc`; its runtime copy
is generated. OVMF is discovered from the host or selected with `OVMF=...`.
Its local cached firmware is ignored. Checked-in Linux UAPI files are C header
sources with hashes and license notices, not executable libraries.

```sh
make source-check test-source-tree
make source-archive
git status --short
```

`tools/source_tree.py` checks both tracked and new files, so force-adding an
ignored binary or key still fails the audit. It rejects compiled-file magic,
archive downloads, generated paths, unsafe links, unusually large files and
private-key/token patterns without printing their contents. The exported source
kit has deterministic ownership/timestamps and a SHA-256 inventory. Its one
explicitly bundled source archive is the verified lwIP kernel dependency.

The GitHub workflow pins its checkout action and runs source publication tests,
source-fetch tests, ACPICA/power/EFI/boot-config host checks, and fresh kernel and
native desktop compilation. Generated outputs must remain ignored after those
checks. Original Coast code uses MIT; third-party licenses are preserved in
[LICENSES.md](../LICENSES.md).

## Retired browser

The native NetSurf frontend, WebCore renderer/broker port, obsolete UI tests and
build targets have been removed. Chromium owns Super+B and the browser dock
entry. Existing saved dock IDs migrate without renumbering other applications.
HTML source can still open in the text editor; Chromium's existing restriction
on navigating local file URLs remains in place.

Standalone JavaScriptCore remains for the text-core editor/SDK. Kernel IPC and
sandbox ABI tests remain where they still exercise implemented kernel behavior.
The Chromium image assembler also filters old browser programs/resources from
all inherited initramfs modules instead of only hiding their desktop entry.

## Generated caches

```sh
python3 -B tools/clean_cache.py
python3 -B tools/clean_cache.py --apply
```

Cleanup targets known obsolete Chromium candidate ISOs, retired WebCore caches,
failed QEMU staging/test trees, superseded Electron/Node object caches, retired
media caches, the old NetSurf download/notices and Python bytecode.
Selected images and rollback targets, active package/community service bundles,
compilers, dependency SDKs, source pins and signing keys are preserved. The
record of the latest cleanup is `build/cache-cleanup.json`; earlier receipts
are retained with timestamped filenames. Its logical byte
count differs from physical space recovered when filesystem clones share data.
Historical reports can retain paths to removed generated artifacts.

`make clean` removes the selected kernel/image output; `make userland-clean`
removes the generated userland staging tree. Do not delete all of `build/` while
using the local package/community services or historical runtime assembly paths.
See [source-builds.md](source-builds.md) for current full-image rebuild coverage.
