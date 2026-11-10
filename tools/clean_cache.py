#!/usr/bin/env python3
"""Preview or remove regenerable caches while retaining source build products.

Print a dry-run inventory by default. --apply removes only the listed generated
paths. Toolchains, signing keys, selected images and package services are kept.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil

from source_tree import files

ROOT = Path(__file__).resolve().parents[1]


def candidates(root=ROOT, intermediates=False):
    root = Path(root).resolve()
    build = root / 'build'
    if not build.is_dir():
        return
    protected = {p.resolve() for p in build.iterdir() if p.is_symlink()}
    tracked = [root / name for name in files(root)]
    selected = set()
    private = {'private-keys-v1.d', 'openpgp-revocs.d', 'coast-signing-keys',
               'state', 'credentials', 'local-notes', 'cleanup-records'}
    skip = private | {'.git', 'node_modules', 'checkout', 'src', 'sources', 'upstream', 'vendor'}
    for base in (build, root / 'tools', root / 'ports', root / 'userland/packages'):
        if not base.is_dir():
            continue
        for current, directories, _ in os.walk(base, followlinks=False):
            current = Path(current)
            if any(current == p or current.is_relative_to(p) for p in protected):
                directories[:] = []
                continue
            for name in list(directories):
                path = current / name
                if path.is_symlink() or name in skip:
                    directories.remove(name)
                elif name in {'__pycache__', '.pytest_cache', 'npm-cache', 'thinlto-cache'}:
                    selected.add(path)
                    directories.remove(name)
    if intermediates:
        from build_desktop import verify_receipt
        for directory in build.glob('electron-source-*'):
            receipt = directory / 'BUILD.json'
            if not receipt.is_file():
                continue
            record = verify_receipt(receipt)
            products = record.get('products', {})
            if not {'electron', 'node'}.issubset(products) or any(
                    not isinstance(products[name], dict)
                    or not products[name].get('path') or not products[name].get('sha256')
                    for name in ('electron', 'node')):
                continue
            references = []

            def paths(value):
                if isinstance(value, dict):
                    for child in value.values():
                        paths(child)
                elif isinstance(value, list):
                    for child in value:
                        paths(child)
                elif isinstance(value, str) and value.startswith(str(directory)):
                    path = Path(value)
                    if path.exists():
                        references.append(path.resolve())

            paths(record)
            for product in ('electron', 'node'):
                for pattern in ('obj', 'thinlto-cache', '*-dwo'):
                    for path in (directory / product).glob(pattern):
                        if path.is_dir() and not any(p == path or p.is_relative_to(path) for p in references):
                            selected.add(path)
    for path in sorted(selected):
        if path.is_symlink() or not path.resolve().is_relative_to(root):
            continue
        if any(p == path or p.is_relative_to(path) or path.is_relative_to(p) for p in protected):
            continue
        if any(p == path or p.is_relative_to(path) for p in tracked):
            continue
        if any(p.name in private for p in path.parents):
            continue
        if any(p.name in private or p.name in {'secring.gpg', 'id_rsa', 'id_ed25519', '.env'}
               for p in path.rglob('*')):
            continue
        yield path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true')
    parser.add_argument('--intermediates', action='store_true', help='also prune verified Electron/Node object and split-debug caches')
    args = parser.parse_args()
    before = shutil.disk_usage(ROOT).free
    records = []
    for path in candidates(intermediates=args.intermediates):
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
