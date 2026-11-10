#!/usr/bin/env python3
"""Search, review, build and install a real SSH-submitted community recipe in Coast."""
import argparse
import hashlib
import json
from pathlib import Path
import traceback
import test_runtime as runtime

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--iso',type=Path,required=True)
p.add_argument('--package',required=True)
p.add_argument('--search-term',default='community-check')
p.add_argument('--executable',default='hello-community')
p.add_argument('--expected-output',default='COAST_COMMUNITY_HELLO uid=1000')
p.add_argument('--output-root',type=Path,required=True)
a=p.parse_args();assert __import__('re').fullmatch('[a-z0-9][a-z0-9.+_-]*',a.package)
out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=False);runtime.WORK=out
report=dict(state='failed',checks=[],iso=str(a.iso.resolve()),package=a.package,
            target_host_executed=False,extra_disks=False,physical_network_tested=False)
vm=None
def check(name,command,expected=None,user=True,timeout=120):
    text=vm.shell_command(command,as_user=user,expect=expected,timeout=timeout)
    (out/(f'{len(report["checks"]):02d}-output.txt')).write_text(text)
    report['checks'].append(name);print('PASS '+name,flush=True);return text
try:
    vm=runtime.VM(4,storage=False,network=True,memory='16G',accel='kvm',iso=a.iso.resolve(),label='community-client')
    check('owned VM obtains network configuration','for i in 1 2 3 4 5 6 7 8 9 10 11 12; do test -s /etc/resolv.conf && break; sleep 1; done; test -s /etc/resolv.conf')
    check('default community endpoint reaches the actual upstream v5 API','brine community --check','Community API available')
    check('partial names search the live community','brine search --community -q '+a.search_term,a.package)
    check('one-letter combined search retains the reported GCC fix',"brine search --json g > /tmp/community-search.json; python3 -I -c 'import json; p=json.load(open(\"/tmp/community-search.json\"))[\"results\"]; assert p[0][\"Name\"]==\"gcc\"; assert not any(q[\"Name\"]==\"qt6-demo\" for q in p); print(\"GCC_FIRST\")'",'GCC_FIRST')
    check('community metadata is read without executing the recipe','brine info --community '+a.package,a.package)
    check('review displays the committed PKGBUILD and source metadata','brine show --community '+a.package,'Community commit:')
    check('clone uses the real anonymous Git transport','brine clone --community '+a.package,'Recipe checkout:')
    check('noninteractive package confirmation cannot bypass source review','brine build --community '+a.package+' --noconfirm < /dev/null; test $? -ne 0','review this checkout first')
    check('reviewed community recipe builds and checks as the normal account','brine build --community '+a.package+' --reviewed --noconfirm','Finished making',timeout=240)
    vm.command('true')
    check('installation authenticates through real sudo and upstream makepkg/pacman','brine install --community '+a.package+' --reviewed --noconfirm','installing '+a.package,timeout=180)
    check('built package file metadata is intact before execution','pacman -Qkk '+a.package,'0 altered files')
    check('community executable runs with ordinary UID 1000',a.executable,a.expected_output)
    check('installed metadata belongs to the actual built community package','pacman -Qkk '+a.package,'0 altered files')
    check('foreign package version checks query the community','brine checkupdates')
    check('community updates use the same checked Git origin','brine update --community '+a.package,'Review the new Git diff')
    check('normal users cannot change the community trust settings directly','python3 -I /usr/lib/brine/client.py _set-community off; test $? -ne 0','requires authenticated sudo')
    vm.command('true')
    check('community package can be removed through authenticated pacman','brine remove '+a.package+' --noconfirm','removing '+a.package)
    vm.command('check',expect='USERLAND CHECK PASS',timeout=90);report['checks'].append('guest remains healthy after community transactions')
    report['state']='passed'
except BaseException as error:
    if vm:
        try:
            diagnostics=''
            for command in ['bsdtar -xOf *.pkg.tar.zst .MTREE | gzip -d',
                            'bsdtar -xOf *.pkg.tar.zst usr/bin/hello-community > /tmp/community-archive-file; sha256sum /tmp/community-archive-file /usr/bin/hello-community',
                            'sha256sum pkg/*/usr/bin/hello-community src/hello-community',
                            'readelf -l /usr/bin/hello-community']:
                diagnostics+=vm.shell_command('cd /home/user/Builds/'+a.package+'; '+command,as_user=True,timeout=90)
            (out/'package-diagnostics.txt').write_text(diagnostics)
        except BaseException as diagnostic_error:(out/'diagnostic-error.txt').write_text(repr(diagnostic_error))
    report['error']=repr(error);(out/'exception.txt').write_text(traceback.format_exc());raise
finally:
    if vm:vm.close()
    report['owned_vm_stopped']=bool(vm and vm.process.poll() is not None)
    with a.iso.open('rb') as stream:report['iso_sha256']=hashlib.file_digest(stream,'sha256').hexdigest()
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
