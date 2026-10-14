#!/usr/bin/env python3
"""Measure actual guest composition/upload times during live window dragging."""
import argparse
from datetime import datetime
import re
import time
from test_runtime import VM
from test_desktop import DesktopTest, fixtures


def report_display(vm, started, ended, display):
    """Measure surface updates separately from guest render timings.

    Trace timestamps come from QEMU's log backend. A surface switch records
    GTK has a frontend switch trace. SDL uses QEMU's surface creation trace,
    which measures publication to the display core, not physical scanout.
    """
    path = vm.qmp_path.with_suffix('.display-trace')
    trace = path.read_text()
    refresh = re.findall(r'console_refresh interval (\d+) ms', trace)
    gtk = display.split(',')[0] == 'gtk'
    event = 'gd_switch' if gtk else 'displaysurface_create_from'
    result = 'HOST_DISPLAY' if gtk else 'QEMU_SURFACE'
    updates = []
    for line in trace.splitlines():
        if f' {event} ' in line:
            timestamp = datetime.fromisoformat(line.split()[0]).timestamp()
        else:
            match = re.search(r'@(\d+\.\d+):' + event + ' ', line)
            if not match:
                continue
            timestamp = float(match[1])
        if started <= timestamp <= ended:
            updates.append(timestamp)

    intervals = sorted((b - a) * 1000 for a, b in zip(updates, updates[1:]))
    if intervals:
        print(f'{result} updates={len(updates)} update_hz={(len(updates)-1)/(updates[-1]-updates[0]):.1f} '
              f'mean_ms={sum(intervals)/len(intervals):.3f} '
              f'p95_ms={intervals[(len(intervals)-1)*95//100]:.3f} max_ms={max(intervals):.3f} '
              f'refresh_ms={refresh[-1] if refresh else "unknown"} trace={path}', flush=True)
    else:
        print(f'{result} updates={len(updates)} refresh_ms={refresh[-1] if refresh else "unknown"} '
              f'trace={path}', flush=True)


def measure(cpus, buffered=True, iso=None, sse2=False, accel='tcg', display='none', trace_display=False):
    label = 'drag' if accel == 'tcg' and display == 'none' else 'drag-' + accel + '-' + re.sub(r'[^a-zA-Z0-9_-]', '-', display)
    if not buffered:
        label += '-linear'
    vm = VM(cpus, storage=False, buffered=buffered, iso=iso,
            label=label,
            cpu='max,tcg-cpuid=off' if sse2 else None,
            accel=accel, display=display, trace_display=trace_display)
    d = None
    drag_started = drag_ended = None
    try:
        fixtures(vm)
        assert vm.qmp('query-kvm')['enabled'] == (accel == 'kvm')
        vm.command('gui --info', 'Shadow blending=SSE2' if sse2 or accel ==
                   'kvm' else 'Shadow blending=integer')
        vm.send('gui --stats --trace\n')
        vm.until(b'GUI_READY', 30)
        d = DesktopTest(vm)
        d.screenshot(f'drag-initial-{cpus}cpu')
        d.move(d.width - 45, d.height - 70)
        first = d.screenshot(f'drag-start-{cpus}cpu')
        left = (d.width - d.width * 78 // 100) // 2
        top = d.height * 8 // 100
        d.move(left + 52, top + 20)
        d.button(True)
        drag_started = time.time()
        for i in range(120):
            d.move(left + 52 + (i % 60) * 2, top + 20 + (i % 30), delay=1 / 120)
        drag_ended = time.time()
        time.sleep(.3)

        # Include the final movement in the release report. It must be applied.
        x, y = left + 52, top + 20
        vm.qmp('input-send-event', {'events': [
            {'type': 'rel', 'data': {'axis': 'x', 'value': x - d.x}},
            {'type': 'rel', 'data': {'axis': 'y', 'value': y - d.y}},
            {'type': 'btn', 'data': {'button': 'left', 'down': False}}]})
        d.x, d.y = x, y
        geometry = d.expect('GEOMETRY')
        m = re.search(r'x=(\d+) y=(\d+)', geometry)
        assert tuple(map(int, m.groups())) == (left, top), geometry
        d.move(d.width - 45, d.height - 70)
        final = d.screenshot(f'drag-end-{cpus}cpu')

        # The clock is in the excluded taskbar. No stale window, cursor, or
        # shadow pixels may remain after returning to the same geometry.
        assert first[:d.width * (d.height - 42) * 3] == final[:d.width * (d.height - 42) * 3], 'Drag left stale pixels'

        # Rapid press/move/release retains the actual press position. A second
        # drag immediately afterward must not be mistaken for a double-click.
        d.move(left + 52, top + 20)
        vm.qmp('input-send-event',
               {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': True}}]})

        # QMP completion acknowledges injection, not USB delivery. Wait for
        # the guest to receive the press before issuing the rapid movement;
        # otherwise QEMU may combine both into one report at the destination.
        press = d.expect('POINTER', 'pressed=1')
        m = re.search(r'position=\((\d+),(\d+)\)', press)
        assert m and tuple(map(int, m.groups())) == (left + 52, top + 20), press
        time.sleep(.025)
        d.move(left + 76, top + 38, delay=.025)
        vm.qmp('input-send-event',
               {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': False}}]})
        geometry = d.expect('GEOMETRY')
        m = re.search(r'x=(\d+) y=(\d+)', geometry)
        assert tuple(map(int, m.groups())) == (left + 24, top + 18), geometry
        d.button(True)
        d.move(left + 52, top + 20)
        d.button(False)
        geometry = d.expect('GEOMETRY')
        m = re.search(r'x=(\d+) y=(\d+)', geometry)
        assert tuple(map(int, m.groups())) == (left, top), geometry
        d.move(d.width - 45, d.height - 70)
        final = d.screenshot(f'drag-burst-end-{cpus}cpu')
        assert first[:d.width * (d.height - 42) * 3] == final[:d.width * (d.height - 42) * 3], 'Fast drag left stale pixels'
        output = d.exit()
        d = None
        m = re.search(r'GUI_TIMING compose_us=(\d+) upload_us=(\d+) drag_frames=(\d+) '
                      r'drag_us=(\d+) drag_max_us=(\d+) surface_updates=(\d+)', output)
        assert m, output[-2000:]
        compose, upload, frames, elapsed, worst, updates = map(int, m.groups())
        assert frames > 0
        assert updates <= max(
            10, frames // 4), 'Window content was repeatedly repainted while dragging'

        print(f'DRAG_RESULT accel={accel} display={display} cpus={cpus} frames={frames} average_ms={elapsed/frames/1000:.3f} '
              f'max_ms={worst/1000:.3f} compose_us={compose} upload_us={upload} '
              f'surface_updates={updates}', flush=True)
        pacing = re.search(r'GUI_PACING[^\n]*', output)
        if pacing:
            print(pacing.group(), flush=True)
    except BaseException:
        if d:
            try:
                print('DRAG_FAILURE_DIAGNOSTICS\n' + d.exit()[-2000:], flush=True)
            except Exception:
                pass
        raise
    finally:
        vm.close()
        if trace_display and drag_started is not None and drag_ended is not None:
            report_display(vm, drag_started, drag_ended, display)


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--cpus', type=int, default=1)
    p.add_argument('--linear', action='store_true')
    p.add_argument('--iso')
    p.add_argument('--sse2', action='store_true',
                   help='Hide the TCG signature to test the hardware blend path; timings remain emulated')
    p.add_argument('--accel', choices=('tcg', 'kvm'), default='tcg')
    p.add_argument('--display', default='none',
                   help='QEMU frontend, e.g. sdl,gl=on; opens a window when enabled')
    p.add_argument('--trace-display', action='store_true',
                   help='Record QEMU refresh intervals and surface changes')
    a = p.parse_args()

    if a.sse2 and a.accel != 'tcg':
        p.error('--sse2 requires --accel tcg')
    measure(a.cpus, not a.linear, a.iso, a.sse2, a.accel, a.display, a.trace_display)
