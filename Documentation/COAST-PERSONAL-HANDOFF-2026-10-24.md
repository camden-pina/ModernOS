# Coast Coast Personal r6

Recorded 2026-10-24 21:21:55 UTC. Actual project: /home/camdenp/Desktop/Coast. This completes the requested launcher/sliders/avatars/Terminal pass. The source is applied, builds cleanly and has separate verified images. The old read-only permission rejection was resolved by one approved normal retry after clarified authorization. No permission check was bypassed; no approval is pending.

## Delivered behavior

- The exact Super-key launcher artifact was reproduced in Profiles r5: 34,396 differing pixels versus forced recomposition. Its hover region covered the entire app list, while pointer and keyboard each drew a separate highlight. R6 shares one selected app and damages the old/new rows. Pointer/keyboard transitions, filtering, dismissal and clicking the intended application are tested; cached/full-repaint comparisons now have zero differences.
- Settings opens on Profiles, with a prominent active-profile avatar, six original vector pictures and a description. Existing profile names, migration, version-1 registry and legacy file preservation remain. Appearance, wallpaper, Dock pins/density, input, editor and Terminal preferences are scoped to each profile. Files, open windows, clipboard and permissions remain shared.
- Numeric sliders support drag capture, outside release, Escape to revert a drag, wheel, arrows, Shift+arrows and Home/End. Wheel adjustments stay precise even with the scroll multiplier at maximum. Dock size is 16–112 logical pixels; magnification is 0–100%. Neighbors move apart, drawing and hit tests share fitted geometry, and the animated backplate reuses a stable real backdrop cache. Defaults remain 30px and 16%. Unavailable touchpad controls stay disabled.
- Terminal has five palettes, text/background choices, ANSI color control, 40–100% opacity, and native antialiased monospaced output sizes 9/11/13/15/17/19 at 100/125/150/200%. Lower opacity uses real sampled/blurred content behind the body. Footer text follows the selected foreground. The cooked input bar retains normal UI font metrics. Shell changes affect new windows without replacing running processes.
- The actual existing GNU Bash 5.3.20 binary was absent from the old desktop initramfs; the new images include it, complete upstream source/official patches, GPL license and build provenance. Real console/GUI commands, version queries and file writes pass. It is labeled “GNU Bash (basic)”: nano launched from Bash never established raw PTY mode. Native-shell nano edit/save and raw/canonical transitions pass. Full Bash job-control/signal compatibility is not claimed.
- zsh, fish and Oh My Zsh are assessed from official upstream documentation, not installed or shown as working. Fish needs a Rust/cargo toolchain unavailable on inspected PATH. A real zsh guest port and its prerequisite compatibility remain future work; no remote installer or disabled compaudit check was used.
- Password/sudo capability labels remain unavailable. No credentials, login, privilege helper or security enforcement was implemented. Checked syscall source returns UID/GID 0, but Bash on this frozen kernel reports UID/EUID 4294967295 (unsigned -1); source and runtime identity behavior must not be conflated. GUEST-ACCOUNTS-DESIGN.md records the proper identity/authorization/login/elevation design and future action-time approval boundary.

## Verified release

build/coast-personal-20261024/Coast-Coast-Personal-native-tested.iso requests 2560x1600, default 150%.
build/coast-personal-20261024/Coast-Coast-Personal-tested.iso requests 1280x800, default 100%.

Both use the frozen existing kernel. GUI, ISO and kernel SHA256s are in the release MANIFEST.json. The original build/os.iso and initramfs, all older Coast/Glass/Clarity/Polish/Profiles tested releases and paused handoffs are preserved.

The final candidate 08 images passed complete real-guest suites at 100% and 200%, including cold reboot, migration, profile separation, avatar/description bytes, slider edge input, menu interactions, launcher exact repaint comparison, Terminal materials and saved Bash/nano bytes. Final native-image 150% boot/profile screenshot/native nano round-trip also passes. A full 150% dark/light profile suite passed candidate 06 before the final wheel normalization/footer-color corrections. These corrections are covered by the final 100/200 suites. QA storage was fresh 64 MiB project-owned FAT/GPT images only; no host raw disk, host account, host setting or guest credential was modified.

ASan/UBSan checks pass: all 12 native font faces at four scales, profile path/name rejection,25,200 Dock fit/cache/neighbor/hit cases and 1,056 icon raster/cache/clip cases. Icon masks peak at 4 MiB and their owned allocation counter returns to zero. LeakSanitizer is unavailable under this execution environment's ptrace; host tests used detect_leaks=0, so this is not a claim of a global leak scan.

Matched 4-vCPU TCG/native 150% default opaque-Terminal drag: r5 mean 12.558 ms over 59 frames, r6 mean 12.844 ms over 58; maxima 15.926/15.825 ms. Default behavior is comparable in this sample. At the extreme 200%/112px/100%-magnification/60%-opacity setting, stable Dock caching reduced blur updates 68→29 and average composition 99.8→82.7 ms; large translucent-window dragging still cost about 129 ms in a small 3-frame/coalesced sample. It is not smooth at that extreme under software emulation. No claim of macOS parity or full acceleration is made. These timing comparisons used candidates04/06(extreme) and07(defaults); final08 changes only the footer foreground color after07, and its full guest regressions pass.

Screenshots and QA reports in the release are actual guest captures. Screenshot names identify the 200% test override. profiles-native-150pct.png shows the final native image's default scale. Local deliverables only: the unavailable Library upload was not retried, and nothing was published.

## Launch

cd /home/camdenp/Desktop/Coast
make run-image ISO_IMAGE=build/coast-personal-20261024/Coast-Coast-Personal-native-tested.iso LINUX_DISK= FAT_DISK= SATA_DISK= QEMU_NETWORK='-nic none' QEMU_EXTRA_ARGS=-full-screen

Type gui at the guest prompt. Ctrl+Alt+F toggles fullscreen; Ctrl+Alt+Escape returns to the console. This target launches existing image bytes without rebuilding the original ISO or attaching host disks. Diskless preferences are RAM-only and clear on reboot, as Settings states; persistence was tested on attached project-owned guest FAT32 storage.

## Paused work and next milestone

Trading and the browser allocator investigation were not touched. Code OSS M2 remains unchanged at customization-r6's sibling codeoss-m2/CHECKPOINT-20261024-native-build.json, with its native runner, file-service adapter and source archive retained. No M2 source was applied or guest-executed in this pass. The next authorized port milestone is its checked file-service/edit/save round-trip and tests, following that checkpoint; further UI feedback can take priority. Full Code OSS workbench requirements remain separate.

For this UI release there is no open functional regression or pending permission. Remaining limitations are the explicitly basic Bash integration, absent zsh/fish/Oh My Zsh, preference-only profiles, and high software-rendering cost at extreme translucent settings. Authentication/privilege changes need concrete action-time approval; do not silently fold them into UI follow-up.
