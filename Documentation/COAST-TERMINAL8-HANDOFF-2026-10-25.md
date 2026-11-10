# Coast Terminal r8 / Bash foreground handoff — 2026-10-25 00:31 UTC

This is the latest implementation handoff, after the r7 completion handoff. The reconnected r7 preferences reboot test passed; r7 was collected/audited and installed by one approved normal retry after an obsolete read-only review denial. No check was bypassed. This r8 release adds a verified Bash-only foreground handoff fix and preserves r7 and every older tested image. Trading/browser allocator work, host settings/accounts/raw disks, guest credentials/sudo/security enforcement remain untouched.

Release: build/coast-terminal8-20261025.
- Coast-Coast-Terminal8-native-tested.iso: 2560x1600, default 150%; SHA-256 29b03647f60111817e1d54d2126df9681167141064ccd2c3470b014d961374a7.
- Coast-Coast-Terminal8-tested.iso: 1280x800, default 100%; SHA-256 43c2e9763c7dbafab4c1dc31d671a39a65323d7673a1086a8140d6ec77440fa1.
- GNU Bash binary: cb7642d9cf1ffa178d364ca6cf3e87e5948ce15a648a5f80342a9ed566053fcc.
- Frozen kernel: 9e03c90f99e0d458cf766740e40c4e594b1b3dc55c10293b73d64957f16b04a0.
The GUI, default native shell, JavaScriptCore and Code OSS binaries are byte-identical to r7. The 83-path preservation audit verifies prior images, kernel/TTY sources, old generic Bash cache, r7/older handoffs and scoped source changes. Source/build manifests, GNU/JSC corresponding source, licenses, relink objects, actual screenshots and QA receipts are included locally. No Library retry, upload or publication occurred.

Established cause: GNU Bash 5.3 has the parent foreground transfer compiled out and asks the child to call tcsetpgrp. Coast requires that transfer to be made by the existing controller. The child receives EPERM and remains outside the PTY foreground group; Ctrl-C reaches Bash while its child continues, and full-screen child termios changes are denied. Exact trace: BASH_TTY_TRACE pid=19 selfgrp=19 fd=127 before=18 target=19 result=-1 errno=1 job=1. An existing parent-authorized native probe succeeds. Registering Bash's own group alone did not solve it.

The Coast-only GNU patch restores parent-authorized handoff before closing the existing process-group synchronization pipe. All new job-group stages wait until that handoff; child-side promotion is skipped. No kernel permission or foreground ownership check was weakened. A clean offline build used verified GNU 5.3 sources +20 official patches, explicit port patch, bundled Readline, static musl/ncurses.

Verified now:
- Exact final normal100 and native150 images pass ten guest test groups each. The same clean binary also passes at 200%.
- Five repeated foreground child interruptions; pipelines; child stdin; command substitution; background launch/wait; nested-shell interruption and outer foreground restoration; nano raw mode, resize, edit, save and return.
- Background attempts to promote their own foreground group remain rejected.
- Genuine Code OSS upstream text-core CLI launched through Bash accepts interactive edits/saves and produces exact CRLF bytes (42 61 73 68 20 43 6f 64 65 4f 53 53 0d 0a).
- Baseline three-second sleep survives Ctrl-C until 3.20 seconds. Fixed measured return averages 0.429 seconds at 100% and 0.442 seconds at native150; pipelines about 0.53/0.55 seconds. Timings include test input/event overhead, not a kernel-latency benchmark.
- Final screenshots were inspected. All private test VMs close through their own process handles; no user VM was controlled or terminated.

The r7 UI is retained: direct inline PTY Terminal, bounded VT scrollback/reflow, owned Terminal Preferences child with staged Apply/Cancel/Done and profile protection, and labeled plaintext editor actions/settings with bounded undo/redo and literal Find/Replace. R7 disk-backed profile preferences/reboot, byte preservation, 100/150/200% UI and zero-pixel cached/full material repaint equivalence remain recorded in the preserved r7 release. The latest r8 images reuse the identical GUI. Editor is plain text, not rich text or a spreadsheet.

Limits: Ctrl-Z/stop/resume job control remains unsupported; the frozen kernel rejects stop signals 19-22, and background terminal reads can be denied rather than stopped. Do not advertise complete Linux job control or full xterm/Unicode terminal parity. Bash UID/EUID variables still show unsigned -1, while direct syscall/libc probes return 0; that separate discrepancy was not repaired and is not authentication/isolation evidence. zsh/fish/Oh My Zsh remain unbuilt. Profiles remain preference sets with shared files/permissions. Code OSS remains real upstream PieceTree + checked native file adapter/CLI, not the full graphical workbench, web version, language server or extension host.

Build tools: tools/build_bash_readline.py verifies original archives/patches and applies ports/bash/COAST-PARENT-TTY.patch in a new build root. tools/build_terminal_release.py extracts an explicit immutable r7 ISO, retains its kernel/current application binaries, overlays the new Bash/source/notes/fixture and produces a separate image. tools/test_terminal_bash.py uses its own private diskless/network-disabled VM. These workflows do not rebuild engines, replace generic phase-5 Bash cache, kill unrelated VMs or run remote installers. Retain the release source/patch/license material.

Launch without rebuilding or attaching additional disks:
```sh
cd /home/camdenp/Desktop/Coast
make run-image O=build/coast-terminal8-20261025 ISO_IMAGE=build/coast-terminal8-20261025/Coast-Coast-Terminal8-native-tested.iso MEMORY=4G LINUX_DISK= FAT_DISK= SATA_DISK= QEMU_NETWORK='-nic none' QEMU_EXTRA_ARGS=-full-screen
```
Type gui at the guest prompt. Ctrl+Alt+Escape returns to its console. In Terminal > Preferences (Ctrl+comma), Shell > GNU Bash applies to new Terminal windows. Without a guest writable disk, preferences persist within this boot only. O= gives this release its own serial log; do not run simultaneous instances sharing the same writable disk/log.

Next planned terminal step: design stopped-job states, waitpid/WUNTRACED reporting and terminal restoration tests before exposing Ctrl-Z/bg/fg. No stop/resume kernel change is in this release. The separate next Code OSS milestone is binding its genuine text model/file adapter to a guest graphical view; Electron/Chromium/Node/workbench/extension prerequisites remain larger work. Keep allocator/trading paused and preserve these release checkpoints.
