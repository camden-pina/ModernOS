#!/usr/bin/env python3
"""Capture public-site behavior in the actual sandboxed desktop browser.

This is an optional diagnostic, not a claim of rendering compatibility. Engine
load completion and HTTP success do not establish that a page looks correct.
Public sites may require verification or rate-limit the test connection.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import time
from urllib.parse import parse_qs, urlparse
from test_runtime import ANSI, VM, WORK, assert_success
from test_desktop import DesktopTest


SITES = (
    ('google-search', 'https://www.google.com/search?q=osdev'),
    ('osdev', 'https://osdev.org/'),
    ('yahoo', 'https://www.yahoo.com/'),
    ('bing', 'https://www.bing.com/'),
    ('bing-search', 'https://www.bing.com/search?q=osdev'),
    ('gcc', 'https://gcc.gnu.org/'),
)


def capture_navigation(vm, window, timeout, expect_loading=True, expected_title=None, minimum_settle=12):
    """Wait through script/HTTP redirects, including intermediate READY events."""
    started = time.monotonic()
    last_change = started
    finished = False
    seen_loading = not expect_loading
    row = {'load_finished': False, 'title': '', 'final_url': '', 'script_errors': [], 'complete_paints': 0}
    last_visible_change = started;
    last_paint = 0
    line_buffer = bytearray()
    trace = re.compile(r'DESKTOP BROWSER_(TITLE|URL|LOADING|READY|ERROR|PRESENT) window=' +
                       str(window) + r' workspace=\d+ (.*)')
    while time.monotonic() - started < timeout:
        line_buffer.extend(vm.pending)
        vm.pending.clear()
        while b'\n' in line_buffer:
            line, _, remaining = line_buffer.partition(b'\n')
            line_buffer = bytearray(remaining)
            text = ANSI.sub(b'', line).replace(b'\r', b'').decode(errors='replace')
            assert 'PANIC:' not in text, text
            match = trace.search(text)
            if match:
                kind, value = match.groups()
                if kind == 'LOADING':
                    seen_loading = True
                    finished = False
                    last_change = time.monotonic()
                    last_visible_change = last_change
                elif kind == 'READY' and seen_loading:
                    finished = True
                    row['title'] = value
                    last_change = time.monotonic()
                elif kind in ('TITLE', 'URL'):
                    field = 'title' if kind == 'TITLE' else 'final_url'
                    if row[field] != value:
                        row[field] = value
                        last_change = time.monotonic()
                        last_visible_change = last_change
                elif kind == 'PRESENT':
                    last_paint = time.monotonic();
                    row['complete_paints'] += 1
                elif kind == 'ERROR':
                    row['browser_error'] = value

            error = re.search(
                r'(?:Uncaught error in JS: |WebCore console .*?:\d+: )((?:.*?(?:ReferenceError|TypeError|SyntaxError|SecurityError|RangeError|QuotaExceededError):).*)', text)
            if error and error[1] not in row['script_errors'] and len(row['script_errors']) < 10:
                row['script_errors'].append(error[1])
            if re.search(r'DESKTOP BROWSER_LOG window=' + str(window) + r' .*?(?:HTTP resource:|Resource error:)', text):
                last_change = time.monotonic()

        now = time.monotonic()

        if row.get('browser_error'):
            break
        title_matches = expected_title is None or expected_title.lower() in row['title'].lower()
        if finished and title_matches and now - started >= minimum_settle and now - last_change >= 3 and last_paint >= last_visible_change:
            row['load_finished'] = True
            break

        assert vm.process.poll() is None, 'QEMU exited during browser navigation'

        if vm.selector.select(.1):
            data = os.read(vm.process.stdout.fileno(), 65536)
            assert data, 'QEMU output closed during browser navigation'
            vm.pending.extend(data)
            vm.log.write(data)
            vm.log.flush()

    row['navigation_capture_seconds'] = round(time.monotonic() - started, 3)
    return row


def capture_scroll(vm, desktop, window, name):
    desktop.move(desktop.width // 2, desktop.height // 2)
    before = desktop.screenshot(name)

    # Discard trace records already written before the gesture. They can still
    # be waiting in stdout even after navigation consumed its pending buffer.
    vm.pending.clear()

    while vm.selector.select(0):
        data = os.read(vm.process.stdout.fileno(), 65536)
        assert data, 'QEMU output closed before scrolling'
        vm.log.write(data);
        vm.log.flush()

    scroll_started = time.monotonic()

    for _ in range(4):
        vm.qmp('input-send-event', {'events': [
            {'type': 'btn', 'data': {'button': 'wheel-down', 'down': True}},
            {'type': 'btn', 'data': {'button': 'wheel-down', 'down': False}}]})

    start = 200 * desktop.width * 3;
    end = (desktop.height - 42) * desktop.width * 3
    last = before;
    stable_since = scroll_started;
    first_change = None;
    first_complete = None
    line_buffer = bytearray()
    while True:
        if vm.selector.select(.01):
            data = os.read(vm.process.stdout.fileno(), 65536)
            assert data, 'QEMU output closed during scrolling'
            vm.pending.extend(data);
            vm.log.write(data);
            vm.log.flush()

        line_buffer.extend(vm.pending);
        vm.pending.clear()

        while b'\n' in line_buffer:
            line, _, line_buffer = line_buffer.partition(b'\n')
            if f'DESKTOP BROWSER_PRESENT window={window} '.encode() in line:
                stable_since = time.monotonic()
                if first_complete is None:
                    first_complete = stable_since

        after = desktop.screenshot(name + '-scrolled');
        now = time.monotonic()
        if after[start:end] != last[start:end]:
            stable_since = now

        # A cursor, scrollbar highlight or tiny animation is not evidence that
        # the document scrolled. Require visible changes across the body.
        changed_samples = sum(before[i:i + 3] != after[i:i + 3] for i in range(start, end, 96))
        if changed_samples >= 100 and first_change is None:
            first_change = now

        if (first_complete is not None and first_change is not None and now - stable_since >= .5) or now - scroll_started >= 4:
            break
        last = after;
        time.sleep(.05)

    translated = translation_samples = 0

    # Match detailed 3x3 patches at the requested 400-pixel translation. Flat
    # backgrounds and the fixed header cannot make this check pass.
    for y in range(600, desktop.height - 100, 32):
        for x in range(280, desktop.width - 100, 48):
            old = b''.join(before[((y + dy) * desktop.width + x) * 3:((y + dy) * desktop.width + x + 3) * 3] for dy in range(3))
            if len({old[i:i + 3] for i in range(0, len(old), 3)}) < 4:
                continue
            new = b''.join(after[((y - 400 + dy) * desktop.width + x) * 3:((y - 400 + dy) * desktop.width + x + 3) * 3] for dy in range(3))
            translation_samples += 1;
            translated += old == new

    return dict(scroll_capture_seconds=round(time.monotonic() - scroll_started, 3),
                scroll_first_body_change_seconds=None if first_change is None else round(
                    first_change - scroll_started, 3),
                scroll_first_complete_paint_seconds=None if first_complete is None else round(
                    first_complete - scroll_started, 3),
                scroll_changed_pixels=sum(before[i:i + 3] != after[i:i + 3]
                                          for i in range(start, end, 3)),
                scroll_translated_detail_samples=translated,
                scroll_detail_samples=translation_samples,
                scroll_400px_confirmed=translation_samples >= 20 and translated / translation_samples >= .5,
                scroll_screenshot=str(WORK / (name + '-scrolled.ppm')))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--accel', choices=('tcg', 'kvm'), default='tcg',
                        help='Guest CPU accelerator; hardware tests still use the actual OS renderer')
    parser.add_argument('--iso')
    parser.add_argument('--label', default='browser-sites',
                        help='Distinct VM log and screenshot/report prefix for preserved comparisons')
    parser.add_argument('--timeout', type=float, default=60)
    parser.add_argument('--settle-seconds', type=float, default=12,
                        help='Minimum observation time before accepting a quiet completed navigation')
    parser.add_argument('--url', action='append',
                        help='Diagnose these URLs instead of the default sites')
    args = parser.parse_args()

    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.label):
        parser.error('--label must contain only letters, numbers, underscores and hyphens')
    sites = tuple((f'site-{i + 1}', url) for i, url in enumerate(args.url)) if args.url else SITES
    report = []
    vm = VM(args.cpus, storage=False, iso=args.iso, label=args.label, accel=args.accel)
    try:
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do '
            'sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))

        for name, url in sites:
            script = ('curl -sSL --max-time 25 -D /tmp/site-headers ' + shlex.quote(url) +
                      ' -o /tmp/site-body -w "HTTP_CODE=%{http_code} SIZE=%{size_download}\\n"; '
                      'grep -Ei "^(HTTP/|cf-mitigated:)" /tmp/site-headers')
            output = vm.command('build-sh -c ' + shlex.quote(script), timeout=35)
            status = re.search(r'HTTP_CODE=(\d+) SIZE=([^\n]+)', output)
            report.append(dict(name=name, url=url,
                               curl_http_code=int(status[1]) if status else None,
                               curl_verification_challenge='cf-mitigated: challenge' in output.lower()))

            print('PUBLIC HTTPS', name, status[0] if status else output[-200:], flush=True)

        vm.send('gui --trace --stats\n')
        vm.until(b'GUI_READY', 40)
        desktop = DesktopTest(vm)
        desktop.screenshot(args.label + '-start')

        for row in report:
            window = desktop.launch('Browser')

            # Fast guests can emit OPEN and READY in the same stdout read.
            # DesktopTest.launch consumes that read; accept an already
            # observed READY for this exact new window from the guest log.
            vm.log.flush()
            ready = re.compile(rb'DESKTOP BROWSER_READY window=' + str(window).encode() +
                               rb' workspace=\d+ ModernOS Browser\r?\n')
            if not ready.search(Path(vm.log.name).read_bytes()):
                desktop.expect('BROWSER_READY', 'ModernOS Browser')
            left = (desktop.width - 840) // 2 + (window - 1) % 5 * 16
            top = (desktop.height - 42 - 560) // 2 + (window - 1) % 5 * 16
            desktop.click(left + 840 - 43, top + 20)
            desktop.expect('MAXIMIZE')
            desktop.key('l', 'ctrl')
            vm.pending.clear()
            vm.send(row['url'] + '\n')
            query = parse_qs(urlparse(row['url']).query)
            expected_title = (query.get('search_query') or query.get('q') or [None])[0]
            row.update(capture_navigation(vm, window, args.timeout,
                       expected_title=expected_title, minimum_settle=args.settle_seconds))
            name = args.label + '-' + row['name']
            row['screenshot'] = str(WORK / (name + '.ppm'))
            row.update(capture_scroll(vm, desktop, window, name))
            print('GUEST BROWSER', row['name'],
                  'load finished' if row['load_finished'] else 'load timed out',
                  row['title'], row['final_url'], flush=True)

            if row.get('load_finished') and not row.get('browser_error'):
                if 'google.com/search' in row['url']:
                    search = (600, 92)
                elif row['url'].rstrip('/') == 'https://www.youtube.com':
                    search = (1300, 68)
                else:
                    search = None
                if search:
                    for _ in range(4):
                        vm.qmp('input-send-event', {'events': [
                            {'type': 'btn', 'data': {'button': 'wheel-up', 'down': True}},
                            {'type': 'btn', 'data': {'button': 'wheel-up', 'down': False}}]})

                    time.sleep(1)
                    desktop.click(*search);
                    desktop.key('a', 'ctrl')

                    # Keep text and submission on the same USB device. UART
                    # input and USB Enter can arrive in the opposite order.
                    for key in 'limine uefi':
                        desktop.key('spc' if key == ' ' else key)
                    vm.pending.clear();
                    desktop.key('ret')

                    # pushState can report READY with the previous page title
                    # before the search response and DOM update have arrived.
                    row['search_interaction'] = capture_navigation(
                        vm, window, args.timeout, expect_loading=False, expected_title='limine uefi', minimum_settle=args.settle_seconds)
                    desktop.screenshot(name + '-search')
                    row['search_screenshot'] = str(WORK / (name + '-search.ppm'))
                    print('GUEST SEARCH', row['name'], row['search_interaction']['title'],
                          row['search_interaction']['final_url'], flush=True)
                    row['search_scroll'] = capture_scroll(vm, desktop, window, name + '-search')

            desktop.key('esc')
            desktop.key('f4', 'alt')
            desktop.expect('CLOSE', f'window={window}')

        desktop.exit()
        path = WORK / (args.label + '-report.json')
        path.write_text(json.dumps(report, indent=2) + '\n')

        print('Diagnostic report:', path, flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    main()
