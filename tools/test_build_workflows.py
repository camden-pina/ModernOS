#!/usr/bin/env python3
"""Check host installation boundaries and cache cleanup protections."""
from contextlib import redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import clean_cache
import setup_host


class HostSetupTests(unittest.TestCase):
    def invoke(self, *args):
        with patch.object(sys, 'argv', ['setup_host.py', *args]), \
             patch.object(setup_host.platform, 'system', return_value='Linux'), \
             patch.object(setup_host.platform, 'machine', return_value='x86_64'), \
             redirect_stdout(io.StringIO()):
            setup_host.main()

    def test_print_only_never_runs_installation_or_downloads(self):
        with patch.object(setup_host.platform, 'freedesktop_os_release', return_value={'ID': 'ubuntu'}), \
             patch.object(setup_host.subprocess, 'run') as run, \
             patch.object(setup_host.urllib.request, 'urlopen') as download:
            self.invoke('--print')
        run.assert_not_called()
        download.assert_not_called()

    def test_check_only_checks_tools_without_installing(self):
        with patch.object(setup_host, 'check') as check, \
             patch.object(setup_host.subprocess, 'run') as run, \
             patch.object(setup_host.urllib.request, 'urlopen') as download:
            self.invoke('--check')
        check.assert_called_once_with()
        run.assert_not_called()
        download.assert_not_called()

    def test_unverified_download_cannot_be_executed(self):
        with tempfile.TemporaryDirectory() as directory:
            installer = Path(directory) / 'installer.py'
            installer.write_text('raise RuntimeError("unverified")')
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                setup_host.checked_installer(installer)

    def test_root_invocation_is_rejected_before_installation(self):
        with patch.object(setup_host.platform, 'freedesktop_os_release', return_value={'ID': 'ubuntu'}), \
             patch.object(setup_host.os, 'geteuid', return_value=0), \
             patch.object(setup_host.subprocess, 'run') as run, \
             self.assertRaises(SystemExit):
            self.invoke('--install')
        run.assert_not_called()


class CacheProtectionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / 'build').mkdir()
        (self.root / '.gitignore').write_text('/build/\n')
        subprocess.run(['git', 'init', '-q', self.root], check=True)

    def cache(self, name):
        path = self.root / 'build' / name
        path.mkdir(parents=True)
        (path / 'cached').write_text('regenerable')
        return path

    def test_only_regenerable_cache_is_selected(self):
        cache = self.cache('component/npm-cache')
        product = self.root / 'build/component/runtime'
        product.write_bytes(b'guest program')
        self.assertEqual(list(clean_cache.candidates(self.root)), [cache])
        self.assertTrue(product.is_file())

    def test_selected_service_directory_is_protected(self):
        self.cache('service/npm-cache')
        (self.root / 'build/Coast-repository-server').symlink_to('service')
        self.assertEqual(list(clean_cache.candidates(self.root)), [])

    def test_external_symlink_cannot_be_followed(self):
        with tempfile.TemporaryDirectory() as directory:
            outside = Path(directory) / 'npm-cache'
            outside.mkdir()
            (self.root / 'build/outside').symlink_to(directory)
            self.assertEqual(list(clean_cache.candidates(self.root)), [])
            self.assertTrue(outside.is_dir())

    def test_force_added_file_is_protected(self):
        cache = self.cache('component/npm-cache')
        subprocess.run(['git', '-C', self.root, 'add', '-f', cache / 'cached'], check=True)
        self.assertEqual(list(clean_cache.candidates(self.root)), [])

    def test_private_identity_inside_cache_is_protected(self):
        cache = self.cache('component/npm-cache')
        (cache / 'private-keys-v1.d').mkdir()
        self.assertEqual(list(clean_cache.candidates(self.root)), [])

    def test_exported_source_inventory_supports_cleanup_without_git(self):
        root = self.root / 'export'
        cache = root / 'build/component/npm-cache'
        cache.mkdir(parents=True)
        (cache / 'cached').write_text('regenerable')
        (root / 'hello.c').write_text('int main(void) { return 0; }\n')
        (root / 'SOURCE-MANIFEST.json').write_text(json.dumps(dict(files={'hello.c': {}})))
        self.assertEqual(list(clean_cache.candidates(root)), [cache])
        self.assertTrue((root / 'hello.c').is_file())

    def test_receipt_bound_files_are_preserved_when_pruning_objects(self):
        directory = self.root / 'build/electron-source-fixture'
        obj = self.cache('electron-source-fixture/electron/obj')
        products = {}
        for name in ('electron', 'node'):
            product = (obj if name == 'electron' else directory) / (name + '-binary')
            product.write_bytes(name.encode())
            products[name] = dict(path=str(product), sha256=hashlib.sha256(product.read_bytes()).hexdigest())
        (directory / 'BUILD.json').write_text(json.dumps(dict(products=products)))
        self.assertNotIn(obj, list(clean_cache.candidates(self.root, intermediates=True)))


if __name__ == '__main__':
    unittest.main()
