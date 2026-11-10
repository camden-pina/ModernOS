#!/usr/bin/env python3
"""Build a separate job-control kernel/GUI image from an explicit immutable base.
No host packages, credentials, raw disks, networking or running VMs are used.
"""
from build_state import project_lock
from build_state import atomic_write
from pathlib import Path
import argparse,hashlib,io,json,subprocess,tarfile

def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def run(cmd):subprocess.run(list(map(str,cmd)),check=True)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--base-iso',type=Path,required=True);p.add_argument('--output-root',type=Path,required=True);p.add_argument('--preset',choices=('normal','native'),default='normal');p.add_argument('--jobs',type=int,default=4);p.add_argument('--kernel',type=Path);p.add_argument('--gui',type=Path);a=p.parse_args();R=a.root.resolve();O=a.output_root.resolve();base=a.base_iso.resolve();assert base.is_file() and base.suffix=='.iso';assert 1<=a.jobs<=16;O.mkdir(parents=True,exist_ok=False)
 basehash=sha(base);frozen=R/'build/vmlinux';frozenhash=sha(frozen);kernel=a.kernel.resolve() if a.kernel else O/'kernel/vmlinux'
 if not a.kernel:
  assert (R/'userland/vendor/lwip-2.2.1.tar.gz').is_file(),'Cached verified lwIP archive required; no automatic download'
  run(['make','-C',R,'O='+str(O/'kernel'),'-j'+str(a.jobs),'vmlinux'])
 gui=a.gui.resolve() if a.gui else O/'ui/gui'
 if not a.gui:
  ui=O/'ui';ui.mkdir();objects=[]
  flags=['-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-pie','-fno-stack-protector','-m64','-mno-avx','-mno-avx2','-I'+str(R/'userland/include'),'-I'+str(R/'include'),'-I'+str(R/'arch/x86/include/asm')]
  for src in sorted((R/'userland/desktop').glob('*.c')):
   obj=ui/(src.stem+'.o');run(['cc',*flags,'-c',src,'-o',obj]);objects.append(obj)
  runtime=[R/'userland/obj'/name for name in ['crt0.o','libc.o','gui.o']];completion=[R/'userland/obj'/name for name in ['shell_completion.o','shell_completion_native.o']]
  assert all(x.is_file() for x in runtime+completion),'Existing native runtime relink objects required'
  run(['ld','-nostdlib','-static','-z','max-page-size=4096','-z','noexecstack','-T',R/'userland/user.ld',*runtime,*objects,*completion,'-o',gui])
 compiler=R/'build/compiler/cross/bin/x86_64-linux-musl-gcc';assert compiler.is_file(),'Existing project musl toolchain required'
 fixture=O/'job-control-check';run([compiler,'-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread','-static','-no-pie','-fno-stack-protector',R/'ports/terminal/job-control-check.c','-o',fixture])
 base_ram=O/'base-initramfs.tar';run(['xorriso','-osirrox','on','-indev',base,'-extract','/boot/initramfs.tar',base_ram]);ram=O/'initramfs.tar';extras={'bin/gui':gui,'bin/job-control-check':fixture};seen=set()
 with tarfile.open(base_ram) as src,tarfile.open(ram,'w:',format=tarfile.USTAR_FORMAT) as dest:
  for item in src:
   seen.add(item.name)
   if item.name=='usr/share/coast/desktop-defaults.ini':
    data=('scale='+('2' if a.preset=='native' else '0')+'\nterminal_shell=1\n').encode();item.size=len(data);dest.addfile(item,io.BytesIO(data))
   elif item.name in extras:
    file=extras.pop(item.name);item.size=file.stat().st_size
    with file.open('rb') as f:dest.addfile(item,f)
   elif item.isfile():
    with src.extractfile(item) as f:dest.addfile(item,f)
   else:dest.addfile(item)
  for name,file in extras.items():
   assert str(Path(name).parent) in seen,name
   item=tarfile.TarInfo(name);item.mode=0o755;item.size=file.stat().st_size
   with file.open('rb') as f:dest.addfile(item,f)
 resolution='2560x1600x32' if a.preset=='native' else '1280x800x32';conf=O/'boot.conf';conf.write_text('version = 1\n[boot]\ntitle = "Coast"\nkernel = "/boot/kernel.elf"\nresolution = "'+resolution+'"\ncmdline = ""\n[[boot.modules]]\npath = "/boot/initramfs.tar"\nstring = "initramfs"\n[menu]\ntimeout = 0\nresolution = "'+resolution+'"\nwallpaper = "/boot/boot-background.jpg"\n')
 iso=O/('Coast-Terminal-Jobs'+('-native' if a.preset=='native' else '')+'-tested.iso')
 run(['python3','-B',R/'tools/build_boot_iso.py','--bootloader','limine','--config',conf,'--kernel',kernel,'--initramfs',ram,'--source-root',R,'--root',O/'iso-root','--output',iso,'--limine-dir',R/'build/limine-binary'])
 assert sha(base)==basehash and sha(frozen)==frozenhash,'Preserved input changed'
 record={'iso':str(iso),'iso_sha256':sha(iso),'base_iso':str(base),'base_iso_sha256':basehash,'kernel':str(kernel),'kernel_sha256':sha(kernel),'original_frozen_kernel_sha256':frozenhash,'gui_sha256':sha(gui),'fixture_sha256':sha(fixture),'preset':a.preset,'resolution':resolution,'default_scale':150 if a.preset=='native' else 100,'fresh_default_shell':'GNU Bash; existing per-profile selection remains authoritative','validation':'not yet run; filename is a release convention, not a validation claim','extra_disks_or_network':False};atomic_write(O/'MANIFEST.json', json.dumps(record,indent=2)+'\n');print(json.dumps(record,indent=2))
if __name__ == '__main__':
    with project_lock():
        main()
