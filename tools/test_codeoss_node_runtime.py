#!/usr/bin/env python3
"""Verify the bounded genuine Node runtime milestone in an owned offline guest.
Does not build an image, attach storage, alter credentials, or claim Electron support.
"""
from pathlib import Path
import argparse,sys,json,time,traceback,hashlib
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1])
p.add_argument('--iso',type=Path,required=True)
p.add_argument('--output-root',type=Path,required=True)
p.add_argument('--label',required=True)
p.add_argument('--cpus',type=int,choices=[1,4],default=4)
p.add_argument('--smoke-only',action='store_true')
args=p.parse_args();R=args.root.resolve();sys.path.insert(0,str(R/'tools'));import test_runtime as rt
out=args.output_root.resolve()/args.label;out.mkdir(parents=True,exist_ok=False);rt.WORK=out
label=args.label;cpus=args.cpus;iso=args.iso.resolve()
with iso.open('rb') as f:iso_hash=hashlib.file_digest(f,'sha256').hexdigest()
report={'state':'failed','cpus':cpus,'accel':'tcg','network':False,'extra_disks':False,'iso_sha256':iso_hash,'checks':{}};v=None
def check(name,command,required,timeout=60):
 start=time.monotonic();s=v.command(command,timeout=timeout);report['checks'][name]={'output':s,'seconds':time.monotonic()-start};assert required in s,(name,s[-2000:]);print('PASS '+name,flush=True);return s
try:
 v=rt.VM(cpus,storage=False,network=False,memory='8G',iso=iso,label=label)
 check('mapping-guards','mmap-file-guard-check','MMAP_GUARD_PASS checks=4157')
 check('epoll-output-capacity','epoll-capacity-check','EPOLL_CAPACITY_PASS')
 check('glibc-loader','runtime-launch-check /lib64/ld-linux-x86-64.so.2 --version','RUNTIME_WAIT status=0 exit=0 signal=0')
 for i in range(3):
  s=check('node-version-'+str(i),'runtime-launch-check /opt/codeoss-runtime/node --version','\nv24.18.0\n');assert 'RUNTIME_WAIT status=0 exit=0 signal=0' in s
 check('codium-unchanged-elf-budget','runtime-launch-check /opt/codeoss-runtime/codium --version','RUNTIME_EXECVE_ERROR errno=27')
 s=v.command("runtime-launch-check /opt/codeoss-runtime/node -e 'console.log(1+2)'",timeout=35);report['default_javascript']={'output':s,'state':'blocked' if '\n3\n' not in s else 'executed'};print('OBSERVE default JavaScript '+report['default_javascript']['state'],flush=True)
 s=v.command("runtime-launch-check /opt/codeoss-runtime/node --jitless -e 'console.log(1+2)'",timeout=35);report['jitless_diagnostic']={'output':s,'state':'executed' if '\n3\n' in s and 'RUNTIME_WAIT status=0 exit=0 signal=0' in s else 'blocked','changes_guest_security':False};print('OBSERVE jitless diagnostic '+report['jitless_diagnostic']['state'],flush=True)
 if report['jitless_diagnostic']['state']=='executed':
  cases=[
   ('node-jitless-file-roundtrip','let f=require("fs");f.writeFileSync("/tmp/a1-node.txt","node bytes",{flag:"wx"});console.log(f.readFileSync("/tmp/a1-node.txt","utf8"));f.renameSync("/tmp/a1-node.txt","/tmp/a1-node-renamed.txt");f.unlinkSync("/tmp/a1-node-renamed.txt");console.log("NODE_FILE_OK")','NODE_FILE_OK'),
   ('node-jitless-timer','setTimeout(()=>console.log("NODE_TIMER_OK"),10)','NODE_TIMER_OK'),
   ('node-jitless-async-file','require("fs").readFile("/usr/share/licenses/codeoss-runtime/node-LICENSE","utf8",(e,s)=>{if(e)throw e;console.log("NODE_ASYNC_READ_OK")})','NODE_ASYNC_READ_OK')]
  for name,script,marker in cases:
   text=v.command("runtime-launch-check /opt/codeoss-runtime/node --jitless -e '"+script+"'",timeout=35)
   success='\n'+marker+'\n' in text and 'RUNTIME_WAIT status=0 exit=0 signal=0' in text
   report['checks'][name]={'output':text,'state':'passed' if success else 'blocked'}
   print(('PASS ' if success else 'BLOCKED ')+name,flush=True)
 if not args.smoke_only:
  check('native-userland','check','USERLAND CHECK PASS',90)
  check('native-posix','posix-check','POSIX CHECK PASS',90)
  check('job-control-primitives','job-control-check','JOB_CONTROL_PRIMITIVES_PASS',60)
  check('musl-dynamic-driver','dynamic-driver-check','DYNAMIC DRIVER CHECK PASS',180)
  check('shared-cpp-abi','/usr/lib/coast/dynamic-cpp/cpp-check','DYNAMIC CPP CHECK PASS',120)
  check('native-shared-text-core','codeoss-core --script /usr/share/codeoss/core-check-native.js','CODEOSS_CORE_CHECK_PASS',90)
 report['state']='passed supported milestone; full application blocked'
except Exception as e:
 report['error']=str(e);(out/'exception.txt').write_text(traceback.format_exc());raise
finally:
 if v:v.close()
 (out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(report['state'],flush=True)
