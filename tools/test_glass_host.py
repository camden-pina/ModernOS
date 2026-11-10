#!/usr/bin/env python3
"""Sanitize production glass kernel and premultiplied cached foreground painter."""
from pathlib import Path
import tempfile,subprocess,os
r=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='coast-glass-host-') as t:
 flags=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(r/'userland/desktop')]
 for name,extra in [('test_glass_filter.c',[]),('test_glass_pixels.c',[str(r/'userland/desktop/glass.c'),str(r/'userland/desktop/window_manager.c'),str(r/'userland/desktop/coverage.c'),str(r/'userland/desktop/pixel_blend.c'),str(r/'userland/desktop/theme.c')])]:
  includes=[] if name=='test_glass_filter.c' else ['-I'+str(r/'userland/include'),'-I'+str(r/'include')]
  exe=Path(t)/name[:-2];subprocess.run(flags+includes+[str(r/'tools'/name)]+extra+['-Wl,--gc-sections','-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
