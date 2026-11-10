# Coast Profiles r5 handoff — 2026-10-24

Recorded: 2026-10-24 20:16:05 UTC. Actual project: `/home/camdenp/Desktop/Coast`.
UI pass is applied, built, visually inspected and verified in real guest VMs.
All eight final suites passed. No permission action is blocked or pending.

## Completed changes

- Open top-level menus switch when the pointer enters another visible heading.
  Closed menus stay closed on hover. Same-heading clicks still close; popup
  selection, dismissal and keyboard navigation remain functional. The fix
  includes forwarding passive top-bar pointer events, not just click dispatch.
- Settings now uses an Appearance/Wallpaper/Dock/Display/Mouse/Touchpad/Profiles
  sidebar, focused category panels, a fixed save/storage footer, and local
  content scrolling. The sidebar compacts in short HiDPI windows. Ctrl+1 through
  Ctrl+7 selects categories; Tab and arrows navigate controls. Wallpaper previews
  start when the Wallpaper category is opened. Existing setting meanings remain.
- Desktop profiles can be created, renamed and switched. They group appearance,
  wallpaper, scale/workspace, pointer, Dock/pins and editor preferences. Switching
  saves outgoing preferences, reloads the selected set and handles live DPI and
  workspace changes. New profiles copy current preferences. Active selection and
  names persist on a writable guest volume. The original legacy config remains
  unchanged; supported values migrate into Default on save.
- This foundation shares files, open windows, clipboard, app state and OS
  permissions. Home stays `/home/user`; recent-file lists reset on switch.
  Authentication, UID/GID accounts, private homes and session isolation remain
  future work. No credentials or persistent access were created. No host account
  or security setting changed. The UI states this scope explicitly.

## Mouse artifact: detected cause and verification

The actual delivered Polish r4 image reproduced 6,034 stale pixels after clicking
an Editor preference control and moving into blank space inside the SAME editor.
Forced full composition cleared them. Leaving the whole window hid the bug by
triggering the existing pointer-owner repaint. Before/after proof and difference
image are under `qa/r4-hover-artifact02`.

`desktop.c` had an obsolete Editor toolbar hover rectangle and no preference-panel
control coverage. The cached foreground retained the prior button hover color.
The new `editor_hover` supplies exact current toolbar, preference and Done-control
regions, clipped to the panel viewport. The reproduction now gives ZERO changed
pixels against a fresh full repaint. This establishes a hover/damage omission;
no cursor-underlay corruption was established. Cursor stamp/restore and scanout
paths were inspected and left unchanged.

Final 100/150/200% input suites also compare actual screenshots against forced full
recomposition after screen-edge cursor movement, popups, Dock transitions and
interrupted asynchronous wallpapers. Native and dark HiDPI glass suites verify
window drag restoration, menu/launcher/modal layers and dynamic lower-window
updates. These are actual guest framebuffer captures, not mockups or scaled assets.

## Profile layout and boundaries

`Documentation/DESKTOP-PROFILES.md` documents controls, format and scope.
`BASE` is the existing `.coast` directory on the selected writable guest FAT
volume, or `/home/user/.config` in RAM:

- `BASE/profiles.ini`: version, active generated ID, display names.
- `BASE/profiles/default/desktop.ini`: Default settings.
- `BASE/profiles/profile-N/desktop.ini`: another preference set.

Default falls back to supported legacy `BASE/desktop.ini` values, then image
defaults. New IDs are generated independently of names. Eight total profiles and
1-40 printable ASCII display names are supported. There is no profile deletion
or sign-in UI. RAM profiles last for this boot; disk-backed sets survive reboot.
Individual files use complete temporary writes/fsync and no-replace publication;
there is no cross-file crash transaction or concurrent-session protocol.
Malformed registries disable writes and are preserved. Unknown versions/unsafe
IDs, embedded NUL, directory targets and read-only writes are refused with visible
errors. Original settings/registry bytes were verified unchanged in failure tests.
Secure guest authentication would require a separate account/permission/session
and credential-storage design. It was not attempted in this pass.

## Checks performed now

- Native compile/link with `-Wall -Wextra -Werror`; source/binary/image hashes match.
- Real pointer and keyboard tests at 100%, 150%, 200%, including a 1280x800 display
  switching into 200%: closed-hover behavior, three repeated open-menu hover cycles,
  click toggle, popup item hit, category navigation, visible sidebar endpoints,
  focus scrolling, profile create/rename/switch and hot DPI changes.
- Default and Studio retain separate Dock, editor, dark/light, wallpaper and scale
  values; cold reboot restores the active set and switching restores Default.
  Legacy config bytes stayed unchanged. Diskless RAM profile creation and desktop
  relaunch also passed, with its reboot lifetime disclosed.
- Four failure guests: traversal-style registry ID, embedded NUL, directory config
  target and a read-only volume. All refused the action and preserved originals.
- Production profile format checks, glass/pointer ownership, fonts/label clipping
  and HID/input checks passed ASan/UBSan. LSAN is disabled because this traced
  executor cannot run it. No app memory allocator investigation was resumed.
- Native/dark HiDPI glass pixel equivalence, 30-step dragging, dynamic Terminal
  backdrop updates and read-only Disks inventory/refresh passed. The disposable
  read-only GPT/FAT image hash stayed unchanged.

Cold Settings OPEN was observed at 243.5 ms, the
roughly 240 ms floor of two synthetic 120 ms inputs. Queued input remained usable
during wallpaper work. The native 150% standalone glass run averaged 19.55 ms
per drag composition over 21 frames; maximum 22.73 ms
under 4-vCPU QEMU TCG. This is software-emulation evidence, not a 120 Hz guarantee.

Evidence: `build/coast-profiles-20261024/VALIDATION.json`, `qa`, `logs`,
`r5-source.patch`, manifests, changed-file list and preservation hashes.
Earlier harness failures are retained in the task workspace; corrected final
suites all pass. Historical fixed-coordinate UI runners target older layouts;
`tools/test_profiles_desktop.py` is the regression entry point for this pass.

## Run

```sh
cd /home/camdenp/Desktop/Coast
make run-image ISO_IMAGE=build/coast-profiles-20261024/Coast-Coast-Profiles-native-tested.iso LINUX_DISK= FAT_DISK= SATA_DISK= QEMU_EXTRA_ARGS=-full-screen
```

Enter `gui` in the guest. Native image: 2560x1600 with 150% guest defaults. The
alternative `Coast-Coast-Profiles-tested.iso` is 1280x800/100%, using the same
run-image target. Ctrl+Alt+Escape returns to the console; Ctrl+Alt+F toggles QEMU
fullscreen. No rebuild or disk attachment is needed. The command clears all
optional disk variables and attaches no host raw disk. Diskless profiles are RAM
only; persistent profiles require an explicitly selected writable guest volume.

## Preserved work and next step

The delivered Polish images/handoff, original os.iso/initramfs/kernel and paused
browser handoffs/status match their checkpoints. Code OSS M1 source, jsc and build
recipe also stayed unchanged. Trading and the browser allocator investigation
remain paused. Disks remains read-only. No host disk, display, security or account
setting changed. No files were uploaded/published; unavailable Library upload was
not retried. The Desktop tree has no .git; the separate Documents/test checkout was
not touched. Source checkpoint: `preservation/polish-r4.tar.gz`.

Code OSS M2 remains safely staged and unbuilt/unapplied in task-2/codeoss-m2.
Its explicit platform/process branches, C-API runner, checked file service and
upstream-model CLI are captured by CHECKPOINT.json and the source archive, also
copied into this release's preservation folder. That WIP is not part of these
ISOs. Exact next port step: resolve the checkpoint's temp/backup-name collision,
failed-save FD cleanup and C-API rooting review items; compile/link the standalone
host against existing JavaScriptCore; then validate genuine guest file/edit/save
round trips, conflicts, UTF-8/CRLF, retained originals and read-only failures.
The full Code OSS workbench/runtime remains future work; the native Text Editor
retains its own name and implementation.

Native ISO SHA256: `687733226337035e3b408124abf19c7d8f3bdf6083df32b6f681d7e7cbc9f28a`
Normal ISO SHA256: `31feb3cc87d134769a8cbfba2290cc784a83c91686203e3c25d315be805bf700`
GUI SHA256: `c11e550f3a82800bfa9940c1fd92abe76e5322fa59553b73ddb479309518ecb7`
Kernel SHA256: `9e03c90f99e0d458cf766740e40c4e594b1b3dc55c10293b73d64957f16b04a0`
