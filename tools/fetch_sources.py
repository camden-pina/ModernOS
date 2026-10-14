#!/usr/bin/env python3
"""Restore pinned upstream archives without storing downloads in Git.

Existing valid files work offline. Missing files are fetched over HTTPS and
installed only after checking their SHA-256. Existing corrupt files are left
in place so a failed check never silently replaces a local archive.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'userland/vendor'
MANIFEST = VENDOR / 'sources.json'


def read_manifest(path=MANIFEST):
    packages = {}
    for entry in json.loads(path.read_text()):
        name = entry['archive']
        url = urlsplit(entry['url'])
        if (not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._+-]*', name)
                or name in packages
                or not re.fullmatch(r'[0-9a-f]{64}', entry['sha256'])
                or url.scheme != 'https' or not url.hostname
                or url.username or url.password):
            raise ValueError(f'Invalid source record: {name}')
        packages[name] = entry
    return packages


def verify(path, digest):
    with path.open('rb') as source:
        actual = hashlib.file_digest(source, 'sha256').hexdigest()
    if actual != digest:
        raise ValueError(f'SHA-256 mismatch: {path}; preserve or remove this file before retrying')


def fetch(entry, destination=VENDOR, check_only=False):
    archive = destination / entry['archive']
    if archive.exists():
        verify(archive, entry['sha256'])
        return

    if check_only:
        raise FileNotFoundError(f'Missing source: {archive}; run make fetch-sources')
    destination.mkdir(parents=True, exist_ok=True)
    print(f'Fetching {entry["archive"]}', flush=True)

    # Each concurrent invocation owns its partial file. A failed transfer or
    # checksum cannot leave a file that later builders mistake for an archive.
    with tempfile.NamedTemporaryFile(prefix=f'.{archive.name}.', suffix='.part',
                                     dir=destination, delete=False) as temporary:
        partial = Path(temporary.name)

    try:
        command = ['curl', '-fL', '--proto', '=https', '--proto-redir', '=https',
                   '--retry', '2', '--connect-timeout', '20', '--max-time', '600',
                   '--output', str(partial), entry['url']]
        try:
            subprocess.run(command, check=True)
        except subprocess.CalledProcessError as error:
            # Some host resolvers stall on dual-stack release-asset redirects.
            # Retry connection failures over IPv4 with the same TLS and hash
            # checks, without forcing IPv4 on hosts that support IPv6 properly.
            if error.returncode not in (5, 6, 7, 28):
                raise
            print(f'Retrying {entry["archive"]} over IPv4 after a connection failure', flush=True)
            subprocess.run([command[0], '--ipv4', *command[1:]], check=True)

        verify(partial, entry['sha256'])
        partial.replace(archive)
    finally:
        partial.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archives', nargs='*', help='archive names; default: all sources')
    parser.add_argument('--check', action='store_true',
                        help='verify local sources without downloading')
    parser.add_argument('--list', action='store_true', help='print selected archive names')
    parser.add_argument('--destination', type=Path, default=VENDOR,
                        help='alternate download directory')
    args = parser.parse_args()

    packages = read_manifest()
    names = args.archives or list(packages)
    unknown = set(names) - packages.keys()
    if unknown:
        parser.error('Unknown archives: ' + ', '.join(sorted(unknown)))
    if args.list:
        print('\n'.join(names))
        return
    for name in names:
        fetch(packages[name], args.destination, args.check)

    print(f'Verified {len(names)} pinned source archives.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
