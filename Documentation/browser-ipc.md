# Modern engine process/IPC facilities

These facilities now support the native WebCore/JavaScriptCore browser. The
trusted desktop launches a Soup broker, which starts an immutable-origin page
renderer. Top-level origin changes replace that renderer; same-origin pages
retain WebCore's history. Local file associations retain NetSurf and its existing
restricted profile. Cross-origin frame routing and several advanced web APIs
remain unavailable; public-site compatibility is still being validated.

The resource/service channel uses bounded 64-byte v1 metadata packets and sealed
memfd bodies. HTTP/verified-HTTPS HTML, CSS, scripts, PNG and fetch go to real
WebCore loaders; CORS/preflight is enforced there. Extended 1/4-CPU fixture
checks include genuine pixels, mouse/editing/backspace, wheel rendering and form
POST. Native toolbar/pixels/navigation, cookies/HttpOnly/credentials, UTF-16
storage/quota, frames, script history, downloads, resize and lifecycle checks
are recorded separately in [AI-HANDOFF.md](AI-HANDOFF.md). Component or READY
success is insufficient evidence of correct public-site rendering.

Broker cookie services expose only DOM-visible cookies; Soup attaches HTTP
cookies and retains HttpOnly values. Storage requests are confined to the
renderer's canonical origin. SQLite owns local persistence using its real
`unix-dotfile` VFS; window-local session storage survives origin switches. A
bounded history service restores original request method/body across renderer
replacement. The broker strips Set-Cookie response headers from renderer IPC.
No renderer receives network or private filesystem handles, and the legacy
NetSurf sandbox has not gained authority.

## Explicit process capabilities

`spawn_web(409)` takes path, argv and the 40-byte `os_spawn_web` specification.
This is a separate ABI from the preserved 40-byte browser surface protocol.
Only unsandboxed trusted code may launch a broker. A broker may launch only a
renderer; a renderer cannot launch processes or grant itself privileges.
Helper images are explicitly restricted to `/bin/web-renderer` (native WebCore renderer), `/bin/jsc`, `/bin/glib-check` and `/bin/web-capability-check` (controlled
development components/checks). The last three are not browser engines with DOM.

The kernel creates a fresh ELF image, VM, stack, context and sanitized environment.
It inherits exactly a full-duplex Unix IPC endpoint at fd 0 and two write-pipe
endpoints at fds 1/2. No extra file descriptors, broker memory, saved locks,
environment overrides, display or input handles are inherited. This launch works
while broker threads are live. Ordinary multi-threaded fork/vfork/exec/spawn
remain rejected. `web_identity(410)` reports immutable role, TGID, broker domain
and canonical origin. Threads share these capabilities and remain on one CPU.

Broker paths permit installed fonts/component fixtures, explicit CA/resolver
files, entropy/null/zero, its canonical HOME/.config/webkit and HOME/Downloads.
Outbound IPv4 is allowed; listeners, mounts, display/input, arbitrary file access
and ordinary process creation remain denied. Renderer paths permit only installed
fonts/component fixtures and entropy/null/zero. Network socket creation, private
files, certificates, downloads, local grants and process launches are denied.
Both profiles permit the real pthread/futex, IPC, memory and event-loop subset. A broker's only foreign signal authority is SIGKILL
of a live renderer child with matching parent/domain/role, validated under the
scheduler lock. Renderer, unrelated-child and process-group kills are denied.
Final broker death terminates its renderer children before orphan reparenting.
The legacy NetSurf allowlist has not gained clone, IPC, memfd or epoll authority.

Native renderers have a 768-MiB mapped-page budget; brokers and legacy windows
retain 512 MiB. `memory_usage(411)` returns only the calling process's current,
peak and limit bytes. WebCore's memory-footprint backend reads those real
counters. Peak RSS is also tracked by `getrusage` and `wait4`, including reaped
child accounting. The current eager VM gives every mapping physical backing.

Read-only self `prlimit64(302)` reports the same real limits as `getrlimit`.
This is required by musl's stack discovery and prevents WebKit's GC from using
an uninitialized stack bound after an EPERM. Limit writes and foreign queries
remain denied. `raise`/`tkill` may signal a thread in the calling thread group;
the scheduler checks the resolved target TGID under its lock before allowing
a restricted process to signal another task.

The 40-byte surface protocol also carries bounded retained-canvas copies and
paint completion. A copy is confined to this window's initialized canvas; both
source and destination are checked by the broker and desktop before applying
overlap-safe row copies. The renderer retains its real Skia raster and applies
WebCore's scroll operations, including its exposed-strip and fixed/sticky
invalidations. Consecutive moves in the same direction and viewport can merge.
Painting is clipped to the invalidated regions; the transport mirrors the exact
backing moves and compares the actual painted pixels in those regions. Complex
damage falls back to a bounded full frame.

The desktop stages native copy/damage packets without changing the displayed
canvas. `BROWSER_PRESENT` commits the complete paint on the desktop thread.
The queue is limited to 256 packets and 64 MiB plus 64 KiB of metadata. A new
canvas size must begin with a complete frame. `BROWSER_CANCEL_PAINT` (type 9,
empty payload and zero fields) discards an incomplete paint when the trusted
broker replaces or stops a renderer; it retains the last displayed image.
Local NetSurf windows retain their existing complete-packet presentation.
The actual shared copy/queue/commit/cancel routines have ASan/UBSan checks.

Pixel pipes have a 256-KiB capacity; the kernel refuses capacity changes by page
renderers. A writer thread drains immutable prepared output while DOM/JSC stay
on the main thread. A genuine five-second JavaScript task verifies that an
already prepared paint completes during the task. Persistent scrollbars avoid
overlay fade redraws in this CPU desktop.

Renderer origins must be normalized lower-case ASCII HTTP(S) host names with an
explicit decimal nonzero port, bounded to 255 bytes. URL paths, userinfo, query,
fragments, trailing dots, empty labels and invalid ports are rejected. Engine IDNA,
IPv6, opaque/file origins and registrable-domain decisions are not implemented by
this parser. Each launch creates a fresh process with an immutable origin; there
is no API to retarget that process to another origin. The native broker uses this policy for top-level navigation. Cross-origin
frames are denied until separate renderer routing is implemented. Legacy local
windows still have one NetSurf process across their navigation history.

IPC endpoints and memory objects belong to a broker domain. Renderer-created
channels/counters/memory objects are bound to their creating TGID. Broker-created
objects/endpoints/counters become bound to the first renderer granted access.
Duplication, close, queued transfer and thread creation retain these labels.
Passing the resource to a different renderer is refused even within the same
broker domain. Broker file/network handles cannot enter a renderer through
SCM_RIGHTS. Denied or truncated ancillary rights set MSG_CTRUNC; WebKit's checked
upstream IPC receiver rejects that flag. The broker must still validate messages,
URLs/origins, surfaces, cookies, downloads and local-file decisions before using
its own authority. The native Soup resource and cookie/storage broker enforces
these checks; foreign document-frame routing remains unimplemented.

## Unix transport and shared memory

AF_UNIX `socketpair` supports stream, datagram and seqpacket endpoints, blocking
and nonblocking transfers, vectors, peek, record/ancillary truncation, shutdown,
names, SO_TYPE/SO_ERROR/bounded-buffer getters, FIONREAD/FIONBIO and poll readiness.
It works independently of a NIC. Bounds: 128 pairs globally, 32 records and 64 KiB
queued bytes per direction, 16 vectors and 64 descriptor attachments per send.
Large receiver control buffers are accepted, including upstream WebKit's buffer.
Sender descriptors are pinned across waits/reuse; queued rights own references;
receive installs actual descriptors with MSG_CMSG_CLOEXEC support. Peeking
duplicates rights. Discarded/truncated/denied rights and closed queues release
their references. Unix reference cycles are rejected with ELOOP rather than
leaking objects. Named/abstract connect/bind/listen, credentials, arbitrary socket
options and MSG_WAITALL remain unsupported. Passing an epoll object is rejected
to prevent mixed IPC/watch reference cycles. No complete Linux Unix ABI is claimed.

`memfd_create(319)` implements anonymous memory-object descriptors, checked
ftruncate, read/write/pread/pwrite/fstat/lseek and real seal/seal-shrink/seal-grow/
seal-write fcntl operations. MAP_SHARED maps the object's actual physical pages;
MAP_PRIVATE is an independent snapshot. Coherent aliases work across separate
address spaces and CPUs. Each shared PTE retains its object through descriptor
close, fork, protection changes, partial unmap and final VM destruction.
Writable mapping counts prevent premature write sealing; later mmap/mprotect
cannot regain sealed write permission. Shared/executable aliases are denied.

Bounds: 128 MiB per object, 128 MiB of owned object backing per renderer group,
512 MiB per other creator group and 512 MiB globally. Accounts survive creator
exit while objects are mapped or transferred. The existing 512-MiB mapped-page
quota separately counts every renderer/broker mapping, image and stack. Imported
objects are charged to their creator and to the importing VM's mapping quota.
Live mapped-object shrink returns EBUSY: cross-CPU invalidation/SIGBUS-on-shrink
is not implemented. Shared mremap, anonymous MAP_SHARED, general coherent file
mappings, huge/executable memfd flags and process-shared futex aliases remain
unsupported. Ordinary existing file mappings retain snapshot behavior.

## Event loops and dependencies

epoll_create/create1, epoll_ctl, epoll_wait and epoll_pwait implement bounded
level/one-shot readiness, 128 watches per object, round-robin return ordering,
timeouts, hangup and temporary signal masks/interruption. Watches retain actual
file descriptions across dup/close/reuse and disappear with their last external
reference. Regular files/memfds are rejected as non-pollable. Nested epoll and
edge/exclusive/wakeup modes are explicitly unsupported. Gio's real Unix stream
pollability probe uses these operations; its main loop uses the existing poll
and eventfd facilities.

`tools/build_webkit_deps.py` builds upstream GLib 2.86.5, GObject/Gio, libffi 3.5.2
and PCRE2 10.46 statically against musl in build/modern-browser/prefix. It reuses
the real POSIX C++ runtime and target zlib, preserves all SDK/JSC/NetSurf caches,
verifies pinned archives and uses incremental stamps. PCRE2 JIT and libffi's
Linux static executable trampoline mechanism are disabled. Real libffi calls,
Unicode regex and async/main-loop behavior are tested; executable closures,
dynamic Gio modules/TLS backends and full WPE rendering are not claimed.

```sh
make webkit-deps
make test-webkit-deps
make test-browser-ipc
```

The test targets build components **before** completing ISO packaging and booting
VMs. Never rewrite an ISO used by a running guest. Guest `ipc-check` exercises
shared memory, separate-process coherence, seals, FD transfer/truncation/peek,
600 queued-close lifetimes, blocked close/reuse and epoll masks/events.
`web-capability-check` launches concurrent origins from a threaded broker, checks
malicious main/worker confinement, sanitized inheritance, memory quotas, foreign
signals and cross-origin resource denial, then launches real JSC/agents and Gio
under renderer capabilities. `glib-check` exercises genuine cross-thread GLib
dispatch/TLS, GObject generic libffi signal marshalling, PCRE2 Unicode regex,
Gio Unix/control-message transport, pollable streams and async completion.

These checks passed in actual UEFI guests with 1 and 4 CPUs. Logs are under
build/modern-browser/{ipc-guest-tests,glib-guest-tests}.log and
build/runtime-tests/uefi-{1,4}cpu-{browser-ipc,glib}.log. Existing native browser
pixel/input and certificate regressions are reported separately in the handoff.
The last public-site and 2560×1600 drag failures remain unresolved.

Linux-numbered subset semantics were checked against primary
[recvmsg documentation](https://man7.org/linux/man-pages/man2/recvmsg.2.html),
[Unix sockets documentation](https://man7.org/linux/man-pages/man7/unix.7.html)
and [memfd documentation](https://man7.org/linux/man-pages/man2/memfd_create.2.html).
