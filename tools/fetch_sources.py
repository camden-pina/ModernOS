#!/usr/bin/env python3
"""Restore pinned upstream archives without storing downloads in Git.

Existing valid files work offline. Missing files are fetched over HTTPS and
installed only after checking their SHA-256. Existing corrupt files are left
in place so a failed check never silently replaces a local archive.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'userland/vendor'
MANIFEST = VENDOR / 'sources.json'


def valid_https_url(value):
    if not isinstance(value, str):
        return False
    url = urlsplit(value)
    return url.scheme == 'https' and bool(url.hostname) and not (url.username or url.password)


def read_manifest(path=MANIFEST):
    packages = {}
    for entry in json.loads(path.read_text()):
        name = entry['archive']
        mirrors = entry.get('mirrors', [])
        if not isinstance(mirrors, list) or any(not isinstance(url, str) for url in mirrors):
            raise ValueError(f'Invalid source mirrors: {name}')
        if (not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._+-]*', name)
                or name in packages
                or not re.fullmatch(r'[0-9a-f]{64}', entry['sha256'])
                or any(not valid_https_url(url) for url in (entry['url'], *mirrors))):
            raise ValueError(f'Invalid source record: {name}')
        packages[name] = entry
    return packages


def verify(path, digest):
    with path.open('rb') as source:
        actual = hashlib.file_digest(source, 'sha256').hexdigest()
    if actual != digest:
        raise ValueError(f'SHA-256 mismatch: {path}; preserve or remove this file before retrying')


def source_urls(entry, mirror_bases=None):
    # A project/company mirror uses the cache's flat archive filenames. It is
    # tried first and can serve every pinned source and browser host tool.
    if mirror_bases is None:
        mirror_bases = os.environ.get('SOURCE_MIRRORS', '').split()
    urls = []
    for base in mirror_bases:
        if not valid_https_url(base) or urlsplit(base).query or urlsplit(base).fragment:
            raise ValueError('Invalid source mirror base; use HTTPS without credentials, query or fragment')
        urls.append(base.rstrip('/') + '/' + entry['archive'])
    urls.extend((entry['url'], *entry.get('mirrors', [])))
    if any(not valid_https_url(url) for url in urls):
        raise ValueError(f'Invalid source URL: {entry["archive"]}')
    return list(dict.fromkeys(urls))


def download(url, partial, retries=2, retry_ipv4=True):
    command = ['curl', '-fL', '--proto', '=https', '--proto-redir', '=https',
               '--retry', str(retries), '--retry-max-time', '120',
               '--connect-timeout', '20', '--max-time', '600',
               '--speed-limit', '1024', '--speed-time', '30',
               '--output', str(partial), url]
    try:
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError as error:
        # Some host resolvers stall on dual-stack release-asset redirects.
        # Retry connection failures over IPv4 with the same TLS and hash
        # checks, without forcing IPv4 on hosts that support IPv6 properly.
        if not retry_ipv4 or error.returncode not in (5, 6, 7, 28):
            raise
        print(f'Retrying {url} over IPv4 after a connection failure', flush=True)
        subprocess.run([command[0], '--ipv4', *command[1:]], check=True)


def fetch(entry, destination=VENDOR, check_only=False, mirror_bases=None):
    archive = destination / entry['archive']
    if archive.exists():
        verify(archive, entry['sha256'])
        return

    if check_only:
        raise FileNotFoundError(f'Missing source: {archive}; run make fetch-sources')
    urls = source_urls(entry, mirror_bases)
    destination.mkdir(parents=True, exist_ok=True)
    print(f'Fetching {entry["archive"]}', flush=True)

    # Each concurrent invocation owns its partial file. A failed transfer or
    # checksum cannot leave a file that later builders mistake for an archive.
    with tempfile.NamedTemporaryFile(prefix=f'.{archive.name}.', suffix='.part',
                                     dir=destination, delete=False) as temporary:
        partial = Path(temporary.name)

    try:
        for index, url in enumerate(urls):
            if index:
                print(f'Trying next source for {entry["archive"]}: {url}', flush=True)
            try:
                # Prefer another server over repeated waits for one hostname.
                # Retain normal retries and the IPv4 fallback on the last URL.
                last = index == len(urls) - 1
                download(url, partial, retries=2 if last else 0, retry_ipv4=last)
            except subprocess.CalledProcessError as error:
                # Try another server for network/HTTP failures. TLS, local I/O
                # and checksum errors still fail rather than bypassing checks.
                if error.returncode not in (5, 6, 7, 18, 22, 28, 52, 56) or index == len(urls) - 1:
                    raise
            else:
                break

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
    parser.add_argument('--mirror', dest='mirror_bases', action='append', metavar='HTTPS_BASE',
                        help='try a flat archive mirror first (repeatable); default: SOURCE_MIRRORS')
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
        fetch(packages[name], args.destination, args.check, args.mirror_bases)

    print(f'Verified {len(names)} pinned source archives.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
