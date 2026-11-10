#!/usr/bin/env python3
"""Check fixture links remain inside the staged target SDK."""
import hashlib
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

from test_llvm import fixture


class FixtureLinks(unittest.TestCase):
    def build(self, directory, aliases):
        source = directory / 'library'
        source.write_bytes(b'source-built library fixture\n')
        receipt = directory / 'BUILD.json'
        receipt.write_text(json.dumps(dict(target_host_executed=False, source_sha256={},
            files={'usr/lib/library': dict(path=str(source), sha256=hashlib.sha256(source.read_bytes()).hexdigest(), mode=0o644)},
            aliases=aliases)))
        return receipt

    def test_relative_sdk_link_resolves_after_extraction(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / 'sdk.tar.gz'
            receipt = self.build(root, {'usr/lib/alias': './library', 'usr/bin/ld.so': '../lib/alias'})
            fixture(receipt, archive)
            with tarfile.open(archive) as bundle:
                bundle.extractall(root / 'sdk', filter='data')
            self.assertEqual((root / 'sdk/usr/bin/ld.so').read_bytes(), (root / 'library').read_bytes())

    def test_unsafe_or_dangling_links_are_rejected(self):
        for aliases in [{'usr/lib/alias': '../../../outside'}, {'usr/lib/alias': '/usr/lib/library'},
                        {'usr/lib/alias': 'missing'}, {'usr/lib/a': 'b', 'usr/lib/b': 'a'}]:
            with self.subTest(aliases=aliases), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                with self.assertRaises(ValueError):
                    fixture(self.build(root, aliases), root / 'sdk.tar.gz')


if __name__ == '__main__':
    unittest.main()
