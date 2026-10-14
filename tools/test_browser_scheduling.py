#!/usr/bin/env python3
"""Check real engine idle tasks and native input/paint scheduling."""
import argparse
import shlex
import time
from test_runtime import VM, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture


def exercise(cpus, iso):
    fixture = WebFixture(webcore=True)
    vm = None
    try:
        vm = VM(cpus, storage=False, iso=iso, label='browser-scheduling')
        output = vm.command('webcore-render-check', timeout=200)
        assert_success(output)
        assert output.count(
            'PASS selected interpreter allocations and full GC preserve') == 2, output
        assert output.count(
            'PASS real WebCore idle deadlines, microtasks, continuation, runnable timeout and cancellation') == 2, output

        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do '
            'sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        vm.send('gui --trace\n');
        vm.until(b'GUI_READY', 40)
        desktop = DesktopTest(vm)
        desktop.screenshot(f'browser-scheduling-start-{cpus}cpu')

        window = desktop.launch('Browser');
        desktop.expect('BROWSER_READY', 'ModernOS Browser')
        left = (desktop.width - 840) // 2 + (window - 1) % 5 * 16
        top = (desktop.height - 42 - 560) // 2 + (window - 1) % 5 * 16
        for path, title in (('webcore-frame-scheduling', 'WebCore frame scheduling ready'),
                            ('webcore-custom-scheduling', 'WebCore custom scheduling ready')):
            desktop.key('l', 'ctrl');
            vm.send(fixture.url(path) + '\n')
            desktop.expect('BROWSER_READY', title)
            data = desktop.screenshot(f'browser-scheduling-{path}-{cpus}cpu')
            offset = ((top + 80) * desktop.width + left + 400) * 3
            assert data[offset:offset + 3] == bytes((0, 204, 68)), 'Completed scheduling result was not painted'

        desktop.key('l', 'ctrl');
        vm.send(fixture.url('webcore-busy-scroll') + '\n')
        desktop.expect('BROWSER_READY', 'WebCore busy scroll')
        desktop.expect('BROWSER_LOG', 'WebCore background task stream active')
        desktop.move(left + 270, top + 240)
        started = time.monotonic()

        for _ in range(4):
            vm.qmp('input-send-event', {'events': [
                {'type': 'btn', 'data': {'button': 'wheel-down', 'down': True}},
                {'type': 'btn', 'data': {'button': 'wheel-down', 'down': False}}]})

        while True:
            data = desktop.screenshot(f'browser-scheduling-scroll-{cpus}cpu')
            offset = ((top + 40 + 145) * desktop.width + left + 25) * 3
            if data[offset:offset + 3] == bytes((17, 170, 51)):
                break
            assert time.monotonic() - started < 1, 'Input/paint starved behind continuously queued script tasks'

        latency = time.monotonic() - started
        assert latency < 1, latency
        desktop.key('f4', 'alt');
        desktop.expect('CLOSE', f'window={window}')
        desktop.exit()

        print(
            f'PASS controlled idle deadlines/continuation/timeouts, native animation/microtask/custom element callbacks and scrolling during background tasks ({latency:.2f}s, {cpus} CPUs)', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--iso', required=True)
    args = parser.parse_args()
    for cpus in args.cpus:
        exercise(cpus, args.iso)
