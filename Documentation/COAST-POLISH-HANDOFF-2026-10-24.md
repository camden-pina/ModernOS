# Coast Polish r4 handoff — 2026-10-24

Recorded: 2026-10-24 19:30:39 UTC. Actual project: `/home/camdenp/Desktop/Coast`.
Latest completed UI pass after Clarity r3. Implementation is applied, built and
verified in real QEMU guests. No approval is pending.

## What shipped

- Settings opens without waiting for wallpaper previews. Preview decoding is lazy;
  live image decoding uses a nonblocking pipe and WNOHANG child handling. Composition
  and procedural painting run in bounded row slices. The current wallpaper remains
  visible until the replacement is complete; later selections cancel older requests.
  Two decoded photo sources and a spare frame are cached. Known children are reaped.
  Static Settings storage information is cached instead of repeatedly walking paths.
- Clicking the open application's same top menu heading now closes it. Other headings
  switch normally. Visible heading bounds govern hit testing, including narrow screens.
- Settings > View > Dock and right-click > Dock preferences expose 24/30/36/42 logical
  pixel icons and Off/16%/24% magnification. The default is 30 and 16%. Magnification
  eases, and icons are rasterized at their actual physical size into a bounded cache.
  Enlarged portions accept clicks. Pin order and running-app roles remain stable;
  context menus offer Keep in Dock/Remove from Dock. Unpinned running apps remain
  visible until their last window closes. Adaptive work areas and popup material
  keys follow Dock size/content changes.
- Text Editor Settings toolbar button, View > Preferences and Ctrl+, open an in-window
  preferences panel. Native 11/13 monospace sizes; 2/4/8-column tabs; spaces; autoindent;
  line numbers; whitespace markers; current-line highlight; Detect/LF/CRLF newline
  insertion are supported. No implicit existing-file conversion. CRLF cursor and
  Backspace handling keep the pair together. The native editor retains its 256 KiB limit.
- Dock/editor values automatically save through the existing atomic preferences
  writer. Errors remain visible. Settings shows the actual config path/storage.
  Writable guest FAT storage persists across reboot; RAM storage lasts for the
  session. Image display defaults live separately under `/usr/share/coast/` so
  the seed does not prevent user preference saves.

## Measured behavior

Same native 2560x1600 guest at 150%, 4 vCPUs, TCG, no network or attached storage.
Each measurement includes two 120 ms synthetic input calls, so about 240 ms is the
harness floor rather than a precise application launch cost.

| Observation | Clarity r3 | Polish r4 |
|---|---:|---:|
| Cold Settings OPEN observed | 1858.2ms | 243.8ms |
| Queued menu accepted after cold launch | 1940.9ms | 243.8ms |
| Queued menu accepted after Alpine selection | 1299.4ms | 243.0ms |
| Warm Settings reopen | 242.6ms | 243.2ms |

The new complete wallpaper work took 321.8ms for Dusk
and 754.2ms for Alpine, while input remained usable.
This removes synchronous stalls; applying an image remains asynchronous rather
than instant. Native 150 drag composition averaged 20.71 ms over 20
frames (maximum 25.51 ms) in the final standalone glass run.
TCG software composition does not guarantee a 120 Hz HiDPI desktop.

## Verification performed now

- Full native GUI compiles/links with `-Wall -Wextra -Werror`; candidate05 binaries are
  the exact bytes in both delivered images. Kernel and original images are unchanged.
- 100%, 150% and 200% real USB/serial guest input: repeat/interrupted top menus;
  eased Dock hover and return; enlarged-edge clicks; magnification Off; running
  pin/unpin; last-window removal; Dock/editor autosave and full cold-reboot reload.
- Actual saved document bytes match indentation, spaces, CRLF, Detect, End and
  CRLF Backspace expectations. Native Settings/panel/Done mouse controls and
  RAM preference save/relaunch passed with no disk attached.
- Light normal 100%, light native 150% and dark HiDPI 200%: cached glass versus forced
  full lower-layer recomposition; menus/launcher/modal dismissal; Dock settling;
  overlapping window drag restoration. Dynamic real Terminal output reaches upper
  glass. Read-only Disks inventory/partition selection/refresh passed; its attached
  disposable GPT/FAT image hash stayed unchanged.
- ASan/UBSan wallpaper row code: 22 pixel-exact comparisons against the preserved
  r3 oracle, through 2560x1600 and all placement/background modes. Actual glass,
  font/label clipping and HID/input checks passed. LeakSanitizer was disabled for
  the existing text/input scripts because this traced executor cannot run LSAN.
  Initial LSAN-only failures are retained in the task workspace.
- Wallpaper cancellation was observed; final stats report zero live decoder children.
  The row loop aims for 2 ms, but allocation/small preview/syscall work can exceed that
  budget; see per-run max_tick_us. No claim of strict whole-tick 2 ms latency is made.

Full evidence: `build/coast-polish-20261024/qa`, `logs`, `VALIDATION.json`,
`r4-source.patch`, `changed-files.json`, manifests and preservation hashes.
The task workspace also retains exploratory harness failures, raw PPMs and
project-owned test disk images. Early harness issues were trace matching, serial/
USB input ordering and immutable-preset setup; these were corrected before the
release runs. No failing release case remains.

## Code OSS port milestone and remaining work

Real unmodified MIT upstream `microsoft/vscode` commit
`211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb` PieceTree modules execute in Coast native
JavaScriptCore. The shipped test passes 2000 deterministic model-checked edits,
UTF-16 Unicode, CRLF, stable snapshots and offset/position mapping. Run in the guest:

```sh
jsc /usr/share/codeoss/core-check.js
```

Source subset, license, source/tool archive hashes, build recipe and status are in
`ports/codeoss`. esbuild 0.28.2 is host-only in the task workspace, with registry
integrity verified; no global install was performed. A console-to-print bridge
is the only shim. Upstream's explicit `Unable to resolve platform.` warning is
retained: no Node, Electron, DOM or navigator is fabricated.

This is an upstream text-engine component port, not the full IDE, an Electron
native port or a running web workbench. The native Text Editor is its own app.
Coast currently lacks a full Node/Electron/Chromium environment, glibc/X11 or
Wayland desktop services. Upstream pins Node 24.22.1 and its Linux contribution
instructions require X11/xkbfile/secret/krb5 development dependencies. A web
workbench is a separate route with browser/DOM and server requirements. See
`Documentation/binary-compatibility.md` and the official upstream contribution
instructions: https://github.com/microsoft/vscode/wiki/How-to-Contribute .

Exact next port step: introduce an explicit Coast platform adapter and a
native file/service bridge for this real upstream model, test file-backed edit/
snapshot round trips, then choose the presentation/runtime strategy. Full
workbench UI, extensions, terminals and their runtimes remain unavailable.
The prior browser allocator investigation stays paused; it was not executed.

## Run the delivered UI

Native 2560x1600 with guest 150% defaults (recommended on this desktop):

```sh
cd /home/camdenp/Desktop/Coast
make run-image ISO_IMAGE=build/coast-polish-20261024/Coast-Coast-Polish-native-tested.iso LINUX_DISK= FAT_DISK= SATA_DISK= QEMU_EXTRA_ARGS=-full-screen
```

At the guest console enter `gui`. Ctrl+Alt+Escape returns to the console;
Ctrl+Alt+F toggles QEMU fullscreen. The command clears all optional disk
attachments and uses no host raw disk. The 1280x800/100% alternative is
`build/coast-polish-20261024/Coast-Coast-Polish-tested.iso` with the same target.
No rebuild is needed. Preferences on a diskless boot are RAM-only; persist them
by using an existing explicitly chosen writable guest volume rather than a host
raw disk. The test images used for persistence are disposable project artifacts.

## Preservation and delivery

Original `build/os.iso`, `build/initramfs.tar`, `build/vmlinux`, prior Coast/Glass/
Clarity ISOs, `Documentation/AI-HANDOFF.md`, `CONTINUATION-HANDOFF.md` and phase5
status match their pre-r4 hashes (10 protected artifacts). Source checkpoint:
`build/coast-polish-20261024/preservation/clarity-r3.tar.gz`.
The Desktop tree has no .git directory; the separate Documents/test checkout was
not changed. Trading stays paused. Disks remains read-only. No host disk,
security setting or host display setting was changed.

Local screenshots under `screenshots` are actual final candidate05 framebuffer
captures. Dock preferences shows 42 px selected for the test; the default remains 30.
No files were published or uploaded. Library upload remains unavailable and was
not retried. No approval blocker remains.

Native ISO SHA256: `8e246b382c0cb7155cce9b634fa2fa0550c47f74710249724986b225a997cecd`
Normal ISO SHA256: `355c540f4cc3f5c70dea5a914f215645e403c2f2ac2e161b334b589dd27d5cb1`
GUI SHA256: `e686c86b79e00d6141843581785b841bbf16adf5ceed3aa7090051382a66c128`
Kernel SHA256: `9e03c90f99e0d458cf766740e40c4e594b1b3dc55c10293b73d64957f16b04a0`
