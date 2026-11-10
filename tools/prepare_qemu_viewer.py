#!/usr/bin/env python3
"""Stage source noVNC and the Coast VM launcher for an explicit fresh runtime."""
import argparse
import json
from pathlib import Path
import shutil

from build_desktop import verify_receipt
from fetch_sources import fetch
from source_archive import unpack
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu-build', type=Path, required=True)
    parser.add_argument('--program-build', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    qemu, program = verify_receipt(args.qemu_build), verify_receipt(args.program_build)
    if qemu.get('backend') != 'TCI' or qemu.get('compiler_build_sha256') != program.get('compiler_build_sha256'):
        parser.error('Use QEMU TCI and Python from the same source-built musl compiler')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime = out / 'runtime'
    source_record = ROOT / 'ports/qemu/novnc-source.json'
    pin = json.loads(source_record.read_text())
    fetch(pin, out / 'vendor')
    unpack(out / 'vendor' / pin['archive'], out)
    source = out / pin['source']

    def copy(path, name):
        destination = runtime / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, destination)

    for directory in ('core', 'vendor'):
        for path in (source / directory).rglob('*'):
            if path.is_file():
                copy(path, 'usr/share/novnc/' + str(path.relative_to(source)))
    for name in ('AUTHORS', 'LICENSE.txt'):
        copy(source / name, 'usr/share/novnc/' + name)
    for path in (source / 'docs').glob('LICENSE.*'):
        copy(path, 'usr/share/licenses/novnc/' + path.name)
    copy(out / 'vendor' / pin['archive'], 'usr/src/coast/novnc/' + pin['archive'])
    copy(source_record, 'usr/src/coast/novnc/SOURCES.json')
    copy(ROOT / 'ports/qemu/coast.html', 'usr/share/novnc/coast.html')
    copy(ROOT / 'ports/qemu/coast_vm.py', 'usr/share/coast/qemu/coast_vm.py')
    copy(ROOT / 'LICENSE', 'usr/share/licenses/coast-vm/LICENSE')
    launcher = runtime / 'usr/bin/coast-vm'
    launcher.parent.mkdir(parents=True, exist_ok=True)
    launcher.write_text('#!/usr/bin/python3\nfrom runpy import run_path\nrun_path("/usr/share/coast/qemu/coast_vm.py", run_name="__main__")\n')
    launcher.chmod(0o755)
    files = {str(p.relative_to(runtime)): dict(path=str(p), sha256=digest(p), mode=p.stat().st_mode & 0o777)
             for p in sorted(runtime.rglob('*')) if p.is_file()}
    inputs = [Path(__file__), source_record, ROOT / 'ports/qemu/coast_vm.py', ROOT / 'ports/qemu/coast.html', ROOT / 'LICENSE']
    (out / 'BUILD.json').write_text(json.dumps(dict(state='source viewer staged; guest qualification pending',
        target_host_executed=False, files=files, aliases={}, source=pin,
        qemu_build_sha256=digest(args.qemu_build), program_build_sha256=digest(args.program_build),
        source_sha256={str(p.relative_to(ROOT)): digest(p) for p in inputs}), indent=2) + '\n')
    print('QEMU source viewer staged:', runtime, flush=True)


if __name__ == '__main__':
    main()
