# Coast Terminal Jobs R9 — 2026-10-25 03:03 UTC

Completed the requested Terminal stop/resume milestone on /home/camdenp/Desktop/Coast. Source modifications, project build and final release/handoff installation passed normal permission review. The first copy was rejected before execution on superseded read-only scope; one reviewed retry with clarified authorization succeeded. No bypass occurred. This is a separate kernel candidate; build/vmlinux, original ISO, M4/M3/r8 and every earlier tested release are preserved. No git checkpoint/commit was made; exact source/hash checkpoints are in the release preservation directory.

## What changed and why

The frozen-kernel baseline actually returned ENOTSUP for SIGSTOP/TSTP and had no stopped wait events. PTYs had no VSUSP path and the scheduler had no stopped state. This was independent of the paused browser allocator problem. R9 adds default job-control signal handling, safe thread-group stop rendezvous, live stopped/continued wait4 events, SA_NOCLDSTOP, unblocked-thread routing, SIGCONT even when masked/ignored, and killing stopped children. Kernel mutex/pin/transaction owners are not abandoned; syscall cleanup or explicitly lock-free timing waits reach the parking boundary.

PTY Ctrl-Z targets the foreground job group. Background reads and mode changes stop with TTIN/TTOU and retry after authorized fg; TOSTOP output is opt-in. Ignored/blocked TTIN/TTOU stay denied, preserving the previous access policy. The existing parent-owned Bash foreground transfer, terminal/session/return-capability checks, PID 1 protection and web-role signal limits remain. No credentials, sudo, UID/authentication policy or host security settings changed.

The first candidate's GUI test found sleep exiting immediately after bg because a default stop leaked EINTR. Pure nanosleep and clock_nanosleep now retain their original deadline across repeated stops. Ordinary signal handlers keep existing restart/frame rules. PTY VQUIT support is also wired; a complete termios/full-screen compatibility claim is not made.

Fresh R9 images default to real GNU Bash 5.3.20. Saved per-profile shell choices are retained. Existing profiles choosing the native shell can select GNU Bash in Terminal > Preferences and open a new Terminal. The native shell lacks fg/bg, so VSUSP remains disabled there; this was checked in an actual migrated-preference guest. Terminal's child preferences, editor toolbar/settings, read-only Disks and genuine Code OSS graphical text-core preview remain inherited from M4/r8. This is not the complete VS Code workbench, and zsh/fish/Oh My Zsh support has not been added.

## Verified final artifacts

Installed, hash-verified release directory: /home/camdenp/Desktop/Coast/build/coast-terminal-jobs-r9-20261025. The workspace copy remains at /home/camdenp/Documents/Codex/2026-10-24/task-2/terminal-r9/release.
- Coast-Terminal-Jobs-tested.iso: 1280x800, 100% default; SHA256 faaf5d0b31c7d083db339641e3bb443f88ef7766c380997108b4ff18aa7f4e7d.
- Coast-Terminal-Jobs-native-tested.iso: 2560x1600, 150% default; SHA256 8fbde45b2db6efda6611732730464282d7d2398e6273568b9ffda464caa22512.
- Each ISO is 1018314752 bytes. vmlinux-r9 SHA256 3893bc0d58daaf1e56400b7b5062dd3cfa43012dd948339504217b367eac87b4.
- Original frozen build/vmlinux SHA256 9e03c90f99e0d458cf766740e40c4e594b1b3dc55c10293b73d64957f16b04a0 remains intact.
- terminal-r9-corresponding-source.tar: 861 entries, inherited M4 GNU/JSC/Code OSS sources/notices plus exact current kernel/integration sources, lwIP and Limine notices, native GUI relink objects and test/build scripts. SOURCE-MANIFEST.json identifies every regular entry. No public upload was performed.

The checkout kernel was rebuilt from scratch into an isolated O= output, with a complete native GUI compile under -Wall -Wextra -Werror. Existing unrelated lwIP type-limits and PCI volatile warnings remain. Final software-emulated QEMU checks used owned diskless/offline 4GiB VMs only; no host raw disk or user-owned VM was controlled. KVM was unavailable and physical hardware was not tested.

Final checks:
- 428 original fixture checks passed on each of 1 and 4 CPUs, plus core USERLAND CHECK PASS. These include stop/continue event consumption, killed stopped child, ignored/caught/blocked signals, SA_NOCLDSTOP, repeated three-thread stops, TLS/FPU/unblocked-thread routing, and relative/absolute timing deadlines.
- Ten GUI job-control groups passed at normal 100% and native 200%, and through the installed project test tool. Repeated Ctrl-Z/jobs/bg/fg/Ctrl-C, foreground pipeline, TTIN input retry, TTOU mode retry, access denials, parent-controlled TOSTOP transfer, nested Bash, nano unsaved-buffer/suspend/resume/resize/save, and close/reopen cleanup all passed. After GUI close, ps showed no leaked Terminal/job process. Nano 8.7 uses Ctrl-T then Ctrl-Z to suspend, then fg.
- Ctrl-Z observation measured 0.126–0.131s including emulated input and test overhead. This is not a whole-frame or physical-hardware latency claim.
- Existing ten-group Bash regression passed at native 150%, including repeated Ctrl-C, stdin, pipeline, substitution, background wait, rejected takeover, nested shell, genuine upstream Code OSS CLI byte round trip and nano save/resize.
- Nine shared graphical Code OSS lifecycle/recovery groups plus upstream model/file checks passed: distinct models, repeated open/close with concurrent edits, canceled power review, own-service failure and independent recovery/save. No unreaped service or queued bytes remained. Cold first/recovery load still has the separate known loader/VFS pause; R9 does not fix that.
- 22 Linux compatibility cases were observed: all supported cases passed, FIFO creation remains the already documented ENOSYS limitation. Baseline and development failure receipts are retained separately.
- 317 protected paths match expected hashes, with only the nine intended existing source edits; four new source/test/build/doc files were added. Original kernel, all existing native binaries/release images and paused handoffs are preserved. All 70 collected release files are covered by SHA256SUMS and passed complete workspace and destination hash audits. The separate Documentation handoff matches the release copy.

## Run and reproduce

cd /home/camdenp/Desktop/Coast
make run-image O=build/coast-terminal-jobs-r9-20261025 ISO_IMAGE=build/coast-terminal-jobs-r9-20261025/Coast-Terminal-Jobs-native-tested.iso MEMORY=4G LINUX_DISK= FAT_DISK= SATA_DISK= QEMU_NETWORK='-nic none' QEMU_EXTRA_ARGS=-full-screen

Enter gui at the guest console, then Super+T opens Terminal. Fresh guests use GNU Bash. Existing profiles retain their shell setting; choose GNU Bash in Terminal > Preferences and open a new Terminal for jobs/fg/bg.
Try sleep 30, Ctrl-Z, jobs -l, bg %+, fg %+, Ctrl-C. Nano 8.7 uses Ctrl-T then Ctrl-Z to suspend.
Native image: 2560x1600, 150% default. Normal image: 1280x800, 100%; replace ISO_IMAGE filename above with Coast-Terminal-Jobs-tested.iso. Validation also covers native 200%.
Ctrl+Alt+Escape returns to the guest console after unsaved-work review; close QEMU to exit. This command only launches the existing image, with no attached disks or networking. Guest files are volatile without a writable guest disk. Avoid sharing the same output/log directory between concurrent user VMs.
The original build/vmlinux and all earlier release images remain unchanged. R9 is a separate kernel candidate.

Build a new separate candidate with tools/build_terminal_job_release.py; see Documentation/TERMINAL-JOB-CONTROL.md for required cached sources/toolchain and an explicit immutable base ISO. The builder never replaces the frozen kernel or base image. A generated file's name alone is not a test claim: run tools/test_terminal_jobs.py --iso IMAGE --work-dir PRIVATE-OUTPUT --label UNIQUE --scale 0 (3 is 200%) and inspect receipts. Release VALIDATION.json and qa/ contain final logs, with actual screenshots under screenshots/.

## Remaining bounds and next step

The requested Bash Ctrl-Z/jobs/fg/bg path is working in this candidate. Full Linux/POSIX parity is not established: native-shell fg/bg, orphan-group HUP/CONT, ptrace/waitid, complete termios, stop/power-management interaction and restart_syscall semantics for every timed poll/select/futex wait remain unsupported or unverified. Such timed waits may return EINTR; retry-aware applications remain necessary. Ignored/blocked TTOU mode changes intentionally retain stricter denial than Linux. Next planned focused kernel step is a timed poll/select/futex stop/continue matrix, before extending orphan-group lifecycle behavior or claiming full job control.

Implementation, build, validation and project release installation are complete. No current approval or technical blocker prevents this bounded release. Guest files on the supplied diskless launch are volatile. Browser allocator investigation and trading remain paused; their handoffs were not changed. Host accounts/settings/disks were untouched; Disks is still read-only. No password/sudo enforcement or new persistent privileged access was created. Library delivery remains unavailable and was not retried; artifacts are local only.


Final installation verified 2026-10-25 03:08 UTC: /home/camdenp/Desktop/Coast/build/coast-terminal-jobs-r9-20261025 and /home/camdenp/Desktop/Coast/Documentation/TERMINAL-JOBS-HANDOFF-2026-10-25.md. Original protected files and every copied delivery hash passed. No approval is pending.
