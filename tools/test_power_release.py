#!/usr/bin/env python3
"""Power checks against the supplied release ISO, with NO image modifications.
Validates embedded kernel/initramfs and expected shell/GUI bytes before boot.
Only disposable FAT media are writable. Caller must admit one exclusive guest;
never overlap a memory-constrained compiler. --validate-only never boots.
"""
import argparse,hashlib,json,os,pathlib,re,select,subprocess,tarfile,time
from test_power import PowerVM,digest,drain,file_input,member_name,plain
from test_runtime import run
from test_desktop import DesktopTest
ROOT=pathlib.Path(__file__).resolve().parents[1]
WORK=ROOT/'build/power-release-tests'

def verify_image(args,folder):
    inputs={k:file_input(getattr(args,k),k,elf=k!='iso') for k in ('iso','kernel','shell','gui')}
    for entry in ('kernel.elf','initramfs.tar','coast.conf'):
        run('xorriso','-osirrox','on','-indev',args.iso,'-extract','/boot/'+entry,folder/entry)
    # Supplied ISO remains untouched; the extraction is provenance inspection.
    configs=[folder/'coast.conf']
    try:
        run('xorriso','-osirrox','on','-indev',args.iso,'-extract','/boot/limine/limine.conf',folder/'limine.conf')
        configs.append(folder/'limine.conf')
    except subprocess.CalledProcessError:
        pass # Native EFI images may have no Limine configuration.
    config='\n'.join(path.read_text() for path in configs)
    assert not re.search(r'(?:^|[\s=])(?:power\.test=|acpi\.selftest(?:\s|$))',config,re.M),'release ISO carries a test boot token'
    assert digest(folder/'kernel.elf')==inputs['kernel']['sha256'],'embedded kernel does not match --kernel'
    expected={'bin/coast-sh':inputs['shell']['sha256'],'bin/gui':inputs['gui']['sha256']}
    seen={};found={}
    with tarfile.open(folder/'initramfs.tar','r:') as archive:
        for item in archive:
            name=member_name(item.name)
            metadata=(item.type,item.mode,item.uid,item.gid)
            if name in seen:
                assert item.isdir() and seen[name]==metadata,('duplicate release initramfs member',name)
                continue # Identical parent directories in historic full images.
            seen[name]=metadata
            if name in expected:
                assert item.isfile(),('expected regular release executable',name)
                with archive.extractfile(item) as data:found[name]=hashlib.file_digest(data,'sha256').hexdigest()
    assert found==expected,('supplied initramfs shell/GUI mismatch',found,expected)
    inputs['embedded']={p.name:dict(bytes=p.stat().st_size,sha256=digest(p)) for p in (folder/'kernel.elf',folder/'initramfs.tar',folder/'coast.conf')}
    inputs['boot_configurations']={path.name:digest(path) for path in configs}
    inputs['image_modified']=False
    return inputs

def terminal_event(vm,reason,timeout):
    deadline=time.monotonic()+timeout;events=[];found=False
    while time.monotonic()<deadline:
        drain(vm)
        if select.select([vm.qmp_output],[],[],.02)[0]:vm.qmp_pending.extend(os.read(vm.qmp_output,65536))
        while b'\n' in vm.qmp_pending:
            line,_,rest=vm.qmp_pending.partition(b'\n');vm.qmp_pending=bytearray(rest)
            if not line.strip():continue
            event=json.loads(line);events.append(event)
            data=event.get('data',{})
            if event.get('event')=='SHUTDOWN' and data.get('reason')==reason and data.get('guest') is True:found=True
        if found and vm.process.poll() is not None:break
    text=plain(vm)
    assert found,(reason,events,text)
    assert all(x in text for x in ('power: COMMIT','flush complete','reversible park acknowledged')),text
    assert 'Power request accepted' in text or 'DESKTOP POWER_REQUEST' in text,text
    assert not any(x in text for x in ('PANIC:','terminal failure','failed stage=')),text
    return events

def gui_request(vm,prefix):
    vm.send('gui --trace --stats\n');vm.until(rb'GUI_READY: Coast desktop',60)
    d=DesktopTest(vm);d.screenshot(prefix+'-ready')
    editor=d.launch('Text Editor');vm.send('DISPOSABLE_RELEASE_DIRTY_DOCUMENT');time.sleep(.3)
    d.launch('System & Power')
    # Capture the real diagnostics and controls while this window is foreground,
    # before any confirmation modal or pending power request covers it.
    d.screenshot(prefix+'-system-power')
    d.key('tab');d.key('ret');d.expect('POWER_REVIEW','shutdown')
    d.key('ret');d.expect('POWER_WAIT','Text Editor');d.screenshot(prefix+'-dirty-confirm');d.key('esc')
    assert 'DESKTOP POWER_REQUEST' not in plain(vm),'canceled editor review submitted a request'
    d.key('tab','alt');d.key('ret');d.expect('POWER_REVIEW','shutdown')
    d.key('ret');d.expect('POWER_WAIT','Text Editor');d.key('ret')
    vm.until(rb'DESKTOP POWER_CONSENT[^\n]*Text Editor[\s\S]*DESKTOP POWER_REQUEST[^\n]*accepted/coalesced[^\n]*\n',20)
    assert not re.search(r'DESKTOP CLOSE window='+str(editor)+r'\b',plain(vm))
    return editor

def scenario(args,folder,report):
    disk=folder/'release.volume'
    with disk.open('wb') as stream:stream.truncate(64*1024*1024)
    run('mkfs.fat','-F','32',disk)
    extra=['-drive','file='+str(disk).replace(',',',,')+',format=raw,if=none,id=pwr']
    if args.transport=='ahci':extra+=['-device','ide-hd,drive=pwr,bus=ide.0'];mount='/mnt/sata0'
    else:extra+=['-device','nvme,drive=pwr,serial=RELEASE-POWER'];mount='/mnt/nvme0n1'
    payload='COAST_RELEASE_POWER_'+folder.name;vm=None
    report['qemu_extra_args']=extra
    def boot(suffix=''):
        return PowerVM(args.cpus,storage=False,network=False,memory=args.memory,iso=args.iso,
                       label='release-power-'+folder.name+suffix,extra_args=extra)
    try:
        vm=boot();report['serial_log']=str(vm.log.name)
        vm.command('check','USERLAND CHECK PASS',60)
        status=vm.command('power','power_state=idle')
        assert 'poweroff_ready=1' in status and 'reset_ready=1' in status,status
        vm.command('write '+mount+'/release-power.txt '+payload)
        vm.command('cat '+mount+'/release-power.txt',payload)
        vm.qmp('query-status') # Complete QMP negotiation before the OS request.
        if args.kind=='gui-poweroff':
            prefix='release-power-'+folder.name
            report['editor_window']=gui_request(vm,prefix)
            report['screenshots']=[str(ROOT/'build/runtime-tests'/(prefix+suffix+'.ppm'))
                                   for suffix in ('-ready','-system-power','-dirty-confirm')]
        else:vm.command('reboot' if args.kind=='reboot' else 'poweroff','Power request accepted')
        if args.kind=='cancel':
            vm.command('power cancel','Cancellation accepted');time.sleep(1.2)
            status=vm.command('power','power_state=idle')
            assert 'firmware_prep_attempted=0' in status and 'power: COMMIT' not in plain(vm),status
            vm.command('cat '+mount+'/release-power.txt',payload)
            vm.command('check','USERLAND CHECK PASS',60);vm.command('thread-check','THREAD CHECK PASS',120)
            vm.command('ipc-check','IPC CHECK PASS',90)
            vm.command('sync');report['teardown_sync']=True;report['cancel_snapshot']=status
        else:report['qmp_events']=terminal_event(vm,'guest-reset' if args.kind=='reboot' else 'guest-shutdown',args.timeout)
        vm.close();vm=None
        run('fsck.fat','-n',disk)
        actual=run('mtype','-i',disk,'::release-power.txt')
        assert actual==(payload+'\n').encode(),actual
        report['persisted_sha256']=hashlib.sha256(actual).hexdigest()
        if args.kind!='cancel':
            vm=boot('-reopen');report['reopen_serial_log']=str(vm.log.name)
            vm.command('cat '+mount+'/release-power.txt',payload);vm.command('check','USERLAND CHECK PASS',60)
        report['result']='passed'
    finally:
        if vm:vm.close()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('iso','kernel','shell','gui'):p.add_argument('--'+key,type=pathlib.Path,required=True)
    p.add_argument('--kind',choices=('poweroff','reboot','cancel','gui-poweroff'),required=True)
    p.add_argument('--cpus',type=int,default=4);p.add_argument('--transport',choices=('ahci','nvme'),default='nvme')
    p.add_argument('--memory',default='2G');p.add_argument('--timeout',type=int,default=60)
    p.add_argument('--validate-only',action='store_true');args=p.parse_args()
    assert args.cpus>0 and args.timeout>0 and re.fullmatch(r'[1-9][0-9]*(?:[KMGTP](?:B)?)?',args.memory,re.I)
    for key in ('iso','kernel','shell','gui'):setattr(args,key,getattr(args,key).resolve(strict=True))
    folder=WORK/(args.kind+'-'+str(args.cpus)+'-'+args.transport+'-'+str(time.time_ns()));folder.mkdir(parents=True)
    report=dict(result='failed',kind=args.kind,cpus=args.cpus,transport=args.transport,memory=args.memory,
                mode='supplied release ISO unchanged; no injected binaries, AML or boot tokens')
    try:
        report['inputs']=verify_image(args,folder)
        if args.validate_only:report['result']='validated-no-guest'
        else:scenario(args,folder,report)
    except BaseException as error:
        report['error']=str(error);raise
    finally:
        if 'inputs' in report:
            report['read_only_inputs_unchanged']=all(digest(pathlib.Path(report['inputs'][key]['path']))==report['inputs'][key]['sha256'] for key in ('iso','kernel','shell','gui'))
        (folder/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    assert report['read_only_inputs_unchanged']
    print('RELEASE POWER '+report['result'].upper(),args.kind,args.cpus,args.transport,'report='+str(folder/'report.json'),flush=True)
if __name__=='__main__':main()
