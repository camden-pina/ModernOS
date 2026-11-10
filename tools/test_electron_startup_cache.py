#!/usr/bin/env python3
"""Boot the test-only Electron identity-cache verifier and check native I/O."""
import argparse,fcntl,hashlib,json,traceback
from pathlib import Path
import test_runtime as runtime


def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--iso',type=Path,required=True);p.add_argument('--output-root',type=Path,required=True);a=p.parse_args()
 iso=a.iso.resolve();out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=False);runtime.WORK=out
 (out/'HARNESS.py').write_bytes(Path(__file__).read_bytes())
 result=dict(state='failed',iso=str(iso),network=False,extra_disks=False,checks={});vm=None
 with Path(str(iso)+'.lock').open('a') as lock:
  fcntl.flock(lock,fcntl.LOCK_SH|fcntl.LOCK_NB)
  with iso.open('rb') as stream:result['iso_sha256']=hashlib.file_digest(stream,'sha256').hexdigest()
  try:
   vm=runtime.VM(2,storage=False,network=False,memory='16G',accel='kvm',iso=iso,label='startup-cache')
   vm.log.flush();boot=Path(vm.log.name).read_text(errors='replace')
   marker='ELECTRON_PROFILE_CACHE_SELFTEST_PASS immutable/hit/canonical-path/same-size-write/reverify/replacement'
   assert marker in boot,'The input must be built with --selftest and pass the actual loader verification checks'
   result['checks']['boot_identity_cache']=marker
   print('PASS immutable cache hit, canonical path, same-size mutation, rehash and replacement',flush=True)
   for command,marker in [('ipc-check','IPC CHECK PASS'),('posix-check','POSIX CHECK PASS'),('check','USERLAND CHECK PASS')]:
    output=vm.command(command,timeout=90);assert marker in output,output[-2000:]
    result['checks'][command]=output;print('PASS '+command,flush=True)
   result['state']='passed real loader cache invalidation and native descriptor/I/O regressions'
  except BaseException as error:
   result['error']=repr(error);(out/'exception.txt').write_text(traceback.format_exc());raise
  finally:
   if vm:vm.close();result['owned_vm_stopped']=vm.process.poll() is not None
   with iso.open('rb') as stream:result['image_unchanged']=hashlib.file_digest(stream,'sha256').hexdigest()==result['iso_sha256']
   (out/'report.json').write_text(json.dumps(result,indent=2)+'\n')
if __name__=='__main__':main()
