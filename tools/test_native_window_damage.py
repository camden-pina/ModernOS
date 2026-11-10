#!/usr/bin/env python3
"""Exercise copied native frames and incremental transfers in a diskless guest."""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import traceback
import test_runtime as runtime


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso',type=Path,required=True)
    parser.add_argument('--output-root',type=Path,required=True)
    args=parser.parse_args();iso=args.iso.resolve();out=args.output_root.resolve()
    out.mkdir(parents=True,exist_ok=False);runtime.WORK=out
    (out/'HARNESS.py').write_bytes(Path(__file__).read_bytes())
    result=dict(state='failed',iso=str(iso),network=False,extra_disks=False,checks={})
    vm=None
    with Path(str(iso)+'.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_SH|fcntl.LOCK_NB)
        with iso.open('rb') as stream:result['iso_sha256']=hashlib.file_digest(stream,'sha256').hexdigest()
        try:
            vm=runtime.VM(2,storage=False,network=False,memory='16G',accel='kvm',iso=iso,label='window-damage')
            for command,marker in [
                ('window-check','WINDOW_SEMANTICS_PASS'),
                ('pixel-window-check','NATIVE_PIXEL_WINDOW_PASS'),
                ('pixel-window-check --geometry','NATIVE_WINDOW_GEOMETRY_PASS'),
                ('pixel-window-check --damage','NATIVE_WINDOW_DAMAGE_PASS'),
                ('check','USERLAND CHECK PASS')]:
                output=vm.command(command,timeout=90);result['checks'][command]=output
                assert marker in output,(command,output[-2500:])
                print('PASS '+command,flush=True)
            result['state']='passed native copied-frame, geometry, cursor, damage and guest health checks'
        except BaseException as error:
            result['error']=repr(error);(out/'exception.txt').write_text(traceback.format_exc());raise
        finally:
            if vm:
                vm.close();result['owned_vm_stopped']=vm.process.poll() is not None
            with iso.open('rb') as stream:result['image_unchanged']=hashlib.file_digest(stream,'sha256').hexdigest()==result['iso_sha256']
            (out/'report.json').write_text(json.dumps(result,indent=2)+'\n')


if __name__=='__main__':main()
