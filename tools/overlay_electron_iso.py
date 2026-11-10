#!/usr/bin/env python3
"""Stage source-built Electron/Node and their matching kernel in a test ISO.

This preserves the original image and inherits its application/library payload.
It is a runtime qualification step, not a fresh full-image source assembler.
"""
import argparse
import fcntl
import json
from pathlib import Path
import subprocess
import tarfile

from electron_admission import prepare
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--runtime-build', type=Path, required=True)
    parser.add_argument('--kernel-build', type=Path, required=True, help='desktop kernel output directory containing KERNEL.json')
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    base = args.iso.resolve(strict=True)
    receipt = args.runtime_build.resolve(strict=True)
    record = json.loads(receipt.read_text())
    kernel_root = args.kernel_build.resolve(strict=True)
    kernel = kernel_root / 'vmlinux'
    kernel_record = json.loads((kernel_root / 'KERNEL.json').read_text())
    admission = kernel_root / 'electron-admission.json'
    if (kernel_record['kernel_sha256'] != digest(kernel) or
            kernel_record['runtime_build_sha256'] != digest(receipt) or
            kernel_record['admission_sha256'] != digest(admission) or
            json.loads(admission.read_text())['source_build_sha256'] != digest(receipt)):
        parser.error('The kernel and runtime must have matching unchanged build receipts')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    prepare(receipt, out / 'electron-admission.h')
    files = {'opt/electron-43.7.3/' + name: Path(record['products'][name]['path']).resolve(strict=True)
             for name in ['electron', 'node']}
    sidecars = {'snapshot_blob.bin', 'resources.pak', 'chrome_100_percent.pak', 'chrome_200_percent.pak', 'icudtl.dat', 'libffmpeg.so'}
    for name, asset in record['sidecars'].items():
        if name not in sidecars or digest(Path(asset['path'])) != asset['sha256']:
            raise ValueError('Unexpected or changed Electron sidecar: ' + name)
        files['opt/electron-43.7.3/' + name] = Path(asset['path']).resolve(strict=True)
    node = json.loads(receipt.with_name('NODE.json').read_text())
    license_path = Path(node['license']['path']).resolve(strict=True)
    if digest(license_path) != node['license']['sha256']:
        raise ValueError('Node runtime license changed')
    files['usr/share/licenses/coast-electron-node/LICENSE'] = license_path
    source = license_path.parents[2]
    for relative, target in [('electron/LICENSE', 'Electron-LICENSE'), ('LICENSE', 'Chromium-LICENSE')]:
        files['usr/share/licenses/coast-electron-source/' + target] = source / relative
    files['usr/share/licenses/coast-electron-source/V8-LICENSE'] = source / 'v8/LICENSE'
    before_files = {name: digest(path) for name, path in files.items()}
    with Path(str(base) + '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
        before = digest(base)
        config_path = out / 'limine.conf'
        subprocess.run(['xorriso', '-no_rc', '-osirrox', 'on', '-indev', str(base),
            '-extract', '/boot/limine/limine.conf', str(config_path)], check=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        config = config_path.read_text()
        if config.count('\n/Reboot') != 1 or '/boot/coast-electron.tar' in config:
            raise ValueError('Unexpected or already overlaid Limine configuration')
        config = config.replace('\n/Reboot',
            '    module_path: boot():/boot/coast-electron.tar\n    module_string: initramfs\n\n/Reboot')
        config_path.write_text(config)
        archive = out / 'coast-electron.tar'
        with tarfile.open(archive, 'w', format=tarfile.USTAR_FORMAT) as bundle:
            directories = {str(parent) for name in files for parent in Path(name).parents if str(parent) != '.'}
            for name in sorted(directories, key=lambda p: (p.count('/'), p)):
                info = tarfile.TarInfo(name)
                info.type, info.mode, info.mtime = tarfile.DIRTYPE, 0o755, 1793923200
                bundle.addfile(info)
            for name, path in sorted(files.items()):
                info = tarfile.TarInfo(name)
                info.size, info.mode, info.mtime = path.stat().st_size, (0o755 if name in
                    ['opt/electron-43.7.3/electron', 'opt/electron-43.7.3/node'] else 0o644), 1793923200
                with path.open('rb') as stream:
                    bundle.addfile(info, stream)
        image = out / 'Coast-browser.iso'
        subprocess.run(['cp', '--reflink=auto', str(base), str(image)], check=True)
        with (out / 'image.log').open('w') as log:
            subprocess.run(['xorriso', '-no_rc', '-dev', str(image),
                '-map', str(archive), '/boot/coast-electron.tar',
                '-map', str(config_path), '/boot/limine/limine.conf', '-map', str(kernel), '/boot/kernel.elf',
                '-boot_image', 'any', 'keep', '-commit', '-end'], check=True, stdout=log, stderr=subprocess.STDOUT)
        if digest(base) != before or digest(kernel) != kernel_record['kernel_sha256']:
            raise ValueError('An image assembly input changed')
        if any(digest(files[name]) != expected for name, expected in before_files.items()):
            raise ValueError('A runtime input changed during image assembly')
    manifest = dict(state='source-built Electron runtime candidate; guest qualification pending',
        iso=str(image), iso_sha256=digest(image), base_iso=str(base), base_iso_sha256=before, inherited_iso=True,
        runtime_build_sha256=digest(receipt), kernel_sha256=digest(kernel), files=before_files,
        source_sha256={str(Path(__file__).relative_to(ROOT)): digest(Path(__file__))},
        target_execution_on_linux=False)
    original_manifest = base.with_name('MANIFEST.json')
    if original_manifest.is_file():
        old = json.loads(original_manifest.read_text())
        for name in ['glibc_build_sha256', 'diagnostics_qa_only', 'gui_runtime_sha256', 'addons_build_sha256']:
            if name in old:
                manifest[name] = old[name]
    (out / 'MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Electron runtime qualification ISO:', image, flush=True)


if __name__ == '__main__':
    main()
