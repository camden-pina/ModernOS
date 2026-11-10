#!/usr/bin/env python3
"""Build the pinned GNU application ABI libraries for Coast from Debian sources."""
from build_state import project_lock
from build_state import atomic_write
import argparse
import fcntl
import json
from pathlib import Path
import platform
import shlex
import shutil
import subprocess

from fetch_sources import fetch
from linux_uapi import prepare as prepare_uapi
from source_archive import unpack
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / 'ports/gnu-runtime/glibc-source.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 24 or args.native and platform.system() != 'Coast':
        parser.error('Use --jobs 1..24; --native requires Coast')
    out = (args.output_root or ROOT / ('build/glibc-native' if args.native else 'build/glibc')).resolve()
    out.mkdir(parents=True, exist_ok=True)
    lock = (out / '.build.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    record = json.loads(SOURCES.read_text())
    vendor = out / 'vendor'
    vendor.mkdir(exist_ok=True)
    archives = record['archives']
    for name, asset in archives.items():
        cached = ROOT / 'build/gnu-runtime/vendor' / name
        if not (vendor / name).exists() and cached.is_file():
            if digest(cached) != asset['sha256']:
                raise ValueError('GNU libc source cache checksum mismatch')
            shutil.copyfile(cached, vendor / name)
        fetch(asset, vendor)
    source = out / record['source']
    if not source.exists():
        unpack(vendor / 'glibc_2.36.orig.tar.xz', out)
    if not (source / 'debian').exists():
        unpack(vendor / 'glibc_2.36-9+deb12u14.debian.tar.xz', source)
    series = source / 'debian/patches/series'
    stamp = source / '.coast-debian-series'
    if not stamp.exists():
        with (out / 'debian-patches.log').open('w') as log:
            for line in series.read_text().splitlines():
                fields = shlex.split(line, comments=True)
                if not fields:
                    continue
                patch = (source / 'debian/patches' / fields[0]).resolve(strict=True)
                if not patch.is_relative_to(source.resolve() / 'debian/patches'):
                    raise ValueError('Invalid Debian patch path')
                subprocess.run(['patch', '--batch', '--forward', '--no-backup-if-mismatch',
                    '-p1', *fields[1:], '-i', str(patch)], cwd=source, check=True,
                    stdout=log, stderr=subprocess.STDOUT)
        stamp.write_text(digest(series))
    if stamp.read_text() != digest(series):
        raise ValueError('Debian patch series changed')
    port = ROOT / 'ports/gnu-runtime/glibc-nx-stack.patch'
    stamp = source / '.coast-nx-stack'
    if not stamp.exists():
        subprocess.run(['patch', '--batch', '-p1', '-i', str(port)], cwd=source, check=True)
        stamp.write_text(digest(port))
    if stamp.read_text() != digest(port):
        raise ValueError('GNU libc port changed; select a fresh output directory')
    headers = prepare_uapi(out / 'linux-uapi')
    build, runtime = out / 'objects', out / 'runtime'
    build.mkdir(exist_ok=True)
    environment = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
        # glibc uses C++ only for optional tests. A newer host libstdc++ can
        # require a newer glibc than the target runtime being built here.
        CC='/usr/bin/gcc', CXX='false', BUILD_CC='/usr/bin/gcc',
        AR='/usr/bin/ar', RANLIB='/usr/bin/ranlib', MAKEINFO='true',
        CFLAGS='-O2 -g0 -std=gnu11 -march=x86-64 -mno-avx -mno-avx2 -fno-stack-protector',
        LDFLAGS='-Wl,-z,noexecstack', libc_cv_slibdir='/usr/lib/x86_64-linux-gnu',
        libc_cv_rtlddir='/usr/lib/x86_64-linux-gnu')
    confinement = [] if args.native else ['bwrap', '--unshare-all', '--die-with-parent', '--new-session',
        '--ro-bind', '/', '/', '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    commands = []

    def run(name, command):
        command = list(map(str, command))
        commands.append(command)
        print('GNU libc ' + name, flush=True)
        with (out / (name + '.log')).open('w') as log:
            subprocess.run([*confinement, '--chdir', str(build), '--', *command] if confinement else command,
                cwd=build, env=environment, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, check=True)

    # The distinct vendor keeps glibc's cross-compilation guards active even
    # on x86-64 Linux: build helpers use BUILD_CC and target libc/ldconfig are
    # never executed on the Linux host, including during installation.
    build_triplet = 'x86_64-linux-musl' if args.native else subprocess.check_output(['gcc', '-dumpmachine'], text=True).strip()
    run('configure', [source / 'configure', '--prefix=/usr', '--libdir=/usr/lib/x86_64-linux-gnu',
        '--host=x86_64-coast-linux-gnu', '--build=' + build_triplet, '--with-headers=' + str(headers),
        '--enable-kernel=3.2.0', '--disable-werror', '--disable-multi-arch', '--disable-cet', '--disable-nscd'])
    if 'cross-compiling = yes' not in (build / 'config.make').read_text():
        raise ValueError('GNU libc target execution must stay disabled during compilation')
    run('build', ['make', '-j' + str(args.jobs)])
    run('install', ['make', 'install_root=' + str(runtime), 'install'])
    libraries = list((runtime / 'usr/lib/x86_64-linux-gnu').glob('*.so*'))
    for library in libraries:
        if not library.is_file() or library.read_bytes()[:4] != b'\x7fELF':
            continue
        header = subprocess.check_output(['readelf', '-lW', str(library)], text=True)
        if ' RWE ' in header:
            raise ValueError('GNU runtime requests a writable executable mapping: ' + str(library))
    notice = runtime / 'usr/share/licenses/coast-glibc'
    notice.mkdir(parents=True, exist_ok=True)
    for name in ['COPYING', 'COPYING.LIB', 'LICENSES']:
        shutil.copyfile(source / name, notice / name)
    inputs = [Path(__file__), SOURCES, port, ROOT / 'tools/fetch_sources.py',
        ROOT / 'tools/source_archive.py', ROOT / 'tools/linux_uapi.py']
    files = {}
    aliases = {}
    for path in sorted(runtime.rglob('*')):
        name = str(path.relative_to(runtime))
        if path.is_symlink():
            aliases[name] = str(path.readlink())
        elif path.is_file():
            files[name] = dict(path=str(path), sha256=digest(path), mode=0o755 if path.stat().st_mode & 0o111 else 0o644)
    atomic_write(out / 'BUILD.json', json.dumps(dict(version=record['version'], source=record,
        files=files, aliases=aliases, commands=commands,
        source_sha256={str(p.relative_to(ROOT)): digest(p) for p in inputs},
        build_mode='native-coast' if args.native else 'linux-cross', target_host_executed=False,
        state='compiled; Coast loader, compiler and desktop qualification pending'), indent=2) + '\n')
    print('GNU libc compiled; qualify its loader and application behavior inside Coast', flush=True)


if __name__ == '__main__':
    with project_lock():
        main()
