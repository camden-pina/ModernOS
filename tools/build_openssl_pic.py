#!/usr/bin/env python3
"""Cross-build PIC OpenSSL archives for dynamically linked native package tools."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT=Path(__file__).resolve().parents[1]
EXPECTED='796831ec533bf41998eaaee9d632d921b8a69f57394fa4086ff01a941113d26d'


def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output-root',type=Path,required=True)
    a=p.parse_args();out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=False)
    archive=ROOT/'userland/vendor/openssl-3.5.9-source.tar.gz';assert sha(archive)==EXPECTED
    with tarfile.open(archive) as tar:tar.extractall(out,filter='data')
    source=out/'openssl-openssl-3.5.9';build=out/'objects';build.mkdir();prefix=out/'prefix'
    before={str(path.relative_to(source)):sha(path) for path in source.rglob('*') if path.is_file()}
    cross=ROOT/'build/compiler/cross/bin/x86_64-linux-musl-'
    flags='-O2 -g0 -fPIC -fno-stack-protector -mno-avx -mno-avx2 -idirafter '+str(ROOT/'build/qt6/port-r1/linux-uapi')
    env=dict(PATH='/usr/bin:/bin',HOME='/tmp',TMPDIR='/tmp',LANG='C.UTF-8',CC=str(cross)+'gcc',
             AR=str(cross)+'ar',RANLIB=str(cross)+'ranlib',CFLAGS=flags,LDFLAGS='')
    conf=['bwrap','--unshare-all','--die-with-parent','--new-session','--ro-bind','/','/',
          '--proc','/proc','--dev','/dev','--tmpfs','/tmp','--bind',str(out),str(out),
          '--ro-bind',str(source),str(source),'--chdir',str(build),'--']
    commands=[['perl',str(source/'Configure'),'linux-x86_64','threads','no-shared','no-module','no-tests',
               'no-asm','no-engine','--with-rand-seed=getrandom','--prefix=/usr','--openssldir=/etc/ssl','--libdir=lib'],
              ['make','-j8','build_libs']]
    for i,command in enumerate(commands):
        with (out/('build-'+str(i)+'.log')).open('w') as log:
            subprocess.run(conf+command,env=env,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,check=True)
    (prefix/'lib').mkdir(parents=True);(prefix/'include/openssl').mkdir(parents=True)
    for name in ('libssl.a','libcrypto.a'):shutil.copyfile(build/name,prefix/'lib'/name)
    for directory in (source/'include/openssl',build/'include/openssl'):
        for header in directory.glob('*.h'):shutil.copyfile(header,prefix/'include/openssl'/header.name)
    assert before=={str(path.relative_to(source)):sha(path) for path in source.rglob('*') if path.is_file()}
    receipt=dict(state='compiled; guest checks pending',version='3.5.9',prefix=str(prefix),source_archive=str(archive),
                 source_archive_sha256=EXPECTED,commands=commands,confinement=conf,environment=env,
                 target_host_executed=False,source_sha256={'tools/build_openssl_pic.py':sha(Path(__file__))},
                 artifacts={str(path):sha(path) for path in prefix.rglob('*') if path.is_file()})
    (out/'BUILD.json').write_text(json.dumps(receipt,indent=2)+'\n');print('PIC OpenSSL archives built for native package tools.')


if __name__=='__main__':main()
