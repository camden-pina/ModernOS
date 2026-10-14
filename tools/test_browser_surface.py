#!/usr/bin/env python3
"""Check the compositor's actual untrusted surface-copy operation with ASan/UBSan."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/browser-surface-test'
BUILD.mkdir(exist_ok=True)
binary = BUILD / 'surface-check'
subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                str(ROOT / 'tools/browser_surface_check.c'), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
