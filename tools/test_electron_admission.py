#!/usr/bin/env python3
"""Check source receipt and ELF boundaries before kernel runtime admission."""
import copy
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from electron_admission import prepare, ROOT


class AdmissionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.lock = self.root / 'lock.json'
        self.lock.write_text('{"pinned":true}\n')
        self.receipt = self.root / 'BUILD.json'
        self.header = self.root / 'output/admission.h'
        self.record = dict(state='source compiled; guest qualification pending',
            guest_execution_on_host=False, source_lock_sha256=self.digest(self.lock),
            args_sha256=self.digest(ROOT / 'ports/electron/args.gn'),
            source_policy_sha256={},
            features=dict(jitless_compiled=True, drumbrake_explicit_bounds_checks=True), products={})
        for name in ('electron', 'node'):
            binary = self.root / name
            # A small inert ELF layout is sufficient; the test never executes it.
            data = bytearray(128)
            data[:16] = b'\x7fELF\x02\x01\x01' + bytes(9)
            struct.pack_into('<HHIQQQIHHHHHH', data, 16, 2, 62, 1, 0x400000, 64, 0, 0, 64, 56, 1, 0, 0, 0)
            struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 0, 0x400000, 0x400000, len(data), len(data), 4096)
            binary.write_bytes(data)
            self.record['products'][name] = dict(path=str(binary), sha256=self.digest(binary))
        self.record.update(binary=str(self.root / 'electron'), binary_sha256=self.digest(self.root / 'electron'))
        self.arguments = self.root / 'ports/electron/args.gn'
        self.arguments.parent.mkdir(parents=True)
        self.arguments.write_bytes((ROOT / 'ports/electron/args.gn').read_bytes())

    @staticmethod
    def digest(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def generate(self):
        self.receipt.write_text(json.dumps(self.record))
        return prepare(self.receipt, self.header, self.lock)

    def test_exact_pair_and_deterministic_header(self):
        self.generate()
        first = self.header.read_bytes()
        first_time = self.header.stat().st_mtime_ns
        self.generate()
        self.assertEqual(first, self.header.read_bytes())
        self.assertEqual(first_time, self.header.stat().st_mtime_ns)
        evidence = json.loads(self.header.with_suffix('.json').read_text())
        self.assertEqual(evidence['canonical_paths']['node'], '/opt/electron-43.7.3/node')
        self.assertFalse(evidence['target_executed'])

    def test_runtime_mutation_is_rejected(self):
        with (self.root / 'node').open('ab') as stream:
            stream.write(b'changed')
        with self.assertRaises(ValueError):
            self.generate()

    def test_changed_source_graph_is_rejected(self):
        self.lock.write_text('{"pinned":false}\n')
        with self.assertRaises(ValueError):
            self.generate()

    def test_changed_coast_source_policy_is_rejected(self):
        policy = self.root / 'policy.cc'
        policy.write_text('pinned Coast policy\n')
        self.lock.write_text(json.dumps(dict(overlays={'src/policy.cc': 'policy.cc'})))
        self.record.update(source_lock_sha256=self.digest(self.lock), source_policy_sha256={'policy.cc': self.digest(policy)})
        with patch('electron_admission.ROOT', self.root):
            self.generate()
            policy.write_text('different policy\n')
            with self.assertRaises(ValueError):
                self.generate()

    def test_missing_policy_or_incomplete_build_is_rejected(self):
        original = copy.deepcopy(self.record)
        for field, value in [('features', {}), ('state', 'building'), ('guest_execution_on_host', True)]:
            self.record = copy.deepcopy(original)
            self.record[field] = value
            with self.assertRaises(ValueError):
                self.generate()

    def test_changed_build_arguments_are_rejected(self):
        with patch('electron_admission.ROOT', self.root):
            self.generate()
            self.arguments.write_text('different runtime arguments\n')
            with self.assertRaises(ValueError):
                self.generate()

    def test_bad_elf_layouts_are_rejected_even_with_matching_receipt_hash(self):
        binary = self.root / 'node'
        original = binary.read_bytes()
        for offset, format, value in [(18, '<H', 3), (54, '<H', 57), (68, '<I', 7),
                (104, '<Q', 1 << 40), (72, '<Q', 1 << 60), (96, '<Q', 129)]:
            data = bytearray(original)
            struct.pack_into(format, data, offset, value)
            binary.write_bytes(data)
            self.record['products']['node']['sha256'] = self.digest(binary)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                self.generate()

    def test_electron_product_alias_cannot_select_another_binary(self):
        self.record['binary_sha256'] = '0' * 64
        with self.assertRaises(ValueError):
            self.generate()


if __name__ == '__main__':
    unittest.main()
