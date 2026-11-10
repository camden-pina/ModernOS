#!/usr/bin/env python3
"""Overlay development package-security tools on a preserved QA image."""
from build_state import project_lock
from build_state import atomic_write
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

ROOT=Path(__file__).resolve().parents[1]


def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base-iso',type=Path,required=True)
    p.add_argument('--boot-config',type=Path,required=True)
    p.add_argument('--kernel',type=Path,required=True)
    p.add_argument('--fixtures',type=Path,required=True)
    p.add_argument('--pacman',type=Path,required=True)
    p.add_argument('--output-root',type=Path,required=True)
    p.add_argument('--fixtures-only',action='store_true',help='preserve every product file and add only the three test executables')
    a=p.parse_args();out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=False)
    files={'usr/bin/'+name:a.fixtures/name for name in ('package-seccomp-check','package-landlock-check','package-alpm-sandbox-check')}
    files['usr/bin/pacman']=a.pacman
    for name in ('bsdtar','bsdcpio','bsdcat','bsdunzip'):
        files['usr/bin/'+name]=a.pacman.parent.parent/'prefix/bin'/name
    for name in ('client.py','search.py','community.py'):files['usr/lib/coast/packages/'+name]=ROOT/'userland/packages'/name
    pacman=(ROOT/'userland/packages/config/pacman.conf').read_text().replace('file:///var/lib/coast/repository/coast/os/$arch','http://10.0.2.2:8765/repo/$arch')
    (out/'pacman.conf').write_text(pacman);files['etc/pacman.conf']=out/'pacman.conf'
    config=json.loads((ROOT/'build/local-repository-r20/generated/etc/coast/packages.json').read_text());config['community_url']='http://10.0.2.2:8770'
    atomic_write(out/'packages.json', json.dumps(config,indent=2)+'\n');files['etc/coast/packages.json']=out/'packages.json'
    if a.fixtures_only:files={name:path for name,path in files.items() if name.startswith('usr/bin/package-')}
    archive=out/'package-security-qa.tar'
    with tarfile.open(archive,'w') as tar:
        for name,path in files.items():
            data=path.read_bytes();info=tarfile.TarInfo(name);info.size=len(data);info.uid=info.gid=0;info.mtime=1793836800
            info.mode=0o755 if name.startswith('usr/bin/') else 0o644;tar.addfile(info,io.BytesIO(data))
    config=a.boot_config.read_text();assert config.count('\n/Reboot')==1
    config=config.replace('\n/Reboot','    module_path: boot():/boot/package-security-qa.tar\n    module_string: initramfs\n\n/Reboot')
    (out/'limine.conf').write_text(config)
    image=out/'Coast-security.iso'
    subprocess.run(['cp','--reflink=auto',str(a.base_iso.resolve()),str(image)],check=True)
    with (out/'image.log').open('w') as log:
        subprocess.run(['xorriso','-no_rc','-dev',str(image),'-map',str(a.kernel.resolve()),'/boot/kernel.elf',
            '-map',str(archive),'/boot/package-security-qa.tar','-map',str(out/'limine.conf'),'/boot/limine/limine.conf',
            '-boot_image','any','keep','-commit','-end'],stdout=log,stderr=subprocess.STDOUT,check=True)
    manifest=dict(state='QA overlay; not selected',iso=str(image),iso_sha256=sha(image),kernel=str(a.kernel.resolve()),
        kernel_sha256=sha(a.kernel),base_iso=str(a.base_iso.resolve()),base_sha256=sha(a.base_iso),
        files={name:dict(source=str(path.resolve()),sha256=sha(path)) for name,path in files.items()},
        target_host_executed=False,host_partitions_modified=False)
    atomic_write(out/'BUILD.json', json.dumps(manifest,indent=2)+'\n')


if __name__ == '__main__':
    with project_lock():
        main()
