#!/usr/bin/env python3
"""Kernel account boundary checks in a disposable diskless/networkless guest."""
import argparse,base64,gzip,hashlib,json,subprocess,traceback
from pathlib import Path
import test_runtime as runtime
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--iso',type=Path,required=True);p.add_argument('--output-root',type=Path,required=True);p.add_argument('--gui-build',type=Path,required=True)
a=p.parse_args();out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=False)
receipt=json.loads((a.gui_build/'BUILD.json').read_text());conf=receipt['confinement'][:]
old=str(a.gui_build.resolve())
for i,v in enumerate(conf):
 if v==old:conf[i]=str(out)
cc=next(c for c in receipt['commands'] if str(root/'userland/profile-session.c') in c)[:]
cc[cc.index(str(root/'userland/profile-session.c'))]=str(root/'tools/account-check.c');cc[-1]=str(out/'account-check.o')
env=dict(HOME='/home/camdenp',PATH='/usr/bin:/bin',LANG='C.UTF-8',TMPDIR=str(out))
subprocess.run(conf+['--',*cc],check=True,env=env,stdin=subprocess.DEVNULL)
ld=['/usr/bin/ld','-nostdlib','-static','-z','max-page-size=4096','-z','noexecstack','-T',str(root/'userland/user.ld'),str(a.gui_build.resolve()/'crt0.S.o'),str(a.gui_build.resolve()/'libc.c.o'),str(out/'account-check.o'),'-o',str(out/'account-check')]
subprocess.run(conf+['--',*ld],check=True,env=env,stdin=subprocess.DEVNULL)
subprocess.run(['strip','--strip-all',str(out/'account-check')],check=True)
encoded=base64.b64encode(gzip.compress((out/'account-check').read_bytes(),mtime=0)).decode()
runtime.WORK=out;vm=None;report=dict(state='failed',iso=str(a.iso.resolve()),fixture_sha256=hashlib.sha256((out/'account-check').read_bytes()).hexdigest(),fixture_installed_in_iso=False)
try:
 vm=runtime.VM(2,storage=False,network=False,memory='16G',accel='kvm',iso=a.iso,label='accounts')
 for i in range(0,len(encoded),400):vm.command(('write' if not i else 'append')+' /tmp/accounts.b64 '+encoded[i:i+400])
 vm.shell_command('/bin/busybox base64 -d /tmp/accounts.b64 | /bin/busybox gzip -d > /tmp/account-check; chmod 755 /tmp/account-check')
 result=vm.command('/tmp/account-check',timeout=90);report['output']=result
 assert 'ACCOUNT_CHECK_PASS' in result,result
 assert 'uid=0' in vm.command('id'), 'Root supervisor identity changed'
 assert 'ELEVATED' in vm.command('cat /etc/account-sudo-command')
 report['state']='passed';print(result,flush=True)
 assert 'USERLAND CHECK PASS' in vm.command('check',timeout=90)
except BaseException as e:
 report['error']=repr(e);(out/'exception.txt').write_text(traceback.format_exc());raise
finally:
 if vm:vm.close()
 report['owned_vm_stopped']=bool(vm and vm.process.poll() is not None)
 (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
