#!/usr/bin/env python3
"""Build an isolated upstream codium ELF/ABI preflight image, not a runnable IDE.
Uses cached project tooling, an explicit immutable base ISO and a new output.
"""
from pathlib import Path
import argparse,subprocess,json,tarfile,hashlib

def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def run(args):subprocess.run(list(map(str,args)),check=True)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--base-iso',type=Path,required=True);p.add_argument('--codium-executable',type=Path,required=True);p.add_argument('--output-root',type=Path,required=True);a=p.parse_args();R=a.root.resolve();O=a.output_root.resolve();base=a.base_iso.resolve();codium=a.codium_executable.resolve();assert base.is_file() and codium.is_file();basehash=sha(base);O.mkdir(parents=True,exist_ok=False);cross=R/'build/compiler/cross/bin/x86_64-linux-musl-gcc';assert cross.is_file();contract=O/'codeoss-app-contract';run([cross,'-std=c11','-O2','-Wall','-Wextra','-Werror','-static','-no-pie','-fno-stack-protector','-pthread',R/'ports/codeoss-app/platform-contract.c','-o',contract]);run(['xorriso','-osirrox','on','-indev',base,'-extract','/boot/initramfs.tar',O/'base-initramfs.tar','-extract','/boot/kernel.elf',O/'kernel.elf']);ram=O/'initramfs.tar';extra={'bin/codeoss-app-contract':contract,'opt/vscodium/codium':codium};seen=set()
 with tarfile.open(O/'base-initramfs.tar') as src,tarfile.open(ram,'w:',format=tarfile.USTAR_FORMAT) as dest:
  for i in src:
   seen.add(i.name)
   if i.name in extra:
    f=extra.pop(i.name);i.size=f.stat().st_size
    with f.open('rb') as h:dest.addfile(i,h)
   elif i.isfile():
    with src.extractfile(i) as h:dest.addfile(i,h)
   else:dest.addfile(i)
  for name,f in extra.items():
   for parent in reversed(Path(name).parents):
    key=str(parent)
    if key!='.' and key not in seen:
     i=tarfile.TarInfo(key);i.type=tarfile.DIRTYPE;i.mode=0o755;dest.addfile(i);seen.add(key)
   i=tarfile.TarInfo(name);i.mode=0o755;i.size=f.stat().st_size
   with f.open('rb') as h:dest.addfile(i,h)
 conf=O/'boot.conf';conf.write_text('version = 1\n[boot]\ntitle = "Coast application preflight"\nkernel = "/boot/kernel.elf"\nresolution = "1280x800x32"\ncmdline = ""\n[[boot.modules]]\npath = "/boot/initramfs.tar"\nstring = "initramfs"\n[menu]\ntimeout = 0\nresolution = "1280x800x32"\nwallpaper = "/boot/boot-background.jpg"\n');iso=O/'Coast-CodeOSS-App-Preflight.iso';run(['python3','-B',R/'tools/build_boot_iso.py','--bootloader','limine','--config',conf,'--kernel',O/'kernel.elf','--initramfs',ram,'--source-root',R,'--root',O/'iso-root','--output',iso,'--limine-dir',R/'build/limine-binary']);assert sha(base)==basehash
 record={'purpose':'Original prerequisite probe plus unmodified upstream codium ELF only; full application resources are not staged and this is not a runnable IDE','base_iso':str(base),'base_iso_sha256':basehash,'kernel_sha256':sha(O/'kernel.elf'),'codium_sha256':sha(codium),'contract_sha256':sha(contract),'iso_sha256':sha(iso),'guest_validation':'not yet run','host_installations':False};(O/'MANIFEST.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record,indent=2))
if __name__=='__main__':main()
