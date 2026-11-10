#!/usr/bin/env python3
"""Sanitize actual native font measurement, glyph painting and label clipping."""
import pathlib,subprocess,tempfile
R=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='coast-desktop-text-') as name:
 out=pathlib.Path(name)/'check'
 subprocess.run(['gcc','-O1','-g','-std=gnu11','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(R/'userland/include'),'-I'+str(R/'include'),str(R/'tools/tests/desktop_text.c'),str(R/'userland/desktop/font_data.c'),'-Wl,--gc-sections','-o',str(out)],check=True)
 subprocess.run([str(out)],check=True)
