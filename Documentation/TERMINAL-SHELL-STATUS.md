# Terminal shell and appearance scope

2026-10-24. Revision 6 is applied and validated. The initial obsolete-scope apply/build denial was resolved by one approved retry after clarified chronological authorization.

The native Coast command shell remains the default. The staged Terminal selector additionally launches the already shipped genuine GNU Bash through `/usr/bin/linux-bash`, resolving to `/usr/lib/coast/linux-ports/bash/bash`. It uses the existing BusyBox ash to set guest-only HOME/PATH/TERM/SHELL and exec Bash. Existing shell processes are not replaced when a preference profile changes; shell selection applies to new Terminal windows. Candidate03 executed GNU Bash 5.3.20 in the console and GUI, with a real BASH_VERSION response. Native-shell nano raw/canonical edit/save, appearance settings, profile separation and cold-reboot persistence passed at normal and HiDPI scales. Full Bash job-control and signal compatibility are not claimed.

`/bin/build-sh` is the native launcher for BusyBox ash, not GNU Bash. Existing `tools/build_linux_ports.py` builds Bash with `--without-bash-malloc`, `--disable-readline`, `--enable-static-link`. Its prior phase-5 test expects GNU Bash 5.3.20. The existing compiled Bash binary was absent from the old desktop initramfs, so the new image explicitly includes it with corresponding source, official patches, license and build provenance; version 5.3.20 and GUI command execution are verified. GUI cooked input provides its own editing/history/completion; do not claim Bash readline or every shell job-control behavior works.

The staged preferences add five palettes, foreground/background color choices, background opacity from 40 to 100%, ANSI color on/off, and native antialiased monospaced output font sizes 9/11/13/15/17/19 logical pixels at 100/125/150/200% scaling. Lower opacity uses the existing sampled backdrop blur. The cooked command input bar keeps its normal UI text metrics. Palettes permit low-contrast user combinations; normal/native screenshots and the blur/font/persistence matrix passed, including exact cached/full repaint equivalence. The VT implements a bounded subset of terminal escapes and 16 ANSI colors, not full modern xterm/truecolor/Unicode-terminal parity.

## Other shells

- zsh: the official source directory lists 5.9.2, released 2026-07-12. It is a plausible C/static-musl port using the existing cross compiler and ncurses, but no source download, configure/build, PTY run, completion or module verification was completed in this pass. It is not installed or selectable. A successful compile alone would not establish interactive compatibility.
- fish: current upstream requires Rust 1.85 or newer with cargo, CMake 3.15+, a C compiler and additional dependencies; PCRE2 is optional and the build can automatically retrieve dependencies. No Rust/cargo executable was found on the inspected host PATH. A project-local pinned cross toolchain and reviewed dependency closure, then guest syscall/PTY verification, would be needed. No host-global tooling or automatic dependency installer was run. Fish is not installed or selectable.
- Oh My Zsh: this is a zsh framework, not an independent shell. Upstream requires zsh, curl/wget and git. Since no working zsh guest port has been established, it is unavailable. No remote installer script was executed, shell startup file replaced, compaudit protections disabled, host account changed or network/account configuration altered. A later reviewed manual guest-only integration can follow a verified zsh port.

Primary sources checked:

- zsh official source directory: https://zsh.sourceforge.io/Arc/source.html (linked from https://www.zsh.org/)
- fish official upstream README/build requirements: https://github.com/fish-shell/fish-shell
- Oh My Zsh official prerequisites/manual installation: https://github.com/ohmyzsh/ohmyzsh/blob/master/README.md
- Existing project Bash build recipe: `tools/build_linux_ports.py`; existing phase-5 test: `tools/test_linux_ports.py`.

Identity limitation: the actual guest Bash reports UID/EUID=4294967295 (unsigned -1). The checked current syscall source returns 0, but this frozen kernel image was not rebuilt here. Do not use the shell port as proof of account identity, authentication or privilege isolation, and do not repair identity/security enforcement as part of the UI pass.

Full-screen limitation established: nano successfully starts from Bash, but the GUI never observes the PTY switching out of canonical mode; it is not usable as a full-screen editor through this launch path. The same nano edit/save round-trip through the default Coast shell passes. Bash is therefore labeled "GNU Bash (basic)" with a visible limitation, and full-screen regression tests use the native shell. No kernel job-control, identity or permission enforcement is changed to force the Bash port to work.
