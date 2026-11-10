# Personal desktop r6

Profiles is the first Settings category and the default page. Choose an original avatar, edit the profile description, and use New/Rename/Switch for named preference sets. Each profile keeps its appearance, wallpaper, Dock pins/size/magnification, input, editor and Terminal preferences. Existing legacy settings and the version-1 profile registry migrate without overwriting the legacy file. Files, open windows, clipboard and process permissions remain shared; these are not authenticated user accounts.

Sliders support dragging, mouse wheel, arrow keys, Shift+arrow for larger adjustments, Home/End and Escape to revert an active drag. Wheel changes remain one notch per step even when the desktop scroll multiplier is high. Dock size ranges from 16 to 112 logical pixels and magnification from 0 to 100%; large preferred sizes are fitted to the display. Animated neighbors move apart, hit testing follows the drawn icons, and backdrop blur is cached across the animation. Right-click an app to pin/unpin it. Unavailable touchpad controls remain disabled.

Terminal settings apply its five palettes, text/background colors, ANSI-color toggle, opacity and native monospaced output font size to open Terminal windows. Below 100% opacity the body samples and blurs the actual backdrop. Sizes 9/11/13/15/17/19 use native antialiased glyphs at all supported DPI levels. The cooked command input bar retains normal UI font metrics. Choose readable text/background combinations. Large translucent windows at extreme HiDPI cost more to compose in software-emulated QEMU; the default remains opaque.

Shell selection applies to new Terminal windows, preserving existing processes. Coast is the default and supports the verified native nano raw-input edit/save workflow. GNU Bash 5.3.20 is a genuine upstream port, labeled basic: interactive commands and file writes are verified, while launching nano from Bash did not establish the required raw PTY mode. Use the Coast shell for full-screen applications. zsh, fish and Oh My Zsh are not installed. See TERMINAL-SHELL-STATUS.md for official dependency research and compatibility limits.

Password and sudo capability information is intentionally unavailable. No credentials, login service or privilege-enforcement change is included. GUEST-ACCOUNTS-DESIGN.md records the required guest identity, filesystem authorization, account database and elevation-broker design, and the specific future action-time approval boundary. Host accounts and security settings are outside scope.

Preferences saved on an attached writable guest FAT32 volume persist across desktop restarts/reboots; the tested profile layout is `.coast/profiles/<id>/desktop.ini` with `.coast/profiles.ini`. Diskless sessions use `/home/user/.config/profiles/` in RAM and clear on reboot, as Settings states. No host disk is attached by the launch command below.

## Run the tested native image

```sh
cd /home/camdenp/Desktop/Coast
make run-image ISO_IMAGE=build/coast-personal-20261024/Coast-Coast-Personal-native-tested.iso LINUX_DISK= FAT_DISK= SATA_DISK= QEMU_NETWORK='-nic none' QEMU_EXTRA_ARGS=-full-screen
```

At the guest console type `gui`. The native image requests 2560x1600 and starts at 150% guest UI scale. The alternative `Coast-Coast-Personal-tested.iso` requests 1280x800 at 100%. Ctrl+Alt+F toggles QEMU fullscreen; Ctrl+Alt+Escape leaves the desktop for the guest console. Closing the QEMU window ends that VM. `run-image` launches existing bytes without rebuilding the original ISO.
