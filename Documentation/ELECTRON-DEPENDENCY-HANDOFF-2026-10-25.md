# Electron preflight A3: runtime inventory, dependencies and pending policy proposal

2026-10-25; dependency closure completed 05:36 UTC. Audit/design milestone only. No OS source/cap/accounting/security setting changed, no new kernel/ISO was built, and no Electron executable was run. A2 remains the latest installed tested release. Browser allocator, trading and zsh checkpoints remain untouched.

## Runtime measurements and smaller-artifact decision

Downloaded the official Linux x64 Electron 43.7.3 and maintained 43.8.3 runtime ZIPs into the workspace. Both archives match both their GitHub release-asset SHA-256 and official SHASUMS256. No GPG signature claim is made. ELF headers and hashes were inspected as data, not executed or altered.

| Runtime | ZIP bytes | Executable bytes | Page-rounded PT_LOAD bytes | Fits 128 MiB? |
| --- | ---: | ---: | ---: | --- |
| Electron 43.7.3 | 127,019,681 | 224,537,848 | 226,131,968 | No |
| Electron 43.8.3 | 127,277,783 | 225,008,888 | 226,603,008 | No |

The 43.7.3 executable hash is exactly the selected VSCodium/Codium hash `b315f26fdb0db1764ae6184125686aa5ed4f92a755abc01d95277b91994eb3c3`. It is the same executable, not a smaller alternative. The 43.8.3 hash is `be79f4206514ab4bcca03fc0752c1cceb8b69c0f8ded32cfcb78953777edbf5d`; it is slightly larger and not established as a drop-in VSCodium replacement.

Both release asset lists contain one full Linux x64 runtime. Chromedriver, mksnapshot, ffmpeg, libc++ objects, debug and symbols are not smaller Electron applications. Other architectures do not match the guest's x86-64 ELF ABI. No old unsupported release was selected merely to fit, and no snapshot tool/headless unrelated browser is presented as the requested port. A component rebuild could change ELF layout, but that needs a large upstream build; none was started.

The readonly fuse wire has version 1, nine states, and RunAsNode enabled at index 0 in the unchanged matching ELF. No fuse was flipped. This supports a future genuine Electron Node-mode diagnostic after prerequisites, not a claim that it has started. Upstream 43.7.3 initialization accepts --js-flags before main-process V8 setup; --jitless remains an explicitly limited configuration, not default JIT or workbench parity.

## Current and planned dependencies

The current A2 initramfs provides six of the 34 direct DT_NEEDED names; the official bundle provides matching libffmpeg.so. Twenty-seven direct names are absent by basename. Presence alone does not prove compatible versions or behavior.

Fresh official Debian Bookworm main/updates/security metadata was downloaded and verified with fingerprint-checked public archive keys in an isolated workspace keyring. All signatures and package-index hashes passed; the security metadata dated October 25 expires November 1. Main is the signed July point-release index; updates/security were checked separately rather than treating that baseline as current security coverage. One initial security verification stopped on an additional missing Debian 11 signing key; the official fingerprint-checked key was added and all signatures then passed. The failure and successful logs are retained. No host keyring was changed.

The planning catalogue has 25 direct provider packages, about 11.2 MiB compressed / 39.1 MiB installed. A conservative declared package/data/tool dependency closure is 174 packages, about 71.4 MiB compressed / 282.9 MiB installed. That is not the minimal ELF closure or an apt lockfile; the auditor is a planning tool and virtual alternatives/contents still need validation. Security-index candidates include updated NSS, expat, PCRE2, libpng, OpenSSL and compression libraries. No older library was deliberately chosen to shrink the runtime.

The direct providers plus gcc-12-base notices were SHA/size verified and data-only extracted under the executor's dependency-sysroot. Maintainer scripts and downloaded programs were not executed; no package was installed on the host or guest. Absolute package symlinks were converted to equivalent in-sysroot relative links with tarfile data filtering retained. Licenses/provenance remain alongside the cached packages.

Actual file/ELF inspection found all 34 direct names in that set plus bundled ffmpeg. All 1,466 strong undefined main-executable symbols were found with their requested versions in direct-library exports; 30 weak main symbols are recorded separately. Direct DSOs map about 27.3 MiB in total. This is static evidence only. At the direct-only checkpoint, 30 transitive SONAMEs were absent; that historical result remains in DIRECT-ELF-AUDIT.json. The follow-up fetched 60 additional signed-index/SHA-verified provider packages (about 11.6 MiB), preserving the original direct libraries. All 100 names in the recursive initial DT_NEEDED graph now resolve to actual verified ELF files.

The complete static audit checked 14,270 strong import records across the main executable and 100 libraries. No required symbol was absent from the aggregate exports, and no required symbol-version name was missing from its named provider. Aggregate page-rounded PT_LOAD size is 276,369,408 bytes including the main ELF (the 256 MiB proposal applies only to main plus interpreter; DSOs also consume the shared charged-page budget). See COMPLETE-ELF-AUDIT.json and TRANSITIVE-LIBRARY-AUDIT.json.

Next dependency work: validate actual dynamic link order, relocations/IFUNC/TLS, constructors, guest APIs and dynamically discovered plugins after the image-policy gate is approved. Stage genuine required data/caches with retained licenses; the package control closure is not a claim that optional plugin/service dependencies are complete. Do not manufacture empty DSOs, fake exports, TLS bypasses or host service stubs. ELF export matching does not prove constructor, thread, event-loop, device or syscall compatibility.

## Other known platform gates

* The 128 MiB main-image check is the first established Electron failure; it still returns EFBIG. See ELECTRON-EXEC-BUDGET-PROPOSAL-2026-10-25.md for the exact pending, default-off hash-pinned 256 MiB combined-image / shared 1 GiB profile-tree charged-page design. Revision 2 makes the aggregate explicit: separate descendant VMs add charges, threads sharing one VM count once, and exec overlap is charged. The initial per-VM draft is retained in workspace history. No part was implemented.
* Standalone Node's saved jitless trace peaks at about 612 MiB under eager backing and page_limit=0. That is not Electron's measured peak. Electron's pointer-compressed/memory-cage configuration may request multi-GiB virtual reservations even in interpreter mode; those will remain unsupported by the 512 MiB mmap bound until separately designed. Do not disable the cage or infer compatibility from standalone Node.
* Full graphics needs real display/input integration: the native copied-bitmap window API is not X11/Wayland/Ozone. Loading client libraries does not create a display server. DBus, device/shared-memory paths and file watchers/timers also need actual implementations/verification.
* Current guest UID/EUID are zero, and Chromium's normal sandbox prerequisites are absent. No fake UID, credentials or --no-sandbox workaround is authorized. A Node-mode runtime check does not validate the graphical multiprocess security model.
* The current kernel explicitly refuses fork/vfork/execve and several native spawn calls from a shared multithread context. Upstream subprocess/zygote behavior needs real ABI verification rather than arbitrary success stubs. Existing native service helpers do not establish Linux process compatibility.

## Pending approval and next bounded step

No build/download is waiting. The remaining action-time decision is the exact opt-in budget/profile proposal. If approved, implement it in a separate candidate with identity, accounting, failure/rollback and prior-regression coverage, then use ordinary exec to attempt the exact upstream runtime and record the first real failure. Do not interpret approval as permission for larger virtual reservations, disabling security, unrestricted caps or a production graphical IDE claim.

Available space remains about 23 GiB; this audit adds only a few hundred MiB. No Chromium checkout/build, global tool install, host security/settings or raw-disk operation was performed. Earlier tested images and the frozen kernel are preserved.

Evidence workspace: `/home/camdenp/Documents/Codex/2026-10-24/task-2/codeoss-app-a3`. Machine-readable profile, artifact/fuse/library audits, signed metadata, scripts and hashes are retained there. The official runtime ZIPs and extracted library data remain workspace caches, not installed OS components.

Primary references: [43.7.3 assets](https://github.com/electron/electron/releases/tag/v43.7.3), [43.8.3 assets](https://github.com/electron/electron/releases/tag/v43.8.3), [exact Electron startup source](https://raw.githubusercontent.com/electron/electron/v43.7.3/shell/browser/javascript_environment.cc), [fuse wire reader](https://raw.githubusercontent.com/electron/fuses/main/src/index.ts), [fuse names](https://raw.githubusercontent.com/electron/fuses/main/src/config.ts), [Debian signing keys](https://ftp-master.debian.org/keys.html), [Electron memory cage](https://www.electronjs.org/blog/v8-memory-cage), [V8 jitless](https://v8.dev/blog/jitless). Static measurements and guest-trace inferences are distinguished above from execution that has not happened.
