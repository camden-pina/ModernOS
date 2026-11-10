#!/usr/bin/env python3
"""Check the protected Code OSS CLI without enabling an account password.

The target runs only in an owned diskless, networkless QEMU guest. Inherited
loader variables must never reach the privileged dynamic display bridge.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import time
import traceback

import test_desktop as desktop
import test_runtime as runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = desktop.WORK = out
    vm = display = None
    report = dict(state='failed', iso=str(args.iso.resolve()), checks=[],
                  harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())

    def raw():
        while vm.selector.select(.005):
            data = os.read(vm.process.stdout.fileno(), 65536)
            if not data:
                break
            vm.pending.extend(data)
            vm.log.write(data)
            vm.log.flush()
        return Path(vm.log.name).read_text(errors='replace')

    def observed(start=0):
        source = raw()[start:]
        kernel_pattern = r'\x1b\[0;32m\[\s*\d+[.]\d+\][^\n]*\n'
        kernel = re.findall(kernel_pattern, source)
        source = re.sub(kernel_pattern, '', source)
        output, metadata = [], []
        for record in re.split(r'(?m)(?<=\n)(?=DESKTOP )', source):
            record = record.removesuffix('\n')
            match = re.match(r'DESKTOP TERMINAL_OUTPUT window=\d+ workspace=\d+ ([\s\S]*)', record)
            if match:
                output.append(match[1])
            else:
                metadata.append(record + '\n')
        return re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '',
                      (''.join(output) + '\n' + ''.join(metadata) + ''.join(kernel)).replace('\r', ''))

    def wait(pattern, start=0, timeout=60):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            evidence = observed(start)
            assert 'PANIC:' not in evidence, evidence[-4000:]
            match = re.search(pattern, evidence)
            if match:
                return match
            time.sleep(.02)
        raise AssertionError('Missing ' + pattern + '\n' + observed(start)[-4000:])

    try:
        vm = runtime.VM(2, storage=False, network=False, memory='16G', accel='kvm',
                        iso=args.iso, label='code-launcher', root_commands=False)
        assert '\n1000\n' in vm.raw_command('id -u')
        assert 'administrator access and a profile password are required' in vm.raw_command('sudo -n id')
        vm.send('gui --trace\n')
        vm.until(b'GUI_READY:', 60)
        display = desktop.DesktopTest(vm)
        display.screenshot('00-desktop')
        display.launch('Terminal')
        wait(r'TERMINAL_MODE[^\n]*raw')
        start = len(raw())
        vm.send('LD_PRELOAD=/tmp/not-loadable.so COAST_CODEOSS_QA=1 code /tmp\n')
        wait(r'CODEOSS_PROJECT path=/tmp uid=1000', start)
        wait(r'C1_RUNTIME_CHILD uid=1000 euid=1000 gid=1000 egid=1000', start)
        wait(r'CODEOSS_DOM[^\n]*"ready":"complete"[^\n]*"fonts":"loaded"', start, 90)
        wait('ELECTRON_CLIENT_PIXELS_PRESENTED', start)
        evidence = observed(start)
        assert 'ERROR: ld.so:' not in evidence
        assert 'cannot be preloaded' not in evidence
        report['checks'].append('An owner terminal without a password launches the real sandboxed UID-1000 editor; inherited LD_PRELOAD is discarded before dynamic loading')
        display.screenshot('01-cli-editor')
        start = len(raw())
        display.key('f4', 'alt')
        wait(r'C1_RUNTIME_WAIT status=0 exit=0 signal=0 timeout=0', start)
        wait(r'C1_RUNTIME_DESCENDANTS_REAPED helpers=\d+ complete=1', start)
        wait(r'linux-bash-5[.]3[#$] ', start)
        start = len(raw())
        vm.send('id -u\n')
        wait(r'\n1000\n', start)
        display.exit()
        assert '\n1000\n' in vm.raw_command('id -u')
        assert 'Permission denied' in vm.raw_command('touch /etc/code-launcher-denied')
        report['checks'].append('Closing the editor reaps its descendants and leaves both Terminal and the physical console unprivileged')
        report['state'] = 'passed'
        for check in report['checks']:
            print('PASS ' + check, flush=True)
    except BaseException as error:
        report['error'] = repr(error)
        (out / 'exception.txt').write_text(traceback.format_exc())
        if display:
            try:
                display.screenshot('failure')
            except Exception:
                pass
        raise
    finally:
        if vm:
            vm.close()
        report['owned_vm_stopped'] = bool(vm and vm.process.poll() is not None)
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
