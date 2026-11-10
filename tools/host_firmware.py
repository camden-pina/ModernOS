#!/usr/bin/env python3
"""Find host UEFI firmware without storing a compiled firmware image in Git."""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def ovmf():
    override = os.environ.get('OVMF')
    candidates = [Path(override)] if override else [
        ROOT / 'firmware/ovmf/OVMF.fd',
        Path('/usr/share/edk2/x64/OVMF.4m.fd'),
        Path('/usr/share/OVMF/OVMF.fd'),
        Path('/usr/share/ovmf/OVMF.fd'),
        Path('/usr/share/qemu/OVMF.fd'),
        Path('/usr/share/OVMF/OVMF_CODE.fd'),
    ]
    for path in candidates:
        if path.is_file():
            return path.resolve()
    raise FileNotFoundError('Install host OVMF firmware or set OVMF=/absolute/path/to/OVMF.fd')


if __name__ == '__main__':
    print(ovmf())
