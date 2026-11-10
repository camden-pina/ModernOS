#!/usr/bin/env python3
"""Use Chromium, WebSockets and real USB input to operate a nested Coast VM."""
import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import time
import traceback

from guest_ramfs import GuestRamFS
from serve_repository import RepositoryServer
from source_tree import digest
import test_runtime as runtime
import test_desktop as desktop


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--nested-iso', type=Path)
    parser.add_argument('--native-runtime', type=Path)
    parser.add_argument('--native-ninja', type=Path)
    parser.add_argument('--repository', type=Path)
    parser.add_argument('--preinstalled', action='store_true', help='qualify the source components already assembled into a fresh image')
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    if args.preinstalled:
        image = json.loads(args.iso.with_name('MANIFEST.json').read_text())
        if not image.get('wider_integrations') or digest(args.iso) != image['iso_sha256']:
            parser.error('Select an exact fresh wider image')
        native = Path(image['components']['native_build']['path'])
        if digest(native) != image['components']['native_build']['sha256']:
            parser.error('Native source receipt changed')
        source_native = json.loads(native.read_text())
        args.nested_iso = args.nested_iso or Path(source_native['iso'])
        if digest(args.nested_iso) != source_native['iso_sha256']:
            parser.error('Nested ISO must match this image source-built native base')
    elif not all((args.nested_iso, args.native_runtime, args.native_ninja, args.repository)):
        parser.error('Supply the native artifacts and repository, or use --preinstalled')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = desktop.WORK = out
    report = dict(state='failed', checks=[], target_host_executed=False, iso_sha256=sha(args.iso),
        nested_iso_sha256=sha(args.nested_iso), native_runtime_sha256=sha(args.native_runtime) if args.native_runtime else None,
        preinstalled_source_components=args.preinstalled)
    kernel = out / 'boot-kernel.elf'
    subprocess.run(['xorriso', '-osirrox', 'on', '-indev', str(args.iso.resolve()),
        '-extract', '/boot/kernel.elf', str(kernel)], check=True,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    vm = display = server = thread = fixture = None

    def check(name, command, expected, user=True, timeout=180):
        if command.startswith(('coast server ', 'coast install ')):
            vm.command('true')
        output = vm.shell_command(command, expect=expected, as_user=user, timeout=timeout)
        (out / (f'{len(report["checks"]):02d}-output.txt')).write_text(output)
        report['checks'].append(name)
        print('PASS ' + name, flush=True)
        return output

    def raw():
        while vm.selector.select(.005):
            data = os.read(vm.process.stdout.fileno(), 65536)
            if not data:
                break
            vm.pending.extend(data)
            vm.log.write(data)
            vm.log.flush()
        return Path(vm.log.name).read_text(errors='replace')

    def wait(pattern, start=0, timeout=120):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            text = raw()[start:]
            if 'PANIC:' in text:
                raise AssertionError(text[-4000:])
            if re.search(pattern, text):
                return
            time.sleep(.04)
        raise AssertionError('Missing ' + pattern + '\n' + raw()[-4000:])

    def type_text(text):
        keys = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus', ':': 'semicolon', '?': 'slash', '=': 'equal'}
        for char in text:
            raw()
            display.key(keys.get(char, char.lower()), *(['shift'] if char.isupper() or char in ':?' else []))

    try:
        if args.preinstalled:
            from desktop_repository_fixture import RepositoryFixture
            fixture = RepositoryFixture(out / 'fixture')
            args.repository = fixture.build(args.nested_iso)
        server = RepositoryServer(('127.0.0.1', 0), args.repository)
        fixtures = [('native.iso', args.nested_iso)] if args.preinstalled else [('native.iso', args.nested_iso), ('runtime.tar.gz', args.native_runtime),
                           ('ninja', args.native_ninja), ('ninja_check.py', runtime.ROOT / 'ports/qemu/ninja_check.py')]
        for name, path in fixtures:
            path = path.resolve(strict=True)
            stat = path.stat()
            server.repository['assets']['/distfiles/viewer-' + name] = dict(path=path, sha256=sha(path),
                identity=(stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        origin = 'http://10.0.2.2:' + str(server.server_port)
        vm = runtime.VM(2, storage=False, network=True, memory='20G', accel='kvm',
                        network_backend='passt' if args.preinstalled else 'user',
                        iso=args.iso.resolve(), label='qemu-chromium-viewer')
        vm.command('true')
        (out / 'network-startup.txt').write_text(vm.command('netinfo'))
        if not args.preinstalled:
            check('select signed candidate repository', 'coast server ' + origin, 'Package downloads and source builds now use')
            check('verify signed package metadata', 'pacman -Syy --noconfirm', 'Synchronizing', user=False)
            check('install source noVNC client and local VM launcher', 'coast install coast-vm --noconfirm', 'installing coast-vm', timeout=300)
            check('fetch the native-built ISO and emulator fixture',
            'curl -fsS ' + origin + '/distfiles/viewer-native.iso -o /tmp/nested.iso && '
            'curl -fsS ' + origin + '/distfiles/viewer-runtime.tar.gz -o /tmp/runtime.tar.gz && '
            'mkdir -p /home/user/qemu && tar -xzf /tmp/runtime.tar.gz -C /home/user/qemu && echo NATIVE_FIXTURES_READY', 'NATIVE_FIXTURES_READY')
            check('fetch the native Ninja cancellation fixture',
            'curl -fsS ' + origin + '/distfiles/viewer-ninja -o /tmp/ninja && chmod 755 /tmp/ninja && '
            'curl -fsS ' + origin + '/distfiles/viewer-ninja_check.py -o /tmp/ninja_check.py && '
            'python3 /tmp/ninja_check.py --ninja /tmp/ninja', 'NINJA_NATIVE_PASS')
        else:
            check('fetch and verify the matching source-built native Coast ISO',
                'curl -q --noproxy "*" --retry 20 --retry-all-errors --retry-delay 1 --connect-timeout 3 --max-time 90 -fsS ' + origin + '/distfiles/viewer-native.iso -o /tmp/nested.iso && sha256sum /tmp/nested.iso', report['nested_iso_sha256'])
        check('launch the source-built emulator with local HTTP and WebSocket listeners',
            'coast-vm --iso /tmp/nested.iso --memory 384 ' + ('' if args.preinstalled else
            '--qemu /home/user/qemu/usr/bin/qemu-system-x86_64 --firmware /home/user/qemu/usr/share/qemu ') +
            '> /tmp/viewer.log 2>&1 & echo $! > /tmp/viewer.pid', '')
        check('native Python serves the real VM state',
            'sleep 3; curl -fsS http://127.0.0.1:6080/coast-status', '"running": true')
        vm.shell_command("mkdir -p /home/user/.config && printf 'scale=3\\nterminal_shell=1\\n' > /home/user/.config/desktop.ini", as_user=True)
        vm.send("build-sh -c 'COAST_BROWSER_QA=1 exec /bin/gui --trace --stats'\n")
        vm.until(rb'GUI_READY:', 60)
        display = desktop.DesktopTest(vm)
        window = display.launch('Chromium Browser')
        wait(r'COAST_BROWSER .*"event":"ready"')
        wait(r'COAST_BROWSER .*"event":"page"[^\n]*Welcome to Coast')
        start = len(raw())
        display.key('l', 'ctrl')
        type_text('http://127.0.0.1:6080/coast.html?port=5701')
        display.key('ret')
        wait(r'COAST_BROWSER .*"event":"page"[^\n]*Coast VM connected', start)
        # VNC connects while firmware is still booting. Wait until the guest
        # has initialized its USB devices and reached the shell before typing.
        wait(r'COAST_BROWSER .*"event":"page"[^\n]*Coast VM shell ready', start, timeout=180)
        time.sleep(2)
        before = display.screenshot('01-nested-shell')
        geometry = display.geometry(window)
        report['geometry'] = geometry
        report['checks'].append('Chromium loads the source viewer and completes the actual VNC WebSocket handshake')
        print('PASS ' + report['checks'][-1], flush=True)
        display.click(geometry['x'] + geometry['width'] // 2, geometry['y'] + geometry['height'] // 2)
        start = len(raw())
        type_text('gui --trace')
        display.key('ret')
        wait(r'COAST_BROWSER .*"event":"page"[^\n]*Coast VM desktop ready', start, timeout=180)
        time.sleep(2)
        after = display.screenshot('02-nested-desktop')
        changed = sum(before[i:i+3] != after[i:i+3] for i in range(0, len(before), 3))
        if changed < 10000:
            raise AssertionError('Nested graphical framebuffer did not visibly replace its console')
        report['changed_pixels'] = changed
        report['checks'].append('real USB input travels through Chromium, WebSocket VNC and nested USB to open the source-built desktop')
        print('PASS ' + report['checks'][-1], flush=True)
        from PIL import Image
        crop = Image.open(out / '02-nested-desktop.ppm').crop((geometry['x'], geometry['y'],
            geometry['x'] + geometry['width'], geometry['y'] + min(550, geometry['height'])))
        crop.resize((crop.width * 2, crop.height * 2)).save(out / 'viewer-controls.png')
        words = subprocess.check_output(['tesseract', str(out / 'viewer-controls.png'), 'stdout', '--psm', '6', 'tsv'], text=True, stderr=subprocess.DEVNULL)
        choices = [word for word in csv.DictReader(io.StringIO(words), delimiter='\t')
                   if word['text'].strip().lower() == 'shell' and int(word['left']) > geometry['width']]
        if not choices:
            raise AssertionError('Cannot locate the visible Shell button in the viewer controls')
        word = max(choices, key=lambda w: int(w['left']))
        start = len(raw())
        display.click(geometry['x'] + (int(word['left']) + int(word['width']) // 2) // 2,
                      geometry['y'] + (int(word['top']) + int(word['height']) // 2) // 2)
        wait(r'COAST_BROWSER .*"event":"page"[^\n]*Coast VM shell ready', start, timeout=90)
        display.screenshot('03-nested-shell-return')
        report['checks'].append('viewer Shell control sends Ctrl+Alt+Esc to the nested desktop and returns to its console')
        display.key('f4', 'alt')
        wait(r'C1_RUNTIME_DESCENDANTS_REAPED helpers=\d+ complete=1', timeout=60)
        display.exit()
        display = None
        check('stopping the launcher closes all three local listeners and its QEMU child',
            'kill -TERM $(cat /tmp/viewer.pid); sleep 2; python3 -c "import socket; '
            'assert all(socket.socket().connect_ex((\'127.0.0.1\',p)) for p in (6080,5701,5901)); print(\'VIEWER_STOP_PASS\')"', 'VIEWER_STOP_PASS')
        vm.command('check', expect='USERLAND CHECK PASS', timeout=120)
        report['checks'].append('outer Coast remains healthy after nested desktop and browser cleanup')
        report['state'] = 'passed'
    except BaseException as error:
        report['error'] = repr(error)
        (out / 'exception.txt').write_text(traceback.format_exc())
        if display:
            try:
                display.screenshot('failure')
            except BaseException:
                pass
        raise
    finally:
        if vm:
            try:
                vm.qmp('stop')
                ramfs = GuestRamFS(vm, kernel, out / 'memory')
                try:
                    ramfs.export('/tmp/viewer.log', out / 'viewer.log')
                    match = re.search(r'VM logs: (/tmp/coast-vm-[^\s]+)', (out / 'viewer.log').read_text())
                    if match:
                        for name in ('serial.log', 'http.log'):
                            ramfs.export(match[1] + '/' + name, out / ('nested-' + name))
                except FileNotFoundError:
                    pass
            except BaseException as error:
                report['export_error'] = repr(error)
            finally:
                vm.close()
        if server:
            server.shutdown()
            server.server_close()
        if thread:
            thread.join(5)
        if fixture:
            fixture.close()
        report['owned_vm_stopped'] = bool(vm and vm.process.poll() is not None)
        report['image_unchanged'] = digest(args.iso) == report['iso_sha256']
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
