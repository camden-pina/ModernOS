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

with Path(str(iso) + '.lock').open('a') as lock:
    fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
    try:
        vm = runtime.VM(2, storage=False, network=False, memory='16G', accel='kvm', iso=iso, label='desktop-workflow')
        vm.user_command('mkdir -p /home/user/Projects /home/user/Desktop /home/user/.config')
        vm.user_command('write /home/user/Projects/keep.txt ORIGINAL')
        vm.shell_command("printf 'scale=3\\nterminal_shell=1\\n' > /home/user/.config/desktop.ini",as_user=True)
        vm.send('gui --trace --stats\n'); vm.until(rb'GUI_READY:', 60)
        d = desktop.DesktopTest(vm); d.screenshot('00-desktop')
        original_move = d.move
        def move(x, y, delay=.08):
            # USB relative reports are bounded; wait for each small step to
            # arrive before using the next position as the host origin.
            while abs(x-d.x) > 96 or abs(y-d.y) > 96:
                original_move(d.x + max(-96, min(96, x-d.x)),
                              d.y + max(-96, min(96, y-d.y)), delay=.025)
                raw()
            original_move(x, y, delay=max(delay, .1)); raw()
        d.move = move
        files = int(re.findall(r'DESKTOP OPEN window=(\d+)[^\n]* Files\n', raw())[-1])
        raw(); g = d.geometry(files)
        # Open another view of the same watched inode and close it. Its watch
        # must remain installed for the original view.
        other=d.launch('Files')
        assert other != files
        d.key('f4', 'alt'); time.sleep(.2)
        d.click(g['x']+g['width']-100, g['y']+g['height']-100, 'right')
        d.screenshot('01-folder-context-menu')
        start = len(raw()); menu_index(0)
        wait(r'DESKTOP TERMINAL_CWD[^\n]* /home/user/Projects\n', start)
        terminal = int(wait(r'DESKTOP OPEN window=(\d+)[^\n]* /home/user/Projects\n', start)[1])
        start = len(raw())
        vm.send('pwd > /tmp/open-terminal-cwd; touch watched.txt; mkdir watched-dir; printf DESKTOP_DATA > /home/user/Desktop/outside.txt\n')
        wait(r'DESKTOP FILES_AUTO_REFRESH window='+str(files)+r'[^\n]* /home/user/Projects\n', start)
        wait(r'DESKTOP FILES_AUTO_REFRESH window=-1[^\n]* /home/user/Desktop\n', start)
        passed('External terminal creates appear automatically in Files and on the desktop; closing a shared watcher preserves the remaining view')
        d.key('f4', 'alt'); time.sleep(.2)
        d.find('watched.txt'); start = len(raw()); d.key('ret')
        wait(r'DESKTOP OPEN window=\d+[^\n]* /home/user/Projects/watched.txt\n', start)
        d.key('f4', 'alt'); d.key('esc')
        passed('The automatically refreshed file can be opened through the real Files search and Enter action')
        # Six keyboard-accessible layouts, each with a real screen preview.
        for layout in range(6):
            start = len(raw()); d.key('z', 'meta_l')
            wait(r'DESKTOP SNAP_LAYOUTS window='+str(files)+r'[^\n]* open', start)
            for _ in range(layout): d.key('down')
            if layout == 3: d.key('right')
            d.screenshot('02-layout-'+str(layout))
            recomposition(); start = len(raw()); d.key('ret')
            wait(r'DESKTOP SNAP window='+str(files), start)
            snapped = d.geometry(files)
            assert snapped['width'] < d.width
            if layout == 3: assert snapped['height'] < d.height / 2
            d.key('down', 'meta_l'); time.sleep(.15)
        passed('Six layout-picker previews, keyboard selection, exact restore and incremental/full pixel equivalence')
        raw(); g = d.geometry(files)
        start = len(raw()); d.move(g['x']+120, g['y']+g['body']//2)
        time.sleep(.6)
        wait(r'DESKTOP SNAP_LAYOUTS window='+str(files)+r'[^\n]* open', start)
        d.screenshot('02-maximize-hover')
        start = len(raw()); d.click(g['x']+170, g['y']+g['body']+110)
        wait(r'DESKTOP SNAP window='+str(files), start)
        snapped = d.geometry(files)
        assert snapped['x'] == 0 and snapped['width'] == d.width//2
        d.key('down', 'meta_l'); time.sleep(.2)
        passed('Hovering the maximize control opens the layout picker and a pointer click applies the preview')
        raw(); g = d.geometry(files)
        d.move(g['x']+g['width']//2, g['y']+g['body']//2); d.button(True)
        d.move(3, d.height//2); time.sleep(.15)
        d.screenshot('03-edge-preview'); recomposition(); d.button(False)
        time.sleep(.2); raw(); snapped = d.geometry(files)
        assert snapped['x'] == 0 and snapped['width'] == d.width//2, snapped
        d.key('down', 'meta_l'); time.sleep(.2)
        passed('Dragging to a screen edge shows a preview and snaps on release')
        # Open the actual desktop file, then create another item from its menu.
        d.click(100, 120); d.click(100, 120)
        wait(r'DESKTOP OPEN window=\d+[^\n]* /home/user/Desktop/outside.txt\n')
        d.key('end'); d.key('x'); d.key('s', 'ctrl'); time.sleep(.2)
        d.key('f4', 'alt')
        d.click(100, 420, 'right'); menu_index(1)
        vm.send('SavedHere\n'); time.sleep(.4)
        d.screenshot('04-desktop-files'); recomposition()
        passed('Desktop file icons open focused, keyboard-editable files; the desktop menu creates real folders')

        # The first snap waits for a choice, then one final peer auto-fills.
        editor=d.launch('Text Editor');terminal=d.launch('Terminal');raw();g=d.geometry(terminal)
        d.move(g['x']+g['width']//2,g['y']+g['body']//2);d.button(True)
        start=len(raw());d.move(d.width//2,55)
        wait(r'DESKTOP SNAP_BAR window='+str(terminal),start)
        d.move(1070,145);d.screenshot('05-top-snap-bar');recomposition();d.button(False)
        wait(r'DESKTOP SNAP_ASSIST window='+str(terminal)+r'[^\n]* choose next window',start)
        d.screenshot('05-assist-choose');recomposition()
        assert not re.search(r'DESKTOP SNAP window='+str(editor),raw()[start:])
        # MRU preview is the editor; keyboard and click share the same choice.
        d.click(1100,600);time.sleep(.3);raw()
        boxes=[d.geometry(i) for i in [terminal,editor,files]]
        for box in boxes: assert abs(box['width']-d.width/3)<=1 and box['height']>d.height*.8,box
        for i,box in enumerate(boxes):
            for other in boxes[:i]:
                assert box['x']>=other['x']+other['width'] or other['x']>=box['x']+box['width']
        d.screenshot('06-filled-layout')
        for ident in [terminal,editor]:
            g=d.geometry(ident);d.click(g['x']+g['width']//2,g['y']+g['body']//2);d.key('f4','alt');time.sleep(.15);raw()
        g=d.geometry(files);d.click(g['x']+g['width']//2,g['y']+g['body']//2);d.key('down','meta_l')
        time.sleep(.2);raw()
        passed('Top drag bar starts three-column Snap Assist; an explicit choice places the second window and the last eligible window fills the final slot')
        d.key('up','meta_l');time.sleep(.2);raw()
        def organize(index):
            raw();geometry=d.geometry(files)
            # Expanded search:124px,12px gap, final24px toolbar button.
            d.click(geometry['x']+geometry['width']-328,geometry['y']+32)
            menu_index(index)
        d.find('watched-dir');d.key('home')
        start=len(raw());d.key('f10','shift')
        m=wait(r'DESKTOP MENU_COLORS x=(\d+) y=(\d+) width=(\d+) height=(\d+) selected=(\d+)',start)
        mx,my,mw,mh,selected=map(int,m.groups())
        d.screenshot('07-color-dot-picker');d.click(mx+282,my+mh-40)
        wait(r'DESKTOP FILE_TAG[^\n]* /home/user/Projects/watched-dir',start)
        organize(1);vm.send('Tagged project\n');time.sleep(.25)
        organize(3);time.sleep(.2)
        d.screenshot('07-folder-color-and-tag');recomposition()
        d.key('f2');vm.send('BlueWork\n');wait(r'DESKTOP RENAME[^\n]* /home/user/Projects/BlueWork')
        d.key('esc');d.find('Tagged project');d.key('home');d.key('ret')
        wait(r'DESKTOP NAVIGATE[^\n]* /home/user/Projects/BlueWork')
        d.key('up','alt');time.sleep(.2)
        passed('Blue folder color, searchable text tag and favorite survive a Files rename; tag search opens the renamed folder')
        d.find('keep.txt');d.key('home');start=len(raw());d.key('d','ctrl')
        wait(r'DESKTOP PASTE_COMPLETE[^\n]* /home/user/Projects',start)
        d.key('home');start=len(raw());organize(4);vm.send('/home/user/Desktop\n')
        wait(r'DESKTOP PASTE_COMPLETE[^\n]* /home/user/Desktop',start)
        d.key('home');organize(10);time.sleep(.2)
        d.key('home');organize(13);menu_index(2);vm.send('Notes.md\n');time.sleep(.3)
        d.key('esc');raw();g=d.geometry(files)
        d.key('g','alt');d.key('end');d.key('up');d.key('up');d.key('ret');menu_index(0)
        wait(r'DESKTOP NAVIGATE[^\n]* /home/user/Projects/BlueWork')
        d.key('up','alt');d.screenshot('08-extended-files');recomposition()
        d.key('spc','meta_l');time.sleep(.2);d.screenshot('08-spotlight-launcher');recomposition();d.key('esc');time.sleep(.2)
        passed('Extension-preserving duplicate, copy to Desktop, read-only toggle, Markdown template and saved favorite navigation')
        d.exit()
        output = vm.command('cat /tmp/open-terminal-cwd')
        assert '/home/user/Projects' in output, output
        output = vm.command('ls /home/user/Desktop')
        assert 'SavedHere' in output and 'outside.txt' in output, output
        assert 'DESKTOP_DATAx' in vm.command('cat /home/user/Desktop/outside.txt')
        vm.shell_command('test -d /home/user/Desktop/SavedHere')
        passed('Verified terminal working directory and desktop file/folder bytes through the guest filesystem')

        assert 'ORIGINAL' in vm.shell_command("cat '/home/user/Projects/keep copy 1.txt'")
        assert 'ORIGINAL' in vm.command('cat /home/user/Desktop/keep.txt')
        assert '# Untitled' in vm.command('cat /home/user/Projects/Notes.md')
        readonly=vm.command('stat /home/user/Projects/keep.txt')
        assert 'mode=400 uid=1000 gid=1000' in readonly,readonly
        # Start a fresh GUI process: annotations must load from the saved profile file.
        vm.send('gui --trace --stats\n');vm.until(rb'GUI_READY:',60)
        d=desktop.DesktopTest(vm);d.screenshot('09-reopened');d.move=move
        d.find('Tagged project');d.key('home');start=len(raw());d.key('ret')
        wait(r'DESKTOP NAVIGATE[^\n]* /home/user/Projects/BlueWork',start)
        d.screenshot('10-restored-tag-search');d.exit()
        passed('Verified file bytes, template, read-only mode and saved annotations reloaded by a fresh desktop process')
        assert 'USERLAND CHECK PASS' in vm.command('check', timeout=90)
        report['state'] = 'passed'
    except BaseException as error:
        report['error'] = repr(error); (out / 'exception.txt').write_text(traceback.format_exc()); raise
    finally:
        if vm: vm.close(); report['owned_vm_stopped'] = vm.process.poll() is not None
        with iso.open('rb') as f: report['iso_sha256'] = hashlib.file_digest(f, 'sha256').hexdigest()
        (out / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
