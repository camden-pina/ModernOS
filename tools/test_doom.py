#!/usr/bin/env python3
"""Prove menu-started Doom gameplay, HID/mouse input, PCM and display release."""
import argparse
from array import array
import hashlib
import json
from pathlib import Path
import re
import struct
import time
from test_runtime import VM, WORK, assert_success

FRAME = re.compile(r'DOOM_FRAME frame=(\d+) hash=([0-9a-f]+) state=(\d+) menu=(\d+) '
                   r'map=(\d+) tic=(\d+) x=(-?\d+) y=(-?\d+) angle=(\d+) ammo=(\d+)')


def frame(vm, state=0, menu=0, timeout=30):
    text = vm.until(fr'DOOM_FRAME [^\n]* state={state} menu={menu} [^\n]*\n'.encode(), timeout)
    matches = [m for m in FRAME.finditer(text) if int(m[3]) == state and int(m[4]) == menu]
    assert matches, text
    m = matches[-1]
    return dict(zip(('frame', 'hash', 'state', 'menu', 'map', 'tic', 'x', 'y', 'angle', 'ammo'),
                    [int(m[1]), m[2], *map(int, m.groups()[2:])]))


def event(key, down):
    return {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': key}}}


def key(vm, name, hold=.12, observe=True):
    vm.qmp('input-send-event', {'events': [event(name, True)]})
    time.sleep(hold)
    vm.qmp('input-send-event', {'events': [event(name, False)]})
    if observe:
        usage = {'esc': 41, 'ret': 40, 'w': 26, 'ctrl': 224, 'q': 20}[name]
        return vm.until(f'DOOM_INPUT hid={usage} down=0 key='.encode(), 15)


def screenshot(vm, label):
    path = WORK / (label + '.ppm')
    vm.qmp('screendump', {'filename': str(path)})
    data = path.read_bytes()
    assert data.startswith(b'P6\n') and len(data) > 320 * 200 * 3
    return {'path': str(path), 'sha256': hashlib.sha256(data).hexdigest()}


def capture_check(path):
    data = bytearray(path.read_bytes())
    assert data[:4] == b'RIFF' and data[8:16] == b'WAVEfmt ' and data[36:40] == b'data'
    assert struct.unpack_from('<HHI', data, 20) == (1, 2, 48000)
    assert struct.unpack_from('<H', data, 34)[0] == 16
    pcm = array('h', data[44:])
    nonzero = sum(abs(sample) > 100 for sample in pcm)
    assert nonzero > 2000, f'Insufficient actual device PCM: {nonzero}'
    struct.pack_into('<I', data, 4, len(data) - 8)
    struct.pack_into('<I', data, 40, len(data) - 44)
    fixed = path.with_suffix('.finalized.wav')
    fixed.write_bytes(data)
    return {'path': str(fixed), 'nonzero_samples': nonzero, 'sample_frames': len(pcm) // 2}


def exercise(iso, cpus, accel, audio_device, label=None):
    label = label or f'doom-{cpus}cpu-{accel}-{audio_device}'
    capture = WORK / (label + '.wav')
    capture.unlink(missing_ok=True)
    result = {'label': label, 'iso': str(Path(iso).resolve())}
    vm = VM(cpus, storage=False, iso=iso, network=False, label=label,
            accel=accel, audio_capture=capture, audio_device=audio_device, audio_capture_rate=48000)
    try:
        guide = vm.command('help doom', timeout=25)
        assert_success(guide)
        assert 'Freedoom Phase 2' in guide and 'console' in guide, guide
        info = vm.command('doom --info', timeout=25)
        assert_success(info)
        assert 'available' in info and '48000Hz' in info, info
        vm.send('doom --trace\n')
        startup = vm.until(rb'DOOM_READY|os:[^\n]*\$ $', 45)
        assert 'DOOM_READY' in startup, startup
        frame(vm, state=3, menu=0, timeout=45)
        # Start through the actual game menu, using the emulated USB keyboard.
        key(vm, 'esc')
        frame(vm, state=3, menu=1)
        key(vm, 'ret')
        time.sleep(.25)
        key(vm, 'ret')
        initial = frame(vm, timeout=45)
        result['initial'] = initial
        result['before'] = screenshot(vm, label + '-before')
        key(vm, 'w', hold=1.1)
        moved = frame(vm)
        assert (initial['x'], initial['y']) != (moved['x'], moved['y']), (initial, moved)
        assert moved['tic'] > initial['tic'] and moved['hash'] != initial['hash'], (initial, moved)
        result['moved'] = moved
        result['after'] = screenshot(vm, label + '-after')
        assert result['after']['sha256'] != result['before']['sha256'], result
        # Relative mouse turns feed real Doom ev_mouse events.
        vm.qmp('input-send-event', {'events': [{'type': 'rel', 'data': {'axis': 'x', 'value': 24}}]})
        turned = frame(vm)
        assert turned['angle'] != moved['angle'], (moved, turned)
        result['turned'] = turned
        fire_trace = key(vm, 'ctrl', hold=1.2)
        assert re.search(r'DOOM_SFX start=[0-9]+ name=pistol\b', fire_trace, re.I), fire_trace
        result['pistol_trace'] = next(line for line in fire_trace.splitlines()
                                      if 'name=pistol' in line.lower())
        fired = frame(vm)
        assert fired['ammo'] < turned['ammo'], (turned, fired)
        result['fired'] = fired
        audio = vm.until(rb'DOOM_AUDIO enabled=1 sounds=[1-9][0-9]* submitted=[1-9][0-9]* nonzero=[1-9][0-9]*[^\n]*\n', 25)
        result['audio_trace'] = audio.splitlines()[-1]
        key(vm, 'esc')
        frame(vm, menu=1)
        result['menu'] = screenshot(vm, label + '-menu')
        # The menu's Quit Game path must save config and release the display.
        key(vm, 'q')
        key(vm, 'ret')
        time.sleep(.2)
        key(vm, 'y', observe=False)
        vm.until(rb'DOOM_EXIT[^\n]*returned to console[\s\S]*os:[^\n]*\$ $', 25)
        assert_success(vm.command('echo DOOM_SHELL_RESTORED', 'DOOM_SHELL_RESTORED\n'))
        assert_success(vm.command('check --display-exit', timeout=20))
        assert_success(vm.command('echo DOOM_DISPLAY_REACQUIRED', 'DOOM_DISPLAY_REACQUIRED\n'))
    finally:
        vm.close()
    result['pcm'] = capture_check(capture)
    (WORK / (label + '.json')).write_text(json.dumps(result, indent=2) + '\n')
    print(f'PASS native Doom: USB menu started MAP01; WASD moved real player; relative mouse turned; '
          f'fire used ammo; device PCM nonzero; Escape/quit restored shell/display ({label})', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True)
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='tcg')
    parser.add_argument('--audio-device', choices=('ac97', 'hda'), default='hda')
    parser.add_argument('--label', help='Unique artifact prefix for a preserved candidate run')
    args = parser.parse_args()
    exercise(args.iso, args.cpus, args.accel, args.audio_device, args.label)
