#!/usr/bin/env python3
"""Exercise pinned npm archive extraction and interrupted host-tool recovery."""
import base64
import hashlib
import io
import json
from pathlib import Path
import shutil
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import prepare_electron_host as host


class HostModuleTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.output = self.root / 'host-tools'
        self.downloads = self.output / 'downloads'
        self.downloads.mkdir(parents=True)
        self.archive = self.downloads / 'host-tool-1.0.tgz'
        with tarfile.open(self.archive, 'w:gz') as bundle:
            for name, data in {
                'package/package.json': b'{"name":"@example/host-tool","version":"1.0"}',
                'package/bin/tool': b'pinned host tool',
            }.items():
                member = tarfile.TarInfo(name)
                member.size = len(data)
                bundle.addfile(member, io.BytesIO(data))
        content = self.archive.read_bytes()
        self.digest = hashlib.sha256(content).hexdigest()
        manifest = self.root / 'host-tools.json'
        manifest.write_text(json.dumps([dict(name='@example/host-tool', version='1.0',
            archive=self.archive.name, url='https://example.invalid/host-tool-1.0.tgz',
            sha256=self.digest,
            integrity='sha512-' + base64.b64encode(hashlib.sha512(content).digest()).decode())]))
        self.patches = [patch.object(host, 'ROOT', self.root), patch.object(host, 'MANIFEST', manifest)]
        for setting in self.patches:
            setting.start()
            self.addCleanup(setting.stop)
        self.scratch = self.output / ('unpack-' + self.archive.name)
        self.installed = self.output / 'node_modules/@example/host-tool'

    def test_tgz_extracts_after_an_abandoned_scratch_directory(self):
        (self.scratch / 'package').mkdir(parents=True)
        (self.scratch / 'package/partial').write_text('old partial extraction')
        self.assertEqual(host.prepare(self.output), self.output / 'node_modules')
        self.assertEqual((self.installed / 'bin/tool').read_bytes(), b'pinned host tool')
        self.assertEqual((self.installed / '.coast-source').read_text(), self.digest)
        self.assertFalse(self.scratch.exists())
        with patch.object(host, 'unpack', side_effect=AssertionError('unnecessary extraction')):
            host.prepare(self.output)

    def test_kill_after_publication_reuses_the_stamped_module(self):
        remove = shutil.rmtree

        def interrupted(path, *args, **kwargs):
            if Path(path) == self.scratch:
                raise KeyboardInterrupt
            return remove(path, *args, **kwargs)

        with patch.object(host.shutil, 'rmtree', side_effect=interrupted):
            with self.assertRaises(KeyboardInterrupt):
                host.prepare(self.output)
        self.assertEqual((self.installed / '.coast-source').read_text(), self.digest)
        self.assertTrue(self.scratch.exists())
        with patch.object(host, 'unpack', side_effect=AssertionError('unnecessary extraction')):
            host.prepare(self.output)
        self.assertFalse(self.scratch.exists())


if __name__ == '__main__':
    unittest.main()
