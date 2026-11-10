#!/usr/bin/env python3
"""Exercise the actual ACPICA kernel port in disposable QEMU/UEFI guests.

The injected SSDT writes only a page allocated by the guest selftest. Firmware
NVS is read-only. The input ISO is unchanged: a separate ISO copy enables the
exact acpi.selftest boot token. No host disks, network, or audio devices are used.
Requires QEMU, xorriso and an official ACPICA iasl compiler on PATH.

Example:
    python3 tools/test_acpica.py --iso build/acpica/os.iso
Logs and a machine-readable stage report are under build/acpica-tests/.
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import time

from test_runtime import ANSI, VM, default_iso, run

ROOT = pathlib.Path(__file__).resolve().parents[1]
WORK = ROOT / 'build' / 'acpica-tests'
STAGES = {
    'gpe_admission_reservations': 'ACPICA: reserved dispatch/ordered-finish slots for ',
    'runtime_mapping_preparation': 'ACPICA: runtime mappings ',
    'boot_aml_interrupts': 'ACPICA: boot AML SCI interrupts enabled (IF=1)',
    'boot_consumer_interrupts': 'ACPICA: boot consumers completed with SCI interrupts enabled (IF=1)',
    'namespace': 'ACPICA: namespace initialized,',
    'sleep_types': 'ACPICA: _S5 evaluated,',
    'sci_initialization': 'ACPICA: hardware, SCI and AML initialization complete',
    'gpe_registration': 'ACPICA SELFTEST PASS: GPE method registered and enabled',
    'boot_aml': 'ACPICA SELFTEST PASS: boot AML arithmetic/package/mutex',
    'boot_memory_field': 'ACPICA SELFTEST PASS: boot SystemMemory owned-page field',
    'boot_nvs_field': 'ACPICA SELFTEST PASS: boot firmware NVS read-only field',
    'boot_deferred_notify': 'ACPICA SELFTEST PASS: boot Notify queued before scheduler',
    'scheduled_notify': 'ACPICA SELFTEST PASS: deferred Notify worker after sched_start',
    'runtime_first_mmio': 'ACPICA SELFTEST PASS: runtime-first literal MMIO preflight Field',
    'runtime_aml_memory': 'ACPICA SELFTEST PASS: runtime AML/mutex/SystemMemory/NVS',
    'runtime_timing': 'ACPICA SELFTEST PASS: runtime Sleep/Stall/mutex repeated',
    'mutex_contention': 'ACPICA SELFTEST PASS: concurrent AML mutex contention',
    'repeated_notify': 'ACPICA SELFTEST PASS: repeated deferred Notify (4 callbacks)',
    'selftest_complete': 'ACPICA SELFTEST PASS: complete',
    'ring3': 'USERLAND CHECK PASS',
}
POWER_EVENT = 'ACPICA: fixed power button event received'


def digest(path):
    with path.open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


def plain_log(vm):
    vm.log.flush()
    data = pathlib.Path(vm.log.name).read_bytes()
    return ANSI.sub(b'', data).replace(b'\r', b'').decode(errors='replace')


def reject_failure(log):
    if re.search(r'ACPICA SELFTEST FAIL:|PANIC:|ACPI (?:Error|Exception):', log):
        raise AssertionError(log)


def await_log(vm, needle, timeout=20, count=1):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        log = plain_log(vm)
        reject_failure(log)
        if log.count(needle) >= count:
            return log
        if vm.process.poll() is not None:
            raise AssertionError(f'{vm.name}: QEMU exited before {needle!r}\n{log}')
        # VM.__init__ consumes boot output up to its first prompt. Keep draining
        # the serial pipe while background workers finish, without assuming a
        # second prompt follows an asynchronous kernel log.
        if vm.selector.select(.1):
            data = os.read(vm.process.stdout.fileno(), 65536)
            vm.pending.extend(data)
            vm.log.write(data)
            vm.log.flush()
    raise AssertionError(f'{vm.name}: missing {count} x {needle!r}\n{plain_log(vm)}')


def prepare(iso, iasl):
    WORK.mkdir(parents=True, exist_ok=True)
    prefix = WORK / 'acpica-selftest'
    compiler = run(iasl, '-p', prefix, ROOT / 'tools/fixtures/acpica-selftest.asl').decode()
    (WORK / 'iasl.log').write_text(compiler)
    aml = prefix.with_suffix('.aml')
    if not aml.is_file():
        raise AssertionError(f'iasl did not create {aml}')
    tree = WORK / 'iso-root'
    if tree.exists():
        shutil.rmtree(tree)
    run('xorriso', '-osirrox', 'on', '-indev', iso, '-extract', '/', tree)
    config = tree / 'boot/coast.conf'
    text = config.read_text()
    matches = list(re.finditer(r'^cmdline=(.*)$', text, re.MULTILINE))
    if len(matches) != 1:
        raise AssertionError('Expected one cmdline in the native UEFI configuration')
    text = re.sub(r'^cmdline=(.*)$', lambda m: 'cmdline=' + m[1] + ' acpi.selftest',
                  text, flags=re.MULTILINE)
    config.write_text(text)
    limine = tree / 'boot/limine/limine.conf'
    if limine.is_file():
        text = limine.read_text()
        if re.search(r'^\s+cmdline:', text, re.MULTILINE):
            text = re.sub(r'^(\s+cmdline:.*)$', r'\1 acpi.selftest', text,
                          flags=re.MULTILINE)
        else:
            text = re.sub(r'^(\s+)protocol: limine$',
                          r'\1protocol: limine\n\1cmdline: acpi.selftest', text,
                          flags=re.MULTILINE)
        limine.write_text(text)
        efi_boot = 'boot/limine/limine-uefi-cd.bin'
    elif (tree / 'efiboot.img').is_file():
        # Native EFI adapters read the policy from the FAT boot image too.
        run('mcopy', '-o', '-i', tree / 'efiboot.img', config, '::boot/coast.conf')
        efi_boot = 'efiboot.img'
    else:
        raise AssertionError('Unsupported source ISO: no known UEFI boot image')
    image = WORK / 'selftest.iso'
    image.unlink(missing_ok=True)
    # Rebuild only this disposable UEFI test image. Replaying the hybrid source
    # MBR is unreliable after Limine has installed its BIOS stages.
    run('xorriso', '-as', 'mkisofs', '-R', '-J', '-V', 'COAST_ISO',
        '-e', efi_boot, '-no-emul-boot', '-efi-boot-part', '--efi-boot-image',
        '--protective-msdos-label', tree, '-o', image)
    return image, aml


def scenario(cpus, iso, aml, events, memory, report):
    vm = None
    log_path = ROOT / 'build/runtime-tests' / f'uefi-{cpus}cpu-acpica-selftest.log'
    log_path.unlink(missing_ok=True)
    item = {'cpus': cpus, 'memory': memory, 'firmware': 'UEFI',
            'log': str(log_path.relative_to(ROOT)), 'executed': [], 'missing': list(STAGES),
            'power_events': 0, 'result': 'failed'}
    report['runs'].append(item)
    try:
        vm = VM(cpus, storage=False, network=False, iso=iso, memory=memory,
                label='acpica-selftest', extra_args=['-acpitable', f'file={aml}'])
        item['log'] = str(pathlib.Path(vm.log.name).relative_to(ROOT))
        await_log(vm, STAGES['selftest_complete'])
        output = vm.command('check', 'USERLAND CHECK PASS', timeout=45)
        if re.search(r'FAIL:|exited [1-9]|PANIC:', output):
            raise AssertionError(output)
        for event in range(1, events + 1):
            vm.qmp('system_powerdown')
            await_log(vm, POWER_EVENT, count=event)
            # A fresh round trip checks that SCI acknowledgement/EOI left the
            # system responsive after every power request, not only the first.
            vm.command(f'echo SCI_ALIVE_{event}', f'SCI_ALIVE_{event}\n')
            item['power_events'] = event
        output = vm.command('check', 'USERLAND CHECK PASS', timeout=45)
        if re.search(r'FAIL:|exited [1-9]|PANIC:', output):
            raise AssertionError(output)
        if not vm.qmp('query-status')['running']:
            raise AssertionError('Guest stopped unexpectedly after fixed SCI events')
        log = plain_log(vm)
        reject_failure(log)
        item['executed'] = [name for name, marker in STAGES.items() if marker in log]
        item['missing'] = [name for name in STAGES if name not in item['executed']]
        if item['missing']:
            raise AssertionError(f'Missing verification stages: {item["missing"]}')
        if log.count(POWER_EVENT) != events:
            raise AssertionError('Unexpected fixed power-event count (lost/duplicate SCI)')
        item['result'] = 'passed'
        print(f'PASS UEFI {cpus} CPU: {len(STAGES)} stages, ring-3 check before/after '
              f'{events} fixed power SCI events', flush=True)
    except BaseException as error:
        if vm or log_path.is_file():
            log = plain_log(vm) if vm else ANSI.sub(b'', log_path.read_bytes()).replace(
                b'\r', b'').decode(errors='replace')
            item['executed'] = [name for name, marker in STAGES.items() if marker in log]
            item['missing'] = [name for name in STAGES if name not in item['executed']]
        item['error'] = str(error)
        raise
    finally:
        if vm:
            vm.close()
        (WORK / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=pathlib.Path, default=default_iso())
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--events', type=int, default=3)
    parser.add_argument('--memory', default='2G', help='QEMU guest RAM (default: 2G)')
    parser.add_argument('--iasl', default=shutil.which('iasl') or 'iasl')
    args = parser.parse_args()
    if args.events < 2 or any(cpus < 1 for cpus in args.cpus):
        parser.error('--events must be >=2 and --cpus must be positive')
    iso = args.iso.resolve()
    before = digest(iso)
    report = {'source_iso': str(iso), 'source_sha256': before, 'requested_cpus': args.cpus,
              'result': 'failed', 'runs': []}
    try:
        image, aml = prepare(iso, args.iasl)
        report['selftest_iso'] = str(image)
        report['ssdt_sha256'] = digest(aml)
        for cpus in args.cpus:
            scenario(cpus, image, aml, args.events, args.memory, report)
        report['result'] = 'passed'
    except BaseException as error:
        report['error'] = str(error)
        raise
    finally:
        started = {item['cpus'] for item in report['runs']}
        report['unstarted_cpus'] = [cpus for cpus in args.cpus if cpus not in started]
        report['source_unchanged'] = digest(iso) == before
        (WORK / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if not report['source_unchanged']:
        raise AssertionError('Source ISO changed during the test')
    print(f'ACPICA runtime regression PASS; detailed report: {WORK / "report.json"}')


if __name__ == '__main__':
    main()
