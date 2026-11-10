#!/usr/bin/env python3
"""Lightweight regression tests of the actual portable power guest harness.

All inputs are tiny temporary fixtures. ISO tooling is mocked; no guest,
compiler, firmware evaluator, block tool or existing proof report is touched.
"""
import importlib.util
import io
from types import SimpleNamespace
from pathlib import Path
import struct
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
spec = importlib.util.spec_from_file_location('power_harness_under_test', ROOT / 'tools/test_power.py')
H = importlib.util.module_from_spec(spec)
spec.loader.exec_module(H)
CONFIG = '''/Coast
    protocol: limine
    cmdline: quiet acpi.selftest power.test=old
    path: boot():/boot/kernel.elf
    module_path: boot():/boot/initramfs.tar
/Reboot
    protocol: reboot
'''


def elf_bytes():
    data = bytearray(64)
    data[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HH', data, 16, 2, 62)
    return bytes(data)


def telemetry_snapshot():
    values = {
        'TAC0': 'online=yes _PSR=AE_OK',
        'TBT0': 'source=_BIX _BIX=AE_OK information=AE_OK _BST=AE_OK model=Telemetry mWh\n  unit=mWh/mW design=6000 full=5000 remaining=3000 rate=1000 voltage_mV=12000 percent=60 state=discharging critical=no charge_limiting=no',
        'TBT1': 'source=_BIF _BIX=AE_NOT_FOUND information=AE_OK _BST=AE_OK model=Telemetry mAh\n  unit=mAh/mA design=6000 full=5000 remaining=4000 rate=unknown voltage_mV=unknown percent=80 state=charging critical=no charge_limiting=yes',
        'TBT2': 'source=unavailable _BIX=AE_NOT_EXIST information=AE_NOT_EXIST _BST=AE_NOT_EXIST\n  unit=unknown design=unknown full=unknown remaining=unknown rate=unknown voltage_mV=unknown percent=unknown state=unknown critical=unknown charge_limiting=unknown',
        'TBT3': 'source=_BIX _BIX=AE_OK information=AE_OK _BST=AE_OK model=Telemetry unknown\n  unit=mWh/mW design=unknown full=unknown remaining=unknown rate=unknown voltage_mV=unknown percent=unknown state=discharging critical=no charge_limiting=no',
    }
    lines = ['telemetry=running discovery=complete']
    for index, (name, data) in enumerate(values.items()):
        kind = 'AC' if name == 'TAC0' else 'Battery'
        present = 'no' if name == 'TBT2' else 'yes'
        notify = 'notifications=2 coalesced=1 last_notify=81' if name == 'TBT0' else 'notifications=0 coalesced=0 last_notify=0'
        lines += [f'{kind}[{index + 7}] \\_SB_.{name} present={present} stale=0 refresh_pending=0 attempted=1 last_refresh_us=100 last_success_us=100 _STA=AE_OK path_status=AE_OK notify_status=AE_OK {notify}', '  ' + data]
    return '\n'.join(lines) + '\n'


class PowerHarnessTests(unittest.TestCase):
    def setUp(self):
        (ROOT / 'build').mkdir(exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix='power-harness-static-', dir=ROOT / 'build')
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.base = self.directory / 'packaged-output'
        template = self.base / 'iso_root/boot/limine'
        template.mkdir(parents=True)
        (template / 'limine.conf').write_text(CONFIG)
        (template / 'limine-uefi-cd.bin').write_bytes(b'mocked boot template')
        (self.base / 'vmlinux').write_bytes(elf_bytes())
        self.paths = {}
        for name in ('shell', 'workload', 'thread-check', 'ipc-check', 'vfork-workload'):
            self.paths[name] = self.directory / (name + '.elf')
            self.paths[name].write_bytes(elf_bytes())
        self.write_tar(['bin/check', 'bin/coast-sh', 'bin/power-workload', 'bin/thread-check', 'bin/ipc-check'])
        aml = bytearray(36)
        aml[:4] = b'SSDT'
        struct.pack_into('<I', aml, 4, len(aml))
        aml[9] = (-sum(aml)) & 255
        self.aml = self.directory / 'telemetry.aml'
        self.aml.write_bytes(aml)
        local = self.directory / 'local-checkout'
        firmware = local / 'firmware/ovmf/OVMF.fd'
        firmware.parent.mkdir(parents=True)
        firmware.write_bytes(b'mocked firmware input')
        self.work = self.directory / 'generated'
        self.work.mkdir()
        for name, value in (('ROOT', local), ('WORK', self.work)):
            replacement = patch.object(H, name, value)
            replacement.start()
            self.addCleanup(replacement.stop)

    def write_tar(self, names, special=False):
        with tarfile.open(self.base / 'initramfs.tar', 'w') as archive:
            for index, name in enumerate(names):
                item = tarfile.TarInfo(name)
                if special and index == len(names) - 1:
                    item.type = tarfile.FIFOTYPE
                    archive.addfile(item)
                else:
                    data = elf_bytes() if name == 'bin/check' else b'old fixture'
                    item.size = len(data)
                    archive.addfile(item, io.BytesIO(data))

    def args(self, **changes):
        argv = ['--base', str(self.base), '--kind', 'dry-run', '--memory', '512M',
                '--telemetry-aml', str(self.aml)]
        for name, path in self.paths.items():
            argv += ['--' + name, str(path)]
        args = H.arguments(argv)
        for name, value in changes.items():
            setattr(args, name, value)
        return args

    def test_cmdline_preserves_tokens_and_replaces_test_mode(self):
        result = H.boot_config(CONFIG, 'flush-fail')
        self.assertEqual(result.count('cmdline:'), 1)
        self.assertIn('quiet acpi.selftest power.test=flush-fail', result)
        self.assertNotIn('power.test=', H.boot_config(CONFIG, ''))
        with self.assertRaises(ValueError):
            H.boot_config(CONFIG.replace('    path:', '    cmdline: duplicate\n    path:'), 'dry-run')
        with self.assertRaises(ValueError):
            H.boot_config(CONFIG.replace('/boot/kernel.elf', '/other/kernel'), '')

    def test_invalid_elf(self):
        for data in (b'not ELF', elf_bytes().replace(b'\x7fELF', b'BAD!'), elf_bytes()[:18] + b'\x03\x00' + elf_bytes()[20:]):
            self.paths['shell'].write_bytes(data)
            with self.assertRaises(ValueError):
                H.file_input(self.paths['shell'], 'shell', elf=True)

    def test_invalid_tar_names_duplicates_types_and_missing_check(self):
        for names, special in ((['bin/check', '../outside'], False),
                               (['bin/check', '/absolute'], False),
                               (['bin/check', './bin/check'], False),
                               (['bin/check', 'device'], True), (['bin/other'], False)):
            with self.subTest(names=names):
                self.write_tar(names, special)
                with self.assertRaises(ValueError):
                    H.validate_tar(self.base / 'initramfs.tar')

    def test_invalid_ssdt(self):
        valid = self.aml.read_bytes()
        for data in (b'SSDT', valid[:4] + b'\xff\xff\x00\x00' + valid[8:], valid[:-1] + b'\x01'):
            self.aml.write_bytes(data)
            with self.assertRaises(ValueError):
                H.validate_inputs(self.args())

    def test_portable_manifest_and_input_mutation(self):
        inputs = H.validate_inputs(self.args())
        self.assertEqual(inputs['kernel']['path'], str(self.base / 'vmlinux'))
        self.assertEqual(inputs['memory'], '512M')
        self.assertEqual(inputs['initramfs']['members'], 5)
        self.assertEqual(len(inputs['manifest_sha256']), 64)
        H.verify_inputs(inputs)
        self.paths['shell'].write_bytes(b'changed')
        with self.assertRaises(AssertionError):
            H.verify_inputs(inputs)

    def test_read_only_input_output_collision(self):
        collision = self.work / 'report.json.new'
        collision.write_bytes(elf_bytes())
        with self.assertRaisesRegex(ValueError, 'overlaps'):
            H.validate_inputs(self.args(shell=collision))

    def test_mocked_packaging_records_effective_hashes_and_replaces_once(self):
        args = self.args()
        inputs = H.validate_inputs(args)
        def mocked_image_tool(*arguments):
            self.assertEqual(arguments[0], 'xorriso')
            Path(arguments[-1]).write_bytes(b'mocked ISO')
            return b''
        with patch.object(H, 'run', side_effect=mocked_image_tool) as tool:
            iso, effective = H.image(args, 'dry-run', inputs)
        self.assertEqual(tool.call_count, 1)
        self.assertTrue(iso.is_file())
        self.assertEqual(effective['kernel']['sha256'], inputs['kernel']['sha256'])
        self.assertEqual(effective['ramdisk']['sha256'], H.digest(Path(effective['ramdisk']['path'])))
        with tarfile.open(effective['ramdisk']['path']) as archive:
            names = archive.getnames()
            self.assertEqual(len(names), len(set(names)))
            for name in (*H.REPLACEMENTS, 'power-vfork'):
                self.assertEqual(archive.extractfile('bin/' + name).read(), elf_bytes())
        H.verify_inputs(inputs)

    def test_telemetry_negative_controls(self):
        good = telemetry_snapshot()
        self.assertEqual(set(H.fixture_telemetry(good)), {'TAC0', 'TBT0', 'TBT1', 'TBT2', 'TBT3'})
        for bad in (good.replace('remaining=3000', 'remaining=2500'),
                    good.replace('notifications=2', 'notifications=0'),
                    good.replace('last_notify=81', 'last_notify=80'),
                    good.replace('notify_status=AE_OK', 'notify_status=AE_ERROR'),
                    good.replace('stale=0', 'stale=1'), good + 'telemetry_output_truncated=1\n'):
            with self.assertRaises(AssertionError):
                H.fixture_telemetry(bad)

    def test_report_append_and_failed_atomic_replacement_preserve_history(self):
        path = self.work / 'report.json'
        history = [{'result': 'passed', 'legacy_proof': {'hash': 'old', 'events': [1, 2]}}]
        H.save_report(path, history)
        before = path.read_bytes()
        appended = H.load_report(path) + [{'result': 'passed', 'new': True}]
        with patch.object(Path, 'replace', side_effect=OSError('simulated rename failure')):
            with self.assertRaises(OSError):
                H.save_report(path, appended)
        self.assertEqual(path.read_bytes(), before)
        H.save_report(path, appended)
        self.assertEqual(H.load_report(path)[0], history[0])
        self.assertEqual(len(H.load_report(path)), 2)
        self.assertFalse(path.with_name('report.json.new').exists())
        path.write_text('{}')
        with self.assertRaises(ValueError):
            H.load_report(path)

    def terminal_scenario(self, verification='POWER WORKLOAD VERIFIED bytes=131072 eof=1', kind='poweroff',
                          capability='POWER WORKLOAD CAPABILITY verify=1 bytes=131072 eof=1'):
        """Exercise the actual host scenario control flow; guest/tool IO mocked."""
        args = self.args(kind=kind, transport='ahci')
        inputs = H.validate_inputs(args)
        iso = self.directory / 'terminal.iso'
        iso.write_bytes(b'mocked ISO for scenario control flow')
        effective = {'kernel': inputs['kernel'], 'ramdisk': inputs['initramfs']}
        events = []
        machines = []
        class FakeVM:
            def __init__(self, *unused, **options):
                self.number = len(machines)
                self.log = SimpleNamespace(name=str(H.ROOT / ('guest-' + str(self.number) + '.log')))
                machines.append(self)
                events.append(('boot', self.number))
            def command(self, line, expect=None, timeout=None):
                events.append(('command', self.number, line))
                output = verification if line.endswith(' verify') else capability if line.endswith('--verify-capability') else (expect or '')
                if expect is not None and expect not in output:
                    raise AssertionError(output)
                return output
            def send(self, line): events.append(('send', self.number, line))
            def qmp(self, *unused): return {'running': True}
            def close(self): events.append(('close', self.number))
        expected = bytes((i * 31 + 17) & 255 for i in range(4096)) * 32
        def host_tool(*command):
            return expected if command[0] == 'mtype' else b''
        report = []
        self.terminal_report = report
        self.terminal_events = events
        with patch.object(H, 'PowerVM', FakeVM), patch.object(H, 'run', side_effect=host_tool), \
             patch.object(H, 'terminal', return_value=[{'event': 'SHUTDOWN'}]), \
             patch.object(H, 'cached_telemetry', return_value='mocked cached snapshot'):
            H.scenario(iso, args, report, inputs, effective)
        return events, report[-1]

    def test_terminal_requires_full_content_verifier_in_a_fresh_guest(self):
        for kind in ('poweroff', 'reboot'):
            with self.subTest(kind=kind):
                events, report = self.terminal_scenario(kind=kind)
                verify = ('command', 1, 'power-workload /mnt/sata0/power.bin verify')
                self.assertIn(verify, events)
                self.assertLess(events.index(('close', 0)), events.index(('boot', 1)))
                self.assertLess(events.index(('boot', 1)), events.index(verify))
                self.assertLess(events.index(verify), events.index(('close', 1)))
                self.assertEqual(report['result'], 'passed')
                self.assertEqual(report['reopened_bytes_verified'], 131072)
                self.assertTrue(report['reopened_eof_verified'])

    def test_terminal_rejects_stale_fixture_before_any_write_or_power(self):
        with self.assertRaises(AssertionError):
            self.terminal_scenario(capability='POWER WORKLOAD FAIL unsupported probe')
        self.assertEqual(self.terminal_report[-1]['result'], 'failed')
        self.assertFalse(any(event[0] == 'send' for event in self.terminal_events))
        self.assertNotIn(('boot', 1), self.terminal_events)

    def test_terminal_reopen_corruption_is_a_failed_proof(self):
        with self.assertRaises(AssertionError):
            self.terminal_scenario('POWER WORKLOAD FAIL verification mismatch')
        report = self.terminal_report[-1]
        self.assertEqual(report['result'], 'failed')
        self.assertNotIn('reopened_bytes_verified', report)

    def test_terminal_reopen_marker_with_error_is_not_a_pass(self):
        with self.assertRaises(AssertionError):
            self.terminal_scenario('POWER WORKLOAD VERIFIED bytes=131072 eof=1\npower-workload: exited 1')
        self.assertEqual(self.terminal_report[-1]['result'], 'failed')


if __name__ == '__main__':
    unittest.main(verbosity=2)
