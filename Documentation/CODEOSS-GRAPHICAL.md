> Scope correction, 2026-10-25: this preserved M3/M4 native text-core preview does not meet the requested VSCodium/Code OSS application port. Further application work is tracked in [CODEOSS-APPLICATION-PORT.md](CODEOSS-APPLICATION-PORT.md); do not expand this frontend as a substitute.

# Code OSS Core Preview: M4 shared native model service

This original Coast frontend uses genuine MIT Code OSS PieceTree at commit
211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb. PieceTree is unmodified; all 32 pinned
upstream module hashes remain recorded. The native Text Editor remains separate.
Full VS Code workbench, Monaco browser view, Node/Electron, language/extension
hosts and syntax highlighting are still absent.

Super+Space > Code OSS Core Preview opens the graphical editor. New/Open,
selection/movement, undo/redo, literal Find, clipboard, checked Save/Save As,
line numbers, scrolling, text size and indentation controls remain available.
Open creates another window. Ctrl+S saves, Ctrl+Shift+S creates a new file,
Ctrl+F finds and Ctrl+G repeats Find. Arrow/Home/End/Page keys and Shift selection
operate on the actual model. Direct keyboard events are ASCII; UTF-8 files and
clipboard support Unicode, with existing missing-glyph/shaping limitations.

## Responsiveness and process lifecycle

M3 loaded a roughly 52 MB JSC/ICU ELF for every window. The frozen ELF loader
holds the shared VFS mutex while mapping an executable, and descriptor pinning
for pipe I/O takes that mutex even for nonblocking reads. The source mechanism
is fs/binfmt_elf.c load_elf and kernel/sched/sched.c task_file_pin. An isolated
guest probe executes the model with invalid arguments, returning before a JSC
context is created, while timing reads from an unrelated empty nonblocking pipe.
It observed 92-117 ms peak read times versus 12-14 microsecond adjacent clock
pairs; GUI traces observed larger roughly 394-449 ms cold-launch pauses. The
probe establishes loader-related shared I/O blocking, not an exact decomposition
of every millisecond of a graphical cold start.

M4 uses one native JSC service for the GUI session. Each window has a distinct
upstream model, path, selection, history and checked-save state. Protocol v2
carries and validates an explicit document ID in requests/responses, with one
ordered in-flight request and round-robin scheduling across per-window queues.
Unknown/duplicate identities fail closed. Closing a window waits for accepted
actions and dirty-text review, then waits for an acknowledged model Release
before freeing its frontend state. Close-approval queries (including system
power review) do not release a model or close a window. Release never saves a document. The service
remains warm with zero documents so reopening does not reload the engine. It
is terminated/reaped with the GUI, through its small owned supervisor.

The matched six-open guest workload had median small-file readiness of 1235 ms
on preserved M3 and 28 ms on M4 at 100% scale. M4's 200% test observed 96-102 ms
readiness; this is a different display workload, not a matched M3 comparison.
First startup and a restart after service failure still incur cold loading.
The kernel, ELF consistency rules and JSC libraries were not changed.

Tradeoffs: one idle runtime retains memory until GUI exit; a shared service
failure affects all Code windows. Each frontend retains only its last
acknowledged display/recovery snapshot. Recover recreates that window's text
as a new unsaved model, including an optional BOM; Recover in other windows
reuses the fresh service. Save As requires a new name. Unacknowledged input,
GUI/system crashes and power loss are not durably recovered. Connection loss
during Save is reported as an uncertain outcome. Distinct models are not a
security boundary; profiles still share guest file/permission boundaries.

Limits: 32 model registrations, 256 KiB UTF-8 including BOM per document,
64 KiB clipboard, 64 queued actions/256 KiB queued payload per window and a
1 MiB aggregate request-payload budget including the in-flight action. Queue
rejection is explicit; accepted actions remain ordered. Undo history is bounded
to 64 edits/512 Ki UTF-16 units. Display/indent controls are per window and do
not silently reformat document bytes. Full snapshots still cost time for large
files; incremental transport/layout is a future optimization.

Checked CoreFS is unchanged: bounded regular UTF-8 files, no NUL/symlink
following, read-only checks, checked reads/expected source bytes, exclusive
same-directory staging, flushed files, retained .codeoss-original-* versions,
and non-clobber publication/Save As. Conflicts retain unsaved text. No watcher,
cross-process lock, identity CAS, immutable backup, directory fsync or guaranteed
power-loss atomicity is implemented. A writer with an old open inode can change
a retained file. The experimental native CLI retains its separate 4 MiB limit.

## Rebuild and tests

    make -C userland codeoss-graphical

The opt-in target builds the GUI, service, supervisor and resources using the
existing project-local JSC/musl build. It never downloads, rebuilds the engine or
kernel, installs host tools or builds an ISO. The committed bundle is verified
against GRAPHICAL-SOURCES.json. To regenerate it from TS, use pinned esbuild
0.28.2 with tools/build_codeoss_graphical.py --esbuild /absolute/path/to/esbuild.
Runtime resources include graphical-check.js (27 checks) and
graphical-shared-check.js (26 identity/history/release/file-conflict checks).

    python3 tools/test_codeoss_protocol_host.py
    python3 tools/test_codeoss_shared_desktop.py --iso /absolute/tested.iso --label new-unique-label --scale 100

The desktop harness uses only its private diskless, network-disabled VM and
never controls the user's VM. --scale 150/200 and --dark exercise HiDPI/themes;
--baseline measures the older M3 image without expecting a shared process.
ASan/UBSan protocol checks run with leak detection disabled under sandbox ptrace.
The loader-only C fixture under ports/codeoss is diagnostic source; it runs only
in a disposable guest and forks/executes its own fixed test children.

## Preservation and remaining work

Every prior tested release, original ISO, frozen kernel/window ABI, JSC and
text-core CLI is preserved. Licensing, upstream sources, current application
relink objects and exact build commands accompany the new release. No Apple
resources or proprietary assets are used. The two pre-existing platform.ts /
process.ts adaptations remain the only modified upstream modules.

Next optimize incremental snapshots/layout and integrate genuine upstream
editor/workspace services. A full workbench still needs presentation/runtime,
Node/Electron or a real web-workbench route, platform services, file watching
and language/extension hosts. Browser allocator and trading investigations stay
paused. Terminal Ctrl-Z/stop/resume remains unsupported in the frozen kernel;
editor Ctrl+Z undo works. Bash r8 foreground Ctrl-C is preserved. No credential,
sudo, host-account/security or raw-disk changes are made; Disks stays read-only.
