# Desktop profiles

A profile stores one set of desktop preferences. Settings > Profiles can create,
rename and switch up to eight profiles. New profiles copy the current settings.
The sidebar always identifies the active profile. View or Ctrl+1 through Ctrl+7
selects Settings categories; Tab traverses controls, and arrows on the sidebar
select categories. Short windows use compact navigation and scrollable content.

Profiles scope appearance, wallpaper and placement, display scale, workspaces,
mouse/touchpad preferences, Dock size/magnification/pins, and Text Editor settings.
Switching saves the outgoing preferences and loads the selected set, including
DPI changes. The recent-file list resets on switch. Files, open windows, clipboard,
application state and OS permissions remain shared. Home remains `/home/user`.
Authentication and OS account/session isolation remain future work. These profile
names are not UID/GID records. No credentials or persistent access are created.

## Storage and migration

The existing preference-location policy supplies a base directory: `.coast`
on the selected writable guest FAT volume, or `/home/user/.config` in RAM.
The UI displays the actual active config path and storage lifetime.

- `BASE/profiles.ini` stores format version, active ID and display names.
- `BASE/profiles/default/desktop.ini` stores Default preferences.
- `BASE/profiles/profile-N/desktop.ini` stores another set's preferences.

An absent registry starts with Default. When its new config is absent, Default
loads supported values from the old `BASE/desktop.ini`, then image defaults if
necessary. The first save writes the new namespace. The old file stays unchanged;
unknown old fields remain available there. Newly created IDs are generated from
an available bounded namespace, independently of display names. Names accept
1-40 printable ASCII characters, excluding leading/trailing spaces.

Example registry:

```ini
version=1
active=profile-1
default=Default
profile-1=Studio
```

Disk-backed preferences and the active selection survive reboot. RAM profiles
survive desktop relaunch during the current boot and disappear at reboot.
The diskless demo launch does not attach a persistent volume.

## Errors and limits

Each config/registry write completes a temporary file and fsync before publishing
with no-replace renames and an original backup. Existing backup/temp collisions
cause a visible error. A malformed/unsupported registry is preserved and disables
profile writes. Invalid destination types and read-only writes are refused.
Unregistered directories from an interrupted creation are preserved and skipped
when allocating IDs. This is not a transaction across all profile files and is
not designed for concurrent desktop sessions writing the same registry.

There is no profile deletion UI, login screen, protected credential store,
separate user home or process credential change in this foundation. Implementing
secure guest accounts would require a separately reviewed UID/GID/permission,
authentication, session-isolation and data-migration design. Host accounts and
security settings are outside this feature.

## Verification

`tools/test_profiles_desktop.py --iso IMAGE --label UNIQUE --scale 2
--output-root PATH` creates only a new disposable guest GPT/FAT image under PATH.
It verifies real menu/sidebar input, preference migration/separation, DPI changes,
reboot behavior and cursor/hover pixel equivalence. Scale indices 0,1,2,3 mean
100%,125%,150%,200%. `tools/test_profiles_format_host.py` sanitizes production
identifier/name validation. Final release evidence belongs in the dated UI
handoff and `build/coast-profiles-20261024`, not this general guide.
