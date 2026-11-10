#!/usr/bin/env python3
"""Build Chromium's exact GN revision for Coast's threaded native C++ SDK."""
from build_state import project_lock
from build_state import atomic_write
import argparse
import fcntl
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys

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
    parser.add_argument('--ninja', default='ninja')
    parser.add_argument('--native-toolchain', type=Path, help='alternate native compiler bin directory')
    parser.add_argument('--compiler-prefix', type=Path,
                        default=ROOT / 'build/compiler-threaded/cross/bin/x86_64-linux-musl-')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 24 or args.native and platform.system() != 'Coast':
        parser.error('Use --jobs 1..24; --native requires Coast')
    out = (args.output_root or ROOT / ('build/gn-native' if args.native else 'build/gn')).resolve()
    out.mkdir(parents=True, exist_ok=True)
    lock = (out / '.build.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    record = json.loads(SOURCES.read_text())['gn']
    vendor = out / 'vendor'
    vendor.mkdir(exist_ok=True)
    cached = ROOT / 'build/build-tools/vendor' / record['archive']
    if cached.is_file() and not (vendor / record['archive']).exists():
        if digest(cached) != record['sha256']:
            raise ValueError('GN source cache checksum mismatch')
        shutil.copyfile(cached, vendor / cached.name)
    fetch(record, vendor)
    source = out / record['source']
    if not source.exists():
        source.mkdir()
        unpack(vendor / record['archive'], source)
    patch = ROOT / 'ports/build-tools/gn-spawn.patch'
    stamp = source / '.coast-spawn'
    if not stamp.exists():
        subprocess.run(['patch', '-p1', '-i', str(patch)], cwd=source, check=True)
        stamp.write_text(digest(patch))
    if stamp.read_text() != digest(patch):
        raise ValueError('GN spawn patch changed; select a fresh output directory')
    prefix = (str(args.native_toolchain.resolve()) + '/' if args.native_toolchain else '/usr/bin/') if args.native else str(args.compiler_prefix.resolve())
    headers = prepare_uapi(out / 'linux-uapi')
    flags = '-O2 -g0 -fno-pie -mno-avx -mno-avx2 -D_GNU_SOURCE -DCOAST_NATIVE -idirafter ' + str(headers)
    environment = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
        CC=prefix + 'gcc', CXX=prefix + 'g++', AR=prefix + 'ar',
        CXXFLAGS=flags, LDFLAGS='-static -no-pie', PYTHONDONTWRITEBYTECODE='1')
    confinement = [] if args.native else ['bwrap', '--unshare-all', '--die-with-parent', '--new-session',
        '--ro-bind', '/', '/', '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    build = out / 'objects'
    build.mkdir(exist_ok=True)
    # Gitiles source exports omit .git; preserve the parent DEPS version in the
    # generated header instead of letting upstream label this build UNKNOWN.
    (build / 'last_commit_position.h').write_text('#define LAST_COMMIT_POSITION_NUM ' + record['version'] + '\n'
        '#define LAST_COMMIT_POSITION "' + record['version'] + ' (' + record['commit'][:12] + ')"\n')
    commands = []

    def run(name, command):
        command = list(map(str, command))
        commands.append(command)
        print('GN ' + name, flush=True)
        with (out / (name + '.log')).open('w') as log:
            subprocess.run([*confinement, '--chdir', str(source), '--', *command] if confinement else command,
                cwd=source, env=environment, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, check=True)

    run('configure', [sys.executable, source / 'build/gen.py', '--host=linux', '--platform=linux',
        '--out-path=' + str(build), '--no-last-commit-position', '--allow-warnings'])
    run('build', [args.ninja, '-C', build, '-j' + str(args.jobs), 'gn'])
    runtime = out / 'runtime'
    binary = runtime / 'usr/bin/gn'
    binary.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(build / 'gn', binary)
    binary.chmod(0o755)
    header = subprocess.check_output(['readelf', '-h', '-l', str(binary)], text=True)
    if 'Advanced Micro Devices X86-64' not in header or 'INTERP' in header or ' RWE ' in header:
        raise ValueError('GN target is not a static x86-64 executable')
    license = runtime / 'usr/share/licenses/gn/LICENSE'
    license.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source / 'LICENSE', license)
    inputs = [Path(__file__), SOURCES, patch, ROOT / 'tools/linux_uapi.py',
              ROOT / 'tools/fetch_sources.py', ROOT / 'tools/source_archive.py']
    atomic_write(out / 'BUILD.json', json.dumps(dict(version=record['version'], source=record,
        files={str(p.relative_to(runtime)): dict(path=str(p), sha256=digest(p),
            mode=0o755 if p.stat().st_mode & 0o111 else 0o644)
            for p in sorted(runtime.rglob('*')) if p.is_file()}, commands=commands,
        source_sha256={str(p.relative_to(ROOT)): digest(p) for p in inputs},
        build_mode='native-coast' if args.native else 'linux-cross', target_host_executed=args.native,
        state='compiled; guest qualification pending'), indent=2) + '\n')
    print('GN compiled; validate it inside Coast', flush=True)


if __name__ == '__main__':
    with project_lock():
        main()
