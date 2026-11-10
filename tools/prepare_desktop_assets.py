#!/usr/bin/env python3
"""Fetch pinned font/dictionary data for the desktop without borrowing host fonts."""
from build_state import project_lock
from build_state import atomic_write
import argparse
import json
from pathlib import Path

from debian_package import package_tar, safe_relative
from fetch_sources import fetch
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]
PINS = ROOT / 'ports/chromium/desktop-assets.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/desktop-assets')
    args = parser.parse_args()
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    pins = json.loads(PINS.read_text())
    identity = {str(p.relative_to(ROOT)): digest(p) for p in [Path(__file__), PINS,
        ROOT / 'tools/debian_package.py', ROOT / 'tools/fetch_sources.py']}
    receipt = out / 'BUILD.json'
    if receipt.exists():
        old = json.loads(receipt.read_text())
        if old['source_sha256'] != identity or any(digest(Path(p['path'])) != p['sha256'] for p in old['files'].values()):
            raise ValueError('Desktop asset inputs changed; select a fresh output directory')
        print('Verified pinned desktop assets:', out)
        return
    vendor = out / 'vendor'
    files = {}

    def put(name, data):
        safe_relative(name)
        if data.startswith((b'\x7fELF', b'MZ', b'!<arch>\n')):
            raise ValueError('Desktop assets must not contain executable code: ' + name)
        path = out / 'runtime' / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        files[name] = dict(path=str(path), sha256=digest(path), mode=0o644)

    required = {'DejaVuSans.ttf', 'DejaVuSans-Bold.ttf', 'DejaVuSansMono.ttf', 'DejaVuSansMono-Bold.ttf',
                'NotoSansCJK-Regular.ttc', 'NotoColorEmoji.ttf'}
    found = set()
    for package, pin in pins['font_packages'].items():
        fetch(pin, vendor)
        with package_tar(vendor / pin['archive'], 'data') as archive:
            for member in archive:
                name = safe_relative(member.name) if member.name not in ('.', './') else '.'
                font = name.startswith('usr/share/fonts/') and Path(name).name in required
                notice = name == 'usr/share/doc/' + package + '/copyright'
                if not member.isfile() or not (font or notice):
                    continue
                target = ('usr/share/fonts/coast/' + Path(name).name) if font else 'usr/share/licenses/' + package + '/copyright'
                put(target, archive.extractfile(member).read())
                if font:
                    found.add(Path(name).name)
    if found != required:
        raise ValueError('Missing desktop fonts: ' + repr(required - found))
    dictionary = pins['dictionary']
    fetch(dictionary, vendor)
    put('usr/share/coast-browser/dictionaries/en-US-10-1.bdic', (vendor / dictionary['archive']).read_bytes())
    for name in dictionary['licenses']:
        put('usr/share/licenses/coast-browser-dictionary/' + Path(name).name, (ROOT / name).read_bytes())
    atomic_write(receipt, json.dumps(dict(state='pinned desktop data staged', files=files,
        source_sha256=identity, host_fonts_used=False, executable_payloads=False), indent=2) + '\n')
    print('Pinned desktop assets staged:', out)


if __name__ == '__main__':
    with project_lock():
        main()
