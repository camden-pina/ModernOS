#!/usr/bin/env python3
"""Compile Limine BIOS/UEFI boot files from the checksum-pinned source release."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from fetch_sources import fetch, read_manifest
from source_archive import unpack

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/limine-source')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--compiler', choices=['llvm', 'gnu'], default='llvm',
                        help='GNU mode uses the GCC/binutils available inside Coast')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source_record = read_manifest()['limine-12.9.0.tar.xz']
    fetch(source_record)
    source = out / 'limine-12.9.0'
    if not source.exists():
        unpack(ROOT / 'userland/vendor' / source_record['archive'], out)
    build = out / 'output'
    build.mkdir(exist_ok=True)
    environment = {**os.environ, 'LC_ALL': 'C', 'SOURCE_DATE_EPOCH': '1793923200'}
    if args.compiler == 'gnu':
        environment.update(TOOLCHAIN_FOR_TARGET='', CFLAGS='-O2 -g0 -fno-pie', LDFLAGS='-static -no-pie')
    if not shutil.which('nasm'):
        host_nasm = ROOT / 'build/nasm/host/usr/bin/nasm'
        if not host_nasm.is_file():
            subprocess.run([sys.executable, '-B', str(ROOT / 'tools/build_assembler.py'),
                            '--host', '--jobs', str(args.jobs)], check=True)
        environment['PATH'] = str(host_nasm.parent) + ':' + environment['PATH']
    configure = [source / 'configure', '--enable-bios', '--enable-bios-cd',
                 '--enable-uefi-x86-64', '--enable-uefi-cd']
    if args.compiler == 'gnu':
        configure += ['--build=x86_64-linux-musl', '--host=x86_64-linux-musl']
    commands = [configure, ['make', '-j' + str(args.jobs)]]
    for index, command in enumerate(commands):
        with (out / f'{index}-build.log').open('w') as log:
            subprocess.run(list(map(str, command)), cwd=build, env=environment,
                           stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, check=True)
    products = {}
    for name in ['limine', 'limine-bios.sys', 'limine-bios-cd.bin', 'limine-uefi-cd.bin', 'BOOTX64.EFI']:
        path = build / 'bin' / name
        if not path.is_file():
            raise ValueError('Missing source-built boot file: ' + str(path))
        products[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    (out / 'BUILD.json').write_text(json.dumps(dict(source=source_record, products=products, compiler=args.compiler,
                                                   source_built=True), indent=2) + '\n')
    print('Source-built bootloader:', build / 'bin')


if __name__ == '__main__':
    main()
