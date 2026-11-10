#!/usr/bin/env python3
"""Build static CMake tools for Coast, including an offline native bootstrap."""
from build_state import project_lock
from build_state import atomic_write
import argparse
import fcntl
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess

from fetch_sources import fetch
from linux_uapi import prepare as prepare_uapi
from source_archive import unpack
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / 'ports/build-tools/SOURCES.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--compiler-prefix', type=Path,
                        default=ROOT / 'build/compiler-threaded/cross/bin/x86_64-linux-musl-')
    parser.add_argument('--native-toolchain', type=Path, help='alternate native compiler bin directory')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 24 or args.native and platform.system() != 'Coast':
        parser.error('Use --jobs 1..24; --native requires Coast')
    out = (args.output_root or ROOT / ('build/cmake-native' if args.native else 'build/cmake')).resolve()
    out.mkdir(parents=True, exist_ok=True)
    lock = (out / '.build.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    record = json.loads(SOURCES.read_text())['cmake']
    vendor = out / 'vendor'
    vendor.mkdir(exist_ok=True)
    cached = ROOT / 'build/build-tools/vendor' / record['archive']
    if cached.is_file() and not (vendor / record['archive']).exists():
        if digest(cached) != record['sha256']:
            raise ValueError('CMake source cache checksum mismatch')
        shutil.copyfile(cached, vendor / cached.name)
    fetch(record, vendor)
    source = out / record['source']
    if not source.exists():
        unpack(vendor / record['archive'], out)
    coast = ROOT / 'ports/build-tools/Coast.cmake'
    shutil.copyfile(coast, source / 'Modules/Platform/Coast.cmake')
    patch = ROOT / 'ports/build-tools/cmake-spawn.patch'
    stamp = source / '.coast-spawn'
    if not stamp.exists():
        subprocess.run(['patch', '-p1', '-i', str(patch)], cwd=source, check=True)
        stamp.write_text(digest(patch))
    if stamp.read_text() != digest(patch):
        raise ValueError('CMake spawn patch changed; select a fresh output directory')
    build = out / 'objects'
    build.mkdir(exist_ok=True)
    prefix = (str(args.native_toolchain.resolve()) + '/' if args.native_toolchain else '/usr/bin/') if args.native else str(args.compiler_prefix.resolve())
    headers = prepare_uapi(out / 'linux-uapi')
    flags = '-O2 -g0 -fno-pie -fno-stack-protector -mno-avx -mno-avx2 -D_GNU_SOURCE -DCOAST_NATIVE -idirafter ' + str(headers)
    environment = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
        CC=prefix + 'gcc', CXX=prefix + 'g++', AR=prefix + 'ar', RANLIB=prefix + 'ranlib',
        CFLAGS=flags, CXXFLAGS=flags, LDFLAGS='-static -no-pie', SOURCE_DATE_EPOCH='1793923200')
    confinement = [] if args.native else ['bwrap', '--unshare-all', '--die-with-parent', '--new-session',
        '--ro-bind', '/', '/', '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    commands = []

    def run(name, command):
        command = list(map(str, command))
        commands.append(command)
        print('CMake ' + name, flush=True)
        with (out / (name + '.log')).open('w') as log:
            subprocess.run([*confinement, '--chdir', str(build), '--', *command] if confinement else command,
                cwd=build, env=environment, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, check=True)

    options = ['-DCMAKE_INSTALL_PREFIX=/usr', '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_USE_OPENSSL=OFF', '-DCMAKE_USE_SYSTEM_LIBRARIES=OFF', '-DBUILD_TESTING=OFF',
        '-DCMAKE_EXE_LINKER_FLAGS=-static -no-pie', '-DBUILD_SHARED_LIBS=OFF']
    if args.native:
        # Coast supplies the Linux userspace ABI. Select Linux platform source
        # files in bundled libuv and the generator set during this bootstrap;
        # CMAKE_HOST_SYSTEM_NAME remains Coast for the running build tool.
        options += ['-DCMAKE_SYSTEM_NAME=Linux']
        run('bootstrap', [source / 'bootstrap', '--prefix=/usr', '--parallel=' + str(args.jobs), '--', *options])
    else:
        run('configure', ['cmake', '-S', source, '-B', build, '-G', 'Unix Makefiles',
            '-DCMAKE_SYSTEM_NAME=Linux', '-DCMAKE_C_COMPILER=' + environment['CC'],
            '-DCMAKE_CXX_COMPILER=' + environment['CXX'], '-DCMAKE_C_FLAGS=' + flags,
            '-DCMAKE_CXX_FLAGS=' + flags,
            '-DCMAKE_FIND_ROOT_PATH=' + str(args.compiler_prefix.resolve().parent.parent.parent / 'sysroot'),
            '-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY', '-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY',
            '-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY', '-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER', *options])
    run('build', ['make', '-j' + str(args.jobs)])
    runtime = out / 'runtime'
    run('install', ['make', 'DESTDIR=' + str(runtime), 'install'])
    notices = runtime / 'usr/share/licenses/cmake'
    for path in [source / 'Copyright.txt', *(source / 'Utilities').rglob('*'), source / 'Source/kwsys/Copyright.txt']:
        if not path.is_file() or not path.name.upper().startswith(('LICENSE', 'COPYING', 'COPYRIGHT')):
            continue
        destination = notices / path.relative_to(source)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, destination)
    for name in ('cmake', 'ctest', 'cpack'):
        binary = runtime / 'usr/bin' / name
        header = subprocess.check_output(['readelf', '-h', '-l', str(binary)], text=True)
        if 'Advanced Micro Devices X86-64' not in header or 'INTERP' in header or ' RWE ' in header:
            raise ValueError('CMake target is not a static x86-64 executable')
    files = {str(p.relative_to(runtime)): dict(path=str(p), sha256=digest(p),
        mode=0o755 if p.stat().st_mode & 0o111 else 0o644)
        for p in sorted(runtime.rglob('*')) if p.is_file()}
    inputs = [Path(__file__), SOURCES, coast, patch, ROOT / 'tools/linux_uapi.py',
              ROOT / 'tools/fetch_sources.py', ROOT / 'tools/source_archive.py']
    atomic_write(out / 'BUILD.json', json.dumps(dict(version=record['version'], source=record,
        files=files, commands=commands, target_host_executed=args.native,
        build_mode='native-coast' if args.native else 'linux-cross',
        source_sha256={str(p.relative_to(ROOT)): digest(p) for p in inputs},
        state='compiled; guest qualification pending'), indent=2) + '\n')
    print('CMake tools compiled; validate them inside Coast', flush=True)


if __name__ == '__main__':
    with project_lock():
        main()
