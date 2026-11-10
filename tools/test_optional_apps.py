#!/usr/bin/env python3
"""Assemble selected-app images from source-receipt fixtures, without compiling an OS."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import assemble_desktop as assembly
from build_state import write_json
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]


class SelectedPayloadTests(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.root = Path(work.name)
        self.native = self.root / 'native'
        self.kernel = self.root / 'kernel'
        self.native.mkdir()
        self.kernel.mkdir()
        self.base = self.native / 'initramfs.tar'
        payload = assembly.Payload()
        payload.add('bin/busybox', b'native core fixture')
        payload.add('bin/sh', kind=tarfile.SYMTYPE, link='busybox')
        payload.add('bin/code-launch', b'Code OSS launcher fixture')
        payload.write(self.base)
        (self.kernel / 'vmlinux').write_bytes(b'source-built kernel fixture')
        write_json(self.kernel / 'KERNEL.json', dict(source_sha256={'include/uapi/account.h': digest(ROOT / 'include/uapi/account.h')},
            kernel_sha256=digest(self.kernel / 'vmlinux'), runtime_build_sha256=None))
        write_json(self.native / 'BUILD.json', dict(inherited_iso=False, initramfs=str(self.base),
            initramfs_sha256=digest(self.base), kernel_sha256=digest(self.kernel / 'vmlinux')))
        libc = self.native / 'musl/lib/libc.a'
        libc.parent.mkdir(parents=True)
        libc.write_bytes(b'fixture static musl')
        self.compiler = self.receipt('compiler', dict(thread_model='posix', musl_libc_sha256=digest(libc),
            files={'usr/bin/gcc': self.asset('gcc', b'compiler fixture')}))
        self.assets = self.receipt('assets', dict(files={'usr/share/fonts/coast/font': self.asset('font', b'font'),
            'usr/share/coast-browser/dictionaries/en-US.bdic': self.asset('dictionary', b'dictionary')}))

    def asset(self, name, data):
        path = self.root / name
        path.write_bytes(data)
        return dict(path=str(path), sha256=digest(path))

    def receipt(self, name, record):
        path = self.root / name / 'BUILD.json'
        write_json(path, record)
        return path

    def assemble(self, apps, extra=()):
        output = self.root / 'image'
        argv = ['assemble_desktop.py', '--apps', apps, '--native-build', str(self.native),
            '--kernel-build', str(self.kernel), '--assets-build', str(self.assets), '--output-root', str(output), *map(str, extra)]
        def write_iso(command, **kw):
            Path(command[command.index('--output') + 1]).write_bytes(b'newly assembled ISO fixture')
        with patch.object(sys, 'argv', argv), patch.object(assembly.subprocess, 'run', side_effect=write_iso), \
                patch.object(assembly, 'build_cursors', return_value={}), contextlib.redirect_stdout(io.StringIO()):
            assembly.main()
        with tarfile.open(output / 'initramfs.tar') as archive:
            names = set(archive.getnames())
            defaults = archive.extractfile('usr/share/coast/desktop-defaults.ini').read()
        return json.loads((output / 'MANIFEST.json').read_text()), names, defaults

    def test_core_selection_has_no_omitted_app_files_or_package_claims(self):
        manifest, names, defaults = self.assemble('none')
        self.assertEqual(manifest['apps'], [])
        self.assertIsNone(manifest['package_database'])
        self.assertIn('bin/sh', names)
        self.assertIn('terminal_shell=0', defaults.decode())
        self.assertFalse(any(name.startswith(('opt/electron', 'usr/share/coast-browser', 'usr/share/codeoss-workbench')) for name in names))
        self.assertNotIn('bin/code-launch', names)

    def test_qt_build_dependency_compiler_is_not_installed(self):
        qt = self.receipt('qt', dict(target_host_executed=False, compiler_build_sha256=digest(self.compiler),
            files={'usr/bin/qt6-demo': self.asset('qt-demo', b'Qt application fixture')}))
        manifest, names, _ = self.assemble('qt', ['--qt-build', qt, '--compiler-build', self.compiler])
        self.assertEqual(manifest['apps'], ['qt'])
        self.assertIn('usr/bin/qt6-demo', names)
        self.assertNotIn('usr/bin/gcc', names)
        self.assertIsNone(manifest['runtime_build_sha256'])

    def test_python_does_not_install_its_build_time_package_backend(self):
        packages = self.receipt('packages', dict(native_build_sha256=digest(self.native / 'BUILD.json'),
            files={'usr/bin/pacman': self.asset('pacman', b'package tool fixture')}))
        program = self.receipt('program', dict(target_host_executed=False, compiler_build_sha256=digest(self.compiler),
            files={'usr/bin/python3': self.asset('python', b'Python interpreter fixture')}))
        manifest, names, _ = self.assemble('python', ['--compiler-build', self.compiler,
            '--packages-build', packages, '--program-build', program])
        self.assertIn('usr/bin/python3', names)
        self.assertNotIn('usr/bin/pacman', names)
        self.assertNotIn('usr/bin/gcc', names)
        self.assertIsNone(manifest['package_database'])

    def test_qemu_without_development_environment_excludes_sdk_dependency_claims(self):
        qemu = self.receipt('qemu', dict(target_host_executed=False, backend='TCI', compiler_build_sha256=digest(self.compiler),
            files={'usr/bin/qemu-system-x86_64': self.asset('qemu-bin', b'QEMU fixture'),
                   'usr/lib/coast/qemu-sdk/lib/library.a': self.asset('sdk', b'static SDK fixture')}))
        manifest, names, _ = self.assemble('qemu', ['--qemu-build', qemu, '--compiler-build', self.compiler])
        self.assertIn('usr/bin/qemu-system-x86_64', names)
        self.assertNotIn('usr/lib/coast/qemu-sdk/lib/library.a', names)
        self.assertNotIn('usr/bin/gcc', names)
        self.assertEqual(manifest['apps'], ['qemu'])

    def test_missing_receipts_fail_before_creating_an_image(self):
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            self.assemble('chromium')
        self.assertFalse((self.root / 'image').exists())

    def test_elf_validation_accepts_static_core_and_rejects_unavailable_dynamic_libraries(self):
        source = self.root / 'program.c'
        source.write_text('int main(void) { return 0; }\n')
        binary = self.root / 'program'
        subprocess.run(['cc', '-static', source, '-o', binary], check=True)
        payload = assembly.Payload()
        payload.add('bin/check', binary.read_bytes())
        self.assertIn('bin/check', assembly.validate_elf_payload(payload, {}, self.root))
        subprocess.run(['cc', source, '-o', binary], check=True)
        payload = assembly.Payload()
        payload.add('bin/check', binary.read_bytes())
        with self.assertRaisesRegex(ValueError, 'unavailable libraries/ABI'):
            assembly.validate_elf_payload(payload, {}, self.root)


class PackageClosureTests(unittest.TestCase):
    def records(self):
        def record(names):
            return dict(files={name: {} for name in names})
        return {
            'cli_build': record(['bin/bash']),
            'compiler_build': record(['usr/bin/gcc', 'usr/bin/as']),
            'packages_build': record(['usr/bin/' + name for name in ('pacman', 'gpg', 'bsdtar', 'curl', 'openssl', 'make', 'zstd')]),
            'assets_build': record(['usr/share/fonts/coast/font']),
            'program_build': record(['usr/bin/python3']),
            'makepkg_build': record(['usr/bin/' + name for name in ('ls', 'find', 'file', 'fakeroot', 'gawk', 'git', 'sed', 'diff', 'grep', 'ninja', 'pkgconf', 'm4', 'patch', 'xz')] + ['usr/share/coast/makepkg/environment']),
            'frontend_build': record(['usr/bin/brine']),
        }

    def payload(self, records):
        payload = assembly.Payload()
        for record in records.values():
            for name in record['files']:
                payload.add(name, b'installed source-built fixture')
        return payload

    def test_selected_package_frontend_has_closed_dependencies_and_no_phantom_apps(self):
        from desktop_package_database import create
        records = self.records()
        result = create(self.payload(records), records)
        self.assertIn('brine', result['packages'])
        self.assertIn('base-devel', result['packages'])
        self.assertTrue({'qemu', 'qemu-devel', 'coast-vm', 'novnc', 'qt6-demo'}.isdisjoint(result['packages']))
        provided = set(result['packages']) | {alias.split('=')[0] for package in result['packages'].values() for alias in package['provides']}
        self.assertTrue(all(set(package['depends']) <= provided for package in result['packages'].values()))

    def test_missing_runtime_dependency_is_reported_before_publication(self):
        from desktop_package_database import create
        records = self.records()
        del records['cli_build']
        with self.assertRaisesRegex(ValueError, 'dependency missing.*bash'):
            create(self.payload(records), records)


class DesktopAvailabilityTests(unittest.TestCase):
    def test_production_launcher_availability_tracks_installed_executables(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'availability.c'
            source.write_text('''#include "desktop.h"
static const char *installed;
int stat(const char *path, struct os_stat *result) {
  (void)result;
  return installed && !strcmp(path, installed) ? 0 : -1;
}
int main(void) {
  if (!app_available(APP_FILES) || !app_available(APP_TERMINAL)) return 1;
  if (app_available(APP_CHROMIUM) || app_available(APP_WORKBENCH) || app_available(APP_QT)) return 2;
  installed = "/bin/codeoss-wayland";
  if (!app_available(APP_WORKBENCH) || app_available(APP_CHROMIUM)) return 3;
  installed = "/usr/bin/qt6-demo";
  if (!app_available(APP_QT) || app_available(APP_WORKBENCH)) return 4;
  return 0;
}
''')
            binary = root / 'check'
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-fno-builtin',
                '-I' + str(ROOT / 'userland/desktop'), '-I' + str(ROOT / 'userland/include'), '-I' + str(ROOT / 'include'),
                source, '-o', binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
