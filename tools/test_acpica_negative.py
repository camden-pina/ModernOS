#!/usr/bin/env python3
"""Verify ACPICA selftest opt-in and failure detection in disposable QEMU VMs.

No source ISO, firmware NVS or host disk is written. The deliberately unlocked
SSDT writes only the same guest-owned page used by the positive harness.
"""
import argparse
import json
import os
from pathlib import Path
import re
import time

from test_acpica import ROOT, WORK, digest, plain_log, prepare, run
from test_runtime import VM


def await_marker(vm, marker, timeout=40):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        log = plain_log(vm)
        if 'PANIC:' in log:
            raise AssertionError(log)
        if marker in log:
            return log
        if vm.process.poll() is not None:
            raise AssertionError(f'{vm.name}: exited before {marker}')
        if vm.selector.select(.1):
            data = os.read(vm.process.stdout.fileno(), 65536)
            vm.pending.extend(data)
            vm.log.write(data)
            vm.log.flush()
    raise AssertionError(f'{vm.name}: missing {marker}\n{plain_log(vm)}')


def check_shell(vm, allowed_failures=()):
    output = vm.command('check', 'USERLAND CHECK PASS', timeout=45)
    # Expected asynchronous negative-control diagnostics may arrive in the
    # same read as the shell reply. Never confuse those with a failed check.
    checked = '\n'.join(line for line in output.splitlines()
                        if not ('ACPICA SELFTEST FAIL:' in line and
                                any(message in line for message in allowed_failures)))
    assert not re.search(r'FAIL:|PANIC:|exited [1-9]', checked), output


def exercise(iso, memory, iasl):
    before = digest(iso)
    enabled_iso, aml = prepare(iso, iasl)
    negative = WORK / 'negative'
    negative.mkdir(exist_ok=True)
    source = (ROOT / 'tools/fixtures/acpica-selftest.asl').read_text()
    acquire = 'If (Acquire (MTX0, 0xFFFF)) { Return (Ones) }'
    assert source.count(acquire) == source.count('Release (MTX0)') == 2
    unlocked = negative / 'unlocked.asl'
    unlocked.write_text(source.replace(acquire, '').replace('Release (MTX0)', ''))
    prefix = negative / 'unlocked'
    (negative / 'iasl.log').write_bytes(run(iasl, '-p', prefix, unlocked))
    unlocked_aml = prefix.with_suffix('.aml')
    results = []
    destination = WORK / 'negative-controls.json'
    if destination.is_file() and not (WORK / 'negative-controls-initial.json').exists():
        (WORK / 'negative-controls-initial.json').write_bytes(destination.read_bytes())
    cases = [
        ('fixture without boot token stays inert', iso, aml, 'acpica-negative-inert'),
        ('boot token without fixture reports explicit failure, shell survives', enabled_iso, None,
         'acpica-negative-missing'),
        ('removing AML mutex is detected by concurrent lost-update counter', enabled_iso,
         unlocked_aml, 'acpica-negative-mutex'),
    ]
    try:
        for index, (description, image, fixture, label) in enumerate(cases):
            vm = None
            item = {'test': description, 'result': 'failed'}
            results.append(item)
            try:
                extra = ['-acpitable', f'file={fixture}'] if fixture else None
                vm = VM(1, storage=False, network=False, iso=image, memory=memory,
                        label=label, extra_args=extra)
                item['log'] = str(Path(vm.log.name).relative_to(ROOT))
                if index == 0:
                    check_shell(vm)
                    assert 'ACPICA SELFTEST' not in plain_log(vm), plain_log(vm)
                else:
                    marker = ('ACPICA SELFTEST FAIL: synthetic SSDT missing' if index == 1 else
                              'ACPICA SELFTEST FAIL: AML mutex protected counter')
                    await_marker(vm, marker)
                    allowed = (marker,)
                    if index == 2:
                        complete = 'ACPICA SELFTEST FAIL: complete (1 failures)'
                        await_marker(vm, complete)
                        allowed += (complete,)
                    check_shell(vm, allowed)
                    failures = [line for line in plain_log(vm).splitlines()
                                if 'ACPICA SELFTEST FAIL:' in line]
                    assert all(any(message in line for message in allowed)
                               for line in failures), failures
                    assert 'ACPICA SELFTEST PASS: complete' not in plain_log(vm)
                assert 'PANIC:' not in plain_log(vm), plain_log(vm)
                item['result'] = 'passed'
                print('PASS ' + description, flush=True)
            except BaseException as error:
                item['error'] = str(error)
                raise
            finally:
                if vm:
                    vm.close()
                destination.write_text(json.dumps(results, indent=2) + '\n')
    finally:
        unchanged = digest(iso) == before
        (negative / 'source-integrity.json').write_text(json.dumps({
            'source_iso': str(iso), 'source_sha256': before, 'source_unchanged': unchanged,
            'unlocked_ssdt_sha256': digest(unlocked_aml)}, indent=2) + '\n')
        if not unchanged:
            raise AssertionError('Source ISO changed during negative controls')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--memory', default='2G')
    parser.add_argument('--iasl', default='iasl')
    args = parser.parse_args()
    exercise(args.iso.resolve(), args.memory, args.iasl)
