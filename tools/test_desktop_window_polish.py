#!/usr/bin/env python3
"""Exercise rounded windows, Spotlight and the compact Files toolbar in QEMU."""
import argparse
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
p.add_argument('--scale', type=int, choices=[100, 125, 150, 200], default=200)
p.add_argument('--dark', action='store_true')
a = p.parse_args()
out = a.output_root.resolve()
out.mkdir(parents=True, exist_ok=False)
runtime.WORK = desktop.WORK = out
(out / 'HARNESS.py').write_bytes(Path(__file__).read_bytes())
report = dict(state='failed', iso=str(a.iso.resolve()), scale=a.scale, dark=a.dark, checks=[])
v = d = None


def raw():
    while v.selector.select(.002):
        data = os.read(v.process.stdout.fileno(), 65536)
        if not data:
            break
        v.pending.extend(data)
        v.log.write(data)
        v.log.flush()
    return Path(v.log.name).read_text(errors='replace')


def wait(pattern, start=0, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = raw()[start:]
        assert 'PANIC:' not in value
        found = re.search(pattern, value)
        if found:
            return found
        time.sleep(.02)
    raise AssertionError('Missing ' + pattern + '\n' + raw()[start:][-2500:])


def passed(message):
    report['checks'].append(message)
    print('PASS ' + message, flush=True)


def s(value):
    return (value * a.scale + 50) // 100


def recomposition():
    start = len(raw())
    d.key('f12', 'ctrl', 'alt')
    found = wait(r'DESKTOP RECOMPOSE_CHECK error=0 pixels=\d+ mismatches=(\d+)', start)
    assert found[1] == '0', found[0]


def choose(index):
    d.key('home')
    for _ in range(index):
        d.key('down')
    d.key('ret')


def tool(index):
    g = d.geometry(files)
    d.click(g['x'] + g['width'] - s(148 - index * 28), g['y'] + s(16))


def frame(name):
    d.move(d.width - 10, d.height - s(90))
    time.sleep(.2)
    return d.screenshot(name)


try:
    v = runtime.VM(2, storage=False, network=False, memory='16G', accel='kvm',
                   iso=a.iso.resolve(), label='window-polish')
    desktop.fixtures(v)
    v.shell_command('chown -R 1000:1000 /home/user/Projects')
    v.user_command('mkdir -p /home/user/.config')
    config = f"scale={[100,125,150,200].index(a.scale)}\\ndark={int(a.dark)}\\n"
    v.shell_command("printf '" + config + "' > /home/user/.config/desktop.ini", as_user=True)
    v.send('gui --trace --stats\n')
    v.until(rb'GUI_READY:', 60)
    d = desktop.DesktopTest(v)
    d.screenshot('00-files')
    original = d.move

    def move(x, y, delay=.08):
        while abs(x-d.x) > 96 or abs(y-d.y) > 96:
            original(d.x + max(-96, min(96, x-d.x)), d.y + max(-96, min(96, y-d.y)), .025)
            raw()
        original(x, y, delay)
        raw()

    d.move = move
    files = int(re.findall(r'DESKTOP OPEN window=(\d+)[^\n]* Files\n', raw())[-1])
    g = d.geometry(files)
    sidebar = s(132 if g['width'] < s(600) else 148)
    x, y = g['x']+1+sidebar+s(60), g['y']+g['body']+s(64)
    start = len(raw())
    d.click(x, y)
    d.click(x, y)
    wait(r'DESKTOP NAVIGATE[^\n]* /home/user/Projects/Artwork\n', start)
    d.key('left', 'alt')
    wait(r'DESKTOP NAVIGATE[^\n]* /home/user/Projects\n', start)
    passed('Folder content begins directly below the compact toolbar and opens through its actual pointer hit box')

    tool(0)
    d.screenshot('01-new-menu')
    choose(4)
    start = len(raw())
    v.send('ToolbarFolder\n')
    wait(r'DESKTOP CREATE_FOLDER[^\n]* /home/user/Projects/ToolbarFolder\n', start)
    tool(1)
    wait(r'DESKTOP VIEW[^\n]* details\n')
    d.screenshot('02-list-view')
    recomposition()
    tool(2)
    choose(3)
    tool(2)
    choose(2)
    tool(1)
    wait(r'DESKTOP VIEW[^\n]* hybrid\n')
    passed('Small New, View and Sort controls create a real folder, change view and sort without the former action row')

    start = len(raw())
    tool(3)
    d.screenshot('03-more-actions')
    d.key('end')
    d.key('up')
    d.key('ret')
    wait(r'DESKTOP TERMINAL_CWD[^\n]* /home/user/Projects\n', start)
    d.key('f4', 'alt')
    g = d.geometry(files)
    d.click(g['x']+g['width']-s(24), g['y']+s(16))
    v.send('Artwork\n')
    d.key('home')
    start = len(raw())
    d.key('f10', 'shift')
    palette = wait(r'DESKTOP MENU_COLORS x=(\d+) y=(\d+) width=(\d+) height=(\d+)', start)
    mx, my, mw, mh = map(int, palette.groups())
    d.screenshot('04-context-colors')
    d.click(mx+s(141), my+mh-s(20))
    wait(r'DESKTOP FILE_TAG[^\n]* /home/user/Projects/Artwork\n', start)
    d.key('esc')
    passed('More opens a terminal in the current directory; expandable Search and subtle right-click color dots remain functional')

    # Crossfades must restore the same retained scene, including all corners.
    g = d.geometry(files)
    d.click(g['x']+s(40), g['y']+s(14))
    desktop_frame = frame('05-desktop-before-spotlight')
    start = len(raw())
    d.key('spc', 'meta_l')
    box = wait(r'DESKTOP LAUNCHER x=(\d+) y=(\d+) width=(\d+) height=(\d+) visible=0', start)
    bx, by, bw, bh = map(int, box.groups())
    popup = frame('06-spotlight')
    radius = s(16)
    cutouts = 0
    # Pixels well outside each circular corner must contain only the lower
    # scene and a dark shadow. A square glass fill or mismatched border raises
    # a channel here against the real wallpaper.
    for corner in range(4):
        for yy in range(radius):
            for xx in range(radius):
                if (radius-xx-1)**2 + (radius-yy-1)**2 <= (radius+1)**2:
                    continue
                px = bx+bw-1-xx if corner & 1 else bx+xx
                py = by+bh-1-yy if corner & 2 else by+yy
                at = (py*d.width+px)*3
                assert all(popup[at+c] <= desktop_frame[at+c]+1 for c in range(3)), (corner, xx, yy)
                cutouts += 1
    assert cutouts > 20
    recomposition()
    d.click(bx+bw-s(27), by+s(32))
    d.screenshot('07-expanded-spotlight')
    recomposition()
    d.key('esc')
    restored = frame('08-spotlight-dismissed')
    assert desktop_frame[d.width*s(22)*3:] == restored[d.width*s(22)*3:]
    passed('Spotlight has clear rounded cutouts, clean collapsed/expanded states and exact dismissal after its fade')

    # Restore Files, then exercise shadows against an overlapping live window.
    d.key('tab', 'alt')
    disks = d.launch('Disks')
    g = d.geometry(disks)
    before = frame('09-overlap')
    recomposition()
    # At100% the default centered windows can be disjoint on a2560px screen.
    # Put the upper window over real lower content before checking its glass.
    lower = d.geometry(files)
    x, y = g['x']+g['width']//2, g['y']+s(14)
    d.move(x, y)
    d.button(True)
    start = len(raw())
    d.move(x+lower['x']+s(40)-g['x'], y+lower['y']+s(70)-g['y'], .15)
    d.button(False)
    wait(r'DESKTOP GEOMETRY window='+str(disks), start)
    g = d.geometry(disks)
    before = frame('09b-overlapping-content')
    recomposition()
    x, y = g['x']+g['width']//2, g['y']+s(14)
    d.move(x, y)
    d.button(True)
    d.move(x+s(36), y+s(12), .15)
    d.button(False)
    d.move(x+s(36), y+s(12))
    d.button(True)
    d.move(x, y, .15)
    d.button(False)
    after = frame('10-drag-restored')
    assert before[d.width*s(22)*3:] == after[d.width*s(22)*3:]
    recomposition()
    d.key('up', 'meta_l')
    d.screenshot('11-maximized')
    recomposition()
    d.key('down', 'meta_l')
    frame('12-restored')
    recomposition()
    d.move(g['x']+g['width']-2, g['y']+g['height']-2)
    d.button(True)
    for i in range(1, 9):
        d.move(g['x']+g['width']-2-s(i*4), g['y']+g['height']-2-s(i*3), .035)
    d.button(False)
    frame('13-resized')
    recomposition()
    passed('Overlapping rounded windows drag back to exact pixels; maximize, restore and live resize match complete recomposition')
    d.exit()
    v.shell_command('test -d /home/user/Projects/ToolbarFolder', as_user=True)
    report['state'] = 'passed'
except BaseException as error:
    report['error'] = repr(error)
    (out/'exception.txt').write_text(traceback.format_exc())
    if d:
        try:
            d.screenshot('failure')
        except Exception:
            pass
    raise
finally:
    if v:
        v.close()
        report['owned_vm_stopped'] = v.process.poll() is not None
    with a.iso.open('rb') as stream:
        report['iso_sha256'] = hashlib.file_digest(stream, 'sha256').hexdigest()
    (out/'report.json').write_text(json.dumps(report, indent=2)+'\n')
