#!/usr/bin/env python3
"""Verify actual cross-origin fetch cookies, CORS and Blob-worker requests."""
import argparse
import shlex
from pathlib import Path
from test_runtime import VM, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture


def exercise(iso, cpus, accel, label):
    fixture = WebFixture(webcore=True)
    vm = None
    try:
        vm = VM(cpus, storage=False, iso=iso, label=label, accel=accel)
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do '
            'sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            f'curl -fsS --max-time 30 {fixture.url("ca.pem")} >> /etc/ssl/cert.pem')))
        vm.send('gui --trace --stats\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm);
        d.screenshot(label + '-start');

        window = d.launch('Browser')
        d.key('l', 'ctrl');
        vm.send(fixture.url('webcore-credentials', tls=True) + '\n')
        d.expect('BROWSER_TITLE', 'WebCore credentials ready')
        pixels = d.screenshot(label + '-ready')
        left = (d.width - 840) // 2 + (window - 1) % 5 * 16
        top = (d.height - 42 - 560) // 2 + (window - 1) % 5 * 16 + 40
        at = ((top + 80) * d.width + left + 80) * 3
        assert pixels[at:at + 3] == bytes((17, 170, 51)), 'Successful document was not rendered'
        d.key('f4', 'alt');
        d.expect('CLOSE');
        d.exit()
        log = Path(vm.log.name).read_text(errors='replace')
        assert 'GLib-CRITICAL' not in log and 'BROWSER_FORCED_CLOSE' not in log

        print(f'PASS native credentials: real HTTPS, include/omit/same-origin cookies, '
              f'HttpOnly, CORS denial and Blob-worker fetch; rendered pixels ({cpus} CPUs, {accel})', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True)
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--label', default='native-credentials')
    args = parser.parse_args();
    exercise(args.iso, args.cpus, args.accel, args.label)
