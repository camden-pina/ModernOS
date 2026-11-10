#!/usr/bin/env python3
"""Remove known retired-browser and failed-test caches; preserve selected builds.

Print a dry-run inventory by default. --apply removes only the listed generated
paths. Toolchains, signing keys, selected images and package services are kept.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]


def candidates(root=ROOT):
    build = root / 'build'
    protected = {p.resolve() for p in build.iterdir() if p.is_symlink()}
    candidates = set()
    for pattern in ['modern-browser/webcore-*', 'coast-repository-qemu-r*',
                    'qemu/test-r[1-5]', 'qemu/debug', 'coast-repository-server-r4',
                    'coast-repository-server-r6', 'limine-source-preview', 'modern-browser/ffmpeg-*',
                    'electron-wasm-runtime-r[5-7]-*/cached-view',
                    'node-native-r[1-4]-*/cached-view', 'coast-electron-r3/cached-view',
                    'coast-node-r1/cached-view']:
        candidates.update(build.glob(pattern))
    # Historical Chromium candidates never supply a base image to the active
    # builder. Keep their small manifests/reports while deleting obsolete ISOs.
    for directory in build.glob('coast-chromium*'):
        for image in directory.glob('*.iso'):
            if image.resolve() not in protected:
                candidates.add(image)
    for name in ['userland/vendor/netsurf-all-3.11.tar.gz',
                 'userland/vendor/ffmpeg-9.0.2.tar.xz',
                 'userland/vendor/licenses/netsurf-COPYING.txt',
                 'userland/vendor/licenses/webcore-NOTICES.txt',
                 'userland/vendor/licenses/ffmpeg-NOTICES.txt']:
        path = root / name
        if path.exists():
            candidates.add(path)
    for directory in ['tools', 'ports', 'userland/packages']:
        candidates.update((root / directory).rglob('__pycache__'))
    for path in sorted(candidates):
        if path.is_symlink() or not path.resolve().is_relative_to(root):
            continue
        if any(p == path.resolve() or p.is_relative_to(path.resolve()) for p in protected):
            continue
        yield path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    before = shutil.disk_usage(ROOT).free
    records = []
    for path in candidates():
        size = sum(p.stat().st_size for p in path.rglob('*') if p.is_file() and not p.is_symlink()) if path.is_dir() else path.stat().st_size
        records.append(dict(path=str(path.relative_to(ROOT)), bytes=size))
        if args.apply:
            if path.is_dir():
                shutil.rmtree(path)
            else:
                path.unlink()
    record = dict(applied=args.apply, paths=records, logical_bytes=sum(p['bytes'] for p in records),
                  free_bytes_before=before, free_bytes_after=shutil.disk_usage(ROOT).free)
    if args.apply:
        current = ROOT / 'build/cache-cleanup.json'
        if current.exists():
            stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
            shutil.copyfile(current, current.with_name('cache-cleanup-' + stamp + '.json'))
        current.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
