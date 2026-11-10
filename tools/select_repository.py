#!/usr/bin/env python3
"""Select a guest-qualified local repository and check the restarted HTTP service."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
from urllib.parse import urlsplit
from urllib.request import urlopen
import uuid

from serve_repository import load_repository
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, required=True)
    parser.add_argument('--qualification', type=Path, required=True, help='passed guest report for this exact snapshot')
    parser.add_argument('--iso', type=Path, default=ROOT / 'build/Coast-browser.iso')
    parser.add_argument('--link', type=Path, default=ROOT / 'build/Coast-repository-server')
    parser.add_argument('--service', default='coast-packages.service')
    parser.add_argument('--system', action='store_true', help='restart a system unit rather than the local user unit')
    parser.add_argument('--origin', default='http://127.0.0.1:8765')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    origin = urlsplit(args.origin)
    if origin.scheme not in ('http', 'https') or origin.username or origin.password or origin.query or origin.fragment:
        parser.error('Use the HTTP origin of the local repository service')
    manifest = args.repository.resolve(strict=True)
    verified = load_repository(manifest)
    snapshot = json.loads(manifest.read_text())
    proof = json.loads(args.qualification.read_text())
    if (proof.get('state') != 'passed' or not proof.get('checks') or not proof.get('owned_vm_stopped') or
            proof.get('export_error') or proof.get('repository_manifest_sha256') != digest(manifest) or
            proof.get('iso_sha256') != digest(args.iso.resolve(strict=True))):
        raise ValueError('The report must qualify this exact repository and selected ISO in a stopped guest')
    for name, expected in proof.get('exports', {}).items():
        path = (args.qualification.parent / name).resolve(strict=True)
        if not path.is_relative_to(args.qualification.parent.resolve()) or digest(path) != expected:
            raise ValueError('Qualified export changed: ' + name)
    link = args.link.absolute()
    if not link.is_symlink():
        raise ValueError('The service selection path must already be a symbolic link')
    previous = link.readlink()
    pending = link.with_name(link.name + '.next-' + uuid.uuid4().hex)
    service = ['systemctl', *([] if args.system else ['--user']), 'restart', args.service]

    def select(target):
        pending.symlink_to(target)
        pending.replace(link)

    def ready():
        with urlopen(args.origin.rstrip('/') + '/health', timeout=2) as response:
            health = json.load(response)
        if health.get('fingerprint') != verified['fingerprint'] or health.get('packages') != len(verified['packages']):
            raise RuntimeError('Service has not loaded the selected repository')
        with urlopen(args.origin.rstrip('/') + '/api/v1/search?kind=binary', timeout=2) as response:
            catalog = json.load(response)
        versions = {p['Name']: p['Version'] for p in catalog['results']}
        if versions != {name: p['version'] for name, p in snapshot['packages'].items()}:
            raise RuntimeError('HTTP package versions differ from the qualified snapshot')
        return health

    try:
        select(os.path.relpath(manifest.parent, link.parent))
        try:
            subprocess.run(service, check=True)
            deadline = time.monotonic() + 35
            while True:
                try:
                    health = ready()
                    break
                except Exception:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.5)
        except BaseException:
            select(previous)
            subprocess.run(service, check=True)
            raise
    finally:
        pending.unlink(missing_ok=True)
    record = dict(state='selected and HTTP ready', previous=str(previous), selected=str(manifest.parent),
        repository_manifest_sha256=digest(manifest), qualification_sha256=digest(args.qualification),
        fingerprint=health['fingerprint'], binary_packages=len(snapshot['packages']), source_recipes=len(snapshot['recipes']))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
