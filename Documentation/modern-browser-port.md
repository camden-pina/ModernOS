# Modern browser port: current implementation and remaining work

The native Browser now embeds genuine static WebCore/JavaScriptCore 2.54.0,
CPU Skia and the existing native desktop. It launches a restricted trusted Soup
broker and an immutable-origin renderer through `spawn_web`. A top-level origin
change replaces the renderer; same-origin navigation stays in WebCore. Local
file associations retain NetSurf and its explicit directory grants. The native
40-byte input/state/pixel protocol and trusted compositor remain in use.

The current candidate51 checkpoint adds genuine UI-navigation provenance,
broker-validated ancestor/sibling frame trees and correct opaque-origin message
serialization. Full frame graph tests pass on KVM and TCG; actual OSDev still
returns a Cloudflare HTTP 403 after verification. Exact source, packages,
regressions and failed live evidence are in [AI-HANDOFF.md](AI-HANDOFF.md).

The native regression passed on one and four CPUs through selected v38,
including real Grid/canvas/PNG/JPEG/WebP/SVG and Shadow DOM pixels, Ctrl+A and editing after an
origin switch, wheel pixels, nested/sticky/filtered content and fixed overlays
at 2560×1600, full-repaint equality, generic and zero-sized fonts, form POST, cached
history restoration without POST resubmission, explicit POST reload, cookies,
UTF-16 storage/quota, same-origin frames/events, resize, downloads and cleanup.
Prepared output has a dedicated writer thread; the real five-second JavaScript
regression confirms that an already painted surface drains during the task.
The desktop presents native paints together, with bounded staging and renderer
replacement cancellation. Shared copy/queue/commit/cancel checks pass ASan/UBSan.
Checkpoint measurements and public screenshots are recorded in
[browser-fixes.md](browser-fixes.md) and [AI-HANDOFF.md](AI-HANDOFF.md).

The CPU port consults the real page preference for wheel animation. It applies
the actual scroll operations in its retained Skia raster and repaints WebCore's
invalidated regions. The transport mirrors those moves and compares real pixels
only inside the clipped paint regions; fixed/sticky content still receives damage.
Both pixel pipes have bounded 256-KiB buffers. Paint-completion packets prevent
public diagnostics from mistaking a partial transfer for a complete repaint.
The v38 fixture measured about 0.18–0.20 seconds for four wheel ticks on the small
page and 0.40–0.41 seconds on the maximized dense page under QEMU TCG. These timings
include screenshot overhead and do not establish hardware performance.

The embedder cycles the actual WTF/GLib run loop and delivers its activity
observers, paces rendering updates to the preferred interval and completes
rendering hooks. Successive dispatches have a 4-ms budget and yield for pending
native input. The repeated background-JavaScript fixture scrolls in approximately
0.21 seconds on one and four CPUs. An individual running script task remains
nonpreemptible on the DOM thread.

Controlled component checks enable the experimental idle-callback API and test
real deadlines, cancellation, continuation after expired budgets, microtasks
between callbacks and runnable timeouts under busy work. Native UI retains
upstream's disabled API default: public A/B tests show enabling it delays
initialization on script-heavy pages. Native animation/microtask callbacks and
parser/custom-element notification ordering have separate genuine checks.
The selected-interpreter regression allocates 192,000 temporary objects with
typed arrays and runs three actual full collections in a real DOM realm.
Closures, private fields, a detached DOM wrapper, Map, BigInt, DataView and ICU
remain valid. The native UI has no GC test hook or changed GC preference.

The broker implements cookies and real UTF-16 Web Storage. HttpOnly cookies
stay in Soup; renderer credentials cannot select another origin's store. Local
storage uses a private SQLite database and its genuine `unix-dotfile` locking
VFS; session storage remains in the window broker. Streamed downloads publish
through no-replace rename. WebCore history trees, popstate and scroll restoration
are integrated with the bounded broker history service.

Boot RAMFS references immutable Limine module bytes until a file is modified,
removing a second ~726 MiB copy in ordinary 2-GiB guests. Write/truncate/zero-fill
semantics and the complete renderer ELF export passed on one and four CPUs.
Actual mapped/peak accounting feeds WebCore's upstream cache/GC pressure handler.
Read-only self prlimit64 fixes musl stack discovery and the invalid GC root range.
Native renderers have a 768-MiB budget; broker/legacy processes retain 512 MiB.
The JSC StructureID reservation is configured to fit this eagerly backed VM.

YouTube's zero-sized fallback text exposed a real font-cache empty-key collision;
the cache now uses a negative-size sentinel. A component check exercises the
actual empty typeface at zero size, and the native fixture checks canvas metrics
and visible generic fallback text. Shipped DejaVu fonts have real generic-family
aliases. Canvas font resolution preserves a valid zero-sized request.

The following public captures describe the preserved v38 baseline. Subsequent
media, isolated frame, worker, WebAssembly and animated scrolling changes and
their validation are in [browser-media-scroll.md](browser-media-scroll.md).

Public compatibility remains incomplete. Uninstrumented v38 captures show
readable Google results, actual scrolling and a working submitted `limine uefi`
query. YouTube no longer hits the GC/font crashes. Home-to-search navigation
shows actual results/thumbnails, logo and sidebar icons. On KVM, fresh direct
results also show the full actual header/search/sidebar; v37 does so on KVM as
well, so this result cannot be attributed solely to LLInt. On TCG, fresh direct
results render and scroll but retain a placeholder header after 40 seconds. The
diagnostic waits for the requested query title, complete paints and a minimum
observation interval. It verifies detailed patches translated the requested
400 pixels; tiny animation changes do not establish scrolling success.
OSDev returns a Cloudflare HTTP 403 verification page on TCG and KVM; its foreign
verification frame cannot load until isolated cross-origin frame routing is
implemented. Actual guest console records origin-mismatched postMessage
attempts. Routing would not guarantee server acceptance. Never infer
correct public pixels from a title or HTTP 200 alone. Preserve failed evidence
and inspect complete paints in each newer public run.

At that baseline, video/audio decoding and output, Media Source Extensions,
cross-origin frame routing, IndexedDB, service workers, WebGL/WebGPU, WebAssembly
and clipboard were unavailable. The v38 JavaScript engine uses the validated static x86-64 LLInt
interpreter, with JIT/DFG/FTL and WebAssembly disabled. The older C_LOOP cache
remains isolated and preserved. A checked two-million-iteration native script
workload had a one-CPU TCG median of 1273 ms versus 1906 ms in v37, approximately
1.50× throughput for that workload only. A media element alone does not provide
video playback.

At the v38 checkpoint, `build/os.iso` was an atomic copy of
`build/modern-browser/native-browser-v38.iso`; that image and its packaging
evidence remain preserved. Current media, frames, workers, WebAssembly,
scrolling, hardware work and package status are recorded in
[`browser-media-scroll.md`](browser-media-scroll.md). The production renderer
contains no temporary DOM diagnostics.
`make iso` builds the native renderer/broker incrementally. The Python helper
and both Makefiles default to LLInt, with the userland target rechecking the
selected cache even after a different interpreter trial. Use explicit
`--interpreter cloop` or `WEBCORE_INTERPRETER=cloop` for intentional comparison.
`make test-native-browser` checks the native window through USB input;
`make test-webcore-network` checks resources/CORS/TLS and engine pixels;
`make test-browser-ipc` checks actual capabilities, signals and bounded pipes;
`make test-browser-surface` checks retained copies under ASan/UBSan.
`make test-browser-scheduling` checks actual callback/input scheduling;
`make test-boot-files` verifies immutable backing and exact renderer bytes.
Package candidate test images separately while another VM uses an older ISO.
Preserve pinned archives, source backups and dependency/build caches, and never
run concurrent Ninja/configure jobs.

## Assessment

Upstream documentation and source were checked on 2026-10-09. These are actual
engine projects, rather than user-agent changes or JavaScript compatibility shims.

| Candidate | Relevant upstream design | Consequence for this OS |
| --- | --- | --- |
| WPE WebKit 2.54.0 | Separate WebCore/JavaScriptCore components; JSCOnly has static-library, generic event-loop and interpreter options. WPE includes a software SHM buffer path. | Provides a real engine component that can be ported and tested before the large rendering/IPC integration. Full WPE additionally requires GLib/Gio, Soup3, Skia, HarfBuzz and other target libraries. |
| Ladybird | Its porting guide describes POSIX platform facilities, Unix socket IPC, helper processes and a platform ViewImplementation. | Plausible native offscreen integration, but it also needs a substantial new platform/runtime/dependency port; it is not an existing executable for this kernel. |
| Servo | Rust engine with an embedder interface and its own build dependencies. | The existing guest SDK has no Rust toolchain. Both that runtime and the embedder/platform integration would need implementation. |
| Chromium | Official Linux builds use a large source/dependency checkout and the supplied Clang toolchain. | A Linux build is not a custom-kernel port. Its process, IPC, graphics and sandbox integration would still require implementation. |

Sources: [WPE 2.54.0 release](https://wpewebkit.org/release/wpewebkit-2.54.0.html),
[JSCOnly configuration](https://github.com/WebKit/WebKit/blob/main/Source/cmake/OptionsJSCOnly.cmake),
[WPE configuration](https://github.com/WebKit/WebKit/blob/main/Source/cmake/OptionsWPE.cmake),
[WPE architecture](https://wpewebkit.org/about/architecture.html),
[WPE SHM buffers](https://github.com/WebKit/WebKit/blob/main/Source/WebKit/WPEPlatform/wpe/WPEBufferSHM.cpp),
[Ladybird porting guide](https://github.com/LadybirdBrowser/ladybird/blob/master/Documentation/Porting.md),
[Servo](https://github.com/servo/servo),
[Chromium Linux build instructions](https://chromium.googlesource.com/chromium/src/+/main/docs/linux/build_instructions.md).
The checked local WPE 2.54.0 archive, rather than moving upstream main, defines
the implemented component. That release removed the previous Cairo renderer;
reusing this OS's existing small graphics libraries alone cannot build full WPE.

## Implemented kernel facilities

- Linux-numbered `clone(56)` accepts the musl pthread flag combination: shared
  VM, filesystem context, descriptor table and signal dispositions; TLS and
  parent/child TID registration; clear-child-TID exit wakeup. `getpid` returns
  the thread-group ID, `gettid` the distinct thread ID. FPU/FS state and signal
  masks remain per thread.
- Shared address spaces now have one reference-counted VM metadata object for
  heap bounds, mapping allocation, mapped-page quota and cache generation.
  Copying a handle no longer creates stale metadata. Short VM/table syscalls
  serialize within the group. User copies recheck individual pages with
  preemption disabled before using their physical addresses.
- `futex(202)` implements WAIT/WAKE, WAIT_BITSET/WAKE_BITSET and
  REQUEUE/CMP_REQUEUE, relative monotonic and absolute monotonic/realtime
  deadlines, masks, comparison errors and signal interruption. Compare, queue
  and sleep transitions are atomic with wakeup. Keys use address-space root
  plus virtual address; unrelated private forks cannot wake each other's waits.
- Threads share real descriptors. Blocking file/socket I/O retains file
  references across sibling close/reuse. Joined/detached kernel task slots are
  reclaimed without freeing a live stack. `exit` ends one thread; `exit_group`
  and unhandled fatal faults end the group. Parent wait/reaping occurs after
  the final live thread. Exiting a leader early preserves workers and shared
  pipe writers until their final exit.
- Signal dispositions, umask, cwd, interval timers and aggregate CPU accounting
  are shared. Thread-directed signals validate both TGID and TID. Disposition
  copies are protected from same-CPU preemption. Thread CPU clocks/accounting
  remain distinct. Console foreground ownership follows the group ID.
- Bounded `mremap(25)` supports in-place uniform-page growth and shrink with
  zero-filled new pages, occupied-page checks and allocation rollback. This
  supplies musl's actual main-stack discovery operation. Moving mappings,
  aliases and unsupported flags return errors.

Thread groups deliberately stay on their parent's CPU. There are no cross-CPU
TLB shootdowns or live thread migration. Different processes still use SMP.
Fork/vfork/exec/spawn from a multi-threaded group are rejected. Process-shared
futex aliases, priority inheritance, robust-owner recovery, alternate signal
stacks and complete POSIX cancellation/context behavior are not implemented.
No general Linux/POSIX threading compatibility is claimed. The system still
has 128 total task slots and an FXSAVE context without AVX.

The existing NetSurf sandbox continues to deny clone/fork/exec/spawn and unknown
syscalls. Its 512-MiB quota uses the shared VM metadata. Explicit origin-bound
renderer/broker profiles and fresh helper launching have now been added through spawn_web(409). Component checks exercise JSC/Gio with
these roles. Direct CLI JSC remains a local development command; the desktop
does not launch it or execute downloaded page scripts through it. Desktop top-level navigation now uses these roles; cross-origin frame
routing remains unavailable.

Owned source changes are in `include/{sched.h,mm/user.h,uapi/abi.h}`,
`mm/user.c`, `sched/{elf,process,sched,signals,syscall,sandbox,tty}.c`,
`arch/x86/kernel/interrupts/interrupt.c`, `net/network.c`, `include/net/network.h` and
`drivers/video/fbdev/core/display.c`. Original continuation inputs were saved
under `build/modern-browser/original/` before editing; these are not an old git
baseline and should not be restored over subsequent work.

## Genuine engine build

`tools/build_javascriptcore.py` verifies pinned unmodified WPE WebKit and ICU
archives, uses the existing musl cross compiler, and preserves other SDK/browser
caches. Its separate POSIX libstdc++ runtime uses GCC's real `gthr-posix.h`;
the existing SDK's single-threaded runtime remains intact. A dedicated compiler
wrapper selects the new headers/libraries for this component. Both host and
target ICU are source-built, and the real ICU data archive is statically linked.

JavaScriptCore uses upstream JSCOnly with C_LOOP, a generic event loop,
system malloc and static linking. JIT, WebAssembly, sampling profiler and
libbacktrace are disabled. This requires no executable writable mappings or
dynamic loader. No engine APIs/syscalls are replaced with success stubs.
The resulting x86-64 ET_EXEC has no PT_INTERP; its current size is about 53 MiB.

```sh
make javascriptcore
make test-javascriptcore
make test-threads
```

`test-javascriptcore` finishes the component build before building the ISO, then
boots real UEFI guests with 1 and 4 CPUs. Avoid rewriting the ISO while a test
VM is reading it. A normal `make iso` incrementally builds the native renderer/broker
and packages files in `userland/bin`; it does not rebuild the separate JSC shell. The component is installed as `/bin/jsc`,
with fixtures under `/usr/share/modern-browser/` and notices under
`/usr/share/licenses/native-programs/`.

Host generators use CMake/Ninja, Ruby, Perl and Python 3.12+ in addition to the
existing GCC SDK prerequisites. This session cached CMake 4.4.4, Ninja 1.13.2
and a source-built Ruby 3.4.7 under `build/modern-browser/host-tools/`; no host
system installation was changed. Builder stage stamps permit incremental
continuation. Host tool archives are retained in
`build/modern-browser/downloads/`. Pin URLs/hashes are in the vendor README;
the build script itself does not download dependencies.

## Verification and failures

Real guest execution passed on 1 and 4 CPUs:

- `thread-check`: TLS/IDs, mutexes, condition variables, semaphores, futex
  deadlines/errors, in-place mapping growth/shrink/rollback, shared mapping
  metadata, close/reuse during a blocked
  read, directed signals, over 128 sequential/detached task lifetimes, separate
  private-fork futex domains, group exit, fatal worker faults and leader exit.
- `thread-cxx-check`: real C++ std::thread, mutex/condition, call_once,
  promise/future, exceptions on worker threads and joins using the new runtime.
- `jsc engine-check.js`: private fields, optional chaining, modern collections,
  immutable sorting, BigInt, Unicode regex, binary data, real ICU Intl,
  promises, async generators, allocations and garbage collection.
- Module fixtures: real imports and top-level await.
- `jsc agent-check.js`: four real native JSC agents with separate VMs, shared
  ArrayBuffer data, atomic updates/wait/notify, GC and worker reports. These
  shell agents are not browser Web Workers or isolated site processes.

Logs: `build/modern-browser/{jsc-driver,jsc-build,jsc-guest-tests,thread-tests}.log`
and `build/runtime-tests/uefi-{1,4}cpu-{javascriptcore,threads}.log`.
Native desktop browser, sandbox and general runtime regression results are
recorded in the handoff. Actual browser checks assert pixels and interactions,
including forms/history/downloads/resize and certificate rejection; JSC shell
tests are reported separately.

The 2560×1600 TCG drag stress test fails its 25-ms rapid-drag geometry assertion.
Long-drag pixel restoration and release checks pass. A controlled ISO using the
saved pre-thread kernel, the same current desktop and the same resolution also
fails that assertion, with similar approximately 16-ms moving-frame work.
See `build/modern-browser/{drag-retest,drag-baseline}.log`. The test was not
weakened and the user's resolution/frontend settings were not changed. Earlier
1440×900 or physical KVM/SDL measurements do not establish a pass at this mode.

## Work still needed for the requested browser

The real WebCore/Soup/Skia frontend and top-level origin changes are integrated.
Continue from the active handoff and preserved public captures; do not restart
component/dependency work or claim that successful fixtures prove every website.
Native media decoding/output/MSE, cross-origin frame processes and genuine
remote-window messaging, workers and interpreted Wasm have since been added;
see [browser-media-scroll.md](browser-media-scroll.md). Remaining browser
services include IndexedDB, service workers, WebGL/WebGPU, clipboard and JIT.

Cross-origin frames must retain separate immutable-origin processes and tested
resource/message/surface boundaries. Do not widen one renderer to arbitrary
origins or bypass server verification. OSDev's challenge currently cannot finish.
The current software interpreter also limits long script performance; prepared
pixel transmission now has a separate worker, but DOM/JSC still run on one
main thread. Keep the actual display/input ownership and certificate validation.
