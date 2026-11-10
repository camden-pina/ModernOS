#!/usr/bin/env python3
"""Build the Chromium desktop kernel with the checked-in Coast ABI profile."""
from build_state import project_lock
from build_state import atomic_write
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess

from electron_admission import prepare as prepare_admission
from source_tree import audit, digest

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/desktop-kernel')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--verify-sandbox', action='store_true')
    parser.add_argument('--runtime-build', type=Path,
        help='admit the exact verified source-built Electron/Node pair in this kernel')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    def source_identity():
        inventory = audit()
        names = [name for name in inventory if name in ('Makefile', 'Kbuild') or
            name.startswith(('arch/', 'drivers/', 'fs/', 'include/', 'kernel/', 'lib/', 'mm/', 'net/', 'scripts/'))]
        names += ['tools/build_desktop_kernel.py', 'tools/electron_admission.py',
                  'tools/fetch_sources.py', 'tools/source_tree.py',
                  'userland/vendor/sources.json', 'ports/chromium/kernel-flags.json']
        return {name: inventory[name]['sha256'] for name in sorted(set(names))}

    sources = source_identity()
    flags = json.loads((ROOT / 'ports/chromium/kernel-flags.json').read_text())
    if args.verify_sandbox:
        flags.update(USERNS_VERIFY=True, PID_NAMESPACE_VERIFY=True, FILESYSTEM_SANDBOX_VERIFY=True)
    extra = []
    if args.runtime_build:
        header = prepare_admission(args.runtime_build, args.output_root / 'electron-admission.h')
        extra = ['KCPPFLAGS=' + os.environ.get('KCPPFLAGS', '') + ' -include ' + shlex.quote(str(header))]
    command = ['make', '-C', str(ROOT), 'O=' + str(args.output_root.resolve()),
                    '-j' + str(args.jobs), *[f'{key}={int(value)}' for key, value in flags.items()],
                    *extra, 'vmlinux']
    subprocess.run(command, check=True)
    if source_identity() != sources:
        raise ValueError('Kernel sources changed during compilation')
    admission = args.output_root / 'electron-admission.json'
    atomic_write(args.output_root / 'KERNEL.json', json.dumps(dict(
        kernel_sha256=digest(args.output_root / 'vmlinux'), command=command, flags=flags,
        source_profile_sha256=digest(ROOT / 'ports/chromium/kernel-flags.json'),
        runtime_build_sha256=digest(args.runtime_build) if args.runtime_build else None,
        admission_sha256=digest(admission) if args.runtime_build else None,
        source_sha256=sources,
        builder_sha256=digest(Path(__file__))), indent=2) + '\n')


if __name__ == '__main__':
    with project_lock():
        main()
