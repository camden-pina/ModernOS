#!/usr/bin/env python3
"""Create a separate development ISO with a newly compiled Coast kernel."""
import argparse
import fcntl
import json
from pathlib import Path
import subprocess

from source_tree import digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    base, kernel = args.iso.resolve(strict=True), args.kernel.resolve(strict=True)
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    image = out / base.name
    with Path(str(base) + '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
        before, kernel_sha = digest(base), digest(kernel)
        subprocess.run(['cp', '--reflink=auto', str(base), str(image)], check=True)
        with (out / 'image.log').open('w') as log:
            subprocess.run(['xorriso', '-no_rc', '-dev', str(image), '-map', str(kernel),
                '/boot/kernel.elf', '-boot_image', 'any', 'keep', '-commit', '-end'],
                check=True, stdout=log, stderr=subprocess.STDOUT)
        if digest(base) != before or digest(kernel) != kernel_sha:
            raise ValueError('Image input changed while packaging')
        original = base.parent / 'MANIFEST.json'
        record = dict(iso=str(image), iso_sha256=digest(image), kernel=str(kernel),
            kernel_sha256=kernel_sha, inherited_iso=True, base_iso=str(base), base_iso_sha256=before,
            state='kernel overlay built; guest qualification pending', target_execution_on_linux=False,
            diagnostics_qa_only=original.is_file() and json.loads(original.read_text()).get('diagnostics_qa_only', False))
        (out / 'MANIFEST.json').write_text(json.dumps(record, indent=2) + '\n')
        print('Development ISO:', image)


if __name__ == '__main__':
    main()
