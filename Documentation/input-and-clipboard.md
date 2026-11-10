# Input, Terminal jobs and clipboard

The native desktop receives USB keyboard/pointer events through the kernel
window service. Applications can read their own input queue and submit their
own pixels; ordinary clients cannot inspect or control peer windows.
The signed-in profile and native window focus govern input and clipboard
permissions. See [accounts](accounts.md) and [desktop](desktop.md).

## Terminal and shells

Coast supplies source-built Bash/Readline. Pipelines, redirections,
command/process substitution and foreground/background jobs operate through
the real PTY, process-group and signal interfaces. Ctrl+C interrupts the
foreground job, Ctrl+Z stops a supported foreground job, and Bash provides
`jobs`, `fg` and `bg`. Background jobs cannot take over another session's
terminal. The console command interpreter implements its own bounded command
set; shell choice does not expand kernel authority.

Terminal supports scrollback, text selection, Ctrl+Shift+C copy and
Ctrl+Shift+V or Shift+Insert paste. Large/multiline pastes require review of a
frozen snapshot. Accepted text uses a bounded nonblocking queue; partial writes
preserve the remaining suffix. A mode/foreground change, close or timeout
cancels remaining input. Programs requesting bracketed paste receive its
framing. Canonical console input is bounded to 511 bytes per line.

## Shared text clipboard

The clipboard holds at most 64 KiB of strict UTF-8. TAB, CR and LF are allowed;
NUL, ESC, DEL, other C0 controls, malformed UTF-8 and surrogates are rejected.
Writes are atomic and reads use generation snapshots. Stale or undersized
reads fail without partially mutating the clipboard. `clip -c`, `clip -p` and
`clip --clear` use the same native API as GUI applications.

Text Editor supports selection, copy/cut/paste and undo/redo. Native dialog,
path and search fields keep selection anchors; multiline paste is normalized
for single-line fields and an oversized paste leaves the field unchanged.

Chromium and Code OSS exchange text through their native display/clipboard
bridge. The focused authorized window delegates transfers; renderer processes
have no direct native clipboard syscall authority. In-flight transfers remain
bounded by size, source lifetime and timeout, and conditional writes compare
clipboard generations so a delayed copy cannot replace newer data. New reads
require the appropriate focus. Native Qt Widgets uses the same UTF-8 clipboard
through the Coast QPA plugin.

This interface covers text inside Coast. HTML/images, arbitrary MIME formats,
primary selection and sharing the host computer's clipboard are outside its
scope. Switching profiles closes application sessions and clears the clipboard.

## Verification

`tools/test_desktop_terminal.py` checks actual Bash commands, process
substitution and USB-driven job control. `tools/test_clipboard_native.py`
checks the native permission/data contract. The Chromium and Code OSS guest
suites cover application integration; `tools/test_qt6_clipboard.py` checks Qt
round trips and focus/permission boundaries. Their reports identify the exact
ISO and record owned-VM cleanup. See [source builds](source-builds.md).
