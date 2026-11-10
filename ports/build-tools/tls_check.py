#!/usr/bin/env python3
"""Exercise zero-filled ELF TLS and reject malformed TLS records in Coast."""
import argparse
import errno
import json
import os
from pathlib import Path
import platform
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    if platform.system() != 'Coast':
        parser.error('Target execution is restricted to Coast')
    work = Path(tempfile.mkdtemp(prefix='tls-check-'))
    binary = work / 'original'
    subprocess.run([args.cc, '-std=c11', '-O2', '-static', '-no-pie', '-pthread',
        str(Path(__file__).with_suffix('.c')), '-o', str(binary)], check=True)
    data = binary.read_bytes()
    assert data[:6] == b'\x7fELF\x02\x01'
    phoff, = struct.unpack_from('<Q', data, 32)
    size, count = struct.unpack_from('<HH', data, 54)
    assert size == 56
    headers = [(phoff + index * size, struct.unpack_from('<IIQQQQQQ', data, phoff + index * size))
        for index in range(count)]
    at, tls = next((at, list(p)) for at, p in headers if p[0] == 7)
    assert tls[5] == 0 and tls[6] >= 8, 'The fixture must contain only zero-filled TLS'
    gap = 0x30000000
    assert all(not (p[0] == 1 and p[3] <= gap < p[3] + p[6]) for _, p in headers)
    checks = []

    def run(name, header, accepted):
        modified = bytearray(data)
        struct.pack_into('<IIQQQQQQ', modified, at, *header)
        path = work / name
        path.write_bytes(modified)
        path.chmod(0o755)
        try:
            result = subprocess.run([str(path)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=30, check=False, stdin=subprocess.DEVNULL)
        except OSError as error:
            if accepted or error.errno != errno.ENOEXEC:
                raise
        else:
            if not accepted or result.returncode or result.stdout != b'TLS_ZERO_PASS\n':
                raise AssertionError((name, result.returncode, result.stdout))
        checks.append(name)

    run('original', tls, True)
    good = tls.copy()
    good[3] = good[4] = gap
    run('zero-template-outside-load-segments', good, True)
    bad = good.copy()
    bad[5] = 1
    run('unmapped-file-template', bad, False)
    bad = good.copy()
    bad[2] = len(data) + 1
    run('file-offset-out-of-bounds', bad, False)
    bad = good.copy()
    bad[5] = bad[6] + 1
    run('file-larger-than-memory', bad, False)
    bad = good.copy()
    bad[6] = 128 * 1024 * 1024 + 1
    run('oversized-tls', bad, False)
    bad = good.copy()
    bad[3] = bad[4] = 0xfffffffffffffff0
    run('address-overflow', bad, False)
    bad = good.copy()
    bad[3] = bad[4] = 0x0000800000000000 - 4
    run('crosses-user-address-limit', bad, False)
    bad = good.copy()
    bad[7] = 3
    run('non-power-of-two-alignment', bad, False)
    (work / 'report.json').write_text(json.dumps(dict(state='passed', checks=checks), indent=2) + '\n')
    print('TLS_ALL_PASS ' + str(len(checks)) + ' zero-template and malformed ELF checks', flush=True)


if __name__ == '__main__':
    main()
