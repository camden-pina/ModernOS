#!/usr/bin/env python3
"""Optional disposable AML Wait-forever preparation liveness test.
An observed bounded interval without commit is evidence of the known synchronous
preparation limit, never cancellation/recovery. Host kill is teardown only.
The caller must reserve a guest slot; never use alongside another guest.
"""
import argparse,json,pathlib,select,struct,time
from test_power import PowerVM,digest,drain,plain
ROOT=pathlib.Path(__file__).resolve().parents[1]
WORK=ROOT/'build/power-tests/blocked-prep'
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--iso',type=pathlib.Path,required=True)
    p.add_argument('--aml',type=pathlib.Path,required=True);p.add_argument('--memory',default='384M')
    p.add_argument('--cpus',type=int,default=4);a=p.parse_args();WORK.mkdir(parents=True,exist_ok=True)
    aml=a.aml.read_bytes();assert aml[:4]==b'SSDT' and len(aml)==struct.unpack_from('<I',aml,4)[0] and not sum(aml)%256
    before={str(x.resolve()):digest(x) for x in (a.iso,a.aml)}
    report=dict(result='failed',inputs=before,memory=a.memory,cpus=a.cpus,observation_seconds=7,
                limit='Synchronous firmware preparation is unbounded; no software cancellation/recovery claim')
    vm=None
    try:
        vm=PowerVM(a.cpus,storage=False,network=False,memory=a.memory,iso=a.iso,
                   label='power-blocked-prep',extra_args=['-acpitable','file='+str(a.aml.resolve()).replace(',',',,')])
        boot=plain(vm)
        assert 'ACPI Error:' not in boot and 'AE_ALREADY_EXISTS' not in boot, 'fixture rejected; cannot claim blocked prep\n'+boot
        vm.command('check','USERLAND CHECK PASS');vm.command('poweroff','Power request accepted')
        vm.until(rb'power: preparation BSP0 IF=1[^\n]*\n',15)
        deadline=time.monotonic()+7
        while time.monotonic()<deadline:
            drain(vm)
            assert vm.process.poll() is None,'guest exited during blocked preparation'
            assert vm.qmp('query-status')['running'],'guest stopped during blocked preparation'
            current=plain(vm)
            assert not any(x in current for x in ('power: COMMIT','reversible park acknowledged','power: request 1 failed','PANIC:','ACPI Error:')),current
            time.sleep(.1)
        report.update(result='passed',serial_log=str(vm.log.name),
                      observed='Preparation entered; guest remains running without park/flush/commit or claimed rollback beyond drain deadline')
    finally:
        if vm:vm.close()
        report['inputs_unchanged']=all(digest(pathlib.Path(path))==sha for path,sha in before.items())
        (WORK/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    assert report['inputs_unchanged']
    print('POWER BLOCKED PREP PASS: synchronous AML wait remains pending; teardown is not recovery',flush=True)
if __name__=='__main__':main()
