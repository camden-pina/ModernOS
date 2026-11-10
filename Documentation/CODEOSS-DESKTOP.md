# Full Code OSS desktop entry

Press **Super+Space**, type `Code OSS`, and press **Enter** to start the full
editor. Its own running Dock icon restores it after minimization. Reopening
the entry focuses the existing window, including during startup and across
workspaces. The earlier **Code OSS Core Preview** remains a separate app.
This adapter currently supports one workbench window.

Boot the selected qualified development image with:

```sh
make run-codeoss
```

`build/CODEOSS-CURRENT.json` records the selected image, hashes and qualification
report. Local images and dependency caches are excluded from source control.
This image continues the local Electron port; the stock `make iso` target does
not package the same runtime.

## Files and window controls

The editor now opens `/home/user/Projects` and uses `/home/user` as `HOME`.
Projects, Documents, Downloads, Desktop, Pictures and the personal configuration
directories belong to UID/GID 1000. Native Files and Text Editor create personal
files with that same ownership, including copies and atomic saves.

`code /some/folder` accepts accessible directories outside the former
`/home/port-test` restriction. Normal inode owner/group/mode checks govern
creation and modification on the boot RAM filesystem. System directories
remain protected. `/tmp` permits temporary files and has sticky-bit protection
against deleting or replacing another user's entries. Mounted filesystems
retain this port's existing access limitations. Files in RAM do not persist
across guest reboots. The existing explicit Code OSS profile, extensions and
private runtime channels remain under `/home/port-test` for compatibility.
Starting the editor preserves the existing mode and group of user-owned home
and Projects directories. Private runtime/profile directories remain private.

There is one set of window controls, in the outer desktop frame. Code OSS's
supported `window.controlsStyle: hidden` setting removes its duplicate controls
while retaining its actual menus. A small port stylesheet keeps titlebar
dropdowns above the workbench and outside the titlebar clip. The outer controls, border dragging, Dock,
and F11 provide maximize/restore, resize, minimize/restore and fullscreen.
The editor receives F1 and its Alt-letter menu shortcuts; Ctrl+Alt+M opens the
desktop application menu.

## Responsiveness

The desktop now supports half/quarter edge snapping and six preview layouts,
using the [Windows snap bar/flyout interaction](https://support.microsoft.com/en-us/windows/experience/snap-your-windows).
Hover its maximize control or press Super+Z, then choose a region. Super+Down
restores the original unsnapped bounds. The same geometry reaches Electron
through the existing native configure path. Files and desktop icons receive
external filesystem changes; see [desktop file actions](desktop.md).
Dragging near the top center opens a horizontal layout bar. Choosing a cell
starts Snap Assist: the first window occupies its cell and the next empty cell
is highlighted. Other eligible windows appear as clickable previews entirely
inside the free cells. Choose a preview (or Tab/arrows and Enter) to place it;
when one candidate and one cell remain, the last placement is automatic. Escape
keeps completed placements and reveals unchosen windows at their original sizes.
Too few candidates leave cells empty; extra windows keep their original bounds.
Minimized, fullscreen, other-workspace and owned child windows are excluded. Title-bar clicks preserve snapped geometry;
detachment requires a real drag. Hover changes, including cells with identical
screen geometry in different layouts, repaint the picker immediately.

The R196 native-size resize validation keeps the sampled fixed UI pixels
identical across all eight in-drag screenshots, with no exposed blank bands.
An isolated R196 run (2 CPUs, 16 GiB, 2560x1600) measured first pixels at 3.56 s,
mean resize interval 15.85 ms, p95 26 ms and maximum 28.44 ms. R188's scaled
preview run had 18.03 ms mean and 20 ms p95. That policy performed live
application relayout and retains constant text size, so it does more client work
than the former deferred layout. These are local observations, not a universal
frame-rate guarantee. Exact reports and visual evidence are under
`build/desktop-polish-20261105/`.

The R201 follow-up decouples the border from completed client sizes. Its
96-sample resize check observed 95 distinct border updates, zero border-position
error at the sampled inputs, unchanged fixed UI pixels and no foreign blank
bands. The isolated interaction benchmark measured 17.37 ms mean / 21 ms p95
frame intervals versus R196's 15.85 / 26 ms. Desktop work during the drag fell
from 1.80 s across 141 frames to 1.50 s across 143 frames. Total presented pixels
in the whole workflow fell from 933 million to 381 million. The shadow cache
reuses separable horizontal falloff values instead of recalculating them for
every row. Reports are under `build/desktop-customize-20261105/`; these local
observations do not guarantee a hardware frame rate or remove client reflow cost.

The kernel tracks which filesystem event types have subscribers, so a
change-only desktop watch does not resolve parent paths on loader/font reads.
Mask replacement, mask-add, one-shot removal and queue cleanup update those
subscriptions under the filesystem lock.

The launcher enables Node's version/source/flag-checked module compilation
cache in `/home/port-test/.cache/codeoss-compile`. Explicit-offset vectored
I/O allows Node to persist its temporary cache files and rename completed
bytecode into place. The cache can be generated inside an isolated guest using
`tools/prepare_codeoss_startup_cache.py`, then supplied to the workbench builder
with `--startup-cache PATH/TO/CACHE.json`. Packaging checks the runtime hash and
every installed application source, rejects unfinished cache files and installs
the files for UID/GID 1000. Fontconfig caches are also bound to the packaged
font files and configuration, avoiding a fresh font scan on every boot. The launcher adopts root-imported boot cache files
through a bounded descriptor-based walk before dropping privileges; links and
shared inodes are excluded. Changed code needs a newly generated cache. Existing
user settings remain authoritative; V8 execution remains jitless.

The November 4 comparison of R163 and R174 used the same real-USB harness,
two CPUs, 16 GiB RAM and 2560×1600 software pixels, with one guest at a time:

| Measure | R163 | R174 |
| --- | ---: | ---: |
| First visible editor pixels while moving the cursor | 4.76 s | 4.31 s |
| Resize frame interval, 95th percentile | 26.0 ms | 20.0 ms |
| Slowest resize frame interval | 30.7 ms | 20.1 ms |
| Total desktop work during the same resize input | 2.00 s | 1.49 s |
| Native layout requests during the benchmark | 30 | 5 |

Cold first pixels improved 9%, resize p95 improved 23%, and desktop resize
work fell 25% in this pair. Mean resize cadence was similar (17.4→18.0 ms).
Startup cursor p95 was also similar (58.3→60.4 ms), with a similar maximum
sample (146→148 ms). Screenshot/QMP overhead is included. These are local
measurements, not a guarantee for every workload; jitless software rendering
still limits responsiveness. A separate two-launch sequence without the moving
cursor observer measured R163→R175 cold first pixels at 3.68→3.05 seconds and
relaunch at 3.12→1.81 seconds. Each window settled for twelve seconds before
closing. The final comparison ran without host compilation. Another cold probe
during host sanitizer compilation took 4.56 seconds, showing the host-load
sensitivity; all runs are retained. Exact reports are under
`build/desktop-upgrade-20261104/PERFORMANCE.json`.

The previous R175/R188 checks preserved the approximately 18 ms mean resize
cadence while removing exposed blank bands. R175 measured 17.9–18.1 ms mean
and 20–21 ms p95 across two controls; R188 measured 18.0 ms mean, 20 ms p95 and
23.0 ms maximum. Both issued five native size configurations in the same drag
sequence. Complete previews add some copying: R188 spent 1.66 seconds of
desktop work across 144 resize frames, versus 1.33–1.47 seconds for R175's
anchored content and exposed bands. Final first pixels took 3.59 seconds,
within the two controls' 3.49–4.20 second range. This follow-up does not claim
a further startup improvement. Host image caching/load affects the results;
all intermediate probes are retained in
`build/desktop-followup-20261104/PERFORMANCE.json`.

The latest interaction pass keeps nonblocking child-output reads in an
unthreaded desktop off the global filesystem lock. Console writes use the
same descriptor-lifetime protection. Loading an unrelated Electron executable
therefore does not make those operations wait for its file verification.
Threaded and blocking I/O keep the existing retained-descriptor path.
The loader also reuses successful digest verification for pristine reserved
boot-module bytes. Canonical paths still apply; writes, truncation and
replacement lose that identity and require verification again. Mutable files
are never trusted through this cache.

Cursor-only frames upload the exact old/new cursor bounds from the retained
scene without repainting windows or glass. The native cursors include text,
hand, grab/grabbing, loading and four resize directions; Code OSS retains its
own content cursors. The image installs a standard Xcursor theme generated from
the native cursor paths, including text, hand, grab and resize aliases at 1x/2x.
`fallocate` reports unsupported extent reservation with `EOPNOTSUPP`, allowing
libwayland-cursor's `ftruncate` fallback. The private SHM server retains each
received fd and grows read-only views by mapping the replacement before
unmapping the old view, outside active SHM accesses. It requires no moving
shared `mremap` support. Title bars are 28 logical pixels high. Windows may extend
beyond either side edge with 96 logical pixels remaining reachable, while
the menu bar and Dock retain their work areas.

Native client pixels are composed directly from the retained canvas; only the
header needs a separate decorated surface. Kernel frame buffers retain capacity
in 1 MiB steps instead of allocating two new buffers for each size. Covered
wallpaper and lower windows are skipped while preserving backdrop sample areas.
During resizing the native border follows pointer input immediately. The
application receives coalesced configure requests (16 ms minimum interval,
one outstanding relayout and a 160 ms recovery timeout), independently of
border presentation. Complete client pixels retain their native size and are
clipped at the viewport; newly exposed slivers use the client's own edge colors
until its next complete frame arrives. Text and pointer coordinates never scale.
Electron controls its own responsive layout, as in the
[VS Code layout examples](https://code.visualstudio.com/docs/configure/custom-layout).
The bridge copies full resize frames without comparing against obsolete pixels;
incremental frames still compare and transfer only actual changes. Obsolete
buffers are released without publishing a padded foreign background.
Native bottom/right border changes damage only the header and changed edges.
Damage regions are disjoint rather than expanding an L-shaped border into a
full-window repaint; a bounded 32-region overflow falls back to a full frame.
Trace output marks native views `unscaled=1` and logs client layout latency.
The application can still take longer to reflow than the border takes to move.
Native damage and input coordinates follow current window geometry even when
movement reuses cached pixels. Offscreen cache painting keeps the complete
surface clip; resizing outside the screen then moving back does not expose
unpainted pixels. Content updates invalidate only their owning window.

The follow-up integration checks exercise the top drag bar, explicit Snap Assist selection
and final automatic placement, independent restore, saved colors/tags/favorites after a
fresh GUI process, real file bytes and templates. The resize/cursor check
compares opaque cursor pixels with the generated artwork, checks fixed UI
pixels retain their exact size, and samples the
exposed right/bottom edges in eight screenshots taken during shrink/grow drags:

```sh
python3 -B tools/test_desktop_snap_host.py
python3 -B tools/test_desktop_transitions_host.py
python3 -B tools/test_desktop_damage_host.py
python3 -B tools/test_files_metadata_host.py
python3 -B tools/test_desktop_snap_files.py --iso build/Coast-codeoss.iso --output-root build/snap-files-new
python3 -B tools/test_codeoss_resize_cursor.py --iso build/Coast-codeoss.iso --output-root build/resize-cursor-new
```

The R154-to-R157 comparison used two CPUs, 16 GiB RAM, 2560×1600 pixels,
identical harnesses, real USB input and one guest at a time:

| Measure | R154 | R157 |
| --- | ---: | ---: |
| First visible editor pixels while moving the cursor | 7.04 s | 3.05 s |
| Startup cursor update p95 | 121.9 ms | 50.5 ms |
| Slowest observed startup cursor update | 1472.9 ms | 152.0 ms |
| Mean moving resize frame interval | 34.2 ms | 20.0 ms |
| Mean resize work per delivered frame | 29.9 ms | 18.0 ms |
| First editor pixels in the separate typing benchmark | 4.68 s | 2.50 s |
| Typing p95, 24 verified characters | 103.4 ms | 77.0 ms |

Median typing delay remained roughly 73–74 ms. These are isolated pairs,
including screenshot/QMP overhead, rather than a guarantee for every workload
or physical display. More resize frames are delivered, so total resize pixel
traffic increases even though work per delivered frame decreases. Software
rendering and interpreter execution remain. Reports and rejected diagnostic
pilots are retained under `build/codeoss-interaction-20261103/`.

The historical interaction test with `--check-changes` also verifies recovery from both side edges, resizing
while offscreen, Dock protection, all resize directions and the text cursor.
It compares each incremental result against a full recomposition using the
same retained client frame. The test-only loader fixture checks immutable
cache hits, canonical paths, same-size mutation, repeated re-verification of
mutable bytes and inode replacement, followed by IPC/POSIX/guest health checks.
The host cursor check runs 200 scale/operation/edge cases with ASan/UBSan and
renders an atlas for inspection.

Native frames now damage only their changed pixel bounds. Glass caches track
changes within their sampled regions instead of invalidating for every change
anywhere in a window. Bounded SSE2 comparison/conversion and byte-span region
updates reduce software graphics work. Input is flushed immediately and the
bridge's idle polling interval is shorter. Unchanged keyboard snapshots no
longer cause redundant server updates; focus and overflow recovery remain.
Fresh profiles use reduced motion, a solid caret and no minimap or smooth
scrolling. Saved preferences remain authoritative.

The current responsiveness pass also uses the supported classic pane layout on
fresh profiles. Floating rounded panes require additional offscreen software
passes for ordinary edits. Frameless workbench windows disable Electron's
extra shadow and corner mask at creation; the native desktop supplies the
visible outer frame. Custom HTML titlebar menus remain enabled.

Read-only `sched_getparam`/`sched_getscheduler` queries return the actual normal
time-sharing policy for live namespace-visible tasks. This prevents repeated
mutex-library errors; it does not add scheduling-policy changes. Clock reads
use their own state protection instead of waiting behind sibling file/VM
operations. `CLOCK_MONOTONIC_RAW` uses the existing unadjusted TSC timebase.
The finite read-only `/sys/devices/system/cpu/cpu0/tsc_freq_khz` file reports
the real boot calibration, letting timing libraries reuse it. It exposes no
device or write controls and does not expand mounted-disk access.

A combined language-service, preview and search session exhausted the old
16-TiB allowance for sparse virtual reservations during helper fork/exec. The
admitted process tree now has a finite 32-TiB reservation allowance. These
reservations do not allocate that much RAM. The 8-GiB charged physical-page
limit, 1380-GiB per-request bound, 16,384 range records, task limits and W^X
checks remain. Boundary selftests exercise rejection, failed fork/exec
rollback, charging, cleanup and reuse at the revised limit.
The completed combined workflow reached 17.28 TiB of reserved space and
7.15 GiB of charged physical pages, passed search and file-watcher checks,
then closed and relaunched cleanly. It exceeded the former reservation bound
while remaining within the physical-page allowance.

A later combined session reached the 192-task ceiling with language services
and previews still open, preventing a search helper from spawning. Even the
previous passing run had peaked at exactly 192 tasks. The domain now admits
206 tasks, with 32 additional global slots for native/kernel helpers and 238
retained identity slots. The 32-process ceiling remains. This capacity fits
the existing 72-KiB filter, 100-KiB PID and 112-page total metadata ceilings;
embedded metadata is charged at its revised size. The quota fixture reaches
206 tasks, rejects another thread, joins all threads and verifies cleanup.
Namespace/filter accounting and compact signal-identity boundary selftests
also exercise the revised layout. Physical and virtual memory limits remain.
The revised combined session passed all 45 checks at a peak of 198 tasks and
18 processes, leaving eight task slots. It used 17.28 TiB of sparse virtual
reservations and 7.23 GiB of charged physical pages, then closed and relaunched
with all preferences retained and no live credential references left behind.
No task-limit or helper-spawn errors occurred.

The native graphics path carries changed regions through the entire native
window path. The Wayland adapter converts and compares pixels once, using
the committed surface damage, scale and shadow crop. It skips unchanged
frames. The kernel stages partial writes before publishing them and collects
changes until the desktop successfully reads them. Skipped frames are merged;
a stale retained sequence or a resize requires a full refresh. The desktop
keeps one retained bitmap instead of copying and comparing two full images.
This removes a 15.625-MiB spare desktop bitmap for a maximum-size native window.
Existing full-frame clients retain their original ABI.

`OS_WINDOW_PRESENT_DAMAGE` and `OS_WINDOW_SERVER_READ_DAMAGE` use the additive
48-byte `os_window_damage_frame`. Bounds and owner/display permissions are
checked before transfer. Failed client reads cannot publish partial pixels;
failed server reads do not consume pending damage. Surface damage follows the
[Wayland coordinate and commit rules](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_surface).

The workbench now uses the native Wayland callback to pace software output,
without Chromium adding another synthetic frame-rate wait. The native frame
callback follows the desktop's bounded 120 Hz software cadence and advertises
that rate to Code OSS. Other clients retain their nominal 60 Hz mode. Callback
deadlines advance only when a committed callback is delivered, so an idle
timer cannot add an unrelated wait to the next frame. Custom menus
also escape the menubar's clip after native fullscreen/restore and focus
changes create an opacity group. The package records the actual installed
asset checksums, preserving the upstream integrity check without a false
corrupt-installation warning for the port stylesheet.

The pinned scrollbar stylesheet checks an older animation-disable class and
does not honor the workbench's current reduced-motion class. The port now
disables scrollbar fades when reduced motion is enabled. Normal scrollbar
visibility and saved motion preferences remain available. This removes a
repeatable long-line typing stall when the horizontal scrollbar first appears.

An isolated R149-to-R154 comparison used the same harness and observer,
192 physical USB letter keypresses, verified saved bytes, two CPUs, 16 GiB RAM
and 2560×1600 pixels. Each image settled for 30 seconds, followed by three
ten-second idle measurements, before typing:

| Measure | R149 | R154 |
| --- | ---: | ---: |
| Median key-to-visible-glyph latency | 73.1 ms | 74.5 ms |
| 95th percentile latency | 100.7 ms | 103.5 ms |
| Slowest sample | 359.1 ms | 132.1 ms |
| Scrollbar-boundary keypress | 359.1 ms | 72.2 ms |
| Steady idle QEMU CPU use | 12.5% | 13.4% |
| Cold first visible editor pixels | 4.62 s | 4.58 s |

The slowest update fell 63%; typical latency did not materially change.
A preliminary R151 build with the same bridge and stylesheet also completed
a 56-key pixel capture, crossing the scrollbar boundary in 89 ms with no
update above 102 ms. R151 was not selected because the long combined session
subsequently exhausted its task ceiling. The measurements include screenshot
and QMP overhead and do not establish zero latency or every workload's
performance. Diagnostic captures also confirmed that keeping the scrollbar
always visible or disabling workbench shadows did not fix the stall; those
preferences were not applied. Exact comparison reports are recorded in
`build/codeoss-stalls-20261103/PERFORMANCE.json`.

The historical R142-to-R149 comparison uses identical harness and observer hashes
within each pair, real USB keypresses, verified saved bytes, two CPUs, 16 GiB
RAM and 2560×1600 pixels. Each image ran alone. The larger run measured 72
keypresses after 30 seconds of settling and three ten-second idle samples.
Screenshots are collected during the keypress:

| Measure | R142 | R149 |
| --- | ---: | ---: |
| Median key-to-visible-glyph latency | 97.7 ms | 73.0 ms |
| 95th percentile latency | 130.9 ms | 103.5 ms |
| Slowest sample | 827.4 ms | 537.5 ms |
| Steady idle QEMU CPU use | 11.8% | 12.3% |
| Cold first visible editor pixels | 4.59 s | 4.67 s |

This pair measured 25% lower median typing delay and 21% lower p95 delay. A
separate 24-key comparison measured 96.6→73.3 ms median, while its p95 changed
101.8→104.6 ms. Long delays still occur; the larger run's slowest candidate
sample was about 538 ms. Startup changed only slightly. Idle CPU did not show
a consistent reduction: the shorter warmup sample changed 11.4→17.2%, while
the longer steady samples averaged roughly 12% for both images. CPU percentages
are relative to one CPU core.

The measurements include QMP and screenshot overhead; these pairs do not
establish every workload's latency. Software rendering and interpreter
execution still limit performance.
The harness now waits for the actual Quick Open result before pressing Enter,
so both runs verify that every benchmark character reaches the saved file.
Exact reports and limitations are in
`build/codeoss-responsive-20261103/PERFORMANCE.json`.

The historical R137-to-R142 measurement uses the same updated harness and lightweight
observer, 24 real USB keypresses, exact saved bytes, two CPUs, 16 GiB RAM and
2560×1600 pixels. No other qualification guest ran during either measurement:

| Measure | R137 | R142 |
| --- | ---: | ---: |
| Median key-to-visible-glyph latency | 101 ms | 97 ms |
| 95th percentile latency | 131 ms | 103 ms |
| Slowest sample | 131 ms | 152 ms |
| Idle QEMU CPU use during settling | 11.1% | 15.1% |

This is a modest typing improvement (4% median, 21% p95), with a slower outlier.
In R142, 127 buffer commits would have required 396.2 million pixels of full
transfers; the native path transferred 31.8 million, avoiding 92% of that work.
It scanned only 56.4 million pixels and skipped 29 unchanged commits. A separate
instrumented diagnostic run measured median key-event-to-animation-frame wait
falling from 30.7 to 10.5 ms. That diagnostic used a heavier observer and ran
alongside qualification guests; it is not an end-to-end latency comparison.
Occasional stalls and the existing interpreter/software-rendering limits remain.

The historical R128-to-R137 comparison used the same observer, 24 physical USB
keypresses, actual screenshot changes and exact saved bytes, at 2560×1600 on
two virtual CPUs with 16 GiB RAM, using 50-ms key holds and ten seconds of
settling after editor/font warmup:

| Measure | R128 | R137 |
| --- | ---: | ---: |
| Median key-to-visible-glyph latency | 105 ms | 72 ms |
| 95th percentile latency | 133 ms | 107 ms |
| Slowest sample | 134 ms | 195 ms |
| Desktop pixels uploaded during the benchmark | 623.8 million | 79.6 million |
| Desktop composition time | 2.49 s | 0.53 s |

These runs show about 31% lower median typing latency, 19% lower p95, 87% less
pixel traffic and 79% less desktop composition time. The slowest sample was
worse, so occasional stalls remain. Cold first-pixel times were 5.84 s and
5.11 s respectively; one pair of runs is not a startup-speed guarantee. Electron still uses interpreter execution and software rendering,
which limit how close this port can get to a normal hardware-accelerated editor.
That older sampling loop waited for key release before capturing pixels. The
current loop captures during the press; compare results only from identical
versions of the harness and observer.

## Build and verify

With the preserved compiler and runtime caches available, rebuild the kernel
when its sources change, then package the pinned workbench. Use unused output
paths:

```sh
python3 -B tools/build_electron_iso.py \
  --iso build/Coast-workbench-dac-new.iso
python3 -B tools/build_codeoss_iso.py \
  --base-iso build/Coast-workbench-dac-new.iso \
  --output-root build/codeoss-workbench-new \
  --runtime-build build/electron-wasm-runtime-r7-worker01/BUILD.json \
  --native-node-build build/node-native-r4-worker01/BUILD.json \
  --diagnostics
python3 -B tools/test_codeoss_desktop.py \
  --iso build/codeoss-workbench-new/Coast-codeoss.iso \
  --output-root build/codeoss-desktop-test-new --usability-check
python3 -B tools/benchmark_codeoss.py \
  --iso build/codeoss-workbench-new/Coast-codeoss.iso \
  --output-root build/codeoss-speed-test-new
python3 -B tools/test_native_graphics_host.py \
  --output-root build/codeoss-graphics-test-new
python3 -B tools/test_native_window_damage.py \
  --iso build/codeoss-workbench-new/Coast-codeoss.iso \
  --output-root build/codeoss-window-test-new
python3 -B tools/test_scheduler_queries.py \
  --iso build/codeoss-workbench-new/Coast-codeoss.iso \
  --output-root build/codeoss-scheduler-test-new
python3 -B tools/benchmark_codeoss_interactions.py \
  --iso build/codeoss-workbench-new/Coast-codeoss.iso \
  --output-root build/codeoss-interaction-test-new
python3 -B tools/test_desktop_cursor_host.py \
  --output-root build/codeoss-cursor-test-new
```

For the current snap layouts, watched folders, desktop files and terminal
working-directory flow, run `tools/test_desktop_workflow.py --iso IMAGE
--output-root UNUSED_DIR`. The older interaction test's `--check-changes`
assertions expect release-at-edge to leave a window offscreen, before drag
snapping was added; use its default performance pass for current images.
`tools/test_desktop_snap_host.py` checks exact layout tiling and restore bounds
with ASan/UBSan. `tools/test_codeoss_notifications.py` runs the real unprivileged notification
fixture through Electron, including mask replacement/addition, access/open/close,
rename, deletion, one-shot watches, overflow and instance cleanup.
`vector-io-check` inside the guest checks positional-vector
I/O, short EOF, offset preservation, access errors and validation before writes.

For the test-only digest-cache verifier, build a separate base with
`tools/build_electron_iso.py --selftest --iso UNUSED.iso`, then run
`tools/test_electron_startup_cache.py --iso UNUSED.iso --output-root UNUSED_DIR`.

`--diagnostics` includes observations enabled only by opt-in QA launches. The
integration test drives the real launcher, Files, New File and Save As using
emulated input. It checks exact bytes and ownership, ordinary filesystem
denials, shared `/tmp` protection, outer-window identity, duplicate controls,
startup cancellation, Dock restore, relaunch, cleanup and guest health. It
compares incremental pixels against a forced full desktop recomposition before
another application tick, using exactly the same retained frame even when the
client animates. The two later screenshots remain available for visual review.
Its additional CLI launches open `/tmp` and `/usr` as UID 1000.

Guest tests use KVM, two CPUs and 16 GiB RAM with no network or data disks.
Reports, logs, harness snapshots and screenshots remain in their output
folders. The benchmark substitutes a small QA observer only in disposable
guest RAM, avoiding the heavy full-DOM diagnostic snapshots in both images.
The graphics helper checks use guarded allocations and ASan/UBSan.
The native-window suite checks full frames, exact partial regions, coalesced
updates, stale sequences, a fault after the first staged row, failed-read
retry, overflowing pointers and bounds, ownership, resize and legacy clients.
The typing benchmark observes pixels while a physical key is held, avoiding
an artificial key-hold floor, and checks the entire saved file for lost or
repeated characters.

The preserved `verify_codeoss_workbench_local.py` harness uses
`--workbench-dock-role 9 --desktop-window-controls` with `--window-check` for
this window layout. Its default control expectations support older images.
The current qualification exercises real menus after native resize/fullscreen,
ten full typing sequences, held-key repeat, pointer selection, large-file wheel
scrolling, editing, file operations, the integrated terminal, editor services,
Git and metadata limits. It checks border resize, maximize/restore, Dock restore,
and fullscreen with exact ordinary/maximized restore geometry.
The preserved security harness checks credentials, namespace roots, seccomp,
process/descriptor controls and existing memory bounds on the same image.

The latest qualification also checks exact Unicode and CRLF file contents,
large-file navigation, clipboard transfer in both directions through 64 KiB,
offline extension installation and activation, JavaScript completion,
TypeScript diagnostics, JSON formatting and errors, Markdown preview, shell
tasks, workspace replacement and saved preferences across relaunch. Scheduler
and timer fixtures cover thread lifetime, errors, seccomp precedence, actual
TSC calibration, partial metadata reads, EOF and read-only permissions.

The first November 3 performance and permission evidence is under
`build/codeoss-speed-20261103/`; the next native graphics/integration pass is
under `build/codeoss-native-20261103/`. The earlier responsiveness evidence is
under `build/codeoss-snappy-20261103/`. The selected desktop/launch improvements
and final R175 workflow evidence are under `build/desktop-upgrade-20261104/`. The candidate's `QUALIFICATION.json`
records completed checks and exact hashes. R157 is retained as the previous
qualified image. Its cleanup receipt lists removed obsolete candidate images,
packaged runtime copies and desktop objects, while retaining build records and
active dependencies. Electron/Node binaries, W^X, sandbox controls, resource bounds and the
Git IPC fix are retained. The kernel change replaces the old global path
write restriction with inode permissions and adds shared-directory sticky-bit
checks for deletion and both sides of rename. Empty-discard validation retries
a complete scan at most three times after unrelated VM generation changes;
stable denials still fail. Deterministic kernel selftests cover successful
retry, persistent mutation, real backing and bounds. Fault observations now
include pending termination signals, distinguishing tasks already killed by
the supervisor from faults during normal execution.

The customization and compact launcher workflow is checked with
`python3 -B tools/test_desktop_customization.py --iso build/Coast-codeoss.iso --output-root build/customization-new`.
It verifies the collapsed search field, explicit disclosure, filtered sizing,
inline right-click color dots, applied controls, disabled snapping helpers and
saved preferences after a fresh GUI process.
