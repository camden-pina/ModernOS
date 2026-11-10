#!/usr/bin/env python3
"""Real native-menu/window/scaling workflows on an explicit disposable guest."""
import argparse,hashlib,json,pathlib,re,time
import test_desktop as desktop
from test_desktop import DesktopTest
from test_runtime import VM
ROOT=pathlib.Path(__file__).resolve().parents[1]

def digest(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def raw(vm):
    vm.log.flush();return pathlib.Path(vm.log.name).read_text(errors='replace')
def geometry(d,window):
    pattern=rf'DESKTOP GEOMETRY window={window} workspace=\d+ x=(\d+) y=(\d+) width=(\d+) height=(\d+) body=(\d+) scale=(\d+)\n'
    found=re.findall(pattern,raw(d.vm))
    if not found:
        d.vm.until(pattern.encode(),20);found=re.findall(pattern,raw(d.vm))
    assert found,'Missing actual geometry'
    return dict(zip(('x','y','width','height','body','scale'),map(int,found[-1])))
def start(vm,out,name):
    vm.send('gui --trace --stats\n');vm.until(rb'GUI_READY: Coast desktop',45)
    desktop.WORK=out;d=DesktopTest(vm);d.screenshot(name);return d

def menu(d,letter,name,down=0,action=None,detail=''):
    d.key(letter,'alt');d.expect('MENU',name)
    for _ in range(down):d.key('down')
    d.key('ret')
    return d.expect(action,detail) if action else ''

def windows(d,down,action,detail=''):
    d.key('w','meta_l');d.expect('WINDOW_LIST')
    for _ in range(down):d.key('down')
    d.key('ret');return d.expect(action,detail)

def desktop_case(vm,out,report,passed):
    project='/home/user/Projects/UI-Validation';note=project+'/menu-note.txt'
    vm.command('mkdir -p '+project)
    vm.command('write '+project+'/README.txt PHASE4_REAL_GUEST_FILES')
    vm.command('write '+project+'/build.c "int main(void) { return 0; }"')
    d=start(vm,out,'desktop-initial');g=geometry(d,1);assert g['width']<=1140 and g['height']<=760 and g['body']==64
    report['initial_geometry']=g;d.path(project);d.screenshot('files-real-project');passed('bounded large-screen initial geometry with real guest files')
    menu(d,'f','File',2,'MODAL');vm.send('MenuFolder\n');d.expect('CREATE_FOLDER',project+'/MenuFolder')
    menu(d,'f','File',1,'MODAL');vm.send('menu-note.txt\n');d.expect('CREATE_FILE',note)
    d.find('menu-note.txt');menu(d,'f','File',3,'OPEN',note)
    text='write /tmp/phase4-menu-proof PHASE4_MENU_ROUNDTRIP';vm.send(text);time.sleep(.15)
    menu(d,'f','File',2,'SAVE',note)
    menu(d,'e','Edit',0)
    menu(d,'e','Edit',1,'CLIPBOARD_COPY','Editor')
    menu(d,'f','File',1,'MODAL');vm.send(project+'/README.txt\n');d.expect('OPEN',project+'/README.txt')
    d.key('f4','alt');d.expect('CLOSE')
    d.key('t','meta_l');opened=d.expect('OPEN','Terminal');terminal=int(re.findall(r'OPEN window=(\d+)',opened)[-1])
    d.key('m','ctrl','alt');d.expect('MENU','File');d.key('right');d.expect('MENU','Edit');d.key('ret');d.expect('CLIPBOARD_PASTE','Terminal command field')
    d.key('ret');time.sleep(.3)
    passed('real File/Edit menus create, open, save, copy and paste through Editor and Terminal')
    focused=windows(d,1,'FOCUS','Text Editor');editor=int(re.findall(r'FOCUS window=(\d+)',focused)[-1])
    d.key('spc','alt');d.key('ret');d.expect('MINIMIZE')
    restored=windows(d,1,'FOCUS','Text Editor');assert f'window={editor} ' in restored
    d.key('2','meta_l','shift');d.expect('MOVE_WORKSPACE')
    restored=windows(d,2,'FOCUS','Text Editor');assert f'window={editor} ' in restored and 'workspace=2' in restored
    d.key('1','meta_l');d.expect('WORKSPACE')
    d.key('w','meta_l');d.expect('WINDOW_LIST')
    for _ in range(3):d.key('down')
    d.key('ret');vm.until(rb'DESKTOP SNAP[^\n]*left[^\n]*\n[\s\S]*DESKTOP SNAP[^\n]*right[^\n]*\n',20)
    windows(d,4,'CASCADE');d.screenshot('arranged-windows')
    passed('window list restores minimized windows across workspaces and arranges real windows')
    d.click(d.width-163+60,d.height-20)
    d.expect('STATUS_MENU');d.key('down');d.key('down');d.key('ret');opened=d.expect('OPEN','network')
    power=int(re.findall(r'OPEN window=(\d+)',opened)[-1]);d.screenshot('network-diagnostics')
    for count,title,image in [(5,'Audio device','audio-device'),(3,'Processes','processes'),(2,'Storage mounts','storage-mounts'),(6,'Full diagnostics','full-diagnostics')]:
        menu(d,'v','View',count,'SYSTEM_VIEW',title);d.screenshot(image)
    menu(d,'v','View',4,'SYSTEM_VIEW','Network diagnostics')
    menu(d,'f','System',1,'POWER_REVIEW','shutdown');d.screenshot('menu-power-review');d.key('esc')
    d.key('spc','alt');d.key('ret');d.expect('MINIMIZE');restored=windows(d,2,'FOCUS','System & Power');assert f'window={power} ' in restored
    passed('actual network/audio/process/storage diagnostics and minimized System window access')
    game=d.launch('Doom');vm.until(rb'DESKTOP GAME_FRAME window='+str(game).encode()+rb'[^\n]*sequence=[1-9][^\n]*\n',45)
    d.screenshot('doom-with-desktop-menus');d.key('spc','alt');d.key('ret');d.expect('MINIMIZE')
    restored=windows(d,3,'FOCUS','Doom');assert f'window={game} ' in restored
    d.key('f4','alt');vm.until(rb'DESKTOP GAME_EXIT pid=[0-9]+ status=0 reaped=1[^\n]*\n',30)
    passed('managed Doom remains reachable after minimize and closes/reaps normally')
    # No power action was requested while navigating read-only diagnostics.
    assert 'DESKTOP POWER_REQUEST' not in raw(vm)
    d.exit();vm.command('cat /tmp/phase4-menu-proof','PHASE4_MENU_ROUNDTRIP')
    saved=vm.command('cat '+note,text);assert text in saved
    vm.shell_command("printf 'scale=2\\ndark=1\\nworkspaces=3\\n' > /home/user/.config/desktop.ini")
    d=start(vm,out,'desktop-dark-150');g=geometry(d,1);assert g['scale']==150 and g['body']==96
    d.key('f','alt');d.expect('MENU','File');d.screenshot('menus-dark-150');d.key('esc');d.exit()
    passed('saved document and command result verified; dark150% menus and console return')
    report.update(editor=editor,terminal=terminal,power=power,game=game)

def compact_case(vm,out,report,passed):
    vm.command('mkdir -p /home/user/.config')
    vm.shell_command("printf 'scale=3\\ndark=1\\nworkspaces=3\\n' > /home/user/.config/desktop.ini")
    d=start(vm,out,'compact-desktop-200');assert (d.width,d.height)==(640,480)
    g=geometry(d,1);assert g['scale']==200 and g['body']==128
    settings=d.launch('Settings')
    menu(d,'v','View',2,'SETTINGS_SECTION','Display and scaling');d.key('ret');d.expect('SCALE')
    menu(d,'v','View',2,'SETTINGS_SECTION','Display and scaling')
    for _ in range(3):d.key('right')
    d.key('ret');d.expect('SCALE')
    for window in (1,settings):
        live=d.geometry(window);assert live['scale']==200 and live['x']>=0 and live['y']>=0 and live['x']+live['width']<=640 and live['y']+live['height']<=396,live
    d.screenshot('compact-live-scale-200');d.key('f4','alt');d.expect('CLOSE');passed('live100-to200% scaling keeps real windows inside the640x480 work area')
    d.key('spc','meta_l');output=d.expect('LAUNCHER')
    p=re.findall(r'LAUNCHER x=(\d+) y=(\d+) width=(\d+) height=(\d+) visible=(\d+) scale=(\d+)',output)[-1]
    x,y,w,h,visible,scale=map(int,p);assert x>=0 and y>=0 and x+w<=640 and y+h<=480-84 and visible>=1 and scale==200,p
    d.key('end');d.screenshot('compact-launcher-last');d.key('home')
    # Wheel scrolling uses actual emulated USB input, with all apps retained.
    d.move(x+w//2,y+124+30);d.button(True,'wheel-down');d.button(False,'wheel-down');d.screenshot('compact-launcher-wheel')
    d.key('esc');d.key('f1');d.expect('MODAL');d.key('pgdn');output=d.expect('MODAL_SCROLL')
    offset,height,viewport=map(int,re.findall(r'offset=(\d+) height=(\d+) viewport=(\d+)',output)[-1]);assert 0<offset<=height-viewport
    d.screenshot('compact-scrollable-help');d.key('esc');passed('640x480 at200%: bounded scrolling launcher and accessible modal text')
    d.click(d.width-326+120,d.height-40);d.expect('STATUS_MENU')
    for _ in range(7):d.key('down')
    d.key('ret');output=d.expect('WORKSPACE');assert 'workspace=2' in output
    d.key('e','meta_l');d.expect('OPEN','Files');passed('compact taskbar preserves workspace switching and app access through menus/shortcuts')
    d.key('spc','meta_l');d.expect('LAUNCHER');d.key('end');d.screenshot('compact-console-access')
    # End reveals the last (Return to console) row; click it rather than invoking
    # the quit shortcut, proving pointer access to the retained final entry.
    d.click(x+50,y+124+(visible-1)*78+30)
    vm.until(rb'gui: returned to terminal\.[\s\S]*os:[^\n]*\$ $',30)
    vm.command('echo COMPACT_CONSOLE_RESTORED','COMPACT_CONSOLE_RESTORED\n')
    passed('last launcher entry remains reachable by pointer and restores the console')
    report['launcher_bounds']={'x':x,'y':y,'width':w,'height':h,'visible':visible,'scale':scale}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--iso',type=pathlib.Path,required=True);p.add_argument('--cpus',type=int,choices=[1,4],default=4);p.add_argument('--accel',choices=['kvm','tcg'],default='kvm');p.add_argument('--mode',choices=['desktop','compact'],default='desktop');p.add_argument('--label',required=True);p.add_argument('--output-root',type=pathlib.Path,default=ROOT/'build/continuation-20261015-phase4');a=p.parse_args()
    out=a.output_root/a.label;out.mkdir(parents=True,exist_ok=False);before=digest(a.iso);v={'result':'failed','iso':str(a.iso.resolve()),'iso_sha256':before,'cpus':a.cpus,'mode':a.mode,'cases':[]};vm=None
    def passed(case):v['cases'].append(case);print('PASS '+case,flush=True)
    try:
        vm=VM(a.cpus,storage=False,iso=a.iso,label=a.label,accel=a.accel,network=True,audio_capture=out/'hda.wav',audio_device='hda',audio_capture_rate=48000)
        v['serial_log']=str(vm.log.name)
        (desktop_case if a.mode=='desktop' else compact_case)(vm,out,v,passed)
        v['result']='passed'
    except BaseException as error:v['error']=str(error);raise
    finally:
        if vm:vm.close()
        v['inputs_unchanged']=digest(a.iso)==before
        if not v['inputs_unchanged']:v['result']='failed'
        (out/'report.json').write_text(json.dumps(v,indent=2)+'\n')
    assert v['inputs_unchanged'];print('DESKTOP REFINEMENT PASSED '+str(out/'report.json'),flush=True)
if __name__=='__main__':main()
