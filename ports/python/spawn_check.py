#!/usr/bin/env python3
"""Stress native Python's threaded subprocess path without changing fork policy."""
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import platform
import subprocess
import tempfile

if platform.system() != 'Coast':
    raise RuntimeError('Run the native subprocess regression inside Coast')
work = Path(tempfile.mkdtemp(prefix='python-spawn-', dir='/tmp'))
parent, original = os.getpid(), Path.cwd()


def worker(index):
    directory = work / str(index)
    directory.mkdir()
    for _ in range(16):
        child = subprocess.run(['/bin/pwd'], cwd=directory, capture_output=True, text=True, timeout=30)
        assert child.returncode == 0 and child.stdout.strip() == str(directory), child
        child = subprocess.run(['/bin/false'], cwd=directory, capture_output=True, timeout=30)
        assert child.returncode == 1
        assert os.getpid() == parent and Path.cwd() == original


with ThreadPoolExecutor(max_workers=4) as executor:
    list(executor.map(worker, range(4)))
print('PYTHON_THREADED_SPAWN_PASS 128 children; cwd, pid and exit status preserved', flush=True)
