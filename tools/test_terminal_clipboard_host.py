#!/usr/bin/env python3
"""Sanitize the actual native Terminal paste and clipboard lifecycle code."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='coast-terminal-clipboard-') as temporary:
    binary=Path(temporary)/'check'
    command=['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(ROOT/'userland/include'),'-I',str(ROOT/'include'),str(ROOT/'tools/tests/terminal_clipboard.c'),'-Wl,--gc-sections','-o',str(binary)]
    subprocess.run(command,check=True);subprocess.run([str(binary)],check=True)
