#!/usr/bin/env python3
"""Offline, source-built static GCC/binutils and development SDK for ModernOS.

The build machine first creates a musl cross compiler. That compiler builds
the native tools; no glibc libraries are copied into the guest. Stage stamps
allow an interrupted build to resume without rebuilding the earlier stages.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'userland/vendor'
BUILD = ROOT / 'build/compiler'
SOURCE = BUILD / 'src'
SEED = BUILD / 'cross'
SYSROOT = BUILD / 'sysroot'
STAGE = BUILD / 'install'
MUSL = ROOT / 'build/native-musl'
TARGET = 'x86_64-linux-musl'
VERSION = '14.2.0'
PACKAGES = {
    'gcc-14.2.0.tar.gz': '7d376d445f93126dc545e2c0086d0f647c3094aae081cdb78f42ce2bc25e7293',
    'binutils-2.37.tar.gz': 'c44968b97cd86499efbc4b4ab7d98471f673e5414c554ef54afa930062dbbfcb',
    'gmp-6.2.1.tar.xz': 'fd4829912cddd12f84181c3451cc752be224643e87fac497b69edddadc49b4f2',
    'mpfr-4.1.0.tar.xz': '0c98a3f1732ff6ca4ea690552079da9c597872d30e96ec28414ee23c95558a7f',
    'mpc-1.2.1.tar.gz': '17503d2c395dfcf106b622dc142683c1199431d095367c6aacba6eec30340459',
}
FLAGS = '-O2 -g0 -fno-pie -fno-stack-protector -mno-avx -mno-avx2'
OPTIONS = [
    '--disable-bootstrap', '--disable-multilib', '--disable-multiarch',
    '--disable-shared', '--disable-nls', '--disable-libsanitizer',
    '--disable-libssp', '--disable-libquadmath', '--disable-libgomp',
    '--disable-libvtv', '--disable-libcc1', '--disable-plugin', '--disable-lto',
    '--disable-libstdcxx-pch', '--disable-libstdcxx-verbose', '--disable-libstdcxx-backtrace',
    '--disable-symvers', '--disable-threads',
    '--disable-fixincludes', '--enable-languages=c,c++', '--enable-checking=release',
    '--enable-clocale=generic', '--without-isl', '--without-zstd',
]


def run(where, log, *command, env=None):
    print(f'{log}: {command[0]}', flush=True)
    with (BUILD / log).open('wb') as output:
        result = subprocess.run(command, cwd=where, env=env,
                                stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f'{command[0]} failed; see {BUILD / log}')


def stage(name, source, arguments, env, targets, install, outputs):
    directory = BUILD / name
    relevant = {k: v for k, v in env.items() if k in ('CC', 'CXX', 'CFLAGS', 'CXXFLAGS',
                                                      'CPPFLAGS', 'LDFLAGS', 'CC_FOR_BUILD', 'CXX_FOR_BUILD', 'CFLAGS_FOR_BUILD',
                                                      'CXXFLAGS_FOR_BUILD', 'LDFLAGS_FOR_TARGET', 'AR_FOR_TARGET', 'RANLIB_FOR_TARGET',
                                                      'NM_FOR_TARGET', 'MODERNOS_HOST_TOOLCHAIN') or k.startswith(('ac_cv_', 'gcc_cv_'))}
    key = hashlib.sha256(json.dumps([str(ROOT), arguments, relevant, targets, install,
                                     PACKAGES], sort_keys=True).encode()).hexdigest()
    stamp = directory / '.modernos-stage'
    if stamp.exists() and stamp.read_text() == key and all(p.exists() for p in outputs):
        print(f'{name}: cached', flush=True)
        return
    configured = directory / '.modernos-configured'
    if configured.exists() and configured.read_text() != key:
        shutil.rmtree(directory)
    directory.mkdir(parents=True, exist_ok=True)
    if not configured.exists() or configured.read_text() != key:
        run(directory, name + '-configure.log', str(source / 'configure'), *arguments, env=env)
        configured.write_text(key)
    run(directory, name + '-build.log', 'make', '-j' + str(min(os.cpu_count() or 1, 12)),
        *targets, env=env)
    run(directory, name + '-install.log', 'make', *install, env=env)
    stamp.write_text(key)


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    SOURCE.mkdir(exist_ok=True)
    if not (MUSL / 'lib/libc.a').exists():
        raise SystemExit('Build bin/nano first for the pinned musl development library.')

    for filename, digest in PACKAGES.items():
        path = VENDOR / filename
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise SystemExit(f'Upstream source checksum mismatch: {path}')
        destination = SOURCE / filename.split('.tar.')[0]
        if not (destination / '.modernos-extracted').exists():
            with tarfile.open(path) as archive:
                archive.extractall(SOURCE, filter='data')
            (destination / '.modernos-extracted').touch()

    gcc = SOURCE / ('gcc-' + VERSION)
    for package in ('gmp-6.2.1', 'mpfr-4.1.0', 'mpc-1.2.1'):
        link = gcc / package.split('-')[0]
        if not link.exists():
            link.symlink_to('../' + package, target_is_directory=True)

    (SYSROOT / 'usr').mkdir(parents=True, exist_ok=True)
    shutil.copytree(MUSL / 'include', SYSROOT / 'usr/include', dirs_exist_ok=True)
    shutil.copytree(MUSL / 'lib', SYSROOT / 'usr/lib', dirs_exist_ok=True)

    env = os.environ.copy()

    # Codex/session helper directories in PATH change between turns. Cache the
    # resolved build tools and versions, rather than that volatile path string.
    toolchain = {}
    for tool in ('gcc', 'g++', 'as', 'ld', 'ar', 'ranlib', 'nm', 'make'):
        executable = shutil.which(tool)
        if not executable:
            raise SystemExit(f'Native GCC requires the host build tool {tool}.')
        version = subprocess.check_output([executable, '--version'], text=True).splitlines()[0]
        toolchain[tool] = [executable, version]

    env['MODERNOS_HOST_TOOLCHAIN'] = json.dumps(toolchain, sort_keys=True)
    for name in ('CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LIBS',
                 'CONFIG_SITE', 'GCC_EXEC_PREFIX', 'COMPILER_PATH', 'LIBRARY_PATH'):
        env.pop(name, None)
    env.update(CC='gcc', CXX='g++', CFLAGS=FLAGS + ' -std=gnu11',
               CXXFLAGS=FLAGS + ' -std=gnu++11', LDFLAGS='-no-pie',
               LDFLAGS_FOR_TARGET='-static -no-pie', AR_FOR_TARGET='ar',
               RANLIB_FOR_TARGET='ranlib', NM_FOR_TARGET='nm')
    build = subprocess.check_output(['gcc', '-dumpmachine'], text=True).strip()

    # The seed emits ordinary x86-64 ELF for musl. These build-machine aliases
    # are never installed in the guest; native binutils are built below.
    (SEED / 'bin').mkdir(parents=True, exist_ok=True)

    for tool in ('as', 'ld', 'ar', 'ranlib', 'nm', 'objcopy', 'objdump', 'readelf', 'strip'):
        path = SEED / f'bin/{TARGET}-{tool}'
        if not path.exists():
            path.symlink_to(shutil.which(tool))

    env['PATH'] = str(SEED / 'bin') + ':' + env['PATH']
    stage('seed', gcc, [f'--build={build}', f'--target={TARGET}', f'--prefix={SEED}',
          f'--with-sysroot={SYSROOT}', '--with-as=/usr/bin/as', '--with-ld=/usr/bin/ld',
          *OPTIONS], env,
          ['all-gcc', 'all-target-libgcc', 'all-target-libstdc++-v3',
           'CFLAGS_FOR_TARGET=' + FLAGS, 'CXXFLAGS_FOR_TARGET=' + FLAGS],
          ['install-gcc', 'install-target-libgcc', 'install-target-libstdc++-v3'],
          [SEED / f'bin/{TARGET}-gcc', SEED / f'{TARGET}/lib64/libstdc++.a'])

    native = env.copy()
    native.update(CC=str(SEED / f'bin/{TARGET}-gcc'),
                  CXX=str(SEED / f'bin/{TARGET}-g++'),
                  LDFLAGS='-static -no-pie', CC_FOR_BUILD='gcc', CXX_FOR_BUILD='g++',
                  CFLAGS_FOR_BUILD='-O2 -g0 -std=gnu11 -no-pie',
                  CXXFLAGS_FOR_BUILD='-O2 -g0 -std=gnu++11 -no-pie')

    # No file mmap, madvise discard, process groups or executing a mapped PCH.
    # Upstream implementations already provide read/malloc fallbacks.
    native.update(ac_cv_func_mmap='no', ac_cv_func_madvise='no',
                  ac_cv_func_posix_spawn='yes', gcc_cv_func_mmap_file='no')
    stage('binutils', SOURCE / 'binutils-2.37', [f'--build={build}',
          f'--host={TARGET}', f'--target={TARGET}', '--prefix=/usr',
          '--with-sysroot=/', '--disable-nls', '--disable-werror',
          '--disable-gdb', '--disable-gprofng', '--disable-gold',
          '--disable-shared', '--enable-static', '--without-debuginfod'], native,
          [], ['install', f'DESTDIR={STAGE}'], [STAGE / 'usr/bin/as', STAGE / 'usr/bin/ld'])
    stage('native-gcc', gcc, [f'--build={build}', f'--host={TARGET}',
          f'--target={TARGET}', '--prefix=/usr', '--with-sysroot=/',
          f'--with-build-sysroot={SYSROOT}', '--with-native-system-header-dir=/usr/include',
          '--with-as=/usr/bin/as', '--with-ld=/usr/bin/ld',
          '--with-pkgversion=ModernOS static SDK',
          '--with-specs=%{!shared:%{!r:-static -no-pie}}', *OPTIONS], native,
          ['all-gcc'], ['install-gcc', f'DESTDIR={STAGE}'],
          [STAGE / 'usr/bin/gcc', STAGE / f'usr/libexec/gcc/{TARGET}/{VERSION}/cc1plus'])
    export_sdk(gcc)


def export_sdk(gcc):
    runtime = ROOT / 'userland/runtime'
    for directory in ('lib/gcc', 'libexec'):
        shutil.copytree(STAGE / 'usr' / directory, runtime / 'usr' / directory,
                        dirs_exist_ok=True, symlinks=False)
    tools = ('gcc', 'g++', 'cpp', 'as', 'ld', 'ar', 'ranlib', 'nm', 'objcopy', 'objdump',
             'readelf', 'size', 'strings', 'strip', 'addr2line', 'c++filt', 'gcov')
    (runtime / 'usr/bin').mkdir(parents=True, exist_ok=True)
    for name in tools:
        shutil.copy2(STAGE / 'usr/bin' / name, runtime / 'usr/bin' / name)

    # Target libraries were built by the seed with the same GCC version/ABI.
    shutil.copytree(SYSROOT / 'usr/include', runtime / 'usr/include', dirs_exist_ok=True)
    for path in (MUSL / 'lib').iterdir():
        if path.suffix in ('.o', '.a'):
            shutil.copy2(path, runtime / 'usr/lib' / path.name)
    shutil.copytree(SEED / f'lib/gcc/{TARGET}/{VERSION}',
                    runtime / f'usr/lib/gcc/{TARGET}/{VERSION}', dirs_exist_ok=True)
    shutil.copytree(SEED / f'{TARGET}/include/c++/{VERSION}',
                    runtime / f'usr/include/c++/{VERSION}', dirs_exist_ok=True)
    for name in ('libstdc++.a', 'libsupc++.a', 'libstdc++fs.a', 'libstdc++exp.a'):
        shutil.copy2(SEED / f'{TARGET}/lib64/{name}', runtime / 'usr/lib' / name)

    # Installed tools and templates must be static ET_EXEC; strip debug only.
    for directory in (runtime / 'usr/bin', runtime / 'usr/libexec'):
        for path in directory.rglob('*'):
            if path.is_file() and path.read_bytes()[:4] == b'\x7fELF':
                subprocess.run(['strip', '--strip-debug', str(path)], check=True)

    for name, original in (('cc', 'gcc'), ('c++', 'g++')):
        shutil.copy2(runtime / 'usr/bin' / original, runtime / 'usr/bin' / name)
    licenses = VENDOR / 'licenses'
    for source, name in ((gcc / 'COPYING3', 'gcc-COPYING3.txt'),
                         (gcc / 'COPYING.RUNTIME', 'gcc-runtime-exception.txt'),
                         (SOURCE / 'binutils-2.37/COPYING3', 'binutils-COPYING3.txt')):
        shutil.copy2(source, licenses / name)

    for package, notice in (('gmp-6.2.1', 'COPYING'), ('gmp-6.2.1', 'COPYINGv2'),
                            ('gmp-6.2.1', 'COPYINGv3'), ('gmp-6.2.1', 'COPYING.LESSERv3'),
                            ('mpfr-4.1.0', 'COPYING'), ('mpfr-4.1.0', 'COPYING.LESSER'),
                            ('mpc-1.2.1', 'COPYING.LESSER')):
        shutil.copy2(SOURCE / package / notice, licenses / f'{package}-{notice}.txt')

    print('Built native GCC/G++ 14.2.0, GNU binutils 2.37 and the static musl SDK.', flush=True)


if __name__ == '__main__':
    main()
