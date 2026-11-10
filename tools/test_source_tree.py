#!/usr/bin/env python3
"""Exercise source publication boundaries and deterministic offline source kits."""
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

from source_tree import audit, export


class SourceTreeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / 'source'
        self.root.mkdir()
        subprocess.run(['git', 'init', '-q', self.root], check=True)
        (self.root / '.gitignore').write_text('/build/\n*.key\n*.o\n')
        (self.root / 'hello.c').write_text('int main(void) { return 0; }\n')

    def test_ignored_products_are_absent(self):
        (self.root / 'build').mkdir()
        (self.root / 'build/hello').write_bytes(b'\x7fELFfixture')
        self.assertEqual(set(audit(self.root)), {'.gitignore', 'hello.c'})

    def test_tracked_ignored_object_cannot_bypass_audit(self):
        path = self.root / 'hello.o'
        path.write_bytes(b'\x7fELFfixture')
        subprocess.run(['git', '-C', self.root, 'add', '-f', path], check=True)
        with self.assertRaisesRegex(ValueError, 'compiled output'):
            audit(self.root)

    def test_extensionless_binary_is_rejected(self):
        (self.root / 'program').write_bytes(b'\x7fELFfixture')
        with self.assertRaisesRegex(ValueError, 'compiled output'):
            audit(self.root)

    def test_source_archive_download_is_rejected(self):
        (self.root / 'upstream.tar.xz').write_bytes(b'fixture')
        with self.assertRaisesRegex(ValueError, 'download belongs'):
            audit(self.root)

    def test_private_key_content_is_rejected_without_echo(self):
        (self.root / 'misnamed.txt').write_bytes(b'-----BEGIN ' + b'PRIVATE KEY-----\nsecret')
        with self.assertRaisesRegex(ValueError, 'content withheld') as error:
            audit(self.root)
        self.assertNotIn('BEGIN', str(error.exception))

    def test_escaping_symlink_is_rejected(self):
        (self.root / 'link').symlink_to('/etc/passwd')
        with self.assertRaisesRegex(ValueError, 'regular files'):
            audit(self.root)

    def test_dangling_symlink_cannot_hide_from_git_audit(self):
        (self.root / 'missing').symlink_to('/does/not/exist')
        with self.assertRaisesRegex(ValueError, 'regular files'):
            audit(self.root)

    def test_repeated_export_has_identical_bytes_and_source_manifest(self):
        first = Path(self.temporary.name) / 'first.tar.xz'
        second = Path(self.temporary.name) / 'second.tar.xz'
        record = export(first, self.root)
        export(second, self.root)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        with tarfile.open(first) as archive:
            self.assertEqual(set(archive.getnames()), {'coast/.gitignore', 'coast/hello.c', 'coast/SOURCE-MANIFEST.json'})
            self.assertEqual(json.load(archive.extractfile('coast/SOURCE-MANIFEST.json')), record)
            self.assertTrue(all(m.uid == 0 and m.gid == 0 and m.mtime == record['epoch'] for m in archive))


if __name__ == '__main__':
    unittest.main()
