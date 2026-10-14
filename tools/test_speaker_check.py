#!/usr/bin/env python3
"""Capture the actual speaker-check PCM sequence in QEMU; no physical claim."""
import argparse
from pathlib import Path
import struct
import wave
from test_runtime import VM, WORK, assert_success


def exercise(iso, cpus, accel, label):
    capture = WORK / (label + '.wav')
    vm = VM(cpus, storage=False, iso=iso, label=label,
            accel=accel, audio_capture=capture, audio_device='hda')
    try:
        output = vm.command('speaker-check', timeout=35)
        assert_success(output)
        for marker in ('LEFT only', 'RIGHT only', 'BOTH', 'PAUSE: two seconds of silence',
                       'PAUSE verified:', 'device clock held', 'RESUME: both speakers', 'sequence submitted and drained'):
            assert marker in output, output
        assert_success(vm.command('speaker-check --info'))
    finally:
        vm.close()

    validate_capture(capture)

    print(f'PASS speaker-check: recorded left/right/both PCM, actual two-second paused device clock and resumed stereo '
          f'({cpus} CPUs, {accel}, HDA); physical speakers still require a person to listen', flush=True)


def validate_capture(capture):
    import array

    data = bytearray(capture.read_bytes())
    assert data[:4] == b'RIFF' and data[8:16] == b'WAVEfmt ' and data[36:40] == b'data'
    assert struct.unpack_from('<HH', data, 20) == (
        1, 2) and struct.unpack_from('<H', data, 34) == (16,)
    assert (len(data) - 44) % 4 == 0

    # The QEMU backend records active DMA and omits inactive time. Its zero
    # lengths on termination are finalized from the actual recorded bytes.
    # Preserve the original; do not insert samples to simulate a silent pause.
    struct.pack_into('<I', data, 4, len(data) - 8);
    struct.pack_into('<I', data, 40, len(data) - 44)
    finalized = capture.with_suffix('.finalized.wav');
    finalized.write_bytes(data)
    with wave.open(str(finalized), 'rb') as f:
        rate = f.getframerate();
        samples = array.array('h', f.readframes(f.getnframes()))
    events = []
    step = 2 * (rate // 100)
    for at in range(0, len(samples) - step, step):
        left = max(abs(x) for x in samples[at:at + step:2])
        right = max(abs(x) for x in samples[at + 1:at + step:2])
        kind = 'both' if left > 300 and right > 300 else 'left' if left > 300 else 'right' if right > 300 else 'silence'
        if events and events[-1][0] == kind:
            events[-1][1] += .01
        else:
            events.append([kind, .01])

    totals = {kind: sum(seconds for k, seconds in events if k == kind)
              for kind in ['left', 'right', 'both']}
    assert totals['left'] > 1.7 and totals['right'] > 1.7 and totals['both'] > 3.5, events
    assert next(k for k, s in events if k != 'silence') == 'left', events


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True)
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--label', default='speaker-check')
    args = parser.parse_args();
    exercise(args.iso, args.cpus, args.accel, args.label)
