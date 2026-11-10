#!/usr/bin/env python3
"""Version the current complete OS source kit in a new signed repository snapshot."""
import argparse
import json
from pathlib import Path
import re

from repository_extension import extend
from source_tree import digest, export

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, default=ROOT / 'build/brine-repository-server/repository.json')
    parser.add_argument('--version', required=True, help='dated pacman version, for example 2026.11.07-1')
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--source-output', type=Path, required=True)
    parser.add_argument('--signing-home', type=Path, default=ROOT / 'build/coast-signing-keys')
    parser.add_argument('--repo-add', type=Path, default=ROOT / 'build/coast-security-ports-r3/install/usr/bin/repo-add')
    args = parser.parse_args()
    if not re.fullmatch(r'20\d{2}\.\d{2}\.\d{2}-[1-9]\d*', args.version):
        parser.error('--version must use YYYY.MM.DD-release')
    source = args.source_output.resolve()
    if source.name != 'coast-source-' + args.version + '.tar.xz' or source.exists():
        parser.error('Use a new source archive named coast-source-VERSION.tar.xz')
    baseline = json.loads(args.repository.read_text())
    for name in ['coast-source', 'coast-build', 'bison', 'grep']:
        if name not in baseline['packages']:
            parser.error('Select a snapshot containing the existing native build closure')
    inventory = export(source, kernel_sources=True)
    build_dependencies = ['coast-source', 'base-devel', 'gcc', 'binutils', 'make', 'python', 'git',
        'nasm', 'ninja>=1.13.2-4', 'meson', 'pkgconf', 'm4', 'patch', 'gperf', 'xz', 'wayland-devel',
        'coast-distfiles', 'coast-linux-uapi', 'xorriso', 'mtools', 'qemu-build', 'coast-vm', 'bison', 'grep']
    # Include the qualified ports already present in this particular snapshot;
    # a source-kit update must not invent unavailable binary dependencies.
    for name in ['cmake', 'cmake-build', 'gn', 'gn-build', 'llvm', 'llvm-build']:
        if name in baseline['packages']:
            build_dependencies.append(name)
    for name in ['gcc', 'python']:
        build_dependencies[build_dependencies.index(name)] = name + '>=' + baseline['packages'][name]['version']
    packages = [dict(name='coast-source', version=args.version,
        description='Coast OS, native emulator and build-tool sources with offline kernel inputs',
        license='LicenseRef-Coast-Mixed', depends=[],
        files={'usr/src/coast/os/coast-source.tar.xz': source}),
        dict(name='coast-build', version=args.version,
        description='Native Coast OS and nested QEMU build dependencies', license='MIT', files={},
        depends=build_dependencies)]
    replacement = {p['name']: baseline['packages'][p['name']]['version'] for p in packages}
    manifest = extend(args.repository, args.output_root, args.signing_home, args.repo_add,
        packages, replace_packages=replacement, distfiles=[source])
    (args.output_root / 'OS-SOURCE.json').write_text(json.dumps(dict(version=args.version,
        source_sha256=digest(source), source_files=len(inventory['files']),
        source_compiled_files=False, offline_kernel_sources=True,
        installed_archive='usr/src/coast/os/coast-source.tar.xz',
        repository_manifest_sha256=digest(manifest), guest_qualification_pending=True), indent=2) + '\n')
    print('OS source candidate:', manifest, flush=True)


if __name__ == '__main__':
    main()
