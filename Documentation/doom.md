# Native Doom and Freedoom

Coast includes the real Doomgeneric engine with **Freedoom 0.13.0 Phase 2**
game data. The engine runs as a static user program and uses the existing
Coast display, USB input, clock and stereo audio interfaces. No proprietary
Doom WAD is included.

From the Coast console:

```text
doom
doom -warp 1 -skill 2
doom --help
doom --info
```

The first command opens the title screen. Press Escape, select New Game with
Enter, then choose a difficulty. `-warp 1 -skill 2` starts MAP01 directly.
From a desktop Terminal, the same `doom` command opens a normal managed game
window. The desktop launcher also has a **Doom** entry. The compositor scales
its copied 320x200 software bitmap to a 4:3 viewport inside normal resizable,
maximizable window chrome. Focus loss releases held game keys. Click the game
to capture relative pointer input; F10 releases it. Alt+F4 closes the window
and the engine returns to its Terminal or is reaped by its launcher owner.
The existing sound effects/music backend remains in use.

At a native console with no compositor, `doom` retains its exclusive display
backend. `doom --console` selects that backend explicitly; `--windowed`
requires a running compositor. A compositor already owning the display prevents
exclusive console acquisition. These software paths do not establish physical
GPU acceleration or general SDL/Wayland/X11 support.

| Input | Action |
| --- | --- |
| W/S or Up/Down | Move forward/back |
| A/D | Strafe left/right |
| Left/Right or mouse X | Turn |
| Ctrl or left mouse button | Fire |
| E or Space | Open a door/use a switch |
| Shift | Run |
| Alt + Left/Right | Strafe |
| 1–7 | Select a weapon |
| Tab | Automap |
| Escape | Game menu |
| Ctrl+Alt+Escape | Close the focused game window, or restore the native console |

Inside a managed game window, the capture applies to guest pointer routing.
Any QEMU host-window capture is separately controlled by QEMU.
Mouse Y does not move the player. USB HID keys supply real press/release state;
serial text is useful for menus and diagnostics; continuous movement requires USB keys.
The backend scales Doom's 320x200 image to a centered 4:3 viewport with black
bars when needed, preserving the classic display aspect ratio.

Sound effects are decoded from their real DMX WAD samples, linearly resampled,
panned and mixed into Coast's 48kHz stereo S16 PCM queue. Doom's effect-volume
menu controls work. Up to 16 low-level channels are supported; the original
engine defaults to eight. MUS and Standard MIDI format0/1 music use an original
32voice procedural synthesizer, mixed through the same48kHz native queue.
The synthesizer supplies instrument-family timbres and percussion without
third-party instrument samples. It is an approximation of the score's timbres;
it does not reproduce a specific commercial synthesizer or OPL chip.
Music volume, game pause/resume, looping, sustain, pan, program changes and
pitch bend are supported. Use `-nomusic`, `-nosfx` or `-nosound` as needed.
Network multiplayer, joystick input and general SDL applications remain outside
this port. If no supported audio device is present, the game reports that and
continues silently.

Use the existing QEMU audio configuration, or select HDA explicitly:

```sh
make run-image
make run-image QEMU_AUDIO='-audiodev sdl,id=audio0 -device ich9-intel-hda -device hda-duplex,audiodev=audio0'
```

Configuration and savegames live under `/home/user/.doom`; the guest root is
in RAM. Copy that directory onto a writable FAT32 mount to keep saves across
reboots. The unmodified engine retains its original limits; some large maps
and large savegames can exceed vanilla Doom limits. This is not a Boom/GZDoom
compatibility port. `-iwad /path/to/an/owned.wad` can select other compatible
data. Pixel-format/scaling overrides and response files are rejected because
the native backing buffer has a fixed layout.

## Build and preserve source

```sh
make doom-sources        # restore/verify pinned source archives
make doom-check-sources  # offline checksum verification
make doom                # engine + Freedoom data; one compiler process at a time
make iso                 # include the resulting program/data in a complete image
make test-doom-host
make test-doom           # QEMU gameplay + recorded PCM; rebuilds the ISO first
```

`tools/build_doom.py --engine-only` builds the game binary using the existing
static musl toolchain without rebuilding unrelated programs. Its log is
`build/doom/build.log`. The data builder is `tools/build_freedoom.py`; its cache
is separate under `build/doom-freedoom`. `make iso` includes Doom automatically.

Doomgeneric is pinned at commit
`e257a09527062bfb986b6e5e0780839b6cbd5b71` from
[its official repository](https://github.com/ozkl/doomgeneric/tree/e257a09527062bfb986b6e5e0780839b6cbd5b71).
`userland/doom/sources.json` records every retained upstream source/build/license
file's size, Git blob ID and SHA-256. Optional screenshots are excluded. Its
source archive is a deterministic local reconstruction of those exact files,
with SHA-256 `d5fc313d4c24f68ec906279693a7416052383b9ba6434678aaa8fbd894bd4508`.
The owned builder records its adaptations in the extracted cache: writable
config/save paths, guaranteed process exit in `I_Quit`, bounded combined input
delivery with queued key releases, and secondary arrow/Space gameplay bindings.
The ASCII keys remain available to menu shortcuts and save descriptions. Display/input and sound are owned
platform backends rather than SDL shims.

Freedoom and DeuTex source archives have separate manifests in
`userland/vendor/`. Their official source bytes, licenses and build provenance
are preserved. The IWAD is built locally from pinned source, so its hash need
not equal the separately packaged upstream ZIP's IWAD hash. The engine is
GPL-2.0-or-later; Freedoom data is BSD-3-Clause. The complete image installs
source archives and this port under `/usr/src/coast/doom`, with data notices
and credits under `/usr/share/games/doom` and the engine GPL under
`/usr/share/licenses/native-programs`.

`tools/test_doom.py` starts a game through real USB menu input, checks engine
state and player motion, checks a changed captured framebuffer, turns with
relative mouse input, fires and consumes ammo, verifies nonzero device PCM,
opens Escape, quits through the menu, and runs shell/display checks after exit.
Its JSON evidence, PPM screenshots, serial log and finalized WAV are saved under
`build/runtime-tests`. A PCM capture demonstrates submitted device audio; a
person must listen to verify a particular host's physical speakers.

`--trace` logs actual engine frame hashes, map/tic/player coordinates, input and
audio counters. `--frames N` ends a bounded diagnostic session after N rendered
frames, saving configuration and restoring the console.

The [2026-10-15 validation record](doom-validation.md) documents successful
KVM4 and TCG1 gameplay, exact tested image/program hashes, screenshots, recorded
HDA PCM, host regression checks and remaining limits.

## Music validation

`python3 tools/test_doom_music_host.py` checks bounded MUS/MIDI parsing,
synthetic timing/control cases, malformed input and all35retained Freedoom
scores under ASan/UBSan. The mixer host probe also checks identical PCM after
a native EAGAIN retry. `tools/test_doom_music.py --iso IMAGE --cpus 4 --accel kvm
--label UNIQUE` records actual HDA output from music-only gameplay, verifies
pause/resume advances the score correctly, then checks a separate `-nomusic
-nosfx` guest is silent. These passed on the phase2 music02 probe. The final
integrated image must rerun the gate. SMPTE timing and MIDI format2 are rejected;
score bytes/events/tracks/duration and polyphony have explicit resource bounds.

The public copied-window ABI is `include/uapi/window.h` (private syscall417).
Only the exclusive display owner can register as compositor. Client IDs and
queues belong to the creating process. Frame dimensions, strides and user
buffers are validated, and a complete back buffer is swapped only after a
successful copy. Focus changes, bounded queue overflow and close are explicit
events. Client exit and compositor release reclaim resources. Browser roles
cannot access the service. This first ABI supports four clients up to1024x768;
Doom uses320x200, and no direct GPU memory is mapped.

`tools/test_doom_window.py` exercises managed Terminal/launcher launches,
actual keyboard/pointer gameplay, aspect-preserving resize, focus release,
normal/menu close, clean reap/relaunch and HDA PCM. Its music-only and silent
controls distinguish real score output from sound effects. `window-check`
exercises the actual guest ownership/frame/focus/overflow/exit protocol.
