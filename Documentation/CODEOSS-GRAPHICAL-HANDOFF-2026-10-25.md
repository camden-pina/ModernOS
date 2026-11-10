# Code OSS Core Preview graphical milestone M3

Date: 2026-10-25, 01:22 UTC. Completed after Terminal r8; this is the next active handoff.

Implemented and installed an original native graphical frontend using the genuine
Code OSS PieceTree at pinned commit 211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb.
All 32 upstream module hashes verified; PieceTree unchanged. Existing two
platform.ts/process.ts adaptations remain. This is a useful graphical text-core
port, NOT the full VS Code workbench, browser Monaco, Electron or a themed rename
of the separate native Text Editor.

Open Super+Space > Code OSS Core Preview. New/Open, true upstream-model editing,
selection/movement, bounded undo/redo, literal find, real clipboard, line numbers,
scrolling, text size and indentation controls work. Open creates a new window.
Graphical limit 256 KiB including optional BOM; clipboard 64 KiB. Unicode scalar
and CRLF boundaries checked. ASCII direct key events; Unicode files/clipboard
supported with existing glyph/shaping limitations. Display controls are per
window, not persistent rich formatting. Existing native editor/Terminal r8 and
4 MiB Code OSS CLI remain separate.

Model/file service runs in a separate native JavaScriptCore process. The GUI has
bounded sequenced nonblocking queues and retains only the last acknowledged
snapshot for display/recovery. A small supervisor starts and reaps its heavy
worker; the desktop reaps only the supervisor. Normal close waits for accepted
actions and reviews dirty text. Worker-only failure permits Recover as a new
unsaved copy, including BOM. Unacknowledged input/system crashes are not durably
recovered. An interrupted Save is reported as an uncertain outcome.

Checked CoreFS saves stage an exclusive same-directory file, compare expected
source bytes, retain .codeoss-original-* and refuse occupied Save As destinations.
Conflicts keep unsaved text. No watcher, immutable backups, locks/identity CAS,
parent directory fsync or guaranteed power-loss atomicity. A writer with an old
open inode can change retained data. Profiles share file/permission boundaries.

## Verified release

`build/codeoss-core-m3-20261025/` contains both images, MANIFEST.json, VALIDATION.json,
QA logs, screenshots, source/relink material and preservation/final-verification.json.

- Coast-CodeOSS-Core-tested.iso: 1280x800 / 100%, SHA256
  4d0958a9b09e8e27aaa9db3c6edf5c78ce3a6b1793eb4c88e91f0d31f2663a4b
- Coast-CodeOSS-Core-native-tested.iso: 2560x1600 / 150%, SHA256
  0ea9fb10a15bc9c24ac5a6695f21b1d2c0a6772069941df6589e9e1a31f56bc9

Each image is 1,010,626,560 bytes. Frozen kernel SHA256:
9e03c90f99e0d458cf766740e40c4e594b1b3dc55c10293b73d64957f16b04a0.

```sh
cd /home/camdenp/Desktop/Coast
make run-image O=build/codeoss-core-m3-20261025 ISO_IMAGE=build/codeoss-core-m3-20261025/Coast-CodeOSS-Core-native-tested.iso MEMORY=4G LINUX_DISK= FAT_DISK= SATA_DISK= QEMU_NETWORK='-nic none' QEMU_EXTRA_ARGS=-full-screen
```

Type `gui` at the guest console. Ctrl+Alt+Escape returns after dirty-document review.
Close the QEMU window to exit. Verified dry-run launches the existing ISO only,
with no extra disks/network and a release-specific serial.log. Files/preferences
without a writable guest disk are volatile. Do not share writable disks/logs
between concurrent VMs. User's independent VM was never controlled/terminated.

Project rebuild: `make -C userland codeoss-graphical`; existing JSC/musl only,
no engine/kernel rebuild, downloads or host installs. Optional pinned esbuild
0.28.2 regenerates the TS bundle. Build manifests validate source/binary freshness.
See Documentation/CODEOSS-GRAPHICAL.md and the included LGPL relink/source material.

## Tests performed now

All final image suites passed at 100% light, 150% dark and 200% light in separate
owned 4-vCPU/4-GiB TCG guests, disks/network disabled. Each ran all 27 graphical
model checks and the preserved CLI's 2,000-edit native core check, plus 8 graphical
integration groups: Unicode/BOM/CRLF exact bytes, external-write conflict and
non-clobber Save As, immediate close/cancel with accepted input, forced own-worker
failure/recovery, 218,500-byte file partial IPC/edit/undo, pointer/native text and
indent controls, menus/launcher dismissal and full material repaint equivalence,
and independent guest cmp of original/saved/recovered files. Final screenshots
were visually inspected at native sizes, including HiDPI and dark mode.

Production snapshot validator: 100,000 malformed frame pairs plus valid and
invalid UTF-8/CRLF/scalar/bounds cases under ASan/UBSan. LSan disabled because the
sandbox ptrace environment does not support it; no leak-check claim.

## Measured limitation and exact next step

| Scale/theme | Unicode window startup | 218,500-byte startup | Large Ctrl+End ACK | Per-window maximum tick ms |
| --- | --- | --- | --- | --- |
| 100% / light | 1067 ms | 1444 ms | 48 ms | [2, 5, 449] |
| 150% / dark | 995 ms | 1522 ms | 54 ms | [1, 4, 425] |
| 200% / light | 862 ms | 1554 ms | 66 ms | [1, 4, 394] |

Normal document ticks were 1-5 ms; the older empty window's maximum records a
394-449 ms pause during another model's launch. Per-stage traces establish the
pause in the pipe-I/O portion (reap/receive/wait/layout were 0 at that slow tick).
Source inspection identifies the matching mechanism: fs/binfmt_elf.c load_elf
holds global vfs_mutex while mapping the roughly 52 MB JSC/ICU ELF, while
kernel/sched/sched.c task_file_pin takes the same mutex even for nonblocking
pipe I/O. This is a source-plus-timing diagnosis, not a sampled kernel lock-holder
stack. Earlier reaping hypothesis was insufficient; the supervisor does NOT
remove this VFS/loader pause. Debug stripping would not remove the large mapped
ICU/code segments. No kernel/engine modification was made to conceal it.

Next planned step: design/measure a safe shared multi-document model service to
avoid a new large ELF load for every window, or a carefully reviewed loader/VFS
lock redesign preserving executable-file consistency. Then move from complete
snapshots to incremental edits/layout, followed by upstream editor/workspace
services. Full IDE still needs presentation/runtime, Node/Electron or a genuine
web-workbench route, platform/file watching and language/extension hosts.

Terminal Ctrl-Z/stop/resume remains outstanding (frozen kernel rejects stop and
continue signals); editor Ctrl+Z undo works. GNU Bash r8 foreground Ctrl-C patch
and binary preserved. Shell prerequisites remain in SHELL-DEPENDENCY-ASSESSMENT.md.
No passwords, sudo/privilege enforcement, credentials or host account changes.
Browser allocator and trading remain paused; Disks stays read-only. Host raw
disks/settings untouched. All previous releases/original ISO/kernel/window ABI,
JSC/core CLI and paused handoffs preserved by the 131-path hash audit. No git
metadata was present; exact source tar/hash checkpoint was used. Normal permission
review approved all M3 project mutations; no bypass or pending approval.

Source locations: userland/desktop/codeoss.c and codeoss_view.h,
userland/include/codeoss_protocol.h, userland/codeoss/model-worker.cpp and
launcher.c, ports/codeoss/graphical-model.ts plus native-model.ts,
tools/build_codeoss_graphical.py and build_codeoss_release.py. New app ID appended
without changing prior app IDs. Original native Text Editor not relabeled.

Screenshots are local only under the release screenshots directory. No upload or
Library retry was attempted. Exact inherited r8 images remain:
Coast-Coast-Terminal8-tested.iso and Coast-Coast-Terminal8-native-tested.iso
under build/coast-terminal8-20261025. Its previous no-extra-disks launch command
remains valid with that release's own O= directory.
