#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile,os,argparse
p=argparse.ArgumentParser();p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1]);a=p.parse_args();r=a.root
with tempfile.TemporaryDirectory(prefix='coast-codeoss-host-') as t:
 out=Path(t);binary=out/'check';data=out/'files';data.mkdir()
 subprocess.run(['c++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(r/'tools/test_codeoss_file_service.cpp'),str(r/'userland/codeoss/file-service.cpp'),'-Wl,--wrap=write,--wrap=fsync,--wrap=close,--wrap=syscall','-o',str(binary)],check=True)
 subprocess.run([str(binary),str(data)],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
