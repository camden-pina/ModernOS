#!/usr/bin/env python3
"""Exercise snap previews, watched folders, desktop files and terminal CWD via USB input."""
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
        vm.command('mkdir -p /home/user/Projects /home/user/Desktop /home/user/.config')
        vm.command('write /home/user/Projects/keep.txt ORIGINAL')
        vm.shell_command("printf 'scale=3\\nterminal_shell=1\\n' > /home/user/.config/desktop.ini")
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
        d.click(g['x']+g['width']-100, g['y']+g['height']-100, 'right')
        menu_index(0)
        other = int(wait(r'DESKTOP OPEN window=(\d+)[^\n]* /home/user/Projects\n')[1])
        assert other != files
        d.key('f4', 'alt'); time.sleep(.2)
        d.click(g['x']+g['width']-100, g['y']+g['height']-100, 'right')
        d.screenshot('01-folder-context-menu')
        start = len(raw()); menu_index(1)
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
        d.click(100, 420, 'right'); menu_index(3)
        vm.send('SavedHere\n'); time.sleep(.4)
        d.screenshot('04-desktop-files'); recomposition()
        passed('Desktop file icons open focused, keyboard-editable files; the desktop menu creates real folders')
        d.exit()
        output = vm.command('cat /tmp/open-terminal-cwd')
        assert '/home/user/Projects' in output, output
        output = vm.command('ls /home/user/Desktop')
        assert 'SavedHere' in output and 'outside.txt' in output, output
        assert 'DESKTOP_DATAx' in vm.command('cat /home/user/Desktop/outside.txt')
        vm.shell_command('test -d /home/user/Desktop/SavedHere')
        passed('Verified terminal working directory and desktop file/folder bytes through the guest filesystem')
        assert 'USERLAND CHECK PASS' in vm.command('check', timeout=90)
        report['state'] = 'passed'
    except BaseException as error:
        report['error'] = repr(error); (out / 'exception.txt').write_text(traceback.format_exc()); raise
    finally:
        if vm: vm.close(); report['owned_vm_stopped'] = vm.process.poll() is not None
        with iso.open('rb') as f: report['iso_sha256'] = hashlib.file_digest(f, 'sha256').hexdigest()
        (out / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
