#!/usr/bin/env python3
"""Overlay a separate Code OSS shared-service image onto an immutable tested ISO.
No kernel/engine rebuild, disk attachment, download or replacement of old images.
"""
from build_state import atomic_write
from pathlib import Path
import argparse,hashlib,io,json,subprocess,tarfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--base-iso',type=Path,required=True);p.add_argument('--output-root',type=Path,required=True);p.add_argument('--preset',choices=['normal','native'],default='native');p.add_argument('--image-name',default='Coast-CodeOSS-Shared.iso');a=p.parse_args();R=a.root.resolve();O=a.output_root.resolve();base=a.base_iso.resolve()
assert base.is_file() and not O.exists(),'An immutable base and a new output directory are required'
assert Path(a.image_name).name==a.image_name and a.image_name.endswith('.iso')
def sha(f):
 with f.open('rb') as h:return hashlib.file_digest(h,'sha256').hexdigest()
record=json.loads((R/'userland/runtime/usr/share/codeoss/graphical-build.json').read_text())
assert sha(R/'userland/bin/codeoss-model')==record['worker_sha256']
assert sha(R/'userland/bin/codeoss-launch')==record['launcher_sha256']
assert sha(R/'userland/runtime/usr/share/codeoss/graphical-model.js')==record['bundle_sha256']
for rel,expected in record['sources'].items():assert sha(R/rel)==expected,'Source changed since successful build: '+rel
O.mkdir(parents=True);E=O/'base';E.mkdir()
for guest,name in [('/boot/initramfs.tar','initramfs.tar'),('/boot/kernel.elf','kernel.elf'),('/boot/coast.conf','coast.conf')]:subprocess.run(['xorriso','-osirrox','on','-indev',str(base),'-extract',guest,str(E/name)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
extra={f'bin/{n}':R/'userland/bin'/n for n in ['gui','codeoss-model','codeoss-launch']}
for sub in ['usr/share/codeoss','usr/lib/codeoss/relink-model']:
 for f in (R/'userland/runtime'/sub).rglob('*'):
  if f.is_file():extra[str(Path(sub)/f.relative_to(R/'userland/runtime'/sub))]=f
extra['usr/share/coast/CODEOSS-GRAPHICAL.md']=R/'Documentation/CODEOSS-GRAPHICAL.md'
# Redistributable integration sources accompany the worker. Prior upstream engine,
# compiler, Bash, licenses and CLI source from the immutable base are retained.
for folder in ['ports/codeoss','userland/codeoss']:
 for f in (R/folder).rglob('*'):
  if f.is_file():extra['usr/src/coast/codeoss-m4/'+str(f.relative_to(R))]=f
for n in ['build_codeoss_graphical.py','build_codeoss_release.py','test_codeoss_protocol_host.py','test_codeoss_shared_desktop.py']:extra['usr/src/coast/codeoss-m4/tools/'+n]=R/'tools'/n
for f in (R/'userland/desktop').glob('*'):
 if f.is_file():extra['usr/src/coast/codeoss-m4/userland/desktop/'+f.name]=f
for f in [R/'userland/include/codeoss_protocol.h',R/'userland/Makefile']:extra['usr/src/coast/codeoss-m4/'+str(f.relative_to(R))]=f
seen=set();ram=O/'initramfs.tar';overlaid={name:sha(path) for name,path in extra.items()}
with tarfile.open(E/'initramfs.tar') as src,tarfile.open(ram,'w:',format=tarfile.USTAR_FORMAT) as dest:
 for i in src:
  assert i.name not in seen,i.name;seen.add(i.name)
  if i.name=='usr/share/coast/desktop-defaults.ini':
   data=b'scale=0\n' if a.preset=='normal' else b'scale=2\n';i.size=len(data);dest.addfile(i,io.BytesIO(data))
  elif i.name in extra:
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
  i=tarfile.TarInfo(name);i.size=f.stat().st_size;i.mode=0o755 if name.startswith('bin/') else 0o644
  with f.open('rb') as h:dest.addfile(i,h)
  seen.add(name)
preserved={}
with tarfile.open(E/'initramfs.tar') as src,tarfile.open(ram) as final:
 for name in ['bin/coast-sh','bin/jsc','bin/codeoss-core','usr/lib/coast/linux-ports/bash/bash']:
  with src.extractfile(name) as h:before=hashlib.file_digest(h,'sha256').hexdigest()
  with final.extractfile(name) as h:after=hashlib.file_digest(h,'sha256').hexdigest()
  assert before==after,name;preserved[name]=after
settings={}
for line in (E/'coast.conf').read_text().splitlines():
 if line and not line.startswith('#'):
  key,value=line.split('=',1);assert key not in settings;settings[key]=value
assert settings.get('kernel')=='/boot/kernel.elf' and settings.get('module')=='/boot/initramfs.tar'
resolution='1280x800x32' if a.preset=='normal' else '2560x1600x32'
config='version = 1\n[boot]\ntitle = "Coast"\nkernel = "/boot/kernel.elf"\nresolution = '+json.dumps(resolution)+'\ncmdline = '+json.dumps(settings.get('cmdline',''))+'\n[[boot.modules]]\npath = "/boot/initramfs.tar"\nstring = "initramfs"\n[menu]\ntimeout = 0\nresolution = '+json.dumps(resolution)+'\nwallpaper = "/boot/boot-background.jpg"\n';conf=O/'boot.conf';conf.write_text(config);iso=O/a.image_name
subprocess.run(['python3','-B',str(R/'tools/build_boot_iso.py'),'--bootloader','limine','--config',str(conf),'--kernel',str(E/'kernel.elf'),'--initramfs',str(ram),'--source-root',str(R),'--root',str(O/'iso-root'),'--output',str(iso),'--limine-dir',str(R/'build/limine-binary')],check=True)
record={'state':'built; guest validation separate','base_iso':str(base),'base_iso_sha256':sha(base),'iso':str(iso),'iso_sha256':sha(iso),'kernel_sha256':sha(E/'kernel.elf'),'preset':a.preset,'resolution':resolution,'preserved_binaries':preserved,'overlaid_files':overlaid,'host_disk_or_network_attached':False}
atomic_write(O/'MANIFEST.json', json.dumps(record,indent=2)+'\n');print(json.dumps({k:v for k,v in record.items() if k not in ['overlaid_files','preserved_binaries']},indent=2))
