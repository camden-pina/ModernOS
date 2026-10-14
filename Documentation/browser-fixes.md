# Browser repair and verification — 2026-10-11

This document records the historical v38 browser repairs and evidence.
Current native media, isolated frames/workers, WebAssembly, animated desktop
scrolling, hardware support and package status are in
[`browser-media-scroll.md`](browser-media-scroll.md).

The native HTTP/HTTPS browser embeds WebCore/JavaScriptCore 2.54.0 and CPU Skia
inside the ModernOS desktop. The renderer and trusted Soup network broker run
in the actual x86-64 Limine UEFI guest. Local files retain the existing NetSurf
engine and explicit grants.

## Implemented fixes

- WebCore now drives DOM, layout, CSS Grid, canvas, image decoding and fonts.
  Generic families resolve to packaged DejaVu fonts. Valid zero-sized text no
  longer collides with FontCache's empty key, and canvas keeps zero-size metrics.
- Musl can query genuine read-only self process limits. Its stack discovery
  now supplies valid conservative-GC root bounds. Self thread signals resolve
  thread-group IDs under the scheduler lock. GC remains enabled.
- Boot RAMFS borrows immutable Limine module bytes until a file is modified,
  avoiding a second approximately 726-MiB copy in a 2-GiB guest. Actual mapped
  and peak memory accounting feeds WebCore's memory-pressure handler.
- Wheel input consults the page's animation preference. WebCore's real scroll
  operations move retained Skia pixels; painting and transport comparisons use
  actual invalidated regions. Fixed, sticky, clipped, rounded and filtered
  content retain their correct damage and repaint behavior.
- Both pixel pipes have bounded 256-KiB buffers. A writer thread drains prepared
  immutable pixels during JavaScript execution. The desktop stages complete
  paints and commits them at PRESENT; renderer replacement cancels an incomplete
  paint. Shared bounds, overlap, allocation-failure and quota checks remain real.
- The embedder cycles WTF's run loop with an acquired GLib context, dispatches
  activity observers, paces rendering to the real preferred interval and completes
  rendering updates. A 4-ms dispatch budget and pending-input check keep successive
  background tasks from starving native input and painting.
- Experimental idle-callback queue fixes preserve real deadlines and task
  ordering, resume pending callbacks after expired budgets, and deliver timed
  callbacks exactly once from either pending or runnable queues. Controlled
  component tests enable the API. The native UI retains upstream's disabled
  default because enabling it delays initialization on script-heavy pages.
- Input/modifier state, POST/history/reload, cookies, UTF-16 Web Storage,
  downloads and normal child shutdown have genuine broker/engine integration.
  TLS, CORS, immutable renderer origins and filesystem restrictions remain active.
- The v38 native renderer uses WebKit's static x86-64 LLInt interpreter. JIT,
  DFG/FTL and WebAssembly were disabled. The helper and Makefiles select the
  same incremental LLInt cache; an intentional C_LOOP comparison stays isolated.

## Historical validated checkpoint: v38

`build/modern-browser/native-browser-v38.iso` was selected after native browser,
scheduling, HTTP/TLS/CORS, boot-file/accounting and sandbox/IPC checks passed on
one and four CPUs. Versioned logs are under `build/modern-browser/v38-uefi-*`.
`build/os.iso` was a byte-identical atomic copy of the validated candidate;
the previous default is preserved. Packaging hashes and extracted boot bytes
are in `v38-package-manifest.json` and `v38-package-verification/`. The default
also passed the one-CPU boot-file test, including exact renderer ELF export.

A new regression exercises the selected interpreter's actual allocations and
three full garbage collections in a real DOM realm, retaining closures, private
fields, a detached DOM wrapper, Map, BigInt and binary data. Unicode/ICU checks
also pass. The native UI exposes no test hook and keeps GC enabled.

The native suites compare retained scrolling with a complete repaint, including
scrollbars and nested/sticky/filtered content. Small-page scrolling measured
0.18–0.20 s; maximized dense-page scrolling measured 0.40–0.41 s. Scrolling
during repeated real background JavaScript tasks measured 0.21 s. These QEMU
TCG measurements include screenshot overhead and are not hardware benchmarks.
The actual shared surface routines also passed ASan/UBSan in the unchanged
surface implementation through v30/v31.

For a checked two-million-iteration Math.imul workload in the actual native
browser, one-CPU TCG median of three samples decreased from 1906 ms (v37 C_LOOP)
to 1273 ms (v38 LLInt): about 1.50× throughput or 33% less time for this workload.
This is not a general website speed claim. Reproduce with
`tools/benchmark_browser_interpreter.py`; evidence is preserved in
`build/modern-browser/interpreter-benchmark-v37-v38/`.

Uninstrumented four-CPU evidence is preserved in `public-site-v38/` (TCG),
`public-site-v38-kvm/` and `public-site-v37-kvm-comparison/`, under
`build/modern-browser/`. Each contains the actual guest log, JSON report and
screenshots. Captures wait at least 40 seconds; scrolling checks compare detailed
image patches at the requested 400-pixel translation. Times below measure first
body pixel response, including screenshot overhead.

| Public test | Observed v38 result |
| --- | --- |
| Google `osdev` search and submitted `limine uefi` query | Readable results, working query submission and confirmed scrolling: .189/.195 s on TCG and .180/.192 s on KVM. |
| YouTube fresh `limine uefi` results URL, TCG | Actual thumbnails/results and confirmed scrolling (.776 s); header remains a placeholder after 40 s. |
| YouTube fresh results URL, KVM | Actual full logo/header/search/sidebar/icons/results and confirmed scrolling (.219 s). V37 also fills its fresh header on KVM; this initialization result is not attributable solely to LLInt. |
| YouTube home → native page search | Actual full header/logo/sidebar icons/results and confirmed scrolling: .742 s on TCG and .501 s on KVM. The signed-out home itself has no populated feed in these captures. |
| OSDev root, TCG and KVM | Genuine HTTP 403 Cloudflare challenge. Its foreign verification frame is denied; guest console records origin-mismatched postMessage attempts to challenges.cloudflare.com. |

## Ongoing work and limits

The resumed LLInt candidate is validated and selected, with the previous C_LOOP
cache/v37 ISO intact. Temporary read-only DOM probes were used only in separate
v39/v40/v41 diagnostic ISOs; their source, reports and screenshots remain
preserved. The installed renderer and packaged default match uninstrumented v38.

On fresh TCG YouTube pages, actual topbar data/custom elements exist but a
priority-1 buttons job stays scheduled and active icons lack SVG children.
The first-party scheduler drains priorities 5/4 before lower tasks under a
roughly 13.7-ms wall-clock budget. Expensive work starving lower priorities is
a hypothesis requiring actual task timing; no site priorities, callbacks or
deadlines were altered. Preserved snapshots/scripts are under
`youtube-initialization-investigation/`.

Fresh TCG YouTube initialization, pauses during individual long JavaScript tasks
and the foreign-frame verification path remain unresolved. The browser also
has no media decoder/audio output, MSE, cross-origin frame process routing,
IndexedDB, service workers, WebGL/WebGPU, WebAssembly or clipboard backend.
Search-page success does not establish video playback or perfect compatibility.
Foreign frames require separate immutable-origin processes and real
window/message/resource/surface/input routing, with boundary tests; widening
one renderer's document allowlist would compromise isolation. Supporting these
frames alone would not guarantee that Cloudflare accepts verification.

Run controlled checks with `make test-native-browser`,
`make test-browser-scheduling`, `make test-webcore-network`,
`make test-boot-files`, `make test-browser-ipc` and `make test-browser-surface`.
Public diagnostics use `tools/diagnose_browser_sites.py --label NAME --accel tcg`
or `--accel kvm`; independent labels preserve comparisons. HTTP 200 and a page
title alone do not establish correct rendering. All owned jobs are stopped.
Preserve failed evidence, archives, source backups, licenses and build caches.
Never overwrite an ISO in use or run concurrent Ninja builds.
