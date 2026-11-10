#!/usr/bin/env python3
"""Check the selected boot modules for stale OS paths/text and signing secrets."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile
from build_coast_base import PATTERN


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--iso', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    iso = a.iso.resolve()
    listing = subprocess.check_output(['xorriso', '-no_rc', '-indev', str(iso), '-find', '/boot',
                                       '-type', 'f', '-exec', 'report_lba', '--'], stderr=subprocess.DEVNULL).decode()
    extents = {name: (int(lba) * 2048, int(size)) for lba,size,name in re.findall(
        r"File data lba:\s*0\s*,\s*(\d+)\s*,\s*\d+\s*,\s*(\d+)\s*,\s*'([^']+)'", listing)}
    with iso.open('rb') as stream:
        offset,size = extents['/boot/limine/limine.conf']
        stream.seek(offset)
        config = stream.read(size).decode()
    assert '\n/Coast\n' in config and not PATTERN.search(config)
    entry = config.split('\n/Coast\n', 1)[1].split('\n/', 1)[0]
    modules = re.findall(r'module_path:\s*boot\(\):([^\n]+)', entry)
    errors, language_words, files, text_files = [], [], 0, 0
    for module in modules:
        with iso.open('rb') as stream:
            stream.seek(extents[module][0])
            with tarfile.open(fileobj=stream, mode='r|') as archive:
                for member in archive:
                    files += 1
                    if PATTERN.search(member.name) or PATTERN.search(member.linkname):
                        errors.append(dict(module=module, name=member.name, problem='stale path or link'))
                    if 'private-keys-v1.d/' in member.name and member.isfile():
                        errors.append(dict(module=module, name=member.name, problem='private key published'))
                    if not member.isfile():
                        continue
                    data = archive.extractfile(member).read()
                    # ELFs, caches and unchanged upstream distribution archives
                    # are byte-preserved objects, not editable source text.
                    if b'\0' in data:
                        continue
                    try:
                        text = data.decode('utf-8')
                    except UnicodeError:
                        continue
                    text_files += 1
                    if PATTERN.search(text):
                        # This is Portuguese for "modern", in an unchanged
                        # upstream TypeScript translation, not the OS name.
                        if member.name == 'usr/share/codeoss-workbench/extensions/node_modules/typescript/lib/pt-br/diagnosticMessages.generated.json' and ('recursos modern' + 'os de resolução') in text:
                            language_words.append(member.name)
                            continue
                        errors.append(dict(module=module, name=member.name, problem='stale text'))
    with iso.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    report = dict(state='passed' if not errors else 'failed', iso=str(iso), iso_sha256=digest,
                  modules=modules, entries=files, text_files=text_files, errors=errors, upstream_language_words=language_words,
                  scope='all selected boot archive paths, symlinks and UTF-8 text; binary provenance remains byte-preserved')
    a.output.write_text(json.dumps(report, indent=2) + '\n')
    assert not errors, errors[:20]
    print('PASS Coast boot paths, text and public-only signing data:', files, 'entries;', text_files, 'text files')


if __name__ == '__main__':
    main()
