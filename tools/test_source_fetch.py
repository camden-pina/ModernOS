#!/usr/bin/env python3
"""Check source restoration, offline reuse and failed-download handling."""
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from fetch_sources import ROOT, fetch, main as fetch_main, read_manifest, source_urls
from fetch_refind import RELEASES, main as prepare_refind


class SourceFetchTest(unittest.TestCase):
    def setUp(self):
        environment = patch.dict('fetch_sources.os.environ', {'SOURCE_MIRRORS': ''})
        environment.start()
        self.addCleanup(environment.stop)
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.directory = Path(self.work.name)
        self.payload = b'pinned upstream archive fixture\x00\xff'
        self.entry = {'archive': 'fixture.tar.gz',
                      'url': 'https://example.invalid/fixture.tar.gz',
                      'sha256': hashlib.sha256(self.payload).hexdigest()}

    def test_offline_reuse_and_missing_source(self):
        archive = self.directory / self.entry['archive']
        with patch('fetch_sources.subprocess.run') as download:
            with self.assertRaises(FileNotFoundError):
                fetch(self.entry, self.directory, check_only=True)
            archive.write_bytes(self.payload)
            fetch(self.entry, self.directory, check_only=True)
            fetch(self.entry, self.directory)
            download.assert_not_called()

    def test_existing_bad_archive_is_preserved(self):
        archive = self.directory / self.entry['archive']
        archive.write_bytes(b'local corrupt archive')
        with patch('fetch_sources.subprocess.run') as download:
            with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
                fetch(self.entry, self.directory)
            download.assert_not_called()
        self.assertEqual(archive.read_bytes(), b'local corrupt archive')

    def test_download_is_installed_only_after_verification(self):
        archive = self.directory / self.entry['archive']

        def download(command, **kwargs):
            self.assertFalse(archive.exists())
            self.assertEqual(command[-1], self.entry['url'])
            Path(command[command.index('--output') + 1]).write_bytes(self.payload)

        with patch('fetch_sources.subprocess.run', side_effect=download):
            fetch(self.entry, self.directory)

        self.assertEqual(archive.read_bytes(), self.payload)
        self.assertEqual(list(self.directory.iterdir()), [archive])

    def test_bad_download_and_network_error_leave_no_archive(self):
        def bad_download(command, **kwargs):
            Path(command[command.index('--output') + 1]).write_bytes(b'wrong upstream data')

        with patch('fetch_sources.subprocess.run', side_effect=bad_download):
            with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
                fetch(self.entry, self.directory)

        self.assertEqual(list(self.directory.iterdir()), [])
        with patch('fetch_sources.subprocess.run', side_effect=subprocess.CalledProcessError(6, 'curl')):
            with self.assertRaises(subprocess.CalledProcessError):
                fetch(self.entry, self.directory)

        self.assertEqual(list(self.directory.iterdir()), [])

    def test_connection_failure_can_recover_over_ipv4(self):
        def download(command, **kwargs):
            if '--ipv4' not in command:
                raise subprocess.CalledProcessError(6, command)
            Path(command[command.index('--output') + 1]).write_bytes(self.payload)

        with patch('fetch_sources.subprocess.run', side_effect=download):
            fetch(self.entry, self.directory)
        self.assertEqual((self.directory / self.entry['archive']).read_bytes(), self.payload)

    def test_network_failures_can_recover_from_mirror(self):
        entry = dict(self.entry, mirrors=['https://mirror.invalid/fixture.tar.gz'])
        for status in (5, 6, 7, 18, 22, 28, 52, 56):
            attempted = []

            def download(command, **kwargs):
                attempted.append(command)
                partial = Path(command[command.index('--output') + 1])
                if command[-1] == entry['url']:
                    partial.write_bytes(b'incomplete primary download')
                    raise subprocess.CalledProcessError(status, command)
                partial.write_bytes(self.payload)

            with self.subTest(status=status), \
                    patch('fetch_sources.subprocess.run', side_effect=download):
                fetch(entry, self.directory)

            archive = self.directory / entry['archive']
            self.assertEqual(archive.read_bytes(), self.payload)
            self.assertEqual(list(self.directory.iterdir()), [archive])
            self.assertEqual(attempted[-1][-1], entry['mirrors'][0])
            primary_attempts = attempted[:-1]
            self.assertEqual(len(primary_attempts), 1)
            self.assertNotIn('--ipv4', primary_attempts[0])
            self.assertEqual(primary_attempts[0][primary_attempts[0].index('--retry') + 1], '0')
            archive.unlink()

    def test_all_mirrors_failing_leave_no_archive(self):
        entry = dict(self.entry, mirrors=['https://mirror.invalid/fixture.tar.gz'])

        def download(command, **kwargs):
            Path(command[command.index('--output') + 1]).write_bytes(b'partial')
            raise subprocess.CalledProcessError(28, command)

        with patch('fetch_sources.subprocess.run', side_effect=download) as transfer:
            with self.assertRaises(subprocess.CalledProcessError):
                fetch(entry, self.directory)
        self.assertEqual([call.args[0][-1] for call in transfer.call_args_list],
                         [entry['url'], entry['mirrors'][0], entry['mirrors'][0]])
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_custom_mirror_is_tried_before_upstream_and_package_mirrors(self):
        entry = dict(self.entry, mirrors=['https://backup.invalid/fixture.tar.gz'])
        attempted = []

        def download(command, **kwargs):
            attempted.append(command[-1])
            if command[-1] != entry['mirrors'][0]:
                raise subprocess.CalledProcessError(28, command)
            Path(command[command.index('--output') + 1]).write_bytes(self.payload)

        with patch('fetch_sources.subprocess.run', side_effect=download):
            fetch(entry, self.directory, mirror_bases=['https://cache.invalid/sources/'])
        self.assertEqual(attempted, ['https://cache.invalid/sources/fixture.tar.gz',
                                    entry['url'], entry['mirrors'][0]])
        self.assertEqual((self.directory / entry['archive']).read_bytes(), self.payload)

    def test_environment_mirrors_and_duplicate_urls(self):
        entry = dict(self.entry, mirrors=[self.entry['url'], 'https://backup.invalid/source'])
        with patch.dict('fetch_sources.os.environ',
                        {'SOURCE_MIRRORS': 'https://cache.invalid https://cache.invalid/'}):
            self.assertEqual(source_urls(entry), ['https://cache.invalid/fixture.tar.gz',
                                                 entry['url'], entry['mirrors'][1]])
            self.assertEqual(source_urls(entry, []), [entry['url'], entry['mirrors'][1]])

    def test_cli_mirrors_override_environment(self):
        attempted = []

        def download(command, **kwargs):
            attempted.append(command[-1])
            if command[-1].startswith('https://first.invalid/'):
                raise subprocess.CalledProcessError(28, command)
            Path(command[command.index('--output') + 1]).write_bytes(self.payload)

        arguments = ['fetch_sources.py', self.entry['archive'], '--destination', str(self.directory),
                     '--mirror', 'https://first.invalid', '--mirror', 'https://second.invalid']
        with patch.dict('fetch_sources.os.environ', {'SOURCE_MIRRORS': 'https://env.invalid'}), \
                patch('sys.argv', arguments), \
                patch('fetch_sources.read_manifest', return_value={self.entry['archive']: self.entry}), \
                patch('fetch_sources.subprocess.run', side_effect=download):
            fetch_main()
        self.assertEqual(attempted, ['https://first.invalid/fixture.tar.gz',
                                    'https://second.invalid/fixture.tar.gz'])
        self.assertEqual((self.directory / self.entry['archive']).read_bytes(), self.payload)

    def test_invalid_custom_mirror_is_rejected_before_downloading(self):
        for mirror in ('http://cache.invalid', 'https://user:password@cache.invalid',
                       'https://cache.invalid?query=1', 'https://cache.invalid/#fragment'):
            with self.subTest(mirror=mirror), patch('fetch_sources.subprocess.run') as transfer:
                with self.assertRaisesRegex(ValueError, 'Invalid source mirror base'):
                    fetch(self.entry, self.directory, mirror_bases=[mirror])
                transfer.assert_not_called()
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_make_exports_configured_mirrors(self):
        launcher = self.directory / 'python3'
        launcher.write_text('#!' + sys.executable + '\n'
                            'import os\nprint(os.environ["SOURCE_MIRRORS"])\n')
        launcher.chmod(0o755)
        env = dict(os.environ, PATH=str(self.directory) + os.pathsep + os.environ['PATH'])
        env.pop('MAKEFLAGS', None)
        mirror = 'https://cache.invalid/modernos'
        targets = ((ROOT, 'fetch-sources'), (ROOT / 'userland', 'sources'),
                   (ROOT / 'toolchain', str(self.directory / 'gcc-14.2.0.tar.gz')))
        for directory, target in targets:
            with self.subTest(directory=directory):
                result = subprocess.run(['make', '-s', target, 'SOURCE_MIRRORS=' + mirror,
                                         'SRC_DIR=' + str(self.directory)],
                                        cwd=directory, env=env, text=True,
                                        capture_output=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), mirror)

    def test_refind_uses_shared_mirror_and_verification(self):
        payload = io.BytesIO()
        with zipfile.ZipFile(payload, 'w') as archive:
            archive.writestr('refind-bin-0.14.2/refind/refind_x64.efi', b'pinned EFI fixture')
        contents = payload.getvalue()

        def download(command, **kwargs):
            self.assertEqual(command[-1], 'https://cache.invalid/refind-bin-0.14.2.zip')
            self.assertFalse((self.directory / 'refind-bin-0.14.2.zip').exists())
            Path(command[command.index('--output') + 1]).write_bytes(contents)

        with patch.dict('fetch_sources.os.environ', {'SOURCE_MIRRORS': 'https://cache.invalid'}), \
                patch.dict(RELEASES, {'0.14.2': hashlib.sha256(contents).hexdigest()}), \
                patch('sys.argv', ['fetch_refind.py', '--cache', str(self.directory)]), \
                patch('fetch_sources.subprocess.run', side_effect=download):
            self.assertEqual(prepare_refind(), 0)
        self.assertEqual((self.directory / 'refind-bin-0.14.2/refind/refind_x64.efi').read_bytes(),
                         b'pinned EFI fixture')
        self.assertEqual((self.directory / 'refind-bin-0.14.2.zip').read_bytes(), contents)
        self.assertFalse(list(self.directory.glob('.*.part')))

    def test_bad_mirror_download_is_not_installed(self):
        entry = dict(self.entry, mirrors=['https://mirror.invalid/fixture.tar.gz'])

        def download(command, **kwargs):
            if command[-1] == entry['url']:
                raise subprocess.CalledProcessError(28, command)
            Path(command[command.index('--output') + 1]).write_bytes(b'wrong mirror data')

        with patch('fetch_sources.subprocess.run', side_effect=download):
            with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
                fetch(entry, self.directory)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_tls_and_local_errors_do_not_try_mirrors(self):
        entry = dict(self.entry, mirrors=['https://mirror.invalid/fixture.tar.gz'])
        for status in (23, 60):
            with self.subTest(status=status), \
                    patch('fetch_sources.subprocess.run',
                          side_effect=subprocess.CalledProcessError(status, 'curl')) as download:
                with self.assertRaises(subprocess.CalledProcessError):
                    fetch(entry, self.directory)
                download.assert_called_once()
                self.assertEqual(list(self.directory.iterdir()), [])

    def test_manifest_rejects_unsafe_or_duplicate_records(self):
        manifest = self.directory / 'sources.json'
        invalid = [dict(self.entry, archive='../escape.tar.gz'),
                   dict(self.entry, url='http://example.invalid/source'),
                   dict(self.entry, sha256='not a checksum'),
                   dict(self.entry, mirrors='https://mirror.invalid/source'),
                   dict(self.entry, mirrors=[None]),
                   dict(self.entry, mirrors=['http://mirror.invalid/source']),
                   dict(self.entry, mirrors=['https://user:password@mirror.invalid/source'])]
        for entry in invalid:
            manifest.write_text(json.dumps([entry]))
            with self.subTest(entry=entry), self.assertRaises(ValueError):
                read_manifest(manifest)

        manifest.write_text(json.dumps([self.entry, self.entry]))
        with self.assertRaises(ValueError):
            read_manifest(manifest)

    def test_manifest_accepts_https_mirrors(self):
        manifest = self.directory / 'sources.json'
        entry = dict(self.entry, mirrors=['https://mirror.invalid/fixture.tar.gz'])
        manifest.write_text(json.dumps([entry]))
        self.assertEqual(read_manifest(manifest), {entry['archive']: entry})


if __name__ == '__main__':
    unittest.main()
