# Everyday terminal tools

Coast includes native BusyBox utilities for browsing and transforming real
files. They are installed in `/bin` and work from the OS console, `sh`, `ash`,
`build-sh`, and desktop Terminal. `busybox --list` shows the compiled suite;
`COMMAND --help` prints each upstream program's supported options.

| Command | Use | Example |
| --- | --- | --- |
| `less` | Browse a file, or a pipeline in desktop Terminal, one screen at a time | `less -N /tmp/notes.txt` |
| `nl` | Number all or nonempty lines | `nl -ba /tmp/notes.txt` |
| `tac` | Print lines in reverse order | `tac /tmp/notes.txt` |
| `rev` | Reverse the bytes on each line | `rev /tmp/notes.txt` |
| `paste` | Join corresponding lines or combine lines from one file | `paste -d : /tmp/names /tmp/values` |
| `comm` | Compare two sorted lists | `comm -12 /tmp/list-a /tmp/list-b` |
| `fold` | Wrap long text lines to a chosen width | `fold -s -w 72 /tmp/notes.txt` |
| `base64` | Encode binary data or decode it with `-d` | `base64 /tmp/data.bin` |
| `split` | Split a file into fixed-size pieces with letter suffixes | `split -b 1m /tmp/archive /tmp/part.` |
| `clear` | Clear the screen from POSIX shells as well as the console | `clear` |
| `which` | Find executable files on the POSIX shell's `PATH` | `build-sh -c 'which gcc grep'` |

In `less`, use Space/Page Down to advance, `b`/Page Up to go back, `g`/`G` to
jump to the start/end, `/pattern` and Enter to search, `n` for the next match,
and `q` to quit. `-N` shows line numbers, `-S` truncates long lines, and `-F`
quits when the whole file fits on one screen. The pager tracks terminal resize
events and restores the input mode on exit. Its input limit is 100,000 lines;
use `head`, `tail`, or smaller files when working with larger logs.

The console accepts one command at a time. Use `cd -` to return to the previous
directory; another `cd -` toggles back. A failed `cd` preserves that directory.
The existing bare `cd` still selects `/`. Console `which` finds programs in
`/bin`; the standalone `/bin/which` searches `PATH` and supports `-a`.

Use `build-sh -c 'COMMAND'` for pipes, redirects, wildcards and variables:

```text
build-sh -c 'printf "third\nfirst\nsecond\n" | sort | nl -ba'
build-sh -c 'base64 -d /tmp/data.b64 > /tmp/data.bin'
build-sh -c 'cat /tmp/part.* > /tmp/archive'
build-sh -c 'git log > /tmp/git-log; less /tmp/git-log'
```

Desktop Terminal also supports interactive pager pipelines such as
`git log | less`. On the serial/USB console, redirecting a pipeline changes
foreground input ownership; an interactive `cat FILE | less` currently exits
with `cannot configure terminal: Operation not permitted`. The shell remains
usable. Save the command output to a file and run `less FILE` there, as in the
example above. Direct `less FILE` and `build-sh -c 'less FILE'` both support
interactive console navigation.

The tools preserve upstream behavior and report real filesystem errors.
`comm` expects sorted input. `rev` is byte-oriented in this build. `split`
overwrites matching output names, so choose a fresh prefix or directory.
The files in `/tmp` and the RAM root disappear on reboot; use a writable FAT32
mount for documents that should persist.

Run `python3 tools/test_unix_tools.py --iso PATH` to verify the installed
commands on a packaged image. The RAM-only, network-disabled regression checks
exact output bytes, blank/unterminated lines, binary round trips, error status,
console navigation, interactive file paging on the console, the explicit
console-pipeline refusal with unchanged terminal mode, and interactive file/pipe
paging on real pseudoterminals. A 100-by-240 terminal with a slow reader checks more than
16 KiB of exact screen text, including its last line, and restores original
terminal settings and file-status flags. `unix-tools-check` provides the PTY checks directly in the OS.
