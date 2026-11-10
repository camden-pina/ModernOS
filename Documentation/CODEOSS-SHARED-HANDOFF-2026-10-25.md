# Code OSS Core Preview M4: shared model service

Completed 2026-10-25, 02:07 UTC. Continues the delivered M3 graphical editor.
This is the latest active handoff; dated M3/r8/earlier handoffs remain unchanged.

The repeated-window responsiveness defect is mitigated without kernel, access
check or JavaScriptCore library changes. One native JSC service now holds
separate genuine upstream PieceTree models for the GUI session. It stays warm
when all Code windows close, and is stopped/reaped with the GUI. Upstream commit
211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb and all 32 module hashes are preserved.
This remains an original native graphical text-core frontend, not the full
VS Code workbench, Electron/Node, browser Monaco or an extension/language host.

## What changed

- Private IPC protocol v2 validates document IDs as well as sequence/action.
  Each window retains its own queue, model/path/history/selection/save state
  and last-acknowledged display/recovery cache. An ordered shared channel uses
  round-robin scheduling. Limits remain bounded (32 backend registrations,
  256 KiB/document, 64 KiB clipboard, 64 queued actions/256 KiB per window,
  1 MiB aggregate request payload including the in-flight request).
- Actual window close waits for accepted actions, dirty review and an explicit
  model Release acknowledgement before freeing its frontend. Approval-only
  queries, including system power review, never release a model or close it.
  A regression found during review was corrected by putting Release in the
  actual close callback. Cancelled power review was verified on final images
  with one clean and one dirty model: neither released and no power request sent.
- Shared-service failure marks every Code window as failed, retaining each
  last acknowledged snapshot. Recover creates a new unsaved model; other windows
  recover through that same fresh service. Two distinct dirty models were saved
  to separate new files and compared independently. An interrupted active Save
  remains explicitly uncertain. No automatic overwrite or durable crash recovery.
- CoreFS and guest permissions are unchanged. Checked reads/saves, conflicts,
  retained originals and non-clobber Save As continue to work. No credential,
  sudo, UID or host-account/security change was made.

## Cause and measurements

Source: fs/binfmt_elf.c load_elf holds global vfs_mutex while mapping an ELF;
kernel/sched/sched.c task_file_pin takes the same mutex even for a nonblocking
pipe. The worker maps roughly 52 MB of existing JSC/ICU code/data. M3 reloaded
it for every window, so a new editor delayed an existing window's pipe I/O.

A private guest loader probe executed the model with invalid arguments, exiting
before a JSC context/document was created. An unrelated empty nonblocking pipe
read peaked at 105290, 117325 and 92252 microseconds. Adjacent clock pairs were
12-14 microseconds; the small hello control peaked at 5979 microseconds. This
confirms substantial loader-related I/O blocking; it does not decompose every
millisecond of the larger graphical cold-start pauses. Probe source is
ports/codeoss/loader-lock-check.c and evidence is in VALIDATION.json and
qa/shared-native200-01. No kernel instrumentation or allocator investigation.

Matched six-open, 4-vCPU/4-GiB TCG workload:

| Measurement | Preserved M3 | M4 staged | M4 final |
| --- | --- | --- | --- |
| Small-file reopen median, 100% | 1235 ms | 28 ms | 27 ms |
| New empty window | 940 ms | 23 ms | 18 ms |
| Reopen after every Code window closed | 1999 ms | 24 ms | 26 ms |

Final 200% reopens: 88, 81, 78, 82, 78, 80 ms. Final 150% dark small-file
startup was 59 ms; the 218500-byte file became ready in 366 ms (M3 was about
1406 ms in its earlier native test). These are measured guest runs, not timing
guarantees across hosts. Matched baseline is qa/baseline-m3-normal02.

Warm service work per GUI tick peaked at 1 ms in the final 100% and 200%
lifecycle suites and 5 ms in the 150% large-file suite. This is not a whole-frame
rendering measurement. First startup/restart still has a cold pause: final
maximum cold service ticks were 374, 316 and 330 ms respectively. Initial normal
service readiness was 719 ms. The safe mitigation avoids repeating that cost;
it does not remove the frozen loader/VFS lock.

Tradeoffs: one idle runtime retains memory until GUI exit; one engine failure
affects every Code window. Only acknowledged text is cached; unacknowledged input
or a GUI/system crash is not durably recovered. Distinct models/profiles are not
security isolation. Large files still use complete snapshots. No watcher,
cross-process locks, immutable backups, directory fsync or power-loss atomicity.

## Final verification and artifacts

Final release suites passed at 100% light (9 integration groups), 150% dark
(8 groups) and 200% light (9 groups): repeated opens/closes, idle process reuse,
concurrent independent edits, failed-service recovery, exact saved/recovered
bytes, conflict protection, cancelled power review, pointer/controls/menu flows
and full material repaint equivalence. Screenshots were inspected at native size.
The 27 original model checks, 26 shared registry/history/release/file-conflict
checks and the preserved 2000-edit CLI core check passed on the final build.
Protocol v2 passed 100000 malformed frame pairs under ASan/UBSan, including an
explicit document-ID mismatch. LSan disabled under sandbox ptrace; no leak claim.
All owned supervisors/models were reaped, queued payload was zero, and only
private diskless/network-disabled VMs were used. The user's VM was not controlled.

`build/codeoss-shared-m4-20261025/` contains:

- Coast-CodeOSS-Shared-tested.iso, 1280x800 / 100%, 1018224640 bytes,
  SHA256 b049ca2d8dd589626a4f74bc3e79aace3113d1c9b4f5a7eb1546a67ca5f68188
- Coast-CodeOSS-Shared-native-tested.iso, 2560x1600 / 150%, same size,
  SHA256 5112d8ef15f0863f1dab2f777d7dd773a2494454b054b9d9016b94f1b8ce4b87
- MANIFEST.json, PERFORMANCE.json, VALIDATION.json, QA logs, screenshots,
  corresponding source/notices and current application relink objects/commands.
- preservation/final-verification.json: 182 protected paths checked, including
  all M3/r8/prior tested images, original ISO/initramfs, frozen kernel/window ABI,
  JSC/core CLI and paused handoffs. An exact pre-M4 source archive is retained.
  No git metadata exists in this checkout; source/hash checkpoints were used.

Frozen kernel SHA256 remains
9e03c90f99e0d458cf766740e40c4e594b1b3dc55c10293b73d64957f16b04a0.
No prior release was overwritten. Normal permission review approved project
changes/builds; no bypass or outstanding security authority is involved.

```sh
cd /home/camdenp/Desktop/Coast
make run-image O=build/codeoss-shared-m4-20261025 ISO_IMAGE=build/codeoss-shared-m4-20261025/Coast-CodeOSS-Shared-native-tested.iso MEMORY=4G LINUX_DISK= FAT_DISK= SATA_DISK= QEMU_NETWORK='-nic none' QEMU_EXTRA_ARGS=-full-screen
```

Type gui, then Super+Space > Code OSS Core Preview. Ctrl+Alt+Escape leaves the
GUI after unsaved review; close QEMU to exit. Dry-run verified: existing ISO only,
no extra disks/network, separate release serial.log. Guest files without a
writable guest disk remain volatile. Avoid shared writable disks/logs across VMs.

Rebuild: make -C userland codeoss-graphical (existing local tools/JSC only).
Reproduce full lifecycle/model/power-cancel tests with a unique output label:

```sh
python3 tools/test_codeoss_shared_desktop.py --iso /absolute/tested.iso --label unique-label --scale 100 --model-checks --power-review
```

Use --scale 150/200, --dark as desired; the harness controls only its own private
VM. Do not use --loader-check unless running the private diagnostic image with
that optional fixture. Source locations: desktop/codeoss.c and codeoss_view.h,
include/codeoss_protocol.h under userland; userland/codeoss/model-worker.cpp;
ports/codeoss/graphical-model.ts; build/test helpers under tools. See active
Documentation/CODEOSS-GRAPHICAL.md for limits and build details.

Next planned step: incremental snapshot/layout transport for larger files and
integration of genuine upstream editor/workspace services. A full workbench still
requires a presentation/runtime route, platform/file-watching services and
language/extension hosts. Removing the initial cold pause would require a
careful loader/locking design that preserves executable-file consistency; none
was attempted here. Browser allocator and trading remain paused. Disks stays
read-only; host settings/raw disks untouched. Terminal Ctrl-Z/stop/resume remains
outstanding; Bash r8 Ctrl-C behavior and earlier Terminal/editor releases remain.
Screenshots are local; no upload/Library retry was attempted.
