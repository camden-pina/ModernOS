#!/usr/bin/env python3
"""Exercise additional upstream programs in an owned, diskless Coast VM."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import time
import traceback
import test_runtime as runtime
import test_desktop as desktop


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = desktop.WORK = out
    vm = None
    report = dict(state='failed', iso=str(args.iso.resolve()), checks=[],
                  harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  target_host_executed=False)
    try:
        vm = runtime.VM(4, storage=False, network=False, memory='16G', accel='kvm',
                        iso=args.iso, label='extra-programs')
        vm.user_command('mkdir -p /home/user/Documents')
        result = vm.shell_command('python3 /usr/share/coast/program-ports/extra-check.py /home/user/Documents',
                                  expect='EXTRA_PROGRAMS_ALL_PASS', as_user=True, timeout=240)
        report['checks'] = [line.removeprefix('EXTRA_PROGRAMS_PASS ')
                            for line in result.splitlines() if line.startswith('EXTRA_PROGRAMS_PASS ')]
        assert len(report['checks']) == 13, result
        vm.shell_command("printf '6*7\\n' | bc | zstd -q -c | unzstd -c", expect='42', as_user=True)
        report['checks'].append('actual shell pipeline through bc, zstd and unzstd')
        # GNU bc's real interactive process receives terminal input and exits.
        vm.send("build-sh -c 'bc -q'\n")
        time.sleep(.3)
        vm.send('123456789*987654321\n')
        vm.until(rb'121932631112635269', 20)
        vm.send('quit\n')
        vm.until_prompt(20)
        report['checks'].append('interactive arbitrary-precision calculator at the console')

        def observed():
            while vm.selector.select(.003):
                data = os.read(vm.process.stdout.fileno(), 65536)
                if not data:
                    break
                vm.pending.extend(data)
                vm.log.write(data)
                vm.log.flush()
            source = Path(vm.log.name).read_text(errors='replace')
            output = []
            for record in re.split(r'(?m)(?<=\n)(?=DESKTOP )', source):
                match = re.match(r'DESKTOP TERMINAL_OUTPUT window=\d+ workspace=\d+ ([\s\S]*)',
                                 record.removesuffix('\n'))
                if match:
                    output.append(match[1])
            return re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', ''.join(output).replace('\r', ''))

        def wait(pattern):
            end = time.monotonic() + 30
            while time.monotonic() < end:
                if re.search(pattern, observed()):
                    return
                time.sleep(.02)
            raise AssertionError('Missing ' + pattern + '\n' + observed()[-2000:])

        vm.send('gui --trace\n')
        vm.until(b'GUI_READY:', 60)
        desk = desktop.DesktopTest(vm)
        desk.screenshot('00-desktop')
        desk.launch('Terminal')
        vm.send("printf '2^100\\n' | bc | zstd -q -c | unzstd -c > /home/user/Documents/calculator.txt; printf '\\n%s%d\\n' NEW_PROGRAMS_TERMINAL_STATUS $?\n")
        wait(r'\nNEW_PROGRAMS_TERMINAL_STATUS0\n')
        vm.send('bc -q\n')
        time.sleep(.3)
        vm.send('scale=20; sqrt(2)\n')
        wait(r'1\.41421356237309504880')
        desk.screenshot('01-bc-terminal')
        vm.send('quit\n')
        time.sleep(.2)
        desk.exit()
        vm.shell_command('test "$(cat /home/user/Documents/calculator.txt)" = 1267650600228229401496703205376',
                         as_user=True)
        report['checks'].append('desktop GNU Bash pipeline, exact saved bytes and interactive calculator')
        report['state'] = 'passed'
        for check in report['checks']:
            print('PASS ' + check, flush=True)
    except BaseException as error:
        report['error'] = repr(error)
        (out / 'exception.txt').write_text(traceback.format_exc())
        raise
    finally:
        if vm:
            vm.close()
        report['owned_vm_stopped'] = bool(vm and vm.process.poll() is not None)
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
