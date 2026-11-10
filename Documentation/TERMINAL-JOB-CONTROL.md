# Terminal job control: R9, 2026-10-25

This separate kernel candidate adds real task stopping and continuation for the existing GNU Bash/PTY Terminal. It retains r8's parent-controlled terminal handoff. It does not replace the original build/vmlinux, M4/r8 or any earlier release image. Previously saved terminal-shell preferences remain authoritative; choose GNU Bash in Terminal > Preferences and open a new Terminal to use jobs/fg/bg. Fresh R9 guests default to GNU Bash. The native Coast shell still has no fg/bg implementation, so its Terminal leaves the suspend character disabled.

## Mechanism

ISIG PTY input recognizes VINTR, VQUIT and VSUSP. Ctrl-Z sends SIGTSTP to the actual foreground process group. Default SIGSTOP/TSTP/TTIN/TTOU request a group rendezvous: user frames park at scheduler boundaries, blocked kernel operations wake and clean up, and no mutex/descriptor transaction owner is abandoned. A parent sees one stopped event after every live thread has parked. SIGCONT wakes a stopped group even when ignored or blocked; SIGKILL terminates stopped children. wait4/WUNTRACED/WCONTINUED consumes live notifications without reaping the process. SA_NOCLDSTOP suppresses stop/continue SIGCHLD notifications, not wait events. The process-directed signal receiver can be an unblocked member of the same thread group.

A default stop restarts interrupted read/write/wait operations after syscall cleanup. Pure nanosleep/clock_nanosleep waits park without losing their original monotonic deadline; stopped time counts toward that deadline. No application handler or synthetic EINTR is manufactured for these default stops. Real signal handlers retain the existing kernel-owned return frames and restart rules.

Background reads stop with SIGTTIN before consuming input; background terminal mode/flush changes stop with SIGTTOU. TOSTOP is opt-in for output. Ignoring/blocking TTIN/TTOU still denies the protected operation with EIO/EPERM, rather than granting access. TIOCSPGRP's existing session, controller, direct-child and return-capability checks, PID 1 protection and web-role signal limits remain. No UID, credential, sudo, host account or security policy changes are included.

## Checks and limits

`job-control-check` is an original guest-only, owned-child fixture. It covers stop/continue/wait consumption, killed stopped children, caught/ignored/masked signals, SA_NOCLDSTOP, repeated three-thread stop rendezvous, unblocked-thread signal routing, TLS/FPU state, and relative/absolute sleep deadlines. TTY subcommands check background read/mode denial, foreground retries and TOSTOP output. It refuses to run on a non-Coast host.

`tools/test_terminal_jobs.py --iso IMAGE --work-dir PRIVATE-OUTPUT --label UNIQUE --scale 0` runs the actual GUI through emulated input, using an owned diskless, network-disabled VM. Scale indices 0/1/2/3 mean 100/125/150/200%. It tests repeated Ctrl-Z/jobs/bg/fg/Ctrl-C, a two-process pipeline, background input/mode stops, denied takeover, TOSTOP, nested Bash and nano unsaved-buffer/resize/save. Nano 8.7 suspends with Ctrl-T then Ctrl-Z; use fg to return. Ctrl-Z alone in nano is not its suspend binding. The test reconstructs trace payload chunks separately from trace metadata.

This is bounded job control, not a claim of complete Linux/POSIX terminal behavior. Native-shell fg/bg, orphaned-group HUP/CONT policy, ptrace/waitid, complete termios, SIGTSTP power-management interaction and Linux restart_syscall semantics for every timed poll/select/futex operation are not established. Such timed waits may return EINTR after a stop; retry-aware applications are required. Ignored/blocked TTOU mode changes remain stricter than Linux to preserve existing access policy. No physical hardware or KVM validation is claimed. See the dated handoff and release receipts for actual passed cases.

## Rebuild without replacing previous images

Use an existing immutable M4 image as the explicit base and a new output directory:

```
python3 tools/build_terminal_job_release.py --base-iso build/codeoss-shared-m4-20261025/Coast-CodeOSS-Shared-tested.iso --output-root build/terminal-jobs-rebuild-normal --preset normal
```

The tool builds a kernel into its own `kernel/` output, relinks the GUI with the existing native runtime objects, compiles the static musl guest fixture, then overlays only GUI/fixture/fresh defaults on the base initramfs. It uses cached lwIP, Limine and the existing project musl compiler; no host installation or remote script. Optional explicit --kernel/--gui inputs allow packaging already tested artifacts. Normal and native presets use 1280x800/100% and 2560x1600/150%. The original kernel and base ISO hashes must remain unchanged. A generated ISO still needs guest validation; its filename alone does not establish that it passed.

The source archive includes the inherited M4 notices/sources/relink objects, current kernel and integration sources, cached lwIP, tests and build scripts. GNU Bash remains the actual licensed upstream 5.3.20 build with its documented Coast parent-TTY patch. Code OSS remains the genuine upstream text-core graphical preview, not the full VS Code workbench. Browser allocator and trading work remain paused.
