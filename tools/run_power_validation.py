#!/usr/bin/env python3
"""Print reproducible, serial full-image power/ACPICA validation commands.

Printing is the default and does not read images, build anything or start VMs.
--execute requires explicit --exclusive-ready, --guest-marker and a positive
--min-available-kib. The caller must release competing build/guest work first.
Execution checks MemAvailable before each command and owns the supplied marker
for the entire serial run. Each underlying harness owns its proof report.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
DIRECT = 'direct unchanged ISO'
CONFIG = 'config-only ISO copies + synthetic AML; source kernel/initramfs unchanged'
FIXTURE = 'fixture-enriched full initramfs; source inputs unchanged'


def parse_arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True,
                        help='Final packaged output directory with iso_root/ and initramfs.tar')
    for name, description in (
            ('iso', 'Actual full release ISO, passed unchanged to direct release tests'),
            ('kernel', 'Exact final kernel ELF expected inside the release ISO'),
            ('shell', 'Exact final native shell ELF expected inside the release ISO'),
            ('gui', 'Exact final GUI ELF expected inside the release ISO'),
            ('workload', 'Power-workload ELF for fixture-enriched rollback tests'),
            ('vfork-workload', 'Power-vfork ELF for in-flight dependency tests'),
            ('thread-check', 'Thread regression ELF'),
            ('ipc-check', 'IPC regression ELF'),
            ('telemetry-aml', 'Compiled telemetry SSDT with the real Notify fixture')):
        parser.add_argument('--' + name, type=Path, required=True, help=description)
    parser.add_argument('--memory', default='2G', help='Guest memory for every scenario (default: 2G)')
    parser.add_argument('--iasl', required=True, help='Explicit ACPICA iasl executable path or command')
    parser.add_argument('--execute', action='store_true', help='Execute the printed commands serially')
    parser.add_argument('--exclusive-ready', action='store_true',
                        help='Attest competing compiler/guest work has been released')
    parser.add_argument('--guest-marker', type=Path,
                        help='Exclusive guest marker to atomically create; it must not already exist')
    parser.add_argument('--min-available-kib', type=int,
                        help='Caller-selected MemAvailable floor, required for execution')
    args = parser.parse_args(argv)
    if not re.fullmatch(r'[1-9][0-9]*(?:[KMGTP](?:B)?)?', args.memory, re.I):
        parser.error('--memory must be a positive QEMU size such as 2G')
    if args.min_available_kib is not None and args.min_available_kib <= 0:
        parser.error('--min-available-kib must be positive')
    if args.execute and not (args.exclusive_ready and args.guest_marker and args.min_available_kib):
        parser.error('--execute requires --exclusive-ready, --guest-marker and --min-available-kib')
    for name in ('base', 'iso', 'kernel', 'shell', 'gui', 'workload', 'vfork_workload',
                 'thread_check', 'ipc_check', 'telemetry_aml'):
        setattr(args, name, getattr(args, name).expanduser().resolve())
    if args.guest_marker:
        # Keep the actual marker filename: resolving a pre-existing symlink
        # would hide it from O_EXCL's refusal to overwrite an existing marker.
        marker = args.guest_marker.expanduser()
        args.guest_marker = marker.parent.resolve() / marker.name
    if '/' in args.iasl or args.iasl.startswith('~'):
        args.iasl = str(Path(args.iasl).expanduser().resolve())
    return args


def command_manifest(args):
    python = sys.executable
    def command(script, *options):
        return [python, str(ROOT / 'tools' / script), *map(str, options)]
    steps = []
    release = ['--iso', args.iso, '--kernel', args.kernel, '--shell', args.shell,
               '--gui', args.gui, '--memory', args.memory]
    for cpus, transport in ((1, 'ahci'), (4, 'nvme')):
        for kind in ('poweroff', 'reboot'):
            steps.append((DIRECT, f'{kind}, {cpus} CPU, {transport.upper()}',
                          command('test_power_release.py', *release, '--kind', kind,
                                  '--cpus', cpus, '--transport', transport)))
    for kind, transport in (('cancel', 'ahci'), ('gui-poweroff', 'nvme')):
        steps.append((DIRECT, f'{kind}, 4 CPU, {transport.upper()}',
                      command('test_power_release.py', *release, '--kind', kind,
                              '--cpus', 4, '--transport', transport)))
    fixture = ['--base', args.base, '--kernel', args.kernel, '--shell', args.shell,
               '--workload', args.workload, '--vfork-workload', args.vfork_workload,
               '--thread-check', args.thread_check, '--ipc-check', args.ipc_check,
               '--telemetry-aml', args.telemetry_aml, '--memory', args.memory, '--cpus', 4]
    for kind, transport in (('dry-run', 'ahci'), ('omit-ack', 'ahci'), ('flush-fail', 'nvme'),
                            ('drain-busy', 'ahci'), ('cancel', 'ahci')):
        steps.append((FIXTURE, f'{kind}, 4 CPU, {transport.upper()}, cached telemetry/Notify checks',
                      command('test_power.py', *fixture, '--kind', kind, '--transport', transport)))
    steps.append((FIXTURE, 'desktop dirty consent/cancel/rollback/save; uses generated dry-run ISO',
                  command('test_power_desktop.py', '--iso', ROOT / 'build/power-tests/dry-run.iso',
                          '--gui', args.gui, '--cpus', 4, '--memory', args.memory)))
    steps.append((CONFIG, '20 ACPICA stages, 1 and 4 CPU, three fixed-button events each',
                  command('test_acpica.py', '--iso', args.iso, '--cpus', 1, 4, '--events', 3,
                          '--memory', args.memory, '--iasl', args.iasl)))
    steps.append((CONFIG, 'three negative controls: inert fixture, missing fixture, missing AML mutex',
                  command('test_acpica_negative.py', '--iso', args.iso,
                          '--memory', args.memory, '--iasl', args.iasl)))
    return steps


def print_manifest(steps):
    print('# Serial validation manifest. Printing alone executes nothing.')
    print('# Direct release tests boot --iso unchanged; their embedded kernel/shell/GUI must match.')
    print('# Enriched rollback/UI tests are separate evidence and replace selected initramfs binaries.')
    for index, (category, description, command) in enumerate(steps, 1):
        print(f'\n# {index:02d}. [{category}] {description}')
        print(shlex.join(command))
    sys.stdout.flush()


def available_kib():
    for line in Path('/proc/meminfo').read_text().splitlines():
        if line.startswith('MemAvailable:'):
            return int(line.split()[1])
    raise RuntimeError('Cannot establish MemAvailable; refusing guest execution')


def check_resources(args):
    available = available_kib()
    if available < args.min_available_kib:
        raise RuntimeError(f'MemAvailable={available} KiB is below required '
                           f'{args.min_available_kib} KiB; no next command started')


def validate_execution_inputs(args, steps):
    if not args.base.is_dir() or not (args.base / 'iso_root').is_dir():
        raise ValueError('--base must contain the final iso_root directory')
    files = [args.base / 'initramfs.tar', args.iso, args.kernel, args.shell, args.gui,
             args.workload, args.vfork_workload, args.thread_check, args.ipc_check, args.telemetry_aml]
    files += [Path(command[1]) for _, _, command in steps]
    for path in files:
        if not path.is_file() or not path.stat().st_size:
            raise ValueError(f'Required input is missing or empty: {path}')
    if not args.guest_marker.parent.is_dir():
        raise ValueError('--guest-marker parent directory must already exist')
    if args.guest_marker.exists() or args.guest_marker.is_symlink():
        raise RuntimeError(f'Existing guest marker blocks execution: {args.guest_marker}')
    # A marker must never overwrite/remove a supplied input or a source script.
    if args.guest_marker in files or args.guest_marker == args.base or (args.base / 'iso_root') in args.guest_marker.parents:
        raise ValueError('--guest-marker overlaps a read-only input')
    if not shutil.which(args.iasl):
        raise ValueError(f'Explicit --iasl executable is unavailable: {args.iasl}')
    # The enriched fixture tree must contain the SAME full initramfs as the
    # supplied release ISO; otherwise a stale lean base could masquerade as
    # full-image rollback evidence. Inspection never modifies the source ISO.
    from test_power_release import verify_image, digest
    output = ROOT / 'build' / 'power-release-tests'
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='inspect-', dir=output) as temporary:
        provenance = verify_image(args, Path(temporary))
    if digest(args.base / 'initramfs.tar') != provenance['embedded']['initramfs.tar']['sha256']:
        raise ValueError('--base initramfs does not match the supplied full release ISO')
    record = {'inputs': provenance, 'base_initramfs': str(args.base / 'initramfs.tar'),
              'memory': args.memory, 'commands': [command for _, _, command in steps]}
    manifest = output / ('validation-plan-' + str(time.time_ns()) + '.json')
    manifest.write_text(json.dumps(record, indent=2) + '\n')
    print('Verified full-image input plan: ' + str(manifest), flush=True)


def execute_manifest(args, steps):
    validate_execution_inputs(args, steps)
    check_resources(args)
    marker = args.guest_marker
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, 'O_NOFOLLOW'):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(marker, flags, 0o600)
    identity = os.fstat(descriptor)
    try:
        with os.fdopen(descriptor, 'w') as stream:
            json.dump({'pid': os.getpid(), 'started_ns': time.time_ns(),
                       'memory': args.memory, 'min_available_kib': args.min_available_kib,
                       'commands': len(steps)}, stream)
            stream.write('\n')
        for index, (_, description, command) in enumerate(steps, 1):
            check_resources(args)
            print(f'Executing {index}/{len(steps)}: {description}', flush=True)
            subprocess.run(command, cwd=ROOT, check=True)
    finally:
        current = marker.lstat()
        if (current.st_dev, current.st_ino) != (identity.st_dev, identity.st_ino):
            raise RuntimeError('Guest marker ownership changed; refusing to remove it')
        marker.unlink()


def main():
    args = parse_arguments()
    steps = command_manifest(args)
    print_manifest(steps)
    if args.execute:
        execute_manifest(args, steps)


if __name__ == '__main__':
    main()
