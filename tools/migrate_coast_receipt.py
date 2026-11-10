#!/usr/bin/env python3
"""Reuse verified upstream builds after a strictly checked branding migration.

The old receipt and exact old recipe bytes are retained. This records reuse,
not recompilation, and refuses recipe changes beyond the OS name replacement.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import tarfile
from build_coast_base import rename

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--receipt', type=Path, required=True)
    p.add_argument('--before', type=Path, required=True)
    p.add_argument('--output-root', type=Path, required=True)
    a = p.parse_args()
    out = a.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    old = json.loads(a.receipt.read_text())
    assert old['target_host_executed'] is False
    updated = copy.deepcopy(old)
    inputs, proof = {}, {}
    with tarfile.open(a.before) as backup:
        for name, digest in old['source_sha256'].items():
            current_name = rename(name)
            current = ROOT / current_name
            if sha(current) == digest:
                inputs[current_name] = digest
                continue
            data = backup.extractfile(name).read()
            assert hashlib.sha256(data).hexdigest() == digest, name
            assert rename(data.decode()).encode() == current.read_bytes(), 'Non-branding change: ' + name
            original = out / 'original-sources' / name
            original.parent.mkdir(parents=True, exist_ok=True)
            original.write_bytes(data)
            proof[name] = dict(original_sha256=digest, original_path=str(original),
                               renamed=current_name, renamed_sha256=sha(current))
            inputs[current_name] = sha(current)
    files = {}
    for name, record in old.get('files', {}).items():
        source = Path(record['path'])
        assert sha(source) == record['sha256'], name
        data = source.read_bytes()
        new = data
        if b'\0' not in data:
            try:
                new = rename(data.decode()).encode()
            except UnicodeError:
                pass
        if new != data:
            source = out / 'files' / rename(name)
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(new)
        files[rename(name)] = {**record, 'path': str(source), 'sha256': sha(source)}
    for name, digest in old.get('artifacts', {}).items():
        assert sha(name) == digest, name
    updated.update(source_sha256=inputs)
    if 'files' in updated:
        updated['files'] = files
        updated['aliases'] = {rename(name): rename(target) for name,target in old['aliases'].items()}
    updated['reuse'] = dict(kind='verified upstream compiled bytes; branding-only recipe migration',
                           receipt=str(a.receipt.resolve()), receipt_sha256=sha(a.receipt), changes=proof,
                           before_archive_sha256=sha(a.before))
    (out / 'BUILD.json').write_text(json.dumps(updated, indent=2) + '\n')
    print('Verified reuse:', a.receipt, len(proof), 'branding-only recipe changes')


if __name__ == '__main__':
    main()
