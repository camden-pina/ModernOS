#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile,argparse
p=argparse.ArgumentParser();p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--headers-root',type=Path);a=p.parse_args();r=a.root;h=a.headers_root or r
with tempfile.TemporaryDirectory(prefix='coast-personal-icons-') as t:
 b=Path(t)/'check';subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(h/'userland/include'),'-I'+str(h/'include'),str(r/'tools/tests/personal_icons.c'),str(r/'userland/desktop/pixel_blend.c'),'-Wl,--gc-sections','-o',str(b)],check=True);subprocess.run([str(b)],check=True)
