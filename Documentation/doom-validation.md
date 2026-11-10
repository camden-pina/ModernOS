# Doom validation, 2026-10-15

The native Doomgeneric engine with source-built Freedoom Phase 2 passed real
QEMU gameplay on **four-CPU KVM** and **single-CPU TCG**. Both runs used a fresh
RAM filesystem, the complete candidate image, USB keyboard/mouse, and HDA with
48 kHz stereo S16 WAV capture. Both guests exited and released their owned
processes and exclusive-test markers before the next guest was admitted.

## Tested image and program

- Image: `build/continuation-20261015/coast-candidate04.iso`
- Image SHA-256: `5b80fcce86a1681271c578e0ccf71b802466d2f332ba7bcb90b08f915423ddc0`
- `/bin/doom` SHA-256: `6ab61b7299fb6f0a8af1772f87a878c7d701492bdd3cf1b521fcb64165ae49c6`
- Engine: official Doomgeneric commit `e257a09527062bfb986b6e5e0780839b6cbd5b71`.
- Data: source-built Freedoom 0.13.0 Phase 2, 32 maps, 3,610 lumps, 28,787,748 bytes.
- IWAD SHA-256: `502f40d3b0276a52e096b6ca0b12dcce744cf82e0c4d44031b978db4250f06e5`.

The IWAD is built from the exact pinned official source files, so its hash
honestly differs from the upstream binary ZIP's IWAD. Source manifests,
original BSD notices/credits and the engine GPL are retained. The image includes
corresponding engine/data sources, port code, build scripts and interface header
under `/usr/src/coast/doom`.

## Gameplay evidence

`tools/test_doom.py` performs the following operations through actual guest
execution and emulated USB input:

1. Runs `help doom` and `doom --info` and verifies packaged data/audio discovery.
2. Launches `doom --trace` and starts New Game through Escape, Enter, Enter.
3. Requires real engine `GS_LEVEL` state on MAP01.
4. Holds W, checks changed player coordinates/tic/frame hashes and different
   actual framebuffer captures.
5. Sends relative mouse X and checks changed player angle.
6. Holds Ctrl, checks real pistol sample dispatch and ammo consumption.
7. Verifies nonzero samples in the actual QEMU HDA WAV output.
8. Opens Escape, selects Quit with Q, activates with Enter, and confirms Y.
9. Requires `DOOM_EXIT`, a working shell echo, successful display reacquisition,
   and another shell echo after that check.

| Observation | KVM, 4 CPUs | TCG, 1 CPU |
| --- | --- | --- |
| Initial MAP01 position X | -12582912 | -12582912 |
| Position X after W | 7690436 | 7949365 |
| Engine tics across movement | 6 → 58 | 13 → 66 |
| Mouse angle | 0 → 4194304000 | 0 → 4194304000 |
| Pistol ammo | 50 → 47 | 50 → 47 |
| Captured stereo frames | 158995 | 181591 |
| Samples with absolute amplitude >100 | 262062 | 269478 |
| Menu quit, shell and display recovery | Pass | Pass |
| Owned process cleanup / marker release | Verified | Verified |

Position values are Doom's actual fixed-point coordinates. Framebuffer output
was 2560x1600 with a centered 4:3 game viewport. The KVM gameplay capture was
also inspected visually: MAP01 geometry/textures, pistol, status bar, aspect
ratio and colors rendered correctly.

## Evidence files

All original artifacts remain under `build/runtime-tests/`:

- `doom-c04-kvm4-hda-pass.json`
- `doom-c04-kvm4-hda-pass-before.ppm`, `-after.ppm`, `-menu.ppm`
- `doom-c04-kvm4-hda-pass-after.png` (lossless copy of the actual PPM capture)
- `doom-c04-kvm4-hda-pass.finalized.wav` and original `.wav`
- `uefi-4cpu-doom-c04-kvm4-hda-pass.log`
- `doom-c04-tcg1-hda.json`, matching before/after/menu PPMs and finalized WAV
- `uefi-1cpu-doom-c04-tcg1-hda.log`

Driver logs and owned-process cleanup records are in
`build/continuation-20261015/doom-c04-{kvm4-hda-pass,tcg1-hda}.{driver.log,runner.json}`.
Both successful records have return code 0, no remaining owned PIDs,
`cleanup_verified: true`, and `marker_released: true`.

Earlier failures are preserved too. Candidate03 exposed the missing fresh
`/home/user` parent directory; the startup now creates and verifies the complete
save-directory hierarchy and reports exact path errors. The first candidate04
oracle omitted Enter between the Quit shortcut and Y; only the test sequence
changed before the successful runs on the same image. Their logs, raw WAV and
screenshots were retained under their original unique prefixes.

## Host checks and limits

`python3 tools/test_doom_host.py` passed public-ABI layout checks and ASan/UBSan
harnesses for DMX bounds, resampling, panning, clipping, EAGAIN cursor rollback,
sample lifetime, bounded key/mouse burst delivery, the exact 63-mouse-packets
plus key-release regression, ASCII menu text, modifier aliases, kernel overflow
recovery, fresh directory creation, repeat creation and occupied-file errors.
Mixed-case unsafe scaling/pixel-format options and response files are rejected.
`make doom-check-sources` passed offline archive verification.

Only MAP01 was exercised interactively. All 32 campaign maps passed structural
checks, and all 100 upstream source WADs passed the data project's compatibility
checks; completing every map was not tested. This port retains vanilla-engine
limits. MIDI music was unavailable in this historical candidate04 run. Multiplayer and
joystick support remain unavailable. It owns
the display exclusively, so launch it from the console after exiting the native
desktop. AC97 was not part of these Doom guest runs. Recorded HDA PCM proves
device output; a person must listen to verify particular physical speakers.

See [the Doom guide](doom.md) for controls, source/build details and launch commands.


## Phase 2 music integration

The later source adds an original procedural MUS/SMF0/1 synthesizer; see
[the current Doom guide](doom.md). Host ASan/UBSan exercised all 35 Freedoom
scores, malformed inputs, controls and actual mixed-audio EAGAIN rollback.
The music02 KVM4 probe produced nonzero music-only HDA PCM, stable paused
frame counters, resumed output, and a separate all-zero `-nomusic -nosfx`
negative capture. Its report is
`build/continuation-20261015-phase2/music-kvm4-1792063307145020957/report.json`.
This is component evidence on that probe; final-image reports must be checked
separately. No full campaign playthrough or physical speaker test is claimed.


The final candidate10 image subsequently passed gameplay with HDA and the
music-only/pause/resume/silent-negative pair at both1and4CPUs under KVM.
Exact image/hash/cleanup reports are in
`build/continuation-20261015-phase2/final-features-1792071157499214000/`
and `final-music-1792071847984036880/`. Earlier failures remain preserved.
