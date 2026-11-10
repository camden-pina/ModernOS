#!/usr/bin/env python3
"""Verify bounded mmap hint placement and preserved runtime/security regressions."""
from pathlib import Path
import argparse,sys,json,time,traceback
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--iso',type=Path,required=True);p.add_argument('--output-root',type=Path,required=True);p.add_argument('--label',required=True);p.add_argument('--cpus',type=int,choices=[1,4],default=4);p.add_argument('--baseline',action='store_true');args=p.parse_args()
R=args.root.resolve();sys.path.insert(0,str(R/'tools'));import test_runtime as rt
label=args.label;cpus=args.cpus;baseline=args.baseline;iso=args.iso.resolve();out=args.output_root.resolve()/label;out.mkdir(parents=True,exist_ok=False);rt.WORK=out;v=None;r={'state':'failed','cpus':cpus,'network':False,'extra_disks':False,'checks':{}}
def check(name,command,marker,timeout=60):
 t=time.monotonic();s=v.command(command,timeout=timeout);r['checks'][name]={'output':s,'seconds':time.monotonic()-t};assert marker in s,(name,s[-1500:]);print('PASS '+name,flush=True);return s
try:
 v=rt.VM(cpus,storage=False,network=False,memory='8G',iso=iso,label=label)
 check('mmap-hints','mmap-hint-check'+(' --baseline' if baseline else ''),'MMAP_HINT_BASELINE_PASS' if baseline else 'MMAP_HINT_PASS')
 if not baseline:
  check('EOF-guards','mmap-file-guard-check','MMAP_GUARD_PASS')
  check('epoll-capacity','epoll-capacity-check','EPOLL_CAPACITY_PASS')
  s=check('node-version','runtime-launch-check /opt/codeoss-runtime/node --version','\nv24.18.0\n');assert 'RUNTIME_WAIT status=0 exit=0 signal=0' in s
  check('unchanged-codium-image-budget','runtime-launch-check /opt/codeoss-runtime/codium --version','RUNTIME_EXECVE_ERROR errno=27')
  r['default_node']=v.command("runtime-launch-check /opt/codeoss-runtime/node -e 'console.log(1+2)'",timeout=35);r['default_mode']='working' if '\n3\n' in r['default_node'] else 'blocked'
  check('node-jitless','runtime-launch-check /opt/codeoss-runtime/node --jitless -e \'console.log(1+2)\'','\n3\n')
  check('existing-sandbox-quota','browser-sandbox-check','BROWSER SANDBOX CHECK PASS',120)
  check('native-userland','check','USERLAND CHECK PASS',90)
  check('native-posix','posix-check','POSIX CHECK PASS',90)
  check('job-control','job-control-check','JOB_CONTROL_PRIMITIVES_PASS',60)
  check('shared-musl','dynamic-driver-check','DYNAMIC DRIVER CHECK PASS',180)
  check('shared-cpp','/usr/lib/coast/dynamic-cpp/cpp-check','DYNAMIC CPP CHECK PASS',120)
  check('historical-text-core','codeoss-core --script /usr/share/codeoss/core-check-native.js','CODEOSS_CORE_CHECK_PASS',90)
 r['state']='passed baseline observation' if baseline else 'passed bounded hint fix; default Node remains separately observed'
except Exception as e:r['error']=str(e);(out/'exception.txt').write_text(traceback.format_exc());raise
finally:
 if v:v.close()
 (out/'report.json').write_text(json.dumps(r,indent=2)+'\n');print(r['state'],flush=True)
