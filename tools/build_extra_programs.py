#!/usr/bin/env python3
"""Cross-build additional upstream programs; never execute a target on the host."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / 'userland/vendor/extra-program-sources.json'
PACKAGES = json.loads(SOURCES.read_text())
OUTPUTS = {
    'ninja': {'ninja': 'ninja'},
    'pkgconf': {'pkgconf': 'pkgconf'},
    'm4': {'src/m4': 'm4'},
    'patch': {'src/patch': 'linux-patch'},
    'gperf': {'src/gperf': 'gperf'},
    'bc': {'bc/bc': 'bc', 'dc/dc': 'dc'},
    'zstd': {'programs/zstd': 'zstd'},
    'lz4': {'lz4': 'lz4'},
    'xz': {'src/xz/xz': 'linux-xz'},
    'rsync': {'rsync': 'rsync'},
}
ALIASES = {'pkg-config': 'pkgconf', 'unzstd': 'zstd', 'zstdcat': 'zstd',
           'zstdmt': 'zstd', 'unlz4': 'lz4', 'lz4cat': 'lz4',
           'linux-unxz': 'linux-xz', 'linux-xzcat': 'linux-xz'}


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def source_tree(path):
    return {str(p.relative_to(path)): sha(p) for p in sorted(path.rglob('*'))
            if p.is_file() and not p.is_symlink()}


def verify_base(path):
    record = json.loads(path.read_text())
    assert record['target_host_executed'] is False
    for name, asset in record['files'].items():
        assert not PurePosixPath(name).is_absolute() and '..' not in PurePosixPath(name).parts
        assert sha(asset['path']) == asset['sha256'], name
    for name, digest in record['source_sha256'].items():
        assert sha(ROOT / name) == digest, name
    for name, digest in record.get('dependencies', {}).items():
        assert sha(name) == digest, name
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/program-ports-r2')
    parser.add_argument('--base-build', type=Path, default=ROOT / 'build/program-ports-r1/BUILD.json')
    parser.add_argument('--package', choices=PACKAGES, action='append')
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--install-runtime', action='store_true')
    parser.add_argument('--compiler-build', type=Path, help='fresh source-built POSIX musl GCC receipt')
    args = parser.parse_args()
    assert args.jobs > 0
    base = verify_base(args.base_build)
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    vendor, stage = out / 'vendor', out / 'runtime'
    vendor.mkdir(exist_ok=True)
    stage.mkdir(exist_ok=True)
    compiler_receipt = args.compiler_build.resolve() if args.compiler_build else None
    if compiler_receipt:
        from build_desktop import verify_receipt
        compiler = verify_receipt(compiler_receipt)
        if compiler.get('thread_model') != 'posix' or base.get('compiler_build_sha256') != sha(compiler_receipt):
            raise ValueError('Extra programs and Python must use the same POSIX musl compiler')
        from linux_uapi import prepare
        headers = prepare(out / 'linux-uapi')
    else:
        headers = ROOT / 'build/qt6/port-r1/linux-uapi'
    cross = compiler_receipt.parent / 'cross/bin/x86_64-linux-musl-' if compiler_receipt else ROOT / 'build/compiler/cross/bin/x86_64-linux-musl-'
    flags = f'-O2 -g0 -std=gnu11 -fno-pie -fno-stack-protector -mno-avx -mno-avx2 -idirafter {headers}'
    env = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
               CC=str(cross) + 'gcc', CXX=str(cross) + 'g++', AR=str(cross) + 'ar',
               RANLIB=str(cross) + 'ranlib', STRIP=str(cross) + 'strip',
               CFLAGS=flags, CXXFLAGS=flags.replace(' -std=gnu11', ''), LDFLAGS='-static -no-pie',
               PKG_CONFIG_LIBDIR='/nonexistent', CONFIG_SITE='/dev/null')
    confinement = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session',
                   '--ro-bind', '/', '/', '--proc', '/proc', '--dev', '/dev',
                   '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    commands = []
    inputs = [Path(__file__), SOURCES, ROOT / 'userland/program-ports/extra-check.py']
    recipe = {str(p.relative_to(ROOT)): sha(p) for p in inputs}

    def run(name, command, cwd, source):
        command = list(map(str, command))
        commands.append(dict(name=name, command=command, cwd=str(cwd)))
        print(name, flush=True)
        with (out / (name + '.log')).open('wb') as log:
            subprocess.run([*confinement, '--ro-bind', str(source), str(source),
                            '--chdir', str(cwd), '--', *command],
                           env=env, stdin=subprocess.DEVNULL, stdout=log,
                           stderr=subprocess.STDOUT, check=True)

    def copy(path, relative):
        destination = stage / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, destination)

    compiled = {}
    for name, package in PACKAGES.items():
        if args.package and name not in args.package:
            continue
        archive = vendor / package['archive']
        if not archive.exists():
            pending = archive.with_suffix(archive.suffix + '.download')
            try:
                with urllib.request.urlopen(package['url'], timeout=90) as response, pending.open('wb') as stream:
                    shutil.copyfileobj(response, stream)
                assert sha(pending) == package['sha256'], name
                pending.replace(archive)
            finally:
                pending.unlink(missing_ok=True)
        assert sha(archive) == package['sha256'], name
        source, build = out / package['source'], out / (name + '-build')
        if not source.exists():
            with tarfile.open(archive) as bundle:
                bundle.extractall(out, filter='data')
        before = source_tree(source)
        build.mkdir(exist_ok=True)
        if name in ('ninja', 'zstd', 'lz4'):
            location = source if name == 'ninja' else source / 'build/cmake'
            options = ['cmake', '-S', location, '-B', build, '-G', 'Unix Makefiles',
                       '-DCMAKE_SYSTEM_NAME=Linux', '-DCMAKE_SYSTEM_PROCESSOR=x86_64',
                       '-DCMAKE_C_COMPILER=' + env['CC'], '-DCMAKE_CXX_COMPILER=' + env['CXX'],
                       '-DCMAKE_AR=' + env['AR'], '-DCMAKE_RANLIB=' + env['RANLIB'],
                       '-DCMAKE_C_FLAGS=' + flags, '-DCMAKE_CXX_FLAGS=' + env['CXXFLAGS'],
                       '-DCMAKE_EXE_LINKER_FLAGS=-static -no-pie',
                       '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_SKIP_RPATH=ON',
                       '-DBUILD_TESTING=OFF']
            if name == 'zstd':
                options += ['-DZSTD_BUILD_TESTS=OFF', '-DZSTD_BUILD_SHARED=OFF',
                            '-DZSTD_BUILD_STATIC=ON', '-DZSTD_PROGRAMS_LINK_SHARED=OFF',
                            '-DZSTD_MULTITHREAD_SUPPORT=ON', '-DZSTD_ZLIB_SUPPORT=OFF',
                            '-DZSTD_LZMA_SUPPORT=OFF', '-DZSTD_LZ4_SUPPORT=OFF']
            if name == 'lz4':
                options += ['-DBUILD_SHARED_LIBS=OFF', '-DBUILD_STATIC_LIBS=ON',
                            '-DLZ4_BUILD_CLI=ON', '-DLZ4_BUILD_MULTITHREAD=ON']
            targets = [dict(ninja='ninja', zstd='zstd', lz4='lz4cli')[name]]
        else:
            options = [source / 'configure', '--build=x86_64-pc-linux-gnu',
                       '--host=x86_64-linux-musl', '--prefix=/usr', '--disable-dependency-tracking']
            if name in ('m4', 'patch', 'xz'):
                options += ['--disable-nls']
            if name == 'pkgconf':
                options += ['--disable-shared', '--enable-static',
                            '--with-pkg-config-dir=/usr/lib/pkgconfig:/usr/share/pkgconfig',
                            '--with-personality-dir=/usr/share/pkgconfig/personality.d']
            if name == 'bc':
                options += ['--without-readline', '--without-libedit']
            if name == 'xz':
                options += ['--disable-shared', '--enable-static', '--disable-doc',
                            '--disable-scripts', '--disable-lzmadec', '--disable-lzmainfo',
                            '--disable-xzdec', '--enable-threads=posix']
            if name == 'rsync':
                options += ['--with-included-popt', '--with-included-zlib',
                            '--disable-md2man', '--disable-openssl', '--disable-xxhash',
                            '--disable-zstd', '--disable-lz4', '--disable-acl-support',
                            '--disable-xattr-support', '--disable-iconv', '--disable-iconv-open',
                            '--disable-idn', '--disable-roll-simd', '--disable-md5-asm',
                            '--disable-roll-asm', '--disable-openat2']
            targets = ['all']
        key = hashlib.sha256(json.dumps([list(map(str, options)), env]).encode()).hexdigest()
        stamp = build / '.configured'
        if not stamp.exists() or stamp.read_text() != key:
            run(name + '-configure', options, build, source)
            stamp.write_text(key)
        if name == 'bc':
            # Upstream ships the generated math table. Its regeneration runs a
            # target fbc-bootstrap, which must never be executed on the host.
            copy_header = build / 'bc/libmath.h'
            shutil.copyfile(source / 'bc/libmath.h', copy_header)
            run('bc-library', ['make', '-C', 'lib', '-j' + str(args.jobs), 'all'], build, source)
            run('bc-build', ['make', '-C', 'bc', '-j' + str(args.jobs), '-o', 'libmath.h', 'bc'], build, source)
            run('dc-build', ['make', '-C', 'dc', '-j' + str(args.jobs), 'dc'], build, source)
            assert not (build / 'bc/fbc-bootstrap').exists()
        else:
            run(name + '-build', ['make', '-j' + str(args.jobs), *targets], build, source)
        for relative, command in OUTPUTS[name].items():
            binary = build / relative
            elf = subprocess.check_output(['readelf', '-h', '-l', str(binary)], text=True)
            assert 'Advanced Micro Devices X86-64' in elf and 'INTERP' not in elf and ' RWE ' not in elf, binary
            copy(binary, 'usr/bin/' + command)
        assert source_tree(source) == before, 'Upstream sources changed: ' + name
        (out / (name + '-SOURCES.json')).write_text(json.dumps(before, indent=2) + '\n')
        copy(archive, 'usr/src/coast/extra-programs/' + archive.name)
        for path in sorted(source.rglob('*')):
            if path.is_file() and (path.name.startswith(('COPYING', 'LICENSE')) or path.name == 'Copyright'):
                copy(path, 'usr/share/licenses/extra-programs/' + name + '/' + str(path.relative_to(source)))
        compiled[name] = package
    for alias, target in ALIASES.items():
        if (stage / 'usr/bin' / target).is_file():
            link = stage / 'usr/bin' / alias
            if link.is_symlink():
                assert os.readlink(link) == target
            else:
                assert not link.exists(), link
                link.symlink_to(target)
    copy(inputs[-1], 'usr/share/coast/program-ports/extra-check.py')
    files = {str(p.relative_to(stage)): dict(path=str(p), sha256=sha(p))
             for p in sorted(stage.rglob('*')) if p.is_file() and not p.is_symlink()}
    aliases = {str(p.relative_to(stage)): os.readlink(p) for p in stage.rglob('*') if p.is_symlink()}
    assert not (files.keys() | aliases.keys()) & (base['files'].keys() | base['aliases'].keys())
    for path, digest in recipe.items():
        assert sha(ROOT / path) == digest, 'Recipe changed during compilation: ' + path
    record = dict(state='compiled; guest validation pending',
                  packages={**base['packages'], **{name: p for name, p in PACKAGES.items()
                            if all((stage / 'usr/bin' / command).is_file() for command in OUTPUTS[name].values())}},
                  files={**base['files'], **files}, aliases={**base['aliases'], **aliases},
                  dependencies=base.get('dependencies', {}),
                  source_sha256={**base['source_sha256'], **recipe},
                  base_build=dict(path=str(args.base_build.resolve()), sha256=sha(args.base_build)),
                  compiler_build_sha256=sha(compiler_receipt) if compiler_receipt else None,
                  upstream_source_inventories={name:dict(path=str(out / (name + '-SOURCES.json')),
                                                        sha256=sha(out / (name + '-SOURCES.json')))
                                              for name in compiled},
                  confinement=confinement, commands=commands, target_host_executed=False)
    (out / 'BUILD.json').write_text(json.dumps(record, indent=2) + '\n')
    if args.install_runtime:
        for relative, asset in record['files'].items():
            target = ROOT / 'userland/runtime' / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(asset['path'], target)
        for relative, link in record['aliases'].items():
            target = ROOT / 'userland/runtime' / relative
            if target.is_symlink():
                assert os.readlink(target) == link
            else:
                assert not target.exists(), target
                target.symlink_to(link)
    print('Additional programs staged:', stage, flush=True)


if __name__ == '__main__':
    main()
