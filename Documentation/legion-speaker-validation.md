# Lenovo Legion Pro 7i 16IAX10H: physical speaker validation

The physical listening candidate was
`build/modern-browser/native-amplifier-candidate32.iso` (SHA-256
`4971b71f83b65a9a9d6953f86ea749a34779b358ac7b00a3262a420787b3b59c`).
It includes the real ACPI reset path and `/bin/speaker-check`. Host firmware
and register-model tests, and QEMU recordings, do not establish audible output
from the physical PC. On resuming development, the user reported that the
physical speaker test works. This is a user-confirmed listening pass on
candidate32; no console transcript or separate per-channel observations were
supplied.
The current published candidate38 uses byte-identical kernel, speaker-check
and amplifier firmware. The physical result is retained on those matching
components; no separate physical boot of candidate38 is claimed.

## What the copied firmware establishes

`build/physical-audio-acpi/DSDT.aml` is the user's copied Lenovo table:
645473 bytes, checksum zero, SHA-256
`a5eb51ac942f7b05f8d014608a16db98c3ac91aa00c3d2c7efc7243128a175f8`.
ACPICA disassembly is preserved as `build/physical-audio-acpi/legion.dsl`.

- `\_SB.PC02.I2C2.SPKR` identifies `AWDZ8399`, subsystem `17AA3907`.
- Its `_CRS` has I2C2 addresses 0x35 and 0x34, at a 400-kHz maximum bus rate.
- The output resource is on `\_SB.PGPI`. The table calculates its GPIO with
  `PGPI.GNUM(0x0016040C)`: GPIO 140, GPP_A12.
- The next resource is an interrupt, computed with `GNUM(0x0016050B)`:
  GPIO 171. The driver leaves this input alone.
- PGPI is `INTC1084`, the Intel Meteor Point PCH controller. Its second
  memory resource contains GPP_A; firmware supplies the actual MMIO base.
- SPKR has no `_PR0`, `_PS0`, or `_DSD` in this table. I2C2's `_PS0` is
  empty. PCI D0 and LPSS reset are handled by the existing I2C driver.

The kernel evaluates these resources with LAI and maps the real firmware NVS
fields. It checks the supported codec/subsystem and GPIO controller, resource
bounds, pad ownership, configuration/transmit locks, pin mode and pull-up.
It resets the shared line high/low/high, holding each phase for 1 ms, once
before loading either amp. Failure returns without enabling speaker playback.
Firmware, DSP CRC, factory trim, watchdog and fade checks remain in force.

Primary programming references:
[Intel Meteor Point GPIO mapping](https://github.com/torvalds/linux/blob/master/drivers/pinctrl/intel/pinctrl-meteorpoint.c),
[Intel GPIO ownership, locks and pad programming](https://github.com/torvalds/linux/blob/master/drivers/pinctrl/intel/pinctrl-intel.c),
[AW88399 HDA initialization](https://github.com/torvalds/linux/blob/master/sound/hda/codecs/side-codecs/aw88399_hda.c),
[AW88399 hardware reset](https://github.com/torvalds/linux/blob/master/sound/soc/codecs/aw88399-lib.c).
The locally preserved AW88399 core reference is
`build/browser-media-scroll-baseline/linux-aw88399-lib.c`.

## Listening procedure

Boot this exact ISO manually using your existing removable-media/UEFI setup
when convenient. No host reboot, USB image write, boot-order change or physical
partition change has been performed by the agent. The following checks use the
running OS's console and RAM filesystem.

1. Disconnect headphones, HDMI/DisplayPort audio devices and Bluetooth audio.
   At the ModernOS console, run `speaker-check --info`, then `speaker-check`.
2. Confirm the 440-Hz first tone comes only from the physical left speaker,
   the 660-Hz second tone only from the right, and the third tone from both.
3. Confirm two seconds of silence during `PAUSE`, and that `RESUME` restores
   both speakers. Confirm there is no severe distortion or popping.
4. Run `speaker-check` a second time. This exercises repeated amp stop/start
   and HDA reset, rather than only the first playback after boot.
5. Report the exact console output and what you heard for left, right, both,
   pause and resume. A zero exit status or increasing DMA clock alone is not
   an audible pass. If the tool reports an error, stop and retain the output.

Useful boot messages are `AW88399 ACPI I2C2 ... reset=PGPI GPIO140 MMIO=...`,
`shared amplifier reset high/low/high completed`, and
`left/right speaker DSP firmware and protection loaded`. They identify resource
discovery, reset and firmware stages separately. Failure messages identify ACPI
reset checks, firmware/I2C initialization or playback-clock/DSP protection.

For a manual OSDev check, connect wired Ethernet, run
`gui --trace --browser-network-trace`, open Browser, navigate to
`https://osdev.org/`, and use the real verification widget. Record whether the
actual OSDev document appears. Native Wi-Fi 7 support is still unavailable.
Diagnostics record cookie names/attributes and HTTP identity; cookie values
and challenge bodies are not dumped.

## Recorded scope

The actual supplied `_CRS` templates and production GPIO/parser code pass
ASan/UBSan bounds, ownership, lock, configuration preservation and reset tests.
`speaker-check` passes on four-CPU KVM with recorded HDA left/right/both PCM,
a measured two-second paused device clock, and resumed stereo. QEMU records
active DMA and omits stopped-device time; its recorder is not evidence of a
physical acoustic silent interval. Physical listening is now confirmed by the
user's report that the speaker test works. The report is distinct from the
retained QEMU and register-model evidence.
