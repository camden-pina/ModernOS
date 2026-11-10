#!/usr/bin/env python3
"""Package a separate Terminal image from an explicit immutable r7 base ISO.
Uses cached build tools; never builds a kernel/engine or attaches a host disk.
"""
from build_state import atomic_write
from pathlib import Path
import argparse,hashlib,io,json,re,subprocess,tarfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--base-iso',type=Path,required=True);p.add_argument('--bash-build-root',type=Path,required=True);p.add_argument('--output-root',type=Path,required=True);p.add_argument('--preset',choices=['normal','native'],default='native');p.add_argument('--image-name',default='Coast-Coast-Terminal8.iso');a=p.parse_args()
R=a.root.resolve();B=a.bash_build_root.resolve();O=a.output_root.resolve();base=a.base_iso.resolve()
assert base.is_file() and not O.exists(),'Use an existing base ISO and a new output directory'
assert Path(a.image_name).name==a.image_name and a.image_name.endswith('.iso')
def sha(path):
 with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
record=json.loads((B/'build.json').read_text());binary=B/'bash-5.3/bash';assert sha(binary)==record['binary_sha256'];assert record.get('coast_parent_tty_patch_sha256')==sha(R/'ports/bash/COAST-PARENT-TTY.patch')
O.mkdir(parents=True);extracted=O/'base';extracted.mkdir()
for guest,name in [('/boot/initramfs.tar','initramfs.tar'),('/boot/kernel.elf','kernel.elf'),('/boot/coast.conf','coast.conf')]:
 subprocess.run(['xorriso','-osirrox','on','-indev',str(base),'-extract',guest,str(extracted/name)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
# Explicit guest fixture uses only its own process groups and terminal.
fixture=O/'terminal-job-check';subprocess.run([str(R/'build/compiler/cross/bin/x86_64-linux-musl-gcc'),'-std=gnu11','-O2','-Wall','-Wextra','-Werror','-fno-pie','-fno-stack-protector','-static','-no-pie','-Wl,-z,max-page-size=4096,-z,noexecstack',str(R/'ports/bash/terminal-job-check.c'),'-o',str(fixture)],check=True)
extra={'usr/lib/coast/linux-ports/bash/bash':binary,'bin/terminal-job-check':fixture,'usr/share/coast/shells/bash-readline-build.json':B/'build.json','usr/src/coast/shells/build_bash_readline.py':R/'tools/build_bash_readline.py','usr/src/coast/shells/COAST-PARENT-TTY.patch':R/'ports/bash/COAST-PARENT-TTY.patch','usr/src/coast/shells/terminal-job-check.c':R/'ports/bash/terminal-job-check.c','usr/src/coast/shells/build_terminal_release.py':Path(__file__).resolve()}
for name in ['BASH-FOREGROUND-HANDOFF-2026-10-25.md','SHELL-DEPENDENCY-ASSESSMENT.md']:extra['usr/share/coast/'+name]=R/'Documentation'/name
seen=set();ram=O/'initramfs.tar';binaries={};preserved=[]
with tarfile.open(extracted/'initramfs.tar') as src,tarfile.open(ram,'w:',format=tarfile.USTAR_FORMAT) as dest:
 for i in src:
  assert i.name not in seen,i.name;seen.add(i.name)
  if i.name=='usr/share/coast/desktop-defaults.ini':
   data=b'scale=0\n' if a.preset=='normal' else b'scale=2\n';i.size=len(data);dest.addfile(i,io.BytesIO(data))
  elif i.name in extra:
   path=extra.pop(i.name);i.size=path.stat().st_size
   with path.open('rb') as f:dest.addfile(i,f)
  elif i.isfile():
   with src.extractfile(i) as f:dest.addfile(i,f)
  else:dest.addfile(i)
 def parents(name):
  for x in reversed(Path(name).parents):
   key=str(x)
   if key!='.' and key not in seen:
    i=tarfile.TarInfo(key);i.type=tarfile.DIRTYPE;i.mode=0o755;dest.addfile(i);seen.add(key)
 for name,path in extra.items():
  assert name not in seen;parents(name);i=tarfile.TarInfo(name);i.size=path.stat().st_size;i.mode=0o755 if name.startswith('bin/') else 0o644
  with path.open('rb') as f:dest.addfile(i,f)
  seen.add(name)
# Prove the existing desktop, default shell and upstream text core were retained.
with tarfile.open(extracted/'initramfs.tar') as src,tarfile.open(ram) as final:
 for name in ['bin/gui','bin/coast-sh','bin/jsc','bin/codeoss-core']:
  with src.extractfile(name) as f:before=hashlib.file_digest(f,'sha256').hexdigest()
  with final.extractfile(name) as f:after=hashlib.file_digest(f,'sha256').hexdigest()
  assert before==after,name;binaries[name]=after
# /boot/coast.conf is the loader's key=value payload, not shared TOML.
settings={}
for line in (extracted/'coast.conf').read_text().splitlines():
 if line and not line.startswith('#'):
  key,value=line.split('=',1);assert key not in settings,'Unsupported repeated boot setting';settings[key]=value
assert settings.get('kernel')=='/boot/kernel.elf' and settings.get('module')=='/boot/initramfs.tar' and settings.get('module_string')=='initramfs','Expected an immutable r7 base layout'
resolution='1280x800x32' if a.preset=='normal' else '2560x1600x32'
config='version = 1\n[boot]\ntitle = "Coast"\nkernel = "/boot/kernel.elf"\nresolution = '+json.dumps(resolution)+'\ncmdline = '+json.dumps(settings.get('cmdline',''))+'\n[[boot.modules]]\npath = "/boot/initramfs.tar"\nstring = "initramfs"\n[menu]\ntimeout = 0\nresolution = '+json.dumps(resolution)+'\nwallpaper = "/boot/boot-background.jpg"\n'
conf=O/'boot.conf';conf.write_text(config)
iso=O/a.image_name;subprocess.run(['python3','-B',str(R/'tools/build_boot_iso.py'),'--bootloader','limine','--config',str(conf),'--kernel',str(extracted/'kernel.elf'),'--initramfs',str(ram),'--source-root',str(R),'--root',str(O/'iso-root'),'--output',str(iso),'--limine-dir',str(R/'build/limine-binary')],check=True)
manifest={'state':'built; guest verification is separate','base_iso':str(base),'base_iso_sha256':sha(base),'iso':str(iso),'iso_sha256':sha(iso),'initramfs_sha256':sha(ram),'kernel_sha256':sha(extracted/'kernel.elf'),'bash_sha256':sha(binary),'bash_build':record,'preserved_binaries':binaries,'fixture_sha256':sha(fixture),'preset':a.preset,'resolution':resolution,'default_scale':100 if a.preset=='normal' else 150,'host_disk_or_network_attached':False}
atomic_write(O/'MANIFEST.json', json.dumps(manifest,indent=2)+'\n');print(json.dumps({'result':'built','iso':str(iso),'sha256':manifest['iso_sha256'],'prior_images_unchanged':True},indent=2),flush=True)
