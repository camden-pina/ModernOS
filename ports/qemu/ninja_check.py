#!/usr/bin/env python3
"""Exercise headless Ninja jobs and signal forwarding across Coast sessions."""
import argparse
import errno
from pathlib import Path
import os
import signal
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ninja', type=Path, required=True)
    args = parser.parse_args()
    work = Path(tempfile.mkdtemp(prefix='ninja-native-', dir='/tmp'))
    (work / 'worker.py').write_text('import os,sys,time\nfrom pathlib import Path\n'
        'Path(sys.argv[1]).write_text(str(os.getpid()))\ntime.sleep(30)\n')
    (work / 'build.ninja').write_text('rule worker\n  command = python3 worker.py $out\n'
        'build first.pid: worker\nbuild second.pid: worker\ndefault first.pid second.pid\n')
    process = None
    children = []
    with (work / 'ninja.log').open('w') as log:
        try:
            process = subprocess.Popen([str(args.ninja.resolve()), '-j2'], cwd=work,
                stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 20
            while not all((work / name).is_file() for name in ('first.pid', 'second.pid')):
                if process.poll() is not None or time.monotonic() > deadline:
                    raise AssertionError('Headless Ninja jobs did not start: ' + (work / 'ninja.log').read_text())
                time.sleep(.05)
            children = [int((work / name).read_text()) for name in ('first.pid', 'second.pid')]
            assert len(set(children)) == 2
            groups = [os.getpgid(child) for child in children]
            assert len(set(groups)) == 2 and os.getpgrp() not in groups
            for child, group in zip(children, groups):
                assert os.getsid(child) == group
            process.send_signal(signal.SIGINT)
            assert process.wait(timeout=20) != 0
            for child in children:
                try:
                    os.kill(child, 0)
                except OSError as error:
                    assert error.errno == errno.ESRCH, error
                else:
                    raise AssertionError('Ninja left an interrupted worker running')
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            for child in children:
                try:
                    os.kill(child, signal.SIGKILL)
                except ProcessLookupError:
                    pass
    print('NINJA_NATIVE_PASS parallel headless jobs, private sessions, SIGINT forwarding and complete worker cleanup', flush=True)


if __name__ == '__main__':
    main()
