#!/usr/bin/env python3
"""Check first-use repository trust and preservation of an existing identity."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def module(name):
    spec = importlib.util.spec_from_file_location('coast_' + name, ROOT / 'userland/packages' / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


class ConfigurationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='coast-configure-test-')
        cls.work = Path(cls.temporary.name)
        cls.keys = cls.work / 'signing'
        cls.keys.mkdir(mode=0o700)
        cls.gpg = ['gpg', '--no-options', '--batch', '--homedir', str(cls.keys)]
        subprocess.run([*cls.gpg, '--pinentry-mode', 'loopback', '--passphrase', '', '--quick-generate-key',
                        'Disposable Coast configuration fixture', 'rsa2048', 'sign', '0'], check=True, capture_output=True)
        listing = subprocess.check_output([*cls.gpg, '--with-colons', '--list-keys'], text=True, stderr=subprocess.DEVNULL)
        cls.fingerprint, = [line.split(':')[9] for line in listing.splitlines() if line.startswith('fpr:')]
        cls.public = cls.work / 'public.asc'
        cls.public.write_bytes(subprocess.check_output([*cls.gpg, '--armor', '--export', cls.fingerprint]))
        cls.secret = cls.work / 'secret.asc'
        cls.secret.write_bytes(subprocess.check_output([*cls.gpg, '--pinentry-mode', 'loopback', '--passphrase', '',
                                                       '--armor', '--export-secret-keys', cls.fingerprint]))
        cls.configure, cls.client = module('configure'), module('client')

    @classmethod
    def tearDownClass(cls):
        subprocess.run(['gpgconf', '--homedir', str(cls.keys), '--kill', 'gpg-agent'], check=True, capture_output=True)
        cls.temporary.cleanup()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=self.work)
        self.root = Path(self.directory.name)
        for name in ('etc/coast', 'etc/pacman.d/gnupg'):
            (self.root / name).mkdir(parents=True)
        (self.root / 'etc/coast/packages.json').write_text(json.dumps(dict(version=1, url=None, signing_fingerprint=None, community_url=None)))
        for name in ('pacman.conf', 'makepkg.conf'):
            (self.root / 'etc' / name).write_bytes((ROOT / 'userland/packages/config' / name).read_bytes())

    def tearDown(self):
        self.directory.cleanup()

    def initialize(self, key=None, fingerprint=None, url='https://packages.example.test'):
        self.configure.initialize(url, key or self.public, fingerprint or self.fingerprint,
            root=self.root, owner=os.getuid(), server_texts=self.client.server_config_texts)

    def test_matching_public_key_initializes_only_public_trust(self):
        self.initialize()
        config = json.loads((self.root / 'etc/coast/packages.json').read_text())
        self.assertEqual(config['signing_fingerprint'], self.fingerprint)
        self.assertEqual(config['url'], 'https://packages.example.test')
        self.assertIn('Server = https://packages.example.test/repo/$arch', (self.root / 'etc/pacman.conf').read_text())
        self.assertEqual(sorted(path.name for path in (self.root / 'etc/pacman.d/gnupg').iterdir()), ['pubring.gpg', 'trustdb.gpg'])
        self.assertFalse((self.root / 'etc/coast/.server-update.lock').exists())

    def test_mismatched_fingerprint_leaves_configuration_unchanged(self):
        before = (self.root / 'etc/coast/packages.json').read_bytes()
        with self.assertRaisesRegex(ValueError, 'does not match'):
            self.initialize(fingerprint='0' * 40)
        self.assertEqual(before, (self.root / 'etc/coast/packages.json').read_bytes())
        self.assertFalse(list((self.root / 'etc/pacman.d/gnupg').iterdir()))

    def test_private_key_is_not_accepted_as_a_public_identity(self):
        with self.assertRaises(ValueError):
            self.initialize(key=self.secret)

    def test_existing_identity_and_keyring_are_preserved(self):
        self.initialize()
        files = {str(path.relative_to(self.root)): path.read_bytes() for path in self.root.rglob('*') if path.is_file()}
        with self.assertRaisesRegex(ValueError, 'already configured'):
            self.initialize(url='https://replacement.example.test')
        self.assertEqual(files, {str(path.relative_to(self.root)): path.read_bytes() for path in self.root.rglob('*') if path.is_file()})

    def test_unsafe_configuration_symlink_is_rejected(self):
        config = self.root / 'etc/coast/packages.json'
        original = self.root / 'original'
        config.rename(original)
        config.symlink_to(original)
        with self.assertRaises(OSError):
            self.initialize()
        self.assertIsNone(json.loads(original.read_text())['url'])

    def test_plain_server_change_keeps_the_existing_fingerprint(self):
        self.initialize()
        self.client.apply_server('https://mirror.example.test', root=self.root, owner=os.getuid())
        current = json.loads((self.root / 'etc/coast/packages.json').read_text())
        self.assertEqual(current['signing_fingerprint'], self.fingerprint)
        self.assertEqual(current['url'], 'https://mirror.example.test')


if __name__ == '__main__':
    unittest.main()
