# Foreground input and text clipboard

`build-sh` supplies bounded foreground pipelines on the console and desktop
PTYs. A pipeline's members share an input process group. The shell hands the
terminal to that group, waits, and restores its own group and terminal mode.
This permits `build-sh -c 'cat file | less'`, multiple producers and slow
readers. Ctrl+C reaches the foreground group; background jobs cannot read or
reconfigure its terminal. Existing native process-spawn handoffs remain
supported, including piped input and stdin close/reopen.

The kernel provides `setpgid`, `getpgrp`, `getpgid`, `getsid`, group selectors
for `kill`/`wait4` and `TIOCGPGRP`/`TIOCSPGRP`. Membership and ownership changes
are constrained by the caller's session, direct children and terminal
capability. A controller exit restores its referenced terminal and signals its
foreground job. Browser roles are denied group kill selectors. This is the
single-thread controller fork/exec profile used by the shipped ash; full
`setsid`, stopped-job `fg`/`bg` and general POSIX job control are not supplied.

The shared text clipboard holds up to 64 KiB of strict UTF-8. TAB, CR and LF
are allowed; NUL, ESC, DEL, other C0 controls, malformed/overlong UTF-8 and
surrogates are rejected. Writes are atomic. Reads use a generation snapshot;
undersized/stale reads fail without partial mutation. Native `clip -c`,
`clip -p` and `clip --clear` operate through the same API as the desktop.
`/usr/include/coast/clipboard.h` and the installed source example provide
the public SDK. GUI ownership or foreground native console/PTY access is
required; browser sandbox roles cannot call the service.

Editor uses Ctrl+A/C/X/V and Shift+Insert, preserving multiline UTF-8 across
windows and native applications. Dialog, path, search, launcher and Browser
address controls have selection anchors and Shift+arrow/Home/End navigation;
pasted CR/LF/TAB become spaces in single-line fields. A paste that exceeds a
field's capacity leaves it unchanged.

Browser webpages use the same text clipboard through Ctrl+C/X/V,
Ctrl+Insert, Shift+Insert and the desktop Edit menu. WebCore handles the
selection and paste events, including focused cross-origin frames. Transfers
assemble up to 64 KiB before decoding UTF-8 or publishing a clipboard value.
Cut deletes the selection only after the desktop accepts the copy, provided
the selection has not changed. Oversized/invalid copies retain the previous
clipboard and the source text. The renderer has no direct clipboard syscall
access; a desktop input gesture delegates each transfer. This does not enable
background `navigator.clipboard` access, HTML/image clipboard formats or
clipboard sharing with the host computer.

Electron's Wayland bridge lets an authorized in-flight copy finish when focus
changes. Conditional writes compare the clipboard generation atomically, so
a delayed copy cannot replace newer text. Transfers remain bounded by their
existing size, source-lifetime and time limits; new reads require focus.

Terminal copies selected rendered plain text with Ctrl+Shift+C, and pastes
with Ctrl+Shift+V or Shift+Insert. Ctrl+C interrupts the foreground command.
Multiline/large pastes require review of a frozen snapshot. Cancel sends no
bytes. Accepted text uses a bounded nonblocking queue; partial writes and
EAGAIN retain the exact suffix, and queued typing finishes before paste starts.
Paste checks the current terminal mode even when invoked from its menu. An
unfinished final line remains in the foreground program's input. A
foreground/mode change, close, exit or timeout cancels the remaining paste;
raw applications requesting bracketed paste receive its framing. Canonical
lines are bounded by the console's 511-byte limit.

Run `tools/test_foreground_jobs.py`, `tools/test_unix_tools.py`,
`tools/test_console_input.py`, `tools/test_clipboard_native.py` and
`tools/test_clipboard_desktop.py` and `tools/test_browser_clipboard.py` against
the intended ISO. Use `--memory 16G` for full Code OSS images. Host sanitizer
checks are `tools/test_desktop_input_host.py` and
`tools/test_terminal_clipboard_host.py`. Actual guest files, serial traces,
PNG captures, fresh-guest settings reload and ownership/cleanup reports are
retained in `build/continuation-20261015-phase2/`.

## Interactive completion

Tab completes the console shell and the desktop Terminal command field.
Builtin/external help entries share one authoritative command table; executable
PATH entries and filesystem names are scanned without executing candidates.
Ambiguous names insert a common prefix and list matches. Directory matches
append `/`; single file/command matches append a space when appropriate.
`cd` filters for directories. Quoted/escaped names, UTF-8, middle-of-token
cursors and suffixes are preserved; command position after pipes and
redirections is recognized without evaluating shell expansion.

Completion has bounded scans, 128 stored matches and a 511-byte line. When
results are truncated or expansion will not fit, it leaves the line unchanged.
Malformed text/control names are not offered. Console arrows, Home/End,
Ctrl+A/E/U/K/W, Ctrl+C and 32-entry history retain the unfinished draft.
The GUI retains its existing selection/clipboard shortcuts and restores a
draft after Up/Down history navigation. Its completion queries the actual
owned PTY shell's current directory; it refuses completion while another
foreground command owns that terminal. Interactive `build-sh` uses upstream
ash's raw-mode editor/completion through the VT path instead. The existing
Coast BusyBox port lets that editor take over an ECHO-off GUI PTY and
restore its original mode on return.

UTF-8 can enter the GUI command field through the existing text clipboard;
the legacy serial GUI text transport and the current USB text mapping remain
ASCII. `tools/test_completion.py` uses real guest paths, a disposable custom
PATH executable and actual GUI input. It verifies no execution on Tab or
cancellation and execution only after Enter. `tools/test_completion_host.py`
sanitizes the production shared engine and native filesystem adapter.

Native application menus expose the existing clipboard actions, including
Terminal's Ctrl+Shift+C/V paths. Ctrl+Alt+M opens the menu bar without reserving
Terminal/Doom Alt-letter or function keys. Browser address-menu commands send a
matching key release so modifier state does not remain held. Native labels use
the same fractional-pixel rounding as measurement and clip narrow ellipses.
