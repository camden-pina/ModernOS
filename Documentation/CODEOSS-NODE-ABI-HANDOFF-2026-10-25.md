# Code OSS application prerequisite A1 handoff

2026-10-25 04:22 UTC; final collection/audit follows in the release receipt. Current task: port a genuine upstream VSCodium/Code OSS application, keeping its workbench. No further custom editor frontend development was done. A1 is a runtime prerequisite, not an IDE release.

## Verified result

The unchanged official Node 24.18.0 Linux x64 executable now starts on Coast. `--version` returns `v24.18.0` with exit 0. With its explicit `--jitless` option it evaluates JavaScript (`3`), writes/reads/renames/removes its own guest temporary file, runs a timer and completes an asynchronous read through libuv. Verified on 1 and 4 CPUs under QEMU TCG, 8 GiB guest RAM, no added disks and no network. This standalone binary matches the selected VS Code source build pin; it is not Electron's embedded Node and does not establish Electron compatibility.

Official archive SHA-256: `55aa7153f9d88f28d765fcdad5ae6945b5c0f98a36881703817e4c450fa76742` (matched Node's HTTPS-published SHASUMS256, no signature claim). Unmodified executable SHA-256: `41a74efb34cbde5c7632cdac0cf8bd1a14d0b8d73dc1e82755014d9a9ce70f5c`. glibc `2.36-9+deb12u14`, libgcc/libstdc++ `12.2.0-14+deb12u1` are from Debian's signed exact Bookworm main index; three InRelease signatures, index checksum, package size/checksums verified. Data-only extraction, no host installation/scripts. gcc-12-base copyright data was added from that same signed index. Notices are inside the final image and beside the receipts.

## Implemented in five kernel files

* Accept Linux's ignored MAP_DENYWRITE/MAP_EXECUTABLE and advisory MAP_STACK flags, without accepting other unknown flags or granting access permissions.
* Permit private regular-file snapshot mmap spans to extend past EOF, as glibc's DSO loader expects. Partial EOF tails remain zero; whole EOF pages are allocated/charged but hardware-inaccessible and raise SIGBUS/BUS_ADRERR on permitted accesses. Permission failures remain SIGSEGV. mprotect, fork, munmap, MAP_FIXED, copyin/out and close/unlink lifetime are covered. No live file growth/truncation tracking; remapping EOF-guard ranges returns ENOTSUP. memfd bounds/executable restrictions stay intact.
* Accept larger epoll output capacities while retaining the 128-watch table and 128-record stack buffer. libuv's actual epoll_pwait(maxevents=1024) previously failed EINVAL.

No ELF budget, 512 MiB allocation limit, per-role page budget, W^X rule, sandbox/file authorization, UID behavior or host security setting changed. The original frozen kernel and every earlier release are preserved. Failed-call instrumentation used in diagnostic candidates was removed from the final kernel.

## Remaining blockers, observed versus inferred

Default Node `-e` is not working. Diagnostic candidate04 logged two 512 MiB PROT_NONE|MAP_NORESERVE requests returning ENOMEM. Existing mappings already consume part of the unchanged 512 MiB per-process page budget, and Coast eagerly backs PROT_NONE mappings. V8 subsequently fails an errno assertion in OS::SetPermissions and terminates. The failed permission-call arguments were not traced; do not claim a specific W^X violation is established. No JIT workaround that relaxes protections was applied. `--jitless` is an honestly limited interpreter result; it is not default runtime parity.

The unchanged VSCodium 1.140.06880 `codium` ELF still returns execve errno 27/EFBIG before startup: ~215.7 MiB mapped image exceeds 128 MiB image budget. No attempt was made to bypass validation by manually invoking ld.so on it. No Electron window/workbench has launched. GTK/NSS/other dependencies, display/Ozone integration and a sandbox-preserving process model remain outstanding from A0. Full Chromium source build was not attempted with roughly 25–35 GiB free space. Browser-hosted/remote variants were not substituted.

Next planned step: design bounded virtual reservation versus committed-page accounting and investigate the exact V8 permission-call failure, preserving physical resource enforcement and W^X. Separately propose an application-specific executable-budget/process-policy design for review before any security-sensitive enforcement change. Then test the actual pinned Electron executable; only after a real window is supported should the upstream workbench be attempted. Do not expand M3/M4 to simulate it.

## Validation and preservation

Final image smoke checks passed on 1/4 CPUs. Mapping fixture: 4,157 assertions, including byte-by-byte EOF-tail checks, fault siginfo, fork/permissions and negative cases. Epoll fixture: 49,176 assertions, predominantly buffer-canary bytes, plus output capacity, pwait, timeout, bad mask/buffer, overflow and unsupported-edge checks. These are assertion counts, not tens of thousands of separate scenarios.

Native userland, POSIX, 427 job-control primitive assertions, dynamic musl driver, shared C++ ABI and the historical shared text core passed on 1/4 CPUs. Existing ELF loader host harness passed 27 valid/malformed cases with ASan/UBSan/leak checking. Linux compatibility supported cases passed offline; existing FIFO ENOSYS remains documented. Graphical Bash suite passed all ten groups at native and 200% scale, including pipelines, Ctrl-Z/fg/bg, Ctrl-C, nested shells, background TTY access denial and nano buffer/save restoration. Native/HiDPI screenshots were inspected.

An initial 200% GUI test failed because its fixed 0.2-second delay saw the background reader still Running. The retained workspace harness now waits up to eight seconds for the actual Stopped state; all original assertions remain. No product change was made for that harness race. An initial async-read fixture targeted absent /etc/hostname; it was corrected to the packaged Node license file and passed on both CPU counts. Initial failures and corrected-run logs are retained.

Final license collection adds only a copyright/documentation directory: all 8,677 existing initramfs members are byte/mode/link-identical to the fully regressed candidate. Final smoke tests then reran against the new ISO hash. Every one of 435 pre-A1 protected project paths is audited before installation; only the five intended kernel sources may change. Prior release files, original build/vmlinux, A0 handoff and paused zsh/browser-allocator/trading work remain unchanged. No user VM was touched.

## Deliverables and launch

Project release: `build/codeoss-node-abi-a1-20261025/Coast-CodeOSS-Node-ABI-tested.iso` (1,382,486,016 bytes), SHA-256 `b15525bdc75bb86513d7fe1fdf5291c70f3b944b0cac8379fec19368d81f742d`.
Separate kernel `vmlinux-a1`, SHA-256 `bcd1c69db0d429584e7541df2a762916045b0b5cd30e1cd06f3a67bae6324e70`. The dated handoff, kernel patch/source, provenance/licenses, all QA logs and SHA256SUMS accompany the release. Executor workspace remains `/home/camdenp/Documents/Codex/2026-10-24/task-2/codeoss-app-a1`.

Launch the tested artifact without rebuilding or attaching host disks:

```sh
cd /home/camdenp/Desktop/Coast
qemu-system-x86_64 -accel tcg -cpu max -M q35 -smp 4 -m 8G \
  -bios firmware/ovmf/OVMF.fd \
  -cdrom build/codeoss-node-abi-a1-20261025/Coast-CodeOSS-Node-ABI-tested.iso \
  -vga none -device VGA,vgamem_mb=32,refresh_rate=120 \
  -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-mouse,bus=xhci.0 \
  -nic none -serial stdio -monitor none -no-reboot -display gtk
```

At the guest prompt run the commands in `ports/codeoss-app/runtime/README.md`, or `gui` for the preserved desktop. Ctrl+Alt+Escape leaves the desktop; closing QEMU ends the disposable session. No extra writable disk is attached, so guest file changes are temporary. Do not use make run for this inspection because its iso dependency can rebuild. The VM machine/firmware/device recipe was exercised headlessly; GTK presentation is the existing supported desktop option.

Primary references: [official Node archive](https://nodejs.org/dist/v24.18.0/), [VS Code Node pin](https://raw.githubusercontent.com/microsoft/vscode/1.140.0/.nvmrc), [glibc DSO mapping](https://sourceware.org/git/?p=glibc.git;a=blob;f=elf/dl-map-segments.h;hb=glibc-2.36), [Linux mmap semantics](https://raw.githubusercontent.com/mkerrisk/man-pages/master/man2/mmap.2), [Linux epoll bounds](https://raw.githubusercontent.com/torvalds/linux/v6.12/fs/eventpoll.c), [V8 jitless explanation](https://v8.dev/blog/jitless), [Debian key fingerprints](https://ftp-master.debian.org/keys.html). These explain upstream intent; the guest logs establish the actual observations above.
