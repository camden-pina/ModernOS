# Coast GNU Bash terminal adaptation

`COAST-PARENT-TTY.patch` applies after the 20 official GNU Bash 5.3 patches and is enabled by `-DCOAST_PARENT_TTY=1`. It transfers a foreground job from its authorized parent before releasing the process-group synchronization pipe. All new job-group children wait, and no child promotes itself. Existing kernel permissions are preserved.

Use `tools/build_bash_readline.py` with a new build root. Retain `build.json`, original GNU source/signatures/license and this patch. `tools/build_terminal_release.py` packages a separate ISO from an explicit immutable r7 ISO; it does not replace the generic phase-5 Bash runtime cache or rebuild the kernel. `terminal-job-check.c` is a guest-only test fixture for process groups/termios and denied background takeover, not a desktop application.

Foreground Ctrl-C, pipelines, nested shells, stdin and nano are verified. Ctrl-Z/stop/resume job control remains unsupported. See Documentation/BASH-FOREGROUND-HANDOFF-2026-10-25.md for observed cause, exact evidence and remaining limits.
