#!/usr/bin/env python3
"""Compare the same credential fixture on real Linux and Coast guests."""
import argparse,base64,gzip,hashlib,json,os,select,subprocess,time,traceback
from pathlib import Path
import test_runtime as runtime
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--iso',type=Path,required=True);p.add_argument('--output-root',type=Path,required=True);p.add_argument('--gui-build',type=Path,required=True)
p.add_argument('--linux-kernel',type=Path,default=Path('/usr/lib/modules/6.18.55-2-cachyos-lts/vmlinuz'))
a=p.parse_args();out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=False)
receipt=json.loads((a.gui_build/'BUILD.json').read_text());conf=[str(out) if v==str(a.gui_build.resolve()) else v for v in receipt['confinement']]
cc=next(c for c in receipt['commands'] if str(root/'userland/profile-session.c') in c)[:]
cc[cc.index(str(root/'userland/profile-session.c'))]=str(root/'tools/credential-parity.c');cc[-1]=str(out/'fixture.o')
env=dict(HOME='/home/camdenp',PATH='/usr/bin:/bin',LANG='C.UTF-8',TMPDIR=str(out))
subprocess.run(conf+['--',*cc],check=True,env=env,stdin=subprocess.DEVNULL)
ld=['/usr/bin/ld','-nostdlib','-static','-z','max-page-size=4096','-z','noexecstack','-T',str(root/'userland/user.ld'),str(a.gui_build.resolve()/'crt0.S.o'),str(a.gui_build.resolve()/'libc.c.o'),str(out/'fixture.o'),'-o',str(out/'credential-parity')]
subprocess.run(conf+['--',*ld],check=True,env=env,stdin=subprocess.DEVNULL)
subprocess.run(['strip','--strip-all',str(out/'credential-parity')],check=True)
binary=(out/'credential-parity').read_bytes()
archive=bytearray()
def entry(name,mode,data=b'',rdev=(0,0)):
 fields=[1,mode,0,0,1,0,len(data),0,0,*rdev,len(name.encode())+1,0]
 archive.extend(b'070701'+''.join(f'{v:08x}' for v in fields).encode()+name.encode()+b'\0')
 archive.extend(b'\0'*(-len(archive)%4));archive.extend(data);archive.extend(b'\0'*(-len(archive)%4))
entry('.',0o40755);entry('dev',0o40755);entry('dev/console',0o20600,rdev=(5,1));entry('dev/null',0o20666,rdev=(1,3));entry('dev/zero',0o20666,rdev=(1,5));entry('init',0o100755,binary);entry('TRAILER!!!',0)
(out/'linux-initramfs.cpio').write_bytes(archive)
cmd=['qemu-system-x86_64','-enable-kvm','-cpu','host','-smp','2','-m','512M','-kernel',str(a.linux_kernel),'-initrd',str(out/'linux-initramfs.cpio'),'-append','console=ttyS0 quiet panic=-1','-display','none','-serial','stdio','-nic','none','-no-reboot']
vm=None;linux=None;report=dict(state='failed',fixture_sha256=hashlib.sha256(binary).hexdigest(),linux_kernel=str(a.linux_kernel),iso=str(a.iso.resolve()),fixture_host_executed=False)
try:
 linux=subprocess.Popen(cmd,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,start_new_session=True);data=bytearray();end=time.monotonic()+45
 while time.monotonic()<end and b'CREDENTIAL_PARITY_DONE' not in data:
  if select.select([linux.stdout],[],[],.1)[0]:
   chunk=os.read(linux.stdout.fileno(),65536)
   if not chunk:break
   data.extend(chunk)
 (out/'linux.log').write_bytes(data);assert b'CREDENTIAL_PARITY_DONE' in data,data.decode(errors='replace')[-4000:]
 linux.terminate();linux.wait(10)
 runtime.WORK=out
 vm=runtime.VM(2,storage=False,network=False,memory='16G',accel='kvm',iso=a.iso,label='credential-parity')
 encoded=base64.b64encode(gzip.compress(binary,mtime=0)).decode()
 for i in range(0,len(encoded),400):vm.command(('write' if not i else 'append')+' /tmp/credentials.b64 '+encoded[i:i+400])
 vm.shell_command('/bin/busybox base64 -d /tmp/credentials.b64 | /bin/busybox gzip -d > /tmp/credential-parity; chmod 755 /tmp/credential-parity')
 result=vm.command('/tmp/credential-parity',timeout=45);(out/'coast.log').write_text(result)
 assert 'CREDENTIAL_PARITY_DONE' in result,result
 def traces(text):return [line[line.index('CRED '):].strip() for line in text.splitlines() if 'CRED ' in line]
 expected=traces(data.decode(errors='replace'));actual=traces(result)
 report.update(linux=expected,coast=actual)
 assert expected==actual,'Credential traces differ; see report.json and guest logs'
 report['state']='passed';print('PASS identical Linux / Coast credential traces: 15 fork-isolated cases, including FUTEX_WAKE_OP, descriptor ioctls and null/zero seeks',flush=True)
except BaseException as e:
 report['error']=repr(e);(out/'exception.txt').write_text(traceback.format_exc());raise
finally:
 if vm:vm.close()
 if linux and linux.poll() is None:linux.terminate();linux.wait(10)
 report['owned_guests_stopped']=bool(linux and linux.poll() is not None and (not vm or vm.process.poll() is not None))
 (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
