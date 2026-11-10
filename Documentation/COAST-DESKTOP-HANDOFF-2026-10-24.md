# Coast desktop and Disks — 2026-10-24

Status: implemented in `/home/camdenp/Desktop/Coast`, built and verified.
The browser allocator investigation is paused. Its existing handoffs and
Phase5 status/evidence were not changed. Trading remains untouched.

## Delivered behavior

The original cached software compositor now presents a cohesive Coast desktop:
neutral light surfaces and blue selection, retained dark mode, rounded window
chrome, left close/minimize/maximize controls, cached shadows, an adaptive
floating Dock and global focused-application menus. Existing application menu
callbacks and shortcuts remain real. The top menu bar and Dock tint their
composed backdrop through alpha blending; there is no live blur or new GPU
backend. Original procedural coastal contours and locally drawn outline icons
are used, with the existing licensed DejaVu fonts. No Apple assets or branding
were imported.

Window geometry reserves28 logical pixels above and76 below, including
maximize, snap, move, resize, restore and live scaling. The native app header
is40 logical pixels. Menus scroll within this work area; modal text/input stays
screen-bounded. At narrow widths the application menus remain available with
Ctrl+Alt+M (and Alt-letter shortcuts where supported). All registered apps and
Return to console remain reachable through the scrolling launcher.

**Disks** is registered in the launcher, Dock and system menu. It reads only
`SYS_sysinfo(INFO_LSBLK)` and displays actual device/partition hierarchy,
exact capacity in bytes, sector size, kernel-reported access, filesystem,
mount point and parent when supplied. Refresh is in its title toolbar; F5
also refreshes. Arrows select devices; wheel/Page Up/Down/Home/End scroll.
The parser bounds entries/strings, detects malformed/truncated snapshots and
rejects arithmetic overflow/duplicates instead of fabricating rows.

Capacity is total device/partition size. Used/free filesystem space and
partition scheme/UUID are unavailable from this API. Erase, format, resize,
partition, repair and mount/unmount operations are labeled unavailable; there
are no deceptive destructive buttons or disk mutation calls. Kernel-reported
access can say read/write even when a VM backend independently enforces
read-only access; the app itself always remains read-only.

## Tested local image

`build/coast-desktop-20261024/Coast-Coast-tested.iso`

SHA-256: `c4959f814380557d4c5dec7312b0fde529f5081183b8bf056e2931cf41d44f82`

GUI SHA-256: `6471131fab58c6fe9fbb15b3ae236eb45bac6911111b9fbc5ce2c8a4b935d330`

Kernel retained: `9e03c90f99e0d458cf766740e40c4e594b1b3dc55c10293b73d64957f16b04a0`.
The separately packaged1280x800 image changes only `bin/gui` across all7946
initramfs entries. Entry names/types, ownership, modes, link targets and file
hashes were compared. The640x480 derivative uses the same kernel/initramfs,
with display configuration alone changed.

```sh
make run-image ISO_IMAGE=build/coast-desktop-20261024/Coast-Coast-tested.iso
```

Enter `gui` in the guest shell. Launch **Disks** from the Dock or Super+Space
search. Ctrl+Alt+Escape returns to the console. Do not infer that `build/os.iso`
contains these changes: the original ISO/initramfs/kernel were preserved.
No physical boot disk was written. Nothing was pushed, published or uploaded.

## Validation

- Native `make -C userland -j1 bin/gui` passed normal GCC `-O2`, warnings as
  errors. No full browser rebuild or clean-cache rebuild was attempted.
- Production inventory parser ASan/UBSan checks passed real hierarchy/capacity/
  mount/access snapshots, malformed/truncated fields, overflow, duplicate names,
  the64-entry limit and every truncated prefix of a larger inventory.
- Existing production text and input host checks passed all8 fonts,100/125/150/
  200% label clipping, UTF8 editing, pointer movement and real HID Mouse/relative
  TouchPad decoding. Leak checking was disabled because LeakSanitizer cannot
  run under this environment's ptrace; address/undefined-behavior checks stayed
  enabled.
- Final image TCG4/light100%: guest `lsblk` values, real device/partition selection,
  F5 and pointer Refresh, unavailable-operation scrolling, global menus, all
  three left window controls, Dock restore and maximize/restore passed. Real
  USB dragging away and back restored exact pixels below the clock/menu bar.
- Same payload TCG1/640x480/dark200%: empty inventory, Refresh, details, menus,
  scrolling help, complete launcher and console return passed. Live200-to100-
  to200% changes kept all three real windows inside their work area.
- Final image TCG4/dark150%: real global Editor save/copy and Terminal paste
  dispatch passed; the pasted native-shell command executed, and saved document
  bytes and command output were verified after GUI exit.
- Disposable GPT/FAT test disk was attached `readonly=on`; its complete hash
  stayed unchanged. All final image hashes stayed unchanged. Screenshots were
  inspected, including the corrected compact details/operation labels.

`build/coast-desktop-20261024/FINAL-VALIDATION.json` points to the full reports,
serial logs and screenshots under
`/home/camdenp/Documents/Codex/2026-10-24/task-2/qa/coast04-final-*`.
The first candidate04 harness dragged on the newly placed Refresh button;
it correctly refreshed instead of dragging. That failed test is preserved.
The corrected blank-header drag target passed on the same unchanged ISO.
Earlier candidates and reports are retained separately.

Reproducible checks are `tools/test_disk_inventory_host.py`,
`tools/test_coast_desktop.py` and `tools/test_coast_desktop_modes.py`.
The VM scripts require `--iso IMAGE --label UNIQUE`; `--output-root PATH`
redirects artifacts. The modes script accepts `--compact` for a640x480 image.
The old coordinate-dependent42px-taskbar/64px-header tests describe the older
layout and are not claimed to pass Coast unchanged.

## Preservation and limits

Before edits, desktop sources, GUI binary and relevant documentation were
preserved in `/home/camdenp/Documents/Codex/2026-10-24/task-2/preservation/`.
`baseline.json` records the original ISO/initramfs/kernel and browser handoff
hashes. `Coast-coast-desktop.patch` in that workspace is the final reviewable
source/test/documentation delta. This Desktop tree has no Git metadata; the
separate original `/home/camdenp/Documents/test` checkout remains clean on
`refine-build-system-stable`, commit `4e213f90`.

All owned test guests exited. No host disk, security, network or power setting
was changed. The earlier automatic review denial was resolved by clarified
later implementation authorization, using the normal approval retry. There is
no pending approval or implementation blocker for this UI scope.

Physical hardware/KVM and the full OS/browser regression suite were not run.
This is a software-rendered desktop with bounded application and disk-inventory
APIs, not a new GPU/windowing or general disk-maintenance stack. The known
browser allocator failure remains separate and unresolved. Future work should
start from this handoff and these preserved test artifacts, without resuming
that investigation unless requested.
