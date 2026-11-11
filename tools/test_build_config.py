#!/usr/bin/env python3
"""Check saved configuration, Make overrides and literal argument boundaries."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import build_config as config

ROOT = Path(__file__).resolve().parents[1]


class ConfigurationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.path = self.directory / 'workstation.config'

    def make(self, *arguments, environment=None):
        environment = {key: value for key, value in (environment or os.environ).items()
                       if key not in ('MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES')}
        return subprocess.run(['make', '--no-print-directory', 'CONFIG=' + str(self.path), *arguments],
                              cwd=ROOT, env=environment, text=True, capture_output=True, check=True)

    def test_save_reload_and_backup_preserve_settings(self):
        values = config.defaults()
        config.save(self.path, values)
        self.assertEqual(config.load(self.path), values)
        original = self.path.read_bytes()
        values.update(JOBS=2, MEMORY='24G', DIAGNOSTICS=True, QEMU_AUDIO='')
        config.save(self.path, values)
        self.assertEqual(config.load(self.path), values)
        self.assertEqual(Path(str(self.path) + '.old').read_bytes(), original)
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o600)

    def test_partial_configuration_fills_missing_defaults(self):
        self.path.write_text('format_version = 1\n[build]\nJOBS = 2\n')
        values = config.load(self.path)
        self.assertEqual(values['JOBS'], 2)
        self.assertEqual(values['MEMORY'], '16G')
        self.assertIsNone(values['ISO_BUILD'])

    def test_unknown_invalid_and_incompatible_options_are_rejected(self):
        for text in ('format_version = true\n', 'format_version = 2\n',
                     'format_version = 1\n[build]\nJOBSS = 3\n',
                     'format_version = 1\n[build]\nJOBS = 0\n',
                     'format_version = 1\n[vm]\nMEMORY = "lots"\n',
                     'format_version = 1\n[sources]\nRUNTIME_BUILD = "runtime.json"\n'):
            with self.subTest(text=text):
                self.path.write_text(text)
                with self.assertRaises(ValueError):
                    config.load(self.path)

    def test_failed_save_does_not_replace_existing_configuration(self):
        config.save(self.path, config.defaults())
        original = self.path.read_bytes()
        with self.assertRaises(ValueError):
            config.save(self.path, {**config.defaults(), 'JOBS': 1000})
        self.assertEqual(self.path.read_bytes(), original)
        with patch.object(config.os, 'replace', side_effect=OSError('fixture rename failed')):
            with self.assertRaises(OSError):
                config.atomic_write(self.path, 'not a valid replacement')
        self.assertEqual(self.path.read_bytes(), original)

    def test_make_loads_build_settings_and_all_integrations(self):
        values = config.defaults()
        values.update(JOBS=2, ISO_BUILD=str(self.directory / 'coast'),
                      QUALIFY=True,
                      ELECTRON_CHECKOUT=str(self.directory / 'checkout'),
                      RUNTIME_BUILD=str(self.directory / 'runtime.json'),
                      PACKAGE_CONFIG=str(self.directory / 'trust.json'))
        config.save(self.path, values)
        plan = json.loads(self.make('-s', 'iso', 'ISO_ARGS=--plan').stdout)
        stages = {stage['name']: stage for stage in plan['stages']}
        self.assertEqual(len(stages), 22)
        self.assertNotIn('--with-microsoft-auth', plan['assembly'])
        self.assertIn('--diagnostics', plan['assembly'])
        self.assertEqual(plan['qualification'][plan['qualification'].index('--iso') + 1],
                         str(self.directory / 'coast/image/Coast.iso'))
        self.assertIn('--repository-config', stages['frontend']['argv'])
        self.assertIsNone(plan['runtime_build'])
        self.assertEqual(plan['runtime_receipt'], str(self.directory / 'runtime.json'))
        argv = stages['qt']['argv']
        self.assertEqual(argv[argv.index('--jobs') + 1], '2')
        self.assertFalse((self.directory / 'coast').exists())

    def test_unsupported_microsoft_broker_fails_before_building(self):
        config.save(self.path, {**config.defaults(), 'WITH_MICROSOFT_AUTH': True})
        result = subprocess.run(['make', '--no-print-directory', 'CONFIG=' + str(self.path),
                                 '-s', 'iso', 'ISO_ARGS=--plan'], cwd=ROOT,
                                text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('WITH_MICROSOFT_AUTH=1 needs WebKitGTK', result.stderr)
        self.assertFalse((self.directory / 'coast').exists())

    def test_command_line_overrides_saved_build_and_vm_values(self):
        config.save(self.path, {**config.defaults(), 'JOBS': 2, 'QUALIFY': True,
                               'WITH_MICROSOFT_AUTH': True, 'MEMORY': '8G'})
        plan = json.loads(self.make('-s', 'iso', 'JOBS=3', 'QUALIFY=0',
                                    'WITH_MICROSOFT_AUTH=0', 'ISO_ARGS=--plan').stdout)
        self.assertIsNone(plan['qualification'])
        self.assertNotIn('--with-microsoft-auth', plan['assembly'])
        argv = plan['stages'][0]['argv']
        self.assertEqual(argv[argv.index('--jobs') + 1], '3')
        launch = self.make('-n', 'run', 'MEMORY=24G').stdout.replace('\\\n', ' ')
        argv = shlex.split(next(line for line in launch.splitlines() if line.startswith('env ')))
        self.assertEqual(argv[argv.index('-m') + 1], '24G')

    def test_saved_output_parent_changes_build_and_launch_together(self):
        config.save(self.path, {**config.defaults(), 'O': str(self.directory / 'output')})
        recipe = shlex.split(self.make('-n', 'iso').stdout)
        self.assertEqual(recipe[recipe.index('--output-root') + 1], str(self.directory / 'output/coast'))
        launch = self.make('-n', 'run').stdout.replace('\\\n', ' ')
        argv = shlex.split(next(line for line in launch.splitlines() if line.startswith('env ')))
        self.assertEqual(argv[argv.index('-cdrom') + 1], str(self.directory / 'output/coast/image/Coast.iso'))

    def test_reloading_modified_configuration_does_not_use_stale_values(self):
        config.save(self.path, {**config.defaults(), 'JOBS': 2})
        self.assertIn("'2'", self.make('-n', 'iso').stdout)
        config.save(self.path, {**config.defaults(), 'JOBS': 3})
        self.assertIn("'3'", self.make('-n', 'iso').stdout)

    def test_literal_boundary_values_survive_make_expansion(self):
        for value in ('endef', 'define EXAMPLE', 'trailing\\', 'literal # value'):
            with self.subTest(value=value):
                config.save(self.path, {**config.defaults(), 'QEMU_CPU': value})
                launch = self.make('-n', 'run').stdout.replace('\\\n', ' ')
                argv = shlex.split(next(line for line in launch.splitlines() if line.startswith('env ')))
                self.assertEqual(argv[argv.index('-cpu') + 1], value)

    def test_saved_values_are_literal_make_and_shell_arguments(self):
        # Execute only a recording stub; no QEMU VM, download or build is run.
        sentinel = self.directory / 'must-not-exist'
        payload = '$(shell touch ' + str(sentinel) + ')'
        iso = self.directory / 'Coast.iso'
        iso.write_bytes(b'fixture')
        record = self.directory / 'arguments.json'
        programs = self.directory / 'bin'
        programs.mkdir()
        stub = programs / 'qemu-system-x86_64'
        stub.write_text('#!' + sys.executable + '\nimport json,os,sys\n'
                        'from pathlib import Path\nPath(' + repr(str(record)) + ').write_text('
                        'json.dumps(dict(argv=sys.argv[1:],backend=os.environ.get("GDK_BACKEND"))))\n')
        stub.chmod(0o755)
        disk = str(self.directory / ("a 'quoted' # " + payload + '.img'))
        values = {**config.defaults(), 'ISO_IMAGE': str(iso), 'QEMU_ACCEL': 'tcg',
                  'QEMU_CPU': chr(96) + 'touch ' + str(sentinel) + chr(96),
                  'QEMU_GTK_BACKEND': payload, 'LINUX_DISK': disk,
                  'QEMU_EXTRA_ARGS': '-name ' + shlex.quote(payload)}
        config.save(self.path, values)
        environment = {**os.environ, 'PATH': str(programs) + os.pathsep + os.environ['PATH']}
        self.make('run', environment=environment)
        result = json.loads(record.read_text())
        self.assertEqual(result['backend'], payload)
        self.assertIn(payload, result['argv'])
        self.assertIn(values['QEMU_CPU'], result['argv'])
        self.assertIn('file=' + disk + ',format=raw,if=none,id=linuxdisk', result['argv'])
        self.assertFalse(sentinel.exists())

    def test_make_recovers_invalid_configuration_with_defconfig(self):
        for original in (b'broken configuration\n', b'\xffinvalid encoding'):
            with self.subTest(original=original):
                self.path.write_bytes(original)
                rejected = subprocess.run(['make', '-n', 'iso', 'CONFIG=' + str(self.path)],
                                          cwd=ROOT, text=True, capture_output=True)
                self.assertNotEqual(rejected.returncode, 0)
                self.make('defconfig')
                self.assertEqual(config.load(self.path), config.defaults())
                self.assertEqual(Path(str(self.path) + '.old').read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
