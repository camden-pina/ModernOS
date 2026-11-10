#!/usr/bin/env python3
"""Boot a Coast ISO with Coast's QEMU and verify the nested kernel and shell."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import selectors
import shlex
import socket
import subprocess
import time


ANSI = re.compile(rb'\x1b\[[0-9;?]*[ -/]*[@-~]')
PROMPT = rb'os:[^\n]*\$ $'


def main():
    allocation_limit = resource.getrlimit(resource.RLIMIT_DATA)[0]
    maximum_memory = 16384 if allocation_limit == resource.RLIM_INFINITY else min(16384, allocation_limit // (1024 * 1024) - 32)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--firmware', type=Path, default=Path('/usr/share/qemu'))
    parser.add_argument('--output-root', type=Path, default=Path('/tmp/coast-nested'))
    parser.add_argument('--memory', type=int, default=384, help='nested RAM in MiB (maximum depends on the running kernel)')
    parser.add_argument('--timeout', type=int, default=900)
    parser.add_argument('--gui', action='store_true', help='also launch the nested desktop, capture it and close it through emulated USB input')
    args = parser.parse_args()
    if not 128 <= args.memory <= maximum_memory or not 1 <= args.timeout <= 3600:
        parser.error(f'memory must be 128..{maximum_memory} MiB and timeout 1..3600 seconds')
    image = args.iso.resolve(strict=True)
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    with image.open('rb') as stream:
        image_hash = hashlib.file_digest(stream, 'sha256').hexdigest()
    command = [args.qemu, '-L', str(args.firmware.resolve()), '-machine', 'q35',
        '-accel', 'tcg,thread=single', '-cpu', 'max', '-m', str(args.memory), '-smp', '1',
        '-display', 'none', '-monitor', 'none', '-serial', 'stdio', '-nic', 'none',
        '-no-reboot', '-boot', 'd', '-cdrom', str(image)]
    qmp = out / 'control.sock'
    qmp.unlink(missing_ok=True)
    command += ['-qmp', 'unix:' + str(qmp) + ',server=on,wait=off',
                '-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0',
                '-device', 'usb-mouse,bus=xhci.0']
    report = dict(state='failed', iso_sha256=image_hash, command=command, checks=[],
                  backend='TCI', extra_disks=False, network=False)
    started = time.monotonic()
    process = None
    with (out / 'serial.log').open('wb') as log, selectors.DefaultSelector() as selector:
        try:
            process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, bufsize=0)
            selector.register(process.stdout, selectors.EVENT_READ)
            pending = bytearray()

            def until(pattern, timeout):
                deadline = time.monotonic() + timeout
                while True:
                    clean = ANSI.sub(b'', bytes(pending)).replace(b'\r', b'')
                    if re.search(pattern, clean):
                        pending.clear()
                        return clean.decode(errors='replace')
                    if process.poll() is not None:
                        raise RuntimeError(f'Nested QEMU exited {process.returncode}: {clean[-4000:]!r}')
                    if time.monotonic() >= deadline:
                        raise TimeoutError(f'Nested console timed out waiting for {pattern!r}: {clean[-4000:]!r}')
                    if selector.select(min(1, max(0, deadline - time.monotonic()))):
                        data = process.stdout.read(65536)
                        if data:
                            pending.extend(data)
                            log.write(data)
                            log.flush()

            def send(text):
                process.stdin.write((text + '\n').encode())
                process.stdin.flush()

            def run(text, expected):
                send(text)
                output = until(PROMPT, args.timeout)
                if expected not in output:
                    raise AssertionError(f'{text}: missing {expected!r}: {output[-4000:]}')
                return output

            boot = until(PROMPT, args.timeout)
            if 'Coast' not in boot and 'coast' not in boot:
                raise AssertionError('No Coast kernel/shell banner on nested serial console')
            report['checks'].append('BIOS boots the ISO into the Coast serial shell')
            print('NESTED_PASS ' + report['checks'][-1], flush=True)
            run('gui --info', 'display ABI=')
            report['checks'].append('source-built desktop reports the nested kernel display ABI')
            print('NESTED_PASS ' + report['checks'][-1], flush=True)
            # The ISO is disposable. Set its initial console password so the
            # actual userland ABI fixture runs through the ordinary sudo path.
            send('passwd')
            until(rb'New password:', 60)
            password = 'Coast-nested-QA-2026!'
            send(password)
            until(rb'Confirm new password:', 60)
            send(password)
            output = until(PROMPT, args.timeout)
            if 'Password updated' not in output:
                raise AssertionError(output)
            send('sudo -S /bin/check')
            until(rb'\[sudo\] Password:', 60)
            send(password)
            output = until(PROMPT, args.timeout)
            if 'USERLAND CHECK PASS' not in output:
                raise AssertionError(output[-8000:])
            report['checks'].append('nested Coast passes its actual userland ABI checks')
            print('NESTED_PASS ' + report['checks'][-1], flush=True)
            # Match complete output lines to avoid accepting echoed input.
            output = run('build-sh -c ' + shlex.quote("printf '\\nCOAST_NESTED_%s\\n' SHELL_PASS"),
                         '\nCOAST_NESTED_SHELL_PASS\n')
            report['checks'].append('nested BusyBox executes a shell program')
            print('NESTED_PASS ' + report['checks'][-1], flush=True)
            if args.gui:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as control:
                    control.settimeout(30)
                    control.connect(str(qmp))
                    with control.makefile('rwb', buffering=0) as wire:
                        if 'QMP' not in json.loads(wire.readline()):
                            raise AssertionError('Missing nested QMP greeting')

                        def execute(name, arguments=None):
                            wire.write((json.dumps(dict(execute=name, arguments=arguments or {})) + '\n').encode())
                            while True:
                                response = json.loads(wire.readline())
                                if 'event' not in response:
                                    if 'return' not in response:
                                        raise AssertionError(response)
                                    return response['return']

                        execute('qmp_capabilities')
                        send('gui --trace')
                        until(rb'GUI_READY:', args.timeout)
                        time.sleep(1)
                        screenshot = out / 'desktop.ppm'
                        execute('screendump', dict(filename=str(screenshot)))
                        data = screenshot.read_bytes()
                        header = re.match(rb'P6\n(\d+) (\d+)\n255\n', data)
                        if not header:
                            raise AssertionError('Nested QEMU produced no PPM framebuffer')
                        width, height = map(int, header.groups())
                        pixels = data[header.end():]
                        if len(pixels) != width * height * 3 or len({pixels[i:i+3] for i in range(0, len(pixels), 192)}) < 32:
                            raise AssertionError('Nested desktop framebuffer is incomplete or blank')
                        report['desktop'] = dict(width=width, height=height, sha256=hashlib.sha256(data).hexdigest())
                        report['checks'].append('nested graphical desktop presents a real framebuffer')
                        print('NESTED_PASS ' + report['checks'][-1], flush=True)
                        for down, keys in [(True, ['ctrl', 'alt', 'esc']), (False, ['esc', 'alt', 'ctrl'])]:
                            execute('input-send-event', dict(events=[dict(type='key', data=dict(down=down,
                                key=dict(type='qcode', data=key))) for key in keys]))
                        until(PROMPT, args.timeout)
                        run('gui --info', 'display ABI=')
                        report['checks'].append('emulated USB input closes the desktop and returns to the shell')
                        print('NESTED_PASS ' + report['checks'][-1], flush=True)
            report['state'] = 'passed'
        except BaseException as error:
            report['error'] = repr(error)
            raise
        finally:
            if process is not None:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=20)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=20)
                report['owned_qemu_stopped'] = process.poll() is not None
            report['elapsed_seconds'] = round(time.monotonic() - started, 3)
            (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('COAST_NESTED_ALL_PASS ' + str(len(report['checks'])), flush=True)


if __name__ == '__main__':
    main()
