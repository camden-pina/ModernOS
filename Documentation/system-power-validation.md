# Coordinated power capability and validation

This milestone is deliberately narrow. ACPICA advertisement, mapping readiness,
asynchronous request acceptance and physical completion are separate facts.
`Documentation/system-power.md` describes the shell, desktop and ABI.

## Capability matrix

| Capability | Implementation and evidence | Limit |
|---|---|---|
| S5 poweroff | BSP preparation with IF=1; software drain, AP rendezvous, checked filesystem/device flush, terminal controller stop, IF=0 ACPICA entry. QEMU SHUTDOWN with `guest-shutdown` observed on 1/AHCI and 4/NVMe CPUs/storage. | QEMU evidence, not physical-platform certification. Arbitrary firmware preparation can block or have irreversible side effects. |
| FADT reset | Same coordinated path, prepared FADT backend, no triple-fault fallback. QEMU `guest-reset` observed on 4/NVMe and 1/AHCI fixtures. | VM uses `-no-reboot`, so QMP reports SHUTDOWN reason `guest-reset`; do not require a RESET event or confuse it with S5. |
| Persistence | Real open, dirty 128 KiB FAT writes; every supported writable filesystem/root device must supply sync/flush. Terminal fixtures check read-only fsck, exact bytes/hash and a new boot reopening the disk. | No general power-loss atomicity or forced-unmount claim. Filesystems/devices without the required hooks fail closed. |
| AP quiescence and rollback | Generation-tagged IPI request; safe IF=0 atomic polling loop, intact interrupted stacks/CR3/GS/FPU; RELEASE observation/ack required before another epoch. Host actual-code tests plus 4-CPU dry-run, missing ACK and post-park flush failure guests. | Missing release/GPE-restore acknowledgement leaves `recovering`, blocking new transitions. Physical APIC/x2APIC coverage is not established. |
| Resource/lifecycle admission | Aggregate native mutex and transaction accounting under IRQ guards; owners, dependent vfork child and firmware workers continue. Actual scheduler predicates, actual mutex/coordinator host tests and a live guest vfork dependency. | No generic spin-unlock parking; scheduler raw stack-switch handoff remains intact. New workers/drivers must add appropriate transaction boundaries. Not every driver concurrency interleaving is exhaustively tested. |
| AC/battery snapshots | Bounded BSP-only worker evaluates standard methods. Real SSDT fixture covers AC, BIX/mWh, BIF fallback/mAh, absent bay and unknown sentinels; real Notify 0x80/0x81 refreshes 50% to 60% with coalescing. Host actual parser gets malformed/type/bounds/sentinel fuzzing. | Optional telemetry only. EC/GPIO/GenericSerialBus methods can fail honestly; no bus backend, runtime estimate, charging control or namespace hot-add policy. |
| Shell/desktop | Cached `acpi`/`battery`/`power`, asynchronous `poweroff`/`reboot`/cancel; genuine System & Power application, bounded diagnostics and readiness gating. Actual GUI USB-input test covers dirty editor review/cancel, accepted pending cancel, real rollback and saving preserved bytes. | Reliable interactive cancel is the pending window. Applications/input pause once frozen. Other applications do not all provide dirty-state hooks. |
| Fixed power button | SCI acknowledges/logs event, cached counter opens explicit desktop confirmation; repeated events coalesce. Existing ACPICA suite sends three events each on 1/4 CPUs. | Never automatic terminal action; physical lid/sleep-button policy absent. |
| Driver shutdown | AHCI, NVMe, xHCI, e1000, AC97 and HDA contain bounded preflight/terminal stop paths and retain DMA backing buffers. QEMU storage/xHCI paths exercised. | igc and external AW88399 configurations explicitly reject coordinated actions. Network/audio stop paths and physical controllers need dedicated device/hardware validation. |
| Authorization | Browser sandbox and renderer/broker roles denied; no arbitrary AML syscall. | Trusted native single-user tasks may request power. This is not a multiuser security policy. |
| Suspend/thermal/CPU policy | Explicitly unsupported. | No S1/S3/S4/resume, EC/GPIO/GenericSerialBus handlers, thermal/fan policy, CPU C/P/T/CPPC policy or PCI `_PRT` integration. |

## Safety boundaries

- Runtime mappings are immutable. `acpi_os_get_runtime_status()` readiness is
  necessary, never a guarantee that arbitrary `_PTS` methods will work.
- All AML/status-refresh work stays on BSP0. Ring-3 status queries copy cached
  snapshots and never evaluate AML on an AP.
- The OSL idle/admission transaction holds its work lock while checking both
  ordinary rings, every reserved GPE dispatch/finish slot and all five active
  workers. Raw diagnostics are not used as an idle predicate.
- The final FREEZING-to-PREPARING transition and cancel decision share the
  control lock. Every accepted cancellation before that boundary prevents
  preparation/commit. The pending grace starts on the requester's first
  post-cleanup arm; duplicate requests cannot move it.
- No persistent writer is admitted between final freeze/flush/commit. On any
  reversible failure, RELEASE is published before reopening admission, then all
  targeted AP release observations are awaited. COMMIT alone permits terminal
  halt/controller changes. Failure after that point never resumes applications
  against half-stopped hardware.
- The five-second drain deadlines cannot interrupt a synchronous firmware
  `Wait(event, 0xFFFF)` inside preparation. No live AML owner is force-aborted.
  `acpica-power-blocked.asl` is an optional disposable test of this limitation;
  host teardown is never described as cancellation or recovery. A duplicate
  firmware `_PTS` causing fixture rejection is not a passing blocked-prep test.
- `AcpiTerminate` is not used by the power path. Software/AP recovery does not
  prove reversal of firmware `_PTS` side effects; status exposes the attempt.

## Reproducing focused tests

Only use disposable guests and explicitly selected read-only inputs. The power
harness validates ELF/tar/SSDT/config inputs, hashes every source, rejects output
collisions, verifies source integrity and records effective kernel/initramfs/ISO
hashes. `--base` is a packaged output tree with `iso_root/`, `initramfs.tar` and
`vmlinux`; `--kernel` can select its matching final-image kernel explicitly.
No original-checkout path is inferred. `--validate-only` does not build or boot.

Host checks (single-job where tools invoke a compiler):

```
make -j1 test-acpi-host test-power-host test-kernel-memory test-efi-loader test-boot-config
python3 tools/test_power_harness.py
python3 tools/test_power_workload.py
```

Build the small fixture programs against the already-built native libc/crt and
`userland/user.ld`, or supply equivalent explicit `--workload`, `--thread-check`,
`--ipc-check`, `--shell` and optional `--vfork-workload` binaries. Do not use a
full-port or full-userland build to obtain these small fixtures.

```
python3 tools/test_power.py --base "$BASE" --kernel "$KERNEL" --memory 384M \
  --kind dry-run --cpus 4 --transport ahci \
  --vfork-workload "$VFORK" --telemetry-aml "$TELEMETRY_AML"
```

Repeat with `omit-ack`, `flush-fail` (NVMe), `drain-busy`, and `cancel`; all are
reversible/error cases. Terminal `poweroff`/`reboot` runs must instead require
real QMP guest event/reason plus storage persistence, and reopen the disk in a
fresh guest. The harness never sends a QMP shutdown/reset command to prove its
own terminal test.

The updated terminal fixture first advertises its read-only verifier capability,
so stale binaries fail before any write or power request. After the fresh boot,
`power-workload PATH verify` checks all 131072 bytes and exact EOF without
rewriting the file. Reports require `reopened_bytes_verified=131072` and
`reopened_eof_verified=true`. Earlier reports without these fields established
host-side exact `mtype` content/fsck and fresh-guest directory visibility only.
The verifier's actual-C host checks and the updated guest terminal cases require
a new run; source generation or mocked host orchestration is not that evidence.

```
python3 tools/test_power_desktop.py --iso "$DRY_RUN_ISO" --gui "$GUI" --memory 384M
python3 tools/test_power_blocked.py --iso "$NORMAL_ISO" --aml "$BLOCKED_AML" --memory 384M
python3 tools/test_acpica.py --iso "$NORMAL_ISO" --cpus 1 4 --events 3 --memory 384M
python3 tools/test_acpica_negative.py --iso "$NORMAL_ISO" --memory 384M
```

Resource admission is the caller's responsibility. Run one guest at a time;
never overlap a constrained heavy compiler merely because host MemAvailable
looks adequate. The final integrated full image must repeat these tests against
its actual kernel/initramfs. A lean fixture pass is not a full-browser-image pass.

### Final full-image command runner

`tools/run_power_validation.py` prints a shell-quoted, serial 14-command matrix
by default. Printing does not inspect images, build anything or start a guest.
Select every final binary explicitly and use the actual release ISO, not the
earlier lean milestone image:

```
python3 tools/run_power_validation.py \
  --base "$FINAL_OUTPUT" --iso "$FINAL_ISO" --kernel "$FINAL_KERNEL" \
  --shell "$FINAL_SHELL" --gui "$FINAL_GUI" \
  --workload "$POWER_WORKLOAD" --vfork-workload "$POWER_VFORK" \
  --thread-check "$THREAD_CHECK" --ipc-check "$IPC_CHECK" \
  --telemetry-aml "$TELEMETRY_AML" --iasl "$IASL" --memory 2G \
  > full-image-power-commands.txt
```

The matrix deliberately separates three kinds of evidence:

- Six **direct release** commands boot the supplied ISO byte-for-byte unchanged:
  S5/reset on 1-CPU AHCI and 4-CPU NVMe, pending cancellation, and desktop S5.
  The helper extracts only for inspection and checks the embedded kernel,
  shell and GUI against the explicit binaries; it records the real ISO,
  initramfs, kernel and boot-configuration hashes and rejects test boot tokens.
  No fixture program, AML or boot option is injected. Storage tests create a
  real guest file on disposable FAT, close it without explicit sync, require
  the matching QMP guest event/reason and flush/AP/commit phases, run read-only
  fsck and reopen the exact bytes in a fresh boot. The cancel case alone uses
  an explicit sync for teardown and receives no terminal-action credit. The
  desktop case cancels dirty-editor review, then approves a real S5 request;
  uniquely named screenshots include the foreground System & Power controls
  and diagnostics, plus the dirty-editor confirmation. This is not a browser
  interaction test or a claim that every bundled application has been run.
- Six **fixture-enriched full-initramfs** commands use the final full archive
  as their base, preserving the renderer and all unrelated entries. Four small
  shell/workload/thread/IPC entries, plus the vfork fixture, are replaced or
  added. The desktop copy also replaces `bin/gui` with the explicitly selected
  GUI. Separate disposable Limine ISO copies carry fault-injection tokens and
  a smaller display mode; rollback commands inject the bounded telemetry SSDT.
  These exercise dirty **open** 128 KiB persistence, vfork dependencies,
  telemetry/Notify and post-rollback application recovery. Their reports hash
  both source and effective kernel/initramfs/ISO. They are not unchanged-ISO
  evidence, even if all replacement release binaries happen to be identical.
- Two **ACPICA regression** commands preserve supplied kernel/initramfs bytes
  and use configuration-only ISO copies plus synthetic AML: both 20-stage CPU
  configurations, then three negative controls. The inert negative control
  boots the unchanged source ISO. Each command starts its guests serially.

The 14-command matrix does not include the four additional enriched open-dirty
terminal cases: run `test_power.py` with `poweroff` and `reboot`, each on
1-CPU/AHCI and 4-CPU/NVMe, against the same final full base/kernel and rebuilt
workload. These supplement the unchanged-ISO closed-file cases rather than
replacing them.

The enriched fixture path currently requires a compatible Limine `iso_root/`
template in `--base`. Before execution, the runner also proves that
`--base/initramfs.tar` hashes identically to the archive embedded in the actual
supplied ISO, preventing a stale lean base from being labeled full-image.

For admitted execution, repeat the same command with `--execute`,
`--exclusive-ready`, `--guest-marker "$OWNED_MARKER"` and
`--min-available-kib "$ADMITTED_FLOOR"`. These are explicit caller decisions:
the memory floor is not automatically inferred from `--memory` and the flag
does not pause a compiler. Release competing work first. The runner atomically
owns the marker for the serial matrix, checks MemAvailable before each command,
stops on the first error, and removes only its own marker. The default 2 GiB
guest size is configurable; 384 MiB was sufficient only for the earlier lean
fixture, not an assertion about the full renderer image.

Plans/provenance and unchanged-ISO case reports are written under
`build/power-release-tests/`; existing fixture/ACPICA report locations below
remain unchanged. Each terminal case includes a second boot to verify storage,
so 14 commands do not mean 14 guest boots. The new runner has host-only mocked
tests in `tools/test_power_release_host.py`; actual final full-image execution
is a separate pending release gate and is not part of the lean checkpoint.

## Evidence artifacts

`build/power-tests/report.json` preserves historical failures and successes;
new runs contain complete input manifests/effective boot hashes. Guest serial
logs are under `build/runtime-tests/`. Desktop screenshots/report are under
`build/runtime-tests/power-desktop-*` and `build/power-tests/desktop/`.
`build/acpica-tests/report.json` and `negative-controls.json` retain the existing
20-stage and negative-control results. Host summaries are under
`build/power-tests/*host*.log`. A delivery summary must identify the final tested
kernel hash and any later source changes requiring a rerun.

An earlier 256 MiB storage fixture failed before ACPI initialization. A final
matched comparison with identical initramfs/config/storage reproduced the exact
`PANIC: physical memory exhausted (32768 pages)` on both the frozen baseline and
feature kernels. That is a boot-memory fixture limit, not a shutdown/rollback
result. Logs and hashes are preserved in `build/power-tests/memory-256/`.
Successful power feature tests use 384 MiB.

## Isolated milestone checkpoint

Final tested kernel SHA-256:
`1d76b8e1889427c55f5e271bb02df23c36736d7f54a219a26c129279ab92dc8b`.
The final exclusive single-guest suite completed on 2026-10-14 UTC. All nine
power scenarios, the real desktop test, the synchronous blocked-preparation
fixture, both 20-stage ACPICA CPU configurations and all three guest negative
controls passed. Every final power run reports read-only inputs unchanged.
Host helpers and all nine portable-harness regression cases passed. This is the
isolated lean milestone; main/full-browser image integration and repeats remain
separate release gates. Exact commands, per-run manifests and artifact hashes
are delivered with the feature-only patch.
