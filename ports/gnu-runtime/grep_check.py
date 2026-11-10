#!/usr/bin/env python3
"""Exercise GNU grep's pipe-to-null fallback and native Autoconf-style searches."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, default=Path('/'))
    args = parser.parse_args()
    if platform.system() != 'Coast':
        parser.error('Run grep qualification inside Coast')
    grep = args.runtime.resolve(strict=True) / 'usr/bin/grep'
    work = Path(tempfile.mkdtemp(prefix='grep-check-', dir='/tmp'))
    checks = []

    def run(arguments, data, expected, output=None):
        result = subprocess.run([str(grep), *arguments], input=data, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=30)
        with (work / 'check.log').open('ab') as log:
            log.write(repr(arguments).encode() + b'\n' + result.stdout + result.stderr)
        if result.returncode != expected or output is not None and result.stdout != output:
            raise AssertionError((arguments, result.returncode, result.stdout, result.stderr))
        return result

    assert b'3.12' in run(['--version'], b'', 0).stdout
    run(['-En', '^a.*z$'], b'abz\nother\naz\n', 0, b'1:abz\n3:az\n')
    run(['-F', 'a.b'], b'a.b\naxb\n', 0, b'a.b\n')
    run(['absent'], b'present\n', 1, b'')
    assert run(['-E', '['], b'', 2).stderr
    checks.append('GNU version, regular/fixed expressions, line numbers and exact failure statuses')
    for pattern, code in [('X', 0), ('absent', 1)]:
        producer = subprocess.Popen([sys.executable, '-c',
            'import os,sys\ndata=b"X\\n"+b"a\\n"*524287\n'
            'done=0\nwhile done<len(data):done+=os.write(1,data[done:])\n'
            'print("PRODUCER_BYTES="+str(done),file=sys.stderr)'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        consumer = subprocess.Popen([str(grep), pattern], stdin=producer.stdout,
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        producer.stdout.close()
        errors = consumer.communicate(timeout=60)[1]
        producer_error = producer.communicate(timeout=60)[1]
        assert consumer.returncode == code and not errors, (consumer.returncode, errors)
        assert producer.returncode == 0 and producer_error == b'PRODUCER_BYTES=1048576\n', producer_error
    checks.append('matching and nonmatching 1 MiB pipes drain completely with stdout on /dev/null')
    # This is the search used by older Automake config.status depfile setup.
    run(['X'], b'X\n', 0, b'X\n')
    (work / 'report.json').write_text(json.dumps(dict(state='passed', checks=checks,
        grep_sha256=hashlib.sha256(grep.read_bytes()).hexdigest()), indent=2) + '\n')
    print('GREP_ALL_PASS ' + str(len(checks)) + ' report=' + str(work / 'report.json'), flush=True)


if __name__ == '__main__':
    main()
