#!/usr/bin/env python3
"""Use real USB input to verify clipboard, Terminal paste and pointer preferences."""
import argparse,base64,json,re,time,pathlib,shlex
import test_desktop as desktop
from test_desktop import DesktopTest
from test_runtime import VM,ROOT,prepare,assert_success

CONTROLS=[0,1,2,4,5,6,21,22,23,24,25,26,27,28,29,30,31,32,8,9,10,11,12,13,14,16,17,100,101,102,103,104,105,108,109,112,113,114,116,117,20,33,128]
ACTIVE=[n for n in CONTROLS if not 24<=n<=30]
EXPECTED='CLIP_FIRST\nUTF8: α €\nCLIP_LAST\n'.encode()
SCRIPT=b'write /tmp/clipboard-paste-one QUEUE_ONE\nwrite /tmp/clipboard-paste-two QUEUE_TWO\nTAIL'

def fixture(vm,path,data):
    encoded=base64.b64encode(data).decode()
    assert_success(vm.shell_command('set -e; printf '+encoded+' | base64 -d >'+path))

def start(vm,out,name):
    vm.send('gui --trace --stats\n');vm.until(rb'GUI_READY: Coast desktop',45)
    d=DesktopTest(vm);desktop.WORK=out;d.screenshot(name);return d

def save_as(d,path):
    d.key('s','ctrl','shift');d.key('a','ctrl')
    # Use one ordered USB input stream for the field and its shortcuts. Serial
    # text and USB keys are independent sources; a later Ctrl+A/X can otherwise
    # overtake the serial suffix while the desktop repaints a large selection.
    keys={'/':'slash','.':'dot','-':'minus','_':'minus'}
    for char in path:
        d.key(keys.get(char,char), *(['shift'] if char=='_' else []))
    d.key('a','ctrl');d.key('x','ctrl');d.expect('CLIPBOARD_COPY','Text field')
    d.key('v','ctrl');d.expect('CLIPBOARD_PASTE','Text field')
    d.key('ret');d.expect('SAVE',path)

def term_command(d,command):
    d.vm.send(command+'\n')
    d.vm.until(rb'DESKTOP TERMINAL_COMMAND[^\n]*'+re.escape(command.encode())+
               rb'[\s\S]*DESKTOP TERMINAL_OUTPUT[\s\S]*os:[^\n]*\$ ',45)

def focus(d,current,target):
    while current!=target:
        a,b=ACTIVE.index(current),ACTIVE.index(target)
        if (b-a)%len(ACTIVE)<=(a-b)%len(ACTIVE):d.key('tab');current=ACTIVE[(a+1)%len(ACTIVE)]
        else:d.key('tab','shift');current=ACTIVE[(a-1)%len(ACTIVE)]
    return current

def position(d,button='middle',pressed=4):
    d.button(True,button)
    output=d.vm.until(rb'DESKTOP POINTER position=\([0-9]+,[0-9]+\)[^\n]*pressed='+str(pressed).encode()+rb'[^\n]*\n',20)
    point=tuple(map(int,re.findall(r'DESKTOP POINTER position=\((\d+),(\d+)\)',output)[-1]))
    d.button(False,button)
    return point

def relative(d,dx,dy=0):
    d.vm.qmp('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':dx}},{'type':'rel','data':{'axis':'y','value':dy}}]});time.sleep(.08)

def exercise(cpus,iso,accel,label,output_root=None):
    out=(output_root or ROOT/'build/continuation-20261015-phase2')/label/'gui';out.mkdir(parents=True,exist_ok=False)
    report={'result':'failed','iso':str(iso),'cpus':cpus,'accel':accel,'physical_hardware_validated':False}
    vm=None
    try:
        prepare() # Only disposable images under build/runtime-tests.
        vm=VM(cpus,storage=True,network=False,iso=iso,accel=accel,label=label+'-first')
        fixture(vm,'/tmp/clipboard-expected',EXPECTED);fixture(vm,'/tmp/clipboard-script',SCRIPT)
        assert_success(vm.shell_command('clip -c </tmp/clipboard-expected'))
        d=start(vm,out,'clipboard-initial')
        d.launch('Text Editor');d.key('v','ctrl');d.expect('CLIPBOARD_PASTE','Editor');save_as(d,'/tmp/clipboard-editor.txt')
        d.key('a','ctrl');d.key('c','ctrl');d.expect('CLIPBOARD_COPY','Editor selection')
        d.launch('Text Editor');d.key('v','ctrl');d.expect('CLIPBOARD_PASTE','Editor');save_as(d,'/tmp/clipboard-editor2.txt')
        d.key('home','ctrl');d.key('end','shift');d.key('x','ctrl');d.expect('CLIPBOARD_COPY','Editor selection')
        d.key('s','ctrl');d.expect('SAVE','/tmp/clipboard-editor2.txt')
        terminal_window=d.launch('Terminal');d.key('up','meta_l');d.expect('MAXIMIZE')
        d.vm.send('echo ');d.key('v','ctrl','shift');d.expect('CLIPBOARD_PASTE','Terminal command field');d.key('ret')
        vm.until(rb'DESKTOP TERMINAL_COMMAND[^\n]*echo CLIP_FIRST[\s\S]*DESKTOP TERMINAL_OUTPUT[\s\S]*os:[^\n]*\$ ',30)
        script='(sleep 2; printf "%s%s\\n" BG _SURVIVED) & printf "%s%s\\n" FG _RUNNING; sleep 10; printf "%s%s\\n" FG _MUST_ABORT'
        vm.send('build-sh -c '+shlex.quote(script)+'\n')
        vm.until(rb'FG_RUNNING[\s\S]*\n',30);time.sleep(.15);d.key('c','ctrl')
        interrupted=vm.until(rb'(?s)(?=.*BG_SURVIVED)(?=.*(?:terminated by signal 2|exited 130))(?=.*os:[^\n]*\$ ).*',30)
        assert 'FG_MUST_ABORT' not in interrupted,interrupted
        # Select rendered output, including a real shell response; no ANSI bytes
        # are copied from the PTY transport stream.
        g=d.geometry(terminal_window);d.move(g['x']+18,g['y']+g['body']+15);d.button(True);d.move(d.width-30,d.height-140);d.button(False)
        d.key('c','ctrl','shift');d.expect('CLIPBOARD_COPY','Terminal selection');d.screenshot('clipboard-terminal-selection')
        d.launch('Text Editor');d.key('v','ctrl');d.expect('CLIPBOARD_PASTE','Editor');save_as(d,'/tmp/clipboard-terminal-selected.txt')
        # Use another Terminal to change the shared clipboard from a native
        # application. Browser address fields flatten line breaks into spaces.
        d.launch('Terminal');term_command(d,"build-sh -c 'clip -c < /tmp/clipboard-expected'")
        d.launch('Browser');d.key('l','ctrl');d.key('v','ctrl');d.expect('CLIPBOARD_PASTE','Text field')
        d.key('a','ctrl');d.key('c','ctrl');d.expect('CLIPBOARD_COPY','Text field')
        d.launch('Text Editor');d.key('v','ctrl');d.expect('CLIPBOARD_PASTE','Editor');save_as(d,'/tmp/clipboard-address.txt')
        d.launch('Terminal');term_command(d,"build-sh -c 'clip -c < /tmp/clipboard-script'")
        d.key('v','ctrl','shift');d.expect('CLIPBOARD_PASTE_REVIEW');d.screenshot('clipboard-paste-review');d.key('esc')
        d.exit()
        assert_success(vm.shell_command('set -e; cmp /tmp/clipboard-expected /tmp/clipboard-editor.txt; test ! -e /tmp/clipboard-paste-one; test ! -e /tmp/clipboard-paste-two'))
        fixture(vm,'/tmp/clipboard-cut-expected',EXPECTED[len(b'CLIP_FIRST'):])
        fixture(vm,'/tmp/clipboard-address-expected',EXPECTED.replace(b'\n',b' '))
        assert_success(vm.shell_command('set -e; cmp /tmp/clipboard-cut-expected /tmp/clipboard-editor2.txt; cmp /tmp/clipboard-address-expected /tmp/clipboard-address.txt; grep CLIP_FIRST /tmp/clipboard-terminal-selected.txt; if grep "$(printf \'\\033\')" /tmp/clipboard-terminal-selected.txt; then exit 1; fi'))
        print('PASS real GUI editor/window/Terminal/address/dialog clipboard boundaries, selected UTF8 and canceled multiline paste',flush=True)
        assert_success(vm.shell_command('clip -c </tmp/clipboard-script'))
        d=start(vm,out,'clipboard-paste-session')
        d.launch('Terminal');d.key('v','ctrl','shift');d.expect('CLIPBOARD_PASTE_REVIEW');d.key('ret')
        vm.until(rb'DESKTOP CLIPBOARD_PASTE_ACCEPT[\s\S]*DESKTOP CLIPBOARD_PASTE_DONE',30)
        d.key('a','ctrl');d.key('c','ctrl','shift');d.expect('CLIPBOARD_COPY','Text field')
        d.launch('Settings');current=0
        current=focus(d,current,100);d.key('ret');d.expect('POINTER_PREFERENCES','linear')
        base=position(d);relative(d,1);half=position(d);relative(d,1);whole=position(d)
        assert half==base and whole==(base[0]+1,base[1]),(base,half,whole)
        relative(d,-2*whole[0]-4);time.sleep(.5);edge=position(d);assert edge[0]==0,edge
        relative(d,1);inward_half=position(d);relative(d,1);inward_whole=position(d)
        assert inward_half[0]==0 and inward_whole[0]==1,(inward_half,inward_whole)
        current=focus(d,current,103);d.key('ret');d.expect('POINTER_PREFERENCES','linear')
        base=position(d);relative(d,10);twice=position(d);assert twice==(base[0]+20,base[1]),(base,twice)
        current=focus(d,current,109);d.key('ret');d.expect('POINTER_PREFERENCES')
        position(d,'left',2) # Physical left is now logical right; no primary UI action.
        current=focus(d,current,114);d.key('ret');d.expect('POINTER_PREFERENCES')
        current=focus(d,current,117);d.key('ret');d.expect('POINTER_PREFERENCES')
        d.screenshot('pointer-mouse-preferences')
        # Disabled TouchPad controls are skipped by keyboard focus. These are
        # actual QEMU USB mouse capabilities, never fabricated touchpad support.
        current=focus(d,current,20);d.key('ret');saved=d.expect('CONFIG_SAVED');d.key('esc')
        config=re.findall(r'(/[^\n]+desktop\.ini)',saved)[-1].strip()
        report['config_path']=config;d.screenshot('pointer-persistence-status')
        d.exit()
        output=vm.shell_command('set -e; test "$(clip -p)" = TAIL; cat '+config,'pointer_speed=200')
        for value in ('primary_right=1','wheel_steps=3','natural_scroll=1','touchpad_speed=100'):assert value in output,output
        assert_success(vm.command('cat /tmp/clipboard-paste-one','QUEUE_ONE'))
        assert_success(vm.command('cat /tmp/clipboard-paste-two','QUEUE_TWO'))
        vm.close();vm=None
        vm=VM(cpus,storage=True,network=False,iso=iso,accel=accel,label=label+'-fresh')
        output=vm.command('cat '+config,'pointer_speed=200');assert_success(output)
        d=start(vm,out,'pointer-fresh-guest')
        base=position(d);relative(d,10);twice=position(d);assert twice==(base[0]+20,base[1]),(base,twice)
        d.launch('Settings');current=focus(d,0,128);d.key('ret');d.expect('POINTER_PREFERENCES','linear')
        base=position(d);relative(d,10);normal=position(d);assert normal==(base[0]+10,base[1]),(base,normal)
        d.key('s','ctrl');d.expect('CONFIG_SAVED');d.key('esc');d.screenshot('pointer-reset-defaults');d.exit()
        output=vm.command('cat '+config,'pointer_speed=100')
        for value in ('primary_right=0','pointer_acceleration=0','wheel_steps=1','natural_scroll=0','touchpad_speed=100'):assert value in output,output
        # Invalid stored values are ignored and defaults remain usable.
        invalid=b'pointer_speed=0\npointer_acceleration=9\nprimary_right=-1\nwheel_steps=0\nnatural_scroll=7\ntouchpad_speed=1000\ntouchpad_natural=garbage\n'
        fixture(vm,config,invalid)
        d=start(vm,out,'pointer-invalid-config-defaults');d.launch('Settings');d.key('s','ctrl');d.expect('CONFIG_SAVED');d.key('esc');d.exit()
        output=vm.command('cat '+config,'pointer_speed=100')
        for value in ('primary_right=0','pointer_acceleration=0','wheel_steps=1','natural_scroll=0','touchpad_speed=100','touchpad_natural=0'):assert value in output,output
        report['malformed_config_defaults']=True
        report['terminal_ctrl_c_preserved_background_job']=True
        report['fractional_inward_edge_movement']=True
        report.update(result='passed',fresh_guest_persistence=True,clipboard_files=['/tmp/clipboard-editor.txt','/tmp/clipboard-editor2.txt','/tmp/clipboard-terminal-selected.txt','/tmp/clipboard-address.txt'],pointer_trace_verified=True)
        print('PASS actual GUI multiline paste accepted once with unfinished tail, fractional/double pointer speed, primary mapping, unavailable TouchPad controls, fresh-disk preference reload and reset',flush=True)
    except BaseException:
        if vm:
            try:
                vm.qmp('screendump',{'filename':str(out/'failure.ppm')})
                from PIL import Image
                Image.open(out/'failure.ppm').save(out/'failure.png')
            except BaseException:
                pass
        raise
    finally:
        if vm:vm.close()
        (out/'guest-report.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--iso',type=pathlib.Path,required=True);p.add_argument('--label',required=True);p.add_argument('--output-root',type=pathlib.Path);p.add_argument('--cpus',type=int,default=4);p.add_argument('--accel',choices=('tcg','kvm'),default='tcg');a=p.parse_args();exercise(a.cpus,a.iso.resolve(),a.accel,a.label,a.output_root)
