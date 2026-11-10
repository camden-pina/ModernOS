#!/usr/bin/env python3
"""Build a bootable native Coast desktop from source on Linux or inside Coast.

This internal component supplies the kernel, desktop, account helpers and
native shell tools to the OS assembler and boot tests.
No existing ISO, compiled guest payload or private artifact directory is read.
"""
from build_state import project_lock
from build_state import atomic_write
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys

from fetch_sources import fetch, read_manifest
from source_archive import unpack
from source_tree import audit

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/native-image')
    parser.add_argument('--kernel', type=Path, help='reuse a kernel compiled in this source checkout')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    commands = []

    def source_identity():
        inventory = audit()
        names = [name for name in inventory if name.startswith(
            ('userland/', 'include/', 'arch/x86/include/', 'ports/linux-uapi/'))]
        names += [str(path.relative_to(ROOT)) for path in [Path(__file__),
            ROOT / 'tools/build_desktop_kernel.py', ROOT / 'tools/build_native_ports.py',
            ROOT / 'tools/mkinitramfs.py', ROOT / 'tools/build_limine.py',
            ROOT / 'tools/build_assembler.py', ROOT / 'tools/build_boot_iso.py',
            ROOT / 'tools/fetch_sources.py', ROOT / 'tools/source_archive.py']]
        return {name: inventory[name]['sha256'] for name in sorted(set(names))}

    sources_before = source_identity()

    def run(command, env=None, cwd=ROOT):
        command = list(map(str, command))
        commands.append(command)
        subprocess.run(command, cwd=cwd, env=env, check=True, stdin=subprocess.DEVNULL)

    kernel = args.kernel.resolve() if args.kernel else out / 'kernel/vmlinux'
    if not args.kernel:
        run([sys.executable, '-B', ROOT / 'tools/build_desktop_kernel.py',
             '--output-root', kernel.parent, '--jobs', args.jobs])
    run(['make', '-C', ROOT / 'userland', 'native', '-j' + str(args.jobs)])
    sources = read_manifest()
    fetch(sources['busybox-1.37.0.tar.bz2'])
    compiler = 'gcc'
    if platform.system() != 'Coast':
        fetch(sources['musl-1.2.5.tar.gz'])
        source = out / 'musl-1.2.5'
        if not source.exists():
            unpack(ROOT / 'userland/vendor/musl-1.2.5.tar.gz', out)
        prefix = out / 'musl'
        installed = prefix / '.coast-installed.json'
        if not installed.exists() or not all((prefix / name).is_file() for name in ('bin/musl-gcc', 'lib/libc.a', 'lib/crt1.o')):
            env = {**os.environ, 'CC': 'gcc', 'CFLAGS': '-O2 -std=gnu11 -fno-pie -mno-avx -mno-avx2'}
            run([source / 'configure', '--prefix=' + str(prefix), '--disable-shared'], env, source)
            run(['make', '-j' + str(args.jobs)], env, source)
            run(['make', 'install'], env, source)
            atomic_write(installed, json.dumps(dict(source_sha256=sources['musl-1.2.5.tar.gz']['sha256'], complete=True)) + '\n')
        compiler = str(prefix / 'bin/musl-gcc')
    environment = {**os.environ, 'COAST_PORTS_CC': compiler, 'COAST_BUILD_JOBS': str(args.jobs),
        'COAST_PORTS_BUILD': str(out / 'busybox-build'), 'COAST_PORTS_BIN': str(out / 'programs'),
        'COAST_PORTS_LICENSES': str(out / 'licenses')}
    (out / 'licenses').mkdir(exist_ok=True)
    run([sys.executable, '-B', ROOT / 'tools/build_native_ports.py', '--busybox-only'], environment)
    run([sys.executable, '-B', ROOT / 'tools/build_limine.py', '--compiler', 'gnu',
         '--output-root', out / 'limine', '--jobs', args.jobs])
    programs = out / 'programs'
    programs.mkdir(exist_ok=True)
    makefile = (ROOT / 'userland/Makefile').read_text()
    names = []
    for variable in ['PROGRAMS', 'TEXT_TOOLS', 'APPLETS']:
        names += re.search(r'^' + variable + r' := (.+)$', makefile, re.M)[1].split()
    for name in names:
        shutil.copyfile(ROOT / 'userland/bin' / name, programs / name)
    names.append('busybox')
    # Keep all image inputs tied to the explicit native target, even if a
    # previous wider build left other application binaries in userland/bin.
    for path in programs.iterdir():
        if path.name not in names:
            path.unlink()
    initramfs = out / 'initramfs.tar'
    runtime = out / 'runtime'
    license = runtime / 'usr/share/licenses/coast/LICENSE'
    license.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / 'LICENSE', license)
    run([sys.executable, '-B', ROOT / 'tools/mkinitramfs.py', initramfs, programs,
         '--font-license', ROOT / 'userland/vendor/licenses/dejavu-LICENSE.txt',
         '--native-licenses', ROOT / 'userland/vendor/licenses', '--runtime', runtime])
    image = out / 'Coast-native.iso'
    run([sys.executable, '-B', ROOT / 'tools/build_boot_iso.py', '--bootloader', 'limine',
         '--config', ROOT / 'arch/x86/boot/boot.conf', '--kernel', kernel, '--initramfs', initramfs,
         '--source-root', ROOT, '--root', out / 'iso-root', '--output', image,
         '--limine-dir', out / 'limine/output/bin'])
    if source_identity() != sources_before:
        raise ValueError('Native desktop sources changed during compilation')
    atomic_write(out / 'BUILD.json', json.dumps(dict(iso=str(image), iso_sha256=sha(image),
        initramfs=str(initramfs), initramfs_sha256=sha(initramfs),
        source_sha256=sources_before,
        kernel_sha256=sha(kernel), programs={n: sha(programs / n) for n in names},
        commands=commands, inherited_iso=False, chromium_included=False,
        state='source built; guest qualification separate'), indent=2) + '\n')
    print('Native Coast image:', image)


if __name__ == '__main__':
    with project_lock():
        main()
