#!/usr/bin/env python3
"""Disposable 384MiB-default QEMU power tests with explicit, read-only inputs.

Only generated fixture images are writable. QMP guest events, not tool exit or
AE_OK, prove terminal actions. Closing a recovery VM is teardown only. --base
may be a final packaged output tree containing iso_root/, initramfs.tar and
vmlinux. No original-checkout layout is inferred. --validate-only never builds
an ISO or starts a guest.
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import select
import shutil
import stat
import struct
import tarfile
import time
from test_runtime import VM, run, ANSI

ROOT = pathlib.Path(__file__).resolve().parents[1]
WORK = ROOT / 'build/power-tests'
KINDS = ('dry-run', 'omit-ack', 'flush-fail', 'drain-busy', 'cancel', 'poweroff', 'reboot')
REPLACEMENTS = ('coast-sh', 'power-workload', 'thread-check', 'ipc-check')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def file_input(path, label, elf=False):
    path = path.expanduser().resolve(strict=True)
    if not stat.S_ISREG(path.stat().st_mode) or not path.stat().st_size:
        raise ValueError(f'{label} must be a nonempty regular file: {path}')
    with path.open('rb') as stream:
        header = stream.read(64)
    if elf and (len(header) < 64 or header[:7] != b'\x7fELF\x02\x01\x01' or
                struct.unpack_from('<H', header, 18)[0] != 62 or
                struct.unpack_from('<H', header, 16)[0] not in (2, 3)):
        raise ValueError(f'{label} must be an x86-64 little-endian executable ELF: {path}')
    return dict(path=str(path), bytes=path.stat().st_size, sha256=digest(path))


def member_name(name):
    path = pathlib.PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or not path.parts:
        raise ValueError(f'Unsafe initramfs member name: {name!r}')
    return str(path)


def validate_tar(path):
    names = {}
    regular = set()
    with tarfile.open(path, 'r:') as archive:
        for item in archive:
            name = member_name(item.name)
            metadata = (item.type, item.mode, item.uid, item.gid)
            if name in names:
                # Historic full images repeat identical parent directories.
                # Files, links and differing directory metadata stay ambiguous.
                if item.isdir() and names[name] == metadata:
                    continue
                raise ValueError(f'Duplicate initramfs member: {name}')
            names[name] = metadata
            if not (item.isfile() or item.isdir() or item.issym() or item.islnk()):
                raise ValueError(f'Unsupported initramfs member type: {name}')
            if item.isfile():
                regular.add(name)
                stream = archive.extractfile(item)
                if stream is None:
                    raise ValueError(f'Unreadable initramfs member: {name}')
                # Read through every source member now, before creating outputs.
                remaining = item.size
                with stream:
                    while remaining:
                        block = stream.read(min(remaining, 1024 * 1024))
                        if not block:
                            raise ValueError(f'Truncated initramfs member: {name}')
                        remaining -= len(block)
    if 'bin/check' not in regular:
        raise ValueError('Base initramfs must contain the real bin/check fixture')
    return len(names)


def iso_template(path):
    if not path.is_dir() or path.is_symlink():
        raise ValueError(f'Base iso_root must be a real directory: {path}')
    files = []
    for entry in sorted(path.rglob('*')):
        if entry.is_symlink():
            raise ValueError(f'ISO template symlinks are not accepted: {entry}')
        if entry.is_dir():
            continue
        if not entry.is_file():
            raise ValueError(f'ISO template contains a non-regular entry: {entry}')
        relative = entry.relative_to(path).as_posix()
        if relative not in ('boot/kernel.elf', 'boot/initramfs.tar'):
            record = file_input(entry, 'ISO template')
            record['relative_path'] = relative
            files.append(record)
    for required in ('boot/limine/limine.conf', 'boot/limine/limine-uefi-cd.bin'):
        if not any(item['relative_path'] == required for item in files):
            raise ValueError(f'ISO template is missing {required}')
    data = json.dumps(files, sort_keys=True, separators=(',', ':')).encode()
    return dict(path=str(path), files=files, sha256=hashlib.sha256(data).hexdigest())


def boot_config(text, mode):
    """Edit only the first native boot stanza; preserve other cmdline tokens."""
    text = text.replace('1920x1200x32', '1024x768x32')
    protocol = re.search(r'^([ \t]+)protocol:[ \t]*limine[ \t]*$', text, re.M)
    if not protocol:
        raise ValueError('ISO template has no native Limine boot stanza')
    tail = text[protocol.end():]
    next_entry = re.search(r'^/', tail, re.M)
    end = protocol.end() + (next_entry.start() if next_entry else len(tail))
    stanza = text[protocol.start():end]
    if not re.search(r'^\s+path:\s*boot\(\):/boot/kernel\.elf\s*$', stanza, re.M) or not re.search(
            r'^\s+module_path:\s*boot\(\):/boot/initramfs\.tar\s*$', stanza, re.M):
        raise ValueError('Native boot stanza must use /boot/kernel.elf and /boot/initramfs.tar')
    previous = re.findall(r'^[ \t]+cmdline:[ \t]*(.*)$', stanza, re.M)
    if len(previous) > 1:
        raise ValueError('Native boot stanza has ambiguous duplicate cmdline entries')
    tokens = [token for token in (previous[0].split() if previous else [])
              if not token.startswith('power.test=')]
    if mode:
        tokens.append('power.test=' + mode)
    stanza = re.sub(r'^[ \t]+cmdline:[^\n]*(?:\n|$)', '', stanza, flags=re.M)
    first, rest = stanza.split('\n', 1)
    stanza = first + '\n' + protocol.group(1) + 'cmdline: ' + ' '.join(tokens) + '\n' + rest
    return text[:protocol.start()] + stanza + text[end:]


def validate_inputs(args):
    base = args.base.expanduser().resolve(strict=True)
    if not base.is_dir():
        raise ValueError(f'--base is not a directory: {base}')
    args.base = base
    args.kernel = args.kernel or base / 'vmlinux'
    defaults = {'shell': ROOT / 'userland/bin/coast-sh',
                'workload': WORK / 'power-workload',
                'thread_check': ROOT / 'userland/bin/thread-check',
                'ipc_check': ROOT / 'userland/bin/ipc-check'}
    inputs = {'base': str(base), 'iso_template': iso_template(base / 'iso_root'),
              'kernel': file_input(args.kernel, '--kernel', elf=True),
              'initramfs': file_input(base / 'initramfs.tar', 'Base initramfs')}
    inputs['initramfs']['members'] = validate_tar(pathlib.Path(inputs['initramfs']['path']))
    for option, default in defaults.items():
        record = file_input(getattr(args, option) or default, '--' + option.replace('_', '-'), elf=True)
        setattr(args, option, pathlib.Path(record['path']))
        inputs[option] = record
    if args.vfork_workload:
        inputs['vfork_workload'] = file_input(args.vfork_workload, '--vfork-workload', elf=True)
        args.vfork_workload = pathlib.Path(inputs['vfork_workload']['path'])
    args.kernel = pathlib.Path(inputs['kernel']['path'])
    inputs['firmware'] = file_input(ROOT / 'firmware/ovmf/OVMF.fd', 'QEMU firmware')
    if args.telemetry_aml:
        inputs['telemetry_aml'] = file_input(args.telemetry_aml, '--telemetry-aml')
        args.telemetry_aml = pathlib.Path(inputs['telemetry_aml']['path'])
        aml = args.telemetry_aml.read_bytes()
        if len(aml) < 36 or len(aml) > 1024 * 1024 or aml[:4] != b'SSDT' or \
                struct.unpack_from('<I', aml, 4)[0] != len(aml) or sum(aml) & 255:
            raise ValueError('--telemetry-aml must be a complete, checksum-valid SSDT')
    inputs['memory'] = args.memory
    # Configuration is validated before copying files or invoking host tools.
    boot_config((base / 'iso_root/boot/limine/limine.conf').read_text(), '')
    mode = args.kind if args.kind in ('dry-run', 'omit-ack', 'flush-fail', 'drain-busy') else ''
    outputs = [WORK / ('iso-' + (mode or 'normal')), WORK / ((mode or 'normal') + '.iso'),
               WORK / f'power-{args.kind}-{args.transport}.volume', WORK / 'report.json',
               WORK / 'report.json.new']
    sources = [pathlib.Path(record['path']) for record in inputs.values()
               if isinstance(record, dict) and 'path' in record]
    sources += [pathlib.Path(item['path']) for item in inputs['iso_template']['files']]
    for source in sources:
        if any(source == output or output in source.parents for output in outputs):
            raise ValueError(f'Read-only input overlaps a generated output: {source}')
    inputs['manifest_sha256'] = hashlib.sha256(json.dumps(
        inputs, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    return inputs


def verify_inputs(inputs):
    records = [record for record in inputs.values() if isinstance(record, dict) and 'bytes' in record]
    records += inputs['iso_template']['files']
    for record in records:
        path = pathlib.Path(record['path'])
        if path.stat().st_size != record['bytes'] or digest(path) != record['sha256']:
            raise AssertionError(f'Read-only input changed during test: {path}')


def image(args, mode, inputs):
    tree = WORK / ('iso-' + (mode or 'normal'))
    if tree.exists():
        shutil.rmtree(tree)
    def ignore(directory, names):
        return [name for name in names if (pathlib.Path(directory) / name).relative_to(
            args.base / 'iso_root').as_posix() in ('boot/kernel.elf', 'boot/initramfs.tar')]
    shutil.copytree(args.base / 'iso_root', tree, ignore=ignore)
    shutil.copy2(args.kernel, tree / 'boot/kernel.elf')
    ram = tree / 'boot/initramfs.tar'
    files = dict(zip(REPLACEMENTS, (args.shell, args.workload, args.thread_check, args.ipc_check)))
    if args.vfork_workload:
        files['power-vfork'] = args.vfork_workload
    replace = {'bin/' + name for name in files}
    with tarfile.open(args.base / 'initramfs.tar', 'r:') as src, tarfile.open(
            ram, 'w', format=tarfile.USTAR_FORMAT) as dst:
        for item in src:
            if member_name(item.name) not in replace:
                dst.addfile(item, src.extractfile(item) if item.isfile() else None)
        for name, path in files.items():
            info = tarfile.TarInfo('bin/' + name)
            info.mode = 0o755
            info.size = path.stat().st_size
            with path.open('rb') as stream:
                dst.addfile(info, stream)
    cfg = tree / 'boot/limine/limine.conf'
    cfg.write_text(boot_config(cfg.read_text(), mode))
    effective = {'kernel': file_input(tree / 'boot/kernel.elf', 'Effective boot kernel', elf=True),
                 'ramdisk': file_input(ram, 'Effective boot ramdisk'),
                 'configuration': file_input(cfg, 'Effective boot configuration')}
    verify_inputs(inputs)
    iso = WORK / ((mode or 'normal') + '.iso')
    run('xorriso', '-as', 'mkisofs', '-R', '-J', '-V', 'COAST_POWER', '-e',
        'boot/limine/limine-uefi-cd.bin', '-no-emul-boot', '-efi-boot-part',
        '--efi-boot-image', '--protective-msdos-label', tree, '-o', iso)
    return iso, effective


def plain(vm):
    vm.log.flush()
    return ANSI.sub(b'', pathlib.Path(vm.log.name).read_bytes()).replace(b'\r', b'').decode(errors='replace')


def drain(vm):
    if vm.selector.select(.02):
        data = os.read(vm.process.stdout.fileno(), 65536)
        vm.pending.extend(data)
        vm.log.write(data)
        vm.log.flush()


def terminal(vm, reason, timeout=30):
    deadline = time.monotonic() + timeout
    events = []
    found = False
    while time.monotonic() < deadline:
        drain(vm)
        if select.select([vm.qmp_output], [], [], .02)[0]:
            vm.qmp_pending.extend(os.read(vm.qmp_output, 65536))
        while b'\n' in vm.qmp_pending:
            line, _, rest = vm.qmp_pending.partition(b'\n')
            vm.qmp_pending = bytearray(rest)
            if not line.strip():
                continue
            event = json.loads(line)
            events.append(event)
            data = event.get('data', {})
            if event.get('event') == 'SHUTDOWN' and data.get('reason') == reason and data.get('guest') is True:
                found = True
        if found and vm.process.poll() is not None:
            break
        if vm.process.poll() is not None and not found:
            time.sleep(.05)
    log = plain(vm)
    assert found, (reason, events, log)
    assert 'power: COMMIT' in log and 'flush complete' in log and 'reversible park acknowledged' in log, log
    assert 'POWER WORKLOAD ACCEPTED' in log and 'POWER WORKLOAD FAIL' not in log and 'PANIC:' not in log, log
    assert 'terminal failure' not in log, log
    return events


class PowerVM(VM):
    def until(self, pattern, timeout=30):
        try:
            return super().until(pattern, timeout)
        except AssertionError:
            end = time.monotonic() + .5
            while time.monotonic() < end:
                drain(self)
            raise AssertionError(plain(self))


def telemetry_fields(snapshot):
    devices = {}
    current = None
    for line in snapshot.splitlines():
        match = re.match(r'^(AC|Battery)\[(\d+)\] (\S+) (.*)$', line)
        if match:
            current = match.group(3).rsplit('.', 1)[-1]
            devices[current] = dict(kind=match.group(1), text=line,
                                   fields=dict(re.findall(r'(\w+)=(\S+)', match.group(4))))
        elif line.startswith('  ') and current:
            devices[current]['text'] += '\n' + line
            devices[current]['fields'].update(re.findall(r'(\w+)=(\S+)', line))
        else:
            current = None
    return devices


def fixture_telemetry(snapshot):
    """Assert actual cached standard-method data; no blanket ACPI success badge."""
    assert 'telemetry=running discovery=complete' in snapshot, snapshot
    assert 'telemetry_output_truncated=1' not in snapshot, snapshot
    devices = telemetry_fields(snapshot)
    expected = {
        'TAC0': {'kind': 'AC', 'present': 'yes', 'online': 'yes', '_PSR': 'AE_OK'},
        'TBT0': {'kind': 'Battery', 'present': 'yes', 'source': '_BIX', 'unit': 'mWh/mW',
                 '_BIX': 'AE_OK', 'information': 'AE_OK', '_BST': 'AE_OK',
                 'design': '6000', 'full': '5000', 'remaining': '3000', 'rate': '1000',
                 'voltage_mV': '12000', 'percent': '60', 'state': 'discharging',
                 'critical': 'no', 'charge_limiting': 'no'},
        'TBT1': {'kind': 'Battery', 'present': 'yes', 'source': '_BIF', 'unit': 'mAh/mA',
                 '_BIX': 'AE_NOT_FOUND', 'information': 'AE_OK', '_BST': 'AE_OK',
                 'design': '6000', 'full': '5000', 'remaining': '4000', 'rate': 'unknown',
                 'voltage_mV': 'unknown', 'percent': '80', 'state': 'charging',
                 'critical': 'no', 'charge_limiting': 'yes'},
        'TBT2': {'kind': 'Battery', 'present': 'no', 'source': 'unavailable',
                 '_BIX': 'AE_NOT_EXIST', 'information': 'AE_NOT_EXIST', '_BST': 'AE_NOT_EXIST',
                 'unit': 'unknown', 'design': 'unknown', 'full': 'unknown', 'remaining': 'unknown',
                 'rate': 'unknown', 'voltage_mV': 'unknown', 'percent': 'unknown', 'state': 'unknown',
                 'critical': 'unknown', 'charge_limiting': 'unknown'},
        'TBT3': {'kind': 'Battery', 'present': 'yes', 'source': '_BIX', 'full': 'unknown',
                 '_BIX': 'AE_OK', 'information': 'AE_OK', '_BST': 'AE_OK', 'unit': 'mWh/mW',
                 'design': 'unknown', 'remaining': 'unknown', 'rate': 'unknown', 'voltage_mV': 'unknown',
                 'percent': 'unknown', 'state': 'discharging', 'critical': 'no', 'charge_limiting': 'no'},
    }
    summary = {}
    for name, values in expected.items():
        assert name in devices, (name, snapshot)
        device = devices[name]
        fields = device['fields']
        assert device['kind'] == values['kind'], (name, device)
        for key, value in values.items():
            if key != 'kind':
                assert fields.get(key) == value, (name, key, value, device)
        for key, value in {'stale': '0', 'refresh_pending': '0', 'attempted': '1',
                           '_STA': 'AE_OK', 'path_status': 'AE_OK', 'notify_status': 'AE_OK'}.items():
            assert fields.get(key) == value, (name, key, value, device)
        counts = {key: int(fields[key]) for key in ('notifications', 'coalesced', 'last_refresh_us', 'last_success_us')}
        assert counts['notifications'] >= counts['coalesced'] >= 0 and counts['last_refresh_us'] > 0, (name, counts)
        assert counts['notifications'] == (2 if name == 'TBT0' else 0), (name, counts)
        assert counts['coalesced'] == (1 if name == 'TBT0' else 0), (name, counts)
        assert fields['last_notify'] == ('81' if name == 'TBT0' else '0'), (name, fields)
        if name in ('TBT0', 'TBT1', 'TBT3'):
            model = {'TBT0': 'Telemetry mWh', 'TBT1': 'Telemetry mAh', 'TBT3': 'Telemetry unknown'}[name]
            assert 'model=' + model in device['text'], (name, device)
            assert counts['last_success_us'] > 0, (name, counts)
        summary[name] = {'values': fields, 'statistics': counts}
    return summary


def cached_telemetry(vm, fixture=False, timeout=30):
    deadline = time.monotonic() + timeout
    latest = ''
    while time.monotonic() < deadline:
        latest = vm.command('acpi')
        try:
            if fixture:
                return {'snapshot': latest, 'devices': fixture_telemetry(latest)}
            assert 'telemetry=running discovery=complete' in latest
            return {'snapshot': latest}
        except (AssertionError, KeyError, ValueError):
            time.sleep(.1)
    raise AssertionError('Cached telemetry did not reach expected fixture state:\n' + latest)


def scenario(iso, args, report, inputs, effective):
    label = f'power-{args.kind}-{args.transport}-{effective["kernel"]["sha256"][:8]}-{time.time_ns()}'
    disk = WORK / (label + '.volume')
    with disk.open('wb') as stream:
        stream.truncate(64 * 1024 * 1024)
    run('mkfs.fat', '-F', '32', disk)
    escaped_disk = str(disk).replace(',', ',,')
    extra = ['-drive', f'file={escaped_disk},format=raw,if=none,id=pwr']
    if args.transport == 'ahci':
        extra += ['-device', 'ide-hd,drive=pwr,bus=ide.0']
        mount = '/mnt/sata0'
    else:
        extra += ['-device', 'nvme,drive=pwr,serial=POWER-FIXTURE']
        mount = '/mnt/nvme0n1'
    if args.telemetry_aml:
        extra += ['-acpitable', 'file=' + str(args.telemetry_aml).replace(',', ',,')]
    vm = None
    item = dict(kind=args.kind, cpus=args.cpus, transport=args.transport, memory=args.memory,
                iso_sha256=digest(iso), input_manifest=inputs, effective_boot=effective,
                kernel_sha256=effective['kernel']['sha256'], ramdisk_sha256=effective['ramdisk']['sha256'],
                qemu_extra_args=extra, result='failed')
    report.append(item)
    try:
        vm = PowerVM(args.cpus, storage=False, network=False, memory=args.memory,
                     iso=iso, label=label, extra_args=extra)
        item['log'] = str(pathlib.Path(vm.log.name).relative_to(ROOT))
        vm.command('power', 'poweroff_ready=1')
        item['telemetry_before'] = cached_telemetry(vm, bool(args.telemetry_aml))
        vm.command('check', 'USERLAND CHECK PASS', timeout=45)
        if args.kind in ('poweroff', 'reboot'):
            # Reject an old fixture before any write/power action. Old argc=3
            # "PATH verify" would otherwise fall through to another poweroff.
            capability = vm.command('power-workload --verify-capability',
                                    'POWER WORKLOAD CAPABILITY verify=1 bytes=131072 eof=1')
            assert not re.search(r'POWER WORKLOAD FAIL|PANIC:|exited [1-9]', capability), capability
            item['workload_verification_capability'] = capability
        vm.qmp('query-status')
        vfork = bool(args.vfork_workload and args.kind in ('dry-run', 'omit-ack', 'flush-fail', 'drain-busy'))
        if vfork:
            output = vm.command('power-vfork &')
            if 'POWER VFORK CHILD WAIT' not in output and 'POWER VFORK CHILD WAIT' not in plain(vm):
                vm.until(rb'POWER VFORK CHILD WAIT', timeout=10)
            assert 'POWER VFORK PARENT RESUMED' not in plain(vm), 'vfork child ended before power request'
            item['vfork_child_wait_observed_before_power'] = True
        if args.kind in ('poweroff', 'reboot'):
            vm.send(f'power-workload {mount}/power.bin {args.kind}\n')
            item['qmp_events'] = terminal(vm, 'guest-shutdown' if args.kind == 'poweroff' else 'guest-reset')
        else:
            out = vm.command(f'power-workload {mount}/power.bin ' +
                             ('cancel' if args.kind == 'cancel' else 'recover'),
                             'POWER WORKLOAD RECOVERED', timeout=30)
            if args.kind != 'cancel':
                assert 'software-resumed=1' in out or 'software-resumed=1' in plain(vm), out
            if vfork:
                if 'POWER VFORK PARENT RESUMED' not in plain(vm):
                    vm.until(rb'POWER VFORK PARENT RESUMED', timeout=15)
                assert 'POWER VFORK FAIL' not in plain(vm), plain(vm)
                item['vfork_parent_resumed'] = True
            vm.command('check', 'USERLAND CHECK PASS', timeout=45)
            vm.command('thread-check', 'THREAD CHECK PASS', timeout=120)
            vm.command('ipc-check', 'IPC CHECK PASS', timeout=90)
            item['recovery_snapshot'] = vm.command('power')
            item['telemetry_after_recovery'] = cached_telemetry(vm, bool(args.telemetry_aml))
        vm.close()
        vm = None
        run('fsck.fat', '-n', disk)
        actual = run('mtype', '-i', disk, '::power.bin')
        expected = bytes((i * 31 + 17) & 255 for i in range(4096)) * 32
        assert actual == expected, (len(actual), len(expected))
        item['persisted_sha256'] = hashlib.sha256(actual).hexdigest()
        if args.kind in ('poweroff', 'reboot'):
            vm = PowerVM(args.cpus, storage=False, network=False, memory=args.memory,
                         iso=iso, label=label + '-reopen', extra_args=extra)
            vm.command(f'ls {mount}', 'power.bin')
            verified = vm.command(f'power-workload {mount}/power.bin verify',
                                  'POWER WORKLOAD VERIFIED bytes=131072 eof=1', timeout=45)
            assert not re.search(r'POWER WORKLOAD FAIL|PANIC:|exited [1-9]', verified), verified
            item['reopen_verification'] = verified
            item['reopened_bytes_verified'] = 131072
            item['reopened_eof_verified'] = True
            vm.command('check', 'USERLAND CHECK PASS', timeout=45)
            item['telemetry_after_reopen'] = cached_telemetry(vm, bool(args.telemetry_aml))
            vm.close()
            vm = None
        verify_inputs(inputs)
        item['read_only_inputs_unchanged'] = True
        item['result'] = 'passed'
    except Exception as error:
        item['failure'] = str(error)
        raise
    finally:
        try:
            if vm:
                vm.close()
        finally:
            save_report(WORK / 'report.json', report)


def load_report(path):
    report = json.loads(path.read_text()) if path.exists() else []
    if not isinstance(report, list) or not all(isinstance(item, dict) for item in report):
        raise ValueError(f'Existing proof report must be an array of entries: {path}')
    return report


def save_report(path, report):
    """Keep historical proof entries intact if an interrupted write fails."""
    temporary = path.with_name(path.name + '.new')
    with temporary.open('w') as stream:
        stream.write(json.dumps(report, indent=2) + '\n')
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=pathlib.Path, required=True,
                        help='Read-only packaged output tree containing iso_root/, initramfs.tar, vmlinux')
    parser.add_argument('--kernel', type=pathlib.Path, help='Read-only boot ELF (default: BASE/vmlinux)')
    parser.add_argument('--memory', default='384M', help='QEMU guest memory (default: 384M)')
    parser.add_argument('--shell', type=pathlib.Path, help='Native shell ELF (default: local userland/bin/coast-sh)')
    parser.add_argument('--workload', type=pathlib.Path, help='Power workload ELF (default: local build/power-tests/power-workload)')
    parser.add_argument('--thread-check', type=pathlib.Path, help='Thread fixture ELF (default: local userland/bin/thread-check)')
    parser.add_argument('--ipc-check', type=pathlib.Path, help='IPC fixture ELF (default: local userland/bin/ipc-check)')
    parser.add_argument('--vfork-workload', type=pathlib.Path,
                        help='Optional power-vfork ELF; exercise an in-flight vfork child during rollback scenarios')
    parser.add_argument('--telemetry-aml', type=pathlib.Path,
                        help='Optional compiled acpica-telemetry.asl SSDT; inject and assert cached fixture data')
    parser.add_argument('--validate-only', action='store_true', help='Validate/hash inputs and print manifest; no ISO or guest')
    parser.add_argument('--kind', choices=KINDS, required=True)
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--transport', choices=('ahci', 'nvme'), default='ahci')
    args = parser.parse_args(argv)
    if args.cpus < 1 or args.cpus > 256:
        parser.error('--cpus must be between 1 and 256')
    if not re.fullmatch(r'[1-9][0-9]*(?:[KMGTP](?:B)?)?', args.memory, re.I):
        parser.error('--memory must be a positive QEMU size, e.g. 384M or 2G')
    return args


def main():
    args = arguments()
    inputs = validate_inputs(args)
    if args.validate_only:
        print(json.dumps(inputs, indent=2))
        return
    # Refuse malformed historical reports before creating or starting anything.
    report = load_report(WORK / 'report.json')
    WORK.mkdir(parents=True, exist_ok=True)
    mode = args.kind if args.kind in ('dry-run', 'omit-ack', 'flush-fail', 'drain-busy') else ''
    iso, effective = image(args, mode, inputs)
    scenario(iso, args, report, inputs, effective)
    print('POWER GUEST PASS', args.kind, args.cpus, args.transport, flush=True)


if __name__ == '__main__':
    main()
