#!/usr/bin/env python3
"""Optional live OSDev check, using only the rendered verification checkbox."""
import argparse
import json
from pathlib import Path
import re
import shlex
import time
from urllib.parse import urlparse
from diagnose_browser_sites import capture_navigation
from test_desktop import DesktopTest
from test_runtime import VM, WORK, assert_success


def checkbox(pixels, width, height, light_outline=False):
    def dark(x, y):
        at = (y * width + x) * 3
        return all(c > 140 for c in pixels[at:at + 3]) if light_outline else all(c < 180 for c in pixels[at:at + 3])

    # Recognize the actual empty square, including both vertical sides.
    for y in range(200, min(600, height - 100)):
        start = None
        for x in range(width // 3 - 40, width // 2):
            if dark(x, y):
                if start is None:
                    start = x
            elif start is not None:
                size = x - start
                if 18 <= size <= 28:
                    for h in range(size - 2, size + 3):
                        for inset in (0, 1):
                            if all(dark(start + inset, y + dy) and dark(x - 1 - inset, y + dy) for dy in range(2, h - 1)) and dark(start + size // 2, y + h - 1):
                                return start + size // 2, y + h // 2
                start = None
    return None


def exercise(iso, cpus, accel, label=None, network_trace=False, exception_trace=False, max_attempts=1):
    label = label or f'osdev-verification-{accel}'
    vm = VM(cpus, storage=False, iso=iso, label=label, accel=accel)
    try:
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do '
            'sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        command = 'gui --trace --stats'
        if network_trace:
            command += ' --browser-network-trace'
        if exception_trace:
            command += ' --browser-exception-trace'
        vm.send(command + '\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm);
        d.screenshot(f'{label}-start');

        window = d.launch('Browser')
        left = (d.width - 840) // 2 + (window - 1) % 5 * 16;
        top = (d.height - 42 - 560) // 2 + (window - 1) % 5 * 16
        d.click(left + 797, top + 20);
        d.expect('MAXIMIZE')
        d.key('l', 'ctrl');
        vm.pending.clear();
        vm.send('https://osdev.org/\n')
        deadline = time.monotonic() + 90;
        clicked = False;
        attempts = 0;
        retry_after = 0;
        row = {}
        while time.monotonic() < deadline:
            row = capture_navigation(vm, window, 3, expect_loading=False, minimum_settle=0)
            if 'osdev' in row.get('title', '').lower() and row['title'] != 'Just a moment...':
                break
            pixels = d.screenshot(f'{label}-before-verification')
            point = checkbox(pixels, d.width, d.height)
            if point and time.monotonic() >= retry_after:
                d.screenshot(f'{label}-checkbox-attempt-{attempts+1}')
                d.click(*point);
                clicked = True;
                attempts += 1
                print(
                    f'Clicked the actual rendered verification checkbox at {point} (attempt {attempts})', flush=True)

                if attempts >= max_attempts:
                    row = capture_navigation(vm, window, 90, expect_loading=False,
                                             minimum_settle=2, expected_title='OSDev')
                    break

                # A real navigation can present a fresh verification widget
                # after issuing clearance. Retry only a visibly empty square,
                # with time for the preceding interaction to complete.
                retry_after = time.monotonic() + 15

        d.screenshot(f'{label}-after-verification')
        title = row.get('title', '');
        host = urlparse(row.get('final_url', '')).hostname or ''
        print('OSDev result:', title, row.get('final_url'),
              'checkbox clicked:', clicked, flush=True)
        log = Path(vm.log.name).read_text(errors='replace')
        document_statuses = [int(status) for status in re.findall(
            r'HTTP resource: status=(\d+) bytes=\d+ https://(?:www\.)?osdev\.org/(?:\?[^\s]*)?\r?\n', log)]
        report = dict(row, iso=str(iso), cpus=cpus, accel=accel, exception_trace=exception_trace,
                      checkbox_attempts=attempts, document_http_statuses=document_statuses,
                      screenshot=str(WORK / (label + '-after-verification.png')))
        if network_trace:
            roots = r'https://(?:www\.)?osdev\.org/(?:\?[^\s]*)?'
            report['root_cookie_integrity'] = [dict(matches_jar=bool(int(match[1])), selected_count=int(match[2]))
                                               for match in re.finditer(r'HTTP cookie integrity: id=\d+ matches_jar=([01]) selected_count=(\d+) ' + roots + r'\r?\n', log)]
            report['root_response_diagnostics'] = [dict(status=int(match[1]), http=match[2], tls=match[3],
                                                        certificate_errors=int(match[4]), cf_mitigated=match[5], cf_ray=match[6],
                                                        clock_skew_seconds=None if match[7] == 'unknown' else int(match[7]))
                                                   for match in re.finditer(r'HTTP response diagnostics: id=\d+ status=(\d+) http=\[([^\]]*)\] tls=\[([^\]]*)\] certificate_errors=(\d+) cf_mitigated=\[([^\]]*)\] cf_ray=\[([^\]]*)\] clock_skew_seconds=(unknown|-?\d+) ' + roots + r'\r?\n', log)]
            report['clearance_stored'] = bool(
                re.search(r'HTTP cookie stored: name=cf_clearance domain=\.?osdev\.org ', log))
            report['clearance_sent'] = bool(re.search(
                r'HTTP request headers: .* cookie_names=\[[^\]]*\bcf_clearance\b[^\]]*\] ' + roots + r'\r?\n', log))

        with (WORK / (label + '-result.json')).open('x') as result:
            json.dump(report, result, indent=2);
            result.write('\n')

        assert 'osdev' in title.lower() and title != 'Just a moment...' and host in (
            'osdev.org', 'www.osdev.org'), report
        assert re.search(
            r'HTTP resource: status=200 bytes=\d+ https://(?:www\.)?osdev\.org/(?:\?[^\s]*)?\r?\n', log), 'OSDev document HTTP 200 was not observed'
        assert row.get('complete_paints', 0) > 0, 'OSDev document had no completed rendered paint'

        d.key('f4', 'alt');
        d.expect('CLOSE');
        d.exit()

        print(
            f'PASS live OSDev: actual site document, verified HTTPS and genuine browser verification ({cpus} CPUs, {accel})', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True);
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument(
        '--label', help='Unique evidence prefix; preserves earlier guest logs and screenshots')
    parser.add_argument('--network-trace', action='store_true',
                        help='Trace cookie names/attributes without their values')
    parser.add_argument('--exception-trace', action='store_true',
                        help='Observe caught and uncaught exceptions with the genuine JSC debugger; diagnostic timing differs')
    parser.add_argument('--attempts', type=int, choices=range(1, 4), default=1,
                        help='Maximum genuine clicks on visibly empty verification widgets; retries are spaced by 15 seconds')
    args = parser.parse_args();

    exercise(args.iso, args.cpus, args.accel, args.label,
             args.network_trace, args.exception_trace, args.attempts)
