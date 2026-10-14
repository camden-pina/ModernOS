#!/usr/bin/env python3
"""Measure a checked script workload in the real native browser, without JIT."""
import argparse
import json
import re
import shlex
from pathlib import Path
from test_runtime import VM, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpus', type=int, default=1)
    parser.add_argument('--iso', required=True)
    parser.add_argument('--label', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()

    assert re.fullmatch('[a-zA-Z0-9_-]+', args.label)
    assert not args.output.exists(), 'Preserve the existing measurement'
    fixture = WebFixture(webcore=True)
    vm = None
    try:
        vm = VM(args.cpus, storage=False, iso=args.iso, label='browser-interpreter-' + args.label)
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do '
            'sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        vm.send('gui --trace\n');
        vm.until(b'GUI_READY', 40)
        desktop = DesktopTest(vm)
        desktop.screenshot('browser-interpreter-start-' + args.label)

        window = desktop.launch('Browser');
        desktop.expect('BROWSER_READY', 'ModernOS Browser')
        desktop.key('l', 'ctrl');
        vm.send(fixture.url('webcore-interpreter') + '\n')
        line = vm.until(
            rb'DESKTOP BROWSER_TITLE[^\n]*WebCore interpreter ready: [0-9.,]+[^\n]*\n', 120)
        samples = [float(value) for value in re.search(
            r'WebCore interpreter ready: ([0-9.,]+)', line)[1].split(',')]
        assert len(samples) == 3 and all(0 < value < 120000 for value in samples), samples
        pixels = desktop.screenshot('browser-interpreter-result-' + args.label)
        left = (desktop.width - 840) // 2 + (window - 1) % 5 * 16
        top = (desktop.height - 42 - 560) // 2 + (window - 1) % 5 * 16
        offset = ((top + 80) * desktop.width + left + 40) * 3
        assert pixels[offset:offset + 3] == bytes((0, 204, 68)), 'Checked result was not painted'
        desktop.key('f4', 'alt');
        desktop.expect('CLOSE', f'window={window}')
        desktop.exit()
        result = dict(iso=str(Path(args.iso).resolve()), cpus=args.cpus, label=args.label,
                      iterations_per_sample=2000000, samples_ms=samples, median_ms=sorted(samples)[1])
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + '\n')

        print(json.dumps(result), flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    main()
