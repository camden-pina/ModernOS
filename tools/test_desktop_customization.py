#!/usr/bin/env python3
"""Exercise snap groups, saved file annotations and extended Files actions via USB input."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import time
import traceback
import test_runtime as runtime
import test_desktop as desktop

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--iso', type=Path, required=True)
p.add_argument('--output-root', type=Path, required=True)
a = p.parse_args()
iso, out = a.iso.resolve(), a.output_root.resolve()
out.mkdir(parents=True, exist_ok=False)
runtime.WORK = desktop.WORK = out
(out / 'HARNESS.py').write_bytes(Path(__file__).read_bytes())
report = dict(state='failed', iso=str(iso), checks=[])
vm = None

def raw():
    while vm.selector.select(.002):
        b = os.read(vm.process.stdout.fileno(), 65536)
        if not b: break
        vm.pending.extend(b); vm.log.write(b); vm.log.flush()
    return Path(vm.log.name).read_text(errors='replace')

def wait(pattern, start=0, timeout=30):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        text = raw()[start:]
        assert 'PANIC:' not in text
        found = re.search(pattern, text)
        if found: return found
        time.sleep(.02)
    raise AssertionError('Missing ' + pattern + '\n' + raw()[start:][-3500:])

def passed(text):
    report['checks'].append(text); print('PASS ' + text, flush=True)

def recomposition():
    start = len(raw()); d.key('f12', 'ctrl', 'alt')
    m = wait(r'DESKTOP RECOMPOSE_CHECK error=0 pixels=\d+ mismatches=(\d+)', start)
    assert m[1] == '0', m[0]

def menu_index(index):
    d.key('home')
    for _ in range(index): d.key('down')
    d.key('ret')

with Path(str(iso)+'.lock').open('a') as lock:
    fcntl.flock(lock,fcntl.LOCK_SH|fcntl.LOCK_NB)
    try:
        vm=runtime.VM(2,storage=False,network=False,memory='16G',accel='kvm',iso=iso,label='customization')
        vm.user_command('mkdir -p /home/user/Projects/PaintMe /home/user/Desktop /home/user/.config')
        vm.user_command('write /home/user/Projects/.hidden-choice HIDDEN')
        vm.user_command('write /home/user/Desktop/Desktop-note.txt DESKTOP')
        vm.shell_command("printf 'scale=3\\nterminal_shell=1\\n' > /home/user/.config/desktop.ini",as_user=True)
        vm.send('gui --trace --stats\n');vm.until(rb'GUI_READY:',60)
        d=desktop.DesktopTest(vm);d.screenshot('00-desktop')
        original_move=d.move
        def move(x,y,delay=.08):
            while abs(x-d.x)>96 or abs(y-d.y)>96:
                original_move(d.x+max(-96,min(96,x-d.x)),d.y+max(-96,min(96,y-d.y)),delay=.025);raw()
            original_move(x,y,delay=max(.08,delay));raw()
        d.move=move
        files=int(re.findall(r'DESKTOP OPEN window=(\d+)[^\n]* Files\n',raw())[-1])
        start=len(raw());d.key('spc','meta_l')
        m=wait(r'DESKTOP LAUNCHER x=(\d+) y=(\d+) width=(\d+) height=(\d+) visible=0 scale=200',start)
        x,y,width,height=map(int,m.groups());assert height==128
        d.screenshot('01-spotlight-clean');recomposition()
        d.key('ret');assert 'DESKTOP OPEN ' not in raw()[start:]
        d.click(x+width-54,y+64)
        wait(r'DESKTOP LAUNCHER[^\n]*visible=7',start);d.screenshot('02-spotlight-expanded');recomposition()
        d.key('esc');start=len(raw());d.key('spc','meta_l');vm.send('Terminal')
        wait(r'DESKTOP LAUNCHER[^\n]*visible=1',start)
        d.screenshot('03-spotlight-search');d.key('ret');wait(r'DESKTOP OPEN[^\n]*Terminal',start)
        d.key('f4','alt');time.sleep(.2)
        passed('Spotlight opens as a clean search field; empty Enter does nothing, disclosure expands it, and searching sizes the result list')
        d.find('PaintMe');d.key('home');start=len(raw());d.key('f10','shift')
        m=wait(r'DESKTOP MENU_COLORS x=(\d+) y=(\d+) width=(\d+) height=(\d+) selected=(\d+)',start)
        mx,my,mw,mh,selected=map(int,m.groups());assert selected==0
        d.screenshot('04-inline-folder-colors');recomposition()
        start=len(raw());d.click(mx+282,my+mh-40)
        wait(r'DESKTOP FILE_TAG[^\n]* /home/user/Projects/PaintMe',start)
        start=len(raw());d.key('f10','shift')
        wait(r'DESKTOP MENU_COLORS[^\n]*selected=5',start)
        d.screenshot('05-selected-folder-color');d.key('esc')
        passed('Right-click exposes color dots immediately; one pointer click applies Blue and the selected color is retained')
        settings=d.launch('Settings');d.key('up','meta_l');time.sleep(.2)
        def page(index):
            start=len(raw());d.key(str(index+1) if index<9 else '0','ctrl')
            m=wait(r'DESKTOP SETTINGS_PANEL page='+str(index)+r' x=(\d+) y=(\d+) width=(\d+) height=(\d+) scale=200',start)
            return tuple(map(int,m.groups()))
        panel=page(1);d.screenshot('06-appearance-cards');recomposition()
        def choose(group,col,count,position):
            px,py,pw,ph=panel;gap=12;cell=(pw-128-gap*(count-1))//count
            x=px+64+col*(cell+gap)+cell//2;y=py+(92+position*116+58)*2+32
            start=len(raw());d.click(x,y)
            wait(r'DESKTOP SETTINGS_CUSTOM group='+str(group)+r' value='+str(col),start)
        choose(0,1,2,0);choose(1,3,8,1);choose(2,2,4,2);choose(3,2,3,3)
        d.screenshot('07-custom-appearance');recomposition()
        panel=page(7);choose(5,0,2,0);choose(6,2,3,1);choose(7,0,3,2)
        d.screenshot('08-desktop-preferences');recomposition()
        panel=page(8);choose(8,0,2,0);choose(9,0,2,1)
        d.screenshot('09-window-preferences');recomposition()
        panel=page(9);choose(11,1,2,0);choose(12,1,2,1)
        d.screenshot('10-files-preferences');recomposition()
        config=re.findall(r'DESKTOP CONFIG_SAVED[^\n]* (/[^\r\n]+)',raw())[-1]
        d.key('f4','alt');time.sleep(.2)
        passed('Grouped Settings exposes theme, eight accents, glass, motion, desktop icons, launcher, snapping and Files defaults; controls apply through real pointer input')
        start=len(raw());newfiles=d.launch('Files')
        wait(r'DESKTOP FILES_DEFAULTS window='+str(newfiles)+r' details=1 hidden=1',start)
        d.find('.hidden-choice');d.key('home');start=len(raw());d.key('ret')
        wait(r'DESKTOP OPEN[^\n]* /home/user/Projects/.hidden-choice',start);d.key('f4','alt')
        terminal=d.launch('Terminal');raw();before=d.geometry(newfiles)
        start=len(raw());d.key('z','meta_l');d.key('down');d.key('down');d.key('ret');time.sleep(.3)
        assert 'DESKTOP SNAP_ASSIST' not in raw()[start:]
        assert d.geometry(newfiles)==before
        d.key('down','meta_l');time.sleep(.2);raw();g=d.geometry(terminal)
        start=len(raw());d.move(g['x']+120,g['y']+g['body']//2);time.sleep(.65)
        assert 'DESKTOP SNAP_LAYOUTS' not in raw()[start:]
        d.key('f4','alt')
        passed('New Files windows honor details/hidden defaults; disabled Snap Assist and maximize-hover layouts do not trigger')
        d.exit();content=vm.command('cat '+config)
        expected={'accent':3,'dark':1,'glass_style':2,'ui_motion':2,'desktop_icons':0,'desktop_icon_size':48,'launcher_rows':5,'snap_assist_enabled':0,'snap_hover_enabled':0,'snap_bar_enabled':1,'files_default_details':1,'files_default_hidden':1}
        for key,value in expected.items():assert re.search(r'(?:^|[\r\n])'+key+'='+str(value)+r'(?:[\r\n]|$)',content),(key,value,content)
        report['saved_preferences']=expected
        vm.send('gui --trace --stats\n');vm.until(rb'GUI_READY:',60)
        d=desktop.DesktopTest(vm);d.screenshot('11-reopened-desktop');original_move=d.move;d.move=move
        wait(r'DESKTOP FILES_DEFAULTS window=1 details=1 hidden=1')
        settings=d.launch('Settings');d.key('up','meta_l');panel=page(1)
        d.screenshot('12-restored-appearance');recomposition();d.key('f4','alt')
        start=len(raw());d.key('spc','meta_l');m=wait(r'DESKTOP LAUNCHER x=(\d+) y=(\d+) width=(\d+) height=(\d+) visible=0 scale=200',start)
        x,y,width,height=map(int,m.groups());d.click(x+width-54,y+64)
        wait(r'DESKTOP LAUNCHER[^\n]*visible=5',start);d.screenshot('13-restored-launcher-limit');d.key('esc');d.exit()
        passed('Saved customization values and the launcher result limit reload in a fresh desktop process')
        assert 'USERLAND CHECK PASS' in vm.command('check',timeout=90)
        report['state']='passed'
    except BaseException as error:
        report['error']=repr(error);(out/'exception.txt').write_text(traceback.format_exc())
        if 'd' in globals():
            try:d.screenshot('failure')
            except Exception:pass
        raise
    finally:
        if vm:vm.close();report['owned_vm_stopped']=vm.process.poll() is not None
        with iso.open('rb') as f:report['iso_sha256']=hashlib.file_digest(f,'sha256').hexdigest()
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
