#!/usr/bin/env python3
"""Open the make run configuration for a physical pointer check.

No pointer events are injected. Trace files record only guest pointer events
and display timing. Create the printed .stop file to finish the check.
"""
import argparse
import os
import re
import time

from test_desktop import DesktopTest
from test_runtime import VM, WORK


def diagnose(cpus, display, seconds):
    label = 'physical-mouse-' + re.sub(r'[^a-zA-Z0-9_-]', '-', display)
    stop = WORK / f'uefi-{cpus}cpu-{label}.stop'
    stop.unlink(missing_ok=True)
    vm = VM(cpus, storage=False, accel='kvm', display=display, label=label,
            make_run=True, trace_pointer=True, trace_display=True)
    try:
        vm.send('gui --stats --trace\n')
        vm.until(b'GUI_READY', 30)
        desktop = DesktopTest(vm)
        desktop.screenshot(label + '-initial')
        print(f'PHYSICAL_MOUSE_READY display={display} pid={vm.process.pid}', flush=True)
        print(f'Stop file: {stop}', flush=True)
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline and not stop.exists():
            if vm.process.poll() is not None:
                print('QEMU window closed', flush=True)
                return
            if vm.selector.select(.2):
                data = os.read(vm.process.stdout.fileno(), 65536)
                vm.pending.extend(data)
                vm.log.write(data)
                vm.log.flush()

        desktop.screenshot(label + '-final')

        print(desktop.exit(), flush=True)
    finally:
        vm.close()
        stop.unlink(missing_ok=True)
        trace = (WORK / (vm.name + '.display-trace')).read_text()
        for event in ('input_event_rel', 'input_event_btn', 'input_event_abs'):
            print(f'{event}: {trace.count(event + " ")}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--display', default='sdl,gl=on')
    parser.add_argument('--seconds', type=int, default=300)
    args = parser.parse_args()
    diagnose(args.cpus, args.display, args.seconds)
