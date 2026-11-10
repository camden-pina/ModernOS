#!/usr/bin/env python3
"""Build GCC's GNU ABI unwinder and support libraries against Coast glibc.

An existing C compiler bootstraps the library. GCC's target metadata and
generator programs are built from the pinned GCC sources; a second complete
compiler is unnecessary for this library-only bootstrap.
"""
from build_state import project_lock
from build_state import atomic_write
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
    parser.add_argument('--source-root', type=Path, help='existing pinned GCC source cache')
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 24 or args.native and platform.system() != 'Coast':
        parser.error('Use --jobs 1..24; --native requires Coast')
    out = (args.output_root or ROOT / ('build/gnu-libgcc-native' if args.native else 'build/gnu-libgcc')).resolve()
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
            raise ValueError('GNU libc SDK differs from its receipt: ' + name)
    source_records = read_manifest()
    record = source_records['gcc-14.2.0.tar.gz']
    prerequisites = [source_records[n] for n in ['gmp-6.2.1.tar.xz', 'mpfr-4.1.0.tar.xz', 'mpc-1.2.1.tar.gz']]
    vendor = out / 'vendor'
    vendor.mkdir(exist_ok=True)
    for archive in [record, *prerequisites]:
        cached = ROOT / 'userland/vendor' / archive['archive']
        if cached.is_file() and not (vendor / cached.name).exists():
            if digest(cached) != archive['sha256']:
                raise ValueError('GCC prerequisite archive checksum mismatch')
            shutil.copyfile(cached, vendor / cached.name)
        fetch(archive, vendor)
    source_root = (args.source_root or out / 'src').resolve()
    source_root.mkdir(parents=True, exist_ok=True)
    source = source_root / 'gcc-14.2.0'
    if not source.is_dir():
        unpack(vendor / record['archive'], source_root)
    for prerequisite in prerequisites:
        short, version = prerequisite['archive'].split('-', 1)
        directory = source / short
        if not directory.exists():
            unpack(vendor / prerequisite['archive'], source_root)
            unpacked = source_root / (short + '-' + version.split('.tar')[0])
            shutil.move(unpacked, directory)
    objects = out / 'objects'
    objects.mkdir(exist_ok=True)
    gcc = objects / 'gcc'
    host = subprocess.check_output([args.cc, '-dumpmachine'], text=True).strip()
    environment = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
        CC=args.cc, CXX=args.cxx, CFLAGS='-O2 -g0 -std=gnu11 -mno-avx -mno-avx2',
        CXXFLAGS='-O2 -g0', MAKEINFO='true', AR='ar', RANLIB='ranlib', AWK='gawk')
    confinement = [] if args.native else ['bwrap', '--unshare-all', '--die-with-parent', '--new-session',
        '--ro-bind', '/', '/', '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    commands = []

    def run(name, command, directory=objects, env=environment):
        command = list(map(str, command))
        commands.append(dict(name=name, argv=command, cwd=str(directory)))
        print('GNU libgcc ' + name, flush=True)
        with (out / (name + '.log')).open('w') as log:
            subprocess.run([*confinement, '--chdir', str(directory), '--', *command] if confinement else command,
                cwd=directory, env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, check=True)

    run('configure', [source / 'configure', '--build=' + host, '--host=' + host,
        '--target=x86_64-coast-linux-gnu', '--prefix=' + str(out / 'compiler'), '--with-sysroot=' + str(sysroot),
        '--disable-bootstrap', '--disable-multilib', '--disable-nls', '--disable-libsanitizer',
        '--disable-libquadmath', '--disable-libssp', '--disable-libgomp', '--disable-libvtv',
        '--disable-libcc1', '--disable-plugin', '--disable-lto', '--enable-languages=c,c++',
        '--enable-threads=posix', '--without-isl', '--without-zstd'])
    run('configure-gcc', ['make', '-j' + str(args.jobs), 'configure-gcc'])
    run('host-generators', ['make', '-j' + str(args.jobs), 'all-build-libiberty', 'all-build-libcpp'])
    # These two targets are prerequisites of libgcc.mvars, whose recipe only
    # records configured target flags. The library uses the explicit external
    # CC below, so do not build an unused xgcc executable or its specs file.
    run('target-headers', ['make', '-C', gcc, '-o', 'xgcc', '-o', 'specs', '-j' + str(args.jobs),
        'libgcc.mvars', 'tconfig.h', 'tm.h', 'options.h', 'insn-modes.h', 'insn-constants.h', 'version.h'])
    (gcc / 'include').mkdir(exist_ok=True)
    # GCC 14 cpuinfo.c needs its matching feature-bit header. A newer bootstrap
    # compiler's cpuid.h may have removed legacy names used by these sources.
    shutil.copyfile(source / 'gcc/config/i386/cpuid.h', gcc / 'include/cpuid.h')
    libraries = sysroot / 'usr/lib/x86_64-linux-gnu'
    uapi = ROOT / 'ports/linux-uapi/include'
    if not (ROOT / 'ports/linux-uapi/SOURCE.json').is_file():
        uapi = Path('/usr/lib/coast/linux-uapi/include')
    compiler = [args.cc, '--sysroot=' + str(sysroot), '-B' + str(libraries) + '/']
    if args.native:
        # The Coast MUSL compiler defaults to static linking. Its documented
        # user self-spec can remove those defaults for this separate GNU ABI
        # library build, leaving ordinary native compiler behavior untouched.
        specs = out / 'gnu-driver.specs'
        specs.write_text('*self_spec:\n%<static %<no-pie\n\n')
        compiler += ['-specs=' + str(specs)]
    flags = ['-I' + str(gcc / 'include'), '-O2', '-g0', '-std=gnu11', '-fno-pie', '-mno-avx', '-mno-avx2',
        '--sysroot=' + str(sysroot), '-B' + str(libraries) + '/', '-idirafter', str(uapi)]
    library_environment = {**environment, 'CC': shlex.join(compiler), 'CPP': shlex.join([*compiler, '-E']),
        'CPPFLAGS': shlex.join(['-idirafter', str(uapi)]), 'CFLAGS': shlex.join(flags),
        'LDFLAGS': shlex.join(['--sysroot=' + str(sysroot), '-L' + str(libraries)])}
    library_objects = objects / 'x86_64-coast-linux-gnu/libgcc'
    library_objects.mkdir(parents=True, exist_ok=True)
    run('library-configure', [source / 'libgcc/configure', '--build=' + host,
        '--host=x86_64-coast-linux-gnu', '--target=x86_64-coast-linux-gnu', '--prefix=/usr',
        '--libdir=/usr/lib/x86_64-linux-gnu', '--with-slibdir=/usr/lib/x86_64-linux-gnu',
        '--with-target-subdir=x86_64-coast-linux-gnu', '--with-cross-host=' + host,
        '--disable-multilib', '--enable-shared', '--enable-threads=posix'], library_objects, library_environment)
    run('library-build', ['make', '-j' + str(args.jobs), 'gcc_objdir=' + str(gcc)], library_objects, library_environment)
    runtime = out / 'runtime'
    dso = runtime / 'usr/lib/x86_64-linux-gnu/libgcc_s.so.1'
    dso.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(gcc / 'libgcc_s.so.1', dso)
    subprocess.run(['strip', '--strip-debug', str(dso)], check=True)
    header = subprocess.check_output(['readelf', '-h', '-l', '-d', str(dso)], text=True)
    versions = subprocess.check_output(['readelf', '--version-info', str(dso)], text=True)
    if ('Advanced Micro Devices X86-64' not in header or ' RWE ' in header or
            'INTERP' in header or 'libgcc_s.so.1' not in header or
            max(tuple(map(int, n.split('.'))) for n in re.findall(r'GLIBC_([0-9.]+)', versions)) > (2, 36)):
        raise ValueError('GNU unwinder is not an x86-64 DSO for the source-built glibc')
    support = runtime / 'usr/lib/gcc/x86_64-coast-linux-gnu/14.2.0'
    support.mkdir(parents=True, exist_ok=True)
    for path in [*gcc.glob('libgcc*.a'), gcc / 'libgcov.a', *gcc.glob('crt*.o')]:
        shutil.copyfile(path, support / path.name)
    (support / 'include').mkdir(exist_ok=True)
    shutil.copyfile(gcc / 'include/unwind.h', support / 'include/unwind.h')
    aliases = {'usr/lib/x86_64-linux-gnu/libgcc_s.so': 'libgcc_s.so.1'}
    for name, target in aliases.items():
        alias = runtime / name
        alias.unlink(missing_ok=True)
        alias.symlink_to(target)
    notices = runtime / 'usr/share/licenses/gnu-libgcc'
    notices.mkdir(parents=True, exist_ok=True)
    for name in ['COPYING3', 'COPYING.RUNTIME']:
        shutil.copyfile(source / name, notices / name)
    inputs = [Path(__file__), ROOT / 'userland/vendor/sources.json', ROOT / 'tools/fetch_sources.py',
        ROOT / 'tools/source_archive.py', ROOT / 'tools/source_tree.py']
    atomic_write(out / 'BUILD.json', json.dumps(dict(version='14.2.0', source=record, generator_sources=prerequisites,
        glibc_build_sha256=digest(receipt), bootstrap_compiler=args.cc,
        files={str(p.relative_to(runtime)): dict(path=str(p), sha256=digest(p),
            mode=0o755 if p.stat().st_mode & 0o111 else 0o644)
            for p in sorted(runtime.rglob('*')) if p.is_file() and not p.is_symlink()}, aliases=aliases,
        commands=commands, environment=environment, library_environment=library_environment,
        source_sha256={str(p.relative_to(ROOT)): digest(p) for p in inputs},
        build_mode='native-coast' if args.native else 'linux-cross', target_host_executed=False,
        state='compiled; GNU unwinder exception and thread qualification pending'), indent=2) + '\n')
    print('GNU libgcc compiled; validate inside Coast', flush=True)


if __name__ == '__main__':
    with project_lock():
        main()
