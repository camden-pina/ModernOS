#!/usr/bin/env python3
"""Check boot-time sandbox units in a --verify-sandbox QA image.

This verifier intentionally injects failures into subsequent live namespace
creation. Normal browser/network behavior is tested in the production image.
"""
import argparse
import json
from pathlib import Path
import re
import time
import traceback
import test_runtime as runtime
import test_desktop as desktop


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--iso', type=Path, required=True)
    p.add_argument('--output-root', type=Path, required=True)
    p.add_argument('--seccomp-launcher', help='fixed QA frontend that clears the login context before the C2 fixture')
    a = p.parse_args()
    out = a.output_root.resolve(); out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = desktop.WORK = out
    vm = None; report = dict(state='failed', iso=str(a.iso.resolve()), checks=[])
    try:
        vm = runtime.VM(1, storage=False, network=True, memory='16G', accel='kvm', iso=a.iso.resolve(), label='browser-sandbox')
        evidence = Path(vm.log.name).read_text(errors='replace')
        assert evidence.count('S1_UNIT_PASS unpublished_records_only bounded_states rollback') == 2
        assert 'C2_SELFTEST_PASS' in evidence
        assert 'P1_SELFTEST_PASS' in evidence
        assert 'PANIC:' not in evidence
        report['checks'] = ['Full sixteen-state exhaustion, rollback and cleanup with NNP disabled and enabled',
                            'C2 budget allocation and accounting selftests', 'P1 namespace/identity boundary selftests']
        assert 'USERLAND CHECK PASS' in vm.command('check', timeout=45)
        if a.seccomp_launcher:
            filters = vm.command(a.seccomp_launcher, timeout=120)
            assert 'F1_SECCOMP_BOUNDED_SUITE_PASS' in filters and 'F1_FAIL' not in filters, filters[-4000:]
            report['checks'].append('Full classic BPF suite, 16,384-instruction quota and 32-object refund/reuse boundaries')
        report['state'] = 'passed'
        print('PASS kernel sandbox capacity, accounting, rollback, cleanup and native userland', flush=True)
    except BaseException as error:
        report['error'] = repr(error); (out / 'exception.txt').write_text(traceback.format_exc()); raise
    finally:
        if vm: vm.close()
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__': main()
