#!/usr/bin/env python3
"""Exercise Linux-comparable package security in an owned, diskless Coast VM."""
import argparse
import hashlib
import json
from pathlib import Path
import traceback
import test_runtime as runtime

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--iso',type=Path,required=True)
p.add_argument('--cpus',type=int,choices=[1,4],required=True)
p.add_argument('--output-root',type=Path,required=True)
a=p.parse_args()
out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=False);runtime.WORK=out
vm=None
report=dict(state='failed',checks=[],cpus=a.cpus,iso=str(a.iso.resolve()),
            guest_program_execution_on_host=False,extra_disks=False,network=False)
try:
    vm=runtime.VM(a.cpus,storage=False,network=False,memory='16G',accel='kvm',
                  iso=a.iso.resolve(),label='package-security')
    for name,cases in [('seccomp',14),('landlock',12)]:
        output=vm.shell_command('/usr/bin/package-'+name+'-check',as_user=True,timeout=150)
        (out/(name+'-output.txt')).write_text(output)
        assert 'PACKAGE_'+name.upper()+'_PASS cases='+str(cases) in output,output
        report['checks'].extend(line for line in output.splitlines() if line.startswith('PASS '))
    output=vm.shell_command('/usr/bin/package-alpm-sandbox-check',timeout=120)
    (out/'alpm-output.txt').write_text(output)
    assert 'PACKAGE_ALPM_SANDBOX_PASS real_uid=990 groups=0 nnp=1 seccomp=2 landlock=3' in output,output
    report['checks'].append('upstream libalpm enforces real UID 990, cleared groups, NNP, seccomp and Landlock')
    vm.command('browser-sandbox-check',expect='BROWSER SANDBOX CHECK PASS',timeout=120)
    report['checks'].append('existing browser sandbox still rejects escapes')
    vm.command('check',expect='USERLAND CHECK PASS',timeout=90)
    report['checks'].append('ordinary guest remains healthy after restricted children exit')
    report['state']='passed'
except BaseException as error:
    report['error']=repr(error);(out/'exception.txt').write_text(traceback.format_exc());raise
finally:
    if vm:vm.close()
    report['owned_vm_stopped']=bool(vm and vm.process.poll() is not None)
    with a.iso.open('rb') as f:report['iso_sha256']=hashlib.file_digest(f,'sha256').hexdigest()
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
