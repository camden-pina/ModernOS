#!/usr/bin/env python3
"""Offline static-musl WebKit JavaScriptCore port, separate from NetSurf.

This builds a genuine modern JS engine component. It does NOT build WebCore,
render websites, or replace the native desktop Browser. All caches stay under
build/modern-browser; the existing SDK and browser caches are preserved.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tarfile

from build_threaded_libgcc import build_posix_libgcc
from prepare_browser_host import prepare as prepare_host_tools

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/modern-browser'
VENDOR = ROOT / 'userland/vendor'
WEBKIT = BUILD / 'wpewebkit-2.54.0'
ICU = BUILD / 'icu'
PREFIX = BUILD / 'prefix'
GCC_SOURCE = ROOT / 'build/compiler/src/gcc-14.2.0'
CROSS = ROOT / 'build/compiler/cross/bin/x86_64-linux-musl-'
PACKAGES = {
    'wpewebkit-2.54.0.tar.xz': 'efa9bcc3cb891c2d88f50eec710d9ccee71cbdf1040420361eb98c17355eb452',
    'icu4c-78.1-sources.tgz': '6217f58ca39b23127605cfc6c7e0d3475fe4b0d63157011383d716cb41617886',
}
FLAGS = '-O2 -g0 -fno-pie -fno-stack-protector -mno-avx -mno-avx2'


def run(directory, log, *args, env=None):
    print(f'{log}: {shlex.join(map(str, args[:3]))}', flush=True)
    with (BUILD / log).open('w') as output:
        result = subprocess.run(list(map(str, args)), cwd=directory, env=env,
                                stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f'Failed ({result.returncode}); see {BUILD / log}')


def stage(name, configure, targets, env, jobs, outputs=()):
    directory = BUILD / name
    directory.mkdir(parents=True, exist_ok=True)
    stamp = directory / '.modernos-stage'
    configure_digest = hashlib.sha256(Path(configure[0]).read_bytes()).hexdigest()
    key = hashlib.sha256(json.dumps([list(map(str, configure)), configure_digest, targets,
                                     {k: env.get(k) for k in (
                                         'CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'LDFLAGS')},
                                     PACKAGES], sort_keys=True).encode()).hexdigest()
    if stamp.exists() and stamp.read_text() == key and all(path.is_file() for path in outputs):
        print(f'{name}: cached', flush=True)
        return
    stamp.unlink(missing_ok=True)
    configured = directory / '.modernos-configured'
    if (not configured.exists() or configured.read_text() != key
            or not (directory / 'Makefile').is_file()):
        run(directory, name + '-configure.log', *configure, env=env)
        configured.write_text(key)

    if name == 'libstdcxx-posix':
        # Existing objects from an interrupted, earlier configuration need
        # fresh gthread defines. Force only object targets, protecting every
        # generated Makefile/config.status from recursive Autotools remakes.
        for subdir in ('libsupc++', 'src/c++98', 'src/c++11', 'src/c++17', 'src/c++20', 'src'):
            target = directory / subdir
            objects = sorted(path.name for path in target.glob('*.lo'))
            if objects:
                run(target, 'libstdcxx-' + subdir.replace('/', '-') + '-objects.log',
                    'make', f'-j{jobs}', '-B', '-o', 'Makefile', '-o', directory / 'config.status',
                    '-o', directory / 'Makefile', *objects, env=env)

    run(directory, name + '-build.log', 'make', f'-j{jobs}', *targets, env=env)
    run(directory, name + '-install.log', 'make', 'install', env=env)
    missing = [str(path) for path in outputs if not path.is_file()]
    if missing:
        raise SystemExit(f'{name}: missing build outputs: ' + ', '.join(missing))
    stamp.write_text(key)


def notices():
    licenses = VENDOR / 'licenses'
    licenses.mkdir(exist_ok=True)
    shutil.copy2(ICU / 'LICENSE', licenses / 'icu-LICENSE.txt')

    # mkinitramfs installs .txt notices. Retain both the LGPL text and the
    # individual copyright/permission blocks of the static engine sources.
    seen = set()
    sections = [
        'WebKit JavaScriptCore/WTF notices, WPE WebKit 2.54.0.\n'
        'Corresponding unmodified source: userland/vendor/wpewebkit-2.54.0.tar.xz\n'
        f'SHA-256: {PACKAGES["wpewebkit-2.54.0.tar.xz"]}\n'
    ]
    for component in ('JavaScriptCore', 'WTF', 'bmalloc'):
        for path in sorted((WEBKIT / 'Source' / component).rglob('*')):
            if not path.is_file():
                continue
            block = None

            if path.name.startswith(('LICENSE', 'COPYING')):
                block = path.read_text(errors='replace')
            elif path.suffix in ('.c', '.cpp', '.h', '.hpp', '.mm', '.m', '.js', '.rb', '.asm'):
                with path.open(errors='replace') as source:
                    match = re.match(r'\s*/\*.*?\*/', source.read(32768), re.DOTALL)
                if match and 'copyright' in match[0].lower():
                    block = match[0].strip()

            if block and block not in seen:
                seen.add(block)
                sections.append(f'\n--- {path.relative_to(WEBKIT)} ---\n{block}\n')
    (licenses / 'javascriptcore-NOTICES.txt').write_text(''.join(sections))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--deps-only', action='store_true')
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()

    if not 1 <= args.jobs <= 32:
        parser.error('--jobs must be 1..32')
    BUILD.mkdir(parents=True, exist_ok=True)
    (ROOT / 'userland/bin').mkdir(parents=True, exist_ok=True)
    host_tools = prepare_host_tools(jobs=args.jobs)

    for filename, digest in PACKAGES.items():
        archive = VENDOR / filename
        if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
            raise SystemExit(f'Missing or wrong pinned source: {archive}')
        marker = BUILD / (filename + '.extracted')
        if not marker.exists():
            with tarfile.open(archive) as source:
                source.extractall(BUILD, filter='data')
            marker.touch()

    if not Path(str(CROSS) + 'g++').exists() or not GCC_SOURCE.exists():
        raise SystemExit('Build the existing musl GCC SDK first.')
    env = os.environ.copy()
    for key in ('CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LIBS',
                'CONFIG_SITE', 'GCC_EXEC_PREFIX', 'COMPILER_PATH', 'LIBRARY_PATH'):
        env.pop(key, None)
    env['PATH'] = str(CROSS.parent) + ':' + env['PATH']
    target_env = env.copy()
    target_env.update(CC=str(CROSS) + 'gcc', CXX=str(CROSS) + 'g++',
                      CFLAGS=FLAGS, CXXFLAGS=FLAGS, LDFLAGS='-static -no-pie')
    machine = subprocess.check_output(['gcc', '-dumpmachine'], text=True).strip()

    # The existing SDK intentionally uses single-thread libstdc++. Keep it
    # intact. Build a separate POSIX-gthread runtime with the same codegen/ABI.
    # libstdc++'s standalone configure infers this from the seed driver's -v;
    # explicitly select the new runtime's gthread implementation instead.
    configure = (GCC_SOURCE / 'libstdc++-v3/configure').read_text()
    old = "target_thread_file=`$CXX -v 2>&1 | sed -n 's/^Thread model: //p'`"
    if configure.count(old) != 2:
        raise SystemExit('libstdc++ configure thread-selection patch drifted')
    patched = BUILD / 'libstdcxx-posix-configure'
    patched.write_text(configure.replace(old, 'target_thread_file=posix') +
                       '\n# ModernOS separate POSIX gthread runtime, revision 3.\n')
    patched.chmod(0o755)

    # Standalone libstdc++ configure also expects GCC's generated libgcc
    # gthread header beside its build directory. Supply the real POSIX header.
    gthread = BUILD / 'libgcc'
    gthread.mkdir(exist_ok=True)
    shutil.copy2(GCC_SOURCE / 'libgcc/gthr-posix.h', gthread / 'gthr-default.h')
    stage('libstdcxx-posix', [patched, f'--srcdir={GCC_SOURCE / "libstdc++-v3"}',
                              f'--build={machine}', '--host=x86_64-linux-musl', f'--prefix={PREFIX}',
                              f'--with-gxx-include-dir={PREFIX / "include/c++/14.2.0"}', '--disable-multilib',
                              '--disable-shared', '--enable-static', '--disable-nls', '--disable-symvers',
                              '--disable-libstdcxx-pch', '--disable-libstdcxx-verbose',
                              '--disable-libstdcxx-backtrace', '--enable-clocale=generic'], [], target_env, args.jobs,
          outputs=[PREFIX / 'lib64/libstdc++.a',
                   PREFIX / 'include/c++/14.2.0/x86_64-linux-musl/bits/c++config.h'])

    # libstdc++ and the exception unwinder must use the same thread backend.
    # The seed's single-thread libgcc otherwise races while lazily sorting
    # registered .eh_frame objects on concurrent first C++ exceptions.
    libgcc = build_posix_libgcc(BUILD, PREFIX, args.jobs, target_env)
    runtime_cmake = BUILD / 'posix-runtime.cmake'
    runtime_cmake.write_text(f'link_libraries("{libgcc}")\n')
    symbols = subprocess.check_output(['nm', str(PREFIX / 'lib64/libstdc++.a')],
                                      text=True, stderr=subprocess.DEVNULL)
    if ' T _ZNSt18condition_variableC1Ev' not in symbols or ' T __once_proxy' not in symbols:
        raise SystemExit('POSIX C++ runtime is missing condition-variable/once implementations')
    wrapper = BUILD / 'musl-posix-g++'
    include = PREFIX / 'include/c++/14.2.0'
    wrapper.write_text('#!/bin/sh\nexec ' + shlex.quote(str(CROSS) + 'g++') +
                       ' -nostdinc++ -isystem ' + shlex.quote(str(include)) +
                       ' -isystem ' + shlex.quote(str(include / 'x86_64-linux-musl')) +
                       ' -L' + shlex.quote(str(PREFIX / 'lib')) +
                       ' -L' + shlex.quote(str(PREFIX / 'lib64')) + ' "$@"\n')
    wrapper.chmod(0o755)
    target_env['CXX'] = str(wrapper)
    run(BUILD, 'cxx-runtime-check-build.log', wrapper, *shlex.split(FLAGS),
        '-std=gnu++23', '-pthread', '-static', '-no-pie',
        ROOT / 'userland/modern-browser/runtime-check.cpp',
        '-o', ROOT / 'userland/bin/thread-cxx-check', env=target_env)
    host_env = env.copy()
    host_env.update(CC='gcc', CXX='g++', CFLAGS='-O2', CXXFLAGS='-O2')
    common = ['--disable-shared', '--enable-static', '--disable-tests',
              '--disable-samples', '--disable-extras', '--disable-dyload']
    stage('icu-host', [ICU / 'source/configure', f'--prefix={BUILD / "icu-host-prefix"}',
                       *common], [], host_env, args.jobs,
          outputs=[BUILD / 'icu-host/bin/icupkg', BUILD / 'icu-host-prefix/lib/libicuuc.a'])
    stage('icu-musl', [ICU / 'source/configure', f'--build={machine}',
                       '--host=x86_64-linux-musl', f'--prefix={PREFIX}',
                       f'--with-cross-build={BUILD / "icu-host"}', '--with-data-packaging=static',
                       *common], [], target_env, args.jobs,
          outputs=[PREFIX / ('lib/libicu' + name + '.a') for name in ('uc', 'i18n', 'data')]
          + [PREFIX / 'include/unicode/utypes.h'])
    notices()

    if args.deps_only:
        return
    ruby = str(host_tools['ruby'])
    cmake = str(host_tools['cmake'])
    ninja = str(host_tools['ninja'])
    toolchain = BUILD / 'musl-toolchain.cmake'
    toolchain.write_text(f'''set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "{CROSS}gcc")
set(CMAKE_CXX_COMPILER "{wrapper}")
set(CMAKE_C_FLAGS_INIT "{FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "{FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -no-pie")
set(CMAKE_CXX_STANDARD_LIBRARIES "{PREFIX / 'lib/libicudata.a'}")
set(CMAKE_FIND_ROOT_PATH "{PREFIX};{ROOT / 'build/compiler/sysroot'}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
''')
    directory = BUILD / 'jsc'
    run(BUILD, 'jsc-configure.log', cmake, '-S', WEBKIT, '-B', directory, '-G', 'Ninja',
        f'-DCMAKE_MAKE_PROGRAM={ninja}', f'-DCMAKE_TOOLCHAIN_FILE={toolchain}',
        f'-DCMAKE_PROJECT_INCLUDE={runtime_cmake}',
        '-DCMAKE_BUILD_TYPE=Release', '-DPORT=JSCOnly', '-DDEVELOPER_MODE=OFF',
        '-DENABLE_STATIC_JSC=ON', '-DENABLE_JIT=OFF', '-DENABLE_C_LOOP=ON',
        '-DENABLE_WEBASSEMBLY=OFF', '-DENABLE_SAMPLING_PROFILER=OFF',
        '-DUSE_SYSTEM_MALLOC=ON', '-DENABLE_API_TESTS=OFF', '-DUSE_LIBBACKTRACE=OFF',
        f'-DICU_ROOT={PREFIX}', f'-DRuby_EXECUTABLE={ruby}', env=env)
    run(BUILD, 'jsc-build.log', cmake, '--build', directory, '--target', 'jsc',
        '--parallel', args.jobs, env=env)
    output = directory / 'bin/jsc'
    program = ROOT / 'userland/bin/jsc'
    shutil.copy2(output, program)
    resources = ROOT / 'userland/runtime/usr/share/modern-browser'
    resources.mkdir(parents=True, exist_ok=True)
    for name in ('engine-check.js', 'module-check.mjs', 'feature-module.mjs', 'agent-check.js'):
        shutil.copy2(ROOT / 'userland/modern-browser' / name, resources / name)

    print(f'Built real static-musl JavaScriptCore at {program}', flush=True)


if __name__ == '__main__':
    main()
