# Coast Terminal r7 / Code OSS M2 — 2026-10-25 00:20 UTC

The interrupted preferences reboot test completed successfully: terminal-r7/qa/final-preferences-persistence-03/report.json. The reconnected host lists no QEMU or interrupted test process. No user VM was controlled. Release collection and the 72-path preservation audit pass; all earlier ISOs, the frozen kernel, standalone jsc and paused handoffs remain byte-identical. Trading/browser allocator work remains paused. No host disks/settings/accounts or guest credentials/privilege enforcement changed.

Tested local release: build/coast-terminal-20261025. The initially denied installation was confirmed not executed, then completed by one normal reviewed retry after materially newer project-modification authorization. No check was bypassed. Source collection remains in the executor workspace at terminal-r7/release.

Images:
- Coast-Coast-Terminal-native-tested.iso: 2560x1600, 150%; SHA-256 dbde0f0b90dc91b0d7d7d10ae20586b147fd918f1207b006e1ab4acc10319df2.
- Coast-Coast-Terminal-tested.iso: 1280x800, 100%; SHA-256 7e74fc67f401c34d60a665cf7bd0399e146ddda806c727341b7ac71aaba750bf.
- Frozen kernel: 9e03c90f99e0d458cf766740e40c4e594b1b3dc55c10293b73d64957f16b04a0.
MANIFEST.json records GUI, native shell, GNU Bash Readline and Code OSS C-API binary hashes. Source/manifests, original GNU/JSC source, notices/relink objects, QA receipts and actual screenshots are included. No upload/public distribution occurred.

Completed Terminal: direct PTY/unified VT, inline native editing/history/completion; child stdin and Ctrl-C; nano raw edit/save/alternate-screen return; bounded 512-row scrollback, selection, wrap/reflow, resize while typing; background output preserves input and Ctrl-L redraws visual interleaving. Native shell commands remain bounded to 511 bytes. Complete xterm, wide/combining Unicode terminal layout and unrestricted glyph coverage are not claimed.

Completed Preferences: nonmodal owned child from Terminal > Preferences / Ctrl+comma; staged Apply/Done/Cancel, one per parent, correct focus/reopen/parent closure, profile guard, disk save and reboot. Overlapping child/translucent Terminal equals forced full repaint (zero differing pixels). Existing persisted field names remain; Terminal is no longer a system Settings category.

Completed plaintext editor: labeled file actions, bounded Undo/Redo (64 edits/1 MiB), literal Find/Replace, visible display/indentation controls and Preferences; wrapping toolbar geometry shares hit-testing/viewport calculations. Guest edit/save preserves CRLF bytes. No rich formatting, spreadsheet model, soft wrap or PieceTree integration into this native app is claimed.

Code OSS M2: genuine unmodified upstream PieceTree at 211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb, explicit platform/process adapters and native JSC C API. Core 2000-edit, RAM, positively identified disposable FAT32, mode/read-only protection and interactive CLI tests pass; stale saves preserve unsaved edits and originals. Earlier storage03/04 invalid second-mount fixtures are excluded; storage05 is valid. This is the native experimental text-core CLI, not a full graphical IDE/web workbench. Runtime/UI, workspace, language/extension hosts, watchers/event loop and complete filesystem service semantics remain. See CODEOSS-M2-RELINK.md and PORT-STATUS.txt.

Bash: genuine GNU 5.3.20 +20 official verified patches, bundled Readline. Guest editing/history/Ctrl-U/builtin read pass. Child Ctrl-C/job control remains experimental. After reconnect, focused probes reproduce a three-second sleep surviving Ctrl-C until normal completion; Bash reports monitor enabled, child group differs from the PTY foreground group. Registering Bash's own group explicitly does not fix this. Diagnostic-only tracing is now underway in separate terminal-r8 workspace; original kernel, project and r7 binaries are untouched by that investigation. Default native-shell child interruption/nano passes. zsh/fish/Oh My Zsh remain unbuilt; password/sudo/security enforcement excluded.

Next: inspect exact Bash tcsetpgrp/setpgid error on the frozen guest, then implement the narrowest justified terminal fix with focused regression tests and a separate image. Project installation and dated handoff are complete. Preserve all existing images and the user's VM. The image overlay contains the new Readline Bash, while the project's older generic runtime Bash cache remains preserved; reproduce using the included packaging/build recipe, not a generic original ISO build.
