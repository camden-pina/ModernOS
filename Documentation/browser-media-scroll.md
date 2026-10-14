# Native browser media, frames and animated scrolling

These changes extend the preserved v38 browser baseline. They run in ModernOS
under Limine UEFI; no host browser, remote rendering service or external media
player is involved. Tests use actual guest input, decoded canvas/display pixels,
and QEMU recordings of the kernel audio device.

The selected friendly image is `build/modern-browser/native-media-scroll.iso`.
Its SHA-256 is
`cfdf8ee6405f9df4075b32f1b8cdbf1daa5755cd8fc196c7698caec655dcf038`.
It is immutable candidate38 with codec configuration replay, optional exception
diagnostics and the retained ACPI speaker reset/fetch credentials fixes.
Immutable candidate32 retains its original `4971b71f...` bytes; all earlier
candidates remain preserved. Package hashes, exact extracted boot files,
test results and preserved recordings are under `build/modern-browser/` in
`media-scroll-package-manifest.json`, `media-scroll-validation-results.json`
and `media-package-verification38/`. The validation JSON distinguishes
new passes from retained candidate27 and earlier component tests.

The latest 2026-10-12 follow-up is unpublished candidate46,
`build/modern-browser/native-cloudflare-activation-target-candidate46.iso`, SHA-256
`c9c0233b07fce50a5b5d22d430ecfeb3ea2d204e36ad77de1cf5addebbea66d4`.
It synchronizes real frame focus/activeElement/keyboard paths and exposes
WebCore's actual User Activation API with timestamp, ancestry/origin,
consumption and expiry semantics. Foreign pointer forwarding activates the
actual target; ancestor notifications exclude unrelated local siblings.
Activation passes KVM4/TCG4; focus and full Browser pass TCG4; frames pass
KVM1/KVM4; credentials and full MSE/PCM pass KVM4. New regressions are
`tools/test_native_frame_focus.py` and `tools/test_native_activation.py`.
Package/source/validation/evidence are under `build/modern-browser/cloudflare46/`.
The intermediate45 activation test initially printed a false PASS through
substring title matching; corrected exact titles reject 45's actual activation
leak. Its audit and raw evidence remain in `cloudflare45/`.
Frame IPC is now 4, resource IPC remains 2; broker and renderer must agree.
Kernel, speakers, certificates and GUI match 43. No default was published.
OSDev46 still returns two root HTTP403 responses after a real click and 90
seconds. The official HTTPS diagnostic also fails 600010 with exception tracing
off and reports Turnstile Failed Despite Passing All Checks. The rejection's
cause remains unknown; these input corrections do not establish acceptance.

The earlier 2026-10-12 Cloudflare follow-up was unpublished candidate43,
`build/modern-browser/native-cloudflare-cookie-context-candidate43.iso`, SHA-256
`927d472c84048b704b77db6227f758ed2048782a34abfc380da0f9fd285bce84`.
It corrects an actual isolated-frame DOM/HTTP cookie-context mismatch: reads now
use the real top/ancestor site for SameSite filtering, and writes use the actual
first party under the existing cookie acceptance policy. The old42 image fails
the new `test_native_cookie_context.py`;43 passes KVM4 and TCG4, existing
credentials KVM4 and full native Browser TCG4. Other42 packaged components are
unchanged. That checkpoint's source and installed browser matched43; package/evidence/results are
in `cloudflare39/package43/`, `evidence43/` and `validation-results43.json`.
OSDev still returns root HTTP403 after a real click and 90 seconds; the official
troubleshooter also fails 600010 despite passing its diagnostics. No cause was
identified or success claimed. No candidate was published. The final check found
`build/os.iso` changed since the old checkpoint to SHA-256
`07bd4e3e3203df47ea0e7d4e9b9e7043bb4e836820289c42ce4f27f73eeb956e`; it was
left untouched. The friendly image still matches38. See `default-state43.json`.

The earlier follow-up was unpublished candidate42,
`build/modern-browser/native-cloudflare-referrer-candidate42.iso`
(SHA-256 `dcddc980ba533bdf7f090e33b714b456e5d3b66bd68c4518e09f9f3ca70034c2`).
Current browser source retains its corrections;
the selected/default ISO above remains candidate38. Candidate42 fixes three
independently reproduced integration defects: isolated HTTPS workers received
an opaque top origin and could not use real WebCrypto; resource responses lost
Soup's measured transport timings; and isolated-frame creation substituted a
canonical parent-origin string for WebCore's policy-filtered HTTP referrer.
The broker now supplies the actual top origin, transports measured timestamps,
protocol and encoded/decoded lengths with WebCore's timing privacy checks, and
preserves the original computed referrer. HTTP language defaults use the same
locale as Navigator. IPC resource/frame schemas are version 2; surface/input
packets remain 40 bytes. Old network measurements are omitted on history replay.

Focused checks are `tools/test_native_worker_context.py`,
`tools/test_native_transport.py` and `tools/test_native_frame_referrer.py`.
They verify real worker SHA-256, insecure HTTP ancestry across two isolated
frames, delayed HTTPS timings, gzip byte counts, timing privacy through
redirects, locale/UA agreement, untrusted-certificate rejection, and actual
HTTP/DOM referrers for strict-origin, no-referrer and unsafe-url policies.
Evidence, source snapshots and exact ISO extraction are under
`build/modern-browser/cloudflare39/`. None of these fixes establishes Cloudflare
acceptance: candidate42's normal live OSDev run clicked the rendered checkbox,
waited 90 seconds and received two root HTTP403 responses, with no unhandled
script errors. Actual OSDev access remains unfinished.

Candidate42 retry evidence: three actual rendered-widget clicks and a final
90-second wait still returned four HTTP403 roots, with no unhandled script
errors and 637 completed paints. Prefix: osdev-cloudflare-referrer42-retry3;
validation-results42.json and evidence42/ retain the exact result/log/screenshot.

## Implementation

`userland/modern-browser/port/MediaModernOS.cpp` embeds the pinned static FFmpeg
9.0.2 CPU decoders. Network bytes still pass through WebCore's media resource
loader and the certificate-checking Soup broker. FFmpeg has no network/device
protocols. MP4 H.264/AAC and WebM VP8/VP9/Opus/Vorbis video use bounded HTTP
ranges, three queued video frames and bounded 48-kHz stereo PCM queues.
Playback, pause, seeking, restart, volume, mute and canvas video drawing are
real decoder operations. Maximum decoded dimensions are 1920 by 1080.
Audio-only AAC/M4A, MP3 and Opus playback also uses real native decoders, with
truthful `canPlayType` results, metadata, pause and seek behavior. Ogg/Vorbis,
raw AAC and 16-bit PCM WAV demuxers/decoders are included. Pinned NASM 2.16.03
builds FFmpeg's SSE optimizations; AVX/XOP/FMA/YMM/ZMM paths are disabled both
at build time and in runtime CPU dispatch. Frame and slice decoding use two
threads, including on streams encoded as a single slice.
Converted frames have a padded 64-byte row stride and a final allocation guard;
Skia receives the actual stride. A 426-pixel-wide stream previously let SIMD
conversion overwrite the heap guard. The production-library canary reproduced
24 overwritten bytes, and the old guest crashed in the allocator. Padding
keeps these writes within owned memory, including during canvas drawing.

Playback with audio follows the kernel's actual PCM queue progress. A long
JavaScript task can exhaust the device queue, but cannot skip unheard audio
by advancing a wall clock. Unplayed samples are restored after pause, and
seek resets the device clock. Video-only buffering also holds media time
until future data arrives. The media clock and JavaScript clocks remain
separate; browser timers and `performance.now()` are unchanged.
The refill loop can submit up to sixteen PCM chunks per tick, stopping at the
same 200-ms device-queue bound. Four AAC chunks previously filled only about
85 ms, leaving software-emulated guests prone to underruns. Both file and MSE
decoders preserve dependent decoding while dropping already-late pictures
before RGB conversion during playback; paused seeks still decode their target.

`requestVideoFrameCallback` is enabled with real decoded-frame dimensions,
picture timestamps and increasing presented-frame counts. Callback delivery
consumes new metadata, and cancellation prevents delivery. Presentation and
expected-display timestamps use the actual monotonic software submission time;
they are not measurements of physical scanout or vblank. This follows the
upstream software renderer's timestamp approach. No callback, DOM timer or
media-clock value is synthesized to satisfy YouTube.

`MediaSourceModernOS.inc` implements MP4 H.264/AAC Media Source Extensions.
WebCore handles coded frames, timestamp offsets, append windows, removals,
eviction and seek queues. The native backend parses complete MP4 fragments,
preserves partial moof/mdat input and decodes real samples in timestamp order,
including reordered B-frames. Coded buffers are bounded to 48 MiB; pending
fragment input is bounded to 16 MiB. MSE WebM, AV1, encrypted/DRM media and
hardware video acceleration remain unsupported. CPU decoding performance
depends on the guest accelerator, CPU count and selected video resolution.

Coded samples retain their immutable codec parameters and AVC extradata.
A later initialization segment can change resolution while older samples are
still buffered. Decoding now restores the sample's original configuration,
including after backward seeks, and preserves decoded picture/PCM queues and
clocks. The old implementation decoded retained samples with the newest SPS/PPS,
producing gray pictures and H264 errors. A two-resolution replay test fails on
candidate37 and passes on38 with independent decoded canvas pixels.

`RemoteFrames.inc` and `native-frames.inc` maintain WebCore RemoteFrame mirrors
and a broker-owned frame graph. Each foreign document has a fresh immutable
origin process. Sealed raster/structured-clone payloads cross checked frame
edges. Same-origin policy, message source/origin, targetOrigin, CSP, CORS and
TLS verification remain enforced. Unsupported transferable capabilities
between isolated frames throw DataCloneError before detaching sender data.
The tree currently supports at most eight children per view and three levels.
The desktop supplies actual screen dimensions, available work area and outer
window geometry to every frame process. Input relays preserve the actual
pointer's screen position while translating its client coordinates into the
target iframe. Frame viewport dimensions remain independent of display size.

Local blob loads use WebCore's BlobRegistry and real resource handles.
Dedicated workers execute genuine JavaScript and exchange structured clones.
Their startup avoids the empty IndexedDB provider while the IndexedDB API is
disabled. The LLInt browser also enables real WebAssembly IPInt execution,
JavaScript imports, streaming compilation, memory growth and validation.
It uses explicit bounds checks and eagerly zeroed memory. JIT, BBQ/OMG JIT
and large signal-based memory reservations remain disabled.

`userland/desktop/smooth_scroll.h` supplies a critically damped pixel animation
for Files, Text Editor, Terminal, Settings and menus. Fractional position and
velocity persist between wheel ticks; elapsed time controls movement. Bursts,
direction changes, resizing and suspended views have bounded handling. Editor
and Terminal render partial rows. WebCore's own ScrollAnimator animates browser
wheel input after genuine DOM dispatch, preserving nested scrolling, scroll
snap and preventDefault. The desktop's completed-frame target is 120 Hz;
actual scanout and QEMU frontend timing still depend on hardware.

## Audio devices and firmware

Kernel PCM output supports PCI AC'97 and analog Intel HDA through kernel-owned
DMA rings. The renderer cannot call the audio syscall: bounded, sealed PCM
goes through its origin-bound broker. Device buffer sizes, pointers, ownership
and operations are checked. HDA codec verbs discover analog pins and DAC paths;
the driver disables Intel DSP stream routing when the processing-pipe
capability is present. Unsupported controllers return a real device error.
AC'97 starts DMA once and extends the descriptor tail through LVI. Rewriting
RUN for every PCM chunk made QEMU discard descriptors; its queue clock then
skipped a six-second file to its final scene. The corrected one- and four-CPU
tests preserve video timing and continuous recorded PCM.

The identified physical PC has Intel Arrow Lake HDA (8086:7f50), Realtek ALC287
(10ec:0287, subsystem 17aa:3907) and two AW88399 I2C speaker amplifiers on
8086:7f4e. Only that codec/subsystem selects the built-in-speaker routing and
left/right amplifier quirk. The driver loads actual DSP/EQ firmware, verifies
hardware CRC and factory trim, checks playback clock/watchdog protection and
unmutes with a fade. Initialization/protection failures leave both amps muted.
This hardware path has compiled and passed register-model/firmware tests;
the user has now confirmed that the physical speaker test works on candidate32.
The amplifier reset now comes from this PC's actual ACPI `_CRS`: GPIO140,
GPP_A12 on INTC1084 `\_SB.PGPI`. Its second memory resource supplies the MMIO
base. The separate GPIO171 interrupt is left untouched. The driver verifies
resource identity/bounds, ownership/locks and pad configuration, then performs
one shared high/low/high reset with 1-ms holds before loading either firmware.
LAI now reads actual NVS/SystemMemory, I/O and PCI resources with real time
hooks. SPKR has no separate `_PR0`/`_PS0` in this table; I2C2's `_PS0` is empty.
The actual resource/parser/GPIO code passes ASan/UBSan. `/bin/speaker-check`
passes recorded QEMU HDA stereo and measured pause/device-clock/resume checks;
physical audible output was confirmed by the user after the
[listening procedure](legion-speaker-validation.md). No console transcript or
individual channel observations were supplied.

Explicit cross-origin fetch `credentials: include` now uses the broker's
destination cookies while preserving CORS, HttpOnly and origin isolation.
SameSite context comes from trusted frame ancestry. The focused HTTPS/worker
test fails on candidate27 and passes with the fix. Optional
`gui --browser-network-trace` logs cookie names/attributes and real HTTP
identity/Fetch Metadata without cookie values or challenge bodies.

`make iso` includes the local `/lib/firmware/aw88399_acf.bin` when it exists.
Other builders can supply `AUDIO_FIRMWARE=/path/to/aw88399_acf.bin` or set
`AUDIO_FIRMWARE=` to omit it. No firmware binary is added to the source tree.
The initramfs stores it under `/usr/lib/firmware`, reached through `/lib`.
HDMI/DisplayPort, USB audio and other smart-amplifier models are unsupported.
QEMU's default launch uses AC'97; HDA can be selected with:

```sh
make run QEMU_AUDIO='-audiodev sdl,id=audio0 -device intel-hda -device hda-duplex,audiodev=audio0'
```

The physical PC's wired Intel I226-V (8086:125c) now has a separate polled
advanced-DMA driver. It handles PCI D0/reset, NVM MAC loading, PHY firmware
ownership, 10/100/1000/2500 autonegotiation and bounded kernel-owned queues.
Failure cleanup waits for DMA quiescence before freeing memory. QEMU's 82576
`igb` model validates the shared packet path with real DHCP, HTTP, TLS and Git;
the I226-specific PHY sequence still requires a physical boot. Use the PC's
Ethernet connection; its Intel Wi-Fi 7 device is unsupported.

## Verification

`tools/test_native_media.py` checks native MP4/WebM pixels, canvas drawing,
pause, seek/restart, HTTP/TLS range delivery and sustained recorded stereo PCM.
Its 426-by-240 clips cover row padding in both file decoders.
Candidate27 passes with AC'97 on one- and four-CPU KVM and with HDA on
four-CPU TCG. It also verifies frame-callback metadata, repeated delivery and
cancellation.
`tools/test_native_audio.py` checks AAC, MP3 and Opus, actual audio capability
claims, metadata, a deliberately busy 800-ms JavaScript task, pause/seek and
recorded 440/660-Hz sound. The prior wall-clock engine fails the busy-script
check by advancing about 800 ms; the device-clock engine passes. HDA and
AC'97 have passed the corrected check under KVM; AC'97 also passes under TCG.
The candidate27 audio-only rerun passes on four-CPU TCG with AC'97.
`tools/test_native_mse.py` checks real fragmented H.264 B-frames plus separate
AAC, partial appends, windows/offsets/removal, play/pause/seek and PCM. Detailed
426-by-240 CABAC/B-frame pixels are compared with independent host decoding
at the beginning and after seeking, preserving the coded stream's start time.
It also delays a real second append, checks that media time holds at the
buffered end and verifies decoded playback resumes. The old clock failed
this check; the corrected engine, SSE decoding and HDA sound pass under TCG.
Candidate27 passes these MSE checks with HDA on four-CPU KVM and TCG,
including seeking requested immediately after audio metadata, before video
data arrives, and frame-callback metadata for the detailed 426-pixel stream.

`tools/test_native_frames.py` checks TLS child pixels, same-origin enforcement,
source/origin, typed-array/Map structured clones, targetOrigin and transfer
rejection, click/resize/reload/removal, CPU view-transition promises, actual
streamed Wasm arithmetic/imports/memory/validation and blob-worker execution.
It compares reported display geometry with the actual QEMU screen and checks
that parent and foreign child share screen and outer-window geometry. It also
checks actual global screen coordinates on a trusted iframe click.
`tools/test_native_scroll.py` verifies actual intermediate native/DOM positions,
visible translated pixels, reversal, nested scrolling, cancellation and cleanup.
Native application gestures settled in about 280 ms on one- and four-CPU KVM;
browser gestures had 13–14 observed intermediate DOM positions.
One- and four-CPU TCG also passed native and browser scrolling; browser gestures
had 7–12 intermediate DOM positions. These scrolling runs used clean
candidate19 (KVM) and candidate21 (TCG); the packaged desktop binary is
unchanged. Earlier clean builds also passed sandbox/IPC restrictions,
desktop settings/persistence and the full native browser regression suite.

`tools/test_audio_firmware.py` uses the supplied firmware and the actual parser/
amplifier core with ASan/UBSan. It checks malformed/truncated input, profiles,
byte order, DSP memory contents, CRC, trims and muted failure on clock/watchdog
errors. `tools/test_browser_ipc.py` verifies the kernel IPC/origin restrictions,
threaded broker capabilities and cleanup on one- and four-CPU TCG.

The optional public checks are `tools/test_youtube_playback.py` and
`tools/test_osdev_verification.py`. The short “Me at the zoo” YouTube video has
passed with changing decoded player pixels and actual HDA stereo audio on
four-CPU KVM. The longer Big Buck Bunny stream also passed with decoded motion
and stereo audio in candidate27 on four-CPU KVM and TCG. The strict check waits
for five seconds of actual stereo PCM before measuring further player motion:
KVM had 9/9 changing snapshots and 13.47 seconds of recorded sound; TCG had
3/9 changing snapshots and 5.57 seconds. The video title contains “4K”, but
this test plays a selected CPU-decodable stream within the 1080p limit.
TCG is substantially slower; this result does not establish uninterrupted
high-resolution or real-time playback on every machine.
Cloudflare's real isolated widget runs its worker and checkbox handler, but
OSDev verification still fails. The captured error is `600010`, in Cloudflare's
[generic challenge-rejection family](https://developers.cloudflare.com/turnstile/troubleshooting/client-side-errors/error-codes/).
Its diagnostic page reports no detected automation/privacy interference, a
synchronized clock and a reachable server. These checks do not prove that the
verification service accepts this browser; OSDev document access is unfinished.
The candidate27 KVM check clicked the actual rendered checkbox and
remained on “Just a moment...” after 90 seconds. It observed no unhandled
script errors and no HTTP 200 OSDev document. Real worker/Wasm execution,
correct screen/window/pointer geometry and verified HTTP/script User-Agent
agreement have not resolved this rejection. Cloudflare documents
[limited support for custom or modified engines](https://developers.cloudflare.com/cloudflare-challenges/reference/supported-browsers/);
the rejection's underlying cause has not been established. New candidate31
evidence (byte-identical browser components to candidate32) shows that the
server issues an OSDev `cf_clearance`, the broker stores it and the next
navigation sends it, but still gets HTTP403 after 90 seconds. Loss of this
cookie during reload is ruled out; genuine OSDev access remains unfinished.

Candidate32 preserves live short and long YouTube playback on four-CPU KVM:
both have 6/9 changing player snapshots after five seconds of PCM; the short
clip records 18.14 seconds of actual sound, Big Buck Bunny 14.04 seconds.
Its native scrolling settles in 274-283 ms and WebCore has 13-14 intermediate
positions, including nested targets, reversal and cancellation. Full native
Browser on four-CPU TCG and focused HTTPS credentials/Blob-worker fetch on KVM
pass on candidate31's byte-identical kernel/renderer/broker/desktop components.

Candidate38 passes twelve new checks, including MSE on four-CPU KVM and TCG,
configuration replay, exception tracing off/on, credentials, frames/Wasm/workers,
scrolling and full native Browser TCG. Both live YouTube clips have 9/9 detailed
changes after five seconds of PCM; short records 17.86 seconds, long 14.46 seconds,
with zero H264 errors. Scrolling settles in 277-279 ms with 13 WebCore positions.
The public playback test now counts motion only between detailed player images;
monochrome loading/fade changes cannot satisfy the motion check.

Genuine Cloudflare acceptance remains unfinished. The candidate38 tracer-off
test clicked three actual widgets and observed four HTTP403 root documents,
despite issuing/storing/sending clearance. The official native Turnstile
Troubleshooter also reports 600010 after a real click while its four preflight
checks pass. Neither result identifies the cause; actual OSDev content has not
rendered. Optional exception diagnostics immediately resume genuine execution,
and include bounded values/stack locations plus safe public-name inspection,
without JS evaluation, getters or arbitrary local-string dumps.

The final image's extracted kernel and initramfs match the installed/staged
files, including renderer, broker, desktop, amplifier firmware, certificates
and FFmpeg/NASM notices. Static ELFs have no interpreter or writable/executable
load segment; the renderer and broker declare nonexecutable stacks. No
temporary media/DOM probes remain. The retained candidate27 boot-file check exported
the renderer from the running guest and verified its exact hash, copy/resize
semantics, memory accounting and compiler ABI. The actual previous default,
the historical v38 image and all immutable candidates are preserved.
The user confirmed the physical speaker test on candidate32; candidate38's
kernel, speaker tool and amplifier firmware are identical. The agent performed
no host reboot, raw host device write or partition change.

Pass an explicit candidate ISO to guest checks, for example:

```sh
python3 tools/test_native_scroll.py --iso build/os.iso --cpus 1 4 --accel kvm
python3 tools/test_native_mse.py --iso build/os.iso --cpus 4 --accel tcg --audio-device hda
python3 tools/test_native_frames.py --iso build/os.iso --cpus 1 4 --accel kvm
python3 tools/test_osdev_verification.py --iso build/os.iso --accel kvm
```

Build/test evidence is preserved under `build/browser-media-scroll-baseline/`
and `build/runtime-tests/`. Public challenges, availability and responses can
change; these optional live tests fail when the requested content is not shown.
