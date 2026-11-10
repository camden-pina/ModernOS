#!/usr/bin/env python3
"""Publish the OS source kit and native rebuild tools in the local repository."""
import argparse
import json
from pathlib import Path

from repository_extension import extend
from source_tree import export, digest
from fetch_sources import read_manifest, fetch

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, default=ROOT / 'build/brine-repository-server/repository.json')
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--source-output', type=Path, default=ROOT / 'build/source-release/coast-source-2026.11.06.tar.xz')
    parser.add_argument('--nasm-build', type=Path, default=ROOT / 'build/nasm/BUILD.json')
    parser.add_argument('--wayland-build', type=Path, default=ROOT / 'build/wayland-bridge/BUILD.json')
    parser.add_argument('--image-tools-build', type=Path, default=ROOT / 'build/image-tools/BUILD.json')
    parser.add_argument('--signing-home', type=Path, default=ROOT / 'build/coast-signing-keys')
    parser.add_argument('--repo-add', type=Path, default=ROOT / 'build/coast-security-ports-r3/install/usr/bin/repo-add')
    args = parser.parse_args()
    source = args.source_output.resolve()
    record = export(source, kernel_sources=True)
    nasm = json.loads(args.nasm_build.read_text())
    if nasm['target'] != 'coast-x86_64' or nasm['target_host_executed'] is not False:
        parser.error('A completed Coast NASM cross-build is required')
    for asset in nasm['files'].values():
        if digest(Path(asset['path'])) != asset['sha256']:
            raise ValueError('NASM build output changed')
    wayland = json.loads(args.wayland_build.read_text())
    if wayland['target_host_executed'] is not False:
        parser.error('A completed Wayland cross-build is required')
    sdk = args.wayland_build.resolve().parent / 'sdk'
    staged = source.parent / 'wayland-sdk'
    staged.mkdir(parents=True, exist_ok=True)
    sdk_files = {}
    for path in sdk.rglob('*'):
        if not path.is_file() or path.is_symlink() or path.suffix == '.la':
            continue
        relative = str(path.relative_to(sdk))
        if not relative.startswith(('include/', 'lib/')):
            continue
        destination = path
        if path.suffix == '.pc':
            destination = staged / path.name
            destination.write_text(path.read_text().replace(str(sdk), '/usr/lib/coast/wayland-sdk'))
        sdk_files['usr/lib/coast/wayland-sdk/' + relative] = destination
    sdk_files['usr/share/licenses/wayland-devel/COPYING'] = ROOT / 'ports/wayland/COPYING'
    sdk_files['usr/share/licenses/wayland-devel/libffi-LICENSE'] = args.wayland_build.resolve().parent / 'libffi-3.5.2/LICENSE'
    wayland_sources = {'usr/src/coast/wayland/vendor/' + p.name: p
        for p in (args.wayland_build.resolve().parent / 'vendor').iterdir() if p.is_file()}
    wayland_sources.update({'usr/src/coast/wayland/protocol/' + p.name: p
        for p in (ROOT / 'ports/wayland').iterdir() if p.is_file()})
    image_tools = json.loads(args.image_tools_build.read_text())
    if image_tools['target_host_executed'] is not False:
        parser.error('A completed image-tools cross-build is required')
    for path, expected in image_tools['source_sha256'].items():
        if digest(ROOT / path) != expected:
            raise ValueError('Image-tool source changed after its build: ' + path)
    distfiles = []
    source_groups = {'coast-toolchain-source': {}, 'coast-ports-source': {}, 'coast-engine-source': {}}
    for name, item in read_manifest().items():
        fetch(item)
        path = ROOT / 'userland/vendor' / name
        distfiles.append(path)
        group = 'coast-toolchain-source' if name.startswith(('gcc-', 'binutils-', 'gmp-', 'mpfr-', 'mpc-', 'musl-')) else 'coast-ports-source'
        if name.startswith(('webkitgtk-', 'icu')):
            group = 'coast-engine-source'
        source_groups[group]['usr/src/coast/distfiles/' + name] = path
    headers = { 'usr/lib/coast/linux-uapi/' + str(p.relative_to(ROOT / 'ports/linux-uapi')): p
        for p in (ROOT / 'ports/linux-uapi').rglob('*') if p.is_file() }
    packages = [
        dict(name='coast-source', version='2026.11.06-1', description='Coast OS sources, patches and pinned offline kernel inputs',
             license='LicenseRef-Coast-Mixed', depends=[],
             files={'usr/src/coast/os/' + source.name: source,
                    'usr/src/coast/os/coast-source.tar.xz': source}),
        dict(name='nasm', version='2.16.03-1', description='Native x86 assembler and disassembler',
             url='https://www.nasm.us/', license='BSD-2-Clause', depends=['musl'],
             files={p: a['path'] for p, a in nasm['files'].items() if not p.startswith('usr/src/')}),
        dict(name='nasm-source', version='2.16.03-1', description='Exact NASM source and native PKGBUILD',
             url='https://www.nasm.us/', license='BSD-2-Clause', depends=[],
             files={**{p: a['path'] for p, a in nasm['files'].items() if p.startswith('usr/src/')},
                    'usr/src/coast/nasm/PKGBUILD': ROOT / 'userland/packages/recipes/nasm/PKGBUILD'}),
        dict(name='coast-build', version='2026.11.06-1', description='Native tools for rebuilding Coast kernel and userland sources',
             license='MIT', depends=['coast-source', 'base-devel', 'gcc', 'binutils', 'make',
                 'python', 'git', 'nasm', 'ninja', 'meson', 'pkgconf', 'm4', 'patch', 'gperf', 'xz', 'wayland-devel',
                 'coast-distfiles', 'coast-linux-uapi', 'xorriso', 'mtools'], files={}),
        dict(name='wayland-devel', version='1.21.0-1', description='Static Wayland server/client SDK and libffi for native Coast builds',
             license='MIT', depends=['musl'], files=sdk_files),
        dict(name='wayland-source', version='1.21.0-1', description='Exact Wayland/libffi sources and display protocol definitions',
             license='MIT', depends=[], files=wayland_sources),
        dict(name='coast-linux-uapi', version='7.1-1', description='Pinned Linux UAPI header sources for native port builds',
             license='LicenseRef-Linux-UAPI', depends=[], files=headers),
        dict(name='coast-distfiles', version='2026.11.06-1', description='Offline source inputs for Coast toolchain and native ports',
             license='LicenseRef-Coast-Mixed', depends=list(source_groups), files={}),
    ]
    for name, files in source_groups.items():
        packages.append(dict(name=name, version='2026.11.06-1', description='Verified upstream source archives for Coast rebuilds',
            license='LicenseRef-Coast-Mixed', depends=[], files=files))
    for name, package in image_tools['packages'].items():
        for asset in package['files'].values():
            if digest(Path(asset['path'])) != asset['sha256']:
                raise ValueError('Image-tool output changed: ' + name)
        packages.append(dict(name=name, version=package['version'] + '-' + str(package.get('release', 1)),
            description='Native ' + name + ' for building Coast boot images',
            license=package['license'], depends=['musl'], files={p:a['path'] for p,a in package['files'].items()}))
    recipe = dict(name='nasm', version='2.16.03-2', description=packages[1]['description'],
                  license='BSD-2-Clause', directory=ROOT / 'userland/packages/recipes/nasm', makedepends=['gcc', 'make', 'xz'])
    recipes = [recipe]
    for name, version in [('xorriso', '1.5.8.pl02-3'), ('mtools', '4.0.49-1'), ('limine', '12.9.0-1')]:
        recipes.append(dict(name=name, version=version, description='Build ' + name + ' inside Coast',
            license='GPL-3.0-or-later' if name != 'limine' else 'BSD-2-Clause',
            directory=ROOT / 'userland/packages/recipes' / name,
            makedepends=['gcc', 'binutils', 'make', 'coast-linux-uapi'] + (['nasm', 'mtools'] if name == 'limine' else [])))
    manifest = extend(args.repository, args.output_root, args.signing_home, args.repo_add, packages,
                      recipes=recipes, distfiles=[source, *distfiles, ROOT / 'ports/image-tools/xorriso-private-buffer.patch'])
    print(json.dumps(dict(repository=str(manifest), source_files=len(record['files']),
        source_sha256=digest(source), added=[p['name'] for p in packages],
        source_compiled_files=False, signing_secret_included=False), indent=2))


if __name__ == '__main__':
    main()
