#!/usr/bin/env python3
"""Cross-build pinned CPython and jq without running target code on the host."""
from source_archive import unpack, unpack_zip
from build_state import project_lock
from build_state import atomic_write
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request
import zipfile
import sys
from fetch_sources import fetch

ROOT = Path(__file__).resolve().parents[1]
PACKAGES = {
    'python': dict(version='3.14.10', archive='Python-3.14.10.tar.xz',
                   url='https://www.python.org/ftp/python/3.14.10/Python-3.14.10.tar.xz',
                   sha256='de414a57119abef52784d8993e341d53aed635fc2b05c2607d9f98a2c5b72032',
                   source='Python-3.14.10',
                   provenance='https://www.python.org/downloads/release/python-31410/'),
    'jq': dict(version='1.8.2', archive='jq-1.8.2.tar.gz',
               url='https://github.com/jqlang/jq/releases/download/jq-1.8.2/jq-1.8.2.tar.gz',
               sha256='71b8d6e8f5fe81f6c6d0d110e3892251f6ce76ed095abd315e26e6e1193af3af',
               source='jq-1.8.2',
               provenance='https://github.com/jqlang/jq/releases/expanded_assets/jq-1.8.2'),
}


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/program-ports-r1')
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--package', choices=PACKAGES)
    parser.add_argument('--install-runtime', action='store_true')
    parser.add_argument('--compiler-build', type=Path)
    parser.add_argument('--dependencies-build', type=Path,
                        help='fresh package dependency receipt providing threaded OpenSSL and zlib')
    parser.add_argument('--bootstrap-host-python', action='store_true',
                        help='build the pinned host interpreter even when the system minor version matches')
    args = parser.parse_args()
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    vendor = out / 'vendor'
    vendor.mkdir(exist_ok=True)
    stage = out / 'runtime'
    stage.mkdir(exist_ok=True)
    inputs = [Path(__file__), ROOT / 'userland/program-ports/check.py', ROOT / 'ports/python/native-vfork.patch',
              ROOT / 'tools/linux_uapi.py', ROOT / 'tools/fetch_sources.py', ROOT / 'userland/vendor/linux-ports-sources.json']
    original = {str(p.relative_to(ROOT)): sha(p) for p in inputs}
    if bool(args.compiler_build) != bool(args.dependencies_build):
        parser.error('--compiler-build and --dependencies-build must be supplied together')
    compiler_receipt = args.compiler_build.resolve() if args.compiler_build else None
    dependency_receipt = args.dependencies_build.resolve() if args.dependencies_build else None
    if compiler_receipt:
        from build_desktop import verify_receipt
        compiler = verify_receipt(compiler_receipt)
        dependencies_record = verify_receipt(dependency_receipt)
        if compiler.get('thread_model') != 'posix':
            parser.error('Python requires the POSIX musl compiler')
        for path, checksum in dependencies_record['artifacts'].items():
            if sha(Path(path)) != checksum:
                raise ValueError('Dependency changed: ' + path)
    identity = dict(source_sha256=original,
                    compiler_build_sha256=sha(compiler_receipt) if compiler_receipt else None,
                    dependency_build_sha256=sha(dependency_receipt) if dependency_receipt else None,
                    package=args.package, bootstrap_host_python=args.bootstrap_host_python)
    configuration = out / 'CONFIGURATION.json'
    if configuration.exists() and json.loads(configuration.read_text()) != identity:
        raise ValueError('Program build inputs changed; choose a fresh output directory')
    atomic_write(configuration, json.dumps(identity, indent=2) + '\n')
    cross = compiler_receipt.parent / 'cross/bin/x86_64-linux-musl-' if compiler_receipt else ROOT / 'build/compiler/cross/bin/x86_64-linux-musl-'
    assert Path(str(cross) + 'gcc').is_file(), 'Existing Coast musl toolchain required'
    web = Path(dependencies_record['prefix']) if dependency_receipt else ROOT / 'build/web/deps'
    tls = web if dependency_receipt else out / 'openssl-prefix'
    sqlite = out / 'sqlite-prefix' if dependency_receipt else ROOT / 'userland/runtime/usr/lib/coast/linux-ports/sdk'
    from linux_uapi import prepare
    headers = prepare(out / 'linux-uapi') if compiler_receipt else ROOT / 'build/qt6/port-r1/linux-uapi'
    dependencies = [web / 'lib/libz.a', tls / 'lib/libssl.a', tls / 'lib/libcrypto.a', sqlite / 'lib/libsqlite3.a']
    for dependency in [web / 'lib/libz.a', *([] if dependency_receipt else [sqlite / 'lib/libsqlite3.a'])]:
        assert dependency.is_file(), dependency
    flags = f'-O2 -g0 -std=gnu11 -fno-pie -fno-stack-protector -mno-avx -mno-avx2 -idirafter {headers}'
    env = dict(PATH='/usr/bin:/bin', HOME='/tmp', LANG='C.UTF-8', TMPDIR='/tmp',
               CC=str(cross) + 'gcc', AR=str(cross) + 'ar', RANLIB=str(cross) + 'ranlib',
               CFLAGS=flags, LDFLAGS='-static -no-pie',
               PKG_CONFIG_LIBDIR='/nonexistent', CONFIG_SITE='/dev/null')
    conf = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session',
            '--ro-bind', '/', '/', '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
            '--bind', str(out), str(out)]
    commands = []

    def run(name, command, where, source, extra=None):
        command = list(map(str, command))
        commands.append(dict(name=name, command=command, cwd=str(where)))
        print(name, flush=True)
        with (out / (name + '.log')).open('wb') as log:
            subprocess.run([*conf, '--ro-bind', str(source), str(source), '--chdir', str(where), '--', *command],
                           env={**env, **(extra or {})}, stdin=subprocess.DEVNULL,
                           stdout=log, stderr=subprocess.STDOUT, check=True)

    def copy(source, relative):
        target = stage / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)

    if dependency_receipt:
        pin = next(p for p in json.loads((ROOT / 'userland/vendor/linux-ports-sources.json').read_text())['packages'] if p['name'] == 'sqlite')
        fetch(pin, vendor)
        source = out / 'sqlite-amalgamation-3540000'
        if not source.exists():
            unpack_zip(vendor / pin['archive'], out)
        (sqlite / 'lib').mkdir(parents=True, exist_ok=True)
        (sqlite / 'include').mkdir(exist_ok=True)
        if not (sqlite / 'lib/libsqlite3.a').exists():
            run('sqlite-build', [env['CC'], *flags.split(), '-DSQLITE_THREADSAFE=1', '-DSQLITE_OMIT_LOAD_EXTENSION',
                                '-c', source / 'sqlite3.c', '-o', out / 'sqlite3.o'], out, source)
            run('sqlite-archive', [env['AR'], 'rcs', sqlite / 'lib/libsqlite3.a', out / 'sqlite3.o'], out, source)
            shutil.copy2(source / 'sqlite3.h', sqlite / 'include/sqlite3.h')
        copy(vendor / pin['archive'], 'usr/src/coast/program-ports/' + pin['archive'])

    # Python requires genuinely thread-safe OpenSSL. The older browser prefix
    # deliberately uses OpenSSL's no-threads profile and cannot be reused here.
    ssl_archive = dependency_receipt.parent / 'vendor/openssl-3.5.9-source.tar.gz' if dependency_receipt else ROOT / 'userland/vendor/openssl-3.5.9-source.tar.gz'
    assert sha(ssl_archive) == '796831ec533bf41998eaaee9d632d921b8a69f57394fa4086ff01a941113d26d'
    ssl_source = out / 'openssl-openssl-3.5.9'
    if not dependency_receipt and not ssl_source.exists():
        unpack(ssl_archive, out, filter='data')
    ssl_build = out / 'openssl-build'
    ssl_build.mkdir(exist_ok=True)
    thread_config = tls / 'include/openssl/configuration.h'
    ssl_options = ['perl', ssl_source / 'Configure', 'linux-x86_64', 'threads',
                                 'no-shared', 'no-module', 'no-tests', 'no-asm', 'no-engine',
                                 '--with-rand-seed=getrandom', '--prefix=/usr', '--openssldir=/etc/ssl', '--libdir=lib']
    ssl_key = hashlib.sha256(json.dumps([list(map(str, ssl_options)), flags, sha(ssl_archive)]).encode()).hexdigest()
    if not dependency_receipt and (not (tls / '.complete').exists() or (tls / '.complete').read_text() != ssl_key):
        run('openssl-configure', ssl_options, ssl_build, ssl_source,
            dict(LDFLAGS='-no-pie'))
        # OpenSSL interprets a global -static linker flag as no-threads. Build
        # static archives with no-shared, then link the interpreter statically.
        run('openssl-build', ['make', '-j' + str(args.jobs), 'build_libs'], ssl_build, ssl_source,
            dict(LDFLAGS='-no-pie'))
        (tls / 'lib').mkdir(parents=True, exist_ok=True)
        (tls / 'include/openssl').mkdir(parents=True, exist_ok=True)
        for library in ['libssl.a', 'libcrypto.a']:
            shutil.copy2(ssl_build / library, tls / 'lib' / library)
        for directory in [ssl_source / 'include/openssl', ssl_build / 'include/openssl']:
            for header in directory.glob('*.h'):
                shutil.copy2(header, tls / 'include/openssl' / header.name)
        (tls / '.complete').write_text(ssl_key)
    assert 'OPENSSL_THREADS' in thread_config.read_text(), 'Thread-safe OpenSSL required'
    copy((dependency_receipt.parent / 'openssl-openssl-3.5.9' if dependency_receipt else ssl_source) / 'LICENSE.txt', 'usr/share/licenses/program-openssl/LICENSE.txt')
    copy(ssl_archive, 'usr/src/coast/program-ports/' + ssl_archive.name)
    public = out / 'dependency-headers'
    public.mkdir(exist_ok=True)
    for header in [web / 'include/zlib.h', web / 'include/zconf.h', sqlite / 'include/sqlite3.h']:
        shutil.copy2(header, public / header.name)

    for name, package in PACKAGES.items():
        if args.package and args.package != name:
            continue
        archive = vendor / package['archive']
        if not archive.is_file():
            pending = archive.with_suffix(archive.suffix + '.download')
            try:
                with urllib.request.urlopen(package['url'], timeout=90) as response, pending.open('wb') as stream:
                    shutil.copyfileobj(response, stream)
                assert sha(pending) == package['sha256'], 'Source checksum mismatch'
                pending.replace(archive)
            finally:
                pending.unlink(missing_ok=True)
        assert sha(archive) == package['sha256'], archive
        source = out / package['source']
        if not source.exists():
            unpack(archive, out, filter='data')
        if name == 'python':
            patch = ROOT / 'ports/python/native-vfork.patch'
            stamp = source / '.coast-native-vfork'
            if not stamp.exists():
                subprocess.run(['patch', '-p1', '-i', str(patch)], cwd=source, check=True)
                stamp.write_text(sha(patch))
            if stamp.read_text() != sha(patch):
                raise ValueError('Python spawn patch changed; select a fresh source directory')
        build = out / (name + '-build')
        build.mkdir(exist_ok=True)
        if name == 'python':
            build_python = Path(sys.executable)
            if args.bootstrap_host_python or sys.version_info[:2] != (3, 14):
                host = out / 'python-host'
                host.mkdir(exist_ok=True)
                host_env = dict(CC='/usr/bin/gcc', AR='/usr/bin/ar', RANLIB='/usr/bin/ranlib',
                                CFLAGS='-O2 -std=gnu11', CPPFLAGS='', LDFLAGS='')
                if not (host / 'Makefile').exists():
                    run('python-host-configure', [source / 'configure', '--without-ensurepip',
                        '--without-readline', '--disable-test-modules'], host, source, host_env)
                run('python-host-build', ['make', '-j' + str(args.jobs), 'python'], host, source, host_env)
                build_python = host / 'python'
            extra = dict(MODULE_BUILDTYPE='static', ac_cv_file__dev_ptmx='yes', ac_cv_file__dev_ptc='no',
                         ac_cv_func_sched_setaffinity='no',
                         CPPFLAGS=f'-I{public} -idirafter {headers}',
                         LDFLAGS=f'-static -no-pie -L{tls}/lib -L{web}/lib -L{sqlite}/lib',
                         LIBSQLITE3_CFLAGS=f'-I{public}', LIBSQLITE3_LIBS=f'{sqlite}/lib/libsqlite3.a -lm -pthread',
                         ZLIB_CFLAGS=f'-I{public}', ZLIB_LIBS=f'{web}/lib/libz.a')
            options = [source / 'configure', '--build=x86_64-pc-linux-gnu', '--host=x86_64-linux-musl',
                       '--prefix=/usr', '--disable-shared', '--with-static-libpython',
                       '--with-build-python=' + str(build_python), '--disable-test-modules',
                       '--without-ensurepip', '--without-readline', '--without-mimalloc',
                       '--disable-ipv6', '--with-openssl=' + str(tls), '--with-openssl-rpath=no']
            key = hashlib.sha256(json.dumps([list(map(str, options)), extra, flags, 'isolated-headers-v2']).encode()).hexdigest()
            if not (build / '.configured').exists() or (build / '.configured').read_text() != key:
                run('python-configure', options, build, source, extra)
                run('python-clean', ['make', 'clean'], build, source, extra)
                (build / '.configured').write_text(key)
            run('python-build', ['make', '-j' + str(args.jobs), 'python', 'pybuilddir.txt'], build, source, extra)
            binary = build / 'python'
            copy(binary, 'usr/bin/python3.14')
            for alias in ['python3', 'python']:
                target = stage / 'usr/bin' / alias
                if not target.is_symlink():
                    assert not target.exists(), target
                    target.symlink_to('python3.14')
            # Source-only stdlib: no target interpreter is used for installation.
            library = stage / 'usr/lib/python314.zip'
            library.parent.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(library, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
                for path in sorted((source / 'Lib').rglob('*')):
                    relative = path.relative_to(source / 'Lib')
                    if path.is_file() and not set(relative.parts) & {'test', 'tests', '__pycache__', 'idlelib', 'tkinter', 'ensurepip'} and path.suffix not in ('.pyc', '.pyo'):
                        bundle.write(path, str(relative))
            # sysconfig and extension metadata are regular files, outside the zip.
            libdir = stage / 'usr/lib/python3.14'
            libdir.mkdir(parents=True, exist_ok=True)
            (libdir / 'lib-dynload').mkdir(exist_ok=True)
            (libdir / 'lib-dynload/README').write_text('This port builds its supported extension modules into the interpreter.\n')
            for path in build.glob('build/lib.*/_sysconfigdata_*.py'):
                copy(path, 'usr/lib/python3.14/' + path.name)
            copy(source / 'LICENSE', 'usr/share/licenses/python/LICENSE')
            missing = [line for line in (build / 'Modules/Setup.stdlib').read_text().splitlines() if line.startswith('#') and re_module(line)]
            atomic_write(out / 'PYTHON-MODULES.json', json.dumps(dict(module_build_type='static', unavailable_configure_lines=missing), indent=2) + '\n')
        else:
            if not (build / 'Makefile').exists():
                run('jq-configure', [source / 'configure', '--build=x86_64-pc-linux-gnu',
                                    '--host=x86_64-linux-musl', '--prefix=/usr',
                                    '--disable-shared', '--enable-static', '--enable-all-static',
                                    '--with-oniguruma=builtin', '--disable-maintainer-mode', '--disable-docs'], build, source)
            run('jq-build', ['make', '-j' + str(args.jobs), 'all'], build, source)
            binary = build / 'jq'
            copy(binary, 'usr/bin/jq')
            copy(source / 'COPYING', 'usr/share/licenses/jq/COPYING')
            for license_file in (source / 'vendor').glob('*/COPYING*'):
                copy(license_file, 'usr/share/licenses/jq/' + license_file.parent.name + '-' + license_file.name)
        elf = subprocess.check_output(['readelf', '-h', '-l', str(binary)], text=True)
        assert 'Advanced Micro Devices X86-64' in elf and 'INTERP' not in elf and ' RWE ' not in elf
        copy(archive, 'usr/src/coast/program-ports/' + archive.name)
    example = ROOT / 'userland/program-ports/check.py'
    if example.is_file():
        copy(example, 'usr/share/coast/program-ports/check.py')
    files = {str(p.relative_to(stage)):dict(path=str(p), sha256=sha(p), mode=p.stat().st_mode & 0o777) for p in sorted(stage.rglob('*')) if p.is_file() and not p.is_symlink()}
    aliases = {str(p.relative_to(stage)):os.readlink(p) for p in sorted(stage.rglob('*')) if p.is_symlink()}
    record = dict(state='compiled; guest validation pending', packages=PACKAGES, files=files, aliases=aliases,
                  dependencies={str(p):sha(p) for p in dependencies}, source_sha256=original,
                  compiler_build_sha256=sha(compiler_receipt) if compiler_receipt else None,
                  dependency_receipt=dict(path=str(dependency_receipt), sha256=sha(dependency_receipt)) if dependency_receipt else None,
                  compiler_sha256=sha(Path(str(cross) + 'gcc')), bootstrap_host_python=args.bootstrap_host_python,
                  confinement=conf, commands=commands, target_host_executed=False)
    if any(sha(ROOT / name) != expected for name, expected in original.items()):
        raise ValueError('Program sources changed during compilation')
    atomic_write(out / 'BUILD.json', json.dumps(record, indent=2) + '\n')
    if args.install_runtime:
        runtime = ROOT / 'userland/runtime'
        for name, asset in files.items():
            target = runtime / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(asset['path'], target)
        for name, link in aliases.items():
            target = runtime / name
            if target.is_symlink():
                assert os.readlink(target) == link, target
            else:
                assert not target.exists(), target
                target.symlink_to(link)
    print('Program ports staged:', stage, flush=True)


def re_module(line):
    return len(line) > 1 and (line[1].isalnum() or line[1] == '_') and '.c' in line


if __name__ == '__main__':
    with project_lock():
        main()
