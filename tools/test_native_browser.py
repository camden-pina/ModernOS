#!/usr/bin/env python3
"""Verify WebKit pixels and USB interaction in the real native Browser window."""
import argparse
import re
import shlex
import time
from pathlib import Path
from test_runtime import VM, WORK, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture


def exercise(cpus, iso):
    fixture = WebFixture(webcore=True)
    vm = None
    try:
        vm = VM(cpus, storage=False, iso=iso, label='native-webkit')
        assert_success(vm.command('mkdir -p /home/user/Projects'))
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        assert_success(vm.command(
            'build-sh -c ' + shlex.quote(f'curl -fsS --max-time 30 {fixture.url("ca.pem")} >> /etc/ssl/cert.pem')))
        vm.send('gui --trace --stats\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm);
        d.screenshot(f'native-webkit-start-{cpus}cpu')

        window = d.launch('Browser');
        d.expect('BROWSER_READY', 'ModernOS Browser')
        d.key('f5');
        d.expect('BROWSER_READY', 'ModernOS Browser')
        left = (d.width - 840) // 2 + (window - 1) % 5 * 16
        top = (d.height - 42 - 560) // 2 + (window - 1) % 5 * 16
        body_y = top + 40

        def address(url, title):
            d.key('l', 'ctrl');
            vm.send(url + '\n');
            d.expect('BROWSER_READY', title)

        def pixel(x, y):
            data = d.screenshot(f'native-webkit-current-{cpus}cpu')
            index = ((body_y + y) * d.width + left + x) * 3
            return tuple(data[index:index + 3])

        def wait_pixel(x, y, color, timeout=12):
            deadline = time.monotonic() + timeout
            while True:
                actual = pixel(x, y)
                if actual == color:
                    return
                assert time.monotonic() < deadline, (x, y, actual, color)
                time.sleep(.05)

        address(fixture.url('webcore.html'), 'WebCore rendered')
        wait_pixel(25, 25, (224, 32, 48));
        wait_pixel(75, 25, (32, 64, 224))
        wait_pixel(225, 25, (208, 48, 192));
        wait_pixel(225, 85, (0, 204, 238))

        # First shortcut after an origin switch must reach the fresh page
        # with Ctrl, including after Ctrl+L was consumed by the toolbar.
        d.click(left + 50, body_y + 155);
        d.key('a', 'ctrl');
        vm.send('first')
        d.expect('BROWSER_TITLE', 'WebCore typed: first')
        d.key('a', 'ctrl');
        d.key('backspace');
        d.expect('BROWSER_TITLE', 'WebCore typed: <empty>')
        d.click(left + 50, body_y + 120);
        d.expect('BROWSER_TITLE', 'WebCore clicked')
        wait_pixel(145, 105, (0, 204, 68))
        d.click(left + 50, body_y + 155);
        vm.send('webki');
        d.expect('BROWSER_TITLE', 'WebCore typed: webki')
        d.key('backspace');
        d.expect('BROWSER_TITLE', 'WebCore typed: webk');
        vm.send('it')
        d.expect('BROWSER_TITLE', 'WebCore typed: webkit');
        wait_pixel(145, 105, (17, 34, 238))
        d.key('a', 'ctrl');
        vm.send('replaced');
        d.expect('BROWSER_TITLE', 'WebCore typed: replaced')
        d.key('a', 'ctrl');
        vm.send('webkit');
        d.expect('BROWSER_TITLE', 'WebCore typed: webkit')
        d.move(left + 270, body_y + 200)
        started = time.monotonic()

        # Wheel-down is negative in the OS input protocol. At 100 px/tick,
        # a four-tick gesture exposes the known marker at document y=520.
        for _ in range(4):
            vm.qmp('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'wheel-down', 'down': True}},
                                                   {'type': 'btn', 'data': {'button': 'wheel-down', 'down': False}}]})
        wait_pixel(25, 145, (17, 170, 51), timeout=4)
        latency = time.monotonic() - started
        assert latency < 4, latency
        print(f'Native Browser wheel pixels: {latency:.2f}s ({cpus} CPUs)', flush=True)

        for _ in range(4):
            vm.qmp('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'wheel-up', 'down': True}},
                                                   {'type': 'btn', 'data': {'button': 'wheel-up', 'down': False}}]})
        wait_pixel(25, 25, (224, 32, 48))
        d.click(left + 50, body_y + 155);
        d.key('ret');
        d.expect('BROWSER_READY', 'WebCore form submitted')
        wait_pixel(5, 5, (17, 136, 187))

        assert (fixture.http, 'POST', '/webcore-form') in fixture.requests_by_port, fixture.requests_by_port

        address(fixture.url('webcore.html', tls=True), 'WebCore rendered')
        wait_pixel(25, 25, (224, 32, 48))
        d.key('left', 'alt');
        d.expect('BROWSER_READY', 'WebCore form submitted');
        wait_pixel(5, 5, (17, 136, 187))

        assert fixture.requests_by_port.count(
            (fixture.http, 'POST', '/webcore-form')) == 1, fixture.requests_by_port
        d.key('f5');
        d.expect('BROWSER_READY', 'WebCore form submitted');
        wait_pixel(5, 5, (17, 136, 187))

        assert fixture.requests_by_port.count(
            (fixture.http, 'POST', '/webcore-form')) == 2, fixture.requests_by_port
        d.key('right', 'alt');
        d.expect('BROWSER_READY', 'WebCore rendered');
        wait_pixel(25, 25, (224, 32, 48))
        d.key('f5');
        d.expect('BROWSER_READY', 'WebCore rendered')

        address(fixture.url('webcore-services'), 'WebCore services first');
        wait_pixel(5, 40, (238, 119, 34))

        address(fixture.url('webcore-services', tls=True), 'WebCore services first');
        wait_pixel(5, 40, (238, 119, 34))

        address(fixture.url('webcore-services?mode=return'), 'WebCore services return');
        wait_pixel(5, 40, (238, 119, 34))

        address(fixture.url('webcore-frame'), 'WebCore frame ready')
        wait_pixel(5, 5, (119, 51, 153));
        wait_pixel(25, 25, (224, 32, 48))

        address(fixture.url('webcore-history'), 'WebCore history first')
        wait_pixel(25, 25, (17, 136, 187))
        d.click(left + 50, body_y + 35);
        d.expect('BROWSER_TITLE', 'WebCore history second');
        wait_pixel(5, 5, (238, 119, 34))
        d.move(left + 270, body_y + 200)
        for _ in range(4):
            vm.qmp('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'wheel-down', 'down': True}},
                                                   {'type': 'btn', 'data': {'button': 'wheel-down', 'down': False}}]})
        wait_pixel(25, 145, (17, 170, 51))
        d.key('left', 'alt');
        d.expect('BROWSER_TITLE', 'WebCore history first');
        wait_pixel(25, 25, (17, 136, 187))
        d.key('right', 'alt');
        d.expect('BROWSER_TITLE', 'WebCore history second');
        wait_pixel(25, 145, (17, 170, 51))

        # A DOM history.back call must also update the native history cursor.
        for _ in range(4):
            vm.qmp('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'wheel-up', 'down': True}},
                                                   {'type': 'btn', 'data': {'button': 'wheel-up', 'down': False}}]})
        wait_pixel(25, 25, (17, 136, 187));
        d.click(left + 150, body_y + 35)
        d.expect('BROWSER_TITLE', 'WebCore history first');
        wait_pixel(5, 5, (119, 51, 153))

        # Back restores its document; the explicit reload above resubmits the
        # correct POST body, and no traversal turns it into a network GET.
        assert (fixture.http, 'GET',
                '/webcore-form') not in fixture.requests_by_port, fixture.requests_by_port
        assert fixture.requests_by_port.count(
            (fixture.http, 'POST', '/webcore-form')) == 2, fixture.requests_by_port

        d.click(left + 840 - 43, top + 20);
        d.expect('MAXIMIZE')
        left = top = 0;
        body_y = 40
        wait_pixel(5, 5, (119, 51, 153))
        wait_pixel(d.width - 30, d.height - 120, (119, 51, 153))
        d.screenshot(f'native-webkit-maximized-{cpus}cpu')

        # Dense page scrolling must translate actual content while preserving
        # fixed and script-changing overlays at the maximized guest resolution.
        address(fixture.url('webcore-dense-scroll'), 'WebCore dense scroll')
        wait_pixel(1200, 5, (0, 204, 238));
        wait_pixel(5, 500, (188, 124, 84));
        wait_pixel(280, 90, (238, 119, 34))

        d.move(500, 600);
        dense_started = time.monotonic()
        for _ in range(4):
            vm.qmp('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'wheel-down', 'down': True}},
                                                   {'type': 'btn', 'data': {'button': 'wheel-down', 'down': False}}]})
        wait_pixel(5, 500, (46, 142, 26), timeout=4)
        dense_latency = time.monotonic() - dense_started
        wait_pixel(1200, 5, (0, 204, 238));
        wait_pixel(280, 90, (0, 204, 68))

        retained = d.screenshot(f'native-webkit-retained-{cpus}cpu')
        d.key('r');
        d.expect('BROWSER_TITLE', 'WebCore dense repainted')
        wait_pixel(5, 500, (46, 142, 26));
        wait_pixel(1200, 5, (0, 204, 238));
        wait_pixel(280, 90, (0, 204, 68))
        repainted = d.screenshot(f'native-webkit-repainted-{cpus}cpu')

        begin = body_y * d.width * 3;
        end = (d.height - 84) * d.width * 3
        assert retained[begin:end] == repainted[begin:end], 'Retained scrolling differs from a complete repaint'

        for _ in range(4):
            vm.qmp('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'wheel-up', 'down': True}},
                                                   {'type': 'btn', 'data': {'button': 'wheel-up', 'down': False}}]})
        wait_pixel(5, 500, (188, 124, 84));
        wait_pixel(1200, 5, (0, 204, 238));
        wait_pixel(280, 90, (238, 119, 34))

        print(f'Native Browser dense wheel pixels: {dense_latency:.2f}s ({cpus} CPUs)', flush=True)

        address(fixture.url('webcore-clipped-scroll'), 'WebCore clipped scroll')
        wait_pixel(300, 210, (0, 204, 238));
        wait_pixel(30, 400, (148, 212, 28));
        wait_pixel(430, 430, (238, 119, 34))

        d.move(100, 420)
        for _ in range(4):
            vm.qmp('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'wheel-down', 'down': True}},
                                                   {'type': 'btn', 'data': {'button': 'wheel-down', 'down': False}}]})
        wait_pixel(30, 400, (6, 230, 226));
        wait_pixel(300, 210, (0, 204, 238))

        d.move(700, 600)
        for _ in range(4):
            vm.qmp('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'wheel-down', 'down': True}},
                                                   {'type': 'btn', 'data': {'button': 'wheel-down', 'down': False}}]})
        wait_pixel(610, 130, (17, 170, 51));
        wait_pixel(30, 400, (6, 230, 226));
        wait_pixel(430, 430, (238, 119, 34))

        retained = d.screenshot(f'native-webkit-clipped-retained-{cpus}cpu')
        d.key('r');
        d.expect('BROWSER_TITLE', 'WebCore clipped repainted')
        wait_pixel(610, 130, (17, 170, 51));
        wait_pixel(30, 400, (6, 230, 226));
        wait_pixel(430, 430, (238, 119, 34))
        repainted = d.screenshot(f'native-webkit-clipped-repainted-{cpus}cpu')
        assert retained[begin:end] == repainted[begin:
                                                end], 'Clipped/sticky/filtered scrolling differs from a complete repaint'

        address(fixture.url('webcore-images'), 'WebCore images ready')
        wait_pixel(30, 30, (16, 136, 188));
        wait_pixel(90, 30, (208, 48, 192));
        wait_pixel(150, 30, (16, 136, 188))
        wait_pixel(30, 90, (0, 204, 238));
        wait_pixel(90, 90, (0, 204, 238));
        wait_pixel(150, 90, (0, 204, 238))
        wait_pixel(30, 150, (0, 204, 238))
        wait_pixel(300, 100, (16, 136, 188));
        wait_pixel(200, 20, (250, 250, 250))

        address(fixture.url('webcore-fonts'), 'WebCore fonts ready')
        wait_pixel(1000, 80, (0, 204, 68))
        font_pixels = d.screenshot(f'native-webkit-fonts-{cpus}cpu')
        assert any(font_pixels[((body_y + y) * d.width + x) * 3:((body_y + y) * d.width + x) * 3 + 3] == b'\0\0\0'
                   for y in range(10, 35) for x in range(10, 250)), 'Generic font text was not painted'

        address(fixture.url('webcore-frame-scheduling'), 'WebCore frame scheduling ready')
        wait_pixel(1000, 80, (0, 204, 68))

        # A prepared full-size paint and title must drain while genuine JSC is
        # occupied, rather than leaving a partially updated window for seconds.
        transport_started = time.monotonic()

        address(fixture.url('webcore-transport'), 'WebCore transport ready')
        d.expect('BROWSER_LOG', 'WebCore script busy')
        wait_pixel(d.width - 30, d.height - 150, (32, 64, 224), timeout=3)
        transport_latency = time.monotonic() - transport_started
        assert transport_latency < 4, ('Prepared paint stalled behind JavaScript',
                                       transport_latency)
        d.expect('BROWSER_TITLE', 'WebCore script released')
        print(
            f'Native Browser output during busy JavaScript: {transport_latency:.2f}s ({cpus} CPUs)', flush=True)
        d.key('f4', 'alt');
        d.expect('CLOSE', f'window={window}')

        window = d.launch('Browser');
        d.expect('BROWSER_READY', 'ModernOS Browser')
        left = (d.width - 840) // 2 + (window - 1) % 5 * 16
        top = (d.height - 42 - 560) // 2 + (window - 1) % 5 * 16;
        body_y = top + 40

        address(fixture.url('webcore-services?mode=new-window'), 'WebCore services new-window');
        wait_pixel(5, 40, (238, 119, 34))

        d.key('l', 'ctrl');
        vm.send(fixture.url('download.bin') + '\n')
        d.expect('BROWSER_STATUS', 'Download saved to /home/user/Downloads/download.bin')
        d.key('esc')

        d.key('l', 'ctrl');
        vm.send(fixture.url('download.bin') + '\n')
        d.expect('BROWSER_ERROR', 'Download could not be saved');
        d.key('esc')
        d.key('f4', 'alt');
        d.expect('CLOSE', f'window={window}')

        d.exit();
        output = vm.command('build-sh -c ' + shlex.quote(
            'sha256sum /home/user/Downloads/download.bin; find /home/user/Downloads -name "*.part"'))
        assert fixture.sha256 in output, output
        assert not re.search(r'(?:^|\n)/home/user/Downloads/.*\.part(?:\r?\n|$)', output), output
        tasks = vm.command('ps')
        assert not re.search(r'\bweb-(?:broker|renderer)\b', tasks), tasks
        vm.log.flush()

        assert 'GLib-CRITICAL' not in Path(vm.log.name).read_text(
            errors='replace'), 'GLib critical during native rendering'
        assert 'BROWSER_SCROLL_COPY' in Path(vm.log.name).read_text(
            errors='replace'), 'Retained scroll copies were not exercised'
        assert 'Native backing scroll: WebCore retained raster copy' in Path(
            vm.log.name).read_text(errors='replace'), 'WebCore backing scroll was not exercised'
        assert 'BROWSER_PRESENT' in Path(vm.log.name).read_text(
            errors='replace'), 'Complete native paint was not observed'
        assert 'BROWSER_PAINT_CANCEL' in Path(vm.log.name).read_text(
            errors='replace'), 'Renderer-change paint cancellation was not exercised'
        assert 'BROWSER_FORCED_CLOSE' not in Path(vm.log.name).read_text(
            errors='replace'), 'Normal browser close required forced termination'

        print(
            f'PASS native WebKit Browser: Grid/canvas/PNG/JPEG/WebP/SVG/Shadow DOM pixels, click/editing/backspace, wheel pixels ({latency:.2f}s), dense/nested/sticky/filtered scroll and full-repaint equality, fonts/zero size, frame/microtask callbacks, atomic paints and busy-JavaScript output, form POST, HTTP/TLS origin change, history/reload, cookies/HttpOnly/credentials, origin-isolated UTF-16 storage/quota, resize and graceful child cleanup ({cpus} CPUs)', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpus', type=int, nargs='+', default=[4])
    parser.add_argument('--iso', required=True)
    args = parser.parse_args()
    for cpus in args.cpus:
        exercise(cpus, args.iso)
