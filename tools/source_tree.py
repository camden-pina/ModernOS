#!/usr/bin/env python3
"""Audit and export the publishable Coast source tree, without build caches."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
EPOCH = 1793923200
MANIFEST = 'SOURCE-MANIFEST.json'
COMPILED = (b'\x7fELF', b'MZ', b'!<arch>\n')
ARCHIVES = ('.tar', '.tar.gz', '.tar.xz', '.tar.bz2', '.tgz', '.zip', '.whl', '.deb')
GENERATED = {'build', '__pycache__', '.git', '.cache', '.codex', '.aws'}
PRIVATE_NAMES = {'private-keys-v1.d', 'openpgp-revocs.d', 'id_rsa', 'id_ed25519', 'secring.gpg', 'trustdb.gpg'}
SECRETS = re.compile(rb'-----BEGIN (?:[A-Z ]*PRIVATE KEY|PGP PRIVATE KEY BLOCK)-----'
                     rb'|\bgh[pousr]_[A-Za-z0-9]{30,}\b'
                     rb'|\bAKIA[A-Z0-9]{16}\b')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def files(root=ROOT):
    """Include tracked files even if subsequently ignored, plus new sources."""
    root = Path(root).resolve()
    if (root / '.git').exists():
        names = subprocess.check_output(['git', '-C', str(root), 'ls-files', '-c', '-o',
                                         '--exclude-standard', '-z']).decode().split('\0')
        return sorted({name for name in names if name and ((root / name).exists() or (root / name).is_symlink())})
    manifest = root / MANIFEST
    if manifest.is_file():
        return sorted(json.loads(manifest.read_text())['files'])
    raise ValueError('Expected a Git checkout or an exported SOURCE-MANIFEST.json')


def audit(root=ROOT):
    root = Path(root).resolve()
    inventory, errors = {}, []
    bundled_sources = {}
    if not (root / '.git').exists() and (root / MANIFEST).is_file():
        kit = json.loads((root / MANIFEST).read_text())
        if kit.get('kernel_sources'):
            from fetch_sources import read_manifest
            pin = read_manifest(root / 'userland/vendor/sources.json')['lwip-2.2.1.tar.gz']
            bundled_sources['userland/vendor/' + pin['archive']] = pin['sha256']
    for name in files(root):
        relative = PurePosixPath(name)
        path = root / name
        if (any(p in PRIVATE_NAMES for p in relative.parts) or path.suffix in ('.key', '.p12', '.pfx')
                or path.name == '.env' or path.name.startswith('.env.') and path.name != '.env.example'):
            errors.append(name + ': private machine/signing file')
            continue
        if (relative.is_absolute() or '..' in relative.parts or any(p in GENERATED for p in relative.parts)
                or name.startswith(('userland/runtime/', 'userland/bin/', 'userland/obj/'))):
            errors.append(name + ': generated or unsafe path')
            continue
        if path.is_symlink() or not path.is_file():
            errors.append(name + ': source exports require regular files')
            continue
        size = path.stat().st_size
        if size > 8 * 1024 * 1024:
            errors.append(name + ': file exceeds the source tree size budget (8 MiB)')
            continue
        data = path.read_bytes()
        if data.startswith(COMPILED) or name.endswith(('.iso', '.img', '.fd', '.o', '.a', '.so', '.efi')):
            errors.append(name + ': compiled output belongs in build/')
        content_sha256 = hashlib.sha256(data).hexdigest()
        if name.endswith(ARCHIVES) and bundled_sources.get(name) != content_sha256:
            errors.append(name + ': download belongs in the ignored vendor cache')
        if SECRETS.search(data):
            errors.append(name + ': possible secret; content withheld')
        inventory[name] = dict(sha256=content_sha256, bytes=size,
                               mode=0o755 if path.stat().st_mode & 0o111 else 0o644)
    if errors:
        raise ValueError('\n'.join(errors))
    return inventory


def export(destination, root=ROOT, kernel_sources=False):
    root, destination = Path(root).resolve(), Path(destination).resolve()
    inventory = audit(root)
    extras = {}
    if kernel_sources:
        # This source archive is needed by the kernel. Git keeps its hash/URL;
        # a repository source kit also includes the bytes for offline builds.
        from fetch_sources import read_manifest, fetch
        entry = read_manifest(root / 'userland/vendor/sources.json')['lwip-2.2.1.tar.gz']
        fetch(entry, root / 'userland/vendor')
        name = 'userland/vendor/' + entry['archive']
        extras[name] = root / name
        inventory[name] = dict(sha256=digest(extras[name]), bytes=extras[name].stat().st_size, mode=0o644)
    record = dict(format=1, epoch=int(os.environ.get('SOURCE_DATE_EPOCH', EPOCH)),
                  files=inventory, kernel_sources=kernel_sources, compiled_files=False)
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Atomic installation also prevents an interrupted export becoming a source
    # package that appears complete to the repository publisher.
    temporary = destination.with_name(destination.name + '.part')
    try:
        with tarfile.open(temporary, 'w:xz', format=tarfile.PAX_FORMAT) as archive:
            for name, metadata in sorted(inventory.items()):
                info = tarfile.TarInfo('coast/' + name)
                info.mode, info.mtime, info.size = metadata['mode'], record['epoch'], metadata['bytes']
                with (extras.get(name) or root / name).open('rb') as stream:
                    archive.addfile(info, stream)
            data = (json.dumps(record, indent=2, sort_keys=True) + '\n').encode()
            info = tarfile.TarInfo('coast/' + MANIFEST)
            info.mode, info.mtime, info.size = 0o644, record['epoch'], len(data)
            archive.addfile(info, io.BytesIO(data))
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--output', type=Path, help='write a deterministic source tar.xz')
    parser.add_argument('--kernel-sources', action='store_true', help='include verified lwIP for an offline kernel build')
    args = parser.parse_args()
    if args.output:
        record = export(args.output, args.root, args.kernel_sources)
        print('Source archive:', args.output)
        inventory = record['files']
    else:
        inventory = audit(args.root)
    print(f'SOURCE AUDIT PASS: {len(inventory)} files, {sum(v["bytes"] for v in inventory.values())} bytes; no compiled programs or signing secrets')


if __name__ == '__main__':
    main()
