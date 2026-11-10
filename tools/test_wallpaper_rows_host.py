#!/usr/bin/env python3
from pathlib import Path
import argparse,os,subprocess,tempfile
r=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--headers-root',type=Path,default=r);a=p.parse_args()
with tempfile.TemporaryDirectory(prefix='coast-wallpaper-rows-') as t:
 b=Path(t)/'check'
 subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(r/'userland/desktop'),'-I'+str(a.headers_root/'userland/include'),'-I'+str(a.headers_root/'include'),str(r/'tools/test_wallpaper_rows.c'),'-Wl,--gc-sections','-o',str(b)],check=True)
 subprocess.run([str(b)],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
