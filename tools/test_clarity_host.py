#!/usr/bin/env python3
"""Sanitize actual antialiased edge, icon, cursor and native glyph rendering."""
from pathlib import Path
import argparse,os,subprocess,tempfile
r=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--headers-root',type=Path,default=r);a=p.parse_args()
with tempfile.TemporaryDirectory(prefix='coast-clarity-host-') as temporary:
 binary=Path(temporary)/'check'
 command=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(r/'userland/desktop'),'-I'+str(a.headers_root/'userland/include'),'-I'+str(a.headers_root/'include')]
 command += [str(r/'tools/test_clarity_pixels.c')]+[str(r/'userland/desktop'/name) for name in ['coverage.c','icons.c','cursor.c','font_data.c']]+['-Wl,--gc-sections','-o',str(binary)]
 subprocess.run(command,check=True);subprocess.run([str(binary)],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
