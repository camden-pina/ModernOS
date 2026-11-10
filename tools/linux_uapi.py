#!/usr/bin/env python3
"""Stage the checked-in C UAPI headers without borrowing host /usr/include."""
import hashlib
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]


def prepare(destination):
    source = ROOT / 'ports/linux-uapi'
    if not (source / 'SOURCE.json').is_file():
        source = Path('/usr/lib/coast/linux-uapi')
    for entry in json.loads((source / 'SOURCE.json').read_text())['files']:
        path = source / 'include' / entry['path']
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
            raise ValueError('Linux UAPI source changed: ' + entry['path'])
    destination = Path(destination)
    for entry in json.loads((source / 'SOURCE.json').read_text())['files']:
        target = destination / entry['path']
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / 'include' / entry['path'], target)
    return Path(destination)
