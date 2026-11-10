#!/usr/bin/env python3
"""Check fresh-image archive boundaries and the optional sign-in payload policy."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest

from assemble_desktop import Payload, application_payload

ROOT = Path(__file__).resolve().parents[1]


class ArchiveTests(unittest.TestCase):
    def test_deterministic_archive_and_relative_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            payload = Payload()
            payload.add('usr/lib/library', b'compiled from a pinned source fixture')
            payload.add('usr/lib/alias', mode=0o777, kind=tarfile.SYMTYPE, link='library')
            payload.write(root / 'first.tar')
            payload.write(root / 'second.tar')
            self.assertEqual((root / 'first.tar').read_bytes(), (root / 'second.tar').read_bytes())
            with tarfile.open(root / 'first.tar') as archive:
                self.assertEqual(archive.extractfile('usr/lib/alias').read(), b'compiled from a pinned source fixture')
                self.assertTrue(all(m.uid == 0 and m.gid == 0 for m in archive))

    def test_unsafe_archive_names_and_links_are_rejected(self):
        for name in ['/outside', '../outside', 'usr/../../outside']:
            with self.subTest(name=name), self.assertRaises(ValueError):
                Payload().add(name)
        for target in ['/outside', '../../../outside']:
            with self.subTest(target=target), self.assertRaises(ValueError):
                Payload().add('usr/lib/alias', kind=tarfile.SYMTYPE, link=target)

    def test_dangling_and_cyclic_links_are_rejected(self):
        for links in [{'a': 'missing'}, {'a': 'b', 'b': 'a'}]:
            with self.subTest(links=links), tempfile.TemporaryDirectory() as temporary:
                payload = Payload()
                for name, target in links.items():
                    payload.add(name, kind=tarfile.SYMTYPE, link=target)
                with self.assertRaises(ValueError):
                    payload.write(Path(temporary) / 'archive.tar')

    def test_file_directory_collisions_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            payload = Payload()
            payload.add('usr/lib', b'not a directory')
            payload.add('usr/lib/library', b'data')
            with self.assertRaises(ValueError):
                payload.write(Path(temporary) / 'archive.tar')

    def test_changed_build_outputs_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'artifact'
            path.write_bytes(b'changed output')
            with self.assertRaises(ValueError):
                Payload().file('bin/program', dict(path=str(path), sha256=hashlib.sha256(b'original output').hexdigest()))

    def test_microsoft_broker_is_omitted_unless_explicitly_selected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            files = {}
            for index, (name, data) in enumerate([
                ('product.json', json.dumps(dict(checksums={})).encode()),
                ('extensions/microsoft-authentication/dist/libmsalruntime.so', b'\x7fELFoptional fixture'),
                ('extensions/microsoft-authentication/dist/msal-node-runtime.node', b'\x7fELFoptional addon fixture')]):
                path = root / str(index)
                path.write_bytes(data)
                files[name] = dict(path=str(path), sha256=hashlib.sha256(data).hexdigest())
            for include in [False, True]:
                payload = Payload()
                prebuilt, excluded = application_payload(payload, dict(files=files), dict(files={}), microsoft_auth=include)
                self.assertEqual(len(prebuilt), 2 if include else 0)
                self.assertEqual(len(excluded), 0 if include else 2)


class PipelineTests(unittest.TestCase):
    def test_public_plan_fetches_sources_and_assembles_without_an_input_iso(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'not-created'
            result = subprocess.run([sys.executable, '-B', ROOT / 'tools/build_desktop.py',
                '--plan', '--output-root', output], capture_output=True, text=True, check=True)
            plan = json.loads(result.stdout)
            self.assertFalse(output.exists())
            self.assertFalse(plan['inherited_iso'])
            self.assertIn('--sync', plan['source_sync'])
            self.assertIn('--build', plan['runtime_build'])
            stages = {stage['name']: stage for stage in plan['stages']}
            self.assertIn('--source-build', stages['gui-runtime']['argv'])
            self.assertIn('--prepare', stages['application']['argv'])
            self.assertIn('--build', stages['application']['argv'])
            self.assertIn('--package-app', stages['application']['argv'])
            self.assertIn('--cc', stages['bridge']['argv'])
            self.assertIn('--gnu-sdk', stages['bridge']['argv'])
            self.assertIn('--cli-build', plan['assembly'])
            self.assertIn('--compiler-build', plan['assembly'])
            self.assertIn('--packages-build', plan['assembly'])
            self.assertNotIn('--allow-bootstrap-gui', plan['assembly'])
            self.assertNotIn('--with-microsoft-auth', plan['assembly'])
            self.assertFalse(any(arg.endswith('.iso') for stage in plan['stages'] for arg in stage['argv']))

    def test_reused_components_reject_a_changed_nested_runtime_artifact(self):
        from build_desktop import verify_receipt
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary = root / 'library'
            binary.write_bytes(b'original source-built library')
            receipt = root / 'BUILD.json'
            receipt.write_text(json.dumps(dict(packages={'library': dict(path=str(binary),
                sha256=hashlib.sha256(binary.read_bytes()).hexdigest())},
                host_tools={'executables': {'gn': dict(path='buildtools/linux64/gn',
                    sha256='0' * 64)}})))
            verify_receipt(receipt)
            binary.write_bytes(b'changed library')
            with self.assertRaisesRegex(ValueError, 'Build output changed'):
                verify_receipt(receipt)

    def test_wider_plan_binds_each_integration_to_explicit_source_components(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'not-created'
            result = subprocess.run([sys.executable, '-B', ROOT / 'tools/build_desktop.py',
                '--plan', '--with-integrations', '--qualify', '--output-root', output],
                capture_output=True, text=True, check=True)
            plan = json.loads(result.stdout)
            stages = {stage['name']: stage for stage in plan['stages']}
            self.assertFalse(output.exists())
            for name in ('program', 'makepkg', 'frontend', 'qt', 'qemu'):
                self.assertIn('--compiler-build', stages[name]['argv'])
            for name in ('makepkg', 'frontend', 'viewer'):
                self.assertIn('--program-build', stages[name]['argv'])
            self.assertIn('--packages-build', stages['makepkg']['argv'])
            self.assertIn('--qemu-build', stages['viewer']['argv'])
            for name in ('program', 'makepkg', 'frontend', 'qt', 'qemu', 'viewer'):
                self.assertIn('--' + name + '-build', plan['assembly'])
            self.assertIn('qualify_desktop.py', ' '.join(plan['qualification']))


if __name__ == '__main__':
    unittest.main()
