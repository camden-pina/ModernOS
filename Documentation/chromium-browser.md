# Chromium browser for Coast

Boot the selected browser image with `make run-browser`, then enter `gui` at
the Coast prompt to open the desktop. Press **Super+Space**,
type **Chromium Browser**, and press **Enter**. Reopening the entry restores
the running window. The browser has its own Dock icon and native window controls.

This is a Coast browser frontend using the real, already ported Electron
43.7.3 / Chromium 150.0.7871.250 runtime. It runs inside Coast, including its
networking, sandboxed renderer processes, software compositor and USB input.
It does not run a browser or rendering service on the host. The runtime's
upstream notices remain in the base image. Chromium is the desktop browser; Code OSS remains available. The earlier
NetSurf/WebCore browser has been removed.

The selected image and qualification are recorded in `build/BROWSER-CURRENT.json`.
Images and local runtime caches are build artifacts. The browser image is
separate from `build/Coast-codeoss.iso` and does not replace that editor image.

The source-cleanup candidate is recorded under
`build/coast-chromium-source-r1/`. Its native desktop and static Wayland bridge
are built from checked-in sources and pinned dependencies. The guest browser
suite passes real painting, HTTP/JavaScript, input/forms, history, tabs,
fullscreen, TLS rejection, exact downloads and helper shutdown. The older
browser executables/resources are filtered from every inherited initramfs layer.
The selected public pointer is updated only after guest verification.

## Use

The address bar accepts HTTP/HTTPS URLs, hostnames and search terms. Hostnames
default to HTTPS; plain HTTP requires an explicit `http://` URL. Search uses
Google. There are four tabs, with independent Chromium navigation histories.
Cookies and site storage are shared between tabs in a browser-specific session.

| Shortcut | Action |
| --- | --- |
| Ctrl+L | Focus and select the address bar |
| Ctrl+T / Ctrl+W | Open / close a tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Ctrl+1 through Ctrl+4 | Select a tab |
| Alt+Left / Alt+Right | Back / forward |
| Ctrl+R or F5 / Escape | Reload / stop |
| Ctrl+Plus, Ctrl+Minus, Ctrl+0 | Zoom in, out, reset |
| F11 | Native fullscreen |
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / cut / paste text |
| Ctrl+Insert / Shift+Insert | Copy / paste text |
| Ctrl+A / Ctrl+Z / Ctrl+Shift+Z | Select all / undo / redo |

Downloads go to `/home/user/Downloads`. The download arrow shows transfer
status. Names are sanitized and existing files receive a numbered name.
The browser profile is separate from Code OSS, under
`/home/port-test/.config/CoastBrowser`. This live OS stores those files in RAM;
they reset on reboot. Copy files to a supported writable mount to keep them.

Only the owner profile (UID 1000) currently launches this runtime. Use the
tested two-CPU, 16-GiB VM defaults. Rendering is software based and JavaScript
uses the port's jitless policy. Full Chrome UI, extensions, password management,
camera/microphone permissions and native file-picker uploads are not provided.
Media playback and every public website have not been qualified.

## Page loading and clipboard

`make run-browser` uses the installed rootless `passt` backend with QEMU
11.1 or newer. The guest keeps its existing address, gateway, DHCP and DNS
forwarding. MTU is explicitly 1,500 bytes to match Coast's Ethernet buffers;
no inbound host ports are forwarded. QEMU starts and cleans up the helper.
See [QEMU's passt options](https://www.qemu.org/docs/master/system/invocation.html#network-options)
and the [passt manual](https://passt.top/builds/latest/web/passt.1.html).
The previous slirp backend did not negotiate TCP window scaling in the
captured connections. Increasing the guest window alone left that bottleneck
intact. To select the old backend explicitly, use
`make run-browser CHROMIUM_QEMU_NETWORK='-nic user,model=e1000e'`.

Coast now negotiates a 1-MiB TCP receive window, retains a finite 128-buffer
receive pool, and uses larger e1000 descriptor rings. Large TCP reads can
return more than 65,535 bytes and refund every byte of receive credit.
The native guest check verifies an exact 8-MiB stream, a read above 64 KiB,
pool exhaustion, refund/reuse and independent UDP allocation. The normal
network suite also exercises TCP/UDP, curl, TLS and Git transports.

A controlled 8-MiB page with 100-ms incoming packet batching took
13.90/13.15 seconds on the original image and slirp. The revised image and
passt took 2.84/1.60 seconds on the same two-CPU, 16-GiB KVM configuration.
These are first and repeated loads with caching disabled, approximately
5–8 times faster for this workload. This does not establish a universal
10-times speedup: public responses and verification pages vary between runs.
The reports and packet captures are in `build/chromium-transfer-baseline-r1`
and `build/chromium-transfer-final-r12`. The earlier
`build/chromium-transfer-passt-r9` run measured 2.72/1.61 seconds with the same
kernel. Google still took 13–20 seconds to finish all resources in later live
runs; these changes do not meet a consistent 10-times target on that site.

The default English spelling dictionary is now bundled with a pinned checksum
and its notices. Chromium uses it normally instead of downloading it after
every RAM-only reboot. Ordinary web resources no longer synchronously call
the main JavaScript process for URL filtering; file requests and frame
navigation retain their restrictions. The resolver avoids optional HTTPS/SVCB
queries that the captured DHCP DNS relay sometimes dropped. Browser software
presentation uses 60 Hz; Code OSS retains its 120-Hz cadence. The browser
advertises its actual Chromium version with the standard Linux-ABI user agent
for website compatibility.

Clipboard transfers use the existing focused native window broker. That
compositor remains root while its Chromium child drops credentials; the
kernel now recognizes its actual window ownership and focus. Renderers and
the runtime do not receive clipboard syscall authority. Native edit shortcuts
explicitly target the focused webpage or address bar. The USB-driven checks
copy Unicode text between the native editor, webpages and address bar, and
exercise cut, Shift+Insert and the native 64-KiB clipboard checks.

OSDev can present a Cloudflare **Verify you are human** checkbox. Reaching
that page is recorded separately from reaching OSDev's content; waiting for
that verification is not counted as a successful site load.

```sh
python3 -B tools/benchmark_chromium.py \
  --iso build/Coast-browser.iso --output-root build/my-browser-benchmark \
  --network-backend passt --live --clipboard
python3 -B tools/benchmark_chromium.py \
  --iso build/Coast-browser.iso --output-root build/my-transfer-benchmark \
  --network-backend passt --transfer-only --rx-delay-us 100000 --network-checks
```

## Build

The builder reuses the selected Coast Electron desktop image and the existing
pinned glibc/Wayland/bridge compiler artifacts. It needs the same local cache as
`tools/build_codeoss_iso.py`; it does not download or pretend to port an
arbitrary Linux browser binary. Missing prerequisites fail explicitly.
The first build downloads the small, checksum-pinned SQLite shared library if
it is absent from `build/chromium-browser-vendor`. NSS loads that library
dynamically for certificate verification. Its upstream copyright notice is
packaged with the browser.

```sh
make chromium-browser CHROMIUM_BUILD=build/my-browser
python3 -B tools/test_chromium_browser.py \
  --iso build/my-browser/Coast-browser.iso \
  --output-root build/my-browser-test --live
make run-browser CHROMIUM_ISO=build/my-browser/Coast-browser.iso
```

Choose an unused build and test directory. The builder preserves the base ISO,
records hashes and kernel flags, and compiles only the native kernel, desktop,
display bridge and native check fixtures. The real Chromium runtime is retained from
the base. Builds reject source changes during packaging. `--bridge-build PATH` selects a verified
source-built Wayland bridge receipt. Kernel flags come from
`ports/chromium/kernel-flags.json`; the native desktop builder is self-contained. `--verify-sandbox` creates a QA
image with the filesystem sandbox's actual kernel selftests enabled.
Those verifier flags also inject later namespace failures deliberately. Use
`tools/test_chromium_sandbox.py` for that image and a normal build for browsing.

## Integration and boundaries

The frontend uses Electron's
[WebContentsView](https://www.electronjs.org/docs/latest/api/web-contents-view)
for each page. Its local controls have a small, sender-checked IPC interface;
remote pages have no preload, Node integration or Electron bridge. Sandbox,
context isolation, web security and certificate verification stay enabled.
Device/notification permission requests are denied. Unsafe address schemes and
page access to local file URLs are blocked. See Electron's
[security guidance](https://www.electronjs.org/docs/latest/tutorial/security).

The browser enables Chromium's actual
[built-in resolver](https://www.electronjs.org/docs/latest/api/app#appconfigurehostresolveroptions)
against the system's DHCP-provided nameservers. The launcher waits up to ten
seconds for the resolver configuration before starting network threads; offline
startup remains bounded. Standard `hosts` and `nsswitch.conf` files are included.
The glibc resolver remains incomplete; the browser does not depend on it.

`OS_ACCOUNT_BROWSER_LAUNCH` admits only the root-owned, non-writable
`/bin/chromium-wayland` bridge. Its inherited network marker permits native IP
only in the admitted UID-1000 browser account before entering a child user/PID
namespace. Page namespaces retain their socket denials. Code OSS receives no
marker and retains its private network. The actual guest fixture checks main
network access, child namespace denial and refusal to grant itself admission.

The sandbox filesystem pool grows from four to sixteen independent states so
new tabs and cross-site navigation can fork renderers. The PID arena grows from
22 to 24 pages; its charged allowance grows from 25 to 27 pages. The classic BPF
pool admits 32 filter objects and 16,384 total instructions, with an unchanged
4,096-instruction limit per program and four-filter chain depth. Its arena uses
49 pages; the combined metadata ceiling is 150 pages (600 KiB). These finite,
charged pools allow independent page filters and cached history renderers.
Task, process, physical-page, virtual reservation and executable-memory policies
retain their existing limits. Exhaustion, refund, slot reuse and rollback checks
exercise the revised capacities.

The Wayland adapter validates the requested content rectangle inside each
bounded SHM buffer and crops using its actual geometry. Chromium's top and
bottom shadows can differ; assuming symmetric margins discarded every paint
after the first blank frame. Resize frames must still match the requested
content dimensions before publication.

Chromium creates its final download placeholder with Linux `creat(85)`.
The kernel now routes that syscall through the existing protected `open` path
with `O_WRONLY | O_CREAT | O_TRUNC`. The guest check verifies creation,
truncation, mode bits, invalid-pointer rejection and denial in `/etc`, alongside
the actual completed download bytes. No success stub or alternate downloader
is used.

The guest harness uses real USB input and checks page/canvas pixels as well as
DOM observations, HTTP requests, file bytes, history, tabs, TLS rejection and
helper cleanup. `--live` additionally checks a public HTTPS page. All servers
used by fixtures bind to host loopback; QEMU accesses them through its ordinary
NAT backend. No host disk is attached. Failed runs retain their reports and
screenshots under their respective output directories.

## Source rebuild coverage

The frontend/desktop/bridge assembler no longer reads historical configuration
or scripts from a developer's Documents directory. It still uses an existing
Electron/Code OSS ISO as the runtime base and a checksum-pinned Debian SQLite
library. Its current behavior is an incremental image assembly, not a complete
fresh-checkout runtime build. The independent Electron source recipe, upstream
pins and remaining full-image bootstrap work are documented in
[source-builds.md](source-builds.md).
