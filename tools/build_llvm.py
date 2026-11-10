#!/usr/bin/env python3
"""Build pinned static Clang/LLD for Coast with matching source-built generators."""
import argparse
import fcntl
import json
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile

from linux_uapi import prepare as prepare_uapi
from source_archive import unpack
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / 'ports/llvm/SOURCES.json'
PATCH = ROOT / 'ports/llvm/native-spawn.patch'


def archive(record, destination):
    destination.mkdir(parents=True, exist_ok=True)
    result = destination / record['archive']
    if not result.exists():
        for base in [ROOT / 'build/llvm/vendor', ROOT / 'userland/vendor', Path('/usr/src/coast/distfiles')]:
            cached = base / record['archive']
            if cached.is_file():
                if digest(cached) != record['sha256']:
                    raise ValueError('LLVM source cache checksum mismatch')
                shutil.copyfile(cached, result)
                break
        else:
            # This complete upstream archive can take more than ten minutes
            # over a slow connection. Preserve the same TLS/hash requirements.
            with tempfile.NamedTemporaryFile(dir=destination, suffix='.part', delete=False) as stream:
                partial = Path(stream.name)
            try:
                subprocess.run(['curl', '-fL', '--proto', '=https', '--proto-redir', '=https',
                    '--retry', '2', '--connect-timeout', '20', '--max-time', '7200',
                    '--output', str(partial), record['url']], check=True)
                if digest(partial) != record['sha256']:
                    raise ValueError('Downloaded LLVM source checksum mismatch')
                partial.replace(result)
            finally:
                partial.unlink(missing_ok=True)
    if digest(result) != record['sha256']:
        raise ValueError('LLVM source checksum mismatch')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path)
    parser.add_argument('--source-root', type=Path, help='shared extracted source cache owned by this build workspace')
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--native-toolchain', type=Path)
    parser.add_argument('--compiler-prefix', type=Path,
        default=ROOT / 'build/compiler-threaded/cross/bin/x86_64-linux-musl-')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--ninja', default='ninja')
    parser.add_argument('--reconfigure', action='store_true', help='record and regenerate a changed build configuration')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 24 or args.native and platform.system() != 'Coast':
        parser.error('Use --jobs 1..24; --native requires Coast')
    out = (args.output_root or ROOT / ('build/llvm-native' if args.native else 'build/llvm-cross')).resolve()
    out.mkdir(parents=True, exist_ok=True)
    lock = (out / '.build.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    record = json.loads(SOURCES.read_text())
    upstream = archive(record, out / 'vendor')
    source_root = (args.source_root or out / 'source').resolve()
    source_root.mkdir(parents=True, exist_ok=True)
    source_lock = (source_root / '.source.lock').open('a')
    fcntl.flock(source_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    source = source_root / record['source']
    if not source.is_dir():
        unpack(upstream, source_root)
    stamp = source / '.coast-spawn'
    if not stamp.exists():
        subprocess.run(['patch', '--batch', '-p1', '-i', str(PATCH)], cwd=source, check=True)
        stamp.write_text(digest(PATCH))
    if stamp.read_text() != digest(PATCH):
        raise ValueError('LLVM port changed; select a new source cache')
    headers = prepare_uapi(out / 'linux-uapi')
    flags = '-O2 -g0 -fno-pie -mno-avx -mno-avx2 -D_GNU_SOURCE -DCOAST_NATIVE -idirafter ' + str(headers)
    environment = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
        PYTHONDONTWRITEBYTECODE='1', SOURCE_DATE_EPOCH='1793923200')
    confinement = [] if args.native else ['bwrap', '--unshare-all', '--die-with-parent', '--new-session',
        '--ro-bind', '/', '/', '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    commands = []

    def run(name, command, directory=None, env=None):
        command = list(map(str, command))
        commands.append(command)
        directory = directory or out
        print('LLVM ' + name, flush=True)
        with (out / (name + '.log')).open('w') as log:
            subprocess.run([*confinement, '--chdir', str(directory), '--', *command] if confinement else command,
                cwd=directory, env=env or environment, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, check=True)

    common = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DLLVM_TARGETS_TO_BUILD=X86',
        '-DLLVM_ENABLE_PROJECTS=clang;lld', '-DLLVM_ENABLE_RUNTIMES=',
        '-DLLVM_INCLUDE_TESTS=OFF', '-DLLVM_INCLUDE_BENCHMARKS=OFF', '-DLLVM_INCLUDE_EXAMPLES=OFF',
        '-DCLANG_INCLUDE_TESTS=OFF', '-DLLVM_ENABLE_BINDINGS=OFF', '-DLLVM_ENABLE_TERMINFO=OFF',
        '-DLLVM_ENABLE_ZLIB=OFF', '-DLLVM_ENABLE_ZSTD=OFF', '-DLLVM_ENABLE_LIBXML2=OFF',
        '-DLLVM_ENABLE_LIBEDIT=OFF', '-DLLVM_ENABLE_CURL=OFF', '-DLLVM_ENABLE_LIBPFM=OFF',
        '-DLLVM_ENABLE_BACKTRACES=OFF', '-DLLVM_ENABLE_Z3_SOLVER=OFF',
        '-DLLVM_ENABLE_EH=OFF', '-DLLVM_ENABLE_RTTI=OFF', '-DLLVM_BUILD_LLVM_DYLIB=OFF',
        '-DLLVM_LINK_LLVM_DYLIB=OFF', '-DBUILD_SHARED_LIBS=OFF',
        '-DLLVM_ENABLE_THREADS=ON', '-DCLANG_ENABLE_STATIC_ANALYZER=OFF',
        '-DCLANG_ENABLE_ARCMT=OFF', '-DCLANG_PLUGIN_SUPPORT=OFF', '-DLLVM_ENABLE_PLUGINS=OFF',
        '-DLLVM_PARALLEL_LINK_JOBS=1', '-DCMAKE_MAKE_PROGRAM=' + shutil.which(args.ninja)]
    build = out / 'objects'
    build.mkdir(exist_ok=True)
    cross = []
    if args.native:
        prefix = str(args.native_toolchain.resolve()) + '/' if args.native_toolchain else '/usr/bin/'
    else:
        host = out / 'host-generators'
        run('host-configure', [args.cmake, '-S', source / 'llvm', '-B', host,
            *common, '-DCMAKE_C_COMPILER=/usr/bin/gcc', '-DCMAKE_CXX_COMPILER=/usr/bin/g++',
            '-DCMAKE_C_FLAGS=-O2 -g0', '-DCMAKE_CXX_FLAGS=-O2 -g0'])
        run('host-generators', [args.ninja, '-C', host, '-j' + str(args.jobs),
            'llvm-min-tblgen', 'llvm-tblgen', 'clang-tblgen'])
        prefix = str(args.compiler_prefix.resolve())
        cross = ['-DCMAKE_SYSTEM_NAME=Linux', '-DCMAKE_SYSTEM_PROCESSOR=x86_64',
            '-DLLVM_NATIVE_TOOL_DIR=' + str(host / 'bin'),
            '-DLLVM_TABLEGEN=' + str(host / 'bin/llvm-tblgen'),
            '-DCLANG_TABLEGEN=' + str(host / 'bin/clang-tblgen'),
            '-DCMAKE_FIND_ROOT_PATH=' + str(args.compiler_prefix.resolve().parent.parent.parent / 'sysroot'),
            '-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER', '-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY',
            '-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY', '-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY']
    # GCC's SDK archives contain zlib-compressed DWARF. LLD needs real zlib
    # support even when linking an ordinary static C++ program from that SDK.
    zlib_pin = ROOT / 'userland/vendor/qemu-sources.json'
    zlib_record = json.loads(zlib_pin.read_text())['zlib']
    zlib_archive = archive(zlib_record, out / 'vendor')
    zlib_source = out / zlib_record['source']
    if not zlib_source.is_dir():
        unpack(zlib_archive, out)
    zlib_objects = out / 'zlib-objects'
    zlib_objects.mkdir(exist_ok=True)
    zlib_runtime = out / 'zlib-runtime'
    zlib_environment = {**environment, 'CC': prefix + 'gcc', 'AR': prefix + 'ar', 'RANLIB': prefix + 'ranlib',
        'CFLAGS': '-O2 -g0 -fPIC -mno-avx -mno-avx2', 'LDFLAGS': '-static -no-pie'}
    if not args.native:
        zlib_environment['CHOST'] = 'x86_64-linux-musl'
    run('zlib-configure', [zlib_source / 'configure', '--static', '--prefix=/usr'], zlib_objects, zlib_environment)
    run('zlib-build', ['make', '-j' + str(args.jobs)], zlib_objects, zlib_environment)
    run('zlib-install', ['make', 'DESTDIR=' + str(zlib_runtime), 'install'], zlib_objects, zlib_environment)
    compression = ['-DLLVM_ENABLE_ZLIB=FORCE_ON', '-DZLIB_LIBRARY=' + str(zlib_runtime / 'usr/lib/libz.a'),
        '-DZLIB_INCLUDE_DIR=' + str(zlib_runtime / 'usr/include')]
    identity = dict(source_sha256=record['sha256'], port_sha256=digest(PATCH), flags=flags,
        compiler=prefix, native=args.native, options=common, cross=cross, zlib=zlib_record, compression=compression)
    configuration = out / 'CONFIGURE.json'
    if configuration.exists() and json.loads(configuration.read_text()) != identity:
        if not args.reconfigure:
            raise ValueError('LLVM build configuration changed; select a new output directory or use --reconfigure')
        history = out / 'configuration-history'
        history.mkdir(exist_ok=True)
        snapshot = history / (digest(configuration) + '.json')
        if not snapshot.exists():
            shutil.copyfile(configuration, snapshot)
    configuration.write_text(json.dumps(identity, indent=2) + '\n')
    run('configure', [args.cmake, '-S', source / 'llvm', '-B', build, *common, *cross, *compression,
        '-DCMAKE_INSTALL_PREFIX=/usr', '-DCMAKE_C_COMPILER=' + prefix + 'gcc',
        '-DCMAKE_CXX_COMPILER=' + prefix + 'g++', '-DCMAKE_AR=' + prefix + 'ar',
        '-DCMAKE_RANLIB=' + prefix + 'ranlib', '-DCMAKE_C_FLAGS=' + flags,
        '-DCMAKE_CXX_FLAGS=' + flags, '-DCMAKE_EXE_LINKER_FLAGS=-static -no-pie',
        '-DLLVM_HOST_TRIPLE=x86_64-unknown-linux-musl', '-DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-unknown-linux-musl',
        '-DCLANG_DEFAULT_CXX_STDLIB=libstdc++', '-DCLANG_DEFAULT_RTLIB=libgcc',
        '-DCLANG_DEFAULT_UNWINDLIB=none', '-DCLANG_DEFAULT_PIE_ON_LINUX=OFF',
        '-DCLANG_CONFIG_FILE_SYSTEM_DIR=/usr/lib/coast/clang',
        '-DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF'])
    run('build', [args.ninja, '-C', build, '-j' + str(args.jobs),
        'clang', 'lld', 'llvm-ar', 'llvm-objcopy', 'llvm-readobj', 'llvm-nm', 'llvm-tblgen', 'clang-tblgen'])
    runtime = out / 'runtime'
    binary_root = runtime / 'usr/bin'
    binary_root.mkdir(parents=True, exist_ok=True)
    for name in ['clang', 'lld', 'llvm-ar', 'llvm-objcopy', 'llvm-readobj', 'llvm-nm', 'llvm-tblgen', 'clang-tblgen']:
        binary = binary_root / name
        shutil.copyfile(build / 'bin' / name, binary)
        binary.chmod(0o755)
        subprocess.run(['strip', '--strip-debug', str(binary)], check=True)
        header = subprocess.check_output(['readelf', '-h', '-lW', str(binary)], text=True)
        if 'Advanced Micro Devices X86-64' not in header or 'INTERP' in header or ' RWE ' in header:
            raise ValueError('LLVM target is not static x86-64: ' + name)
    shutil.copytree(build / 'lib/clang', runtime / 'usr/lib/clang', dirs_exist_ok=True, symlinks=False)
    aliases = {'usr/bin/clang++': 'clang', 'usr/bin/clang-cpp': 'clang',
        'usr/bin/ld.lld': 'lld', 'usr/bin/llvm-ranlib': 'llvm-ar', 'usr/bin/llvm-strip': 'llvm-objcopy'}
    for name, target in aliases.items():
        path = runtime / name
        path.unlink(missing_ok=True)
        path.symlink_to(target)
    configuration_root = runtime / 'usr/lib/coast/clang'
    configuration_root.mkdir(parents=True, exist_ok=True)
    for name in ['clang.cfg', 'clang++.cfg']:
        (configuration_root / name).write_text('--gcc-toolchain=/usr\n-static\n-fno-pie\n-no-pie\n-mno-avx\n-mno-avx2\n')
    notices = runtime / 'usr/share/licenses/llvm'
    (notices / 'zlib').mkdir(parents=True, exist_ok=True)
    shutil.copyfile(zlib_source / 'LICENSE', notices / 'zlib/LICENSE')
    for project in ['llvm', 'clang', 'lld', 'third-party']:
        for path in (source / project).rglob('LICENSE*'):
            if path.is_file():
                destination = notices / path.relative_to(source)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, destination)
    inputs = [Path(__file__), SOURCES, PATCH, zlib_pin, ROOT / 'tools/linux_uapi.py',
        ROOT / 'tools/source_archive.py', ROOT / 'tools/source_tree.py']
    (out / 'BUILD.json').write_text(json.dumps(dict(version=record['version'], source=record,
        files={str(p.relative_to(runtime)): dict(path=str(p), sha256=digest(p),
            mode=0o755 if p.stat().st_mode & 0o111 else 0o644)
            for p in sorted(runtime.rglob('*')) if p.is_file() and not p.is_symlink()}, aliases=aliases,
        commands=commands, source_sha256={str(p.relative_to(ROOT)): digest(p) for p in inputs},
        build_mode='native-coast' if args.native else 'linux-cross',
        host_generator_execution='Coast' if args.native else 'Linux',
        target_host_executed=args.native, state='compiled; Coast qualification pending'), indent=2) + '\n')
    print('Static LLVM tools compiled; validate inside Coast', flush=True)


if __name__ == '__main__':
    main()
