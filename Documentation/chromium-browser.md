# Chromium browser for Coast

Build with `make iso`, boot with `make run`, and enter
`gui` at the Coast console. Super+B opens Chromium. Super+Space also opens
Spotlight; search for Chromium Browser. Reopening the application restores its
running window.

Coast uses Electron 43.7.3 / Chromium 150.0.7871.250 with the project's
Linux-compatible ABI profile, native display bridge, networking and renderer
sandbox. Page rendering and JavaScript run inside Coast. The browser uses
software rendering and the port's jitless execution policy.

## Navigation

The address bar accepts HTTP/HTTPS URLs, hostnames and search terms. Hostnames
default to HTTPS; use an explicit `http://` URL for plain HTTP. Search uses
Google. Four tabs maintain independent navigation histories and share the
browser's cookies/site storage for that session.

| Shortcut | Action |
| --- | --- |
| Ctrl+L | Select the address bar |
| Ctrl+T / Ctrl+W | Open / close a tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Ctrl+1 through Ctrl+4 | Select a tab |
| Alt+Left / Alt+Right | Back / forward |
| Ctrl+R or F5 / Escape | Reload / stop |
| Ctrl+Plus / Ctrl+Minus / Ctrl+0 | Zoom in / out / reset |
| F11 | Native fullscreen |
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / cut / paste text |
| Ctrl+Insert / Shift+Insert | Copy / paste text |
| Ctrl+A / Ctrl+Z / Ctrl+Shift+Z | Select all / undo / redo |

Downloads go to the profile's Downloads folder. The download arrow reports
progress; names are sanitized and existing files receive a numbered name.
The browser's session/profile is separate from Code OSS. Live-system storage
is RAM-backed, so copy files to a supported writable mount before shutdown.

## Network and clipboard

`make run` uses QEMU user networking with an e1000e device and needs
no network helper. The guest obtains DHCP/DNS configuration. TCP receive
accounting supports a 1 MiB window with a finite buffer pool; the network
suite checks exact HTTP/chunked/redirect bodies, TLS rejection and Git
transports.

For a host with QEMU's native passt backend, select that network explicitly.
This example also selects UEFI, which needs suitable host OVMF:

```sh
make run FIRMWARE=uefi QEMU_NETWORK='-nic passt,model=e1000e,ipv4=on,ipv6=off,mtu=1500,address=10.0.2.15/24,gateway=10.0.2.2,map-host-loopback=10.0.2.2,tcp-ports=none,udp-ports=none'
```

That launch uses the installed rootless passt backend, a 1,500-byte MTU and no
inbound port forwards. QEMU owns the helper lifetime. See [host setup](host-setup.md),
[QEMU network options](https://www.qemu.org/docs/master/system/invocation.html#network-options)
and the [passt manual](https://passt.top/builds/latest/web/passt.1.html).
Network throughput and website resource/verification behavior vary by server
and host configuration; controlled fixture measurements do not establish a
universal public-site speedup.

The image includes pinned fonts and an English spelling dictionary. Ordinary
web resources use Chromium's network stack; frame navigation and file URL
requests retain the port's restrictions. Browser software presentation uses
60 Hz. The user agent reports the actual Chromium version and Linux ABI.

The focused native window broker performs shared UTF-8 clipboard operations.
Chromium renderer processes receive no direct clipboard syscall authority.
The guest checks use actual USB input to copy Unicode text among the native
editor, webpage and address bar, and cover cut, Shift+Insert and the 64 KiB
native clipboard limit.

## Scope and verification

The runtime currently launches in the owner profile (UID 1000). Use a 16 GiB
VM. Full Chrome UI, extensions, password management, camera/microphone
permissions and native file-picker uploads are outside this port's supported
scope. Arbitrary media playback and every public website are not qualified.
A public site's verification challenge is recorded separately from loading
that site's content.

The guest suite checks actual painting, HTTP/JavaScript, forms/input,
history, tabs, fullscreen, TLS certificate rejection, exact downloads and
helper cleanup. Reports bind results to the selected ISO hash:

```sh
python3 -B tools/test_chromium_browser.py --iso build/desktop-source-wide/image/Coast-desktop.iso --output-root build/browser-check
```

The complete build, source provenance, library closure and qualification
commands are in [source builds](source-builds.md). Project source is at
https://github.com/camden-pina/ModernOS.git.
