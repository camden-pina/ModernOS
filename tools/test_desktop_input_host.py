#!/usr/bin/env python3
"""Sanitize the actual UTF-8 selection, pointer model, HID parser and input core."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='coast-desktop-input-') as temporary:
    binary=Path(temporary)/'check'
    command=['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(ROOT/'userland/desktop'),'-idirafter',str(ROOT/'include'),str(ROOT/'tools/tests/desktop_input.c'),str(ROOT/'drivers/usb/hid_mouse.c'),str(ROOT/'drivers/input/core/mouse.c'),'-o',str(binary)]
    subprocess.run(command,check=True);subprocess.run([str(binary)],check=True)
