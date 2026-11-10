# Coast Clarity desktop revision 3 — 2026-10-24

Recorded UTC: 2026-10-24T17:33:15+00:00

Status: implemented, built, actual VM captures inspected, and verified in
`/home/camdenp/Desktop/Coast`. This handoff supersedes Glass r2 for the current
UI. Original images, Coast v1, Glass r2 and all paused browser handoffs remain
unchanged. Browser allocator work and trading remain paused. No approval is pending.

## Pixelation diagnosis

Confirmed guest causes were binary window/control/Dock silhouettes, outline icons
with only four nonzero coverage levels and 16-segment circles, a 40px cache cap that
limited some HiDPI icons, and normalized wallpaper coordinates quantized onto a
1024-step grid. Old RGB coverage arithmetic also used /256 for /255 alpha and
could darken edge pixels, including zero coverage. These are source/actual-native-
capture findings, independent of host zoom.

Fonts were already FreeType grayscale glyphs rasterized independently at 100/125/
150/200%, without runtime bitmap enlargement. That was retained. Smaller body and
title fonts were regenerated from the original licensed DejaVu outlines at every
physical scale; no old font bitmap was shrunk and no Apple assets/fonts imported.

A read-only `kscreen-doctor -o` query confirmed the CachyOS KDE Wayland panel is
2560x1600 at 120% host scale (logical 2134x1334). The old guest image is 1280x800. A
1280x800 image cannot fill that physical panel without enlargement. No running
user QEMU window was available for measuring its actual zoom; the exact host
resampling contribution remains unverified. No host display settings were changed.

## Implemented changes

- Cached 16x16 physical-pixel corner coverage now antialiases controls, borders,
  Dock tiles, popup materials and completed window silhouettes. Windows blend
  their finished edge against the actual lower scene; old corner pixels cannot
  leak into a cached foreground. Only corner masks are supersampled, not the canvas.
- Vector icon strokes use analytic grayscale coverage and 32-segment curves.
  Thinner 1.3 logical-pixel strokes replace 1.6px strokes. The native cache supports
  up to 80px, including real HiDPI detail icons, with scale in its cache identity.
  A locally authored cached antialiased cursor and antialiased diagonal strokes
  replace hard pixel stair steps. Caches are bounded; no external artwork is used.
- RGB and premultiplied foreground coverage now use correct /255 alpha arithmetic
  and zero coverage is an identity. Native font edges use that corrected path.
- Coast contours are evaluated at physical-pixel centers with subpixel precision
  instead of the 1024-step coordinate grid. Wallpaper remains generated once/cached.
- Default logical density: menu bar 22px (was 24), window header 32 (36), Dock 44 (52),
  Dock icons 30 (36), Files 800x500 (860x540), other app windows 720x460 (780x500),
  Files sidebar 148 (164), sidebar rows 22 (25), file rows 24 (28), device rows 34 (39),
  property rows 28 (31). Body 11px (12), title 16px regular (17 bold). Window control
  hit boxes are 20x22px, independently of the smaller visible lights. Settings
  normally opens 620x480. Paint, hit testing, paging and content heights agree.
- The real backdrop blur, material halos, spatial dependency keys, lower-layer
  reconstruction, premultiplied cached foregrounds and pointer-ownership fix from
  Glass r2 remain. All final runs recorded zero material allocation fallbacks.

## Tested images and launch

For this desktop's native panel, the separately packaged image boots 2560x1600
with 150% guest scale already embedded. This provides more workspace and native
glyphs instead of enlarging a 1280x800 guest. Only guest RAM-home/config entries
are added to the verified compact payload; no host configuration is written.

```sh
cd /home/camdenp/Desktop/Coast
make run-image ISO_IMAGE=build/coast-clarity-20261024/Coast-Coast-Clarity-native-tested.iso LINUX_DISK= FAT_DISK= SATA_DISK= QEMU_EXTRA_ARGS=-full-screen
```

Enter `gui` in the guest console. Ctrl+Alt+Escape returns to the console;
Ctrl+Alt+F toggles QEMU fullscreen, then close the VM window. The fullscreen flag
and shortcut are documented in the official [QEMU manual](https://www.qemu.org/docs/master/system/qemu-manpage.html)
and [frontend keys](https://www.qemu.org/docs/master/system/keys.html). App fullscreen
does not change the host's monitor resolution/scale. Actual SDL/KDE host-window
pixel mapping was not captured; the supplied images themselves are native-sized.

Native ISO SHA-256: `de1fa3b1d5565e62d9f67ba9fe96e7d7ed9579092ceb314527ada61e2fb31fe1`

An ordinary 1280x800/100% image is also preserved separately:

```sh
make run-image ISO_IMAGE=build/coast-clarity-20261024/Coast-Coast-Clarity-tested.iso LINUX_DISK= FAT_DISK= SATA_DISK=
```

Compact ISO SHA-256: `b1284cb78b75c377da2e791110fe37dd4d2d089b4d8de13ab1768306fd8f0e75`

Shared GUI SHA-256: `aeb805764fd8a3945061d2c028685133e7740cf50cfaaa66b4af49c9f215976b`

Shared unchanged kernel: `9e03c90f99e0d458cf766740e40c4e594b1b3dc55c10293b73d64957f16b04a0`.
For the compact image, all 7946 original initramfs entries were compared by type,
ownership, mode, link target and file hash; only `bin/gui` differs. The native
preset retains every compact payload entry unchanged, adding only `home`,
`home/user`, `home/user/.config` and `home/user/.config/desktop.ini` (`scale=2`,
`dark=0`; index 2 is 150%). The 2560x1600/200% QA derivative has the identical compact
initramfs and changes only boot display policy. Existing ISO paths do not contain
Clarity; use the new explicit path. Empty disk overrides attach no optional disks.

## Verification and visual evidence

- Native GUI build passed GCC `-O2` with `-Wall -Wextra -Werror`. Only desktop GUI
  objects were rebuilt; no kernel/browser broker rebuild or browser allocator-issue reproduction.
- ASan/UBSan: actual production corner coverage compared with an independent
  64x64 area oracle (maximum 6/255 error), all 256 alpha values, zero/constant color
  preservation, clipping, analytic icon coverage and full 80px cache, cursor at
  all four scales, and native glyph antialiasing. The final 16x16 corner cache
  replaced an 8x8 prototype that exceeded the area-error target.
- Existing production blur/premultiplied-painter tests passed 72 exact filter
  oracle cases, radius 1..6, odd/single extents, edges, impulse spread, interpolation,
  cached alpha composition and pointer ownership. Production text measurement/
  clipping passed all 8 fonts at 100/125/150/200%. LeakSanitizer remains disabled
  because the environment uses ptrace; address/undefined checks remain enabled.
- Eight final VM runs passed: light/dark100%, native-size 2560x1600 light/dark200%,
  small-display 200% with real 200%→100%→200% Settings changes on TCG1, flat
  baselines at normal/HiDPI, and the actual native150% preset without injecting
  a configuration. The preset's real geometry reports 150%, proving its embedded
  guest preferences were used. All test guests used TCG and were closed afterward.
- Cached versus forced fresh compositions match exactly below the changing clock
  for overlapping glass, menus and launcher. Modal dismissal, Dock hover and 30
  input drag steps restore exact pixels. Delayed lower Terminal output changes
  the upper glass and matches a fresh three-layer composition. Native selected
  rows, close/maximize/restore/minimize controls, Dock restore, Home/Projects
  sidebar clicks and the fourth file row opened the correct unchanged document.
- The real read-only disposable GPT/FAT disk was selected/refreshed through the
  existing API; its complete hash stayed unchanged. No host raw disks were used.
- Actual final full frames and unscaled component crops were visually compared
  with Glass r2 at100% and200%. The lighter title, denser layout and smoother
  curved edges are visible at native pixels. `native-comparison/` contains crop-
  only PNGs and exact source/bounds metadata; no resizing or retouching was used.
  Screenshots contain ordinary guest-created QA files, never hardcoded app rows.

The image directory contains `FINAL-VALIDATION.json`, `pixelation-diagnosis.json`,
actual full screenshots, native component crops, host logs and the reviewable patch.
Full serial/report paths are recorded under `/home/camdenp/Documents/Codex/2026-10-24/task-2/clarity-r3/qa`.
Reproduce with `tools/test_clarity_host.py`, `tools/test_glass_host.py`,
`tools/test_desktop_text_host.py` and `tools/test_glass_desktop.py --iso IMAGE
--label UNIQUE --output-root PATH`. The VM script supports `--dark`, `--scale`,
`--dynamic`, `--disk`, `--layout`, `--live-scale`, `--flat`, `--image-defaults`.

## Measured cost and remaining limits

Sequential QEMU TCG captures include composition and upload. Values are per
rendered drag frame; input events may be coalesced. Geometry is intentionally
smaller in Clarity, so these are practical workload comparisons, not isolated
microbenchmarks or physical-hardware guarantees.

| Case | Glass r2 mean | Clarity mean | Clarity worst |
|---|---:|---:|---:|
| 1280x800 light100%, TCG4 | 9.83 ms | 8.49 ms | 9.88 ms |
| 1280x800 dark100%, TCG4 | 9.76 ms | 8.94 ms | 11.11 ms |
| 2560x1600 light200%, TCG4 | 39.17 ms | 33.38 ms | 36.23 ms |
| 2560x1600 dark200%, TCG4 | 40.07 ms | 34.39 ms | 37.76 ms |
| 1280x800 at200%, cramped stress, TCG1 | 28.83 ms | 31.84 ms | 34.08 ms |
| Native2560x1600 preset150%, TCG4 | — | 20.08 ms | 23.45 ms |

Normal and true HiDPI costs improved. The artificial small-display 200% stress
case is slower than r2 and remains CPU-limited; it is not the shipped normal or
native preset. HiDPI TCG is not claimed to sustain 60/120 fps. KVM is unavailable;
physical hardware, SDL host-presentation timing and the full OS/browser suite
were not tested. The compositor remains CPU-rendered with cached blur, not a new
GPU backend or exact macOS material/font parity.

Disks stays read-only. Capacity/hierarchy/filesystem/mount/access metadata come
from the kernel inventory. Used/free space, UUID/scheme and destructive or
mount/unmount operations remain unavailable. Kernel access can report read/write
while QEMU independently enforces read-only backing; the app issues no mutations.
No host disk, security setting or desktop display configuration was changed.

Original Big Sur visual references remain isolated under the prior workspace's
`glass-r2/references/` and the official [Apple announcement](https://www.apple.com/newsroom/2020/06/apple-introduces-macos-big-sur-with-a-beautiful-new-design/).
No Apple resources or branding are in the product. Library upload remains
unavailable and was not retried without a changed condition; artifacts are local.

## Preservation and next step

Glass r2 source/GUI/tool checkpoint: `/home/camdenp/Documents/Codex/2026-10-24/task-2/clarity-r3/preservation/glass-r2.tar.gz`.
Its `baseline.json` records all protected original/Coast/Glass images and handoffs.
`Coast-coast-clarity-r3.patch` is the scoped delta from Glass r2; unchanged files
and the separate original Git checkout were not edited. This Desktop extraction
has no Git metadata. No changes were published or uploaded.

Next step is user review of the native Clarity preset using the new command.
If display softness remains, measure the actual running QEMU window's mapping;
do not infer it from KDE scale alone. Keep browser allocator work and trading
paused. The known browser mallocng free-guard issue remains unresolved and separate.
