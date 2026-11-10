#!/usr/bin/env python3
"""Run the Linux-comparable seccomp fixture only inside an owned Coast VM."""
import argparse
import hashlib
import json
from pathlib import Path
import traceback
import test_runtime as runtime

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--iso', type=Path, required=True)
parser.add_argument('--cpus', type=int, choices=[1, 4], required=True)
parser.add_argument('--output-root', type=Path, required=True)
args = parser.parse_args()
out = args.output_root.resolve()
out.mkdir(parents=True, exist_ok=False)
runtime.WORK = out
vm = None
report = dict(state='failed', checks=[], cpus=args.cpus, iso=str(args.iso.resolve()),
              guest_program_execution_on_host=False, extra_disks=False, network=False)
try:
    vm = runtime.VM(args.cpus, storage=False, network=False, memory='16G', accel='kvm',
                    iso=args.iso.resolve(), label='package-seccomp')
    output = vm.shell_command('id -u; /usr/bin/package-seccomp-check', as_user=True, timeout=120)
    (out/'fixture-output.txt').write_text(output)
    assert 'PACKAGE_SECCOMP_PASS cases=14' in output, output
    report['checks'] = [line for line in output.splitlines() if line.startswith('PASS ')]
    vm.command('browser-sandbox-check', expect='BROWSER SANDBOX CHECK PASS', timeout=120)
    report['checks'].append('existing browser kernel sandbox still rejects escapes')
    vm.command('check', expect='USERLAND CHECK PASS', timeout=90)
    report['checks'].append('ordinary guest remains healthy after seccomp children exit')
    report['state'] = 'passed'
except BaseException as error:
    report['error'] = repr(error)
    (out/'exception.txt').write_text(traceback.format_exc())
    raise
finally:
    if vm:
        vm.close()
    report['owned_vm_stopped'] = bool(vm and vm.process.poll() is not None)
    with args.iso.open('rb') as stream:
        report['iso_sha256'] = hashlib.file_digest(stream, 'sha256').hexdigest()
    (out/'report.json').write_text(json.dumps(report, indent=2)+'\n')
