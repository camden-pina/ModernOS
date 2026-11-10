#!/usr/bin/env python3
"""Run the unprivileged inotify fixture through the real sandboxed editor."""
import argparse, base64, json, os, time, traceback
from pathlib import Path
import test_runtime as runtime
import test_desktop as desktop
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--iso',type=Path,required=True)
p.add_argument('--output-root',type=Path,required=True)
a=p.parse_args(); out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=False)
runtime.WORK=desktop.WORK=out
vm=None; report=dict(state='failed',iso=str(a.iso.resolve()))
observer="""import {app} from 'electron';
import {execFile} from 'node:child_process';
app.whenReady().then(()=>execFile('/bin/inotify-check', [], {timeout:90000}, (error,stdout,stderr)=>{
 console.log('NOTIFICATION_FIXTURE',JSON.stringify({error:error?.message,stdout,stderr}));
}));
"""
try:
 vm=runtime.VM(2,storage=False,network=False,memory='16G',accel='kvm',iso=a.iso,label='notifications')
 vm.command('mkdir -p /tmp/inotify-root-private')
 vm.command('chmod 700 /tmp/inotify-root-private')
 output=vm.command('vector-io-check');assert 'VECTOR IO PASS' in output,output
 report['vector_io']=output
 encoded=base64.b64encode(observer.encode()).decode()
 for i in range(0,len(encoded),192):
  vm.shell_command("printf '%s' '"+encoded[i:i+192]+"' "+('>' if i==0 else '>>')+'/tmp/notification-observer.b64')
 vm.shell_command('/bin/busybox base64 -d /tmp/notification-observer.b64 > /usr/share/codeoss-workbench/out/coast-diagnostics.mjs')
 vm.send("build-sh -c 'COAST_CODEOSS_QA=1 exec /bin/gui --trace'\n");vm.until(rb'GUI_READY:',60)
 d=desktop.DesktopTest(vm);d.screenshot('00-desktop');d.launch('Code OSS')
 deadline=time.monotonic()+100; source=''
 while time.monotonic()<deadline:
  while vm.selector.select(.01):
   b=os.read(vm.process.stdout.fileno(),65536)
   if not b:break
   vm.pending.extend(b);vm.log.write(b);vm.log.flush()
  source=Path(vm.log.name).read_text(errors='replace')
  if 'NOTIFICATION_FIXTURE' in source:break
  time.sleep(.02)
 assert 'INOTIFY_PASS' in source,source[-4000:]
 report['state']='passed real unprivileged notification fixture and positional-vector I/O'
 print('PASS real UID 1000 notification fixture: masks, replacement, mask-add, access/open/close, rename, deletion, one-shot, overflow, limits and cleanup',flush=True)
 d.key('f4','alt');time.sleep(.5);d.exit()
 assert 'USERLAND CHECK PASS' in vm.command('check',timeout=90)
except BaseException as error:
 report['error']=repr(error);(out/'exception.txt').write_text(traceback.format_exc());raise
finally:
 if vm:vm.close()
 (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
