# Shell dependency assessment — 2026-10-24

This separates observed guest behavior from unbuilt packages. Terminal r7 additionally builds the existing verified GNU Bash 5.3.20 sources with bundled Readline and stages it only in new images; no global shell or security installation is performed.

| Component | Established state | Dependency class and next verification |
| --- | --- | --- |
| GNU Bash 5.3.20 | Genuine source/build present; bundled Readline works. The r8 parent-handoff adaptation now verifies foreground child Ctrl-C, pipeline/nested-shell interruption, child stdin and nano raw edit/resize/save/return on the frozen kernel at 100%/200%. | The earlier failure is identified: upstream disables parent tcsetpgrp while Coast permits only the current controller to transfer foreground ownership. The Bash-only adaptation uses the existing authorized parent path. Stop/resume job control remains unsupported (signals 19-22 are rejected); no kernel permission check was weakened. |
| zsh | Not downloaded, configured, built or guest-run. Existing C/static-musl cross compiler and ncurses make a source-port attempt plausible. | Primarily an unbuilt package. No zsh-specific kernel incompatibility is established. A cross build must be followed by interactive input, signals/foreground jobs, completion/module loading and ownership checks before enabling it. |
| fish | Not built or guest-run. No Rust/cargo executable was found on the inspected host PATH. | A known build-tool dependency gap and an unbuilt package. Current upstream needs Rust >=1.85/cargo, CMake >=3.15 and a C compiler; dependency retrieval should be pinned/project-local. No fish-specific kernel failure is established without a guest run. |
| Oh My Zsh | Not installed. It is a configuration/plugin framework, not another shell. | A working zsh port is the immediate missing dependency. Upstream also expects git and curl/wget. Completion security uses process identity and file ownership/mode checks; these need real compatibility tests, not disabled audits. Plugin-specific tools require separate verification. |

Bash's UID/EUID variables report 4294967295 (unsigned -1), while new direct-syscall and libc probes in the same frozen guest return 0. This establishes a Bash initialization/query discrepancy rather than a demonstrated missing UID syscall. It is not authenticated identity, isolation or sudo support. That separate security-sensitive work remains excluded.

The earlier Bash interruption/nano failures are now explained by foreground ownership, with a narrowly scoped Bash adaptation and positive guest regression. PTY operation and existing ownership enforcement remain intact. zsh/fish have not been built or guest-tested, so their exact compatibility gaps remain unproven. Password/sudo/account enforcement changes remain excluded pending separate action-time approval.

Sources checked directly:

- [fish upstream build requirements](https://github.com/fish-shell/fish-shell#building).
- [Oh My Zsh prerequisites](https://github.com/ohmyzsh/ohmyzsh#prerequisites).
- [zsh completion ownership audit source](https://github.com/zsh-users/zsh/blob/master/Completion/compaudit), which checks EUID and file ownership/modes.
- Local evidence: `build/coast-personal-20261024/VALIDATION.json`, shell status and QA reports; `tools/build_linux_ports.py` for existing Bash flags.

No remote installer, global tool installation, credential, startup-file replacement or compaudit bypass was used.
