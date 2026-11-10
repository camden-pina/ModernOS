# Coast Glass desktop revision 2 — 2026-10-24

Recorded UTC: 2026-10-24T16:51:00+00:00

Status: implemented in `/home/camdenp/Desktop/Coast`, built and verified.
This supersedes the first Coast desktop handoff for UI appearance and compositor
behavior. That handoff and the paused browser allocator evidence remain unchanged.
Trading remains untouched. There is no pending implementation approval.

## Result and scope

The first pass had oversized metrics and backdrop tint without blur. Revision 2
implements real spatial backdrop filtering in the existing software compositor:
window headers, Files/Disks sidebars, menu bar, Dock, launcher, menus and modal
surfaces sample the actual composition beneath them. Lower application content,
including live Terminal output, contributes to the blur. Original procedural
wallpaper and locally drawn app icons are used with licensed DejaVu fonts.
No Apple code, fonts, branding or extracted assets were imported.

Default logical dimensions are now a 24px menu bar, 36px window header, 52px Dock
with 36px icons and 46px item spacing. Files normally opens at 860x540 rather than
1140x760, with a 164px sidebar (140px when narrow) and 28px list rows. Other apps
normally open at 780x500; Settings 660x510. Work-area clamps and scaling remain
shared by maximize, snap, drag and resize. Disks uses a 176px adaptive sidebar,
39px device rows and 31px property rows (stacked at narrow widths). Typography,
neutral light/dark surfaces, controls, spacing, borders and shadows were revised
together. The launcher is centered, with 32px row spacing.

## Compositor design

`glass_filter.h` contains an exact running-sum separable box filter. Two horizontal/
vertical rounds approximate a Gaussian at quarter resolution after 4x4 averaging;
bilinear reconstruction and material-specific tints produce the final surface.
The full two-pass sampling halo is included. This is a CPU implementation, not a
GPU blur or a claim of macOS material parity.

`glass.c` caches only full-resolution chrome/sidebar bands per window and five
system surface materials. Shared quarter-resolution scratch buffers limit memory
use. Cache keys include geometry, scale, theme, spatially intersecting lower
windows, their foreground revisions and lower material keys. Unrelated changes
outside a material's sample halo do not rebuild it. Resizing/closing/cleanup
release caches. A counted solid fallback exists for allocation failure; all
verified final runs recorded zero fallbacks.

Cached window foregrounds now use premultiplied ARGB. Opaque bodies retain the
fast copy path. Before any layer samples a pending material, every required
damage/sample rectangle is reset from the wallpaper and all lower layers are
drawn across it in order. Each system overlay is completed before the next one.
The old cursor and previous-frame foreground cannot feed back into blur.

A 200% regression test found an additional stale foreground hover state in an
occluded Files window. Pointer ownership is now determined by the actual top
window/popups. A change in ownership invalidates the affected cached foreground;
covered windows paint without hover. The previously failing 188-pixel difference
now passes exact cached-versus-fresh comparisons. Temporary hash instrumentation
was removed from the final payload. Icon cache identity also includes UI scale.

## Final local image and launch

`build/coast-glass-20261024/Coast-Coast-Glass-tested.iso`

ISO SHA-256: `574acfc09fab0612bb6bd5c2d63d5999714dbcaf57447f2d540080b87651e99f`

GUI SHA-256: `d0e6751e9cf2e252ef1eb43df544b38b558808a9ce06eb8c54a91d4871f9b4e5`

Kernel retained: `9e03c90f99e0d458cf766740e40c4e594b1b3dc55c10293b73d64957f16b04a0`.
All 7946 initramfs entries were compared by identity, type, ownership, mode, link
target and file hash. Only `bin/gui` changed. The original `build/os.iso`, original
initramfs/kernel and first Coast tested ISO remain unchanged. The standard tested
image boots at 1280x800. A separate 2560x1600 verification derivative uses the same
kernel/initramfs; only boot display configuration changes.

```sh
cd /home/camdenp/Desktop/Coast
make run-image ISO_IMAGE=build/coast-glass-20261024/Coast-Coast-Glass-tested.iso LINUX_DISK= FAT_DISK= SATA_DISK=
```

Enter `gui` at the guest console. Super+Space opens the launcher; Disks is also
in the Dock. Ctrl+Alt+Escape returns to the console; close the VM window to exit.
The empty disk overrides above prevent optional project images being attached.
No host raw disk is passed. Do not use the old Coast v1 path expecting this UI.

## Verification performed now

- Native `make -C userland -j1 bin/gui` passed the normal GCC `-O2` freestanding
  build with warnings as errors, including a full desktop object rebuild after
  the cached foreground structure changed. No kernel/browser broker rebuild or
  browser allocator debugging was performed.
- Production blur filter passed 72 independent naive-oracle comparisons, radius 1
  through 6, single-pixel/odd extents, replicated edges, constant preservation,
  impulse spread and bilinear interpolation under ASan/UBSan.
- Production painter/pointer checks passed opaque/transparent/premultiplied
  coverage and source-over composition; occluded/exposed windows, launcher,
  modal and minimized window pointer ownership. Production glyph measurement
  and clipping passed all 8 fonts at 100/125/150/200%. Existing input checks passed
  UTF-8, selection, pointer motion and HID decoding. LeakSanitizer is disabled
  because this environment's ptrace prevents it; ASan/UBSan remain enabled.
- Final 1280x800 TCG 4 CPU/light 100%: real read-only device and partition inventory,
  selection/F5, overlapping glass, global menus, launcher, modal dismissal, Dock
  hover, 30 input drag steps, exact restoration and return to console passed.
  Delayed output in a lower Terminal changed 6030 pixels in the upper glass;
  its resulting three-layer composition matched a fresh reconstruction exactly.
  The complete disposable GPT/FAT image hash stayed unchanged (`readonly=on`).
- Final 1280x800 TCG 4 CPU/dark 100%, 2560x1600 TCG 4 CPU/light 200% and dark 200% passed the
  same material/repaint, overlay, Dock and drag comparisons. The HiDPI image has
  the identical final GUI/kernel/initramfs payload. Final 1280x800 TCG 1 CPU/200%
  additionally passed real Settings 200%→100%→200% switching and bounded
  geometry for all three windows. Flat-material diagnostic baselines passed
  at normal and HiDPI resolutions on the same corresponding images.
- Actual final screenshots were visually inspected for proportions, readable
  text, material appearance and clipping. These are VM captures, not mockups.
  All final source/image hashes were checked; all owned test guests exited.

`FINAL-VALIDATION.json` in the image directory records reports, serial logs,
screenshots, hashes and measured timings. Reproduce with
`tools/test_glass_host.py` and `tools/test_glass_desktop.py --iso IMAGE --label UNIQUE
--output-root PATH`. The VM tool accepts `--dark`, `--scale`, `--dynamic`,
`--disk`, `--live-scale`, `--flat` and `--cpus`. Optional disks are newly created
disposable images attached read-only. `gui --trace --stats` exposes counters;
Ctrl+Alt+F12 in trace mode forces full material/foreground reconstruction.
`--no-glass` is a diagnostic flat-material baseline, not the default.

## Measured performance and remaining limits

Measured sequentially under QEMU TCG with 4 virtual CPUs, including composition and display upload:

| Display and material | Drag mean | Worst captured frame | Rendered drag frames |
|---|---:|---:|---:|
| 1280x800 light 100%, blur | 9.83 ms | 12.01 ms | 31 |
| 1280x800 light 100%, flat diagnostic | 6.10 ms | 7.45 ms | 31 |
| 2560x1600 light 200%, blur | 39.17 ms | 42.67 ms | 12 |
| 2560x1600 light 200%, flat diagnostic | 26.22 ms | 31.46 ms | 16 |

Blur adds approximately 3.73 ms per rendered drag frame at the default
resolution and 12.96 ms at HiDPI in this comparison. Fast input steps are
coalesced when software rendering cannot keep up; 30 input events do not imply 30
presented frames. HiDPI TCG rendering is slower and is not claimed to sustain
60/120 fps. KVM/physical hardware performance and a new GPU backend were not tested.
The full OS/browser regression suite was not run. Fullscreen opaque/animated
applications are not made translucent; only the specified UI materials are.

Disks remains read-only and uses only the existing kernel inventory API. It
reports actual capacity, hierarchy, sector size, filesystem/mount/access metadata
when supplied. Used/free space and partition scheme/UUID remain unavailable.
Erase, repartition, format, repair and mount/unmount remain unavailable, with no
destructive buttons or mutation calls. Kernel access metadata can say read/write
even when QEMU independently enforces read-only access. No host disk or host
security/settings were changed.

Big Sur visual proportions/material references were inspected at the official
[Apple June 2020 announcement](https://www.apple.com/newsroom/2020/06/apple-introduces-macos-big-sur-with-a-beautiful-new-design/).
Reference images remain separate from product assets in `/home/camdenp/Documents/Codex/2026-10-24/task-2/glass-r2/references/`.
The result is Big Sur-inspired Coast with original artwork and existing APIs.

## Preservation and next step

Revision 1 source/binary checkpoint: `/home/camdenp/Documents/Codex/2026-10-24/task-2/glass-r2/preservation/coast-v1.tar.gz`.
The adjacent `baseline.json` records protected handoffs and original images.
`/home/camdenp/Documents/Codex/2026-10-24/task-2/glass-r2/delivery/Coast-coast-glass-r2.patch` is the reviewable delta from Coast v1.
Earlier successful candidates, the failed 200% test, temporary diagnostic image,
and final reports remain under `/home/camdenp/Documents/Codex/2026-10-24/task-2/glass-r2`. The Desktop tree has no Git metadata;
no unrelated checkout was edited.

Next step: have the user review the separate Glass image at its default 100%
scale. Tune remaining visual preferences from that running image. Physical/KVM
performance and broader application regressions remain unverified; no unrelated
development is resumed automatically. The known browser mallocng free-guard
failure remains separate, unresolved and paused, with its handoff unchanged.
