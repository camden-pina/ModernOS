#!/usr/bin/env python3
"""Use the real desktop/browser process, USB input and local web fixture."""
import argparse
import re
import shlex
import time
from pathlib import Path
from urllib.parse import parse_qs, urlsplit, urlencode
from test_runtime import VM, WORK, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture


def exercise(cpus, iso=None):
    WORK.mkdir(parents=True, exist_ok=True)
    fixture = WebFixture()
    vm = None
    try:
        vm = VM(cpus, storage=False, iso=iso, label='browser')
        assert_success(vm.command('mkdir -p /home/user/Projects'))
        assert_success(vm.command(
            'write /home/user/Projects/native.html \'<!doctype html><title>Native file association</title><h1>Real local HTML</h1>\''))
        vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45)

        # Trust only this disposable test CA in this guest. Production assets
        # retain the real Mozilla bundle; neither browser nor curl skips TLS.
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            f'curl -fsS --max-time 30 {fixture.url("ca.pem")} >> /etc/ssl/cert.pem')))
        vm.send('gui --trace --stats\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm);
        d.screenshot('browser-desktop-initial')
        d.find('native.html');
        d.key('ret')
        output = d.expect('OPEN', 'native.html')
        browser = int(re.findall(r'DESKTOP OPEN window=(\d+)', output)[-1])
        d.expect('BROWSER_READY', 'Native file association')
        d.screenshot('browser-local-html')

        def address(value, title=None):
            d.key('l', 'ctrl');
            vm.send(value + '\n')
            if title:
                d.expect('BROWSER_READY', title)

        address(fixture.url('page.html'), 'ModernOS HTTP page')
        deadline = time.monotonic() + 15
        while True:
            pixels = d.screenshot('browser-http-page')
            if pixels.count(bytes((220, 235, 230))) > 30000:
                break
            assert time.monotonic() < deadline, 'The fetched CSS was not rendered on the actual screen'

        assert any(path == '/style.css' for _, path in fixture.requests), fixture.requests
        assert any(path == '/image.png' for _, path in fixture.requests), fixture.requests

        address(fixture.url('script.html'), 'ModernOS JavaScript page')
        d.key('left', 'alt');
        d.expect('BROWSER_READY', 'ModernOS HTTP page')
        d.key('left', 'alt');
        d.expect('BROWSER_READY', 'Native file association')
        d.key('right', 'alt');
        d.expect('BROWSER_READY', 'ModernOS HTTP page')
        d.key('f5');
        d.expect('BROWSER_READY', 'ModernOS HTTP page')

        # Focus the rendered form input. NetSurf's upstream frontend lacks
        # general keyboard traversal of page links, so use its real mouse
        # hit testing rather than assuming three Tabs reach an input.
        left = (d.width - 840) // 2 + (browser - 1) % 5 * 16
        top = (d.height - 42 - 560) // 2 + (browser - 1) % 5 * 16

        # At the port's 96-DPI CSS/font scale, the form center is 333 px
        # below the native window top, after the heading, links and image.
        d.click(left + 70, top + 333)
        d.key('a', 'ctrl');
        d.vm.send('aA!@#$%^&*()')
        time.sleep(.3)
        d.screenshot('browser-form-input')
        d.click(left + 232, top + 333)
        d.expect('BROWSER_URL', '/form?' + urlencode({'query': 'aA!@#$%^&*()'}))
        deadline = time.monotonic() + 15
        while not any(urlsplit(path).path == '/form' and
                      parse_qs(urlsplit(path).query).get('query') == ['aA!@#$%^&*()']
                      for _, path in fixture.requests):
            assert time.monotonic() < deadline, fixture.requests
            time.sleep(.05)

        d.expect('BROWSER_READY', 'ModernOS HTTP page')

        address(fixture.url('download.bin'))
        d.expect('BROWSER_STATUS', 'Download saved to /home/user/Downloads/download.bin')
        d.key('esc')

        address(fixture.url('download.bin'))
        d.expect('BROWSER_ERROR', 'Download could not be saved')
        d.key('esc')

        address(fixture.url('second.html', tls=True), 'ModernOS second page')

        assert (fixture.https, 'GET', '/second.html') in fixture.requests_by_port, fixture.requests_by_port

        for name, port in [('hostname', fixture.wrong_host), ('trust', fixture.untrusted)]:
            address(f'https://10.0.2.2:{port}/second.html', 'Privacy error')
            d.screenshot('browser-rejected-' + name)
            assert not any(p == port for p, _,
                           _ in fixture.requests_by_port), fixture.requests_by_port

        address(fixture.url('page.html'), 'ModernOS HTTP page')

        # Shared window operations remain live while the engine owns content.
        d.click(left + 840 - 43, top + 20);
        d.expect('MAXIMIZE')
        time.sleep(1);
        d.screenshot('browser-maximized')
        d.key('right', 'meta_l');
        d.expect('SNAP')
        time.sleep(1);
        d.screenshot('browser-snapped')
        d.key('2', 'meta_l');
        d.expect('WORKSPACE', 'workspace=2')

        other = d.launch('Browser')
        assert other != browser
        d.expect('BROWSER_READY', 'ModernOS Browser')
        d.key('1', 'meta_l');
        d.expect('WORKSPACE', 'workspace=1')
        d.screenshot('browser-restored-workspace')
        d.key('f4', 'alt');
        d.expect('CLOSE', f'window={browser}')
        d.key('2', 'meta_l');
        d.expect('WORKSPACE', 'workspace=2')
        d.key('f4', 'alt');
        d.expect('CLOSE', f'window={other}')
        d.exit()
        script = ('sha256sum /home/user/Downloads/download.bin; '
                  'find /home/user/Downloads -name "*.part"; '
                  'test -f /home/user/.config/netsurf/Cookies')
        output = vm.command('build-sh -c ' + shlex.quote(script), fixture.sha256, timeout=30)
        assert '\n/home/user/Downloads/.download-' not in output, output
        assert not re.search(r'pid \d+ exited [1-9]', output), output
        tasks = vm.command('ps')
        assert not re.search(r'\b(?:netsurf|web-broker|web-renderer)\b', tasks), tasks
        vm.log.flush()

        assert 'BROWSER_FORCED_CLOSE' not in Path(vm.log.name).read_text(
            errors='replace'), 'Browser shutdown did not finish gracefully'

        print('PASS native Browser: real file association, HTTP/CSS/PNG, JavaScript, forms, history/reload, safe downloads, HTTPS/trust/hostname rejection, resize/snap/workspaces, child cleanup', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--iso')
    args = parser.parse_args()
    exercise(args.cpus, args.iso)
