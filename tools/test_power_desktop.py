#!/usr/bin/env python3
"""Real desktop dirty-document/cancel/recovery using disposable dry-run ISO.
No terminal action is possible: the input and effective boot config must carry
power.test=dry-run. Uses ordinary emulated USB input, real GUI and shell ABI.
The caller owns guest resource admission; never run in parallel with a guest.
"""
import argparse, hashlib, json, pathlib, shutil, tarfile, time
from test_runtime import run
from test_power import PowerVM, digest, plain
from test_desktop import DesktopTest
ROOT=pathlib.Path(__file__).resolve().parents[1]
WORK=ROOT/'build/power-tests/desktop'

def prepare(iso,gui):
    tree=WORK/'iso_root'
    if tree.exists(): shutil.rmtree(tree)
    run('xorriso','-osirrox','on','-indev',iso,'-extract','/',tree)
    config=tree/'boot/limine/limine.conf'
    assert config.is_file() and 'power.test=dry-run' in config.read_text(), 'requires disposable Limine dry-run ISO'
    ram=tree/'boot/initramfs.tar'; old=ram.with_suffix('.input.tar');ram.rename(old)
    with tarfile.open(old) as src,tarfile.open(ram,'w',format=tarfile.USTAR_FORMAT) as dst:
        for entry in src:
            if entry.name.lstrip('./')=='bin/gui': continue
            dst.addfile(entry,src.extractfile(entry) if entry.isfile() else None)
        entry=tarfile.TarInfo('bin/gui');entry.mode=0o755;entry.size=gui.stat().st_size
        with gui.open('rb') as data: dst.addfile(entry,data)
    old.unlink(); output=WORK/'desktop.iso'
    run('xorriso','-as','mkisofs','-R','-J','-V','COAST_POWER_UI',
        '-e','boot/limine/limine-uefi-cd.bin','-no-emul-boot','-efi-boot-part',
        '--efi-boot-image','--protective-msdos-label',tree,'-o',output)
    return output, {'input_iso':digest(iso),'gui':digest(gui),'effective_iso':digest(output),
                    'effective_kernel':digest(tree/'boot/kernel.elf'),'effective_initramfs':digest(ram)}

def exercise(iso,memory,cpus,report):
    vm=None
    try:
        vm=PowerVM(cpus,storage=False,network=False,memory=memory,iso=iso,label='power-desktop')
        vm.command('power','poweroff_ready=1');vm.command('check','USERLAND CHECK PASS')
        vm.send('gui --trace --stats\n');vm.until(rb'GUI_READY: Coast desktop',40)
        d=DesktopTest(vm);d.screenshot('power-desktop-ready')
        editor=d.launch('Text Editor');vm.send('POWER_DIRTY_CONTENT');time.sleep(.3)
        d.screenshot('power-desktop-dirty')
        d.launch('System & Power');d.key('tab');d.key('ret');d.expect('POWER_REVIEW','shutdown')
        d.key('ret');d.expect('POWER_WAIT','Text Editor')
        d.screenshot('power-desktop-dirty-confirm');d.key('esc')
        assert 'POWER_REQUEST' not in plain(vm), 'canceled application review submitted a request'
        # Editor retained focus/dirty state after cancel. Alt+Tab returns to the
        # still-open power view. Repeat the same review and approve the editor.
        d.key('tab','alt');d.key('ret');d.expect('POWER_REVIEW','shutdown')
        d.key('ret');d.expect('POWER_WAIT','Text Editor');d.key('ret')
        vm.until(rb'DESKTOP POWER_CONSENT[^\n]*Text Editor[\s\S]*DESKTOP POWER_REQUEST[^\n]*accepted/coalesced[^\n]*\n',20)
        # UI is live only in the pending window. One Tab skips disabled actions
        # to Cancel. No claim is made about keyboard delivery once frozen.
        d.key('tab','alt');d.key('tab');d.key('ret');d.expect('POWER_CANCEL','accepted')
        time.sleep(1.2)
        # Focus is Cancel. Shift+Tab twice selects Restart then Shut down.
        d.key('tab','shift');d.key('tab','shift');d.key('ret');d.expect('POWER_REVIEW','shutdown')
        d.key('ret');d.expect('POWER_WAIT','Text Editor');d.key('ret')
        d.expect('POWER_REQUEST','accepted/coalesced')
        vm.until(rb'power: request [^\n]*failed stage=dry-run[^\n]*software-resumed=1[^\n]*\n',30)
        d.screenshot('power-desktop-recovered')
        # Consent never cleared dirty state or closed editor. Save those exact
        # bytes after the real AP rollback, then leave the desktop normally.
        d.key('s','ctrl');vm.send('/tmp/power-desktop.txt\n');d.expect('SAVE','/tmp/power-desktop.txt')
        assert not __import__('re').search(r'DESKTOP CLOSE window='+str(editor)+r'\b',plain(vm))
        d.exit()
        snapshot=vm.command('power','power_state=failed')
        assert 'request_id=2' in snapshot and 'failure_stage=dry-run' in snapshot,snapshot
        vm.command('cat /tmp/power-desktop.txt','POWER_DIRTY_CONTENT')
        vm.command('check','USERLAND CHECK PASS');vm.command('thread-check','THREAD CHECK PASS',120)
        vm.command('ipc-check','IPC CHECK PASS',90)
        report.update(result='passed',power_snapshot=snapshot,serial_log=str(vm.log.name),editor_window=editor)
    finally:
        if vm: vm.close()
        (WORK/'report.json').write_text(json.dumps(report,indent=2)+'\n')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--iso',type=pathlib.Path,required=True)
    p.add_argument('--gui',type=pathlib.Path,default=ROOT/'userland/bin/gui');p.add_argument('--memory',default='384M')
    p.add_argument('--cpus',type=int,default=4);a=p.parse_args();WORK.mkdir(parents=True,exist_ok=True)
    original=digest(a.iso);gui=digest(a.gui);iso,inputs=prepare(a.iso.resolve(),a.gui.resolve())
    report=dict(result='failed',inputs=inputs,memory=a.memory,cpus=a.cpus)
    try: exercise(iso,a.memory,a.cpus,report)
    finally:
        assert digest(a.iso)==original and digest(a.gui)==gui,'read-only input changed'
    print('POWER DESKTOP PASS dirty consent/cancel/pending cancel/AP rollback/save/thread/IPC',flush=True)
if __name__=='__main__': main()
