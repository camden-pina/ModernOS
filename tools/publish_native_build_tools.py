#!/usr/bin/env python3
"""Publish qualified native CMake/GN, offline sources and the POSIX GCC SDK.

The GCC package uses the relocatable, Linux-cross-built /usr SDK exercised by
the qualification. The separately native-built compiler has an absolute build
prefix and is evidence only; it is never installed as a relocated compiler.
"""
import argparse
import gzip
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile

from repository_base import owner
from repository_extension import extend
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]


def checked_path(root, relative):
    path = PurePosixPath(relative)
    if path.is_absolute() or '..' in path.parts or str(path) != relative:
        raise ValueError('Unsafe source/package path: ' + relative)
    return root / relative


def source_kit(destination, prefix, files):
    with destination.open('wb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', mtime=0, filename='') as stream, \
            tarfile.open(fileobj=stream, mode='w|', format=tarfile.PAX_FORMAT) as bundle:
        directories = set()
        for relative, source in sorted(files.items()):
            target = str(checked_path(Path(prefix), relative))
            for parent in reversed(PurePosixPath(target).parents):
                name = str(parent)
                if name != '.' and name not in directories:
                    info = tarfile.TarInfo(name)
                    info.type, info.mode = tarfile.DIRTYPE, 0o755
                    bundle.addfile(info)
                    directories.add(name)
            info = tarfile.TarInfo(target)
            info.mode, info.size = 0o644, source.stat().st_size
            with source.open('rb') as data:
                bundle.addfile(info, data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, required=True)
    parser.add_argument('--qualification', type=Path, required=True)
    parser.add_argument('--gcc-build', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--staging-root', type=Path, required=True)
    parser.add_argument('--signing-home', type=Path, default=ROOT / 'build/coast-signing-keys')
    parser.add_argument('--repo-add', type=Path, default=ROOT / 'build/coast-security-ports-r3/install/usr/bin/repo-add')
    args = parser.parse_args()
    proof = args.qualification.resolve(strict=True)
    report_path = proof / 'report.json'
    report = json.loads(report_path.read_text())
    required_checks = {
        'compile and run C++ mutex, condition variable, TLS, exception, future and shared-lock checks',
        'qualify the newly native-built CMake and CTest',
        'qualify newly native-built GN, Python helpers and Ninja C++ consumers',
        'archive the native-built CMake and GN runtimes'}
    if (report.get('state') != 'passed' or report.get('target_execution_on_linux') is not False or
            not report.get('owned_vm_stopped') or not report.get('native_rebuild') or
            not required_checks.issubset(report.get('checks', []))):
        raise ValueError('A successful, stopped native compiler/CMake/GN qualification is required')
    for name, expected in report['exports'].items():
        if digest(checked_path(proof, name)) != expected:
            raise ValueError('Qualified native export changed: ' + name)
    for name, expected in report['source_files'].items():
        if digest(checked_path(ROOT, name)) != expected:
            raise ValueError('Qualified source changed: ' + name)
    gcc_path = args.gcc_build.resolve(strict=True)
    gcc = json.loads(gcc_path.read_text())
    if (report['fixtures']['gcc']['receipt_sha256'] != digest(gcc_path) or
            gcc.get('target_host_executed') is not False or gcc.get('thread_model') != 'posix'):
        raise ValueError('Select the same relocatable POSIX GCC SDK qualified in Coast')
    for name, expected in gcc['source_sha256'].items():
        if digest(checked_path(ROOT, name)) != expected:
            raise ValueError('GCC source changed: ' + name)
    stage = args.staging_root.resolve()
    stage.mkdir(parents=True, exist_ok=False)
    baseline_path = args.repository.resolve(strict=True)
    baseline = json.loads(baseline_path.read_text())
    packages, recipes, distfiles, evidence = [], [], [], {}
    for name in ('cmake', 'gn'):
        receipt_path = proof / ('native-' + name + '.json')
        archive_path = proof / ('native-' + name + '-runtime.tar.gz')
        if not {receipt_path.name, archive_path.name}.issubset(report['exports']):
            raise ValueError('Missing qualified native artifacts: ' + name)
        build = json.loads(receipt_path.read_text())
        if build.get('build_mode') != 'native-coast' or build.get('target_host_executed') is not True:
            raise ValueError('Native source compilation is required: ' + name)
        for relative, expected in build['source_sha256'].items():
            if digest(checked_path(ROOT, relative)) != expected:
                raise ValueError('Native builder source changed: ' + relative)
        runtime = stage / (name + '-runtime')
        runtime.mkdir()
        with tarfile.open(archive_path) as bundle:
            bundle.extractall(runtime, filter='data')
        for relative, asset in build['files'].items():
            if digest(checked_path(runtime, relative)) != asset['sha256']:
                raise ValueError('Native runtime changed: ' + relative)
        record = build['source']
        upstream = ROOT / 'build/build-tools/vendor' / record['archive']
        if digest(upstream) != record['sha256']:
            raise ValueError('Upstream source changed: ' + record['archive'])
        prefix = 'coast-' + name + '-' + build['version']
        kit = stage / (prefix + '.tar.gz')
        kit_files = {relative: checked_path(ROOT, relative) for relative in report['source_files']}
        kit_files['build/build-tools/vendor/' + record['archive']] = upstream
        source_kit(kit, prefix, kit_files)
        distfiles.append(kit)
        packages.append(dict(name=name, version=build['version'] + '-1',
            description=name.upper() + ' compiled and qualified inside Coast', license=record['license'],
            depends=['musl'], files={p: runtime / p for p in build['files']}))
        packages.append(dict(name=name + '-build', version=build['version'] + '-1',
            description='Complete offline source rebuild kit for ' + name, license='LicenseRef-Coast-Mixed',
            depends=['gcc>=14.2.0-2', 'binutils', 'make', 'python', 'xz', 'patch', 'ninja>=1.13.2-4', 'coast-linux-uapi'],
            files={'usr/src/coast/' + name + '-build/' + kit.name: kit}))
        recipe = stage / (name + '-recipe')
        recipe.mkdir()
        check = ('python3 ports/build-tools/cmake_check.py --runtime build/cmake-native/runtime --ninja /usr/bin/ninja'
                 if name == 'cmake' else
                 'python3 ports/build-tools/gn_check.py --gn build/gn-native/runtime/usr/bin/gn --ninja /usr/bin/ninja')
        (recipe / 'PKGBUILD').write_text('pkgname=' + name + '\npkgver=' + build['version'] + '\npkgrel=1\n'
            'pkgdesc="Native ' + name.upper() + ' for Coast"\narch=(x86_64)\nlicense=(' + record['license'] + ')\n'
            'depends=(musl)\nmakedepends=(' + name + '-build)\n'
            'source=("${BRINE_REPOSITORY_URL:-http://10.0.2.2:8765}/distfiles/' + kit.name + '")\n'
            "sha256sums=('" + digest(kit) + "')\n"
            'build() {\n  cd "$srcdir/' + prefix + '"\n  python3 -B tools/build_' + name + '.py --native --jobs 4\n}\n'
            'check() {\n  cd "$srcdir/' + prefix + '"\n  ' + check + '\n}\n'
            'package() {\n  cd "$srcdir/' + prefix + '"\n  cp -a build/' + name + '-native/runtime/usr "$pkgdir/"\n}\n')
        (recipe / '.SRCINFO').write_text('pkgbase = ' + name + '\n\tpkgdesc = Native ' + name.upper() + '\n'
            '\tpkgver = ' + build['version'] + '\n\tpkgrel = 1\n\tarch = x86_64\n'
            '\tlicense = ' + record['license'] + '\n\tdepends = musl\n\tmakedepends = ' + name + '-build\n'
            '\tsource = ' + kit.name + '\n\tsha256sums = ' + digest(kit) + '\n\npkgname = ' + name + '\n')
        recipes.append(dict(name=name, version=build['version'] + '-1',
            description='Build ' + name.upper() + ' from original sources inside Coast', license=record['license'],
            directory=recipe, depends=['musl'], makedepends=[name + '-build']))
        evidence[name] = dict(native_build_sha256=digest(receipt_path), source_kit_sha256=digest(kit))

    # Preserve all existing GCC-owned SDK helpers and headers, then replace
    # only GCC's files. Binutils and musl retain their own package ownership.
    previous = baseline['packages']['gcc']
    route = '/repo/x86_64/gcc-' + previous['version'] + '-x86_64.pkg.tar.gz'
    old = baseline['host_files'][route]
    old_archive = baseline_path.parent / old['path']
    if digest(old_archive) != old['sha256'] or previous['aliases']:
        raise ValueError('Unexpected baseline GCC package bytes or aliases')
    gcc_root = stage / 'gcc-runtime'
    gcc_root.mkdir()
    gcc_files = {}
    with tarfile.open(old_archive) as bundle:
        for relative in previous['files']:
            member = bundle.getmember(relative)
            if not member.isfile():
                raise ValueError('Expected a regular GCC package file: ' + relative)
            target = checked_path(gcc_root, relative)
            target.parent.mkdir(parents=True, exist_ok=True)
            with bundle.extractfile(member) as data, target.open('wb') as output:
                shutil.copyfileobj(data, output)
            target.chmod(member.mode & 0o777)
            gcc_files[relative] = target
    for relative, asset in gcc['files'].items():
        if owner(relative) != 'gcc':
            continue
        if relative not in gcc_files:
            raise ValueError('Unexpected new GCC ownership: ' + relative)
        source = Path(asset['path'])
        if digest(source) != asset['sha256']:
            raise ValueError('Qualified GCC SDK file changed: ' + relative)
        target = checked_path(gcc_root, relative)
        shutil.copyfile(source, target)
        target.chmod(asset['mode'])
    for filename in ('bootstrap-toolchain.sh', 'prepare-gcc-options.sh'):
        relative = 'usr/lib/coast/toolchain/' + filename
        gcc_files[relative] = ROOT / 'userland/sdk' / filename
    wrapper = stage / 'bootstrap-toolchain'
    wrapper.write_text('#!/bin/sh\nexec build-sh /usr/lib/coast/toolchain/bootstrap-toolchain.sh "$@"\n')
    wrapper.chmod(0o755)
    for relative in ('usr/bin/bootstrap-toolchain', 'bin/bootstrap-toolchain'):
        gcc_files[relative] = wrapper
    packages.append(dict(name='gcc', version=gcc['version'] + '-2',
        description='GNU C/C++ compiler with POSIX C++ threads, qualified inside Coast',
        license=previous['license'], provides=previous.get('provides', []), depends=['musl', 'binutils'], files=gcc_files))
    evidence['gcc'] = dict(build_sha256=digest(gcc_path), build_mode='linux-cross', thread_model='posix',
        qualified_fixture=report['fixtures']['gcc'], relocated_native_compiler=False)
    manifest = extend(args.repository, args.output_root, args.signing_home, args.repo_add,
        packages, recipes=recipes, distfiles=distfiles, replace_packages={'gcc': previous['version']})
    (args.output_root / 'BUILD-TOOLS.json').write_text(json.dumps(dict(
        repository=str(manifest), qualification_sha256=digest(report_path), evidence=evidence,
        source_only_kits=True, signing_secret_included=False,
        state='candidate; signed package installation qualification pending'), indent=2) + '\n')
    print('Native build tools published for installation qualification:', manifest)


if __name__ == '__main__':
    main()
