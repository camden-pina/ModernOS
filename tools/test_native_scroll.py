#!/usr/bin/env python3
"""Measure genuine animated wheel scrolling in the UEFI desktop and WebCore."""
import argparse
import re
import shlex
import time
from pathlib import Path
from test_runtime import VM, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture


def wheel(vm, down, count=4):
    button = 'wheel-down' if down else 'wheel-up'
    for _ in range(count):
        vm.qmp('input-send-event', {'events': [
            {'type': 'btn', 'data': {'button': button, 'down': True}},
            {'type': 'btn', 'data': {'button': button, 'down': False}}]})


def exercise(cpus, iso, accel):
    fixture = WebFixture(webcore=True)
    fixture.assets['/smooth-wheel'] = b'''<!doctype html><meta charset=utf-8>
<title>Smooth wheel ready</title><style>
body{margin:0;height:3000px;background:white}
#marker{position:absolute;left:20px;top:500px;width:60px;height:120px;background:#22cc44}
#inner{position:fixed;left:120px;top:150px;width:180px;height:160px;overflow:auto;background:#00ccee}
#inside{position:relative;height:1500px}
#stripe{position:absolute;left:0;top:500px;width:150px;height:60px;background:#ee7722}
button{position:fixed;left:400px;top:30px;width:120px;height:35px}
</style><div id=marker></div><div id=inner><div id=inside><div id=stripe></div></div></div>
<button id=cancel>Cancel wheel</button><script>
let mode='Root';
onscroll=()=>document.title='Smooth '+mode+' '+Math.round(scrollY);
inner.onscroll=()=>document.title='Smooth Inner '+Math.round(inner.scrollTop);
cancel.onclick=()=>{mode='Cancel';addEventListener('wheel',e=>e.preventDefault(),{passive:false});document.title='Smooth cancel armed';};
</script>''', 'text/html; charset=utf-8'
    vm = None
    try:
        vm = VM(cpus, storage=False, iso=iso, label=f'native-scroll-{accel}', accel=accel)

        # Fixtures use ordinary guest file operations, not production demo rows.
        script = ('mkdir -p /home/user/Scroll; '
                  'i=0; while test "$i" -lt 100; do '
                  'printf "file %s\\n" "$i" > /home/user/Scroll/item-$i.txt; i=$((i+1)); done; '
                  'i=0; while test "$i" -lt 180; do '
                  'printf "document line %s\\n" "$i"; i=$((i+1)); done > /home/user/Scroll/document.txt')
        assert_success(vm.command('build-sh -c ' + shlex.quote(script), timeout=35))
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do '
            'sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        vm.send('gui --trace --stats\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm);
        d.screenshot(f'native-scroll-start-{accel}-{cpus}cpu')
        log_path = Path(vm.log.name)

        def geometry(window, files=False):
            ww = d.width * 78 // 100 if files else 840
            wh = d.height * 788 // 1000 if files else 560
            offset = (window - 1) % 5 * 16
            left = (d.width - ww) // 2 + offset
            top = d.height * 8 // 100 + offset if files else (d.height - 42 - wh) // 2 + offset
            return left, top, ww, wh

        def start_trace():
            vm.log.flush();
            return log_path.stat().st_size

        def native_gesture(window, app, down, target):
            start = start_trace();
            before = d.screenshot(f'native-scroll-{app}-before-{accel}-{cpus}cpu')
            wheel(vm, down)
            vm.until(rb'DESKTOP SCROLL window=' + str(window).encode() + rb' workspace=\d+ ' + app.encode() +
                     rb' position=' + str(target).encode() + rb' target=' + str(target).encode() + rb' time=\d+\r?\n', 8)
            vm.log.flush();
            trace = log_path.read_bytes()[start:]
            points = [(int(p), int(t)) for p, t in re.findall(
                rb'DESKTOP SCROLL window=' + str(window).encode() + rb' workspace=\d+ ' + app.encode() +
                rb' position=(\d+) target=\d+ time=(\d+)', trace)]
            assert len({p for p, _ in points}) >= 6, (app, 'too few intermediate positions', points)
            positions = [p for p, _ in points]
            assert all(a <= b for a, b in zip(positions, positions[1:])), (app, 'reversed animation', positions)
            assert points[-1][0] == target and 0 <= points[-1][1] - points[0][1] < 1000, (app, points)
            after = d.screenshot(f'native-scroll-{app}-after-{accel}-{cpus}cpu')
            assert sum(before[i:i + 3] != after[i:i + 3]
                       for i in range(0, len(before), 12)) > 300, (app, 'content did not move')

            print(f'PASS {app}: {len(set(positions))} real intermediate positions, '
                  f'{points[-1][1]-points[0][1]} ms settling ({cpus} CPUs, {accel})', flush=True)

        files = d.launch('Files');
        d.path('/home/user/Scroll')
        left, top, ww, wh = geometry(files, True);
        d.move(left + ww // 2, top + wh // 2)
        native_gesture(files, 'Files', True, 216)

        # A document opens in the normal editor using Files selection.
        d.find('document.txt');
        d.key('ret')
        output = d.expect('OPEN', 'document.txt')
        editor = int(re.findall(r'DESKTOP OPEN window=(\d+)', output)[-1])
        left, top, _, _ = geometry(editor);
        d.move(left + 400, top + 270)
        native_gesture(editor, 'Editor', True, 240)
        d.key('f4', 'alt');
        d.expect('CLOSE')

        terminal = d.launch('Terminal');
        vm.send('seq 1 180\n')
        d.expect('TERMINAL_COMMAND', 'seq 1 180');
        time.sleep(.5)
        left, top, _, _ = geometry(terminal);
        d.move(left + 400, top + 270)
        native_gesture(terminal, 'Terminal', False, 204)
        d.key('f4', 'alt');
        d.expect('CLOSE')

        browser = d.launch('Browser');
        left, top, _, _ = geometry(browser);
        body = top + 40
        d.key('l', 'ctrl');
        vm.send(fixture.url('smooth-wheel') + '\n')
        d.expect('BROWSER_TITLE', 'Smooth wheel ready')

        def browser_gesture(name, down, target):
            start = start_trace();
            wheel(vm, down)
            vm.until(rb'DESKTOP BROWSER_TITLE window=' + str(browser).encode() + rb' workspace=\d+ Smooth ' +
                     name.encode() + b' ' + str(target).encode() + rb'\r?\n', 8)
            vm.log.flush();
            trace = log_path.read_bytes()[start:]
            positions = [int(p) for p in re.findall(rb'DESKTOP BROWSER_TITLE window=' + str(browser).encode() +
                                                    rb' workspace=\d+ Smooth ' + name.encode() + rb' (\d+)\r?\n', trace)]
            assert len(set(positions)) >= 3, (name, 'wheel jumped instead of animating', positions)
            assert all(a <= b if down else a >= b for a, b in zip(
                positions, positions[1:])), (name, positions)

            print(
                f'PASS WebCore {name}: {len(set(positions))} actual DOM scroll positions ({cpus} CPUs, {accel})', flush=True)

        def pixel(x, y, expected):
            image = d.screenshot(f'native-scroll-browser-{accel}-{cpus}cpu')
            at = ((body + y) * d.width + left + x) * 3
            assert tuple(image[at:at + 3]) == expected, (x, y, expected, tuple(image[at:at + 3]))

        d.move(left + 50, body + 350);
        browser_gesture('Root', True, 400);
        pixel(40, 120, (34, 204, 68))
        browser_gesture('Root', False, 0);
        pixel(40, 120, (255, 255, 255))
        d.move(left + 160, body + 220);
        browser_gesture('Inner', True, 400);
        pixel(140, 270, (238, 119, 34))
        pixel(40, 120, (255, 255, 255))
        d.click(left + 460, body + 45);
        d.expect('BROWSER_TITLE', 'Smooth cancel armed')
        d.move(left + 50, body + 350);
        start = start_trace();
        wheel(vm, True);
        time.sleep(.6)
        pixel(40, 120, (255, 255, 255));
        vm.log.flush()

        assert b'Smooth Cancel ' not in log_path.read_bytes()[start:], 'preventDefault wheel scrolled the document'
        d.key('f4', 'alt');
        d.expect('CLOSE');
        d.exit()

        assert not re.search(r'\bweb-(?:broker|renderer)\b', vm.command('ps'))

        print(
            f'PASS animated native and browser scrolling, nested target, reversal, real pixels, wheel cancellation and cleanup ({cpus} CPUs, {accel})', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True)
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    args = parser.parse_args()
    for cpus in args.cpus:
        exercise(cpus, args.iso, args.accel)
