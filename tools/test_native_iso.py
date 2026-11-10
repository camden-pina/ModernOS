#!/usr/bin/env python3
"""Boot a source-built native desktop through UEFI and BIOS in owned VMs."""
import argparse
import json
from pathlib import Path
import traceback

import test_runtime as runtime
from source_tree import digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--memory', default='4G')
    args = parser.parse_args()
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    manifest = args.iso.resolve().parent / 'MANIFEST.json'
    inherited = manifest.is_file() and any(k in json.loads(manifest.read_text()) for k in ('input_iso', 'base_iso'))
    report = dict(state='failed', checks=[], inherited_iso=bool(inherited),
                  iso_sha256=digest(args.iso), owned_vms_stopped=True)
    vm = None
    try:
        for bios in [False, True]:
            mode = 'BIOS' if bios else 'UEFI'
            vm = runtime.VM(4, bios=bios, storage=False, network=False, memory=args.memory,
                accel='kvm', iso=args.iso, label='source-native')
            vm.command('check', expect='USERLAND CHECK PASS')
            vm.command('gui --info', expect='display ABI=')
            vm.shell_command('printf NATIVE_BUSYBOX_PASS', expect='NATIVE_BUSYBOX_PASS', as_user=True)
            vm.send('gui --trace\n')
            vm.until(rb'GUI_READY:', 60)
            vm.qmp('screendump', {'filename': str(out / (mode.lower() + '-desktop.ppm'))})
            for down, keys in [(True, ['ctrl', 'alt', 'esc']), (False, ['esc', 'alt', 'ctrl'])]:
                vm.qmp('input-send-event', {'events': [dict(type='key', data=dict(down=down,
                    key=dict(type='qcode', data=k))) for k in keys]})
            vm.until_prompt(30)
            vm.command('check', expect='USERLAND CHECK PASS')
            report['checks'].append(mode + ' boots source kernel, native shell, BusyBox and graphical desktop; exits cleanly')
            vm.close()
            report['owned_vms_stopped'] &= vm.process.poll() is not None
            vm = None
            print('PASS ' + report['checks'][-1], flush=True)
        report['state'] = 'passed'
    except BaseException as error:
        report['error'] = repr(error)
        (out / 'exception.txt').write_text(traceback.format_exc())
        raise
    finally:
        if vm:
            vm.close()
            report['owned_vms_stopped'] &= vm.process.poll() is not None
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
