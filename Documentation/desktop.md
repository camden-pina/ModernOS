# ModernOS desktop

Build with `make iso`, boot with `make run SMP=4`, and enter `gui` at the
ModernOS shell. The boot console remains available. `Ctrl+Alt+Escape`, or
**Return to console** in the launcher, exits the desktop and restores console
input. Unsaved documents require an explicit save or discard before exit.
Cancelling exit leaves the other windows and their processes running. Closing
a dialog's owner dismisses that dialog before its window state can be reused.
An unsaved document is brought forward from a different workspace or a
minimized window before its close/exit confirmation is shown.
`gui --info` reports display capabilities without taking the display;
`gui --stats` reports presentation traffic, composition/upload microseconds,
drag timings and cached surface updates on exit. `--trace` enables
serial action logs used by the integration tests.

This is the OS's native userspace desktop. Files reads the existing VFS through
`getdents64` and `stat`, and all mutations use real filesystem syscalls. There
are no production demo entries, fabricated storage numbers, or host GUI libraries.

## Desktop and applications

The preferred Limine framebuffer is 1440×900. At 100% scale the initial Files
window is approximately `(158,72,1123,709)`, with a shared 40-pixel toolbar,
185-pixel sidebar, compact folder cards and file rows. The procedural mineral
wallpaper is cached; the 42-pixel taskbar stays attached to the bottom. Smaller
windows use a narrower sidebar, one folder column, and fewer metadata columns.
Settings also supports 125%, 150%, and 200% scaling when the screen can fit
usable windows. The system uses embedded antialiased DejaVu Sans/Mono glyphs
and a consistent, locally drawn outline icon set.

| Component | Implemented behavior |
| --- | --- |
| Windows | Focus, stacking, dragging, 6-pixel resize zones, minimize/restore, maximize/restore with saved geometry, snapping, close, clipping and common decorations |
| Workspaces | Three initially; independent visible windows and focus; geometry survives switches; windows move between workspaces without closing |
| Taskbar | Fixed Files/Terminal/Browser/Settings positions; focused, running and other-workspace indicators; click to focus/restore; right-click window chooser and New window; Text Editor appears while running |
| Launcher | Compact anchored application search; native application registration; keyboard navigation; Return to console |
| Files | Real Home/Downloads/Projects/Pictures/System, Recent, and Trash; breadcrumbs; 32-entry back/forward history; editable path; refresh; hidden-file toggle; name/modified/size sorting; hybrid and detail views |
| File operations | Selection, Ctrl/Shift multi-selection, select all, open, rename, new folder/file, copy/cut/paste, recursive directory copy, drag selected items into child folders, Trash, restore, and file information |
| Text Editor | Real documents, cursor/selection, copy/cut/paste, tabs, line numbers, vertical/horizontal scrolling, Save/Save As, external-change detection, read-only handling and unsaved-close confirmation |
| Terminal | A persistent native `/bin/modernos-sh --terminal` process over a real PTY; command history/scrollback, VT screen rendering, Ctrl/Alt/navigation/function keys, resize notifications and confirmation before closing a running program |
| Browser | Genuine NetSurf in a separate process; native editable URL field, back/forward/reload/stop, actual HTML/CSS/JS/images/forms, HTTP/HTTPS and safe downloads |
| Settings | Accent, wallpaper previews and custom images, Fill/Fit/Stretch/Center and background colors, scaling, workspaces, shared light/dark appearance, immediate application and saved preferences |
| Clock | Actual Limine firmware boot time plus the OS monotonic timer, displayed in UTC; unavailable state if the time source is absent |

Home is the real writable RAM directory `/home/user`. Initialization creates
its ordinary Projects, Downloads, Pictures, Pictures/Wallpapers, `.Trash`, and `.config` directories;
it does not populate example documents. Files initially opens Projects when
available, otherwise Home. Documents in RAM disappear on reboot. Use a writable
disk mount, such as `/mnt/nvme1n1p1`, for persistent documents.

Search is a case-insensitive filename filter in the current folder. Its scope
is stated in the Files status area. Recent tracks actual opened files for the
current desktop session, up to 48 paths. Neither view pretends to implement
recursive indexing or persistent access history.

Regular text files open in Text Editor after a binary check. The editor also
checks the entire document. Binary and special files receive an informative
Open With notice. HTML/HTM and PNG/JPEG/GIF/BMP/SVG open in Browser. PDFs and
executables have no registered GUI viewer.
Supported executable programs can run through Terminal.

## Keyboard and mouse

| Keys or interaction | Action |
| --- | --- |
| Super / Super+Space | Toggle launcher |
| Super+1/2/3 | Switch workspace |
| Super+Shift+1/2/3 | Move focused window to a workspace |
| Super+Left/Right | Snap focused window |
| Super+Up/Down | Maximize; restore or minimize |
| Alt+Tab / Alt+Shift+Tab | Cycle windows on the active workspace, including minimized windows |
| Alt+F4 | Close focused window; Text Editor confirms unsaved edits |
| Double-click title bar | Maximize/restore |
| Right-click title bar | Window actions, including workspace moves |
| Right-click taskbar application | Choose a real existing window or open another |
| Ctrl+L / Ctrl+F in Files | Edit path / search filenames in this folder |
| Ctrl+L in Browser | Focus and select the real address field |
| Alt+Left/Right, F5/Ctrl+R in Browser | History back/forward, reload |
| Escape while Browser loads, toolbar stop | Cancel the current real page load |
| Ctrl+N in Browser | Open another native browser window |
| Backspace / Alt+Left / Alt+Right / Alt+Up | Back / back / forward / parent |
| Enter / double-click entry | Open selected file or directory |
| Ctrl+A, Ctrl+C/X/V | Select all, copy/cut/paste in Files and Text Editor |
| F2 / Delete | Rename one selected entry / move selection to Trash |
| Ctrl+N / Ctrl+Shift+N in Files | Create file / folder |
| F5 / Ctrl+R | Refresh Files |
| Ctrl+S / Ctrl+Shift+S in Text Editor | Save / Save As |
| Ctrl+Home/End, Shift+movement | Document start/end, extend text selection |
| Shift+click in Text Editor | Extend text selection to the clicked position |
| Mouse wheel / Page Up/Down | Scroll supported application content |
| Tab / Shift+Tab | Navigate Files controls, menus, dialogs and Settings |
| Escape | Dismiss menu/dialog, clear Files search, or cancel an active copy |
| Ctrl+C in Terminal | Interrupt child commands in the shell view; full-screen applications receive the actual control byte |
| Ctrl+Alt+Escape | Leave the desktop safely |

Global shortcuts are routed by the desktop before application keys. Window
controls, title-bar drag regions and Files navigation share one toolbar and
separate hit regions. Hover invalidation uses the same control geometry as
hit testing; hover does not change persistent selection. Menus constrain their
size and position to the work area and scroll when too many rows fit onscreen.

## File operations and saved data

Copy jobs run in 32-KiB chunks, and directory enumeration processes 16 entries
per desktop tick. Input continues between these steps. Copies never overwrite
an existing destination, follow directory symlinks, or recursively copy a
directory into itself. Copying preserves the permissions supported by the
destination filesystem. FAT32 represents writable/read-only permissions using
its read-only attribute, rather than full Unix modes.

Cut uses a same-filesystem rename. A regular file moved across mounts is copied
and flushed before its source is unlinked. Cross-mount directory cuts are not
implemented: copy the folder, verify the result, then move the original to
Trash. Cancellation or a failure can leave completed files and directories;
the dialog reports this. The unfinished regular file is removed when possible.
There is no claim of a transaction across an entire copied tree.

Trash moves an entry to `.Trash` on the same real filesystem, with a separate
`.info` record storing its original path. RAM entries use `/home/user/.Trash`.
The sidebar aggregates these locations. Restore refuses an occupied original
path. Permanent deletion supports regular files and empty directories;
nonempty directories must be emptied first. Read-only mounts and protected or
busy entries report the underlying filesystem error.

Text Editor saves to a new sibling `.modernos-save-PID-WINDOW` file, flushes it,
moves an existing original to `.modernos-save-PID-WINDOW.bak`, then renames the
staged file into place. It attempts rollback if replacement fails and reports
backup cleanup/recovery failures. Existing staging files are preserved rather
than overwritten. The original document is hashed before replacement so a
change by another application is reported. Save As refuses existing targets and
allows a writable copy of a read-only document. Original text bytes, including
loaded CRLF and UTF-8 sequences, are retained unless edited.

These checks protect normal operations; FAT32 has no journal or power-loss
transaction guarantee. The GUI does not report durable disk storage for RAM
files. Recovery files remain ordinary inspectable filesystem entries.

Preferences are saved only when **Save preferences** or Ctrl+S is used in
Settings. The configuration path is shown there. The desktop selects
`/.modernos/desktop.ini` below the first writable FAT32 mount when present;
otherwise it uses `/home/user/.config/desktop.ini` in RAM. Disk values load on
the next launch and after reboot. RAM values load on another launch in the
same boot. Settings explicitly identifies which storage is in use. Selecting
a configuration path does not write anything until Save is invoked.

**Wallpaper** in Settings includes previews of the existing Mineral, Dusk and
Moss backgrounds, plus **Alpine Lake** and **Desert Sunset**. Both sample PNGs
are shipped in the ISO's initramfs at `/usr/share/wallpapers`. The **Built-in
wallpapers** button opens that directory in Files; **My wallpapers** opens
`/home/user/Pictures/Wallpapers`. **Choose image...** accepts an absolute path
or a path relative to Home. You can also right-click a PNG, JPEG or BMP in
Files and select **Set as wallpaper**. The image is applied immediately;
**Save preferences** or Ctrl+S records the selection.

Image wallpapers support four placements: **Fill** preserves aspect ratio
and crops to cover the desktop, **Fit** preserves aspect ratio and shows the
whole image, **Stretch** covers the desktop without preserving aspect ratio,
and **Center** uses the image's original size. Slate, Black and Cream background
colors fill uncovered space and show through transparent PNGs. Placement and
background controls are enabled for image wallpapers. **Reset wallpaper**
restores Mineral, Fill and Slate. All controls support keyboard focus and
scroll into view on smaller windows.

The native decoder supports PNG, JPEG and uncompressed 24/32-bit BMP, with a
maximum of 8192 pixels per edge, 16 megapixels and a 64-MiB encoded file. It
runs as a short-lived static process and returns bounded ARGB pixels; the
desktop caches the decoded selection, thumbnails and composed wallpaper.
Changing accents or appearance does not decode the image again. A failed
selection keeps the current wallpaper. A missing or invalid saved image falls
back to Mineral and displays a notice in Settings.

Optional `wallpaper_mode`, `wallpaper_background` and `wallpaper_path` fields
extend `desktop.ini`; existing numeric wallpaper IDs 0–2 retain their meanings.
IDs 3 and 4 select the samples; ID 5 uses the saved custom path. The image must
remain at that path for a saved selection to reload. Personal wallpapers in
RAM disappear on reboot; place them on a supported persistent disk to retain
them across boots.

**Appearance → Light / Dark** changes the shared semantic palette immediately,
including Files, Text Editor, Terminal, Settings, window decorations, launcher,
menus, inputs and dialogs. Dark surfaces use slate backgrounds, light text,
and readable jade/blue/slate selections. Wallpaper and taskbar retain their
separate desktop colors. The saved `dark=0|1` field is optional, so existing
configuration files continue to load with the light default. Settings scrolls
on short windows; keyboard focus scrolls the selected preference into view.

## Source ownership

| File | Responsibility |
| --- | --- |
| `userland/gui.c` | Native application entry point |
| `userland/desktop/desktop.c`, `desktop.h` | Application registration, event loop, global shortcuts, input dispatch, clock, lifecycle and shared types |
| `userland/desktop/theme.[ch]` | Shared semantic light/dark palette and visual tokens |
| `userland/desktop/render.c` | Scale, clipping, primitives, fonts, widgets, path helpers, dirty composition and presentation |
| `userland/desktop/wallpaper.c`, `userland/wallpaper-decode.c` | Procedural and image wallpapers, bounded native image decoding, previews, placement and caches |
| `userland/desktop/pixel_blend.c`, `pixel_blend.h` | Exact scalar/SSE2 XRGB blending and measured CPU/emulator selection |
| `userland/desktop/icons.c` | Reusable outline icons and cached antialiased masks |
| `userland/desktop/window_manager.c` | Shared decorations, geometry, stacking, window actions and workspace ownership/focus |
| `userland/desktop/taskbar.c` | Taskbar, real window chooser, launcher, workspace controls and tray notices |
| `userland/desktop/widgets.c` | Shared menus, scrolling menus, input dialogs, errors and confirmations |
| `userland/desktop/file_manager.c` | VFS adapter, browsing, sorting, selection, history, clipboard, cooperative copy and Trash |
| `userland/desktop/editor.c` | Document model, text interaction and staged saves |
| `userland/desktop/terminal.c` | Native shell process, PTY streams, cooked/raw input routing, scrollback and child cleanup |
| `userland/desktop/terminal_screen.[ch]` | Bounded VT/ANSI screen, cursor, scrolling regions, erasing, SGR colors, UTF-8 decoding and raw key encoding |
| `userland/desktop/browser.c`, `userland/include/browser_protocol.h` | Native browser controls, separate engine process, bounded client pixels/input and lifecycle |
| `userland/browser/`, `tools/build_browser.py` | Genuine NetSurf surface/navigation/download adapters, packaged resources/fonts and pinned upstream build |
| `userland/desktop/settings.c` | Supported preferences, scaling, workspace count, configuration persistence and real home initialization |
| `userland/desktop/font_data.[ch]` | Embedded glyph atlas; generated offline by `tools/build_gui_fonts.py` |
| `userland/desktop/assets/FONT-LICENSE.txt`, `tools/mkinitramfs.py` | DejaVu notices, included in the boot image at `/usr/share/licenses/modernos-desktop/FONT-LICENSE.txt` |
| `userland/Makefile`, `arch/x86/boot/boot.conf` | Desktop objects, linking and preferred display resolution |

The window manager, decorations, launcher, taskbar and workspace state are
shared system components, outside Files. Native applications register through
the `AppOps` interface and use the same windows/widgets. The existing compositor
entry was replaced in place; the existing kernel framebuffer, USB input,
memory, timer, VFS and task infrastructure remain the backend.

Kernel integration is limited to the interfaces needed by these applications:

* `include/uapi/abi.h`, `drivers/video/fbdev/core/display.c`,
  `drivers/video/fbdev/core/fbcon.c`, and their `include/video/` declarations:
  display ABI 3 capabilities/completed-canvas presentation, while retaining ABI 1/2 operations.
  `scanout.c`, `video_copy.S`, `mm/vmem_io.c` and `mm/user.c` provide verified
  QEMU scanout, framebuffer cache policy and exact transfers.
* `usb_input.c` and `include/video/display.h`: raw HID press/release/modifier
  events for the existing display owner; ordinary console input stays intact.
* `kernel.c` and `kernel/sched/syscall.c`: real firmware boot epoch and realtime clock.
* `include/fs/vfs.h`, `fs/vfs.c`, `fs/binfmt_elf.c`, `kernel/sched/sched.c`,
  `drivers/tty/pty.c`, `include/pty.h`, `include/sched.h`, and `kernel/sched/syscall.c`:
  real pipes and PTYs, explicit standard-stream spawning, terminal sizing,
  foreground ownership and endpoint cleanup when a process exits.
* `kernel/sched/signals.c`, `fs/binfmt_elf.c`, `mm/user.c` and the syscall dispatcher:
  signal, static TLS, environment and memory-guard support needed by the
  native musl/ncurses/nano runtime. See `native-nano.md` for ABI limits.
* `fs/ramfs.c`, `fs/ext.c`, and stat copying in `kernel/sched/syscall.c`: real available
  timestamps. FAT32 now reads/writes its actual stored dates and times, with
  format precision limits and an unknown Unix inode-change timestamp.
* `userland/sh.c`, `userland/libc.c`, `userland/check.c` and
  `userland/posix-check.c`: native shell mode, error reporting and ABI validation.

The committed font atlas is sufficient for an ordinary build. Regenerating it
requires Python Pillow and the locally installed DejaVu font files specified
by `tools/build_gui_fonts.py`; these are not runtime dependencies.

## Rendering and kernel interfaces

The compositor retains a cached wallpaper and one clean composed canvas. It
stamps the cursor into the canvas only for synchronous presentation, then
restores the small covered area. Cursor movement damages old/new pointer bounds;
control changes damage their exact regions. Moving windows retain their cached
decorated surfaces. Content, focus, size and theme changes invalidate those
surfaces. Shadows cache geometry, opacity and foreground products, and blend
only exposed strips with packed two-pixel arithmetic.

Front-to-back opaque coverage skips wallpaper and lower windows hidden behind
upper surfaces. Dirty regions retain clipping and correct stacking; the taskbar
and popups remain above normal windows. Input is processed immediately, with a
120 Hz composition target and a 1 ms scheduler/input wakeup. Damage stays pending
until the next frame deadline. Idle desktops do not continuously repaint.
Consecutive motion reports coalesce while keys, buttons, device transitions and
wheel reports keep their order. Release motion is applied before ending a drag,
and moving a drag clears the title-bar double-click candidate.

Display ABI 3 adds capabilities and completed-canvas presentation. A verified
QEMU standard VGA/Bochs DISPI backend uploads to a hidden framebuffer page, repairs
that page's previous damage from the complete canvas, fences writes and changes
the viewport only after the frame is ready. It preserves the firmware mode,
restores page zero on release/fault/legacy presentation and uses two pages only
when the actual VRAM and mode permit it. `make run` configures 32 MiB VGA VRAM
and the SDL OpenGL frontend, which uploads the CPU-rendered framebuffer to a
host GPU texture. The run recipe selects KVM when accessible, with TCG as its
portable fallback, and prints the chosen configuration. `make run-fast` requires
KVM. `QEMU_DISPLAY=sdl,gl=off` selects the software frontend when necessary.
When WAYLAND_DISPLAY is available, the launcher sets SDL_VIDEODRIVER=wayland
so SDL uses native relative pointer capture. `QEMU_SDL_DRIVER=x11` selects X11;
an explicitly configured SDL_VIDEODRIVER is also respected. Other hosts retain
SDL's default driver. Click inside the window to capture the mouse and use
Ctrl+Alt+G to release it. The previous GTK/XWayland setting passed synthetic
XTest checks but left the physical mouse frozen on the tested KDE Wayland host.
SDL/OpenGL was verified with physical pointer movement and window dragging.
VGA's `refresh_rate=120` sets EDID metadata rather than the frontend timer;
SDL refreshes every 10 ms during host input activity and reduces its refresh
rate when idle. GTK's timer follows the host monitor. See the upstream
[VGA properties](https://github.com/qemu/qemu/blob/master/hw/display/vga-pci.c),
[SDL capture/refresh code](https://github.com/qemu/qemu/blob/master/ui/sdl2.c) and
[SDL OpenGL code](https://github.com/qemu/qemu/blob/master/ui/sdl2-gl.c).
Ordinary integration tests intentionally use headless TCG for consistency;
`make test-drag-native` exercises the visible KVM/OpenGL frontend.
Its QMP input injection does not activate SDL's faster host-input timer, so
the SDL benchmark reports idle-rate QEMU surface publication separately from
guest work. `make test-mouse-live` uses the expanded make run recipe, opens the
desktop without injecting pointer events, and records guest pointer events
and render statistics. Click, move and drag using the physical mouse; close
the window or create the printed stop file to finish. `make test-host-mouse`
uses synthetic XTest events through GTK; it cannot verify physical capture on
Wayland. That separate check requires X11/XWayland, libX11, libXtst and KVM.

Physical Intel/NVIDIA and other unsupported display controllers retain the
Limine framebuffer. Video mappings reuse an existing write-combining PAT entry
when safe, with an uncached fallback; APs interpret shared mappings consistently.
Fully covered, aligned video-aperture ranges use validated 2-MiB page-table
leaves where possible, reducing translation overhead without changing cache
policy or covering unrelated MMIO. `gui --info` exposes the actual capability.
The composition deadline advances on its existing cadence, avoiding cumulative
polling delay; idle/overrun cases reset the deadline without delaying input.
Native XRGB transfers use exact aligned, non-temporal SSE2 stores on real CPUs
and hardware virtualization, preserving used SIMD registers and fencing writes.
Measurements select the same wide transfer path in TCG; an exact REP
fallback handles mappings without write combining. Other pixel layouts use a
fixed conversion row. No hardware address is exposed to userspace. Presentation uses the exclusive
display mutex rather than per-row framebuffer locks, so IRQs stay enabled.

The kernel checks ownership, geometry, pointers, stride and budgets before
presentation. Full-canvas validation is cached by process, address-space root,
VM generation, source and stride; map/protect/unmap invalidates it. ABI 1 and
legacy 528-byte ABI 2 batches remain supported. New batches allow up to 128
regions; the desktop currently accumulates at most 32 composition regions.

There is no live blur, intermediate black frame or full-screen redraw for
ordinary pointer motion. The two desktop pixel surfaces use about 9.9 MiB at
1440x900. Each retained window adds width x height x 4 bytes (about 3 MiB for
initial Files) and cached opacity only for exposed shadow bands, released when
closed. Cached opacity preserves zero-opacity pixels exactly. Wide uniform
spans use four-pixel SSE2 blending on physical CPUs and hardware virtualization;
QEMU TCG retains the measured faster two-pixel integer path. The selector reads
the emulator's actual CPUID signature once. Both paths match the same rounding
and XRGB masking, including unaligned spans and short tails. `gui --info` reports
the selected path. Window copies use an exact-span cached
SSE2 path on physical hardware
and QEMU. Allocation failure
falls back to direct window painting. QEMU buffered scanout avoids showing
partially uploaded frames, but neither it nor the generic framebuffer guarantees
vertical-blank synchronization. Intel/NVIDIA display modesetting, synchronized
page flipping, hardware cursor and GPU acceleration remain missing drivers;
physical tear-free output cannot be claimed without them.

`INPUT_HID_KEY` retains the existing 32-byte event size and carries a USB usage,
ASCII mapping, modifier bitmap and press/release state. Legacy serial text and
ANSI key events still work. The event queue reports overflow instead of hiding
dropped input. The GUI clears held modifiers/repeat state on overflow.

`pipe2` (293) provides real bounded pipes, nonblocking reads/writes, EOF on the
last writer's close, shared duplicated endpoints and `-EPIPE` when no readers
remain. Blocking pipe operations do not hold the global VFS mutex.
`spawn_io` (404) takes `(path, argv, os_spawn_io*)`; its 24-byte specification
chooses validated existing descriptors for stdin/stdout/stderr and an optional
real directory. These children run without taking foreground display ownership.
Native shell children inherit these standard streams; arbitrary descriptors
are not inherited. `pty` (406) allocates a real master/slave pair with independent
bounded input/output buffers. The desktop connects the slave to the native
shell, retains the master, and switches to the VT screen when a program disables
canonical input. `TIOCSWINSZ` publishes the actual viewport and delivers
`SIGWINCH` to its foreground program. GNU nano and Vim use this same path; no editor
screen or filesystem outcome is simulated.

## Explicit remaining dependencies and limits

* Wi-Fi and sound are identified placeholders. Browser uses genuine NetSurf
  and the kernel IPv4/e1000 backend. Unsupported network hardware reports its
  actual missing capability; local pages remain available. Modern web APIs
  beyond NetSurf's support remain unavailable. Desktop browser launches use
  kernel syscall/filesystem restrictions; process isolation per site remains unavailable.
  See [browser-network.md](browser-network.md).
* VFS has no capacity/free-space API. System capacity labels and the utilization
  bar are omitted. RAM, ext and FAT32 expose available actual timestamps;
  unknown fields remain unavailable.
* Terminal supports GNU nano and the implemented VT/ANSI subset, not every
  terminal extension. Job control, process groups, public `/dev/pts` paths,
  spell checking and suspension remain unsupported. Native fork/vfork and
  SIGCHLD are implemented; Vim uses ash for shell filters. For nano external
  commands select `env SHELL=/bin/ash nano FILE`. The kernel rejects missing operations. Nano's suspension prompt
  refers to `fg`, which this OS does not provide; use Ctrl+X to return to the
  shell. `TERM=vt100` is the default; arbitrary external terminfo databases
  are absent.
* Glyph coverage is Latin-1 with fallback for other Unicode; stored UTF-8 is
  preserved and cursor movement respects byte boundaries. Keyboard text input
  uses the existing US ASCII mapping. Complex shaping and input methods are
  absent. GUI Text Editor lacks undo/redo and mouse-drag text selection;
  GNU nano provides its own upstream undo/redo and selection operations.
* Symbolic links can be browsed/opened through the existing VFS resolver.
  Copying link nodes and traversing links during recursive copying are refused;
  RAM supports actual symlink creation; FAT32 reports ENOTSUP. GUI copying
  and preserving symlink objects remain unsupported.
* Bounds: 16 GUI windows, 2048 entries per directory/view, 128 clipboard entries,
  16 directory-copy levels, 32 history locations and 256-KiB text documents.
  Limits produce explicit errors or a visible listing-limit notice.
  Display bounds are 640×480 minimum, 8192 pixels maximum width, and 64 MiB
  maximum per private surface.
* Native GUI controls share one trusted desktop process. NetSurf runs in its
  own kernel address space through a private, bounded pixel/input protocol;
  browser launches additionally enforce kernel syscall/filesystem restrictions.
  General per-application GUI isolation, accessibility
  trees, multiple user sessions and a production desktop security model remain
  dependencies. Native
  shell programs retain the kernel's existing process address-space isolation.
* Mount discovery uses the existing bounded text `sysinfo` interface. Automatic
  mounts work; mount paths containing spaces need a typed mount-list interface
  before reliable automatic configuration/aggregate Trash discovery.
* Scrolling is functional through the wheel and keyboard. Scroll indicators
  do not yet support dragging the thumb. Window resizing uses the ordinary
  pointer; specialized resize cursors are not yet implemented.

## Validation

Final 1440x900 TCG drag measurements (`build/drag-blend-final-1-buffered.log`,
`build/drag-blend-final-4-buffered.log`, `build/drag-blend-final-1-linear.log`) were:

| Backend | CPUs | Mean frame work | Mean moving-frame interval | Maximum frame work |
| --- | --- | --- | --- | --- |
| QEMU buffered | 1 | 4.508 ms | 8.990 ms | 7.267 ms |
| QEMU buffered | 4 | 5.341 ms | 9.003 ms | 10.948 ms |
| Generic linear fallback in QEMU | 1 | 5.586 ms | 8.944 ms | 6.670 ms |

Moving frames arrive at approximately 111 Hz with this emulated USB mouse;
the interval histogram's 95th-percentile upper bound is 11 ms on one and
four CPUs. Each run moved a real window through 122 rendered frames,
compared all pixels above the taskbar after returning to its original geometry,
and checked rapid press/move/release sequences. The background compiler VM was
paused throughout these measurements; `build/drag-blend-benchmark-controller.log`
records pause/resume and all three results.

A paired single-CPU comparison of the shadow cache used the same input test
and existing SSE2 copy routine. Mean frame work changed from 4.941 ms to
4.630 ms; accumulated composition work changed from 337,018 to 287,837 us
(about 15% less). Logs are `build/drag-quiet-baseline-repeat-1.log` and
`build/drag-quiet-cached-scalar-1.log`. SIMD shadow blending remains slower in TCG,
so it is selected only for hardware CPUs/virtualization. A host CPU comparison
of the actual new routine measured approximately 1.9x faster uniform shadow
blending than the integer routine (`build/pixel-blend-cpu-test.log`); this is a
CPU microbenchmark, not a physical desktop/frame-rate measurement.
Independent arithmetic and inaccessible-page tests check all 256 alpha values,
16 alignments, boundaries and tails, both on the host and in one-/four-CPU native
guest processes (`tools/test_pixel_blend.py`). The full window drag/pixel
restoration test also passes with the SSE2 path explicitly exercised in QEMU
(`tools/test_gui_drag.py --sse2`); those timings still measure emulation.
Ordinary window moves reuse cached client
pixels and compact shadow-opacity bands; they do not repaint application
content every frame.

These results measure this host's emulator, not physical Intel/NVIDIA output
or vertical-blank synchronization. The linear fallback still copies into a
live firmware framebuffer; native GPU modesetting and synchronized page flips
remain separate hardware dependencies. `gui --stats` reports local guest timings.

The October 9, 2026 host display comparison used QEMU 11.1.2, KVM/host CPU,
four guest CPUs and the same 1440x900 buffered desktop. The original software
GTK frontend delivered completed frames at 26.8 Hz (37.257 ms mean interval,
73.095 ms maximum), despite its 4 ms refresh timer. Its guest moving-frame
interval was 35.842 ms, and the release-with-motion check failed. GTK with
OpenGL delivered 104.2 Hz (9.593 ms mean, 13.892 ms 95th percentile and 16.006 ms
maximum). Its mean guest drag work was 1.544 ms, with a 4.192 ms maximum;
pixel restoration, release motion and rapid successive drags all passed.
This is about 3.9 times the frontend frame delivery rate. A headless KVM run
measured 0.660 ms mean guest drag work, demonstrating why headless render
timings alone miss the software frontend bottleneck.

The native benchmark verifies `query-kvm`, records timestamped `gd_switch`
events during the moving portion of the drag, and reports `HOST_DISPLAY`
separately from `GUI_TIMING`/`GUI_PACING`. A switch measures completed-frame
delivery to GTK, not the monitor's physical scanout. Trace and guest logs are
`build/runtime-tests/qemu-display-comparison-20261009-gl-{off,on}.{display-trace,log}`.

The final SDL/OpenGL configuration was checked with physical mouse movement
and dragging on the same KDE Wayland host. The user confirmed both worked;
the trace recorded 576 relative axis events and a complete button press/release.
The desktop rendered 131 drag frames with 0.896 ms mean work, 1.940 ms maximum
work and 9.607 ms mean moving-frame interval (about 104 Hz). These are guest
timings, not monitor scanout measurements. The input, display and serial logs
are `build/runtime-tests/uefi-4cpu-physical-mouse-sdl-gl-on.*`.

```sh
make test-desktop
python3 tools/test_desktop.py --cpus 1
python3 tools/test_desktop.py --cpus 4 --storage
make test
make test-native
make test-dark
make test-drag
make test-drag-native
make test-host-mouse
make test-mouse-live
make test-browser
make test-network
```

The desktop runner boots the real ISO under QEMU/OVMF with a USB keyboard and
mouse. It creates actual guest filesystem fixtures, checks screenshot colors
and geometry, then exercises dragging/resizing, maximize/restore,
minimize/restore, navigation, rename/new/copy/collision/Trash, recursive copying,
editor saves, unsaved-close/exit cancellation and dialog-owner cleanup,
workspaces, snapping, workspace moves, the taskbar window chooser, a native shell and child
program, and Settings. It verifies the resulting files through the native
shell. Scaling, workspace configuration and dark appearance are tested with keyboard input.

`--storage` recreates disposable generated images under `build/runtime-tests/`;
it never touches host block devices. Additional GUI tests exercise read-only
errors, writable Save As, read-only copy metadata, cross-mount cuts, disk Trash
restore and a CRLF/tab/long-line document. Host-side extraction compares exact
bytes, checks that the ext4 source remains unchanged, runs `fsck.fat -n`, and
reboots to verify disk preferences reload.

The native nano runner verifies console and GUI editing, saving and exact file
bytes, resize/SIGWINCH, canceling the running-program close dialog, read-only
save failures, reboot persistence, unchanged ext source and a clean FAT32 check.
The separate static-musl check exercises initialized TLS, allocation, guard
pages, signal masks/handlers/return, interrupted sleep, restartable reads,
poll/select, umask and PTY bounds/errors. Appearance tests check light/dark
switching, Files/Settings/Editor/Terminal/dialog colors, saved reload and exact
popup cleanup. The drag test returns a real window to its original position
and compares all pixels above the taskbar to catch cursor/window/shadow trails.
It also checks release-with-motion and rapid successive drags, and limits
content-cache repaints during movement.

`make test-wallpaper` checks actual PNG/JPEG/BMP decoding, PNG alpha, BMP
orientation and row padding, malformed and oversized image rejection, and
both packaged sample assets. Its headless guest test uses USB clicks and
keyboard input to select both previews, open both wallpaper directories,
choose custom images, compare real pixels for each placement and background,
check unchanged pixels after failed selections, and verify saved reload and
missing-image fallback. Screenshots and serial logs are in
`build/runtime-tests/wallpaper-*` and `uefi-4cpu-wallpaper.log`.
Add `--storage` to `python3 tools/test_wallpaper.py` to recreate disposable
disk fixtures and verify image selection, placement and background colors
through an actual reboot with FAT32 preferences.

Earlier cached-renderer measurements on single-CPU QEMU at 1440×900 reduced mean
guest drag composition and presentation time from 33.151 ms to 4.739 ms.
That earlier run presented 108 drag frames with a maximum of 9.047 ms and 18 cached
surface updates across the entire test. Both screen-restoration comparisons
were exact. These timings depend on the host and VM; they do not establish
hardware frame rates or vertical-blank synchronization.

The cursor-only test reports presentation counts and damaged pixels, requiring
one initial full-screen frame and fewer than three screens of pixels in total.
The measured reference run used 67 frames and 1,327,474 presented pixels,
98.47% fewer pixels than repainting the full screen on every frame. This
measures pixel traffic, not a guaranteed hardware frame rate.

Screenshots are saved as PPM files in `build/runtime-tests/`, with PNG copies
when Pillow is available. Examples include `desktop-4cpu.png`,
`editor-4cpu.png`, `terminal-4cpu.png`, `settings-4cpu.png`,
`scale-125-4cpu.png`, `scale-200-4cpu.png`, and
`desktop-reboot-preferences-4cpu.png`. Logs are in the same directory.
Validation establishes behavior in QEMU; physical hardware and abrupt
power-loss recovery require separate testing.
