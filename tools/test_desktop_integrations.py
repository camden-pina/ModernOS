#!/usr/bin/env python3
"""Qualify Brine, real makepkg/fakeroot and QEMU in a fresh image."""
import argparse
import json
from pathlib import Path
import threading
import traceback

from desktop_repository_fixture import RepositoryFixture
from serve_repository import RepositoryServer
from source_tree import digest
import test_runtime as runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    iso = args.iso.resolve(strict=True)
    image = json.loads(iso.with_name('MANIFEST.json').read_text())
    if not image.get('wider_integrations') or digest(iso) != image['iso_sha256']:
        parser.error('Select an exact fresh image with the wider integrations')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    report = dict(state='failed', checks=[], iso_sha256=digest(iso),
        guest_program_execution_on_host=False, extra_disks=False, listeners='127.0.0.1 only',
        source_sha256={str(p.relative_to(runtime.ROOT)): digest(p) for p in
            [Path(__file__), runtime.ROOT / 'tools/desktop_repository_fixture.py']})
    fixture, server, thread, vm = None, None, None, None

    def check(name, command, expected, user=True, timeout=180):
        output = vm.shell_command(command, expect=expected, as_user=user, timeout=timeout)
        (out / (f'{len(report["checks"]):02d}-output.txt')).write_text(output)
        report['checks'].append(name)
        print('PASS ' + name, flush=True)
        return output

    try:
        fixture = RepositoryFixture(out / 'fixture')
        manifest = fixture.build()
        server = RepositoryServer(('127.0.0.1', 0), manifest)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        origin = 'http://10.0.2.2:' + str(server.server_port)
        vm = runtime.VM(4, storage=False, network=True, network_backend='passt', memory='16G', accel='kvm', iso=iso, label='fresh-wide-integrations')
        vm.command('true')
        (out / 'network-startup.txt').write_text(vm.command('netinfo'))
        check('fresh image has no inherited signing identity', 'brine repo', 'not configured')
        check('Coast lists the actual installed source components', 'brine list', 'brine 1.6-1')
        check('compiler, Qt and emulator have real package ownership',
              'pacman -Qo /usr/bin/gcc /usr/bin/qt6-demo /usr/bin/qemu-system-x86_64', 'qemu 10.2.4-1')
        check('initial live database records source assembly without claiming PGP installation',
              'pacman -Qi gcc | grep "Validated By"', 'None')
        check('source tool dependencies resolve through libalpm',
              'pacman -T gcc make musl bash coreutils findutils file fakeroot git gawk grep sed diffutils m4 patch pkgconf && echo DEPENDENCIES_READY', 'DEPENDENCIES_READY')
        check('all required Python native modules execute inside Coast',
              "python3 -I -c 'import ssl,sqlite3,zlib,hashlib,subprocess,json; d=sqlite3.connect(\":memory:\"); assert d.execute(\"select 6*7\").fetchone()[0]==42; print(\"PYTHON_WIDE_PASS\", ssl.OPENSSL_VERSION)'", 'PYTHON_WIDE_PASS')
        check('diffutils cross-configuration assumption is checked on the guest libc',
              '/usr/share/coast/makepkg/strcasecmp-check', 'CONFIGURE_STRCASECMP_PASS')
        check('upstream makepkg refuses a privileged build', 'makepkg; test $? -ne 0', 'root', user=False)
        check('fetch only the disposable fixture public key',
              'curl -q --noproxy "*" --retry 20 --retry-all-errors --retry-delay 1 --connect-timeout 3 --max-time 45 -fsS ' + origin + '/keys/fixture.asc -o /home/user/fixture.asc && sha256sum /home/user/fixture.asc', digest(fixture.public / 'public.asc'))
        check('first-use trust rejects a mismatched fingerprint',
              'brine configure ' + origin + ' --key /home/user/fixture.asc --fingerprint ' + '0' * 40 + '; test $? -ne 0', 'does not match')
        vm.command('true')
        check('initialize only the explicitly matched disposable repository identity',
              'brine configure ' + origin + ' --key /home/user/fixture.asc --fingerprint ' + fixture.fingerprint, 'Configured Brine')
        check('first-use configuration preserves its signing identity on repeat',
              'brine configure ' + origin + ' --key /home/user/fixture.asc --fingerprint ' + fixture.fingerprint + '; test $? -ne 0', 'already configured')
        check('configured server verifies its catalog and binary database signatures', 'brine server --check', 'Verified package database and source catalog')
        check('refresh the signed source catalog', 'brine refresh', '1 recipes')
        check('libalpm verifies the signed binary and command databases', 'pacman -Syy --noconfirm && pacman -Fyy --noconfirm', 'Synchronizing', user=False)
        check('frontend source search uses the verified catalog', 'brine search --source hello', 'hello-coast')
        check('frontend binary search uses signed libalpm metadata', 'brine search --repo wider', 'coast-wide-qa')
        check('clone the actual pinned Git recipe history', 'brine clone hello-coast', 'Recipe checkout')
        check('native makepkg checks source, compiles, tests and packages as the normal user',
              'brine build hello-coast --reviewed --noconfirm', 'Finished making', timeout=420)
        check('fakeroot records root metadata while the builder keeps UID 1000',
              "zstd -dc /home/user/Builds/hello-coast/*.pkg.tar.zst > /home/user/hello.tar; python3 -I -c 'import os,tarfile; assert os.getuid()==1000; a=tarfile.open(\"/home/user/hello.tar\"); assert all(m.uid==0 and m.gid==0 for m in a.getmembers()); print(\"FAKEROOT_ARCHIVE_PASS\")'", 'FAKEROOT_ARCHIVE_PASS')
        check('fakeroot does not grant kernel write permission to system paths',
              "fakeroot /usr/bin/bash -c 'test $(id -u) = 0 || exit 97; echo forbidden > /etc/fakeroot-wide-escape; test $? -ne 0'", 'Permission denied')
        vm.command('true')
        check('install the reviewed native package through authenticated sudo',
              'brine install --source hello-coast --reviewed --noconfirm', 'installing hello-coast', timeout=420)
        check('the locally compiled package runs without elevation', 'hello-coast', 'uid=1000')
        check('install a real signed binary fixture through the frontend',
              'brine install --repo coast-wide-qa --noconfirm', 'installing coast-wide-qa')
        check('signed package bytes and database integrity match',
              'cat /usr/share/coast/wider-integration-qa.txt; pacman -Qkk coast-wide-qa', '0 altered files')
        check('frontend removes the signed package through authenticated sudo',
              'brine remove coast-wide-qa --noconfirm', 'removing coast-wide-qa')
        check('QEMU disk workflows, QMP, VNC, BIOS CPU execution and native SDK consumers',
              'python3 /usr/share/coast/qemu/check.py --sdk', 'QEMU_ALL_PASS', timeout=420)
        check('Qt and QEMU installed file metadata matches source-built payloads',
              'pacman -Qkk qt6-demo qemu qemu-devel', '0 altered files', timeout=180)
        vm.command('check', expect='USERLAND CHECK PASS', timeout=120)
        report['checks'].append('Coast remains healthy after wider integration workflows')
        report['state'] = 'passed'
    except BaseException as error:
        report['error'] = repr(error)
        (out / 'exception.txt').write_text(traceback.format_exc())
        if vm:
            try:
                diagnostic = vm.shell_command('cd /home/user/Builds/hello-coast/src; ./hello-coast; echo RUN_RC=$?; id -u; ./hello-coast | grep -q "Hello from a locally built Coast package"; echo GREP_RC=$?', as_user=True)
                (out / 'recipe-diagnostic.txt').write_text(diagnostic)
            except Exception as probe_error:
                report['diagnostic_error'] = repr(probe_error)
        raise
    finally:
        if vm:
            vm.close()
        if server:
            server.shutdown()
            server.server_close()
        if thread:
            thread.join(5)
        if fixture:
            fixture.close()
        report['owned_vm_stopped'] = bool(vm and vm.process.poll() is not None)
        report['image_unchanged'] = digest(iso) == report['iso_sha256']
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
