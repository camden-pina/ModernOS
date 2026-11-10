#!/usr/bin/env python3
"""Export only verified public repository artifacts for another computer."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil

from serve_repository import load_repository

ROOT = Path(__file__).resolve().parents[1]


def export(repository, output):
    verified = load_repository(repository)
    original = json.loads(Path(repository).read_text())
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    public = {}
    for route, record in sorted(verified['assets'].items()):
        relative = PurePosixPath('public') / route.lstrip('/')
        target = output / str(relative)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(record['path'], target)
        target.chmod(0o644)
        with target.open('rb') as stream:
            assert hashlib.file_digest(stream, 'sha256').hexdigest() == record['sha256']
        public[route] = dict(path=str(relative), sha256=record['sha256'])
    # No build commands, private signing directory, absolute source paths,
    # native guest executables outside their signed archives, or Git worktrees.
    packages = {name: {key: value for key, value in record.items()
                       if key in ('name', 'version', 'description', 'provides', 'license', 'files', 'aliases')}
                for name, record in original['packages'].items()}
    for package in packages.values():
        package['files'] = list(package['files'])
        package['aliases'] = list(package['aliases'])
    snapshot = dict(format='coast-public-repository', version=1, target_host_executed=False,
                    signing_secret_included=False, public_signing_fingerprint=verified['fingerprint'],
                    host_files=public, packages=packages, recipes=original['recipes'],
                    producer_manifest_sha256=verified['manifest_sha256'])
    (output/'repository.json').write_text(json.dumps(snapshot, indent=2)+'\n')
    shutil.copyfile(ROOT/'tools/serve_repository.py', output/'serve_repository.py')
    shutil.copyfile(ROOT/'userland/packages/search.py', output/'search.py')
    shutil.copytree(ROOT/'userland/packages/web', output/'web')
    shutil.copytree(ROOT/'deployment/repository', output/'deployment')
    load_repository(output/'repository.json')
    return dict(path=str(output), artifacts=len(public), packages=len(packages),
                recipes=len(snapshot['recipes']), signing_secret_included=False)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repository', type=Path, required=True)
    p.add_argument('--output-root', type=Path, required=True)
    args = p.parse_args()
    print(json.dumps(export(args.repository, args.output_root), indent=2))


if __name__ == '__main__':
    main()
