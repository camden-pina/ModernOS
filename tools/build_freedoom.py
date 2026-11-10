#!/usr/bin/env python3
"""Build the complete BSD-licensed Freedoom 2 IWAD from verified local sources.

Normal builds use only local archives and perform no host installation.
The explicit --fetch-only mode retrieves missing official source files over HTTPS.  DeuTex is compiled
inside the build directory; Python/Pillow, autotools, a C compiler, make,
pkg-config, and libpng development headers are host build prerequisites.
"""
from source_archive import unpack
from build_state import project_lock
from build_state import atomic_write
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'userland/vendor'
BUILD = ROOT / 'build/doom-freedoom'
SOURCES = {'deutex': {'archive': 'deutex-5.2.3-source.tar.xz',
            'directory': 'deutex-5.2.3',
            'manifest': 'deutex-5.2.3-source.json',
            'manifest_sha256': 'c52c55d036d9057442cb9cb4d62a1800914167c8aa541032f711f98ef7817ce7',
            'sha256': '59f1544ea1a074c30599cdbcd2628c46938f72327e9b4d5032d402552e486813'},
 'freedoom': {'archive': 'freedoom-0.13.0-source.tar.xz',
              'directory': 'freedoom-0.13.0',
              'manifest': 'freedoom-0.13.0-source.json',
              'manifest_sha256': '92f00a51f90b2c083d014dfe6c93cfece5c3ff66b4069067a23522fd8f7d658e',
              'sha256': 'ff7fb2d98b1fdd3031e4ecb79c8e8d256cd40b9056e6e73c8a8b7071b304b70b'}}

OFFICIAL_RELEASE_SHA256 = 'a8772e088847032510d97ba2312406a6998f21cbab44d4ff10696faa9c0ecd4b'
MAP_LUMPS = ('THINGS', 'LINEDEFS', 'SIDEDEFS', 'VERTEXES', 'SEGS',
             'SSECTORS', 'NODES', 'SECTORS', 'REJECT', 'BLOCKMAP')


def sha256(path):
    return hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()


def create_archive(directory, output, manifest):
    with tarfile.open(output, 'w:xz', format=tarfile.USTAR_FORMAT, preset=6) as archive:
        for item in manifest['files']:
            data = (directory / item['path']).read_bytes()
            entry = tarfile.TarInfo(manifest['directory'] + '/' + item['path'])
            entry.size = len(data)
            entry.mode = 0o755 if item['mode'] == '100755' else 0o644
            archive.addfile(entry, io.BytesIO(data))


def fetch_archive(spec, manifest):
    print(f'Fetching pinned official sources: {spec["archive"]}', flush=True)
    with tempfile.TemporaryDirectory(prefix='coast-freedoom-fetch-') as temporary:
        directory = Path(temporary)

        def fetch(item):
            request = urllib.request.Request(item['raw_url'], headers={'User-Agent': 'Coast-source-fetch'})
            for attempt in range(4):
                try:
                    with urllib.request.urlopen(request, timeout=30) as response:
                        data = response.read(item['size'] + 1)
                    break
                except (OSError, urllib.error.URLError):
                    if attempt == 3:
                        raise
                    time.sleep(0.5 * 2 ** attempt)
            actual = hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()
            if len(data) != item['size'] or actual != item['sha']:
                raise ValueError(f'Official source hash mismatch: {item["path"]}')
            target = directory / item['path']
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)

        with ThreadPoolExecutor(max_workers=8) as pool:
            for count, _ in enumerate(pool.map(fetch, manifest['files']), 1):
                if count % 100 == 0:
                    print(f'  {count}/{len(manifest["files"])} files verified', flush=True)
        output = directory / spec['archive']
        create_archive(directory, output, manifest)
        if sha256(output) != spec['sha256']:
            raise SystemExit(f'Reconstructed archive checksum mismatch: {output}')
        VENDOR.mkdir(parents=True, exist_ok=True)
        temporary_output = VENDOR / (spec['archive'] + '.tmp')
        shutil.copyfile(output, temporary_output)
        os.replace(temporary_output, VENDOR / spec['archive'])


def verify_sources(fetch_missing=False):
    manifests = {}
    for name, spec in SOURCES.items():
        archive = VENDOR / spec['archive']
        manifest_file = VENDOR / spec['manifest']
        if not manifest_file.is_file() or sha256(manifest_file) != spec['manifest_sha256']:
            raise SystemExit(f'Missing or changed pinned source manifest: {manifest_file}')
        manifest = json.loads(manifest_file.read_text())
        if not archive.is_file() and fetch_missing:
            fetch_archive(spec, manifest)
        if not archive.is_file() or sha256(archive) != spec['sha256']:
            raise SystemExit(f'Missing or changed pinned source: {archive}')
        manifests[name] = manifest
    return manifests


def extract_sources(manifests):
    source_root = BUILD / 'source'
    source_root.mkdir(parents=True, exist_ok=True)
    result = {}
    for name, spec in SOURCES.items():
        directory = source_root / spec['directory']
        if directory.exists():
            shutil.rmtree(directory)
        unpack(VENDOR / spec['archive'], source_root, filter='data')
        for item in manifests[name]['files']:
            path = directory / item['path']
            if not path.is_file() or path.is_symlink():
                raise SystemExit(f'Missing source file: {path}')
            data = path.read_bytes()
            actual = hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()
            if len(data) != item['size'] or actual != item['sha']:
                raise SystemExit(f'Official Git blob verification failed: {path}')
        result[name] = directory
    return result


def run(where, log, command, env=None):
    with (BUILD / log).open('wb') as output:
        status = subprocess.run(command, cwd=where, env=env,
                                stdout=output, stderr=subprocess.STDOUT)
    if status.returncode:
        raise SystemExit(f'{command[0]} failed; see {BUILD / log}')


def inspect_wad(path):
    data = path.read_bytes()
    if len(data) < 12:
        raise SystemExit('Freedoom IWAD is truncated')
    magic, count, offset = struct.unpack_from('<4sII', data)
    if magic != b'IWAD' or not 1000 < count < 20000:
        raise SystemExit('Invalid Freedoom IWAD header')
    if offset < 12 or offset + count * 16 > len(data):
        raise SystemExit('Invalid Freedoom IWAD directory')
    lumps = []
    for number in range(count):
        start, size, name = struct.unpack_from('<II8s', data, offset + number * 16)
        name = name.rstrip(b'\0').decode('ascii')
        if start > len(data) or size > len(data) - start:
            raise SystemExit(f'Out-of-bounds IWAD lump {name}')
        lumps.append((name, start, size))
    names = [item[0] for item in lumps]
    expected_maps = [f'MAP{i:02d}' for i in range(1, 33)]
    maps = [name for name in names if name.startswith('MAP') and len(name) == 5
            and name[3:].isdigit()]
    if maps != expected_maps:
        raise SystemExit(f'Incomplete Freedoom campaign: {maps}')
    map_counts = {}
    for name in expected_maps:
        start = names.index(name)
        entries = lumps[start + 1:start + 11]
        if tuple(x[0] for x in entries) != MAP_LUMPS:
            raise SystemExit(f'Invalid map lump sequence: {name}')
        if any(entries[i][2] == 0 for i in (0, 1, 2, 3, 4, 5, 6, 7, 9)):
            raise SystemExit(f'Missing map geometry or nodes: {name}')
        things = entries[0]
        if things[2] % 10:
            raise SystemExit(f'Misaligned THINGS lump: {name}')
        records = list(struct.iter_unpack('<hhHHH', data[things[1]:things[1] + things[2]]))
        if not any(item[3] == 1 for item in records):
            raise SystemExit(f'Missing player one start: {name}')
        map_counts[name] = {'things': len(records),
                            'linedefs': entries[1][2] // 14,
                            'sectors': entries[7][2] // 26}
    for name in ('PLAYPAL', 'COLORMAP', 'TEXTURE1', 'PNAMES', 'TITLEPIC',
                 'DEHACKED', 'D_RUNNIN', 'DSPISTOL'):
        if name not in names or not lumps[names.index(name)][2]:
            raise SystemExit(f'Missing game resource: {name}')
    digest = hashlib.sha256(data).hexdigest()
    return {'format': 'IWAD', 'bytes': len(data), 'lumps': count,
            'sha256': digest, 'map_count': len(maps), 'maps': map_counts,
            'official_release_sha256': OFFICIAL_RELEASE_SHA256,
            'matches_official_release': digest == OFFICIAL_RELEASE_SHA256}


def package_notices(report):
    target = BUILD / 'notices'
    target.mkdir(exist_ok=True)
    spec = SOURCES['freedoom']
    with tarfile.open(VENDOR / spec['archive'], 'r:xz') as archive:
        for name in ('COPYING.adoc', 'CREDITS', 'CREDITS-MUSIC', 'README.adoc', 'COMPILING.adoc'):
            data = archive.extractfile(spec['directory'] + '/' + name).read()
            (target / ('FREEDOOM-' + name)).write_bytes(data)
    (target / 'FREEDOOM-NOTICE.txt').write_text(
        'Freedoom: Phase 2 v0.13.0; complete 32-map IWAD.\n'
        'Copyright 2001-2024 Contributors to the Freedoom project.\n'
        'License: BSD-3-Clause. See FREEDOOM-COPYING.adoc and FREEDOOM-CREDITS.\n'
        'Official source: https://github.com/freedoom/freedoom/tree/v0.13.0\n'
        'Source commit: ' + report['source_commit'] + '\n'
        'Built from the complete official source, with each source file verified\n'
        'against its pinned Git blob ID and size. Generated title graphics depend\n'
        'on host Pillow, so the WAD hash may differ from the upstream release.\n'
        'Actual IWAD SHA-256: ' + report['sha256'] + '\n'
        'Exact build/source provenance: freedoom-provenance.json\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--force', action='store_true', help='rebuild all data and the local DeuTex tool')
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--verify-only', action='store_true', help='verify pinned sources and the existing complete IWAD')
    modes.add_argument('--fetch-only', action='store_true', help='fetch missing official source files and reconstruct pinned archives')
    modes.add_argument('--check-sources-only', action='store_true', help='verify pinned source manifests and archives without building')
    parser.add_argument('--jobs', type=int, default=1, help='local DeuTex compiler parallelism (default: 1)')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    manifests = verify_sources(fetch_missing=args.fetch_only)
    if args.fetch_only or args.check_sources_only:
        print('Freedoom and DeuTex source archives verified.')
        return
    BUILD.mkdir(parents=True, exist_ok=True)
    wad = BUILD / 'freedoom2.wad'
    stamp = BUILD / 'freedoom-build.stamp'
    key = hashlib.sha256(Path(__file__).read_bytes() +
                         json.dumps(SOURCES, sort_keys=True).encode()).hexdigest()
    rebuild = args.force or not wad.is_file() or not stamp.is_file() or stamp.read_text() != key
    if args.verify_only and not wad.is_file():
        raise SystemExit(f'Build Freedoom first: {wad}')
    if rebuild and not args.verify_only:
        import PIL
        paths = extract_sources(manifests)
        deutex = paths['deutex']
        env = os.environ.copy()
        for name in ('CC', 'CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LIBS', 'CONFIG_SITE',
                     'CROSS_COMPILE', 'MAKEFLAGS', 'MFLAGS'):
            env.pop(name, None)
        env.update(LC_ALL='C', PYTHONDONTWRITEBYTECODE='1')
        run(deutex, 'deutex-bootstrap.log', ['./bootstrap'], env)
        run(deutex, 'deutex-configure.log', ['./configure', '--disable-dependency-tracking'], env)
        run(deutex, 'deutex-build.log', ['make', '-C', 'src', f'-j{args.jobs}'], env)
        run(paths['freedoom'], 'freedoom-build.log',
            ['make', '-j1', 'VERSION=v0.13.0', f'DEUTEX={deutex / "src/deutex"}',
             'wads/freedoom2.wad'], env)
        run(paths['freedoom'], 'freedoom-map-check.log',
            [sys.executable, 'scripts/test-vanilla-compliance', 'levels'], env)
        shutil.copyfile(paths['freedoom'] / 'wads/freedoom2.wad', wad)
        stamp.write_text(key)
        atomic_write(BUILD / 'host-build-tools.json', json.dumps({
            'python': platform.python_version(), 'pillow': PIL.__version__,
            'deutex': '5.2.3', 'version_argument': 'v0.13.0',
        }, indent=2) + '\n')
    report = inspect_wad(wad)
    report.update({'game': 'Freedoom: Phase 2', 'version': '0.13.0',
                   'license': 'BSD-3-Clause', 'source_built': True,
                   'source_commit': manifests['freedoom']['commit'],
                   'source_tree': manifests['freedoom']['tree'],
                   'source_archives': SOURCES,
                   'notice': 'Locally built from the complete official sources; the WAD hash may differ from the upstream release because generated title graphics depend on host Pillow.'})
    host_tools = BUILD / 'host-build-tools.json'
    if host_tools.exists():
        report['build_tools'] = json.loads(host_tools.read_text())
    package_notices(report)
    atomic_write(BUILD / 'freedoom-data.json', json.dumps(report, indent=2) + '\n')
    atomic_write(BUILD / 'freedoom-provenance.json', json.dumps(report, indent=2) + '\n')
    print(f'Freedoom Phase 2: {report["map_count"]} maps, {report["lumps"]} lumps, '
          f'{report["bytes"]} bytes; SHA-256 {report["sha256"]}')


if __name__ == '__main__':
    with project_lock():
        main()
