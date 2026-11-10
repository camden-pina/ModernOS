# Optional emulated network/audio terminal supplement

Status: the local poweroff and reboot pair passed on the preserved full baseline
image, with four serial boots total, 40 host checks, unchanged input/helper
hashes and verified owned cleanup. The two cases remain separate from the
39-release + 18-power inventory. This is emulated-device baseline evidence;
a changed candidate image requires a new run, and physical hardware is untested.

## Scope and observations

`tools/test_power_devices.py` runs one S5 or FADT-reset case against the exact
supplied final ISO, with four CPUs, TCG, 2 GiB RAM and a fresh 64 MiB FAT NVMe
volume. It never injects guest executables, AML, boot options or kernel code.
The existing final-image inspector verifies embedded kernel/shell/GUI and records
the actual archive hash. Only its newly generated inspection copies are removed
after a durable provenance record; all media, traces, PCM and reports remain.

The primary VM also has QEMU e1000e user networking and `intel-hda` +
`hda-duplex`, captured to a WAV file without physical audio output. The supplement
explicitly selects fixed 48000-Hz stereo signed-16-bit output through the optional
`audio_capture_rate=48000` VM keyword. Omitting that keyword preserves the common
helper's legacy command and checkpoint configuration. An ephemeral
loopback HTTP server serves only a synthetic 256 KiB body, throttled and bounded
to 15 seconds. No external network service is contacted. Guest `curl --max-time
20 ... &` produces actual RX/TX traffic; `speaker-check &` produces bounded PCM
and leaves the shell available. `netinfo` counters must increase; two
`speaker-check --info` samples must show queued PCM and increasing played frames.
The HTTP handler must still be active with a recent successful write before the
request. The actual emulated NIC and HDA initialization logs are mandatory.

Read-only QMP `query-pci` discovers exact BDFs and BAR0 addresses. HMP `xp /1wx`
reads these guest-physical MMIO registers before request and after the actual
terminal event:

- NIC RCTL/TCTL enable bits and CTRL reset bit
- HDA selected output stream RUN, CORB RUN and RIRB DMA bits
- HDA DMA-position enable and INTCTL

The primary VM adds `-no-shutdown` to the existing `-no-reboot`, retaining stopped
emulation for post-action inspection. Actual guest-origin QMP `SHUTDOWN` with
`guest-shutdown` or `guest-reset` remains mandatory, as do BSP/AP/flush/COMMIT
serial markers. Host teardown is never terminal-action evidence. If this QEMU
policy or read-only MMIO inspection behaves differently, the case fails closed.
The local baseline pair below establishes this policy for that image and the
installed QEMU only. QMP and serial use independent pipes: after the terminal
event and stopped status, the harness continues draining serial within the
original terminal timeout until every existing kernel marker is present. QMP
observations and serial-drain counts remain available even when validation
fails; the serial-marker oracle is unchanged.

Selected QEMU traces prove receive DMA occurred, then receive/transmit disable,
controller reset and matching-BDF PCI command bus-master clearing. The HDA
playback `dac` stream and stream number must stop before its matching-BDF
bus-master clear. Trace evidence is snapshotted **before host teardown**, so
teardown reset cannot satisfy the oracle. The installed trace metadata and
parsers are a strict contract; the actual local pair exercised these installed
backend/events and preserved the pre-teardown trace snapshots.

The report separates `audio_running_at_network_stop` and
`audio_drained_before_network_stop`, based on that playback stream's trace state.
`active_audio_at_stop` remains null: a previous nonzero queue, PCM capture or run
transition does not establish an instantaneous DMA-at-stop sample. Natural drain
during the one-second pending grace is accepted only with this lower, explicit
scope and independently disabled post-action engines/bus mastering. This is not
an active-DMA-at-stop proof. The partial WAV validator checks genuine nonzero
48-kHz stereo PCM, not completion of the full speaker-check listening sequence.

Each action also requires host fsck/exact file content and a second fresh boot
of the same final ISO to read the persisted file and run `check`. Thus the pair
is two cases/four serial boots. The reopen has networking/audio disabled and is
persistence evidence only.

## Safe preparation checks

From the checkout containing these files:

```sh
python3 -B tools/test_power_devices_host.py
python3 -B tools/test_power_devices.py --iso /FINAL/full.iso \
  --kernel /FINAL/kernel.elf --shell /FINAL/coast-sh --gui /FINAL/gui \
  --kind poweroff
```

The first command combines pure/mocked oracle checks with short-lived owned
host-process fixtures. It verifies real direct-parent admission, orphan and
escaped-session cleanup, timeout and SIGTERM handling, and that an unrelated
pre-existing child survives. There is no compiler, guest or real HTTP server.
The process checks need Linux pidfds, process-local child reaping and private
Unix-socket IPC; run them on the admitted local host if an execution sandbox
forbids that IPC. Do not change host security settings. The second command
prints a plan; it does not read final images or run inspection, tools, servers
or guests. Neither preparation command reserves the project guest slot.

## Local supervised pair

`tools/run_power_devices_local.py` is the local-host path. Its default invocation
prints the fixed harness command and executes nothing. It does not need the
cloud pilot, its environment setup, file-backed RAM or a QEMU wrapper. The exact
2 GiB/four-CPU/TCG scenario and unchanged-ISO inspector remain the same.

For execution, `--exclusive-ready` attests that every competing builder and
other guest has exited; the flag does not stop them. The explicit
`--min-available-kib` must be at least 4194304 (4 GiB); the commands below choose
8388608 (8 GiB). `MemAvailable` is checked before launch, during child admission
and before each of the two boots. The same checkout's
`build/browser-guests-active` marker is acquired atomically with `O_EXCL` and
`O_NOFOLLOW`. Any existing marker or cleanup-recovery blocker, including a
dangling symlink, refuses admission. No existing marker is borrowed or removed.

The supervisor launches only the fixed power-device harness as its direct
child, with a fresh session/process group. An inherited private Unix socket
carries the actual child/supervisor start identities, marker inode/token and
memory floor. The child checks Linux peer credentials, its direct parent,
session, token and memory; setting local environment strings is insufficient.
The lease is checked again before the persistence reopen boot. The original
cloud admission remains the default for direct `test_power_devices.py` calls;
`--admission local` requires this genuine local reservation.

After the normal guest suite has stopped, set `FINAL_ISO`, `FINAL_KERNEL`,
`FINAL_SHELL`, `FINAL_GUI` to the exact full package inputs. From this checkout:

```sh
python3 -B -u tools/run_power_devices_local.py --execute --exclusive-ready \
  --min-available-kib 8388608 --supervisor-timeout 900 \
  --iso "$FINAL_ISO" --kernel "$FINAL_KERNEL" --shell "$FINAL_SHELL" \
  --gui "$FINAL_GUI" --kind poweroff --memory 2G --timeout 60
# Inspect a passed harness and supervisor report, cleanup_verified=true,
# marker_released=true, and no project marker/blocker before the second action.
python3 -B -u tools/run_power_devices_local.py --execute --exclusive-ready \
  --min-available-kib 8388608 --supervisor-timeout 900 \
  --iso "$FINAL_ISO" --kernel "$FINAL_KERNEL" --shell "$FINAL_SHELL" \
  --gui "$FINAL_GUI" --kind reboot --memory 2G --timeout 60
```

Do not overlap these commands. Local supervisor evidence is under
`build/power-device-supervision/<action>-<unique>/report.json` and `harness.log`;
the harness's own device evidence remains in the directory described below.
The command, reservation, actual process identities, memory observations,
return code, timeout/interruption and cleanup outcome are retained. The helper
manifest now includes the local runner's bytes. A supervisor pass also requires
the harness to exit successfully and all owned descendants to be gone.

On normal completion, nonzero exit, INT, TERM or timeout, bounded cleanup uses
pidfds and rechecks stable process identities. The process-local subreaper also
collects descendants that outlive the harness or escape its group; it excludes
pre-existing unrelated child identities. Only the exact owned marker inode and
token are released after verified cleanup. Unconfirmed cleanup or changed marker
ownership retains the reservation and writes a recovery-required blocker
without replacing an existing blocker. It never signals an unrelated group or
a reused PID. An abrupt supervisor kill leaves the exclusive marker in place
and requires manual identity verification; a later run never auto-clears it.
These host checks establish supervision behavior, not guest terminal results.

## Observed local baseline results

Both cases passed with four CPUs, TCG, 2 GiB RAM, fresh FAT NVMe media, the exact
preserved full ISO and a second fresh unchanged-ISO persistence boot. Each had
actual RX/TX progress, nonzero 48000-Hz stereo PCM, a guest-origin terminal event,
BSP/AP/flush/COMMIT serial markers, disabled final device-engine MMIO and ordered
matching-BDF PCI bus-master clearing. Both reported 32 receive-DMA trace events
and `audio_running_at_network_stop=true`. `active_audio_at_stop` remains null.
Host fsck, exact persisted bytes and the fresh-boot read/`check` passed.

| Case | Guest QMP reason | Captured partial PCM frames | Harness report |
| --- | --- | ---: | --- |
| Poweroff | `guest-shutdown` | 53855 | `build/power-device-tests/poweroff-1792045262455179322-6b4d77c3/report.json` |
| Reboot | `guest-reset` | 53908 | `build/power-device-tests/reboot-1792045309359227131-62d7af10/report.json` |

Supervisor reports are in `build/power-device-supervision/` under
`poweroff-1792045262373847717-b3b65fd3` and
`reboot-1792045309274517148-cee976a7`. Both record `result=passed`,
`cleanup_verified=true`, and `marker_released=true`. After each action, the
project guest marker and recovery blocker were independently verified absent.

The tested full ISO SHA256 is
`a89d7bcaa86543c253857e5da2f71d60be472d13b65fc0e335bf23dbdd6238b9`.
Its kernel SHA256 is
`d6e8221da9ff2a605d289f4fa7a10142fffd726e5dff947f765c5b630b68e24f`,
and embedded full-initramfs SHA256 is
`3520915f36bb82fa172792d160bb58c4a16653b2fc73171712e7458e47d1d8fb`.
The reports also identify exact shell/GUI and unchanged helper hashes.

Two earlier failed attempts remain unchanged under
`build/power-device-tests/poweroff-1792044819506368338-5e954a79` and
`poweroff-1792045040084630175-139bf52b`. The first exposed the serial/QMP
observation race; the second passed terminal/MMIO/trace checks but the strict
PCM validator rejected QEMU's default 44100-Hz capture. The fixes added bounded
serial draining and explicit capture settings. Neither failed attempt is
counted as a pass; no kernel or guest image was changed.

## Exact deferred cloud pair

Run only after the renderer and every real builder have exited, the parent
reserves an exclusive serial guest slot, and the separate reviewed file-RAM
empirical gates have passed for the actual final image/cache configuration.
Apply this test-only patch to the **main** checkout first. The main pilot's
marker and this harness must share the same checkout root. Do not run the
isolated-worktree harness beneath the main-checkout pilot.

Set `FINAL_ISO`, `FINAL_KERNEL`, `FINAL_SHELL`, `FINAL_GUI` to the complete final
package's actual files, not a lean checkpoint. Then, from the main checkout:

```sh
source /workspace/shared/host-tools/env.sh
source build/full-userland-env.sh
python3 -u build/qemu-file-ram-pilot.py \
  --builders-stopped --guest-slot-reserved --tcg-cache-64 \
  --timeout 900 --iso "$FINAL_ISO" -- \
  python3 -u tools/test_power_devices.py --execute \
  --iso "$FINAL_ISO" --kernel "$FINAL_KERNEL" --shell "$FINAL_SHELL" \
  --gui "$FINAL_GUI" --kind poweroff --memory 2G --timeout 60
# Inspect success, immutable inputs, no retained marker/blocker and full owned
# teardown before starting the second command. Never overlap the two pilots.
python3 -u build/qemu-file-ram-pilot.py \
  --builders-stopped --guest-slot-reserved --tcg-cache-64 \
  --timeout 900 --iso "$FINAL_ISO" -- \
  python3 -u tools/test_power_devices.py --execute \
  --iso "$FINAL_ISO" --kernel "$FINAL_KERNEL" --shell "$FINAL_SHELL" \
  --gui "$FINAL_GUI" --kind reboot --memory 2G --timeout 60
```

Cache mode is explicit and must match the prior empirical gate. These cloud commands
remain a deferred plan, not passing evidence. The harness
refuses execution without the actual pilot's marker/token, direct parent and
fresh owned process group; it does not create/spoof a whole-suite marker.

Evidence is under `build/power-device-tests/<action>-<unique>/`: immutable input
provenance, original/pre-teardown traces, register snapshots, HTTP counters,
partial WAV, FAT media, original and copied serial logs, result and SHA256
manifest. Imported helper bytes and supplied ISO/kernel/shell/GUI are checked
before/after. Pilot command/launch/telemetry reports must be archived alongside
this directory by the release coordinator. Existing suite evidence is untouched.

Nested host tools remain in the pilot-owned group; unreviewed preexec/session
escapes are refused. INT/TERM unwinds the harness, closes its VM/server, then
bounded pidfd-verified owned-group cleanup/reaping runs. Unconfirmed cleanup
writes the existing recovery-required blocker without overwriting any owner;
all compiler/guest admission must remain held until identity-verified recovery.
No automatic deletion of another stage's marker or blocker occurs.
