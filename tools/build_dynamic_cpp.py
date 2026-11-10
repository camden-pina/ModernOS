#!/usr/bin/env python3
"""Build a separate GCC 14 POSIX shared C++ ABI using pinned local sources."""
import argparse
import hashlib
import inspect
import shlex
import json
import os
from pathlib import Path
import shutil
import subprocess

from build_dynamic import build_musl, PREFIX as MUSL

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/dynamic-cpp'
PREFIX = BUILD / 'prefix'
GCC = ROOT / 'build/compiler/src/gcc-14.2.0'
SEED = ROOT / 'build/compiler/seed'
CROSS = ROOT / 'build/compiler/cross/bin/x86_64-linux-musl-'
INPUT = ROOT / 'userland/dynamic-cpp'
RUNTIME = ROOT / 'userland/runtime'
SPECS = INPUT / 'coast-cpp.specs'
FLAGS = '-O2 -g0 -fPIC -fno-stack-protector -mno-avx -mno-avx2'
LINK = '-Wl,-z,max-page-size=4096,-z,relro,-z,now,-z,noexecstack'


def run(name, command, cwd=BUILD, env=None):
    print(name, flush=True)
    with (BUILD / (name + '.log')).open('wb') as log:
        result = subprocess.run(list(map(str, command)), cwd=cwd, env=env,
                                stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f'{name} failed ({result.returncode}); see {BUILD / (name + ".log")}')


def key_for(paths):
    return hashlib.sha256(b''.join(str(p).encode() + p.read_bytes() for p in paths)).hexdigest()


def environment():
    env = os.environ.copy()
    for name in ('CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LIBS',
                 'CONFIG_SITE', 'GCC_EXEC_PREFIX', 'COMPILER_PATH', 'LIBRARY_PATH',
                 'MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES'):
        env.pop(name, None)
    env['PATH'] = str(CROSS.parent) + ':' + env['PATH']
    return env


def build_unwinder(jobs, env):
    original = SEED / 'x86_64-linux-musl/libgcc'
    key = hashlib.sha256(inspect.getsource(build_unwinder).encode() + key_for([SPECS, original / 'Makefile', original / 'auto-target.h', GCC / 'libgcc/gthr-posix.h']).encode()).hexdigest()
    directory = BUILD / 'unwinder' / key
    if not (directory / '.complete').exists():
        shutil.copytree(original, directory, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns('*.o', '*.a', '*.dep', 'gthr-default.h', 'libgcc_s*', '.complete'))
        shutil.copy2(GCC / 'libgcc/gthr-posix.h', directory / 'gthr-default.h')
        cc = f'{CROSS}gcc -specs={SPECS} -L{MUSL / "lib"}'
        run('unwinder-build', ['make', f'-j{jobs}', f'gcc_objdir={SEED / "gcc"}',
                              'enable_shared=yes', 'thread_header=gthr-posix.h', f'CC={cc}',
                              'CFLAGS=' + FLAGS, '-o', 'Makefile', '-o', 'config.status',
                              'libgcc_s.so', 'crtbeginS.o', 'crtendS.o'], directory, env)
        (directory / '.complete').touch()
    lib = PREFIX / 'lib'
    lib.mkdir(parents=True, exist_ok=True)
    shutil.copy2(directory / 'libgcc_s.so.1', lib / 'libgcc_s.so.1')
    alias = lib / 'libgcc_s.so'
    if not alias.exists():
        alias.symlink_to('libgcc_s.so.1')
    for name in ('crtbeginS.o', 'crtendS.o'):
        shutil.copy2(directory / name, lib / name)


def build_standard_library(jobs, env):
    configure = (GCC / 'libstdc++-v3/configure').read_text()
    old = "target_thread_file=`$CXX -v 2>&1 | sed -n 's/^Thread model: //p'`"
    if configure.count(old) != 2:
        raise SystemExit('Pinned libstdc++ thread configuration drift')
    patched = BUILD / 'configure-posix'
    patched.write_text(configure.replace(old, 'target_thread_file=posix') + '\n# Coast shared POSIX profile\n')
    patched.chmod(0o755)
    (BUILD / 'libgcc').mkdir(exist_ok=True)
    shutil.copy2(GCC / 'libgcc/gthr-posix.h', BUILD / 'libgcc/gthr-default.h')
    cc = BUILD / 'shared-gcc'
    cxx = BUILD / 'shared-g++'
    for path, compiler in ((cc, 'gcc'), (cxx, 'g++')):
        path.write_text('#!/bin/sh\nexec ' + str(CROSS) + compiler + ' -specs=' + str(SPECS) +
                        ' -L' + str(PREFIX / 'lib') + ' -L' + str(MUSL / 'lib') + ' "$@"\n')
        path.chmod(0o755)
    args = [str(patched), f'--srcdir={GCC / "libstdc++-v3"}',
            '--build=' + subprocess.check_output(['gcc', '-dumpmachine'], text=True).strip(),
            '--host=x86_64-linux-musl', f'--prefix={PREFIX}', f'--libdir={PREFIX / "lib"}',
            f'--with-gxx-include-dir={PREFIX / "include/c++/14.2.0"}', '--disable-multilib',
            '--enable-shared', '--disable-static', '--disable-nls', '--disable-symvers',
            '--disable-libstdcxx-pch', '--disable-libstdcxx-verbose', '--disable-libstdcxx-backtrace',
            '--enable-clocale=generic']
    target = env.copy()
    target.update(CC=str(cc), CXX=str(cxx), CFLAGS=FLAGS, CXXFLAGS=FLAGS,
                  LDFLAGS='-L' + str(PREFIX / 'lib') + ' -L' + str(MUSL / 'lib') + ' ' + LINK)
    key = hashlib.sha256(json.dumps([args, FLAGS, LINK, str(ROOT), inspect.getsource(build_standard_library) + key_for([SPECS])]).encode()).hexdigest()
    directory = BUILD / 'libstdcxx'
    stamp = directory / '.configured'
    if stamp.exists() and stamp.read_text() != key:
        shutil.rmtree(directory)
    directory.mkdir(exist_ok=True)
    if not stamp.exists():
        run('libstdcxx-configure', args, directory, target)
        stamp.write_text(key)
    if not (directory / '.complete').exists():
        run('libstdcxx-build', ['make', f'-j{jobs}'], directory, target)
        run('libstdcxx-install', ['make', '-j1', 'install'], directory, target)
        (directory / '.complete').touch()


def package():
    lib = RUNTIME / 'usr/lib'
    lib.mkdir(parents=True, exist_ok=True)
    for source in sorted([* (PREFIX / 'lib').glob('lib*.so*'), * (PREFIX / 'lib64').glob('lib*.so*')]):
        if source.name.endswith('.py'):
            continue
        destination = lib / source.name
        if destination.is_symlink():
            destination.unlink()
        if source.is_symlink():
            destination.symlink_to(os.readlink(source))
        elif source.is_file():
            shutil.copy2(source, destination)
    headers = RUNTIME / 'usr/include/c++-dynamic/14.2.0'
    shutil.copytree(PREFIX / 'include/c++/14.2.0', headers, dirs_exist_ok=True)
    for name in ('crtbeginS.o', 'crtendS.o'):
        target = lib / 'coast/c++' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(PREFIX / 'lib' / name, target)
    shutil.copy2(SPECS, lib / SPECS.name)
    for directory in (ROOT / 'userland/bin', RUNTIME / 'usr/bin'):
        shutil.copy2(INPUT / 'g++-dynamic', directory / 'g++-dynamic')
        (directory / 'g++-dynamic').chmod(0o755)
    notices = ROOT / 'userland/vendor/licenses'
    notices.mkdir(parents=True, exist_ok=True)
    for source, name in [('COPYING3', 'gcc-COPYING3.txt'), ('COPYING.RUNTIME', 'gcc-runtime-exception.txt')]:
        shutil.copy2(GCC / source, notices / name)
    sdk = RUNTIME / 'usr/src/coast/dynamic-cpp'
    sdk.mkdir(parents=True, exist_ok=True)
    for source in INPUT.iterdir():
        if source.is_file():
            shutil.copy2(source, sdk / source.name)
    records = {str(p.relative_to(RUNTIME)): hashlib.sha256(p.read_bytes()).hexdigest()
               for p in (lib / 'libgcc_s.so.1', lib / 'libstdc++.so.6')}
    (BUILD / 'manifest.json').write_text(json.dumps({'gcc':'14.2.0','musl':'1.2.5','thread_backend':'posix','shared':records},indent=2)+'\n')


def build_fixtures():
    directory = BUILD / 'fixtures'
    directory.mkdir(exist_ok=True)
    headers = PREFIX / 'include/c++/14.2.0'
    command = [str(CROSS) + 'g++', '-specs=' + str(SPECS), '-fPIE', '-pthread',
               '-B' + str(PREFIX / 'lib') + '/', '-L' + str(PREFIX / 'lib'),
               '-L' + str(PREFIX / 'lib64'), '-L' + str(MUSL / 'lib'), '-nostdinc++',
               '-isystem', str(headers), '-isystem', str(headers / 'x86_64-linux-musl'),
               '-O2', '-std=c++17', '-Wall', '-Wextra', '-Werror', LINK]
    wrapper = BUILD / 'fixture-g++'
    wrapper.write_text('#!/bin/sh\nexec ' + shlex.join(command) + ' "$@"\n')
    wrapper.chmod(0o755)
    for source in INPUT.iterdir():
        if source.suffix in ('.cpp', '.hpp') or source.name == 'Makefile':
            shutil.copy2(source, directory / source.name)
    run('fixtures-build', ['make', '-j1', 'CXX=' + str(wrapper)], directory)
    run('native-example-build', [*command, INPUT / 'hello.cpp', '-o', directory / 'cpp-hello'])
    destination = RUNTIME / 'usr/lib/coast/dynamic-cpp'
    destination.mkdir(parents=True, exist_ok=True)
    for name in ('cpp-check', 'cpp-hello', 'libcpp-service.so', 'libcpp-plugin.so', 'libcpp-unresolved.so'):
        shutil.copy2(directory / name, destination / name)
    hidden = directory / 'required'
    hidden.mkdir(exist_ok=True)
    run('required-library-build', [*command, '-fPIC', '-shared', INPUT / 'service.cpp',
                                  '-Wl,-soname,libcpp-required.so', '-o', hidden / 'libcpp-required.so'])
    run('missing-dependency-build', [*command, INPUT / 'missing.cpp', '-L' + str(hidden),
                                    '-lcpp-required', '-Wl,-rpath,/no-cpp-library',
                                    '-o', destination / 'cpp-missing'])
    needed = subprocess.check_output(['readelf', '-d', str(destination / 'cpp-check')], text=True)
    if 'libstdc++.so.6' not in needed or 'libgcc_s.so.1' not in needed:
        raise SystemExit('C++ fixture must use both actual shared C++ and unwind runtimes')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=1)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32:
        parser.error('--jobs must be1..32')
    BUILD.mkdir(parents=True, exist_ok=True)
    build_musl(args.jobs)
    env = environment()
    build_unwinder(args.jobs, env)
    build_standard_library(args.jobs, env)
    package()
    build_fixtures()
    print('Shared C++ runtime packaged', flush=True)

if __name__ == '__main__':
    main()
