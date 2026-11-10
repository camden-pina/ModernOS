#!/usr/bin/env python3
"""Build the real local bc/Ninja PKGBUILDs in an owned offline guest."""
import argparse
import hashlib
import json
from pathlib import Path
import traceback
import test_runtime as runtime


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso',type=Path,required=True)
    parser.add_argument('--output-root',type=Path,required=True)
    args=parser.parse_args()
    out=args.output_root.resolve()
    out.mkdir(parents=True,exist_ok=False)
    runtime.WORK=out
    vm=None
    report=dict(state='failed',checks=[],iso=str(args.iso.resolve()),target_host_executed=False,
                harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    try:
        vm=runtime.VM(4,storage=False,network=True,memory='16G',accel='kvm',iso=args.iso,label='coast-sources')
        def check(name,command,expected=None,user=True,timeout=90):
            output=vm.shell_command(command,expect=expected,as_user=user,timeout=timeout)
            (out/(f'{len(report["checks"]):02d}-output.txt')).write_text(output)
            report['checks'].append(name)
            print('PASS '+name,flush=True)
            return output
        check('local recipe update versions use vercmp','brine checkupdates','bc 1.08.2-1 -> 1.08.2-3')
        check('clone GNU bc PKGBUILD Git repository','brine clone bc','Recipe checkout')
        check('build verified GNU bc source with native GCC','cd /home/user/Builds/bc && makepkg --noconfirm','Finished making',timeout=360)
        # Native builds can outlast the disposable QA sudo timestamp. Renew
        # it through the real password-based harness before user installation.
        vm.command('true')
        check('install reviewed bc source build through sudo','brine install --source bc --reviewed --noconfirm','upgrading bc',timeout=180)
        check('upstream tracks new package release','pacman -Q bc','bc 1.08.2-3')
        check('source-built bc computes exact result',"printf '2^100\\n' | /usr/bin/bc -q",'1267650600228229401496703205376')
        check('explicit repository install follows downgrade rules','pacman -S --noconfirm bc','downgrading bc',user=False)
        check('clone Ninja PKGBUILD Git repository','brine clone ninja','Recipe checkout')
        check('bootstrap actual Ninja with native Python and G++','cd /home/user/Builds/ninja && makepkg --noconfirm','Finished making',timeout=480)
        vm.command('true')
        check('install reviewed Ninja source build','brine install --source ninja --reviewed --noconfirm','upgrading ninja',timeout=180)
        check('source-built Ninja executes normally','/usr/bin/ninja --version','1.13.2')
        check('remove Qt including owned binary','pacman -R --noconfirm qt6-demo && test ! -e /usr/bin/qt6-demo','removing qt6-demo',user=False)
        check('restore Qt from signed repository','pacman -S --noconfirm qt6-demo','installing qt6-demo',user=False,timeout=180)
        check('Qt binary/source/license integrity after reinstall','pacman -Qkk qt6-demo','0 altered files')
        check('remove a standalone language package and aliases','pacman -R --noconfirm lua && test ! -e /usr/bin/lua && test ! -e /usr/bin/linux-lua','removing lua',user=False)
        check('restore standalone language and symlinks','pacman -S --noconfirm lua','installing lua',user=False)
        check('restored language executes real code',"lua -e 'print(6*7)'",'42')
        report['state']='passed'
    except BaseException as error:
        report['error']=repr(error)
        (out/'exception.txt').write_text(traceback.format_exc())
        raise
    finally:
        if vm:vm.close()
        report['owned_vm_stopped']=bool(vm and vm.process.poll() is not None)
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':
    main()
