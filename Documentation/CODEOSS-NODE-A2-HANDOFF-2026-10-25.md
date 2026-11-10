# Genuine upstream application port: A2 mapping fix and established W^X boundary

2026-10-25 04:43 UTC; installation status updated after the approved retry at 04:50 UTC. Continue the genuine upstream VSCodium/Code OSS port. No custom editor frontend was expanded. A2 completes a bounded mmap ABI correction and precise default-V8 diagnosis. It does not provide a running default-mode Node, Electron window or workbench.

Installation status: one normal reviewed retry was approved; the five prepared source/test/documentation files were installed at 04:50 UTC and all 1,011 protected paths verified. The earlier denial executed nothing and cited obsolete read-only scope. No bypass was used. Release collection/destination verification follows in the final audit. The unrelated W^X policy boundary below remains unresolved.

## Correction to A1's preliminary explanation

A1's attribution of Node's initial 512 MiB reservation failures to a 512 MiB per-process quota was incorrect. A2's numeric guest trace shows this ordinary command-line Node process has `page_limit=0`. Existing 512/768 MiB budgets are applied to sandboxed roles, not every command-line process. No quota was disabled by A1 or A2.

The first rejected hint was `0xe4d000`, inside the unchanged Node executable's RX PT_LOAD segment (starts `0x73d000`, size `0x28b3bb4`). The old mmap code treated this non-fixed hint as a required address. A 512 MiB fallback mapping at a kernel-selected address then succeeded. At the later permission failure, the process owned 286,012 charged pages, about 1,117 MiB. The exact original trace and a structured diagnosis are retained.

The subsequent fatal failure is independently established:

```
APP_A2_VM pid=13 nr=10 addr=22af40000 len=1ffc0000 prot=7 ... result=-22 pages=286012 limit=0
```

V8 requests `mprotect(PROT_READ|PROT_WRITE|PROT_EXEC)` over 511.75 MiB of code space. The kernel rejects it under its existing W^X rule. Address/size validation succeeds; the requested simultaneous write and execute permissions cause EINVAL. Upstream OS::SetPermissions then checks `if (ret != 0) CHECK_EQ(ENOMEM, errno);` and fails because EINVAL is not ENOMEM. No errno was changed to hide this assertion. This is not the paused browser heap/allocator issue.

## Completed safe ABI change

Only mmap placement changed in `kernel/sched/syscall.c`:

* Non-fixed hints are rounded down. A free valid hint can be used; occupied, below-minimum or out-of-range hints fall back without replacing existing mappings.
* Fallback searches from mmap_next, with at most 1 GiB of page-table probing per call. The caller's maximum allocation remains 512 MiB. Existing user-address bounds, resident accounting, per-role budgets and all sandbox/file/W^X checks are unchanged.
* MAP_FIXED addresses remain mandatory; invalid addresses are rejected. MAP_FIXED at zero no longer silently substitutes mmap_next. No MAP_FIXED_NOREPLACE, live VMA backing or new reservation accounting is claimed.

The diagnostic candidate confirms the formerly rejected occupied 512 MiB hint now falls back successfully. Default V8 still reaches and fails the same RWX request. Numeric tracing was removed from the final kernel. The official Node 24.18.0 binary and A1's pinned glibc/sysroot remain byte-for-byte unchanged.

## Security boundary and next step

The stopped portion is enabling default execution of this unchanged Node binary. Accepting its RWX mprotect request, globally or through a Node exception, would weaken kernel W^X and permit simultaneously writable/executable code pages. That change was not implemented or submitted for approval; no reviewer denied an action. It requires explicit action-time security-policy authorization if pursued. Changing an error code, suppressing the V8 assertion or adding --no-sandbox is not a fix.

See `CODEOSS-WX-PORT-ASSESSMENT-2026-10-25.md` for the completed source-backed feasibility assessment. Prefer assessing a genuine upstream V8/Node platform adaptation using the already supported RW-to-RX transitions, with correct concurrent code-generation behavior. PKU/pkey support or executable shared aliases are separate security designs, not assumptions that may be silently enabled. A source port is distinct from unmodified-binary compatibility. No large runtime source build was attempted.

Virtual-reservation accounting would reduce eager physical backing, but it would not solve the established RWX incompatibility. No accounting policy was changed in A2. Any later reservation design must keep finite reserved-address limits and physical-page quotas separately enforced, including fork, threads, failure rollback and unmap. Do not simply raise/remove caps. Codium's ~216 MiB ELF still exceeds the unchanged 128 MiB image budget; display/Ozone, dependencies and a sandbox-preserving Electron process model also remain outstanding.

## Tests

* A1 baseline reproduced rejection of occupied and unaligned non-fixed hints.
* Final clean kernel passed 43 guest placement assertions on 1/4 CPUs: overlap/partial overlap, unaligned/free/invalid hints, fixed replacement and invalid fixed addresses, allocation-cap/unknown-flag negatives and actual RW-to-RX execution with rejected RWX preserving prior permissions.
* Actual selector source compiled under ASan/UBSan/leak checking passed modeled search exhaustion, last-page fit, alignment and overflow tests. This is search-work exhaustion; it is not mislabeled as physical RAM exhaustion.
* The existing isolated process-sandbox C test verified each role-constrained 300 MiB mapping succeeds, a second exceeds the 512 MiB quota and fails ENOMEM, and unmap permits allocation again. It also retained filesystem/descriptor/syscall denials. No browser engine or paused allocator debugging ran.
* On 1/4 CPUs: A1 EOF guards, epoll capacity, Node version/jitless arithmetic, native userland/POSIX, 427 job-control assertions, dynamic musl/C++ and historical shared text core passed. Normal Node remains explicitly recorded as blocked.
* Graphical Bash regressions passed all ten groups at both native and 200% scale, covering repeated jobs/fg/bg/Ctrl-C, pipelines, nested shells, TTY access guards, nano suspend/save restoration and closure. Reports and screenshots accompany the release.

All tests use owned disposable QEMU TCG guests with no added disks/network. Host disks/accounts/settings and the user's VM are untouched. A1, R9, all earlier releases, original frozen kernels and paused zsh/browser-allocator/trading checkpoints are preserved. Source and release destination hash audits accompany the final collection receipt. Only the intended source/documentation changes are allowed; all prior releases and frozen kernels remain protected. The old A1 handoff and release remain immutable; this dated handoff supplies its corrected diagnosis.

## Artifacts and reproduction

Project release directory: `build/codeoss-mmap-a2-20261025`. Executor collection: `/home/camdenp/Documents/Codex/2026-10-24/task-2/codeoss-app-a2/release`. The final receipt records destination verification.

* `Coast-CodeOSS-Mmap-A2-tested.iso`: 1,382,541,312 bytes; SHA-256 `eb54871502e2c174ada43aaf494115dadaca26b612d9590a9a527140f51ee304`.
* `vmlinux-a2`: SHA-256 `fdd9bd71ced2b3f3ca13d83e0e2ae9e3c673bc0d7cf30d35152e0e2783161668`.
* Patch, original source checkpoint, complete kernel/diagnostic source archive, exact VM traces, provenance, test scripts/reports, SHA256SUMS and final audit receipt.

Run the installed guest verifier:

```sh
python3 -B tools/test_codeoss_mmap_hints.py \
  --iso build/codeoss-mmap-a2-20261025/Coast-CodeOSS-Mmap-A2-tested.iso \
  --output-root /tmp/coast-a2-checks --label a2-4cpu --cpus 4
```

To inspect manually without rebuilding:

```sh
cd /home/camdenp/Desktop/Coast
qemu-system-x86_64 -accel tcg -cpu max -M q35 -smp 4 -m 8G \
  -bios firmware/ovmf/OVMF.fd \
  -cdrom build/codeoss-mmap-a2-20261025/Coast-CodeOSS-Mmap-A2-tested.iso \
  -vga none -device VGA,vgamem_mb=32,refresh_rate=120 \
  -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-mouse,bus=xhci.0 \
  -nic none -serial stdio -monitor none -no-reboot -display gtk
```

At the prompt run `mmap-hint-check`, or `gui` for the preserved desktop. Closing QEMU ends the disposable session. Guest edits are temporary. The machine/firmware/device configuration was exercised headlessly; GTK is the existing desktop presentation option. No default image is overwritten.

Primary sources: [exact Node/V8 platform code](https://raw.githubusercontent.com/nodejs/node/v24.18.0/deps/v8/src/base/platform/platform-posix.cc), [V8 code-range setup](https://raw.githubusercontent.com/nodejs/node/v24.18.0/deps/v8/src/heap/code-range.cc), [exact upstream flags](https://raw.githubusercontent.com/nodejs/node/v24.18.0/deps/v8/src/flags/flag-definitions.h), [Linux mmap hint semantics](https://raw.githubusercontent.com/mkerrisk/man-pages/master/man2/mmap.2). Upstream sources explain intent; the retained guest traces establish the specific failures above.
