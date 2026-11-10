# Genuine VSCodium / Code OSS application port: A0 baseline

Updated 2026-10-25 03:34 UTC. The user's corrected target is the upstream VSCodium/Code OSS application, retaining its workbench and features. The original Coast PieceTree frontend (M3/M4) is preserved historical text-core work and does not satisfy this target. No further custom frontend expansion is part of this port.

## Actual upstream artifact and first guest failure

Inspected the full official VSCodium Linux x64 desktop release 1.140.06880, based on upstream VS Code 1.140.0 according to its release notes. The 212,538,680-byte download matches GitHub's official asset SHA-256 `61223b74f9e40834117311ec4177cb386d09b84c1aae4f9e674c96580569cef3`. No GPG signature claim is made. The ELF itself is unmodified, SHA-256 `b315f26fdb0db1764ae6184125686aa5ed4f92a755abc01d95277b91994eb3c3`, 224537848 bytes, with 226131968 bytes of page-aligned PT_LOAD mappings (~215.7 MiB). Coast's image budget is 134,217,728 bytes (128 MiB). An owned guest's execve of this exact ELF returned errno 27 (EFBIG), before the upstream runtime started. This is a real loader rejection, not an IDE launch or a successful port.

The artifact product commit is `807ba2d6075c29587f884698cd7370c81ab0724c`; it did not resolve at that path in microsoft/vscode. Do not label it as a verified Microsoft source commit. Upstream VS Code tag 1.140.0 pins Node 24.18.0 for building and Electron 43.7.3 in .npmrc; the actual guest runtime versions were not obtained. The older text-core archive is a different 1.144.0 checkout and is not the selected stable application release.

## Established dependencies and constraints

The kernel already has validated PT_INTERP/ET_DYN loading, AT_PHDR/TLS/RELRO metadata support and a musl dynamic loader. It is incorrect to call it static-only. The official ELF requests `/lib64/ld-linux-x86-64.so.2`, which is absent in the tested guest. The ELF has 34 direct DT_NEEDED libraries, including glibc, GTK3, GLib/GIO, NSS/NSPR, D-Bus, X11, GBM, xkbcommon and audio. Only libffmpeg.so was found in the official bundle and libgcc_s.so.1 by basename in the R9 initramfs. 32 direct-library names were absent from both inventories. Presence alone does not establish compatible symbol versions or runtime semantics; transitive dependencies still need resolution. No host libraries were copied into the guest and no untrusted ldd/executable was run on the host.

Coast's mmap bound is 512 MiB per mapping; a 4 GiB PROT_NONE reservation returns EINVAL. mmap currently commits/maps eagerly instead of providing a general sparse-reservation VMA model. V8 uses large virtual cages in common Chromium configurations. The exact selected runtime's V8 mapping sequence is not yet executed, so the probe establishes a missing large-reservation capability rather than the exact later failure. Private RW-to-RX succeeds and anonymous RWX is denied; this does not justify disabling W^X. Shared executable memfd mappings are denied by current source policy and need careful runtime investigation, not a blind permission change.

There is no X11/Wayland server, corresponding socket or DRI device in the tested image. Current GUI apps are native. Coast does expose an owned copied-bitmap/input window ABI v1 (4 slots, max 1024x768). It is not an X11 or Wayland protocol endpoint and is not a drop-in Ozone implementation. A genuine native route can implement Chromium's Ozone PlatformWindow, software surface, screen, cursor, clipboard and input adapters against this owned interface while retaining the upstream workbench. Alternatively, a compatibility route needs a real guest display-protocol server and the Linux library stack. Neither display route is implemented yet.

Chromium's Linux sandbox uses namespaces/setuid and seccomp-BPF facilities. Read-only prctl/seccomp queries return ENOSYS here; inspected clone flags also do not implement Linux namespaces. UID/EUID are hardcoded 0 in the current guest, without authenticated user separation. Existing Coast web-role restrictions are not demonstrated equivalents of Chromium's sandbox. No sandbox-disabling flag, setuid helper installation, credential, account or security-enforcement change was used. A sandbox-preserving native backend or properly enforced compatibility implementation is a prerequisite to shipping the full application.

The first preflight measured approximately 37 GiB free; after validating the reproducible image builder, approximately 35 GiB remained. Chromium's upstream build guide asks for at least 100 GB free; Electron refers to Chromium's build requirements and supports clang. Host RAM is sufficient for a small probe, but this storage is insufficient for a normal full Electron/Chromium checkout/build while preserving all releases. Node/npm and Rust/cargo are absent from the inspected PATH; Python, clang, ninja and the existing project musl compiler are present. No host-global install or full runtime rebuild has begun.

## Guest observations, not a full runtime test

Owned QEMU TCG guest: 4 CPUs, 8 GiB, R9 kernel, no extra disks/network. The preflight ISO stages an original probe and only the exact upstream codium ELF; full app resources are not staged. It is not a runnable editor image. The full official archive remains in the workspace for inspection. No browser allocator investigation or custom text-core frontend was launched.

| Probe | Result | errno |
| --- | ---: | ---: |
| reserve_4GiB_PROT_NONE | -1 | 22 |
| anonymous_RWX_denied | -1 | 22 |
| private_RW_to_RX | 0 | 0 |
| inotify_init1 | -1 | 38 |
| timerfd_create | -1 | 38 |
| signalfd4 | -1 | 38 |
| eventfd | 0 | 0 |
| PR_GET_NO_NEW_PRIVS | -1 | 38 |
| PR_GET_SECCOMP | -1 | 38 |
| SECCOMP_GET_ACTION_AVAIL | -1 | 38 |
| AF_UNIX_socketpair | 0 | 0 |
| AF_UNIX_transfer | 0 | 0 |
| memfd_create | 0 | 0 |
| memfd_shared_alias | 0 | 0 |
| pthread_create_join | 0 | 0 |
| glibc_interpreter_present | -1 | 2 |
| musl_interpreter_present | 0 | 0 |
| proc_self_status | -1 | 2 |
| dev_shm | -1 | 2 |
| X11_display_socket | -1 | 2 |
| Wayland_display_socket | -1 | 2 |
| DRI_device_directory | -1 | 2 |
| official_codium_ELF_execve | -1 | 27 |
| upstream_child_wait_status | 0 | 0 |

The probe checks current primitives needed by real runtimes: Unix socket transfer, shared memfd aliases, eventfd, pthread creation/join and private RW-to-RX all succeeded. Missing timerfd/signalfd/inotify/proc paths are observed gaps; whether a particular runtime can use a fallback still needs real execution. A missing optional API is not automatically the first mandatory launch blocker. The current first launch blocker is the ELF image budget, followed by absent glibc runtime/dependencies. No kernel bound or access check was changed.

## Reproduce the groundwork

`tools/audit_codeoss_application.py --application-root EXTRACTED-OFFICIAL-BUNDLE --output AUDIT.json` inspects the real ELF, package/native dependencies and current kernel image limit without executing the application on the host.

`tools/build_codeoss_app_preflight.py --base-iso build/coast-terminal-jobs-r9-20261025/Coast-Terminal-Jobs-tested.iso --codium-executable EXTRACTED-OFFICIAL-BUNDLE/codium --output-root NEW-OUTPUT` uses the cached compiler/Limine to compile the guest-only contract, extracts the original kernel from that same immutable base and creates a new diagnostic image. It does not fetch, rebuild a runtime, install globally, overwrite an existing output or boot a VM. Boot only in an owned disposable VM with no added disks/network and run `codeoss-app-contract /opt/vscodium/codium`. Expected gaps are observations; CODEOSS_APP_CONTRACT_END does not mean an IDE works.

Source/receipts live in the executor workspace `codeoss-app-a0`; the full official bundle is `vendor/VSCodium-linux-x64-1.140.06880.tar.gz`. Source provenance, direct dependencies and original guest logs are retained. R9, M4/M3/r8 and every earlier release stay unchanged. zsh was safely checkpointed at `zsh-r10/CHECKPOINT.json`: signed upstream 5.9.2 built, parent-controlled handoff/Ctrl-C proved, ZLE adaptation and selector UI staged but not fully guest-validated or installed.

## Next genuine milestones

1. First runtime milestone: a pinned, unmodified upstream Electron executable reaches version/Node-mode startup inside Coast with checked file I/O, worker threads and event loop behavior, using an appropriate C runtime and memory model. This is a runtime prerequisite, not the full editor. Design the executable/reservation budgets with existing per-role limits and W^X preserved; do not simply raise global caps to make a banner print.
2. Establish a sandbox-preserving Electron process model and a real Ozone/display/input integration. Then render an upstream Electron test window through the native compositor. A headless dump is a diagnostic result, not a visible desktop port.
3. Run the unchanged upstream Code OSS/VSCodium workbench, then verify editor/file round trips, native file watchers, node-pty terminal, extension/language hosts, search, settings and multiple windows. Native modules must target the exact Electron ABI. The present original Text Editor and PieceTree preview are separate products.

Browser-hosted Code OSS and remote development are distinct alternatives and are not substituted for the requested local application. No cloud build, upload, external account action or remote workstation was used. No passwords/sudo/host security settings or raw disks were changed. Trading and the browser allocator task remain paused.

Primary sources checked:

- [VSCodium stable release and notes](https://github.com/VSCodium/vscodium/releases/tag/1.140.06880), [official release API](https://api.github.com/repos/VSCodium/vscodium/releases/latest).
- [Upstream source Node pin](https://raw.githubusercontent.com/microsoft/vscode/1.140.0/.nvmrc), [Electron pin](https://raw.githubusercontent.com/microsoft/vscode/1.140.0/.npmrc).
- [VSCodium build requirements](https://github.com/VSCodium/vscodium/blob/master/docs/howto-build.md).
- [Electron Linux build guidance](https://www.electronjs.org/docs/latest/development/build-instructions-linux), [Chromium build/storage requirements](https://chromium.googlesource.com/chromium/src/+/main/docs/linux/build_instructions.md).
- [Chromium Ozone port interfaces](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/ozone_overview.md).
- [Electron sandbox behavior](https://www.electronjs.org/docs/latest/tutorial/sandbox), [Chromium Linux sandbox](https://chromium.googlesource.com/chromium/src/+/main/sandbox/linux/README.md).
- [V8 pointer-compression layout](https://v8.dev/blog/pointer-compression). This explains a common large-reservation need; it does not establish this exact binary's runtime sequence.

Final tool verification 2026-10-25 03:39 UTC: the installed project audit matches the independently inspected ELF. The installed preflight builder reproduced the exact R9 kernel and codium hashes; its owned guest reproduced all 25 observations and closed normally. Sources/tools/docs passed normal review and a 393-path preservation audit. Evidence is saved at `/home/camdenp/Desktop/Coast/build/codeoss-application-a0-20261025`. No major runtime rebuild or new editor release is claimed.
