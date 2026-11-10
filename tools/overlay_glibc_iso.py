#!/usr/bin/env python3
"""Stage source-built GNU ABI libraries in a separate Coast qualification ISO."""
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
    parser.add_argument('--glibc-build', type=Path, required=True)
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    base = args.iso.resolve(strict=True)
    receipt = args.glibc_build.resolve(strict=True)
    record = json.loads(receipt.read_text())
    if record['target_host_executed'] is not False:
        parser.error('Use a source-built, unexecuted GNU runtime fixture')
    for name, expected in record['source_sha256'].items():
        if digest(ROOT / name) != expected:
            raise ValueError('GNU runtime source changed: ' + name)
    prefix = 'usr/lib/x86_64-linux-gnu/'
    files = {}
    for name, asset in record['files'].items():
        relative = PurePosixPath(name)
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('Invalid runtime path: ' + name)
        if not (name.startswith(prefix) and '.so' in relative.name or
                name.startswith('usr/share/licenses/coast-glibc/')):
            continue
        source = Path(asset['path']).resolve(strict=True)
        if digest(source) != asset['sha256']:
            raise ValueError('GNU runtime output changed: ' + name)
        # Headers, archives and GNU linker scripts belong to its GNU SDK,
        # separate from the native musl SDK. Stage the shared runtime here.
        if name.startswith(prefix) and source.read_bytes()[:4] != b'\x7fELF':
            continue
        files[name] = source
    for required in ('libc.so.6', 'libm.so.6', 'libresolv.so.2', 'ld-linux-x86-64.so.2'):
        if prefix + required not in files:
            raise ValueError('Incomplete GNU runtime: ' + required)
    # Coast's existing ELF images use this canonical interpreter path.
    files['lib64/ld-linux-x86-64.so.2'] = files[prefix + 'ld-linux-x86-64.so.2']
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    with Path(str(base) + '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
        before = digest(base)
        config_path = out / 'limine.conf'
        subprocess.run(['xorriso', '-no_rc', '-osirrox', 'on', '-indev', str(base),
            '-extract', '/boot/limine/limine.conf', str(config_path)], check=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        config = config_path.read_text()
        if config.count('\n/Reboot') != 1 or '/boot/coast-glibc.tar' in config:
            raise ValueError('Unexpected or already overlaid Limine configuration')
        config = config.replace('\n/Reboot',
            '    module_path: boot():/boot/coast-glibc.tar\n    module_string: initramfs\n\n/Reboot')
        config_path.write_text(config)
        archive = out / 'coast-glibc.tar'
        with tarfile.open(archive, 'w', format=tarfile.USTAR_FORMAT) as bundle:
            directories = {str(p) for name in files for p in PurePosixPath(name).parents if str(p) != '.'}
            for name in sorted(directories, key=lambda p: (p.count('/'), p)):
                info = tarfile.TarInfo(name)
                info.type, info.mode, info.mtime = tarfile.DIRTYPE, 0o755, 1793923200
                bundle.addfile(info)
            for name, source in sorted(files.items()):
                info = tarfile.TarInfo(name)
                original = prefix + 'ld-linux-x86-64.so.2' if name.startswith('lib64/') else name
                info.size, info.mode, info.mtime = source.stat().st_size, record['files'][original]['mode'], 1793923200
                with source.open('rb') as stream:
                    bundle.addfile(info, stream)
        image = out / 'Coast-browser.iso'
        subprocess.run(['cp', '--reflink=auto', str(base), str(image)], check=True)
        extra = []
        kernel_sha = None
        if args.kernel:
            kernel = args.kernel.resolve(strict=True)
            kernel_sha = digest(kernel)
            extra = ['-map', str(kernel), '/boot/kernel.elf']
        with (out / 'image.log').open('w') as log:
            subprocess.run(['xorriso', '-no_rc', '-dev', str(image),
                '-map', str(archive), '/boot/coast-glibc.tar',
                '-map', str(config_path), '/boot/limine/limine.conf', *extra,
                '-boot_image', 'any', 'keep', '-commit', '-end'], check=True,
                stdout=log, stderr=subprocess.STDOUT)
        if digest(base) != before:
            raise ValueError('Preserved base image changed')
        if kernel_sha and digest(kernel) != kernel_sha:
            raise ValueError('Kernel input changed')
        for name, source in files.items():
            expected = record['files'][prefix + 'ld-linux-x86-64.so.2' if name.startswith('lib64/') else name]['sha256']
            if digest(source) != expected:
                raise ValueError('Runtime input changed during assembly: ' + name)
        manifest = dict(state='source-built GNU runtime overlay; guest qualification pending',
            iso=str(image), iso_sha256=digest(image), base_iso=str(base), base_iso_sha256=before,
            inherited_iso=True, glibc_build_sha256=digest(receipt), kernel_sha256=kernel_sha,
            files={name: digest(source) for name, source in sorted(files.items())},
            source_sha256={str(Path(__file__).relative_to(ROOT)): digest(Path(__file__))},
            target_execution_on_linux=False)
        original_manifest = base.parent / 'MANIFEST.json'
        manifest['diagnostics_qa_only'] = bool(original_manifest.is_file() and
            json.loads(original_manifest.read_text()).get('diagnostics_qa_only'))
        (out / 'MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print('GNU runtime qualification ISO:', image)


if __name__ == '__main__':
    main()
