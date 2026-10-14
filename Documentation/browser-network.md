# Native browser, Git and networking

ModernOS runs genuine WebCore/JavaScriptCore 2.54.0 and Git 2.56.0 as static
musl programs. Launch Browser from the taskbar/launcher to browse HTTP/HTTPS
or search from its address field. Local HTML/HTM, SVG, PNG, JPEG, GIF and BMP
file associations retain NetSurf 3.11 and work offline.

## Browser integration

The desktop owns window decorations, stacking, focus, workspaces, resize,
minimize/maximize/close, menus and input. Browser registers through the existing
AppOps interface. Its native shared 40-pixel toolbar contains an editable URL
field and real back/forward/reload/stop controls. Navigation availability and
loading state come from the engine; disabled arrows do not imply a history item.
Ctrl+L selects the address, Enter navigates, Escape cancels address editing or
the active page load, Alt+Left/Right navigate history, F5/Ctrl+R reload, and
Ctrl+N opens another native browser window. Plain words or phrases in the
address field become percent-encoded Google searches; hostnames default to
HTTPS. Addresses are bounded to 4096 bytes;
absolute filesystem paths retain the OS's 511-byte path limit.

The renderer runs in a separate kernel address space. A private versioned pipe
protocol carries input, navigation, load/history state, titles/addresses,
notices and damaged XRGB client rectangles. The desktop validates dimensions,
payload sizes and clipping before applying a complete packet. The engine never
owns a hardware framebuffer or replaces the shared compositor/input system.
Client surfaces are bounded to 4096 pixels per dimension and 16 million pixels.
Pipe backpressure sleeps the engine; GUI reads are nonblocking and budgeted.
Pure pointer motion can coalesce, while button and key transitions stay ordered.

The web renderer uses genuine WebCore DOM, modern CSS, canvas, image decoders,
HarfBuzz/FreeType fonts and JavaScriptCore. The trusted Soup broker loads real
resources with TLS chain/hostname validation, HTTP cookies and CORS/preflight
handled through WebCore. Bodies travel through sealed memfds using the separate
64-byte resource/service protocol. The chrome client schedules actual rendering
updates; the display loop paints changed pixels with a retained CPU surface.
Wheel scrolling consults the page's animation preference in this CPU port.
WebCore's actual scroll operations reuse the retained raster; only invalidated
regions are repainted and compared for transport. Changed fixed/sticky content
still receives real damage. Both pixel pipes are 256 KiB. The desktop commits
native copy/damage packets together at paint completion, with a bounded queue
and cancellation when the renderer changes. A writer thread drains prepared
pixels during long JavaScript tasks; persistent scrollbars avoid fade redraws.
The actual WTF/GLib loop delivers activity observers and paces rendering.
Dispatches yield after 4 ms or when native input is queued, so successive
background script tasks cannot monopolize the browser's input/paint loop.
Generic font families
resolve to the shipped DejaVu files; zero-sized fallback text has a distinct
font-cache key and canvas font setters retain a valid zero size.

The broker owns top-level origin changes and launches a fresh renderer for each
origin. Same-origin pages retain WebCore history items; bounded document
snapshots preserve form method/body across renderer replacement. Cookies and
UTF-16 local/session storage have actual broker services. Local storage uses
SQLite's directory-lock VFS; session storage stays in the window broker. Native
input, history, storage, frames, downloads, resize and lifecycle checks are in
`tools/test_native_browser.py`; see [AI-HANDOFF.md](AI-HANDOFF.md) for exact passes
and open failures. See [browser-fixes.md](browser-fixes.md) for the versioned
public-site screenshots and scroll measurements.

Isolated frames synchronize actual WebCore focus and keyboard paths through the
broker. The User Activation API uses genuine WebCore state: real input updates
ancestors and same-origin descendants, while synthetic clicks do not grant
activation. Consumption and expiry preserve their original monotonic timestamps.
Frame input and activation regressions are in `tools/test_native_frame_focus.py`
and `tools/test_native_activation.py`. Cloudflare acceptance remains unfinished;
the current native OSDev and official diagnostic tests still fail verification.

Direct address/toolbar navigation passes its actual client provenance to
WebCore's Fetch Metadata logic; script/frame/history loads retain their own
context. The broker validates structural snapshots and synchronizes real
WebCore ancestor/sibling trees, names and stable WindowProxy identities across
isolated renderers. Local, foreign-sibling and ancestor postMessage routing uses
the actual source and destination frames. Opaque sources use WebCore's HTML
`null` serialization, preserving target-origin exclusion and SOP. Frame IPC is
version5. Regressions are in `tools/test_native_fetch_metadata.py` and
`tools/test_native_frame_graph.py`, with sanitizer/authority checks in
`tools/test_frame_tree_protocol.py` and `tools/test_frame_tree_authority.py`.
These integration fixes do not establish OSDev/Cloudflare acceptance; exact
current images, failed live evidence and validation are in [AI-HANDOFF.md](AI-HANDOFF.md).

Native CPU decoding supports MP4 H.264/AAC and WebM VP8/VP9/Opus/Vorbis;
MP4 Media Source Extensions use real WebCore coded-frame processing. Analog
PCM output supports AC'97 and HDA. Foreign frames have separate origin-bound
processes; local blob resources and dedicated workers use WebCore's actual
implementations. WebAssembly uses its bounds-checked IPInt interpreter.
IndexedDB, service workers, WebGL/WebGPU and clipboard remain unavailable.
JavaScript runs without JIT or GPU acceleration. A server verification challenge
can still prevent a site from opening. Read [browser-media-scroll.md](browser-media-scroll.md)
for the new implementation, tests and current limits.

## Local-file and legacy engine

The framebuffer port now paints SVG paths instead of silently discarding them.
It handles affine coordinates, cubic curves, nonzero-winding holes, fills and
strokes with bounded subdivision and clipped rasterization. CSS/font metrics
use a consistent 96 DPI. Positioned siblings
use z-index order for both painting and hit testing. Fixed elements use viewport
dimensions and retain their position during scrolling. Scroll bounds never go
negative for short documents, and resize starts with a correctly sized surface
rather than preserving pixels with an obsolete row stride. These are targeted
port fixes, not a complete implementation of modern CSS.

Each document also exposes `globalThis`/`self` aliases and the basic
`performance.now()`/`performance.timeOrigin` clock operations. The former uses
the OS monotonic clock at millisecond precision; the latter is read-only and
records the document context's creation time. Resource/navigation timing and
the complete Performance interface remain absent.

Downloads use HOME/Downloads and sanitized real filenames. The engine opens an
exclusive temporary file, checks every write, flushes/closes it and publishes
it with RENAME_NOREPLACE. An occupied destination causes an informative error;
existing data stays intact. Failed/cancelled transfers remove their partial
file. Closing cancels outstanding transfers and saves the actual cookie jar
in HOME/.config/webkit/cookies.txt (web) or HOME/.config/netsurf/Cookies
(local/legacy) before the process is reaped. A stalled engine
has a bounded termination fallback. RAM cookies/downloads disappear on reboot;
storage persistence depends on the real home/mount configuration.

## Browser process sandbox

Web windows use `spawn_web(409)` with an explicit broker role. The broker may
access only its private WebKit configuration/downloads, installed resources,
CA/resolver files and outgoing network. Each immutable-origin renderer has its
own address space and 768-MiB mapped-page quota; it cannot open network sockets,
read home/configuration/download files, change its origin, execute helpers or
signal another process. Broker cookie/storage requests enforce the renderer's
origin. Broker SIGKILL is restricted to its own renderer child, and broker death
cleans its renderer children. See [browser-ipc.md](browser-ipc.md).

Local-file windows use `spawn_browser(407)` before the child becomes runnable.
The following filesystem grants and syscall limits describe that legacy role. The kernel applies a default-deny syscall policy,
separate userspace mappings and a 512-MiB limit on mapped user pages. Only three
explicit pipe endpoints are inherited; desktop file/device descriptors and
environment overrides are not passed to the engine. The desktop still owns the
display and input devices.

The engine may read its installed resources/fonts, the system CA bundle and
resolver files, and entropy/null/zero devices. It may read and write only its
HOME/.config/netsurf and HOME/Downloads trees. Local document selection in Files
or the native address field grants that document's directory read-only, for
relative images/stylesheets. A file selected directly under `/` grants only that
file. The immediate trusted parent can replace this grant through
`browser_grant(408)`; a web page cannot grant itself filesystem access. HOME and
grants resolve to canonical paths. VFS checks normalized paths, every symlink
target, creation/rename destinations and file reads/writes under its lock.

Fork/clone/exec and other process launches, display/input access, mount and
system controls, filesystem permission/symlink changes, foreign signals and
unknown syscalls are denied. Outbound IPv4 sockets and DNS remain available;
listening/accepting sockets and binding a nonzero port/address are denied.
HTTP/HTTPS documents cannot fetch `file:` content, including cached images,
frames and documents from an earlier local visit. HTTP redirects to local or
internal URL schemes are rejected before cache lookup. Explicit address-field
navigation to an authorized local file remains supported.

This is a kernel-enforced browser process sandbox. Pages in one window still
share one NetSurf process, its cookies, downloads and local document grant;
there is no process isolation per site. It does not make NetSurf's incomplete
web APIs or origin policies equivalent to a production browser, and it does not
limit all kernel/network/storage resource consumption. Direct unsandboxed
command-line execution of the engine is outside the desktop launch policy.

## Networking and TLS

The existing kernel now includes lwIP 2.2.1, IPv4 TCP/UDP, local sockets,
ARP/ICMP, DHCP and DNS configuration. The Intel e1000/e1000e driver supports
PCI IDs 8086:100e and 8086:10d3, including QEMU's e1000e adapter. It uses bounded
DMA rings and the existing PCI, memory and scheduler infrastructure. The new
`drivers/net/igc.c` driver adds I225-V/LM (15f3/15f2), I226-V/LM (125c/125b)
and the 82576 (10c9), including QEMU's igb device. It uses advanced descriptors,
bounded PHY/firmware ownership polling, autonegotiation and kernel-owned DMA.
Its packet path passes DHCP/HTTP/TLS/Git in QEMU; the I226 physical PHY path
still needs a real boot. Wi-Fi is unsupported. A worker
polls receive/completion queues and stack deadlines without holding the VFS
mutex across network waits. DHCP writes actual resolver addresses to
/etc/resolv.conf. `netinfo` reports link/address/gateway and real packet counters.

Linux-numbered socket/connect/bind/listen/accept/accept4, send/receive,
sendmsg/recvmsg, socket options, names and shutdown use this real stack.
Descriptors support nonblocking mode, close-on-exec, poll/select, read/write,
dup and fork lifetime. TCP receive storage is bounded to 64 KiB per socket;
UDP and accept queues hold 32 and 16 items respectively. Vector transfers
accept up to 16 entries and a 16-MiB aggregate. AF_UNIX socketpairs now implement bounded SCM_RIGHTS and stream/datagram/seqpacket
IPC independently of the NIC; coherent memfd and epoll facilities support the new
engine components. See [browser-ipc.md](browser-ipc.md). IPv4 ancillary data, raw
sockets, IPv6, named Unix sockets, Wi-Fi and other NIC drivers remain unsupported;
unsupported families/options return errors. No NIC leaves working loopback and
honest external connection failures. This does not establish connectivity on
the user's Intel/NVIDIA laptop without a supported Ethernet adapter.

curl 8.22.0 and NetSurf use OpenSSL 3.5.9 LTS and a pinned Mozilla CA bundle at
/etc/ssl/cert.pem. Certificate-chain and hostname checks remain enabled.
NetSurf's OpenSSL adapter distinguishes numeric IP SANs from DNS names.
The integration tests trust an explicit disposable fixture CA inside that
guest only, and separately reject untrusted certificates and wrong hosts.
TLS uses actual OS time and hardware entropy; unavailable prerequisites cause
errors. The OS exposes getrandom(318), /dev/random and /dev/urandom using checked
RDSEED/RDRAND availability and bounded retries, without substituting clock or
address seeds. Missing hardware returns ENOTSUP; exhaustion yields EAGAIN or a
short read. This currently relies on the CPU's entropy implementation.

eventfd(284)/eventfd2(290) provide real shared 64-bit counters, semaphore mode,
nonblocking reads/writes, poll readiness, overflow checking and descriptor
lifetime. They supply curl's actual wakeup dependency. CPU accounting exposes
getrusage(98), times(100), optional wait4 usage and process/thread CPU clocks.
User/kernel time is sampled at the existing 1-ms timer; peak RSS and the private
current/peak/limit memory query use actual mapped-page counters. Other untracked
resource fields remain zero. Read-only self `prlimit64` supplies musl's real
stack bounds. The kernel now supports the tested musl pthread clone/TLS
subset, shared thread-group context and private-VM futex waits/wakes/requeues.
Threads in a group stay on one CPU; general shared file mappings and process-shared futex aliases remain absent.
Coherent memfd mappings and explicit renderer/broker profiles are now tested as
separate modern-engine facilities. The legacy NetSurf sandbox still denies clone;
see [modern-browser-port.md](modern-browser-port.md) for precise ABI limits.

## Git and command-line use

```text
netinfo
curl -fsS https://example.com/ -o /tmp/page.html
git --version
git init /home/user/Projects/repository
git -C /home/user/Projects/repository config user.name "Your Name"
git -C /home/user/Projects/repository config user.email you@example.com
git clone https://server.example/project.git /tmp/project
git -C /tmp/project status
```

Git includes its real object/index/ref handling and smart HTTP/HTTPS helpers.
Local init/add/commit/status/log/diff/branch/checkout/clone/bundle/fsck and
network clone/fetch/push use upstream code and real data. Configure identity
before committing. Work in RAM: FAT32's occupied-target rename and symlink
limits prevent a dependable Git worktree there. Export a bundle or separate
files to writable disk for reboot persistence. SSH requires an SSH client,
which is not installed. Perl/Python/Tcl/Rust, GUI/gitk, gettext, threads,
credential-service integration and legacy WebDAV push are disabled. Explicit
server authentication/credential behavior still follows supported Git/curl
options; this OS has no credential vault or multi-user isolation.

## Offline assets and source ownership

Pinned upstream archives live in userland/vendor with SHA-256 verification.
The build includes NetSurf's thirteen libraries, parser/binding generator and
resources; OpenSSL, curl, zlib, libpng, libjpeg, FreeType and Expat are static.
Eight licensed DejaVu fonts are bundled under userland/browser/fonts, so a
normal browser build does not depend on host-installed fonts. Default Choices,
home page, CSS/messages/images and the real CA bundle are packaged in the
initramfs. Corresponding headers/static libraries/pkg-config metadata are
installed under /usr for further native ports. License notices are installed
at /usr/share/licenses/native-programs. Engine patches affect generated build
copies; the original source archives remain intact.

Host dependencies include GCC/G++, binutils, make, Python, tar, pkg-config,
Perl, autoconf/automake/libtool, flex/bison, ncurses tools and xorriso. The host
binding generator and GNU gperf are build tools; guest programs execute real
static x86-64 binaries. No Linux desktop service or web framework is required.

| Source | Responsibility |
| --- | --- |
| userland/desktop/browser.c, desktop.[ch], window_manager.c | Registered native Browser, shared toolbar dispatch, engine lifetime and client rendering |
| userland/include/browser_protocol.h | Bounded private packet declarations |
| userland/browser/surface.c, actions.inc, downloads.inc, path.inc, box_order.inc, stacking.inc, hit_test.inc, fetch_policy.inc, performance.inc | NetSurf offscreen/input, navigation, downloads, rendering/hit testing, local-fetch policy and basic document clocks |
| userland/browser/Choices, modernos.html, fonts/ | Default options, local start page and licensed fonts |
| tools/build_browser.py, build_web_deps.py, build_git.py | Verified offline engine, libraries, SDK and Git builds |
| net/network.c, include/net/, drivers/net/e1000.c | Existing-kernel IPv4 stack, sockets, deadlines, DHCP/DNS and supported NIC |
| drivers/char/random.c, fs/eventfd.c, kernel/sched/syscall.c, kernel/sched/sched.c | Entropy, event descriptors and sampled CPU accounting |
| kernel/sched/sandbox.c, fs/vfs.c, fs/binfmt_elf.c, mm/user.c | Browser syscall/path restrictions, controlled spawn/grants and mapped-page limit |
| tools/web_fixture.py, test_network.py, test_browser_protocol.py, test_browser_render.py, test_browser_script.py, test_browser_policy.py, test_browser_sandbox.py, test_browser_guest.py | Disposable fixtures, renderer/script/file-policy regressions and real kernel/desktop integration |
| tools/diagnose_browser_sites.py | Optional public-site diagnostics and screenshots from the sandboxed desktop engine |

```sh
make test-network
make test-browser
make test-browser-surface             # ASan/UBSan retained-copy bounds check
make run QEMU_NETWORK='-nic none'     # deliberate offline boot
python3 tools/test_web_live.py --cpus 4  # optional public Internet check
python3 tools/diagnose_browser_sites.py --iso build/os.iso --timeout 120 --url 'https://www.google.com/search?q=osdev' --url https://www.youtube.com/ --url https://osdev.org/
```

Tests use actual kernel sockets and USB desktop input. They verify HTTP body
hashes, chunking/redirects, TLS trust/hostname rejection, Git clone/fetch/push
against a disposable repository, HTML/CSS/PNG/JavaScript/forms, history/reload,
safe downloads, shared window operations/workspaces and child cleanup.
Browser tests also check actual pixels and clicks, cached local-file denial,
HTTP redirect denial and hostile sandbox children on one and four CPUs. The
hostile child tries filesystem/symlink escapes, inherited descriptors, process
creation, desktop/mount access, foreign signals and excess memory allocation,
while verifying that cookies, downloads, selected local assets and entropy work.
No external account or repository is modified. Results apply to the tested
QEMU adapter; other physical NICs and graphics controllers need native drivers.
The optional public-network test also verifies actual DNS lookup, the production
CA bundle, the rendered GNU GCC HTTPS page and read-only GNU Hello Git refs.
The public-site diagnostic records redirects, engine load completion,
JavaScript errors and actual screenshots. An HTTP 200 or completed load is
not a rendering-compatibility pass; inspect the screenshots. It waits through
script redirects so an intermediate load state cannot count as a finished site.

The following is historical NetSurf evidence from 2026-10-09, before the native
WebCore integration. Current native-browser evidence and remaining limitations
are recorded in [modern-browser-port.md](modern-browser-port.md) and the active
section of [AI-HANDOFF.md](AI-HANDOFF.md). Public diagnostic output under
`build/runtime-tests/` is transient; preserved revision directories are under
`build/modern-browser/public-site-v*/`.

| Site | Observed result |
| --- | --- |
| Google `/search?q=osdev` | Initial HTTPS request succeeds; scripts now advance to a redirect to Google's unusual-traffic verification page. Search results are not available in this test. |
| OSDev | Returns a Cloudflare verification challenge (HTTP 403); its script uses unsupported syntax. The requested site does not open. |
| Yahoo | Browser fetch succeeds, but current scripts have syntax/binding errors and the rendered feed/layout is incomplete. The separate curl probe is rate-limited (429). |
| Bing home/search | Search results appear, but styling/layout and scripts remain incomplete. |
| GNU GCC | The public HTML/CSS page renders and certificate-verified HTTPS succeeds. |

These historical results do not describe the current WebCore renderer.
Compatibility still depends on implemented OS/browser services and server
verification. A successful fixture or HTTP response is insufficient evidence
that a public page looks and behaves correctly.
