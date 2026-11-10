#!/usr/bin/env python3
"""Stage the public GUI library closure and optional source-built C/C++ addons.

Creates a separate qualification ISO and preserves its input. The kernel,
Electron and application JavaScript are inherited; this is not a fresh-image
assembler or a claim that the bootstrap GUI libraries were source compiled.
"""
import argparse
import fcntl
import json
from pathlib import Path, PurePosixPath
import subprocess
import tarfile

from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--gui-runtime', type=Path, required=True)
    parser.add_argument('--addons-build', type=Path)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    base = args.iso.resolve(strict=True)
    receipt = args.gui_runtime.resolve(strict=True)
    gui = json.loads(receipt.read_text())
    records = [gui]
    addon = json.loads(args.addons_build.read_text()) if args.addons_build else None
    if addon:
        records.append(addon)
    for record in records:
        for relative, expected in record['source_sha256'].items():
            if digest(ROOT / relative) != expected:
                raise ValueError('Runtime recipe changed: ' + relative)
    files = {name: dict(path=Path(asset['path']), sha256=asset['sha256'], mode=asset['mode'])
             for name, asset in gui['files'].items()}
    if addon:
        for name, asset in addon['files'].items():
            if not name.startswith('node_modules.asar.unpacked/') or not name.endswith('.node'):
                raise ValueError('Unexpected application module path')
            files['usr/share/codeoss-workbench/' + name] = dict(path=Path(asset['path']), sha256=asset['sha256'], mode=0o755)
    aliases = gui['aliases']
    for name, asset in files.items():
        path = PurePosixPath(name)
        if path.is_absolute() or '..' in path.parts or digest(asset['path']) != asset['sha256']:
            raise ValueError('Changed or invalid runtime input: ' + name)
    runtime_root = receipt.parent / 'runtime'
    for name, target in aliases.items():
        resolved = (runtime_root / name).resolve(strict=True)
        if (PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts or
                PurePosixPath(target).is_absolute() or not resolved.is_relative_to(runtime_root) or
                not resolved.is_file() or (runtime_root / name).readlink().as_posix() != target):
            raise ValueError('Invalid GUI runtime alias: ' + name)
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    archive = out / 'coast-gui-libraries.tar'
    with tarfile.open(archive, 'w', format=tarfile.USTAR_FORMAT) as bundle:
        directories = {str(p) for name in [*files, *aliases] for p in PurePosixPath(name).parents if str(p) != '.'}
        for name in sorted(directories, key=lambda p: (p.count('/'), p)):
            info = tarfile.TarInfo(name)
            info.type, info.mode, info.mtime = tarfile.DIRTYPE, 0o755, 1793923200
            bundle.addfile(info)
        for name, asset in sorted(files.items()):
            info = tarfile.TarInfo(name)
            info.size, info.mode, info.mtime = asset['path'].stat().st_size, asset['mode'], 1793923200
            with asset['path'].open('rb') as stream:
                bundle.addfile(info, stream)
        for name, target in sorted(aliases.items()):
            info = tarfile.TarInfo(name)
            info.type, info.mode, info.linkname, info.mtime = tarfile.SYMTYPE, 0o777, target, 1793923200
            bundle.addfile(info)
    with Path(str(base) + '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
        before = digest(base)
        config_path = out / 'limine.conf'
        subprocess.run(['xorriso', '-no_rc', '-osirrox', 'on', '-indev', str(base),
            '-extract', '/boot/limine/limine.conf', str(config_path)], check=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        config = config_path.read_text()
        if config.count('\n/Reboot') != 1 or '/boot/coast-gui-libraries.tar' in config:
            raise ValueError('Unexpected or already overlaid Limine configuration')
        config_path.write_text(config.replace('\n/Reboot',
            '    module_path: boot():/boot/coast-gui-libraries.tar\n    module_string: initramfs\n\n/Reboot'))
        image = out / 'Coast-browser.iso'
        subprocess.run(['cp', '--reflink=auto', str(base), str(image)], check=True)
        with (out / 'image.log').open('w') as log:
            subprocess.run(['xorriso', '-no_rc', '-dev', str(image), '-map', str(archive), '/boot/coast-gui-libraries.tar',
                '-map', str(config_path), '/boot/limine/limine.conf', '-boot_image', 'any', 'keep', '-commit', '-end'],
                check=True, stdout=log, stderr=subprocess.STDOUT)
        if digest(base) != before or any(digest(a['path']) != a['sha256'] for a in files.values()):
            raise ValueError('A preserved image or runtime input changed')
    record = dict(state='public GUI runtime and module candidate; Coast qualification pending',
        iso=str(image), iso_sha256=digest(image), base_iso=str(base), base_iso_sha256=before, inherited_iso=True,
        gui_runtime_sha256=digest(receipt), glibc_build_sha256=gui['component_build_sha256']['glibc'],
        addons_build_sha256=digest(args.addons_build) if addon else None,
        files={name: asset['sha256'] for name, asset in files.items()}, aliases=aliases,
        source_sha256={str(Path(__file__).relative_to(ROOT)): digest(Path(__file__))}, target_execution_on_linux=False)
    if base.with_name('MANIFEST.json').is_file():
        record['diagnostics_qa_only'] = json.loads(base.with_name('MANIFEST.json').read_text()).get('diagnostics_qa_only', False)
    (out / 'MANIFEST.json').write_text(json.dumps(record, indent=2) + '\n')
    print('GUI runtime qualification ISO:', image, flush=True)


if __name__ == '__main__':
    main()
