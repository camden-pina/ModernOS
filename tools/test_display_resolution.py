#!/usr/bin/env python3
"""Exercise live Coast display modes, timeout rollback and saved resolution."""
import argparse
import json
import os
from pathlib import Path
import re
import time
import traceback

from source_tree import digest
import test_runtime as runtime
import test_desktop as desktop


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--browser', action='store_true',
                        help='also verify Chromium adapts while its window remains open')
    args = parser.parse_args()
    iso = args.iso.resolve(strict=True)
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'HARNESS.py').write_bytes(Path(__file__).read_bytes())
    runtime.WORK = desktop.WORK = out
    report = dict(state='failed', iso=str(iso), iso_sha256=digest(iso), checks=[])
    vm = None

    def capture(d, name, expected):
        pixels = d.screenshot(name)
        header = (out / (name + '.ppm')).read_bytes().split(b'\n', 3)
        size = tuple(map(int, header[1].split()))
        assert size == expected, (name, size, expected)
        report['checks'].append(name + ': ' + str(size))
        return pixels

    def display_page(d):
        d.key('5', 'ctrl')
        d.expect('SETTINGS_SECTION', 'Display')

    def choose(d, index):
        display_page(d)
        for _ in range(index):
            d.key('tab')
        d.key('ret')

    def browser_alive(window):
        while vm.selector.select(0):
            data = os.read(vm.process.stdout.fileno(), 65536)
            if not data:
                break
            vm.pending.extend(data)
            vm.log.write(data)
            vm.log.flush()
        log = Path(vm.log.name).read_bytes()
        assert f'DESKTOP CLOSE window={window} '.encode() not in log, 'Chromium closed during resolution change'
        assert b'C1_RUNTIME_WAIT status=11' not in log, 'Chromium crashed during resolution change'

    try:
        vm = runtime.VM(4, iso=iso, accel='kvm', storage=False,
                        network=False, memory='16G', label='display-modes')
        info = vm.command('gui --info')
        assert '2560x1600' in info and 'Resolution switching=1' in info, info
        gui = "build-sh -c 'COAST_BROWSER_QA=1 exec /bin/gui --trace'\n" if args.browser else 'gui --trace\n'
        vm.send(gui)
        vm.until(rb'GUI_READY:', 60)
        d = desktop.DesktopTest(vm)
        capture(d, 'default', (2560, 1600))
        d.launch('Settings')
        choose(d, 1)
        vm.until(rb'DESKTOP DISPLAY_MODE width=1920 height=1200', 8)
        capture(d, 'preview', (1920, 1200))
        vm.until(rb'DESKTOP DISPLAY_MODE width=2560 height=1600', 22)
        capture(d, 'timeout-revert', (2560, 1600))
        choose(d, 1)
        vm.until(rb'DESKTOP DISPLAY_MODE width=1920 height=1200', 8)
        d.key('ret')  # The selected mode puts focus on Keep resolution.
        saved_event = d.expect('CONFIG_SAVED', 'desktop.ini')
        capture(d, 'kept', (1920, 1200))
        d.exit()
        saved_path = re.findall(r'DESKTOP CONFIG_SAVED[^\n]*?(/[\w./-]+desktop\.ini)', saved_event)[-1]
        saved = vm.command('cat ' + saved_path)
        assert 'display_width=1920\n' in saved and 'display_height=1200\n' in saved, saved
        vm.send(gui)
        vm.until(rb'GUI_READY:', 60)
        d = desktop.DesktopTest(vm)
        capture(d, 'reloaded', (1920, 1200))
        d.launch('Settings')
        choose(d, 0)
        vm.until(rb'DESKTOP DISPLAY_MODE width=2560 height=1600', 8)
        d.key('ret')
        capture(d, 'expanded', (2560, 1600))
        if args.browser:
            d.key('f4', 'alt')  # Close Settings before opening Chromium.
            browser_window = d.launch('Chromium Browser')
            vm.until(rb'COAST_BROWSER .*"event":"page"[^\n]*Welcome to Coast', 60)
            d.launch('Settings')
            choose(d, 1)
            vm.until(rb'P6_DISPLAY_MODE width=1920 height=1200 scale=1', 8)
            d.key('ret')
            d.key('f4', 'alt')
            time.sleep(1)
            assert capture(d, 'browser-reduced', (1920, 1200)).count(bytes.fromhex('f8fafc')) > 10000, 'Chromium page did not repaint at 1920x1200'
            browser_alive(browser_window)
            d.launch('Settings')
            choose(d, 0)
            vm.until(rb'P6_DISPLAY_MODE width=2560 height=1600 scale=2', 8)
            d.key('ret')
            d.key('f4', 'alt')
            time.sleep(1)
            assert capture(d, 'browser-expanded', (2560, 1600)).count(bytes.fromhex('f8fafc')) > 10000, 'Chromium page did not repaint at 2560x1600'
            browser_alive(browser_window)
            d.key('f4', 'alt')  # Close Chromium and its owned helpers.
        d.exit()
        report['state'] = 'passed'
    except BaseException as error:
        report['error'] = repr(error)
        (out / 'exception.txt').write_text(traceback.format_exc())
        if vm and 'd' in locals():
            try:
                d.screenshot('failure')
            except BaseException:
                pass
            try:
                (out / 'registers.txt').write_text(str(vm.qmp(
                    'human-monitor-command', {'command-line': 'info registers'})))
            except BaseException:
                pass
        raise
    finally:
        if vm:
            vm.close()
        report['owned_vm_stopped'] = bool(vm and vm.process.poll() is not None)
        report['image_unchanged'] = digest(iso) == report['iso_sha256']
        if not report['image_unchanged']:
            report['state'] = 'failed'
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
