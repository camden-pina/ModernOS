#!/usr/bin/env python3
"""Publish Coast-qualified cross-built LLVM and its complete offline source kit."""
import argparse
import json
from pathlib import Path
import posixpath

from publish_native_build_tools import checked_path, source_kit
from repository_extension import extend
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, required=True)
    parser.add_argument('--qualification', type=Path, required=True)
    parser.add_argument('--llvm-build', type=Path, required=True)
    parser.add_argument('--python-build', type=Path, help='the native-spawn Python fixture qualified by this LLVM report')
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--staging-root', type=Path, required=True)
    parser.add_argument('--signing-home', type=Path, default=ROOT / 'build/coast-signing-keys')
    parser.add_argument('--repo-add', type=Path, default=ROOT / 'build/coast-security-ports-r3/install/usr/bin/repo-add')
    args = parser.parse_args()
    proof_path = args.qualification.resolve(strict=True)
    proof = json.loads(proof_path.read_text())
    receipt = args.llvm_build.resolve(strict=True)
    build = json.loads(receipt.read_text())
    if (proof.get('state') != 'passed' or not proof.get('owned_vm_stopped') or proof.get('export_error') or
            proof.get('target_execution_on_linux') is not False or
            proof['fixtures']['llvm']['receipt_sha256'] != digest(receipt) or
            'qualify real C/C++, threads, subprocesses, LLD, ThinLTO and archive workflows' not in proof['checks'] or
            build['target_host_executed'] is not False or build['build_mode'] != 'linux-cross'):
        raise ValueError('A successful stopped Coast qualification of this exact cross-build is required')
    for name, expected in proof['exports'].items():
        if digest(checked_path(proof_path.parent, name)) != expected:
            raise ValueError('LLVM qualification artifact changed: ' + name)
    for source in [proof['source_files'], build['source_sha256']]:
        for name, expected in source.items():
            if digest(checked_path(ROOT, name)) != expected:
                raise ValueError('LLVM source changed: ' + name)
    baseline = json.loads(args.repository.read_text())
    if baseline['packages']['gcc']['version'] != '14.2.0-2' or 'cmake' not in baseline['packages']:
        parser.error('Select the qualified POSIX GCC/CMake package snapshot')
    files = {}
    for name, asset in build['files'].items():
        checked_path(Path('.'), name)
        source = Path(asset['path'])
        if digest(source) != asset['sha256']:
            raise ValueError('LLVM runtime changed: ' + name)
        files[name] = source
    for name in build['aliases']:
        checked_path(Path('.'), name)
        resolved, seen = name, set()
        while resolved in build['aliases']:
            if resolved in seen:
                raise ValueError('Cyclic LLVM runtime alias')
            seen.add(resolved)
            target = build['aliases'][resolved]
            checked_path(Path('.'), posixpath.normpath(posixpath.join(posixpath.dirname(resolved), target)))
            if target.startswith('/'):
                raise ValueError('Absolute LLVM runtime alias')
            resolved = posixpath.normpath(posixpath.join(posixpath.dirname(resolved), target))
        if resolved not in files:
            raise ValueError('Missing LLVM runtime alias target')
        files[name] = files[resolved]
    stage = args.staging_root.resolve()
    stage.mkdir(parents=True, exist_ok=False)
    prefix = 'coast-llvm-' + build['version']
    kit = stage / (prefix + '.tar.gz')
    kit_files = {name: checked_path(ROOT, name) for name in proof['source_files']}
    pins = [build['source'], json.loads((ROOT / 'userland/vendor/qemu-sources.json').read_text())['zlib']]
    for pin in pins:
        archive = receipt.parent / 'vendor' / pin['archive']
        if digest(archive) != pin['sha256']:
            raise ValueError('LLVM upstream source archive changed: ' + pin['archive'])
        kit_files['build/llvm/vendor/' + pin['archive']] = archive
    source_kit(kit, prefix, kit_files)
    packages = [dict(name='llvm', version=build['version'] + '-1',
        description='Static Clang, LLD and LLVM tools built from source and qualified inside Coast',
        license='Apache-2.0 WITH LLVM-exception AND Zlib', depends=['gcc>=14.2.0-2', 'binutils', 'musl'], files=files),
        dict(name='llvm-build', version=build['version'] + '-1',
        description='Offline LLVM and zlib sources with Coast compiler/linker recipes and checks',
        license='LicenseRef-Coast-Mixed', depends=['gcc>=14.2.0-2', 'binutils', 'cmake', 'make', 'ninja>=1.13.2-4', 'python', 'xz', 'patch', 'coast-linux-uapi'],
        files={'usr/src/coast/llvm-build/' + kit.name: kit})]
    replacements = {}
    python_evidence = None
    if args.python_build:
        python_path = args.python_build.resolve(strict=True)
        python = json.loads(python_path.read_text())
        if (proof.get('fixtures', {}).get('python', {}).get('receipt_sha256') != digest(python_path) or
                'native Python preserves parent state across 128 threaded subprocesses' not in proof['checks'] or
                python['target_host_executed'] is not False):
            raise ValueError('The report must qualify this exact Python fixture and its threaded spawn regression')
        for relative, expected in python['source_sha256'].items():
            if digest(checked_path(ROOT, relative)) != expected:
                raise ValueError('Python source changed: ' + relative)
        previous = baseline['packages']['python']
        python_files = {}
        for relative in previous['files']:
            asset = python['files'][relative]
            source = Path(asset['path'])
            if digest(source) != asset['sha256']:
                raise ValueError('Qualified Python runtime changed: ' + relative)
            python_files[relative] = source
        for relative in previous['aliases']:
            target = python['aliases'][relative]
            resolved = posixpath.normpath(posixpath.join(posixpath.dirname(relative), target))
            if resolved not in python_files:
                raise ValueError('Missing qualified Python alias target')
            python_files[relative] = python_files[resolved]
        python_files['usr/src/coast/program-ports/native-vfork.patch'] = ROOT / 'ports/python/native-vfork.patch'
        python_files['usr/share/coast/python/spawn_check.py'] = ROOT / 'ports/python/spawn_check.py'
        packages.append(dict(name='python', version=python['packages']['python']['version'] + '-2',
            description='CPython with bounded native vfork retry, qualified on threaded subprocesses inside Coast',
            license=previous['license'], provides=previous.get('provides', []), depends=['musl'], files=python_files))
        replacements['python'] = previous['version']
        python_evidence = dict(build_sha256=digest(python_path), build_mode='linux-cross',
            native_threaded_spawn_qualified=True, native_source_rebuild_qualified=False)
        packages[1]['depends'].remove('python')
        packages[1]['depends'].append('python>=3.14.10-2')
    recipe = stage / 'recipe'
    recipe.mkdir()
    (recipe / 'PKGBUILD').write_text('pkgname=llvm\npkgver=' + build['version'] + '\npkgrel=1\n'
        'pkgdesc="Source-built static Clang/LLD and LLVM tools for Coast"\narch=(x86_64)\n'
        "license=('Apache-2.0 WITH LLVM-exception' Zlib)\ndepends=('gcc>=14.2.0-2' binutils musl)\nmakedepends=(llvm-build)\n"
        'source=("${BRINE_REPOSITORY_URL:-http://10.0.2.2:8765}/distfiles/' + kit.name + '")\n'
        "sha256sums=('" + digest(kit) + "')\n"
        'build() {\n  cd "$srcdir/' + prefix + '"\n  python3 -B tools/build_llvm.py --native --jobs 4\n}\n'
        'check() {\n  cd "$srcdir/' + prefix + '"\n  python3 ports/llvm/check.py --runtime build/llvm-native/runtime --sysroot /\n}\n'
        'package() {\n  cd "$srcdir/' + prefix + '"\n  cp -a build/llvm-native/runtime/usr "$pkgdir/"\n}\n')
    (recipe / '.SRCINFO').write_text('pkgbase = llvm\n\tpkgdesc = Source-built static LLVM tools for Coast\n'
        '\tpkgver = ' + build['version'] + '\n\tpkgrel = 1\n\tarch = x86_64\n'
        '\tlicense = Apache-2.0 WITH LLVM-exception\n\tlicense = Zlib\n\tdepends = gcc>=14.2.0-2\n'
        '\tdepends = binutils\n\tdepends = musl\n\tmakedepends = llvm-build\n\tsource = ' + kit.name +
        '\n\tsha256sums = ' + digest(kit) + '\n\npkgname = llvm\n')
    manifest = extend(args.repository, args.output_root, args.signing_home, args.repo_add,
        packages, distfiles=[kit], replace_packages=replacements, recipes=[dict(name='llvm', version=build['version'] + '-1',
            description='Compile LLVM, LLD and zlib from original sources inside Coast',
            license='Apache-2.0 WITH LLVM-exception AND Zlib', directory=recipe,
            depends=['gcc>=14.2.0-2', 'binutils', 'musl'], makedepends=['llvm-build'])])
    (args.output_root / 'LLVM.json').write_text(json.dumps(dict(repository=str(manifest),
        build_sha256=digest(receipt), qualification_sha256=digest(proof_path), source_kit_sha256=digest(kit),
        build_mode='linux-cross', native_execution_qualified=True, native_source_rebuild_qualified=False,
        python=python_evidence,
        state='candidate; signed package installation qualification pending'), indent=2) + '\n')
    print('Coast-qualified LLVM published for installation testing:', manifest)


if __name__ == '__main__':
    main()
