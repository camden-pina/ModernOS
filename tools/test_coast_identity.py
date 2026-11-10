#!/usr/bin/env python3
"""Verify renamed native paths, signed catalog integrity and desktop startup."""
import argparse
import hashlib
import json
from pathlib import Path
import traceback
import test_runtime as runtime
import test_desktop as desktop


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--iso', type=Path, required=True)
    p.add_argument('--output-root', type=Path, required=True)
    a = p.parse_args()
    out = a.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = desktop.WORK = out
    report = dict(state='failed', iso=str(a.iso.resolve()), checks=[],
                  harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    vm = display = None
    try:
        vm = runtime.VM(2, storage=False, network=True, memory='16G', accel='kvm', iso=a.iso, label='coast-identity')

        def check(name, command, expected=None, user=True, timeout=60):
            output = vm.shell_command(command, expect=expected, as_user=user, timeout=timeout)
            (out / (f'{len(report["checks"]):02d}-output.txt')).write_text(output)
            report['checks'].append(name)
            print('PASS', name, flush=True)
            return output

        check('kernel, hostname and BusyBox identify as Coast', 'test "$(uname -s)" = Coast && test "$(uname -n)" = coast && test "$(busybox uname -o)" = Coast')
        check('freedesktop release metadata identifies Coast OS', 'cat /etc/os-release; cat /etc/hostname', 'PRETTY_NAME="Coast OS (Live)"')
        check('native SDK and shared source directories use Coast paths', 'test -f /usr/include/coast/window.h && test -d /usr/src/coast && test -x /bin/coast-sh')
        check('native compiler branding was rebuilt', 'gcc --version; g++ --version', 'Coast static SDK')
        check('account policy uses its renamed root-owned file', 'test -f /etc/coast-accounts; echo x >> /etc/coast-accounts; test $? -ne 0', 'Permission denied')
        check('Coast base/helper package identities are installed', 'pacman -Q coast-base brine', 'brine 1.3-1')
        check('a sudo session cannot impersonate the admitted editor sandbox', "native-node-launch-check -p 'require(\"os\").type()'; r=$?; test $r -eq 113", 'exit=113', user=False)
        check('Coast configuration pins the host and signing key', 'brine repo', 'http://10.0.2.2:8765')
        check('refresh fetches authenticated host metadata', 'brine refresh', '3 recipes')
        check('modified source metadata is rejected', 'p=/home/user/.cache/brine/current/index.json; cp $p /tmp/coast-catalog.good; printf x >> $p; brine search --source; r=$?; cp /tmp/coast-catalog.good $p; test $r -ne 0', 'BAD signature')
        check('restored authenticated catalog remains usable', 'brine search --source hello-coast', 'hello-coast')
        check('ordinary source review creates no root-owned build', 'brine show hello-coast; test ! -d /home/user/Builds/hello-coast', 'pkgname=hello-coast')
        vm.send('gui --trace --stats\n')
        vm.until(b'GUI_READY: Coast desktop', 60)
        display = desktop.DesktopTest(vm)
        display.screenshot('00-coast-desktop')
        browser = display.launch('Browser')
        display.expect('BROWSER_READY', 'Coast Browser')
        display.screenshot('01-coast-browser')
        display.key('f4', 'alt')
        display.expect('CLOSE')
        report['checks'].append('native Browser starts and presents the Coast start page')
        print('PASS native Browser starts and presents the Coast start page', flush=True)
        display.launch('Settings')
        display.screenshot('02-coast-settings')
        display.key('f4', 'alt')
        display.expect('CLOSE')
        report['checks'].append('renamed desktop preferences open Settings')
        display.exit()
        check('desktop returns cleanly to the renamed native shell', 'echo COAST_DESKTOP_RETURNED', 'COAST_DESKTOP_RETURNED')
        report['state'] = 'passed'
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
