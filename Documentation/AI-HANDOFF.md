# ModernOS continuation handoff — 2026-10-11

The user requested a next-session prompt before stopping for the night.
Read this file alongside the current source; do not infer that every
change shown by git was made in the most recent session.

## User requests and scope

The current request is to access osdev.org through its Cloudflare verification,
watch YouTube videos and make scrolling smooth throughout this custom 64-bit
Limine UEFI OS. The user selected all three targets: QEMU KVM, QEMU TCG and
this Intel Arrow Lake physical PC. They use its built-in speakers. Earlier
browser rendering, sandbox and general search work remains preserved. Genuine
media, isolated frames, workers and Wasm are implemented; Cloudflare acceptance
and physical networking remain unfinished. The user has now confirmed that
the physical speaker test works on candidate32.

The latest user instruction is to prioritize fixing Cloudflare verification
and finish the Arrow Lake PC's built-in speakers. YouTube and smooth scrolling
already have passing guest tests; preserve those improvements. The ready-to-use
next-session prompt is `Documentation/AI-RESUME-PROMPT.md`. It identifies the outstanding
Cloudflare investigation and physical validation of the implemented amplifier
ACPI GPIO/power/reset sequencing, with
the actual hardware identities, firmware, source paths and validation criteria.

Later the user asked how to use disks/persistent storage and access their Linux
partition. This was answered as a capabilities/how-to question; no physical
partition was mounted, formatted or changed. It was not a request to implement
a Btrfs driver. Finally, the user asked what makes QEMU responsive and requested
a prompt/handoff for continuing the remaining work.

Work autonomously on the authorized browser goal, preserve the OS and existing
performance improvements, and report actual functionality and remaining limits.
Do not repeatedly ask permission for routine edits/builds/tests. Follow the new
instance's actual permission policy for network access and GUI execution. No
subagents were used in the recent browser work; check applicable instructions
before delegating. No skills were invoked.

## Workspace and preservation

- Repository: `/home/camdenp/Documents/test`, Bash on Linux.
- The tree contains substantial preexisting modified AND untracked OS source.
  Many essential `kernel/`, `userland/`, `tools/` and `Documentation/` files are untracked.
  Do not use `git reset`, `git clean`, restore old files, delete unknown files,
  discard untracked files, or assume the git baseline is the current OS.
- `.git` was read-only in the environment. No commit was made.
- No applicable AGENTS.md was found; check again if instructions have changed.
- Preserve pinned vendor archives, license notices, build caches and user data.
  `build/` is large; a clean rebuild of every dependency wastes substantial time.
- Latest boot artifact is `build/os.iso`; native engines are
  `userland/bin/web-renderer`, `userland/bin/web-broker` and local NetSurf; kernel is `build/vmlinux`.
- Browser test VMs from the recent work were closed. Do not terminate unrelated
  QEMU/compiler jobs if present when resuming.

## ACTIVE Cloudflare follow-up: candidate51 — 2026-10-13

OSDev access remains UNFINISHED. Installed browser binaries and production
source match the immutable, unpublished candidate:
`build/modern-browser/native-cloudflare-frame-tree-opaque-candidate51.iso`,
SHA-256 `68a4db3b6a150812dea1531281237826f32635dd1fb3bcc5a39b48901e70c409`.
Exact extraction, component/source hashes and static ELF protections are in
`build/modern-browser/cloudflare51/package/manifest.json`. Only the renderer
changes from50. Preserve49,50,51, all failed runs, vendor caches and dirty source.
The original console/getter fix remains intact.

Three actual integration defects are repaired, without establishing Cloudflare
acceptance:

- Candidate49 fixes real address/toolbar navigation provenance using WebCore's
  existing client flag. Script/frame/history navigation does not claim that
  provenance. Old48 fails the actual HTTP-header regression;49 KVM/TCG and50
  TCG pass. The trusted private flag is rejected on renderer-sourced packets.
- Candidate50 synchronizes the actual WebCore frame tree across immutable-origin
  renderers: real top/ancestors/local and foreign siblings, child order, names,
  stable WindowProxy objects and correct local/sibling/ancestor message targets.
  The broker validates bounded owned snapshots, PID/root ownership, canonical
  origins, foreign leaves and preorder. A snapshot cannot claim an existing
  foreign child as local. Existing CREATE/CLOSE capabilities and sealed-copy
  message forwarding remain authoritative. There are no JS accessor shims.
- Candidate51 fixes opaque postMessage origin serialization. Pinned
  SecurityOriginData::toString() returns empty for opaque data, so the old IPC
  generated an invalid empty source and the broker correctly closed it. Using
  the genuine SecurityOrigin serializer yields HTML's actual `null` origin.
  Wildcard messages work; an opaque default `/` target cannot match a distinct
  foreign document and is excluded. Broker origin checks were not relaxed.

Frame IPC is now version5; resource IPC remains version2. Rebuild renderer and
broker together after changes to the shared tree protocol. Owned source includes
`userland/include/web_frame_tree.h`, `native-frame-tree.inc`,
`port/NativeFrameTree.inc`, the existing frame/broker/service includes and
`renderer-native.inc`. Incremental Make dependencies include the new files.

The final full `tools/test_native_frame_graph.py` passes on51 KVM4 and TCG4,
including true nested top/local-parent relationships across two isolation
boundaries, stable proxies through insertion/removal, actual window.name changes,
local/foreign sibling and ancestor messaging, SOP, opaque source identity and
target exclusion. Successful runs and exact test sources are under
`cloudflare51/evidence/frame-graph-expanded/`. Failed50 iterations and their
audit are under `cloudflare50/evidence/frame-graph-iterations/summary.json`.
The initial capture race and mistaken expectations that iframe.name changes
rename an existing navigable, or DOM insertion necessarily determines indexed
FrameTree order, were corrected against upstream/spec and parent observations.
Do not count those initial failures as complete passes. The later correct50
opaque test genuinely failed and led to51. Independent ASan/UBSan parser checks
(377 cases plus 20,000 seeded mutations) and 26 broker authority checks pass;
failed draft authority/compile checks are retained. The corrected source is
validated by actual51 guest tests, not only a host model.

Traced51 OSDev still fails after one real rendered-checkbox click and 90 seconds:
two root HTTP 403 responses, actual verification pixels, 363 completed paints,
no unhandled script errors, clearance stored/sent and complete Cookie matching
the jar. Both roots use h2/TLS 1.3, certificate_errors=0 and Date skew=-3 seconds;
both say cf-mitigated=challenge. Rays `a49f93ad9fbe27dc-ATL` and
`a49f93f9dd4c317a-ATL`. Prefix `osdev-frame-tree-opaque-candidate51`.
Trace-off51 also fails after one actual checkbox click and 90 seconds: two root
HTTP 403 responses, 316 complete paints and no unhandled script errors. Prefix
`osdev-frame-tree-opaque-candidate51-normal`. Both final screenshots were
inspected and still display verification. No HTTP200 OSDev document or actual
site content has been observed; the rejection reason remains
UNKNOWN. Do not invent a missing API/TLS/IP cause or claim these fixes solve it.
Cloudflare's current official custom-engine support and expected PAT/DNS guidance
remain applicable background, not a diagnosed cause.

Fresh51 isolated frames/SOP/messaging/geometry/Wasm/workers and console KVM4
checks pass. Retained50 focus, activation, secure worker ancestry, referrers,
cookies/credentials, transport, MSE H264/AAC/canvas/PCM and codec reconfiguration
checks pass. Full native Browser51 TCG4 passes: wheel 0.46 seconds, dense/nested
scroll 0.63 seconds, actual input/history/storage/form/pixels and cleanup.
Source51 differs only in the checked opaque-origin serializer from50. The
package records unchanged broker/kernel/GUI/speaker/firmware/certificates/notices.

`make run` now honors ISO_IMAGE; `make run-image ISO_IMAGE=EXISTING_IMAGE`
launches existing bytes without a rebuild. `build/os.iso` was already different
at session start and remains untouched, SHA-256
`2adef1d80de6dad2162e5fadc7110e81a3dc9c6be88cf30d8acb9eb20f2d8cf0`.
No external support message, physical disk write, reboot, reset/clean or commit
occurred. This session used implementation and independent read-only review
agents; all owned test VMs are closed. Final reports are in
`cloudflare49/validation-results49.json`, `cloudflare50/validation-results50.json`
and `cloudflare51/validation-results51.json`.

Preservation correction: the first49 full native Browser run used the harness's
legacy `native-webkit` runtime prefix and could overwrite inherited transient
runtime log/screenshots. Preserved checkpoint directories and all Cloudflare
evidence remain. Subsequent50/51 legacy regressions use exclusive artifact
directories through `cloudflare50/run-isolated-test.py`; use the same method or
unique labels for future tests. Do not claim every old transient filename was
preserved, or relabel a new run as the original evidence.

## Preserved Cloudflare follow-up: candidate49 — 2026-10-13

OSDev access remains UNFINISHED. Candidate49 is immutable and unpublished:
`build/modern-browser/native-cloudflare-navigation-candidate49.iso`, SHA-256
`fdc413715900b461f3f3ad7d8e12235674caede1467a1bd346f391bf55a9842d`.
Installed browser binaries match49. Frame-tree source work for50 is currently
in progress; do not assume the evolving source is validated or matches49.
The pre-change49 sources are preserved under `cloudflare50/pre-change49/`.
No concurrent configure/Ninja builds are permitted.

The session found a genuine UI-navigation integration defect: the native port
never set WebCore's `isRequestFromClientOrUserInput` flag. Actual HTTPS fixture
requests from fresh/same-origin address navigation and F5 reload had incorrect
Sec-Fetch-Site values. `tools/test_native_fetch_metadata.py` fails on old48 and
passes on49 KVM4 and TCG4, including script navigation/reload, foreign iframe
and fetch exclusion and stable actual UA. A trusted broker provenance bit and
startup switch now mark only client navigation; renderer packets cannot claim
that bit. History, script navigation and embedded startup retain their separate
behavior. Native toolbar reload uses WebCore's real client flag. The pinned
upstream does not generate Sec-Fetch-User; no header or user activation is forged.
UI cross-origin HTTP redirect provenance and redirect POST method conversion
remain separate untested gaps; the observed OSDev path contains no HTTP3xx.

Normal49 OSDev clicked the actual checkbox and waited 90 seconds with exception
observation off. It still returns two root HTTP 403 responses and a visibly
rendered verification page, not site content. It has 283 completed paints and
no unhandled script errors. Clearance is stored/sent and the complete header
matches the jar. Both root requests use h2/TLS 1.3 with zero certificate errors
and -2 seconds Date skew. Initial Sec-Fetch-Site is now correctly `none`; the
script followup is `same-origin`. Rays: `a49ebceb4d6014c7-SJC` and
`a49ebd242fd591f0-SJC`. The header fix did not establish Cloudflare acceptance.
Validation and exact source/package manifests are in `cloudflare49/`.
Full native Browser TCG4, console KVM4 and transport KVM4 also pass. GUI was
rebuilt after the protocol header edit; all its loadable ELF segments match48
exactly, while debug information differs. The first overly strict GUI raw-hash
package assertion failure is retained; corrected segment verification passes.

A second genuine defect is reproduced by `tools/test_native_frame_graph.py`:
the real top has a local first iframe and foreign second iframe, while the
foreign child sees parent.length=1, itself at parent[0], an exception at
parent[1], and empty window.name instead of its assigned name. Parent===top and
postMessage source identity pass. Old48 KVM evidence and source are preserved
under `cloudflare49/evidence/old48-frame-graph/`. Cloudflare relevance remains
unproved. Candidate50 work uses genuine WebCore remote trees, bounded broker
validated structural snapshots, true ancestors/siblings/names and real message
routing; no JavaScript accessor shim. This work is not yet validated.

The preexisting Makefile hard-coded48 in `make run`, ignoring ISO_IMAGE. Run
now honors the variable; new `make run-image ISO_IMAGE=EXISTING_IMAGE` launches
packaged bytes without rebuilding that image. Both dry-run checks pass. At
session start `build/os.iso` already differed from the handoff, SHA-256
`2adef1d80de6dad2162e5fadc7110e81a3dc9c6be88cf30d8acb9eb20f2d8cf0`.
It is unchanged by this session. Preserve it and all older images/evidence.
The current session uses read-only audits and isolated implementation/review
agents. No external message, physical disk write, reboot or commit occurred.

## Preserved Cloudflare follow-up: candidate48 — 2026-10-13

OSDev access remains UNFINISHED. The user confirmed OSDev opens in their Linux
browser on this same computer/network. Current source and installed binaries
match `build/modern-browser/native-cloudflare-diagnostics-candidate48.iso`,
SHA-256 `08bc940734190611515d738bab4c8d070b99787e89e1fc111c0185795324d5b5`.
The default `build/os.iso` was not replaced. Preserve all earlier images and
the extensive dirty/untracked OS source.

A genuine console defect is repaired. JSC's ScriptArguments converted logged
objects to strings; the native all-arguments log also repeated coercion of the
first argument. A controlled page on46 recorded 51 unintended page-code calls
in both its main document and foreign frame. The shared JSC formatter now uses
side-effect-free Error previews and object labels, preserving primitives and
Proxy handling. It does not invoke object toString/Symbol.toPrimitive, array
element coercion, or Error getters. Frame, worker and inspector formatting use
the same implementation. The owned pinned patch is in tools/build_webcore.py.
New tools/test_native_console.py verifies real main/frame/Blob-worker execution,
getter/proxy/revoked-proxy exclusion, primitive/error logs, timers and pixels.
Make has a test-native-console target. The initial47 pixel assertion sampled
inside the foreign iframe; the corrected sample and bounded paint wait pass.
Do not count that initial failed assertion as a completed test pass.

Candidate47 contains the console fix; candidate48 adds opt-in transport observations only.
`--browser-network-trace` now compares the full outgoing Cookie header privately
against Soup's selected cookies and records negotiated HTTP/TLS, certificate
errors, server Date skew, cf-mitigated and cf-ray. Cookie values are never logged.
tools/test_native_transport.py checks a long HttpOnly cookie's complete value,
log privacy, credential omission, timing/CORS privacy and genuine clock/TLS data.
tools/test_osdev_verification.py includes these observations in its JSON report.
Resource/frame IPC versions and normal network behavior are unchanged.

Normal47 and traced48 OSDev runs each clicked the actual rendered checkbox and
waited 90 seconds. Both still returned two HTTP403 root documents. Final48 had 282
complete paints, no unhandled script errors, and clearance stored/sent. Both
root requests used h2/TLS 1.3, certificate_errors=0 and Date skew=-2 seconds; the
outgoing Cookie header matched the jar exactly, including both cookies after
verification. Both responses explicitly say cf-mitigated=challenge. Ray IDs:
`a49e17e36b04c38b-SJC` and `a49e181e4a9e6cb7-SJC`. The remaining rejection reason
is UNKNOWN. The console fix did not establish Cloudflare acceptance. Do not
blame an unproved missing API, TLS fingerprint or IP reputation. Cloudflare's
official docs describe custom engines as having limited support, and failed PAT
or wildcard DNS probes as potentially expected, non-fatal behavior.

Passing checks: console KVM4 on47 and TCG4 on48; enhanced transport KVM4 on48;
exception observation off/on KVM4 on48; full native Browser TCG4 on48 (wheel 0.44s,
dense scroll 0.62s); exact ISO extraction, installed/staged hashes, certificates,
firmware, notices and static ELF protections. Package48 verifies the renderer
is unchanged from47; package47 verifies other components unchanged from46.
All raw failed/successful runs and source snapshots are under cloudflare47/48.
Public prefixes are osdev-investigation-20261013-baseline (46),
osdev-console-candidate47-normal and osdev-diagnostics-candidate48.
No external support message, physical disk write, reboot, commit or agent
delegation occurred. The user subsequently reported that the suggested updated
build still did not work and requested a prompt for another AI session. Exact
steps for that attempt were not supplied. OSDev access is still unresolved;
do not treat the new build as a verification fix.

## Preserved Cloudflare follow-up: candidate46 — 2026-10-12

OSDev access remains UNFINISHED. Current source and installed browser match the
new immutable, unpublished
`build/modern-browser/native-cloudflare-activation-target-candidate46.iso`, SHA-256
`c9c0233b07fce50a5b5d22d430ecfeb3ea2d204e36ad77de1cf5addebbea66d4`.
Exact extraction, installed/staged hashes, static ELF protections and source
copies are in `build/modern-browser/cloudflare46/package/manifest.json`.
Kernel, GUI, speaker-check, amplifier firmware, certificates and media notices
match43. No default image was published or overwritten.

Two real input integration gaps are repaired:

- Isolated frame focus was not synchronized. A child click focused its own
  document while the parent exposed the wrong active element; later parent
  clicks did not blur the child. Chrome's real focused-frame callback now
  updates the broker's frame path. WebCore owns activeElement, hasFocus,
  focus/blur events and keyboard targeting. One/two isolation boundaries,
  ancestor focus and trusted keyboard routing are tested by
  `tools/test_native_frame_focus.py`. The original43 image fails;44 and46 pass.
- The stable upstream User Activation API was disabled, and real activation
  callbacks were dropped at process boundaries. It now exposes WebCore's
  actual state. The broker relays original monotonic timestamps to ancestors
  and same-origin descendants and propagates transient activation consumption
  across the browser tree. Ancestor updates follow the actual remote owner
  chain, excluding unrelated local siblings. Parent pointer handling defers
  its gesture until WebCore identifies a local target: forwarding a foreign
  click must not activate the parent's same-origin descendants.

`tools/test_native_activation.py` checks synthetic click exclusion, actual
clicks/keys, two isolation boundaries, same-origin descendant propagation,
local sibling exclusion, picker consumption, sticky state and timer expiry.
It fails on44 because the API is absent and on intermediate45 because a
foreign click incorrectly activates another-origin descendant. The first45
test mistakenly matched a success phrase inside a failure title; raw logs
exposed this. Exact title matching and failure-log rejection corrected the
test. Do not count the initial45 PASS line as a pass; its audit and corrected
old-image failure are preserved in `cloudflare45/`. Candidate44/45 ISO files
and source/binary snapshots remain preserved. Final46 passes both KVM4 and TCG4.

Final46 also passes focus TCG4, full native Browser TCG4, isolated frames/SOP/
geometry/messaging/Wasm/workers KVM1 and KVM4, credentials/CORS/HttpOnly/Blob
workers KVM4, and full native MSE H264/AAC/canvas/seek/buffering/PCM KVM4.
Wheel scrolling settles in 0.44 seconds; source and compiler checks pass.
Resource IPC remains version2; frame IPC is version4, so rebuild broker and
renderer together. Source includes a pinned EventHandler target-selection
patch and the extensible Chrome focused-frame callback in build_webcore.py.

Normal46 OSDev still returns two root HTTP 403 documents after a rendered-widget
click and 90 seconds. It produces 295 completed paints, no unhandled script
errors and issued/stored/sent clearance. Actual site content never loads.
Prefix: `osdev-cloudflare-activation-target46`. The official HTTPS Turnstile
Troubleshooter also received a real rendered-widget click with exception tracing
OFF; it reports 600010 and "Turnstile Failed Despite Passing All Checks".
Screenshot/log/result prefix: `cloudflare-widget-candidate46-normal`.
The rejection cause remains unidentified. Do not blame an unproved missing API
or claim that these input corrections established Cloudflare acceptance.
No external support message was sent. All owned test VMs are closed.
Validation and copied evidence are under `cloudflare46/`; preserve43 and all
older fixes. Continue from46 source, not the intermediate45 image.
The final default scan confirms `build/os.iso` still has SHA-256
`07bd4e3e3203df47ea0e7d4e9b9e7043bb4e836820289c42ce4f27f73eeb956e`
and the friendly media ISO still matches38. No holders were found and neither
image was replaced; exact state is `cloudflare46/default-state46.json`.

## Preserved Cloudflare follow-up: candidate43 — 2026-10-12

OSDev access remains UNFINISHED. Source and installed browser match the new
immutable, unpublished `build/modern-browser/native-cloudflare-cookie-context-candidate43.iso`,
SHA-256 `927d472c84048b704b77db6227f758ed2048782a34abfc380da0f9fd285bce84`.
Only the broker changed from42; renderer, GUI, kernel, certificates, firmware
and speaker-check are identical. Exact extracted image/staged/installed hashes,
static ELF protections and source copies are in `cloudflare39/package43/`.

A fourth integration defect was reproduced: isolated cross-site DOM cookie
reads exposed SameSite=Lax/Strict cookies excluded from HTTP requests; DOM writes
used the child's URI as its own first party, bypassing the jar's existing
NO_THIRD_PARTY acceptance policy. DOM reads now use Soup's real SameSite query
with the trusted top origin and ancestor-derived cookie site. Writes use the
real first party. Policy, existing SameSite=None cookies and HttpOnly protection
are preserved. `tools/test_native_cookie_context.py` checks actual server Cookie
headers and native DOM results on same-site and cross-site HTTPS frames. It fails
on42 (both reads and writes) and passes on43 KVM4 and TCG4. Existing credentials/
CORS/HttpOnly/Blob workers KVM4 and full native Browser TCG4 also pass; scrolling
settles in 0.44 seconds and dense scrolling in 0.63 seconds. Raw logs and results
are in `cloudflare39/evidence43/` and `validation-results43.json`.

The normal43 OSDev run clicked the rendered widget and waited 90 seconds: two
root HTTP403 responses, 280 completed paints, no unhandled script errors. Clearance
was issued/stored/sent; actual site content never loaded. Prefix:
`osdev-cloudflare-cookie-context43`. The official HTTPS Turnstile Troubleshooter
also received a real rendered-widget click, with opt-in exception observation.
It reports 600010 / "Turnstile Failed Despite Passing All Checks". The caught
exceptions do not establish a particular missing interface or rejection cause.
This diagnostic is distinct from normal tracer-off OSDev evidence. Do not claim
success or blame an unproved capability gap. Prefix:
`cloudflare-widget-candidate43-trace`. No support message was sent. All owned
test VMs are closed. Preserve current43 source and all earlier evidence.

The final default-image check found `build/os.iso` changed since the previous
checkpoint: SHA-256 `07bd4e3e3203df47ea0e7d4e9b9e7043bb4e836820289c42ce4f27f73eeb956e`.
It was left untouched; no candidate was published. `native-media-scroll.iso`
still matches38. A sequential fuser scan after hashing found no remaining
holders. Actual hashes and scan output are in `cloudflare39/default-state43.json`.
Do not restore or replace the changed default based on the old38 checkpoint.

## Preserved Cloudflare follow-up: candidate42 — 2026-10-12

The requested OSDev access was still UNFINISHED. This preserved checkpoint's
renderer/broker matched the then-new immutable, unpublished
`build/modern-browser/native-cloudflare-referrer-candidate42.iso`, SHA-256
`dcddc980ba533bdf7f090e33b714b456e5d3b66bd68c4518e09f9f3ca70034c2`.
`build/os.iso` and `native-media-scroll.iso` still match published candidate38;
no candidate/default was overwritten. Kernel, speaker-check, amplifier firmware
and certificates are unchanged; exact extracted files and source hashes are in
`build/modern-browser/cloudflare39/package42/manifest.json`.

Three genuine integration defects were reproduced and repaired:

- A remote parent's FrameTreeSyncData origin does not initialize Page's separate
  top-document origin. Isolated HTTPS workers therefore reported insecure and
  lacked crypto.subtle. The broker now passes the real top origin from its
  frame graph, and the renderer uses Page::setMainFrameURLAndOrigin. HTTP ancestry
  stays insecure, including two isolation boundaries; actual SHA-256 is tested.
- Broker responses discarded Soup's timing/protocol/byte measurements and the
  embedding omitted default Accept-Language. The version-2 resource metadata
  carries actual CLOCK_MONOTONIC measurements. WebCore's real TAO checks and
  sticky redirect failure state hide private cross-origin timings; WebCore
  additionally hides cross-origin byte counts even with TAO. Header locale
  matches Navigator, preserving explicit values. History replay omits old
  measurements rather than reporting a prior navigation's timestamps.
- Frame creation discarded WebCore's computed HTTP referrer and always sent a
  canonical parent origin without URL serialization or the requested policy.
  Version-2 frame metadata now carries URL/NUL/referrer. The broker validates
  its source origin and control characters. Parent-initiated child navigation
  starts a fresh document, preserving strict-origin, no-referrer and unsafe-url.

New tests are tools/test_native_worker_context.py, test_native_transport.py and
test_native_frame_referrer.py. Their old-image failures, intermediate candidates
39/40/41, new passes, source snapshots and package evidence are retained under
cloudflare39/. Candidate40 is intermediate; encoded byte counting and timing
privacy were corrected in 41. The initial 41 transport assertion incorrectly
expected cross-origin byte counts with TAO; pinned WebCore intentionally hides
them, and the corrected test preserves that privacy behavior. No success was
claimed for the failed runs.

Candidate42 has seven passing regression groups in cloudflare39/validation-results42.json:
worker contexts on one-CPU KVM; transport on four-CPU TCG; referrers on four-CPU
KVM and TCG; full frames/SOP/Wasm/workers on one- and four-CPU KVM;
credentials/CORS/HttpOnly on four-CPU KVM; and full native Browser on four-CPU
TCG. Raw logs, final OSDev screenshot/result and source are retained separately.
Candidate41 also passed transport and worker contexts on four-CPU KVM, plus
frames on one/four KVM and full Browser TCG. Full42 Browser retains scroll
copies, real fonts/forms/history/storage/downloads, busy-script paints and cleanup.
Package42 checks exact image/staged/installed binaries, certificates, firmware
and license files, static ELF/no-WX protections, and unchanged38 kernel/speaker/
firmware/certificate hashes. All owned test VMs are closed.

Normal OSDev checks on 38, 39, 40, 41 and 42 each clicked the actual rendered
widget, waited 90 seconds and still received HTTP403 root documents. Candidate42
has no unhandled script errors, issued/stored/sent clearance and 306 completed
paints, but its title stays "Just a moment...". Result/log/screenshot prefix:
osdev-cloudflare-referrer42 in build/runtime-tests/. Cause remains unidentified;
the fixed defects do not establish server acceptance. Direct host HEAD probes
to www.osdev.org, wiki.osdev.org/Main_Page and forum.osdev.org also returned
Cloudflare403. Do not fabricate browser identity/APIs, timestamps, cookies or
results, or interpret clearance issuance as actual access.

Preserve all previous source/vendor artifacts and improvements. Source builds
incrementally; both the broker and renderer must be rebuilt with version-2
headers. Build a NEW immutable ISO, never overwrite a held image, and retain
real default backups before any future publication. No host reboot, physical
device write, external support message, commit or subagent was used.

Candidate42 retry evidence: three actual rendered-widget clicks and a final
90-second wait still returned four HTTP403 roots, with no unhandled script
errors and 637 completed paints. Prefix: osdev-cloudflare-referrer42-retry3;
validation-results42.json and evidence42/ retain the exact result/log/screenshot.

## Published candidate38: codec reconfiguration and completed speaker validation — 2026-10-11 (UTC 2026-10-12)

Published defaults `build/os.iso` and `build/modern-browser/native-media-scroll.iso`
now exactly match immutable `build/modern-browser/native-media-reconfiguration-candidate38.iso`:
SHA-256 `cfdf8ee6405f9df4075b32f1b8cdbf1daa5755cd8fc196c7698caec655dcf038`.
Publication occurred at 2026-10-12T05:54:04.240198+00:00 after a successful unrestricted host
fuser scan of both target paths (exit1, empty stdout/stderr: no holders), then
atomic replacement. Exact package extraction, ELF protections, source snapshot,
regressions and publication records are under `media-reconfiguration38/` and
`media-package-verification38/`. The root manifest and validation JSON select 38.

Immutable candidate32 still has its original
`4971b71f83b65a9a9d6953f86ea749a34779b358ac7b00a3262a420787b3b59c` bytes.
Both actual previous defaults were copied before replacement. Importantly,
`build/os.iso` had changed during this session to
`f1a3e45b8d69b3f888d0cea1d80198f039a6382fce8ed92efaf5b7a0a494aa1a`;
its observed mtime was 21:25:55 local, and the Makefile also changed after the
first inspection. The writer was not identified; those changes were preserved.
The actual OS backup is `os-before-media38-20261012T054933Z.iso`, and the prior
candidate32 friendly alias is `native-media-scroll-before-media38-20261012T054933Z.iso`.
Do not falsely relabel the former as candidate32. All prior candidates remain.

Physical speakers are a user-confirmed PASS on candidate32. After saying that
he had tested, the user reported **"the speaker test works, thats all that metters"**.
No raw console/per-channel transcript was supplied; none should be demanded.
`physical-speaker-confirmation32.json` preserves the report. Candidate38's kernel,
speaker-check and amplifier firmware are byte-identical to 32, so its evidence
retains that physical result explicitly on matching components. GPIO140 reset
and the untouched GPIO171 interrupt remain as implemented. No host reboot or
raw physical disk write was performed. The user then said no more questions,
authorized continuing and went to sleep. Honor that preference.

The night's JSC compile blocker is fully resolved. `NativeExceptionDebugger`
uses the public `Debugger::Observer::didPause` exception value and pinned
`dynamicDowncast`, immediately continuing through `runEventLoopWhilePaused`.
Observer lifetime and a Page scope guard cover detach/early returns. All trace
flags remain opt-in and GUI help is complete. It bounds 128 distinct messages
and 256 script URLs; repeated caught probes no longer exhaust the log budget.
For indexed undefined accesses it reads direct lexical value slots/dense array
entries through `VMInquiry`, never evaluating JS or calling getters. Only
value types and a fixed public Web-name allowlist are logged. Arbitrary strings
and private cookie values are excluded from this local inspection. The JSC
private header paths are confined to renderer-fixture.cpp; WebCore's config
header is chosen first, and media retains its original include environment.
`tools/test_native_exception_trace.py` passes off/on, including genuine values,
no getter/proxy calls, private-string omission, timers, foreign frames, HTTPS,
origin changes/reload and cleanup. Worker VMs are not attached to this observer.
Diagnostic timing is not production acceptance evidence.

A real MSE issue was found while preserving YouTube: a new AVC init segment
replaced the decoder configuration even for older retained coded samples.
Seeking/playing those samples under the newer SPS/PPS produced H264 errors
and gray pictures. These errors also occurred on baseline32; they were not
identified as an exception-tracer regression. Candidate37's controlled two-
resolution regression fails (red clip becomes gray130 after a blue init).
Candidate38 retains immutable copied AVCodecParameters with each NativeMediaSample,
including copies/trims, and selects its original configuration during decoding.
It separates parser and active configurations, drains delayed old pictures,
opens the replacement before releasing the old context, and preserves shared
picture/PCM queues and clocks. No guessed colors, clocks or media APIs were added.
The old/new repeated seek regression now passes with independently decoded
canvas pixels and no H264 errors.

The live YouTube test now counts motion only between two detailed player patches
(over 128 sampled colors each). This is stricter: monochrome loading/fade changes
cannot terminate the check early. The original five-second PCM, detail and motion
requirements and 30-second KVM observation budget remain. Failed candidate36
long runs, baseline32 comparison and all intermediate builds remain preserved.

All 12 candidate38 regression checks pass, with fresh evidence under
`media-reconfiguration38/evidence/`: exception observer off/on; configuration
replay; full MSE H264/AAC/CABAC/B-frame/canvas/callback/seek/early-seek/buffering
and PCM on four-CPU KVM AND TCG; HTTPS credentials/CORS/HttpOnly/Blob workers;
frames/SOP/messaging/geometry/trusted input/Wasm; smooth scrolling; full native
Browser on four-CPU TCG; short and long live YouTube on KVM; recorded HDA speaker
sequence. Native scrolling settles 277-279ms with 13 WebCore positions. Short
YouTube has 9/9 detailed changes and 17.86 seconds sound; Big Buck Bunny has 9/9
and 14.46 seconds, zero H264 decode errors. Package boot files, licenses,
certificates, firmware and static ELF protections pass. Older tests remain
labelled by their real candidate; none was relabelled as a new pass.

Priority1 remains UNFINISHED: actual HTTPS OSDev content was not rendered.
The tracer-off candidate38 run clicked three real empty widgets at least 15
seconds apart, got four root document responses all HTTP403, and remained on
`Just a moment...` after the final 90-second wait. Clearance was issued, stored
and sent; no unhandled script errors appeared. Logs/JSON/screenshots use
`osdev-production-candidate38-retry3` in `build/runtime-tests/`, summarized by
`media-reconfiguration38/osdev-result.json`. Candidate36 diagnostics and normal
runs have the same genuine failure. Cookie loss remains ruled out; rejection
cause is unidentified. Preserve origin/TLS/UA/Fetch Metadata/credentials/input,
Wasm/workers and clocks. Do not fabricate APIs, identity or success.

New independent evidence: a real click on the official HTTPS Turnstile
Troubleshooter's widget also fails with 600010, despite all four displayed
preflight checks passing (automation, clock, privacy, server reachability).
The page says "Turnstile Failed Despite Passing All Checks"; this is not an
OSDev-specific clearance/reload test. Screenshot/raw logs use
`cloudflare-widget-candidate38-r3` in `build/runtime-tests/`. The first two
attempts did not click: one-pixel light outlines and asynchronous widget
startup needed handling. Their evidence is retained. The checkbox helper now
handles one/two-pixel edges and an optional light-outline mode, tested on actual
saved native screenshots; the diagnostic runner waits for the visible widget.
No support message was sent. 600010 is still a generic rejection, not a cause.

Host DNS observed brunhild.challenges.cloudflare.com as IPv6-only; the host
only had link-local IPv6 and TCP failed with errno101, while ModernOS is IPv4-
only. This explains the unusable-address error, not Cloudflare rejection.
Evidence is `exception-diagnostic33/host-ipv6-probe.json`. Do not guess that this,
missing graphics/audio/database APIs or the intentional upstream Version/60.5
UA token caused rejection. Investigate real integration defects/actual evidence.

The original uncompiled night state remains in exception-diagnostic33-handoff/.
New candidates 33-37 are intermediate/historical, including 35's incorrect
getPureResult lexical null diagnostic. Current source and installed userland
match 38; no unfinished compile blocker remains. Build incrementally into a
NEW unused immutable candidate, never configure/Ninja concurrently, preserve
actual defaults and obtain a successful unrestricted fuser scan before publishing.
The current Makefile's run target uses ISO_IMAGE and depends on iso: do not
point a rebuilding recipe at an existing immutable candidate. Use explicit
--iso paths for VM checks. No reset/clean, source/evidence removal or subagent
was performed by the agent. Physical I226 remains unverified; Wi-Fi7 unsupported.

## RETAINED candidate32 initial checkpoint — 2026-10-11 (UTC 2026-10-12)

The newest immutable boot candidate is
`build/modern-browser/native-amplifier-candidate32.iso`, SHA-256
`4971b71f83b65a9a9d6953f86ea749a34779b358ac7b00a3262a420787b3b59c`.
`build/os.iso` and `build/modern-browser/native-media-scroll.iso` now contain
these exact candidate32 bytes, atomically published at
`2026-10-12T03:52:00.811512+00:00` after a successful unsandboxed fuser check.
The actual prior default differed from candidate27 by publication time; its
SHA-256 is `c8eb65fec627b87717633a0cc7a783c9732d6b3abe79ea1d69f70ab3d86a3bad`,
preserved as `build/modern-browser/os-before-amplifier32-20261012T034940Z.iso`.
The old friendly candidate27 image is separately preserved as
`native-media-scroll-before-amplifier32-20261012T034940Z.iso`; immutable
candidate27 and all older candidates remain intact. The manifest records the
actual backups, hashes, publication and fuser result.

The user copied this PC's root-protected DSDT and made the copy readable.
It is preserved at `build/physical-audio-acpi/DSDT.aml`, 645473 bytes,
checksum zero, SHA-256
`a5eb51ac942f7b05f8d014608a16db98c3ac91aa00c3d2c7efc7243128a175f8`.
ACPICA disassembly and primary Intel GPIO references are alongside it.
Speaker `_CRS` is at `\_SB.PC02.I2C2.SPKR`: 0x35/0x34, maximum 400 kHz,
reset GpioIo on `\_SB.PGPI` GPIO140/GPP_A12, computed by
`GNUM(0x0016040C)`. GPIO171 is the separate interrupt and is left untouched.
PGPI is INTC1084, Intel Meteor Point PCH, not the CPU INTC1082 controller.
MMIO comes from its second `_CRS` memory window. SPKR exposes no `_PR0`,
`_PS0` or `_DSD` here; I2C2's `_PS0` is empty.

New production `aw88399_power.c`/`aw88399_power.h` evaluates these exact
resources with LAI, validates identities/bounds, ownership and pad locks,
sets GPIO output/pull-up and performs one shared high/low/high reset with
1-ms holds before either amp's firmware loads. Kernel LAI SystemMemory,
port-I/O, PCI ECAM and time hooks now access real resources; their previous
stubs could not read the firmware NVS variables required by `_CRS`.
Firmware/DSP/CRC/factory trim/protection/fade and muted failures are preserved.
The new parser/GPIO code passes ASan/UBSan using copied firmware templates and
register models. This is not a physical audible result.

`/bin/speaker-check` plays quiet left/right/both tones, resets/pauses for two
seconds, checks the actual paused PCM clock, then resumes both speakers.
Candidate32 passes its actual recorded QEMU HDA stereo/pause/resume check on
four-CPU KVM. QEMU's WAV backend omits inactive DMA time and leaves zero RIFF
lengths at termination; the test preserves raw recordings and creates a
finalized-length copy, without inserting silent samples.
Physical instructions are in `Documentation/legion-speaker-validation.md`.
The user replied **"I can test it now"** to a request to manually boot
candidate32 and run `speaker-check` twice, then stopped for the night and
requested a continuation prompt. No audible result has been received.
Do not assume that the physical test happened or passed. Do not reboot the
host or write physical media on the user's behalf.

Cloudflare remains unfinished. A real fetch integration defect is fixed:
explicit cross-origin `credentials: include` now uses broker-owned destination
cookies. WebCore still enforces CORS, and cookies/HttpOnly remain outside the
renderer. SameSite cookie context is derived from the trusted frame ancestry,
including an opaque cookie site below foreign ancestors. The focused native
HTTPS regression fails on old candidate27 and passes on candidates28/31,
including omitted credentials, HttpOnly, CORS denial and Blob-worker fetch.

Opt-in diagnostics are available with `gui --browser-network-trace`. They
record cookie names/attributes, actual HTTP User-Agent and Fetch Metadata,
without cookie values or challenge bodies. Native console output now includes
all real console arguments, bounded to 4096 characters. Diagnostic candidate30
compiled a platform guard that was false for the reused WPE configuration;
candidate31 contains the corrected console change. Do not assume candidate30
captured all arguments.

Live `tools/test_osdev_verification.py` still fails on candidate31 after the
real checkbox click and 90 seconds: HTTP403 / "Just a moment...", no unhandled
script errors. Crucially, candidate29/31 logs show Cloudflare **issued** an
OSDev `cf_clearance`, Soup **stored** it, and the next genuine navigation
**sent** it, yet got 403. This rules out loss of that cookie during reload;
it does not identify the underlying rejection cause. HTTP identity stays
consistent on top/foreign documents and requests, including Fetch Metadata.
The odd `Version/60.5` token is the pinned upstream GLib User-Agent's intentional
compatibility token, not a newly found ModernOS version bug. No UA spoofing,
TLS bypass, origin relaxation, fabricated API or challenge result was added.
Some live runs also report DNS failure for brunhild.challenges.cloudflare.com;
its causal relevance has not been established.

Current regression results: four-CPU TCG full native browser on candidate31
passes pixels/input/TLS/history/cookies/storage/resize/cleanup; four-CPU KVM
candidate32 native/browser scrolling passes (13-14 WebCore positions; native
settling 274-283 ms). Candidate32 live short YouTube video passes 6/9 changing
player snapshots after five seconds of actual HDA sound, 18.14 recorded seconds.
Candidate32 long Big Buck Bunny also passes 6/9 changing player snapshots and
14.04 seconds actual HDA sound after five PCM seconds, four-CPU KVM.
Candidate27 MSE/file/TCG evidence remains intact and explicitly labelled as
retained in the validation JSON. Exact extracted candidate32 boot files,
host amplifier firmware/certificates/notices and static ELF protections pass.
Extraction also proves candidate31's kernel/renderer/broker/desktop are
byte-identical to candidate32, supporting the retained candidate31 browser,
credentials and Cloudflare results. The new package and source snapshot are
under `build/modern-browser/amplifier-package-verification32/`.

New immutable candidates28/29/30/31/32 preserve the progression. Previous
mutable native-browser/scroll/YouTube evidence was copied to
`build/modern-browser/pre-candidate31-retained-evidence/` before reruns.
Never reset/clean the extensive dirty/untracked source. Build incrementally,
never configure/Ninja concurrently, preserve old images and run a successful
unsandboxed fuser check immediately before default publication. The sandboxed
fuser invocation reported "Cannot open a network socket" and is not proof
that an image is unused. No physical disk write, host reboot or subagent was
performed. Physical stereo and genuine OSDev content are still pending.

### Night's stopping point: uncompiled exception diagnostic

**Published candidate32 remains validated; the current working source differs
from it and does not compile yet.** Immediately before the user stopped, an
opt-in JSC exception diagnostic was started to investigate real caught errors
in the challenge. It is wired through `gui --browser-exception-trace`, broker
`--exception-trace`, renderer `--exception-trace` and the OSDev test's new
`--exception-trace` option. These are source edits, not installed functionality.

The incremental `ModernOSRenderer` build exited 1: in
`userland/modern-browser/port/renderer-native.inc`, `JSC::jsDynamicCast` was
not found in that namespace and `Debugger::exceptionOrCaughtValue` is private.
No diagnostic candidate33 ISO was built or tested. Renderer/broker/kernel
installed binaries and both published ISO aliases were rehashed after failure
and still match validated candidate32. The failed build is finished; no new
build was started after the user stopped.

Preserved logs, a snapshot of the nine affected source files, and exact state
are in `build/modern-browser/exception-diagnostic33-handoff/`:
`native-exception-renderer-build33.log`, `webcore-llint-build.log`,
`uncompiled-exception-trace-source.tar.gz`, and `state.json`. The clean
candidate32 source snapshot in `amplifier-package-verification32/` predates
these edits. Preserve both snapshots and the entire working tree.

Next session, inspect this compile blocker before building another image. The
pinned JSC `Debugger::Observer::didPause(JSGlobalObject*, DebuggerCallFrame&,
JSValue)` is a public callback supplying the actual exception value; inspect
its lifecycle and the proper cast API if finishing this diagnostic. Any tracer
must immediately continue real execution, preserve isolation and clocks, and
remain opt-in; debugger timing is not production acceptance evidence. Desktop
help text has not yet been updated for the extra trace flag. Do not infer a
Cloudflare cause from the unsuccessful diagnostic build.

For physical speaker diagnosis, the GPIO implementation currently accepts
revision IDs from 0x92 through below 0x110 (16-byte pads and legacy PAD_OWN
layout). It rejects newer revisions muted; the primary Intel reference also
documents their different ownership layout. The actual physical revision has
not been measured. Use real boot diagnostics before deciding this needs a
change. GPIO171 must remain untouched.

## RETAINED candidate27 media, frames and smooth-scrolling checkpoint — 2026-10-11

This section is the historical candidate27 record. The ACTIVE checkpoint above
supersedes its image selection and statement that ACPI reset is unimplemented.

Read `Documentation/browser-media-scroll.md` for implementation and hardware details.
This checkpoint supersedes every older section below. The selected clean image
is `build/modern-browser/native-media-scroll.iso`, an exact copy of
`native-media-scroll-candidate27.iso`, SHA-256
`d872523723a42e20545995aa607c38b51ac7af98cd8f420a64a2805f0b878cb0`.
`build/os.iso` contains the same bytes. The actual previous default is preserved;
see `build/modern-browser/media-scroll-package-manifest.json` for its backup.
Its hash differed from the older v38 checkpoint when publication began;
the historical v38 image remains intact as `native-browser-v38.iso`.
Diagnostic candidates20, 23 and 24 contain temporary probes; never publish them.

The renderer is genuine static WebCore/JSC 2.54.0 with CPU Skia and an
origin-bound, certificate-checking Soup broker. LLInt and Wasm IPInt execute
real code; JIT/DFG/FTL/BBQ/OMG remain off. GC remains enabled. The installed
clean renderer has SHA-256
`f8070a28d240118769f8d4acb23869758d8f1bbeabb7b576407f4133e9e36a75`.
No temporary media/DOM diagnostics remain in production source.
The kernel has SHA-256
`2189fda673f7664e3a8e0a3e506cc0f16977c748455f187a5c28244af5d0846e`.

Implemented since v38:

- Bounded native FFmpeg 9.0.2 file video/audio and MP4 H.264/AAC MSE,
  canvas video drawing, B-frame ordering, buffering, pause, seek and resume.
  The playback clock follows actual kernel PCM queue consumption, preserving
  unheard samples when JavaScript stalls. A busy 800-ms DOM task fails the old
  wall-clock implementation and passes the corrected device clock.
- Padded 64-byte RGB rows and an allocation guard prevent FFmpeg SIMD writes
  beyond tight buffers. The actual production-library canary overwrites 24
  guard bytes at width426 without padding, and zero with padding. The old MSE
  guest crashes in the allocator; the corrected detailed 426-pixel tests pass.
- Real `requestVideoFrameCallback` metadata/cancellation, actual picture PTS,
  dimensions and presented-frame counts. Presentation/expected-display times
  use monotonic software submission, not physical scanout. Sixteen PCM chunks
  can fill the same bounded 200-ms queue per tick; late decoded video can skip
  RGB conversion while preserving dependent decoding and paused seek targets.
- NASM 2.16.03 and SSE decoder paths with two frame/slice threads. AVX/XOP/FMA
  and extended YMM/ZMM state are disabled in both build and CPU dispatch.
  Max video dimensions are 1920 by 1080; AV1, MSE WebM and DRM are unsupported.
- Genuine immutable-origin foreign frames, RemoteFrame/window messaging,
  structured clones, local Blob resource loads, dedicated workers and real
  WebAssembly arithmetic/imports/growth/streaming/validation. Actual display,
  outer-window and global pointer coordinates propagate into child processes.
  Unsupported capability transfers throw rather than detach data.
- Time-based damped pixel scrolling for Files, Editor, Terminal, Settings and
  menus, plus WebCore's real browser ScrollAnimator. Fractional rows and pixels,
  nested targets, reversal and cancellation are checked through actual guest
  input. Desktop completed-frame target is 120 Hz; there is no GPU vblank path.
- Kernel-owned AC'97 and HDA PCM DMA. AC'97 now starts RUN only once, using LVI
  for subsequent writes. Rewriting RUN discarded descriptors in QEMU and made
  the device clock jump to the final scene of a six-second file; the corrected
  one/four-CPU KVM tests pass with actual continuous PCM output.
- Arrow Lake HDA/ALC287 subsystem 17aa3907 speaker routing, DesignWare I2C2 and
  dual AW88399 firmware/DSP/protection path. The firmware is loaded from the
  user's `/lib/firmware/aw88399_acf.bin` during packaging, not vendored. No
  physical audible-output claim: amplifier GPIO/ACPI reset sequencing is not
  implemented, and this PC has not been booted into the new image.
- A separate Intel I225/I226 advanced-DMA wired NIC driver. QEMU 82576 `igb`
  validates real DHCP/HTTP/TLS/Git/offline loopback; the I226 PHY needs physical
  boot validation. Use wired Ethernet; the PC's Intel Wi-Fi 7 is unsupported.

Final candidate27 passes native MP4/WebM/canvas/seek/pause/frame callbacks with
AC'97 on one/four-CPU KVM and HDA on four-CPU TCG. Detailed MSE independent
CABAC/B-frame pixels, pause/seek/restart, early metadata seek, callbacks,
delayed-append clock hold/resume and HDA PCM pass on four-CPU KVM and TCG.
Audio-only AAC/MP3/Opus/device-clock/busy-JavaScript/pause/seek passes on
four-CPU TCG AC'97. The actual guest boot-file export on one CPU matches the
installed renderer exactly and passes memory/copy/resize/compiler checks.

Retained clean checks: full native Browser suite plus actual HTTP/navigator
User-Agent agreement on four-CPU TCG (candidate21); native/browser scrolling
on one/four-CPU KVM (candidate19) and TCG (candidate21); frames/Wasm/workers/
display/window/global-pointer checks on one/four KVM (candidate19); sandbox
and IPC on one/four TCG; desktop settings/persistence and QEMU82576 advanced
networking on four KVM. Host ASan/UBSan exercises actual smooth-scroll,
amplifier firmware/core and I226 register-model code. These retained runs are
labelled separately from final candidate27 media tests in the validation JSON.

Live YouTube Big Buck Bunny passes on final candidate27, requiring five
seconds of actual PCM before measuring further video motion. Four-CPU KVM
has 9/9 changing player snapshots and 13.47 seconds of recorded HDA sound;
four-CPU TCG has 3/9 changing snapshots and 5.57 seconds. The title says “4K”,
but the selected stream is within the native 1080p bound. TCG remains slow;
these results do not establish uninterrupted high-resolution playback.
No YouTube DOM, script priorities or clocks are altered. Actual browser API
coverage remains incomplete (WebGL/WebGPU, WebAudio, IndexedDB and service
workers are disabled).

OSDev still returns Cloudflare's actual HTTP 403 challenge. The real foreign
widget runs workers and accepts trusted checkbox input. Earlier diagnostics
captured generic rejection 600010; the official diagnostic page reported
synchronized clock/reachable server/no detected automation/privacy problem.
Real screen/window/pointer metadata is now correct, and HTTP/script User-Agent
consistency is verified. These fixes have not established Cloudflare acceptance.
Final candidate27 KVM clicked the actual rendered checkbox and remained on
“Just a moment...” after 90 seconds, with no unhandled script errors and no
HTTP200 OSDev document. The underlying rejection cause remains unknown.
Do not fake a pass or relax origins/TLS to make it pass.

The actual ISO kernel/initramfs were extracted to
`build/modern-browser/media-scroll-package-verification27/`. Hashes match
installed/staged kernel, renderer, broker, desktop, user-supplied AW88399
firmware, certificates and FFmpeg/NASM notices. Static ELFs have no interpreter
or writable/executable LOAD; renderer/broker have nonexecutable GNU_STACK.
`media-scroll-package-manifest.json` records publication and file hashes;
`media-scroll-validation-results.json` records actual passes, live CF failure
and physical limitations. Corresponding source and test evidence are retained
under the verification27 directory. The friendly and default ISO copies were
published atomically after checking no VM held the previous default.

Never configure or run Ninja concurrently. Build incrementally with
`python3 -u tools/build_webcore.py --interpreter llint --target ModernOSRenderer
--jobs 20`, then `make -j8 iso ISO_IMAGE=build/modern-browser/<new-candidate>.iso`.
Never overwrite an ISO in use. Inspect `fuser build/os.iso` immediately before
an atomic final publication; preserve existing defaults and all immutable
candidate images. No host reboot, device write, physical disk change, commit,
reset/clean, subagent or skill was used for this continuation.

## HISTORICAL native-browser continuation — 2026-10-11, v38 LLInt validated and packaged

This historical section records the v38 baseline. The ACTIVE media checkpoint
above supersedes its capability and packaging status. Continue the authorized
Google/YouTube/OSDev and scrolling repair in the actual x86-64 Limine UEFI OS.
Compatibility remains incomplete; do not claim working video or successful
OSDev verification. Preserve dirty/untracked source, archives, licenses, user
data, both interpreter caches and all failed/passing evidence. No reset/clean,
physical partition changes, fake site responses/APIs, TLS bypass, disabled GC,
relaxed origins or host-browser rendering was used.

The interrupted static x86-64 LLInt build resumed incrementally and succeeded.
LLInt is now selected after validation, with JIT/DFG/FTL and WebAssembly off.
The installed `userland/bin/web-renderer`,
`build/modern-browser/webcore-llint/bin/web-renderer` and saved uninstrumented
`v38-source-and-renderer/web-renderer` have SHA-256
`d23e8b3f1f009ec3c41b3219f72a1c00214ef5f5930a9817d75ace2a48f5a432`.
The static ET_EXEC has no PT_INTERP or writable/executable LOAD segment; its
stack is nonexecutable. The older C_LOOP cache and v37 ISO remain intact.

Selected candidate: `build/modern-browser/native-browser-v38.iso`.
Default `build/os.iso` is an atomic, byte-identical copy, SHA-256
`a946f48b6a5767e132b23c618eaba262a6fec3dbf5b371e96c3393cc53192c81`.
The prior default is preserved as
`build/modern-browser/default-os-before-v38-20261011T190855Z.iso`.
`v38-package-manifest.json` and `v38-package-verification/verification.json`
record the hashes. The candidate's actual initramfs and kernel were extracted;
its renderer matches the installed ELF. Both `build/initramfs.tar` and
`build/iso_root/boot/initramfs.tar` now match that validated initramfs. Previous
diagnostic staging is preserved. The default ISO also passed the one-CPU
borrowed-boot-backing/write/resize/exact-renderer/memory/compiler check.

The Python build helper and both Makefiles default to LLInt. Use
`WEBCORE_INTERPRETER=cloop` or explicit `--interpreter cloop` for an intentional
comparison. The userland renderer target rechecks the selected incremental
cache even when a previous trial installed a newer binary. Never mix backend
ABIs or run simultaneous configure/Ninja jobs. Restoring older source with its
older timestamp initially left a diagnostic object in the cache; touching the
owned restored source and rebuilding produced the exact validated v38 hash.
The installed renderer and default ISO contain no temporary DOM probes.

Validated v38 on **one and four CPUs** using separate immutable candidate ISOs:

- Complete native pixels/input/forms/POST/history/reload/cookies/UTF-16
  storage/events/quota/download/resize/graceful-close suites. Retained scrolling
  equals full repaint for dense/nested/sticky/rounded/filtered content and
  scrollbars. Small/dense/busy TCG scrolling was .18–.20/.40–.41/.21 seconds,
  including screenshot overhead. Prepared output drains during a real five-second
  JS task; this does not make that DOM task preemptible.
- Scheduling, actual frame/microtask/custom-element/MutationObserver ordering,
  genuine idle deadlines/continuation/timeouts/cancellation in the controlled
  helper. Native UI retains upstream's disabled experimental idle API default.
- New selected-interpreter allocation/full-GC regression in a real DOM realm:
  192,000 temporary objects with typed arrays, three actual full collections,
  live closures/private fields/detached DOM wrapper/Map/BigInt/DataView and ICU.
  No GC changes or test hooks are exposed in the native view.
- Real HTTP/TLS/CORS/preflight/edit/wheel/forms and zero-font checks on two origins.
- Borrowed boot backing/write/resize/exact ELF, real memory/child accounting and
  compiler ABI; sandbox/IPC/memfd/seals/FD/pipe/quota/immutable-origin/main and
  worker confinement/scoped signal/child-cleanup suites.
- Shared surface code is unchanged; preserved v30/v31 ASan/UBSan passes still
  apply. Standalone JSC component checks remain separate from the native LLInt
  renderer and do not establish public-site compatibility.

Pass logs: `build/modern-browser/v38-uefi-{1,4}cpu-*-pass.log` and
`v38-default-uefi-1cpu-boot-files-pass.log`.
`v38-validation-results.json` records observed run outcomes; controlled
screenshots are in `v38-controlled-evidence/` and the default guest's actual
RAMFS memory/ELF export is in `v38-default-guest-boot-export/`.
`tools/benchmark_browser_interpreter.py` measures a checked 2,000,000-iteration
Math.imul workload through the actual native Browser. One-CPU TCG median of
three samples was 1906 ms in v37 and 1273 ms in v38: about 1.50× throughput,
33% less time **for this workload only**. JSON/logs/screenshots are preserved in
`interpreter-benchmark-v37-v38/`.

Uninstrumented public captures wait at least 40 seconds and verify detailed
patches translated the requested 400 pixels. Readable titles/HTTP 200/tiny
animations alone are insufficient. Evidence:

- `public-site-v38/` (four-CPU TCG): Google `osdev` and submitted `limine uefi`
  results render and truly scroll (.189/.195 s first body response). Fresh direct
  YouTube results/thumbnails scroll (.776 s) but retain placeholder headers;
  home → actual search has full logo/header/sidebar/icons and scrolls (.742 s).
- `public-site-v38-kvm/` (four-CPU hardware virtualization): fresh direct YouTube
  shows full actual logo/header/search/sidebar/icons/results and truly scrolls
  (.219 s); home → search also works (.501 s). Google queries render and truly
  scroll (.180/.192 s). These are genuine guest-rendered pixels.
- `public-site-v37-kvm-comparison/`: v37 also fills the fresh YouTube header on
  KVM and truly scrolls (.509 s). Thus the KVM initialization result cannot be
  attributed solely to LLInt. Do not call the remaining TCG initialization fixed.
- OSDev still returns an actual Cloudflare HTTP 403 challenge on both TCG and
  KVM. Guest console records failed posts to challenges.cloudflare.com because
  the denied foreign iframe remains at the parent's origin. Separate frame
  routing is required; successful routing alone would not guarantee server
  verification. YouTube home has no populated feed in these signed-out captures.

Read-only DOM probes were confined to **diagnostic** v39/v40/v41 ISOs and source
snapshots, all preserved and clearly named. `public-site-v{39,40,41}-diagnostic/`
and `youtube-initialization-investigation/` contain logs/snapshots and exact
first-party scripts fetched with certificate verification for inspection.
Fresh TCG pages have actual topbar data and attached custom elements; the
priority-1 buttons job stays scheduled, with active icons lacking SVG children.
YouTube's real scheduler drains priorities 5/4 before lower tasks under an
approximately 13.7-ms wall-clock budget. Lower-priority starvation from expensive
work is a hypothesis, not a proven complete root cause. No site callbacks,
deadlines, priorities or DOM were fabricated or changed.

Public helper now accepts `--label` and `--accel tcg|kvm` to preserve independent
comparisons. A fast-guest OPEN/READY read race was fixed by checking the actual
READY record for the exact new window. Original startup-timeout and missing-import
failures are preserved in `public-site-v38-kvm-startup-failure/` and
`public-site-v38-kvm-helper-import-failure/`; subsequent runs exited successfully.
Initial docs/source/logs are in `continuation-baseline-20261011T190855Z/`.

NEXT WORK: profile actual task/queue timing in a separate candidate to establish
the remaining fresh TCG YouTube initialization cause. Preserve the actual
clock/input/scheduling semantics. Foreign frames need separate immutable-origin
processes plus real RemoteFrame/window/postMessage/resource/surface/input
routing and confinement/ordering/cancellation tests. The current native
FrameLoaderClient and BrokerLoader deny foreign documents; widening either
allowlist is not an implementation of isolated frames.

No media decoder/audio/MSE, foreign frame routing, IndexedDB/service workers,
WebGL/WebGPU/Wasm, clipboard backend or JIT exists. Individual running JS tasks
remain nonpreemptible. Google/search rendering passes do not imply video playback
or perfect compatibility. All owned build/test jobs are stopped; no subagents
or skills were used. Continue from selected v38, avoiding overwrites of ISOs in
use. Packaging is already complete; do not restore old defaults or diagnostics.

## Previous native-browser checkpoint — v37 before LLInt validation

Historical snapshot; the ACTIVE v38 section above supersedes this state.

This section supersedes older checkpoints. The user wants Google searches,
OSDev and YouTube fixed in the real x86-64 Limine UEFI OS, including layout,
errors and slow scrolling. Work remains active; do not claim perfection,
working video or completion of OSDev's verification. Read browser-fixes.md
for the current fixes, measurements and public evidence.

The current installed renderer and exact candidate ISO are v37:
`build/modern-browser/native-browser-v37.iso`. It embeds real static
WebCore/JSC 2.54.0, CPU Skia, restricted Soup resources and immutable-origin
renderers. Local files retain NetSurf/grants. No fabricated page/API, TLS
bypass, disabled GC, relaxed origins or host-browser UI was introduced.

Validated v37 on one and four CPUs:

- Complete native pixel/input/forms/history/cookies/storage/download/resize/
  graceful-close suites, including retained vs full-repaint equality for dense,
  nested/sticky/rounded/filtered content and persistent scrollbars.
- Genuine frame/microtask scheduling; controlled experimental idle API checks
  for real deadlines, microtasks between tasks, continuation after an 80-ms
  callback, runnable timeouts under busy work and cancellation exactly once.
- Native UI uses the upstream disabled requestIdleCallback default. A/B public
  captures show enabling the experimental API delays sidebar initialization.
- Actual WTF loop observers, paced rendering, 4-ms dispatch budget/pending-input
  yielding, and native scroll during genuine continuous background tasks.
  Small/dense/busy scrolling approximately .20–.21/.41–.42/.21 seconds.
- Real HTTP/TLS HTML/CSS/JS/fetch/images/forms and CORS/preflight on two origins.
- Borrowed immutable boot backing/write/resize semantics, exact renderer ELF,
  real memory/child accounting and compiler ABI.
- ASan/UBSan shared copy/queue/commit/cancel through v30/v31 and legacy browser
  through v31 remain valid; sandbox/IPC through v23 remains preserved.

Preserved evidence: `build/modern-browser/v37-uefi-{1,4}cpu-*-pass.log`,
`public-site-v37/` (uninstrumented, minimum 40-second observation), all older
passing/failing logs, screenshots, sources and ISOs. Google search and submitted
query render and truly scroll (~.19 s). Fresh direct YouTube results/thumbnails
truly scroll (~.52 s) but header remains placeholder. Home → real page search
has genuine logo/search/sidebar icons and scrolls (~1.30 s). The diagnostic
requires 400-pixel detailed-patch translation; tiny animation or HTTP 200 is
not a scroll/rendering pass. OSDev returns a genuine Cloudflare 403 challenge;
its foreign iframe is denied without separate origin/frame process routing.

The embedder now cycles RunLoop::cycle with owned GLib context, delivers
activity observers, caps successive dispatches at 4 ms, stops for pending native
input, paces real rendering updates and calls didUpdateRendering. IdleCallback
source patches use one callback per actual idle task, real idle deadlines,
pending/runnable timeout removal and wakeups after expired budgets. Production
UI leaves that experimental API disabled; controlled helper explicitly enables
it for regression checks. No temporary DOM/abort/RenderImage/wheel diagnostics
remain in v37. Sources use pinned, checked build_webcore.py transformations.

HANDOFF STATE: all owned test VMs are closed and no build is running. The user
asked for a continuation prompt. The isolated x86-64 LLInt trial was interrupted
normally at 3756/3839 remaining-build steps to leave a clean handoff; its completed
objects/cache are preserved. This was not a compile failure. Resume via
`python3 -u tools/build_webcore.py --interpreter llint --target ModernOSRenderer
--jobs 20`. Cache is `build/modern-browser/webcore-llint`; JIT/DFG/FTL and Wasm
remain off. Existing `webcore` C_LOOP cache and v37 ISO remain intact. This new
backend must receive full native/scheduling/network/GC/memory/capability checks
and uninstrumented public comparison before being selected. Build helper now
has explicit --interpreter cloop/llint (default currently cloop). Do not mix
LLInt JSC objects with C_LOOP WebCore objects. No concurrent Ninja/configure.
A controlled parser/custom-element/MutationObserver notification-order fixture
passed on one CPU against v37 (0.24-s busy scroll); the pass log is
`v37-uefi-1cpu-custom-scheduling-pass.log`. This rules out the simple premature
microtask-notification hypothesis but does not fix fresh YouTube startup.
The interruption log is `webcore-llint-build-handoff-progress.log`. LLInt has no
linked renderer yet and has not been validated. The installed renderer still
matches the validated C_LOOP build, SHA-256
`9609b18b6b7f4a60c176cc94d022cc9c26864cb4810c5dd010498f8f44fc0ac8`.
Its source is `build/modern-browser/webcore/bin/web-renderer`; do not silently
replace a selected LLInt renderer with the build helper's current cloop default.

`build/os.iso` STILL predates the latest validated fixes (09:39 vs v37 11:40). After selecting
and validating a final candidate, preserve the old default, atomically package
the exact candidate into build/os.iso, compare hashes and verify boot bytes.
Refresh this section, AI-RESUME-PROMPT.md, browser-fixes.md and modern-browser-port.md.
Do not leave the shipped/default ISO old or include diagnostic code in it.

No media decoder/audio/MSE, foreign frame routing, IndexedDB/service workers,
WebGL/Wasm/clipboard or JIT exists. Do not bypass Cloudflare or fake API success.
An individual running JS task remains nonpreemptible on the DOM thread.
No subagents or skills were used. Preserve the dirty/untracked OS and caches,
licenses, user files and all failed/passing evidence; no git reset/clean or
physical Linux disk changes. Follow current approval rules for local listeners
and public QEMU networking. Avoid wrappers with redirected public test output
that caused long approval waits; invoke the approved python script directly and
preserve its actual guest log afterward.

## Previous native-browser checkpoint — v25/v26

This section supersedes every historical checkpoint below. The current user
asked to fix Google searches, OSDev and YouTube in the actual Limine UEFI OS,
including broken layout, errors and very slow scrolling. Work is ongoing;
never report complete compatibility or video playback from these checks.

The native Browser embeds real static WebCore/JavaScriptCore 2.54.0, CPU Skia
and a trusted restricted Soup broker. Top-level origin changes replace the
immutable-origin renderer. Local file associations retain NetSurf/grants.
There is no host-browser UI, changed site response, disabled GC, TLS bypass or
relaxed renderer-origin policy. No subagents or skills were used.

Validated changes through `native-browser-v25.iso`:

- Actual render-update scheduling, DOM/CSS/canvas/PNG and native input. GUI
  toolbar modifier state and broker modifier replay fix Ctrl+A immediately
  after an origin change. First-gesture selection/backspace has a regression.
- Real WebCore history trees, DOM pushState/back/popstate and scroll restore;
  bounded broker snapshots retain original POST method/body. Back restores
  without resubmitting; explicit reload performs the real POST again.
- Soup cookies/HttpOnly/credential rules, origin-confined binary UTF-16 SQLite
  local storage with the real unix-dotfile VFS, bounded window-local session
  storage and source-excluding same-origin storage events.
- Streamed downloads, no-replace publication, safe names, part cleanup and
  child cleanup. Legacy shutdown prioritizes Quit and waits a bounded two
  seconds to save cookies; normal checks prohibit forced close.
- RAMFS borrows immutable Limine initramfs bytes until modification, avoiding
  a second ~726-MiB copy. Write/truncate/zero-fill and exact ELF export pass.
- Actual process mapped/peak/limit counters, peak RSS/reaped-child accounting
  and WebCore's real memory pressure backend. Native quota is 768 MiB; broker
  and legacy quota 512 MiB. JSC's genuine StructureID reservation is 64 MiB,
  configured for this eagerly backed VM; GC remains enabled.
- Read-only self prlimit64 fixes musl stack discovery. Prior EPERM prevented
  musl's getrlimit fallback and left WebKit's bound uninitialized, producing
  the invalid main-stack-to-heap-environ conservative GC range. Foreign limit
  queries and limit writes remain denied; real main/worker checks cover it.
- Self raise/tkill now resolve and check the target TGID under the scheduler
  lock. Foreign authority remains confined to a broker's live own-domain
  renderer children with SIGKILL; broker death cleans those children.
- Zero-sized fallback text no longer collides with FontCache's empty key.
  The sentinel is -1, leaving genuine checks intact. An actual empty typeface
  at zero size is tested in the real cache. Generic aliases use installed
  DejaVu files; canvas font resolution retains valid zero-sized requests.
- The CPU ScrollAnimator override consults the real page preference. Disabling
  wheel animation now actually removes intermediate frames. Scroll copies
  reuse only regions whose actual new pixels match translated old pixels;
  source/destination bounds, overlap and canaries have ASan/UBSan checks.
  Fixed and independently changed content still receives normal damage.
- F_GETPIPE_SZ/F_SETPIPE_SZ provide real 4–256-KiB capacities, wrapped-data
  preservation, failed-shrink/backpressure/atomic-write behavior. Two browser
  pixel pipes use 256 KiB; page renderers cannot change kernel pipe capacity.
- BROWSER_PRESENT follows all copy/damage packets in a genuine paint. Public
  diagnostics observe completed paints and actual body pixels. A separate
  writer thread transfers immutable prepared output while DOM/JSC stay on
  the main thread. A five-second real JavaScript task reproduces the old v24
  partial-frame stall; the v25 regression completes during the busy task.

Preserved evidence under `build/modern-browser/`:

- `v25-uefi-{1,4}cpu-native-webkit-pass.log`: complete native suite, dense fixed
  overlays, generic/zero fonts, JPEG/WebP/decode/fades, busy-JS output, native
  input/history/form/storage/download/resize/close. The final 4-CPU repeat
  also checks real IntersectionObserver bounds/visibility.
- `v24-uefi-{1,4}cpu-webcore-network-pass.log`: HTTP/TLS/CORS/preflight and actual
  pixels/click/edit/wheel/form, plus the real zero-sized empty-typeface cache.
- `v23-uefi-{1,4}cpu-browser-ipc-pass.log`: genuine IPC/JSC/GLib, self signals,
  limits, malicious main/worker denials, bounded pipes and cleanup.
- `v25-uefi-{1,4}cpu-boot-files-pass.log`: borrowed backing, actual writes,
  zero-fill/resize, exact current ELF, memory and child accounting.
- `v25-uefi-4cpu-browser-pass.log`: local file associations, NetSurf cookies,
  normal web pages/downloads/TLS errors/workspaces and graceful child cleanup.
- `v24-uefi-4cpu-output-stall-regression-failure.log`: prior renderer stalls
  prepared pixels during the real five-second JSC task. v25 passes the same
  probe and the permanent full native regression.
- `public-site-v25/`: real Google results and search/scroll, YouTube home and
  exact `limine uefi` search, OSDev 403 challenge. Google complete paint appears
  around 0.25 seconds on the initial result scroll under TCG; this is a
  diagnostic with screenshot overhead, not a hardware benchmark.
- Old failed ISOs/logs/screenshots, including GC and font-cache aborts, remain
  preserved. v18 renderer symbols identify the genuine FontCache abort;
  v19 symbols and temporary abort diagnostic source are saved. Abort wrapping
  and verbose wheel instrumentation have been removed from owned sources.

The v25 native fixtures measured about 0.19–0.20 seconds for four small-page
wheel ticks and 0.38–0.62 seconds for the maximized dense page under concurrent
QEMU TCG tests. These include screenshot overhead. Long JavaScript still runs
on one CPU/interpreter; a transport thread does not supply JIT/compositing.

Current investigation: YouTube search text now appears and scrolls, but actual
public thumbnails remain missing despite successful JPEG/WebP/fade/observer
fixtures. `native-browser-v26.iso` adds temporary periodic RenderImage bounds,
cache/completion/error and ancestor-opacity diagnostics to identify the real
cause. Run/inspect YouTube home plus a fresh direct search, then remove these
periodic diagnostics after fixing the cause. Preserve each public revision.
OSDev returns a genuine Cloudflare HTTP 403 verification page; its foreign
verification frame is denied by the immutable-origin policy. Separate foreign
frame routing remains unimplemented; do not bypass the verification or widen
one renderer to arbitrary origins.

`build/os.iso` STILL PREDATES the native integration and must be refreshed from
an actually validated final candidate. All v5–v26 test images and build caches
are retained. Never run concurrent WebCore CMake/Ninja jobs or replace an ISO
that a VM is using. Follow the current permission policy for local listeners
and public network/QEMU. Check owned live sessions before resuming, and never
kill unrelated processes. No physical Linux partition was touched.

Remaining browser capabilities include genuine video/audio decoding/output,
MSE, cross-origin frame/remote-window/message routing, IndexedDB, service
workers, WebGL/WebGPU, Wasm, clipboard and JIT. Continue actual public rendering
investigation, finish meaningful regressions for new fixes, remove temporary
instrumentation, package default ISO and keep current evidence/docs accurate.

## Latest continuation: genuine WebCore dependencies and software port

2026-10-10 work resumes from the tested IPC closing checkpoint below. The
desktop still uses NetSurf/Duktape. Modern public-site rendering, native
WebCore page integration and real page/frame isolation remain UNFINISHED.
Do not interpret dependency checks as browser support.

### Fresh-chat closing checkpoint: compiled update, guest failure unresolved

The user-visible desktop Browser is still NetSurf. This continuation did not
change the native desktop/browser source or connect WebCore to its window.
The new engine has produced verified controlled guest pixels, but the requested
ordinary browsing experience has not improved yet. Prioritize getting the
existing real engine into the native browser once its input/lifecycle failure
is resolved; do not restart dependency/component work to demonstrate progress.

- The earlier controlled rendering/click and brokered HTTP/HTTPS/CORS pixel
  checks passed on 1 and 4 CPUs. Their preserved logs are listed below. Those
  passes describe the checkpoint **before** the latest input/lifecycle edits,
  not a passing result for the current rebuilt binary.
- The most recent extended 4-CPU run passed its typing/backspace/value/title
  assertions and third-frame input marker pixels, then failed wheel scrolling:
  `scrollY=0`, document height `1401`, visible height `240`. Form submission
  was not reached. Preserve `webcore-network-wheel-failure.log` and
  `webcore-network-wheel-update-failure.log`; do not weaken the assertion.
- `EditorModernOS.h` uses real WebCore editing/undo and upstream WPE keyboard
  commands. Unimplemented clipboard and persistent storage services remain
  denied/disabled; they are not implemented by empty success providers.
- The empty chrome client dropped rendering-update requests. The new
  `ChromeModernOS.h` records real view invalidation and scheduling;
  `renderer-fixture.cpp::tick()` now calls actual `Page::updateRendering()`
  and `Page::finalizeRenderingUpdate({})`. This is a candidate fix, **not a
  verified resolution** of the wheel failure.
- The final incremental build exited 0 and linked/copied `web-renderer`.
  `webcore-driver.log` and `webcore-build.log` record success. Installed binary
  SHA256: `da579d9861ad6d59957c05cd45721e6ced6e0f9fbf3003e34e6895624f0cae33`.
  It has **not** been packaged or guest-tested after this chrome/lifecycle
  change. `webcore-network.iso` still contains the earlier failing build;
  `build/os.iso` remains the original tested closing ISO.
- The owned build/test jobs have finished and the test VM is closed. Still
  inspect processes before packaging; never overwrite an ISO in use or kill
  unrelated jobs. No implementation work continued after the user requested
  this fresh-chat handoff.

Next concrete action: package the current binaries into the separate
`build/modern-browser/webcore-network.iso`, then run
`python3 -u tools/test_webcore_network.py --cpus 4 --iso build/modern-browser/webcore-network.iso`.
The disposable loopback HTTP/TLS fixture requires the environment's permitted
listener/QEMU access. If it passes, check 1 CPU; if it fails, preserve evidence
and fix the actual rendering/scroll lifecycle. Then continue native surface and
input integration, trusted navigation and origin changes, lifecycle cleanup,
cookie/storage services and genuine public-site tests. No native frontend,
general form/navigation or requested public-site success is demonstrated yet.

- `tools/build_webcore_deps.py` builds genuine static HarfBuzz 11.2.1,
  Fontconfig 2.16.0, WebP 1.6.0, libjpeg-turbo 3.2.0, libxml2 2.14.6,
  SQLite 3.54.0, libgpg-error 1.61, libgcrypt 1.12.4, libtasn1 4.20.0,
  libpsl 0.21.5, nghttp2 1.65.0, glib-networking 2.80.1 and Soup 3.8.0.
  Archives stay in `userland/vendor`; new network source URLs/hashes are
  pinned in `userland/modern-browser/dependency-sources.json`. The three
  old exploratory font/WebP downloads are now built. New dependencies use
  separate `build/modern-browser/*-target` caches and the existing modern
  prefix/POSIX C++ runtime. The NetSurf JPEG/dependency prefix and SDK were
  preserved. AVX/SIMD dispatch and GPU requirements are disabled through
  genuine library build options, rather than fake GL entry points.
- glib-networking's upstream OpenSSL backend is built as a static module
  archive instead of a shared module. The Meson patch changes packaging,
  not certificate/hostname validation. Link the real `libgioopenssl.a`
  **and** `glib-networking-2.80.1-target/tls/base/libtlsbase.a`, and call
  upstream `g_io_openssl_load(NULL)` before selecting the Gio TLS backend.
  Soup's host-executed TLS configure probe is disabled for cross compilation;
  actual certificate acceptance/rejection has now been checked in guests.
- `webcore-deps-check` passed in 1- and 4-CPU UEFI guests: real Fontconfig
  matching, HarfBuzz ligatures and Arabic RTL, FreeType raster ink, exact WebP
  lossless pixels, libjpeg-turbo BGRA/crop/skip decode, XML entity parsing,
  SQLite transaction rollback, public suffix lookup and SHA256/AES-GCM vectors.
- `web-soup-check` passed in 1- and 4-CPU guests under the actual broker role:
  complete 190123-byte HTTP/chunked/redirect bodies, explicit 503, default
  rejection of the fixture CA, exact verified HTTPS body with explicitly
  installed fixture trust, wrong-host rejection and untrusted rejection.
  Private `/tmp` writes remain denied. This test is not a production page
  resource broker and does not add renderer network authority.
- Separate test artifact: `build/modern-browser/webcore-deps.iso`.
  `build/os.iso` remains the previous tested closing ISO. No kernel or desktop
  source changed in this continuation so far. Guest logs:
  `build/runtime-tests/uefi-{1,4}cpu-webcore-deps.log`; the 4-CPU successful
  font/image/data output is preserved in
  `build/modern-browser/webcore-deps-first-http-failure.log` because its first
  Soup request preceded DHCP and failed honestly with ENETUNREACH. The test
  now waits for DHCP as the existing network tests do. Soup retry evidence:
  `build/modern-browser/webcore-deps-soup-guest.log`; 1-CPU full evidence:
  `build/modern-browser/webcore-deps-1cpu-guest.log`.
- `tools/build_webcore.py` configures genuine WPE WebKit 2.54.0 with owned
  `userland/modern-browser/port/{OptionsModernOS.cmake,PlatformModernOS.cmake,
  SourcesModernOS.txt}`. It extracts a **separate** source tree under
  `build/modern-browser/webcore-source/wpewebkit-2.54.0`, preserving the tested
  JSC source tree. `build/modern-browser/webcore` retains all incremental
  objects. The software Skia port excludes GPU/Ganesh and AVX objects, retains
  real CPU raster/shaders/filters/encoders, and uses the genuine GLib loop.
  Checked patches retain original upstream sources under `webcore-original`.
  WebCore, PAL, JSC, WTF and CPU Skia now build, and the genuine static
  `userland/bin/web-renderer` links (~129 MiB file / 107 MiB load segments).
  It has no dynamic interpreter or GPU/JIT requirement. This development
  renderer is not yet the desktop browser; no public-site success is claimed.
  Initial compilation exposed the real IJG/libjpeg-turbo mismatch; the new
  genuine target libjpeg-turbo fixed it. Read `webcore-build.log` for current
  progress/failures before continuing; do not discard caches or restart SDK.
- Controlled page fixture sources now exist in `userland/modern-browser/port/`
  (`renderer-fixture.cpp`, `FixtureStrategies.h`, `Fixture.cmake`). They load
  real HTML through FrameLoader, paint Grid/canvas/text via Skia and send a
  mouse click through EventHandler. `userland/webcore-render-check.c` validates
  bounded 40-byte packets and exact pixels in two immutable-origin renderer
  roles launched by a threaded broker. **The guest pixel/click test passes on
  1 and 4 CPUs**: exact Grid colors, actual canvas pixels, shaped text ink,
  promises/DOM, mouse dispatch and changed marker pixels, distinct immutable
  origins and direct-network/CA/private-file denial. Logs are
  `build/runtime-tests/uefi-{1,4}cpu-webcore-render.log` and
  `build/modern-browser/webcore-render-{1,4}cpu-guest.log`. The first 4-CPU run
  faulted because the embedder activated focus before `frame->init()`; moving
  activation after frame/view initialization fixed it. Its original evidence
  is `webcore-render-first-focus-failure.log`. Do not repeat that fixed work.
  `make webcore-fixture` / `make test-webcore-render` are development targets;
  the latter uses a separate `webcore-render.iso`. Fixture LoaderStrategy
  schedules genuine data-URL loads and explicitly refuses HTTP(S), sync
  network, ping and preconnect; it is not a production resource service.
  Clipboard, storage, frames/history/native UI and keyboard/forms/scrolling
  need genuine implementation and verification before browser claims.
  Additional preserved compile diagnostics are the accessibility wrapper,
  disabled-video binding, BitReader include, raster source rectangle and
  XDGMIME configuration failure logs under `build/modern-browser`. Corrected
  port code reuses the real generic native accessibility object model, adds
  the missing feature guard, correct headers, source geometry and bundled MIME
  target. Native accessibility integration is not implemented. Current modifier
  state and KeyDown splitting use the platform event data/upstream logic;
  bounded typing/backspace assertions now pass before the unresolved wheel
  failure above. General native keyboard/IME integration remains unfinished.
  Additional link fixes explicitly reject unavailable default graphics-layer
  compositing and dispatch actual custom factories; they do not create fake
  layers. Two unprefixed private xdgmime symbols were namespaced to coexist
  with Gio's real static copy. Renderer include/link diagnostics are preserved
  in `webcore-fixture-include-failure.log`,
  `webcore-graphics-layer-link-failure.log` and
  `webcore-xdgmime-static-link-failure.log`. `webcore-NOTICES.txt` preserves
  WebCore/PAL/Skia/xdgmime notices and LGPL text in the initramfs.
- The additional `--network-check` renderer mode now loads actual main HTML
  through FrameLoader and `BrokerLoader.h`/SubresourceLoader, with real
  asynchronous Soup delivery in `userland/modern-browser/web-broker.c`.
  Resource metadata uses a separate versioned 64-byte seqpacket protocol
  (`web_resource_protocol.h`), bounded to 32 KiB; nonempty bodies are at most
  16 MiB and passed as sealed memfds with validated descriptor/size/seals.
  Up to eight requests run concurrently; cancellation, deferral, genuine
  response headers/status and errors are routed to the real loader. WebCore's
  CSP/CORS/preflight/redirect checks remain enabled. Main documents require the
  renderer's immutable origin; other origins need a new renderer. Files/blobs
  in upload FormData and synchronous network requests remain unsupported.
  There is no broker cookie service or persisted web storage yet.
  `tools/test_webcore_network.py` passed HTTP and verified HTTPS page pixels,
  external CSS/script/PNG, promises/fetch redirect, mouse click, allowed CORS,
  denied cross-origin response access and a real preflight/POST on 1/4 CPUs.
  Evidence before the input extension is preserved in
  `webcore-network-http-click-{1,4}cpu.log`; main test logs are
  `build/runtime-tests/uefi-{1,4}cpu-webcore-network.log`.
  The initial inherited fixture redirect denial failed honestly; its log is
  `webcore-network-first-redirect-failure.log`. That policy now delegates
  actual HTTP redirects while rejecting unsupported/cross-origin documents.
  `make test-webcore-network` uses separate `webcore-network.iso` and disposable
  loopback servers. Explicit fixture trust is private and temporary; normal
  broker trust uses `/etc/ssl/cert.pem`, without disabling TLS checks.
  The extended keyboard/wheel/form test is failing at wheel scrolling; its
  typing/backspace portion passed and form submission was not reached. The
  closing chrome/lifecycle fix compiled but has not been guest-tested. Native
  frontend/history/frame routing and requested public-site compatibility are
  still unfinished. Resume at the fresh-chat checkpoint above.
- `make webcore-deps`, `make webcore-checks`, `make webcore` and
  `make test-webcore-deps` are incremental/offline targets using the existing
  modern prerequisites. `test-webcore-deps` packages its separate ISO.
  Never package an ISO while a VM is using that same image.
- Actual pre-edit dirty sources/docs were saved once under
  `build/modern-browser/webcore-original`. Older `original`, `ipc-original`,
  compiler caches and baseline artifacts were retained. No physical disk,
  host system installation, repository reset/clean or sandbox widening occurred.

## Previous tested continuation: WebKit IPC, capabilities and GLib/Gio

Closing checkpoint: the user asked to stop here because credits were running
low. The final ISO and guest checks described below are complete. No WebCore or
graphics dependency compilation is running. A copyable continuation prompt is
in [AI-RESUME-PROMPT.md](AI-RESUME-PROMPT.md).

The next 2026-10-09 continuation implements actual Unix descriptor IPC, coherent
memory objects, event-loop support, explicit broker/renderer capabilities and a
genuine static GLib/GObject/Gio dependency layer. **WebCore/DOM/CSS rendering,
modern native browser integration and desktop per-site page isolation remain
UNFINISHED.** NetSurf/Duktape remains the desktop engine. No modern public-site
rendering success is claimed. Read [browser-ipc.md](browser-ipc.md) for the exact
API, origin/resource policy, bounds and unsupported operations.

Concrete implementation:

- AF_UNIX socketpair stream/datagram/seqpacket transport, vectors, nonblocking
  backpressure, real SCM_RIGHTS references, CLOEXEC, peek/truncation, shutdown,
  poll and descriptor lifetime. Queues release discarded rights. Cyclic socket
  reference graphs and epoll transfers are rejected instead of leaking them.
- memfd_create, resizing and file I/O, real MAP_SHARED physical aliases across
  processes/CPUs, MAP_PRIVATE snapshots, object/VM lifetime and write/size seals.
  Shared PTEs survive descriptor close and private fork, and are reclaimed at
  unmap/VM teardown. Mapped shrink returns EBUSY; general file coherence,
  executable/shared anonymous mappings, shared mremap and shared futex aliases
  remain unsupported. Renderer-created backing has a separate 128-MiB group
  quota; other groups/global backing have 512-MiB caps, plus the existing
  512-MiB mapped-page quota for each browser profile.
- Bounded epoll create/ctl/wait/pwait with level/one-shot readiness, timeouts,
  hangups, temporary signal masks and interruption. Watches follow real file
  descriptions through dup/close/reuse and disappear on final external close.
  Edge/nested/exclusive/wakeup modes are explicitly unsupported.
- spawn_web(409) creates a fresh static ELF/VM/context while broker threads are
  live. A trusted unsandboxed parent may create a broker; only that role may
  create restricted renderers. Exactly three explicit endpoints and sanitized
  environment are inherited. Ordinary threaded fork/exec/spawn remain rejected.
  web_identity(410) reports immutable origin/role/TGID/broker domain. Threads
  inherit their sandbox. Filtering precedes the threaded-operation guard, so
  forbidden renderer calls retain the default-deny EPERM result.
- Renderer roles deny direct IPv4, private/local/download/CA files, launches,
  foreign signals, desktop/input/mount controls and excess memory. Broker roles
  permit outgoing network, CA/resolver files and private WebKit state/downloads.
  Fresh renderer instances have canonical HTTP(S) origins; resources/channels/
  counters are bound to their creator or first renderer recipient. Cross-origin
  descriptor grants and forged network/CA transfers are denied. The legacy
  NetSurf profile did NOT gain clone, memfd, Unix IPC or epoll permissions.
- Genuine GLib 2.86.5, GObject/Gio, libffi 3.5.2 and PCRE2 10.46 now build against
  musl in the separate modern-browser prefix. Real eventfd/poll/epoll/pthreads
  drive their event loops. PCRE2 JIT and libffi Linux static executable trampolines
  are disabled. Generic libffi signal calls, Unicode regex and real async tasks
  are exercised; Gio dynamic modules/TLS backends are not ported.

Owned new files:

- net/unix/unix_socket.c and fs/{memfd,epoll}.c and corresponding include/fs headers.
- userland/{ipc-check,web-capability-check}.c, include/web_process.h and
  modern-browser/glib-check.c.
- tools/{build_webkit_deps,test_webkit_deps,test_browser_ipc}.py and
  Documentation/browser-ipc.md.

Updated existing files include kernel include/{sched.h,mm/user.h,uapi/abi.h,
fs/vfs.h,sandbox.h}, mm/user.c, fs/vfs.c, net/network.c,
sched/{elf,process,syscall,sandbox}.c, Makefiles, vendor README/licenses and the
browser/kernel docs. Inputs to this continuation were saved without overwriting
prior backups in build/modern-browser/ipc-original/<path>. The old thread
continuation's original/ and drag baseline logs remain intact.

Build commands: make webkit-deps, make test-webkit-deps, make test-browser-ipc.
The test targets finish component builds before ISO packaging and guest boots.
A normal make iso packages existing glib-check/jsc binaries without rebuilding
these components. Preserve build/modern-browser/{prefix,glib-target,
libffi-3.5.2-target,pcre2-10.46-target,host-tools,downloads}, extracted sources,
stamps and the separate POSIX C++ runtime. No SDK was overwritten or clean/reset
performed. Pinned archives are in userland/vendor; exact primary URLs/hashes and
host-only Meson 1.10.2 wheel pin are in userland/vendor/README.md. Builders work
offline from these retained sources/tools. No host system package installation
or physical partition access occurred.

Guest verification on 1 and 4 CPUs:

- ipc-check: coherent aliases/physical writes across forked processes, private
  copies, growth/zero-fill/seals and final lifetime; stream/datagram/seqpacket
  records/vectors/peek/truncation/backpressure/shutdown; SCM rights/CLOEXEC,
  invalid/cyclic rights, 600 queued-close lifetimes and blocked close/reuse;
  epoll level/one-shot, dup/close/reuse/hangup, deadlines and interrupted pwait
  with restored masks. PASS.
- web-capability-check: malicious main/worker confinement, quota allocation and
  reclamation, sanitized FD/environment inheritance, invalid origins/helper paths,
  concurrent immutable origins/private VMs, foreign-signal and cross-origin
  memory/channel/counter grant denial, including renderer-created resources.
  A live threaded broker launches genuine JSC/agents AND Gio under renderer
  capabilities. PASS. These are controlled fixtures, NOT webpages/Web Workers.
- glib-check: real GLib thread/TLS/main-context wakeup, generic GObject libffi
  marshalling, PCRE2 Unicode regex, actual Gio Unix rights/shared data and epoll
  stream pollability, real asynchronous completion. PASS in development CLI
  guests and under renderer capabilities.
- thread-check, legacy browser-sandbox-check, and general --no-storage runtime
  regressions: PASS on 1 and 4 CPUs for the final artifact.
- Native Browser regression: PASS on 4 CPUs, including actual CSS/PNG pixels,
  mouse/keyboard/form input, history/reload, safe downloads, validated HTTPS,
  certificate/hostname rejection, resize/workspaces and cleanup. The current
  browser-form-input.png was inspected. This remains a NetSurf fixture check.
- Network/Git regressions: PASS on 4 CPUs, including bodies/chunks/redirects,
  certificate trust/hostname rejection and disposable local smart HTTP/HTTPS
  clone/fetch/push. Offline loopback/error behavior also passes.

Logs: build/modern-browser/{ipc-iso-build,ipc-guest-tests,glib-guest-tests,
webkit-deps-driver,glib-check-build,glib-target-configure,glib-target-build,
ipc-thread-regression,ipc-sandbox-regression,ipc-runtime-regression,
ipc-browser-regression,ipc-network-regression}.log; guest logs under
build/runtime-tests/uefi-{1,4}cpu-{browser-ipc,glib}.log. Initial fixture failures
were corrected (missing fixture HOME, filtering order, an attempted simultaneous
cyclic-rights test); no isolation/guest assertions were weakened to make them pass.

Next engine work remains substantial: build target HarfBuzz/Skia/Soup/TLS and
remaining WPE libraries, then WebCore/DOM/CSS; integrate software surfaces/input,
real resource broker/network/TLS/cookie/download/local-grant services and native
helper launching, navigate/route frames by origin and test actual modern webpage
pixels/interactions and public sites. Kernel roles/IPC and JSC/Gio component tests
are not completion of that work. The original Google/OSDev verification and
Yahoo/Bing rendering/script failures below have not been resolved or newly
re-diagnosed with a modern renderer. The 2560×1600 rapid-drag failure and saved
pre-change-kernel comparison are untouched; resolution and tests were not changed.

Immediately before closing, upstream rendering dependencies were investigated.
Three source archives were downloaded into build/modern-browser/downloads only;
they have NOT been extracted, built, added to the dependency builder or packaged:

- harfbuzz-11.2.1.tar.xz, SHA-256
  093714c8548a285094685f0bdc999e202d666b59eeb3df2ff921ab68b8336a49,
  https://github.com/harfbuzz/harfbuzz/releases/download/11.2.1/harfbuzz-11.2.1.tar.xz
- fontconfig-2.16.0.tar.xz, SHA-256
  6a33dc555cc9ba8b10caf7695878ef134eeb36d0af366041f639b1da9b6ed220,
  https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.16.0.tar.xz
- libwebp-1.6.0.tar.gz, SHA-256
  e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564,
  https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz

The attempted libepoxy 1.5.10 GitHub release download returned HTTP 404; its
archive is absent. That is a source URL failure, not an engine/kernel failure.
The downloader exited and no background download remains. These exploratory
versions are not yet adopted build pins. Verify provenance/compatibility before
using them. Cached WPE Source/cmake/OptionsWPE.cmake lists full dependencies;
Source/ThirdParty/skia/CMakeLists.txt currently includes Ganesh/EGL and AVX/AVX512
targets. The kernel has no AVX context support. A software-only adaptation must
use genuine raster paths and prevent unsupported SIMD dispatch, without adding
fake GL APIs. Existing NetSurf FreeType/PNG/JPEG/Expat/zlib caches are available
under build/web/deps; do not overwrite those libraries or the SDK to resume.
No WebCore configuration, Skia build or new rendering test was started.

## Previous continuation: genuine JavaScriptCore component and threads

The following records the earlier JavaScriptCore stage. Its missing IPC/renderer
capability facilities are superseded by the latest continuation above.

The 2026-10-09 continuation implemented a real WebKit JavaScriptCore 2.54.0
static-musl component and its required kernel threading/runtime facilities.
**It did not replace NetSurf, build WebCore, implement DOM/CSS rendering or
complete per-site isolation.** Google/OSDev verification and Yahoo/Bing behavior
below remain unfinished. Do not describe a successful JS shell as successful
website rendering. Read [modern-browser-port.md](modern-browser-port.md) for
the engine assessment, primary sources, build choices and remaining integration.

- Genuine source-built JavaScriptCore/ICU 78.1 executes on this custom kernel.
  C_LOOP interpreter, generic event loop, system malloc, static ET_EXEC, no
  PT_INTERP. JIT/WebAssembly/sampling/libbacktrace are disabled. Real modern
  syntax, ICU Intl, promises/async jobs, modules and GC are exercised.
- Implemented musl pthread clone flags, TLS/TID setup, shared VM metadata and
  group context (FDs/cwd/umask/dispositions/timers/accounting), distinct TGID/TID,
  clear-TID futex wakeup, joined/detached task reaping and group/fatal exit.
  Blocking I/O pins descriptor references across sibling close/reuse. Mapping
  copies and signal dispositions are protected from stale page addresses and
  partially copied actions during same-CPU preemption.
- Real futex WAIT/WAKE, WAIT_BITSET/WAKE_BITSET, REQUEUE/CMP_REQUEUE with atomic
  queue transitions, masks, deadlines, signal interruption and private-VM keys.
  Bounded in-place mremap growth/shrink/rollback supplies musl stack discovery.
- Threads in a group stay on ONE CPU until TLB shootdowns exist. Separate
  processes still use SMP. Multi-threaded fork/vfork/exec/spawn, coherent shared
  file mappings, process-shared futex aliases, PI, robust-owner recovery,
  alternate signal stacks and full POSIX cancellation/context behavior remain
  unsupported. There are still only 128 total task slots and no AVX context.
- The existing browser sandbox still denies clone/fork/exec/spawn. Its syscall
  allowlist was not widened. The quota and display canvas cache now use the
  canonical shared VM metadata. Native toolbar/protocol/TLS/frontends were
  preserved. `/bin/jsc` is an unsandboxed local development CLI component,
  not a desktop page renderer or a new renderer sandbox profile.
- A separate POSIX-gthread libstdc++ was built from the existing GCC 14.2.0
  source with its real POSIX gthread header. The existing SDK's single-threaded
  libstdc++ was preserved. New component wrappers/prefix/stamps live under
  `build/modern-browser/`; never erase them or rebuild the whole SDK to resume.

Owned implementation files:

- `include/{sched.h,mm/user.h,uapi/abi.h}` and `mm/user.c`.
- `fs/binfmt_elf.c, kernel/sched/{process,sched,signals,syscall,sandbox}.c, and drivers/tty/tty.c`,
  `arch/x86/kernel/interrupts/interrupt.c`, `net/network.c`, `include/net/network.h`,
  `drivers/video/fbdev/core/display.c`.
- `userland/thread-check.c`, `userland/modern-browser/{runtime-check.cpp,
  engine-check.js,agent-check.js,module-check.mjs,feature-module.mjs}`.
- `tools/{build_javascriptcore,test_threads,test_javascriptcore}.py`, root and
  userland Makefiles, updated browser/kernel docs and vendor README/notices.
- Pinned new archives in `userland/vendor`: WPE WebKit 2.54.0, ICU 78.1 and
  host-only Ruby 3.4.7. Exact hashes/primary links are in the vendor README.
  Host CMake 4.4.4/Ninja 1.13.2 caches and original downloads are under
  `build/modern-browser/host-tools/` and `downloads/`. No host system packages
  were installed, no Linux partition was mounted/modified, no reset/clean ran.

Existing input files were copied before editing to
`build/modern-browser/original/<path>`. This preserves the actual dirty source,
not the unrelated old git baseline. It also enabled an isolated baseline-kernel
build and controlled drag comparison without resetting the working tree.

Build commands: `make javascriptcore`, `make test-javascriptcore`,
`make test-threads`. The JSC target is separate from normal `make iso`, which
packages the existing `/bin/jsc` and `/bin/thread-cxx-check` when present.
`test-javascriptcore` builds the component, then completes ISO packaging, then
boots guests. **Do not rebuild/rewrite the ISO while a test VM is using it.**
One intermediate browser rerun hit invalid-initramfs because of that sequencing
mistake; stable-artifact reruns passed. It was not a remaining kernel boot fault.

Continuation verification (real UEFI QEMU guests, no host disks):

- `tools/test_threads.py --cpus 1 4`: PASS. Eight checks cover invalid/time-based
  futex calls, mapping resize/rollback/zero fill, main-stack discovery,
  musl TLS/IDs/mutexes/conditions, descriptor close/reuse during blocked read,
  semaphores/directed signals, >128 joined/detached lifetimes, private-fork
  futex domains, exit_group, fatal worker SIGSEGV and leader exit before worker.
  The deliberate worker fault's exception log is expected; the enclosing child
  must report SIGSEGV, while the parent completes THREAD CHECK PASS.
- `tools/test_javascriptcore.py --cpus 1 4`: PASS. Real C++ threads,
  condition/call_once/promise/future/exception checks, modern JS/ICU/jobs/GC,
  imports/top-level await and four threaded JSC agents with separate VMs and
  shared ArrayBuffer atomic wait/notify/reporting. Shell agents do not prove
  browser Web Workers or per-site process isolation.
- `tools/test_browser_sandbox.py --cpus 1 4`: PASS after thread/VM changes.
- `tools/test_runtime.py --cpus 1 4 --no-storage`: PASS after these kernel changes.
- `tools/test_browser_guest.py --cpus 4`: PASS; actual rendered CSS/PNG and
  input/form submission, history/reload, safe downloads, verified HTTPS,
  hostname/trust rejection, resize/workspaces and child cleanup. The current
  screenshot `build/runtime-tests/browser-form-input.png` was also inspected.
  This remains a NetSurf regression check, not a modern public-site success.
- `make -j8 iso`: PASS. Python syntax checks of owned new builders/tests pass.
  git diff --check still reports only the known preexisting ps2_mouse.c CR line.

Logs: `build/modern-browser/{jsc-driver,jsc-configure,jsc-build,jsc-guest-tests,
thread-tests,sandbox-final,runtime-final,browser-guest-final,iso-build}.log`
and `build/runtime-tests/uefi-{1,4}cpu-{javascriptcore,threads}.log`.

One unresolved desktop failure: `tools/test_gui_drag.py --cpus 1` at the user's
current **2560×1600** mode fails its 25-ms rapid-drag position assertion. The
long-drag pixel restoration/release checks pass. The same test using an ISO
with the saved pre-thread kernel, the current desktop and identical resolution
also fails at the same assertion. Current 66 moving frames take 1,094,702 us
(16.59-ms mean); baseline 70 take 1,115,461 us (15.94-ms mean). These are TCG
work measurements, not host scanout; do not claim a new performance improvement
or a drag-test pass. Logs: `drag-retest.log`, `drag-baseline.log` in that directory.
No test was weakened or resolution/frontend setting changed. The older cached
1440×900 ISO lacks the current GUI info marker and is not a valid comparison.

At the end of that earlier continuation, integration needed full WebCore/WPE,
Unix descriptor IPC, shared memory, threaded broker launching and renderer roles.
The latest continuation above supplies bounded IPC/memory/launch capabilities
and GLib/Gio, while full rendering/resource broker/frame/navigation work remains. Do not run
downloaded page scripts through the unsandboxed JSC shell or globally relax the
existing NetSurf policy as a substitute for that work.

## Current OS/browser architecture

This is the actual custom kernel, not Linux running beneath a desktop.
It has ring-3 processes, separate address spaces, SMP/preemption, a Linux-numbered
syscall subset, VFS, NVMe/AHCI storage, USB HID, framebuffer desktop, static musl
programs, native GCC/G++, Git, curl/OpenSSL, and lwIP IPv4 networking.
The tested musl pthread/private-VM futex subset is now implemented as described
above. Bounded Unix socketpairs/descriptor IPC and coherent memory-object mappings now
work. General shared file coherence, dynamic linking and named Unix sockets
remain missing. The precise subset is in browser-ipc.md.

The real engine is pinned NetSurf 3.11, libcss/libdom, Duktape, FreeType and image
libraries, built statically against musl. It is NOT a Chrome/Firefox equivalent.
ES6 syntax/APIs, modern DOM APIs and CSS features remain incomplete. Changing
the user-agent or adding a few globals cannot provide modern compatibility.

`userland/desktop/browser.c` is the trusted native toolbar/window adapter.
`userland/browser/surface.c` implements the offscreen surface and pipe events.
`userland/include/browser_protocol.h` defines the bounded private 40-byte
protocol, magic 0x3142574d, damaged XRGB rectangles, navigation and load state.
The desktop validates packets, owns hardware display/input, and drains pipes
without blocking its event loop. `gui --trace` launches the engine with `-v` and
logs detailed engine errors; normal launches omit `-v`.

`tools/build_browser.py` verifies and extracts
`userland/vendor/netsurf-all-3.11.tar.gz` to
`build/browser/netsurf-all-3.11`. It patches generated copies with owned source
fragments. Edit owned source/build patch logic, not just generated upstream
copies, which are re-extracted when the digest changes. The original archive is
unchanged. Builds use existing offline dependency caches.

## Implemented browser fixes

- `userland/browser/path.inc`: replaced the framebuffer SVG path no-op with
  bounded affine path rendering, cubic subdivision, nonzero-winding fills/holes,
  and thick strokes. This is not the complete SVG rendering specification.
- `box_order.inc`, `stacking.inc`, `hit_test.inc`: stable sibling painting by
  stacking category/z-index and matching pointer targets. Covered links no
  longer win clicks. This is not full CSS stacking-context hoisting.
- Build patches fix fixed-position viewport sizing, cancel ancestor offsets,
  retain viewport positioning on scroll, and propagate HAS_FIXED_DESCENDANTS
  so an offscreen ancestor does not cull a visible fixed child.
- CSS and FreeType use consistent 96 DPI. Fixed viewport lengths use the
  complete CSS-to-device conversion rather than rounding each pixel unit.
- Short-page scrolling clamps to zero. Scroll repaints instead of copying old
  pixels that would move fixed content. Resizes allocate a fresh white surface
  with the correct stride. Failed FreeType glyphs advance UTF-8 instead of
  hanging line breaking/hit testing.
- Local `.js`/`.mjs` assets get JavaScript MIME types, `.txt` gets text/plain.
- `performance.inc`: basic per-document `performance.now()` from CLOCK_MONOTONIC
  at millisecond precision, read-only timeOrigin from context creation time,
  branded receiver checks, and `globalThis`/`self` window aliases. No fabricated
  navigation/resource timing or complete Performance interface. Alias own
  properties use duk_def_prop; assigning through inherited self's getter had
  broken window construction and was corrected.
- `fetch_policy.inc`: remote documents cannot fetch local file content, even
  previously cached images/frames/scripts/local links. Policy runs before cache
  lookup as well as fetch creation. Built-in resource CSS remains accessible.
  An upstream redirect-scheme check inspected the source instead of the target;
  it now rejects HTTP redirects to file/about/resource destinations.

## Implemented sandbox and its limits

- New `kernel/sched/sandbox.c` and `include/sandbox.h` implement default-deny
  syscall filtering and normalized/canonical path restrictions.
- UAPI in `include/uapi/abi.h`: spawn_browser 407 and browser_grant
  408. os_spawn_browser is 32 bytes, retaining the original 24-byte os_spawn_io.
  The desktop uses 407 for EVERY native Browser window before the child runs.
- Only three explicit pipe endpoints are inherited; no extra descriptors or
  arbitrary environment. HOME is canonical, environment is sanitized.
- Read access: installed NetSurf resources/fonts, explicit CA/resolver files
  and entropy/null/zero devices. Write access: HOME/.config/netsurf and
  HOME/Downloads. A trusted user local-document selection grants its canonical
  directory read-only for relative assets; a root-level file grants just itself.
  Only the immediate trusted parent can replace this grant. Existing file reads
  are checked against the current grant, and remote file fetches are denied.
- VFS checks normalized paths, every symlink target, canonical mutation parents,
  FD reads/writes and truncation under its lock. Tests cover symlink escapes,
  `..`, creation/rename/truncate escapes, and canonical symlink HOME.
- Fork/clone/exec/spawn, display/input, mounts/system controls, filesystem
  permission/symlink changes, foreign signals and unknown syscalls are denied.
  Outgoing IPv4/DNS is allowed. Listening/accepting and nonzero bind are denied;
  wildcard ephemeral bind is allowed for outgoing use.
- `mm/user.c` counts mapped pages and enforces a 512-MiB per-browser limit,
  including image/stack/mappings. mmap/brk and unmap accounting obey this;
  getrlimit(RLIMIT_AS) reports the limit. General unsandboxed programs keep their
  existing behavior.
- LIMITS: each window still has ONE NetSurf process shared by its pages/sites,
  cookies, downloads and current local grant. No per-site process isolation.
  This does not prove production-grade same-origin behavior, protect against all
  kernel bugs, or bound all network/kernel/storage resource use. Direct CLI
  engine execution is outside the desktop sandbox launch path.

## Verification already completed

Do not treat HTTP 200, a title, or a load-finished event as proof of correct
rendering. Public failures below remain real failures.

- `tools/test_browser_protocol.py`: actual engine files/history/resize/clean exit.
- `tools/test_browser_render.py`: real pixels/clicks for SVG curves/holes/strokes,
  stacking, covered links, nested fixed elements before/after multiple scrolls,
  short-page bounds and resized stride. Passed after the latest engine changes.
- `tools/test_browser_script.py`: actual imported scripts, aliases, monotonic
  clocks, origin immutability/alignment and invalid receiver checks. Passed.
- `tools/test_browser_policy.py`: temporary local HTTP server, primed local
  image/script cache, remote images/frames/scripts/file-base/links/HTTP redirect
  denial and explicit trusted local navigation. Passed. Host socket access may
  require escalation in a new sandbox.
- `userland/browser-sandbox-check.c` and `tools/test_browser_sandbox.py`: hostile
  child filesystem/syscall/FD/grant/memory escape tests plus allowed cookies,
  downloads, entropy and selected files; passed on 1 AND 4 CPUs.
- `tools/test_browser_guest.py --cpus 4`: actual desktop USB input, file association,
  HTTP/CSS/PNG, current clock/script fixture, forms, history/reload, downloads,
  HTTPS trust/hostname rejection, resizing/workspaces and child cleanup. Passed.
  Form-click y coordinate is now 333 because corrected 96-DPI fonts moved it;
  do not revert it to the old 319 without inspecting actual pixels.
- General `tools/test_runtime.py --cpus 1 4 --no-storage` passed after kernel/VFS
  changes. Actual guest posix-check, build-check and build-check --files /tmp
  passed. `tools/test_kernel_memory.py` tests actual kernel memory primitives
  against host guard-page boundaries; it passed (not a mapped-page quota test).
- `make -j8 iso` succeeded. Recent logs: build/browser-port-build.log,
  build/browser-iso-build.log and build/runtime-tests/uefi-*-browser*.log.
- git diff --check reports preexisting CR/trailing whitespace in
  drivers/input/mouse/ps2_mouse.c:2; do not "fix" unrelated user edits. Owned new browser
  and sandbox files passed whitespace and Python syntax checks.

`make test-browser` builds the ISO and runs the listed browser checks. Individual
checks are available for targeted changes. Avoid rebuilding the whole SDK or
repeating long suites without a new change or unresolved concern.

## Public sites: still unfinished

`tools/diagnose_browser_sites.py` captures the real sandboxed desktop engine,
waits through script redirects/intermediate READY events, and records errors
and screenshots. Defaults: Google search, OSDev, Yahoo, Bing home/search, GCC.
It supports --url, --iso, --cpus, --timeout. This is an optional diagnostic,
NOT an assertion that these sites are compatible. It needs public network.

Latest evidence is `build/runtime-tests/browser-sites-report.json`,
`browser-sites-{google-search,osdev,yahoo,bing,bing-search,gcc}.png`/`.ppm`, and
`uefi-4cpu-browser-sites.log`. Observed on 2026-10-09:

- Google: adding the real performance clock removed the first missing-global
  failure. Scripts then redirected search to Google's unusual-traffic
  verification (`/sorry/`, 429). Verification page renders; search results do not.
- OSDev: server returns Cloudflare challenge, 403 and cf-mitigated: challenge.
  Challenge script hits unsupported syntax. The requested website does not open.
- Yahoo: real browser request succeeds, but Next.js scripts hit Duktape syntax
  and binding errors. Incomplete navigation/feed/layout. Separate curl probe
  returns 429 because requests differ; do not equate curl with browser behavior.
- Bing: actual search results appear, but layout/styles and scripts are still
  incomplete; errors include absent performance.timing.navigationStart and
  subsequent _w/_d/_ge globals. Home is also incomplete.
- GNU GCC: public HTML/CSS page renders over production-CA-verified HTTPS.

TLS checks remain enabled. Do not turn off certificate or hostname validation,
claim cached fixture pages are real public sites, fake APIs/timing to satisfy a
script, spoof browser capabilities, or bypass verification challenges as a
substitute for a working engine.

## Remaining browser work / continuation approach

1. Read Documentation/browser-network.md and inspect the engine/OS boundary. Assess a
   genuine modern engine port using current PRIMARY upstream documentation and
   source, then implement the required OS/userland facilities and integration.
   The continuation has ported genuine WebKit JavaScriptCore, rendering/network
   dependencies and CPU WebCore. Controlled DOM/Grid/canvas/text pixels and
   a mouse click pass in two confined renderers on 1/4 CPUs. Resume genuine
   broker resource delivery, software frontend/input, page/frame/navigation
   integration and public-site checks; exact gaps are in modern-browser-port.md.
   Blindly building an unported desktop Chromium binary is not a working
   custom-kernel port.
2. Keep the actual custom kernel and native desktop. Preserve the bounded
   offscreen protocol, trusted toolbar, input/compositor ownership, verified
   HTTPS, safe downloads and existing regression coverage.
3. Modern engines may require threads/futexes/shared memory/IPC and helper
   processes. Design renderer/broker capabilities rather than globally removing
   the current sandbox. Implement actual site/origin isolation and local-file
   boundaries; the current per-window confinement is only a starting point.
4. Verify real layout, images, scripts, inputs and navigation with meaningful
   standards fixtures and actual screenshots/clicks in the guest. Separately
   report server verification/rate limiting. Do not promise all arbitrary future
   websites work merely because a sample returns 200.
5. Rebuild the ISO, run appropriate affected checks, update limitations/evidence,
   and tell the user exactly which requested sites still fail. Do actual work;
   do not stop at another general plan or pretend the old NetSurf fixes completed
   the original modern-browser goal.

## QEMU responsiveness: preserve these implemented improvements

Current Makefile automatically chooses KVM when /dev/kvm is readable/writable,
uses host CPU with KVM or max with TCG, prints the selection, and provides
`make run-fast` to require KVM. Default frontend is SDL/OpenGL (`sdl,gl=on`),
native Wayland SDL when available, with explicit backend overrides. This replaced
a slow software GTK path and fixed physical pointer capture on the tested KDE
Wayland host. QEMU standard VGA gets 32 MiB VRAM. refresh_rate=120 is EDID metadata,
not a promise of 120-Hz host output.

The desktop caches wallpaper/decorated window surfaces/shadow masks, paints only
dirty rectangles, skips opaque-covered content, coalesces consecutive motion
without dropping button/key transitions, and targets an 8,333-us composition
cadence with 1-ms input/idle wakeups. Ordinary drags reuse app content; idle
desktops do not continuously repaint. Native browser pipes/damage packets keep
page loading separate from the trusted GUI event loop.

Display ABI 3 validates batches and complete canvases and uses QEMU VBE hidden
page upload/flip when supported. It repairs prior damage, fences writes and
preserves/releases the firmware mode. Generic framebuffer fallback remains.
Video memory uses safe write-combining PAT mappings and validated 2-MiB leaves
where possible. Exact SSE2 framebuffer/window copies and cached shadow blending
reduce work; TCG uses the measured faster integer shadow path. Canvas validation
is cached with address-space generation invalidation. Presentation holds one
display mutex rather than repeated framebuffer locks across rows.

Evidence, already recorded in Documentation/desktop.md and logs:
- Final physical-mouse SDL/GL drag: 131 frames, 117,349 us total work = 0.896 ms
  mean, 1.940 ms max, 9.607-ms moving-frame interval (about 104 Hz). These are
  guest timings, not monitor scanout; worst interval was 46.018 ms.
- Saved GTK software vs GL host publication benchmark: about 26.8 vs 104.2 Hz.
  Do not present this GTK comparison as a direct SDL monitor measurement.
- TCG buffered drag: 4.508-ms mean work on 1 CPU, 5.341 ms on 4, about 9-ms
  intervals; real pixel restoration and release/rapid-drag checks passed.
- Tools: test_gui_drag.py, test_pixel_blend.py, diagnose_mouse.py. QMP/XTest
  injection does NOT prove physical Wayland pointer capture. make test-mouse-live
  exercises physical input; make test-drag-native runs visible KVM/GL tests.
- Relevant files: Makefile, userland/desktop/{desktop,render,window_manager}.c,
  drivers/video/fbdev/core/{display,scanout,fbcon}.c and video_copy.S,
  mm/vmem_io.c and mm/user.c.

Intel/NVIDIA native modesetting, GPU rendering and synchronized hardware page
flips remain missing. Host OpenGL texture upload is not guest GPU acceleration.

## Storage answer given to the user

Current root/Home/tmp are RAM. Supported NVMe/AHCI ext2/ext4 mount automatically
read-only when clean; FAT32 mounts read/write and persists. Files Ctrl+L can
open paths from `lsblk`/`mounts`; save documents there and `sync` before shutdown.
Desktop preferences use the first writable FAT32 mount's .modernos/desktop.ini
when available, but Home/downloads are not automatically moved to disk.
FAT12/16, exFAT, NTFS, Btrfs, encryption/LVM and USB mass-storage are not supported.

READ-ONLY host discovery found /dev/nvme0n1p4 is Btrfs (Linux root/home),
/dev/nvme0n1p1 is vfat mounted at /boot, and the remaining identified partitions
are NTFS. Recheck if hardware changes. Do not format/pass through a live mounted
Linux system disk for testing. Use disposable disk images for driver work.

The user was shown a safe NEW-file QEMU FAT32 image example from Linux:

```sh
test ! -e modernos-data.img &&
truncate -s 1G modernos-data.img &&
mkfs.fat -F32 modernos-data.img &&
make run FAT_DISK="$PWD/modernos-data.img"
```

No image was created by the agent. Reuse the same image on later boots with
`make run FAT_DISK="$PWD/modernos-data.img"`. A single unpartitioned attached
image normally mounts at /mnt/nvme0n1; always verify actual discovery names.
Adding Btrfs access or persistent Home is a separate feature request, not work
already completed or explicitly started.

Further context: README, Documentation/browser-network.md, Documentation/desktop.md,
Documentation/kernel-userland.md and Documentation/native-tools.md. There is no need to reproduce
the entire completed session before continuing the browser work.
