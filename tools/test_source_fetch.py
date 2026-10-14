#!/usr/bin/env python3
"""Check source restoration, offline reuse and failed-download handling."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from fetch_sources import fetch, read_manifest


class SourceFetchTest(unittest.TestCase):
    def setUp(self):
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

    def test_manifest_rejects_unsafe_or_duplicate_records(self):
        manifest = self.directory / 'sources.json'
        invalid = [dict(self.entry, archive='../escape.tar.gz'),
                   dict(self.entry, url='http://example.invalid/source'),
                   dict(self.entry, sha256='not a checksum')]
        for entry in invalid:
            manifest.write_text(json.dumps([entry]))
            with self.subTest(entry=entry), self.assertRaises(ValueError):
                read_manifest(manifest)

        manifest.write_text(json.dumps([self.entry, self.entry]))
        with self.assertRaises(ValueError):
            read_manifest(manifest)


if __name__ == '__main__':
    unittest.main()
