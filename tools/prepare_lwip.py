#!/usr/bin/env python3
"""Verify/extract the upstream TCP/IP core outside the kernel source tree."""
import hashlib
from pathlib import Path
import shutil
import tarfile

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / 'userland/vendor/lwip-2.2.1.tar.gz'
DIGEST = 'ce0b7461c0ad9602c376f0bf07c5eb7253b48c7bf66f011c6bf3e2a96731c539'
TARGET = ROOT / 'build/lwip'


def main():
    if hashlib.sha256(ARCHIVE.read_bytes()).hexdigest() != DIGEST:
        raise SystemExit('lwIP source checksum mismatch')
    stamp = TARGET / '.prepared'
    if stamp.is_file() and stamp.read_text() == DIGEST:
        return
    temporary = ROOT / 'build/lwip-extract'
    if temporary.exists():
        shutil.rmtree(temporary)
    temporary.mkdir(parents=True)
    with tarfile.open(ARCHIVE) as source:
        source.extractall(temporary, filter='data')
    extracted = temporary / 'lwip-STABLE-2_2_1_RELEASE'
    if TARGET.exists():
        shutil.rmtree(TARGET)
    extracted.rename(TARGET)
    shutil.rmtree(temporary)
    licenses = ROOT / 'userland/vendor/licenses'
    licenses.mkdir(exist_ok=True)
    shutil.copy2(TARGET / 'COPYING', licenses / 'lwip-COPYING.txt')
    stamp.write_text(DIGEST)


if __name__ == '__main__':
    main()
