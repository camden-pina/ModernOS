#!/usr/bin/env python3
"""Exercise the production read-only disk inventory parser, including malformed snapshots."""
import os
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='coast-disk-parser-') as temp:
    exe = Path(temp) / 'check'
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I'+str(root/'userland/desktop'),
                    str(root/'tools/test_disk_inventory.c'), '-o', str(exe)], check=True)
    # This stack-only parser has no heap ownership to leak-check; some Codex
    # execution environments use ptrace, which LeakSanitizer cannot support.
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
    subprocess.run([str(exe)], env=env, check=True)
