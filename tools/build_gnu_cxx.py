#!/usr/bin/env python3
"""Build GNU ABI libstdc++ and its headers against Coast's source-built glibc."""
import argparse
import fcntl
import json
from pathlib import Path, PurePosixPath
import platform
import re
import shlex
import shutil
import subprocess

from fetch_sources import fetch, read_manifest
from source_archive import unpack
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--glibc-build', type=Path, required=True)
    parser.add_argument('--output-root', type=Path)
    parser.add_argument('--source-root', type=Path, help='optional existing GCC source cache')
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--cc')
    parser.add_argument('--cxx')
    parser.add_argument('--compiler-family', choices=['gcc', 'clang'],
                        help='native GNU ABI driver selection; defaults to Clang on Coast')
    parser.add_argument('--bootstrap-gcc-libdir', type=Path)
    parser.add_argument('--libgcc-build', type=Path, help='matching source-built GNU unwinder and GCC support library')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 24 or args.native and platform.system() != 'Coast':
        parser.error('Use --jobs 1..24; --native requires Coast')
    out = (args.output_root or ROOT / ('build/gnu-cxx-native' if args.native else 'build/gnu-cxx')).resolve()
    out.mkdir(parents=True, exist_ok=True)
    lock = (out / '.build.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    receipt = args.glibc_build.resolve(strict=True)
    glibc = json.loads(receipt.read_text())
    sysroot = receipt.parent / 'runtime'
    for name, asset in glibc['files'].items():
        if PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts:
            raise ValueError('Invalid GNU libc SDK path')
        if (digest(sysroot / name) != asset['sha256'] or
                not args.native and digest(Path(asset['path'])) != asset['sha256']):
            raise ValueError('GNU libc SDK differs from its build receipt: ' + name)
    record = read_manifest()['gcc-14.2.0.tar.gz']
    vendor = out / 'vendor'
    vendor.mkdir(exist_ok=True)
    cached = ROOT / 'userland/vendor' / record['archive']
    if cached.is_file() and not (vendor / cached.name).exists():
        if digest(cached) != record['sha256']:
            raise ValueError('GCC source cache checksum mismatch')
        shutil.copyfile(cached, vendor / cached.name)
    fetch(record, vendor)
    source_root = (args.source_root or out / 'src').resolve()
    source_root.mkdir(parents=True, exist_ok=True)
    source = source_root / 'gcc-14.2.0'
    if not source.is_dir():
        unpack(vendor / record['archive'], source_root)
    # Standalone libstdc++ configuration expects this target gthread header
    # in its sibling libgcc build directory, just as a top-level GCC build does.
    gthreads = out / 'libgcc'
    gthreads.mkdir(exist_ok=True)
    shutil.copyfile(source / 'libgcc/gthr-posix.h', gthreads / 'gthr-default.h')
    family = args.compiler_family or ('clang' if args.native else 'gcc')
    cc, cxx = args.cc or ('clang' if family == 'clang' else 'gcc'), args.cxx or ('clang++' if family == 'clang' else 'g++')
    bootstrap = args.bootstrap_gcc_libdir
    libgcc_receipt, support = None, None
    if args.libgcc_build:
        if bootstrap:
            parser.error('--libgcc-build and --bootstrap-gcc-libdir are mutually exclusive')
        libgcc_receipt = args.libgcc_build.resolve(strict=True)
        libgcc = json.loads(libgcc_receipt.read_text())
        if libgcc['glibc_build_sha256'] != digest(receipt):
            raise ValueError('The GNU unwinder and C++ runtime must use the same glibc SDK')
        libgcc_runtime = libgcc_receipt.parent / 'runtime'
        for name, asset in libgcc['files'].items():
            if PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts:
                raise ValueError('Invalid GNU unwinder SDK path')
            if digest(libgcc_runtime / name) != asset['sha256']:
                raise ValueError('GNU unwinder SDK differs from its receipt: ' + name)
        bootstrap = libgcc_runtime / 'usr/lib/x86_64-linux-gnu'
        support = libgcc_runtime / 'usr/lib/gcc/x86_64-coast-linux-gnu/14.2.0'
    if bootstrap is None:
        bootstrap = (Path('/usr/lib/x86_64-linux-gnu') if args.native else
            Path(subprocess.check_output([cc, '-print-file-name=libgcc_s.so.1'], text=True).strip()).resolve().parent)
    bootstrap = bootstrap.resolve(strict=True)
    unwinder = bootstrap / 'libgcc_s.so.1'
    if unwinder.read_bytes()[:4] != b'\x7fELF':
        raise ValueError('Select the existing GNU bootstrap unwinder directory')
    libraries = sysroot / 'usr/lib/x86_64-linux-gnu'
    flags = ['-O2', '-g0', '-mno-avx', '-mno-avx2', '--sysroot=' + str(sysroot),
        '-B' + str(libraries) + '/', '-idirafter', str(ROOT / 'ports/linux-uapi/include')]
    if not (ROOT / 'ports/linux-uapi/SOURCE.json').is_file():
        flags[-1] = '/usr/lib/coast/linux-uapi/include'
    target = ['--no-default-config', '--target=x86_64-linux-gnu',
        '--gcc-install-dir=/usr/lib/gcc/x86_64-linux-musl/14.2.0'] if family == 'clang' else []
    if args.native and family == 'gcc':
        specs = out / 'gnu-driver.specs'
        specs.write_text('*self_spec:\n%<static %<no-pie\n\n')
        target += ['-specs=' + str(specs)]
    if support:
        target += ['-B' + str(support) + '/']
    environment = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
        CC=shlex.join([cc, *target]), CXX=shlex.join([cxx, *target]),
        CFLAGS=shlex.join(flags), CXXFLAGS=shlex.join([*flags, '-nodefaultlibs']),
        CPPFLAGS=shlex.join(['-I' + str(gthreads)]),
        LDFLAGS=shlex.join(['--sysroot=' + str(sysroot), '-L' + str(libraries),
            '-L' + str(bootstrap), *(['-L' + str(support)] if support else []),
            '-Wl,-rpath-link,' + str(libraries)]),
        LIBS='-lc -l:libgcc_s.so.1 -lgcc', AR='ar', RANLIB='ranlib', MAKEINFO='true',
        # This optional tracing header is absent from the target SDK.
        glibcxx_cv_sys_sdt_h='no')
    build = out / 'objects'
    build.mkdir(exist_ok=True)
    confinement = [] if args.native else ['bwrap', '--unshare-all', '--die-with-parent', '--new-session',
        '--ro-bind', '/', '/', '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    commands = []

    def run(name, command):
        command = list(map(str, command))
        commands.append(command)
        print('GNU C++ ' + name, flush=True)
        with (out / (name + '.log')).open('w') as log:
            subprocess.run([*confinement, '--chdir', str(build), '--', *command] if confinement else command,
                cwd=build, env=environment, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, check=True)

    build_triplet = 'x86_64-linux-musl' if args.native else subprocess.check_output([cc, '-dumpmachine'], text=True).strip()
    run('configure', [source / 'libstdc++-v3/configure', '--build=' + build_triplet,
        '--host=x86_64-coast-linux-gnu', '--target=x86_64-coast-linux-gnu', '--prefix=/usr', '--libdir=/usr/lib',
        '--with-gxx-include-dir=/usr/include/c++/14.2.0', '--disable-cet', '--disable-multilib',
        '--disable-nls', '--disable-werror', '--enable-shared', '--disable-static',
        '--disable-libstdcxx-pch', '--disable-libstdcxx-verbose', '--disable-libstdcxx-backtrace', '--enable-clocale=gnu'])
    configuration = (build / 'include/x86_64-coast-linux-gnu/bits/c++config.h').read_text()
    if '#define _GLIBCXX_HAS_GTHREADS 1' not in configuration:
        raise ValueError('GNU C++ runtime must include real POSIX thread support')
    run('build', ['make', '-j' + str(args.jobs)])
    staging = out / 'install'
    run('install', ['make', 'DESTDIR=' + str(staging), 'install'])
    runtime = out / 'runtime'
    include = runtime / 'usr/include/c++/14.2.0'
    include.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(staging / 'usr/include/c++/14.2.0', include, dirs_exist_ok=True)
    binary = next(p for p in staging.rglob('libstdc++.so.6.0.*')
        if p.is_file() and not p.is_symlink() and p.read_bytes()[:4] == b'\x7fELF')
    destination = runtime / 'usr/lib/x86_64-linux-gnu' / binary.name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(binary, destination)
    subprocess.run(['strip', '--strip-debug', str(destination)], check=True)
    versions = subprocess.check_output(['readelf', '--version-info', '-d', str(destination)], text=True)
    if max(tuple(map(int, n.split('.'))) for n in re.findall(r'GLIBC_([0-9.]+)', versions)) > (2, 36):
        raise ValueError('GNU C++ runtime accidentally requires the host glibc')
    aliases = {'usr/lib/x86_64-linux-gnu/libstdc++.so.6': binary.name,
               'usr/lib/x86_64-linux-gnu/libstdc++.so': binary.name}
    for name, target_name in aliases.items():
        path = runtime / name
        path.unlink(missing_ok=True)
        path.symlink_to(target_name)
    notices = runtime / 'usr/share/licenses/gnu-libstdc++'
    notices.mkdir(parents=True, exist_ok=True)
    for name in ['COPYING3', 'COPYING.RUNTIME']:
        shutil.copyfile(source / name, notices / name)
    inputs = [Path(__file__), ROOT / 'userland/vendor/sources.json', ROOT / 'tools/fetch_sources.py',
        ROOT / 'tools/source_archive.py', ROOT / 'tools/source_tree.py']
    (out / 'BUILD.json').write_text(json.dumps(dict(version='14.2.0', source=record,
        glibc_build_sha256=digest(receipt), bootstrap_unwinder_sha256=digest(unwinder),
        libgcc_build_sha256=digest(libgcc_receipt) if libgcc_receipt else None,
        files={str(p.relative_to(runtime)): dict(path=str(p), sha256=digest(p),
            mode=0o755 if p.stat().st_mode & 0o111 else 0o644)
            for p in sorted(runtime.rglob('*')) if p.is_file() and not p.is_symlink()}, aliases=aliases,
        commands=commands, environment=environment, source_sha256={str(p.relative_to(ROOT)): digest(p) for p in inputs},
        build_mode='native-coast' if args.native else 'linux-cross', target_host_executed=False,
        state='compiled; GNU C++ loader and thread qualification pending'), indent=2) + '\n')
    print('GNU C++ runtime compiled; validate inside Coast', flush=True)


if __name__ == '__main__':
    main()
