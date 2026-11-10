#!/usr/bin/env python3
"""Verify the running portable host service from an owned diskless Coast VM."""
import argparse
import hashlib
import ipaddress
import json
from pathlib import Path
import traceback
from urllib.parse import urlsplit

import test_runtime as runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--server', required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    endpoint = urlsplit(args.server)
    assert endpoint.scheme == 'http' and endpoint.path == ''
    assert endpoint.port and not endpoint.username and not endpoint.password
    assert not endpoint.query and not endpoint.fragment
    assert ipaddress.ip_address(endpoint.hostname).version == 4
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    report = dict(state='failed', checks=[], iso=str(args.iso.resolve()),
                  server=args.server, persistent_host_service=True,
                  two_physical_computers_tested=False, target_host_executed=False,
                  extra_disks=False)
    (out/'HARNESS.py').write_bytes(Path(__file__).read_bytes())
    vm = None

    def check(name, command, expected=None, user=True, timeout=90):
        result = vm.shell_command(command, expect=expected, as_user=user, timeout=timeout)
        (out/(f'{len(report["checks"]):02d}-output.txt')).write_text(result)
        report['checks'].append(name)
        print('PASS '+name, flush=True)

    try:
        vm = runtime.VM(4, storage=False, network=True, memory='16G', accel='kvm',
                        iso=args.iso.resolve(), label='host-repository')
        check('DHCP configures the owned VM',
              'for i in 1 2 3 4 5 6 7 8 9 10 11 12; do test -s /etc/resolv.conf && break; sleep 1; done; test -s /etc/resolv.conf')
        check('default QEMU address reaches the persistent host service',
              'curl -fsS http://10.0.2.2:8765/health', 'Coast Packages')
        check('configured endpoint verifies signed package and source metadata',
              'coast server '+args.server+' --check', 'Verified package database and source catalog')
        vm.command('true')
        check('real sudo authorizes the selected mirror',
              'coast server '+args.server, 'Package downloads and source builds now use '+args.server)
        check('partial name search discovers GCC and G++ commands', 'coast search g', 'g++')
        check('GCC is first and unrelated Qt metadata is absent from one-letter search',
              "coast search --json g > /tmp/search-g.json; python3 -I -c 'import json; p=json.load(open(\"/tmp/search-g.json\"))[\"results\"]; assert p[0][\"Name\"]==\"gcc\"; assert not any(q[\"Name\"]==\"qt6-demo\" for q in p); print(\"SEARCH_ORDER_OK\")'", 'SEARCH_ORDER_OK')
        check('literal plus signs resolve the compiler package', 'coast search g++', 'coast/gcc')
        check('signed source catalog refreshes from the persistent service', 'coast refresh', '3 recipes')
        check('upstream pacman syncs the signed database from the persistent service',
              'pacman -Sy --noconfirm', 'Synchronizing', user=False)
        check('remove installed calculator and every cached archive',
              'pacman -R --noconfirm bc && rm -f /var/cache/pacman/pkg/bc-*', 'removing bc', user=False)
        vm.command('true')
        check('download and install the real signed calculator through the service',
              'coast install bc --noconfirm', 'installing bc')
        check('downloaded executable and ownership metadata are intact',
              "printf '6*7\\n' | bc -q; pacman -Qkk bc", '0 altered files')
        check('clone an actual recipe Git repository from the service',
              'coast clone hello-coast', 'Recipe checkout')
        check('noninteractive confirmation cannot bypass recipe review',
              'coast build hello-coast --noconfirm < /dev/null; test $? -ne 0', 'review this checkout first')
        check('review PKGBUILD and source before invoking makepkg',
              'cd /home/user/Builds/hello-coast && cat PKGBUILD hello.c && makepkg --printsrcinfo', 'pkgbase = hello-coast')
        vm.command('true')
        check('manual AUR workflow uses upstream makepkg -si and authenticated pacman',
              'cd /home/user/Builds/hello-coast && makepkg -si --noconfirm', 'installing hello-coast', timeout=180)
        check('source-built executable runs with ordinary credentials', 'hello-coast', 'uid=1000')
        check('Git recipe update permits the signed fast-forward',
              'coast update hello-coast', 'Review the new Git diff')
        vm.command('true')
        check('source-built package removes through authenticated upstream pacman',
              'coast remove hello-coast --noconfirm', 'removing hello-coast')
        check('repository configuration remains protected from normal users',
              'test "$(id -u)" = 1000 && test ! -w /etc/coast/packages.json')
        vm.command('check', expect='USERLAND CHECK PASS', timeout=90)
        report['checks'].append('guest remains healthy after host-service transactions')
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


if __name__ == '__main__':
    main()
