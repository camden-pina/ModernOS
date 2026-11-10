#!/usr/bin/env python3
"""Check production presentation fades and retained-scene isolation."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='coast-fades-') as temporary:
    binary = Path(temporary) / 'check'
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-ffreestanding', '-fno-builtin', '-ffunction-sections', '-fdata-sections',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I' + str(ROOT / 'userland/include'), '-I' + str(ROOT / 'include'),
                    str(ROOT / 'tools/test_desktop_transitions.c'), '-Wl,--gc-sections', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
