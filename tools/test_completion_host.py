#!/usr/bin/env python3
"""Sanitize production shared completion and its real Linux filesystem adapter."""
from pathlib import Path
import subprocess,tempfile,os
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='coast-completion-') as temporary:
 work=Path(temporary);program=work/'native-command';program.write_text('#!/bin/sh\ntouch '+str(work/'MUST_NOT_EXECUTE')+'\n');program.chmod(0o755)
 binary=work/'check'
 subprocess.run(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-ffreestanding','-fno-builtin','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(ROOT/'userland/include'),'-I'+str(ROOT/'include'),str(ROOT/'tools/tests/shell_completion.c'),str(ROOT/'userland/shell_completion.c'),str(ROOT/'userland/shell_completion_native.c'),'-o',str(binary)],check=True)
 subprocess.run([str(binary),str(work)],check=True);assert not (work/'MUST_NOT_EXECUTE').exists()
