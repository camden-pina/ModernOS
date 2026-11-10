#!/usr/bin/env python3
"""Verify a real signed package transaction in an owned offline desktop VM."""
import argparse
import base64
import io
import json
from pathlib import Path
import subprocess
import tarfile
import traceback

from source_tree import digest
import test_runtime as runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    iso = args.iso.resolve(strict=True)
    image = json.loads(iso.with_name('MANIFEST.json').read_text())
    component = image['components']['packages_build']
    if digest(iso) != image['iso_sha256'] or digest(Path(component['path'])) != component['sha256']:
        parser.error('Select the exact image and package-tools receipt')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    keyring = out / 'fixture-keyring'
    keyring.mkdir(mode=0o700)
    package = out / 'coast-source-qa-1.0-1-x86_64.pkg.tar.gz'
    payload = b'coast-signed-source-package-bytes\n'
    info = ('pkgname = coast-source-qa\npkgbase = coast-source-qa\npkgver = 1.0-1\n'
            'pkgdesc = Disposable source-build qualification fixture\n'
            'url = https://example.invalid/\nbuilddate = 1793923200\n'
            'packager = Coast qualification\nsize = ' + str(len(payload)) + '\narch = x86_64\nlicense = MIT\n')
    with tarfile.open(package, 'w:gz', format=tarfile.USTAR_FORMAT) as archive:
        for name, data in [('.PKGINFO', info.encode()), ('usr/share/coast/source-package-qa.txt', payload)]:
            member = tarfile.TarInfo(name)
            member.size, member.mode, member.mtime = len(data), 0o644, 1793923200
            archive.addfile(member, io.BytesIO(data))
    report = dict(state='failed', checks=[], iso_sha256=digest(iso), network=False,
        extra_disks=False, packages_build_sha256=component['sha256'],
        source_sha256={str(Path(__file__).relative_to(runtime.ROOT)): digest(Path(__file__))},
        signing_identity='new disposable qualification key; existing keys and services untouched')
    vm = None

    def host(*arguments, capture=False):
        return subprocess.run(['gpg', '--batch', '--homedir', str(keyring), *map(str, arguments)],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
            stderr=subprocess.PIPE, check=True).stdout

    def check(name, command, expected, user=False, timeout=180):
        output = vm.shell_command(command, expect=expected, as_user=user, timeout=timeout)
        (out / (f'{len(report["checks"]):02d}-output.txt')).write_text(output)
        report['checks'].append(name)
        print('PASS ' + name, flush=True)

    try:
        host('--pinentry-mode', 'loopback', '--passphrase', '', '--quick-generate-key',
            'Coast disposable package qualification', 'rsa2048', 'sign', '0')
        listing = host('--with-colons', '--list-keys', capture=True).decode()
        fingerprint, = [line.split(':')[9] for line in listing.splitlines() if line.startswith('fpr:')]
        public = out / 'fixture-public.asc'
        public.write_bytes(host('--armor', '--export', fingerprint, capture=True))
        host('--pinentry-mode', 'loopback', '--passphrase', '', '--detach-sign', package)
        signature = Path(str(package) + '.sig')
        report.update(fixture_signing_fingerprint=fingerprint,
            package_sha256=digest(package), signature_sha256=digest(signature))
        vm = runtime.VM(4, storage=False, network=False, memory='16G', accel='kvm',
            iso=iso, label='fresh-source-packages')
        vm.command('true')
        for name, source in [('qa-public.asc', public), (package.name, package), (signature.name, signature)]:
            target = '/tmp/' + name
            encoded = base64.b64encode(source.read_bytes()).decode()
            vm.shell_command(': > ' + target + '.b64')
            for offset in range(0, len(encoded), 200):
                vm.shell_command("printf '%s' '" + encoded[offset:offset + 200] + "' >> " + target + '.b64')
            check('transfer and verify ' + name,
                '/bin/busybox base64 -d ' + target + '.b64 > ' + target +
                ' && /bin/busybox sha256sum ' + target, digest(source))
        check('upstream pacman is runnable', '/usr/bin/pacman -V', 'Pacman v7.1.0')
        check('import only the disposable qualification public key',
            '/usr/bin/gpg --batch --homedir /etc/pacman.d/gnupg --import /tmp/qa-public.asc', 'imported')
        check('trust the disposable key for this RAM-only transaction',
            "printf '%s\\n' '" + fingerprint + ":6:' > /tmp/qa-ownertrust && "
            '/usr/bin/gpg --batch --homedir /etc/pacman.d/gnupg --import-ownertrust /tmp/qa-ownertrust',
            'ownertrust')
        check('GnuPG verifies the exact package signature',
            '/usr/bin/gpg --batch --homedir /etc/pacman.d/gnupg --verify /tmp/' + signature.name +
            ' /tmp/' + package.name, 'Good signature')
        check('pacman verifies the signature and installs the actual package',
            '/usr/bin/pacman -U --noconfirm /tmp/' + package.name, 'installing coast-source-qa')
        check('the package database records its installed version',
            '/usr/bin/pacman -Q coast-source-qa', 'coast-source-qa 1.0-1')
        check('installed payload bytes match the signed package',
            'cat /usr/share/coast/source-package-qa.txt', payload.decode().strip(), user=True)
        check('GNU Make runs a recipe inside Coast',
            "printf 'all:\\n\\t@echo SOURCE_NATIVE_MAKE_PASS\\n' > /tmp/qa-Makefile && "
            '/usr/bin/make -f /tmp/qa-Makefile', 'SOURCE_NATIVE_MAKE_PASS', user=True)
        check('pacman removes the package and its database record',
            '/usr/bin/pacman -R --noconfirm coast-source-qa', 'removing coast-source-qa')
        check('package removal deletes the installed file',
            'test ! -e /usr/share/coast/source-package-qa.txt && echo SOURCE_PACKAGE_REMOVED', 'SOURCE_PACKAGE_REMOVED')
        vm.command('check', expect='USERLAND CHECK PASS', timeout=120)
        report['checks'].append('kernel and userland remain healthy after signed package transactions')
        if digest(iso) != report['iso_sha256']:
            raise ValueError('The package-tested image changed')
        report.update(state='passed', image_unchanged=True)
    except BaseException as error:
        report['error'] = repr(error)
        (out / 'exception.txt').write_text(traceback.format_exc())
        raise
    finally:
        if vm:
            vm.close()
        subprocess.run(['gpgconf', '--homedir', str(keyring), '--kill', 'gpg-agent'],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        report['owned_vm_stopped'] = bool(vm and vm.process.poll() is not None)
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
