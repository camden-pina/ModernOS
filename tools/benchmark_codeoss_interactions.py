#!/usr/bin/env python3
"""Measure real cursor updates during Code OSS startup and continuous resize.

Runs one owned, diskless/networkless QEMU guest. Screenshot/QMP overhead is
included; measurements are comparable only with this same harness and host.
"""
import argparse, fcntl, hashlib, json, os, re, statistics, time, traceback
from pathlib import Path
from PIL import Image, ImageChops
import test_runtime as runtime
import test_desktop as desktop


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--iso',type=Path,required=True)
    p.add_argument('--output-root',type=Path,required=True)
    p.add_argument('--check-changes',action='store_true')
    a=p.parse_args();iso=a.iso.resolve();out=a.output_root.resolve()
    out.mkdir(parents=True,exist_ok=False);runtime.WORK=desktop.WORK=out
    (out/'HARNESS.py').write_bytes(Path(__file__).read_bytes())
    report=dict(state='failed',iso=str(iso),network=False,extra_disks=False,
        cpus=2,memory='16G',startup_cursor_ms=[],checks=[],
        harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    vm=d=None
    def raw():
        while vm.selector.select(.001):
            b=os.read(vm.process.stdout.fileno(),65536)
            if not b:break
            vm.pending.extend(b);vm.log.write(b);vm.log.flush()
        return Path(vm.log.name).read_text(errors='replace')
    def wait(pattern,start=0,timeout=60):
        end=time.monotonic()+timeout
        while True:
            source=raw()[start:];m=re.search(pattern,source)
            if m:return m
            if time.monotonic()>end:raise AssertionError('Missing '+pattern+'\n'+source[-2500:])
            time.sleep(.01)
    def screen():
        path=out/'measurement.ppm';vm.qmp('screendump',{'filename':str(path)})
        with Image.open(path) as im:return im.copy()
    def passed(text):
        report['checks'].append(text);print('PASS '+text,flush=True)
    def geometry():
        raw()  # Geometry must reflect input just processed by the guest.
        return d.geometry(ident)
    def drag(x,y,dx,dy,steps=72):
        start=len(raw())
        d.move(x,y);d.button(True)
        press=wait(r'DESKTOP POINTER position=\((\d+),(\d+)\) buttons=1 pressed=1',start)
        x,y=d.x,d.y=int(press[1]),int(press[2])
        for i in range(1,steps+1):
            d.move(max(0,min(d.width-1,x+round(dx*i/steps))),max(0,min(d.height-1,y+round(dy*i/steps))),delay=.016)
            raw()
        d.button(False);time.sleep(.25);raw()
        source=raw()[start:]
        press=re.findall(r'DESKTOP POINTER position=\((\d+),(\d+)\) buttons=1 pressed=1',source)[-1]
        release=re.findall(r'DESKTOP POINTER position=\((\d+),(\d+)\) buttons=0 pressed=0 released=1',source)[-1]
        actual=[int(release[i])-int(press[i]) for i in range(2)]
        d.x,d.y=map(int,release)
        report.setdefault('drags',[]).append(dict(requested=[dx,dy],press=press,release=release,actual=actual))
        return actual
    def recomposition():
        start=len(raw());d.key('f12','ctrl','alt')
        m=wait(r'DESKTOP RECOMPOSE_CHECK error=0 pixels=\d+ mismatches=(\d+)',start)
        assert m[1]=='0',m[0]
        passed('Incremental pixels equal full recomposition on the same retained frame')
    with Path(str(iso)+'.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_SH|fcntl.LOCK_NB)
        with iso.open('rb') as f:report['iso_sha256']=hashlib.file_digest(f,'sha256').hexdigest()
        try:
            vm=runtime.VM(2,storage=False,network=False,memory='16G',accel='kvm',iso=iso,label='interactions')
            vm.command('mkdir -p /home/user/.config')
            vm.shell_command("printf 'scale=3\\nterminal_shell=1\\n' > /home/user/.config/desktop.ini")
            vm.send('gui --trace --stats\n');vm.until(rb'GUI_READY:',60)
            d=desktop.DesktopTest(vm);d.screenshot('00-desktop')
            report['framebuffer']=[d.width,d.height]
            # Both targets are wallpaper, outside the initial Files/editor.
            # Capture their background while the cursor is elsewhere.
            background=screen();targets=[(8,220),(42,220)]
            started=time.monotonic();start=len(raw());ident=d.launch('Code OSS')
            first=None;end=started+12;index=0
            while time.monotonic()<end:
                x,y=targets[index%2];area=(x,y,x+32,y+48)
                began=time.monotonic();d.move(x,y,delay=0)
                deadline=began+5
                while time.monotonic()<deadline:
                    current=screen()
                    diff=ImageChops.difference(background.crop(area),current.crop(area)).getbbox()
                    if diff and diff[2]-diff[0]>=8 and diff[3]-diff[1]>=12:break
                    raw();time.sleep(.004)
                else:raise AssertionError('Cursor did not reach its requested wallpaper position')
                report['startup_cursor_ms'].append((time.monotonic()-began)*1000)
                if first is None and 'ELECTRON_CLIENT_PIXELS_PRESENTED' in raw()[start:]:
                    first=(time.monotonic()-started)*1000
                index+=1;time.sleep(.04)
            if first is None:wait('ELECTRON_CLIENT_PIXELS_PRESENTED',start);first=(time.monotonic()-started)*1000
            report['first_pixels_observed_ms']=first
            values=sorted(report['startup_cursor_ms'])
            report['cursor_median_ms']=statistics.median(values)
            report['cursor_p95_ms']=values[min(len(values)-1,int(.95*len(values)))]
            report['cursor_max_ms']=max(values)
            passed('Cursor reaches real screenshot positions while Electron loads')
            time.sleep(4)
            # Older desktops did not trace the initial native surface fit.
            # Maximize/restore emits authoritative outer geometry in both.
            d.key('up','meta_l');time.sleep(.6)
            d.key('down','meta_l');time.sleep(.6)
            g=geometry();report['initial_geometry']=g
            d.screenshot('01-editor')
            # Shrink and grow the actual native border with 72 USB samples.
            for dx,dy in [(-360,-180),(360,180)]:
                g=geometry()
                actual=drag(g['x']+g['width']-2,g['y']+g['height']-2,dx,dy)
                resized=geometry()
                assert resized['width']==g['width']+actual[0] and resized['height']==g['height']+actual[1],(g,resized,actual)
                time.sleep(.6)
            report['resized_geometry']=geometry()
            d.screenshot('02-resized');recomposition()
            if a.check_changes:
                g=geometry();assert g['body']==56,g
                passed('Compact title bar is 28 logical pixels at 200 percent scale')
                # Move off the left and right edges, then recover using the
                # remaining reachable title bar. Move to the Dock boundary.
                drag(g['x']+200,g['y']+g['body']//2,-d.width,0,40)
                left=geometry();assert left['x']<0,left
                recomposition();d.screenshot('03-offscreen-left')
                drag(left['x']+left['width']-2,left['y']+left['height']-2,-120,-60,30)
                left=geometry();assert left['x']<0,left
                recomposition()
                drag(100,left['y']+left['body']//2,d.width-200,0,40)
                right=geometry();assert right['x']+right['width']>d.width,right
                recomposition();d.screenshot('04-offscreen-right')
                drag(d.width-24,right['y']+right['body']//2,-right['x']+400,0,40)
                g=geometry()
                assert abs(g['x']-400)<12,g
                drag(g['x']+200,g['y']+g['body']//2,0,d.height,40)
                bottom=geometry()
                assert bottom['y']>g['y'],(g,bottom)
                assert bottom['y']+bottom['height']<=d.height-100,bottom
                recomposition();d.screenshot('05-dock-boundary')
                passed('Windows move off both side edges, remain recoverable, and stop above the Dock')
                # Native borders expose all four resize directions.
                g=bottom
                points=[(g['x']+1,g['y']+1),(g['x']+g['width']-2,g['y']+1),
                    (g['x']+1,g['y']+g['height']//2),(g['x']+g['width']//2,g['y']+1),
                    (g['x']+1,g['y']+g['height']-2),
                    (g['x']+g['width']//2,g['y']+g['body']//2)]
                for x,y in points:d.move(min(d.width-2,x),y,delay=.2);raw()
                kinds=set(map(int,re.findall(r'DESKTOP CURSOR kind=(\d+)',raw())))
                assert {3,4,5,6,7,8}.issubset(kinds),kinds
                report['cursor_kinds']=sorted(kinds)
                passed('Move, grabbing, horizontal, vertical and both diagonal resize cursors follow hit testing')
                d.move(65,220);time.sleep(.2);recomposition()
            start=len(raw());d.key('f4','alt')
            wait(r'C1_RUNTIME_WAIT status=0 exit=0 signal=0 timeout=0',start)
            wait(r'DESKTOP GAME_EXIT pid=\d+ status=0 reaped=1',start)
            if a.check_changes:
                editor=d.launch('Text Editor');raw();g=d.geometry(editor)
                d.move(g['x']+g['width']//2,g['y']+g['height']//2,delay=.2)
                assert re.search(r'DESKTOP CURSOR kind=1\b',raw()),'Text area must select the I-beam cursor'
                passed('Native Text Editor text area selects the I-beam cursor')
                d.key('f4','alt');raw()
            d.exit();source=raw()
            report['gui_stats']=re.findall(r'GUI_[A-Z_]+[^\n]*',source)
            report['native_configures']=len(re.findall('DESKTOP NATIVE_CONFIGURE',source))
            report['faults']=re.findall(r'ELECTRON_USER_FAULT[^\n]*',source)
            assert all(re.search(r'pending_signal=(?:9|15)\b',fault) for fault in report['faults'])
            report['state']='passed real startup cursor, resize and desktop interaction checks'
            print(json.dumps({key:report[key] for key in ['state','first_pixels_observed_ms','cursor_median_ms','cursor_p95_ms','cursor_max_ms','gui_stats']},indent=2),flush=True)
        except BaseException as error:
            report['error']=repr(error);(out/'exception.txt').write_text(traceback.format_exc())
            if d:
                try:d.screenshot('failure')
                except Exception:pass
            raise
        finally:
            if vm:raw();vm.close();report['owned_vm_stopped']=vm.process.poll() is not None
            with iso.open('rb') as f:report['image_unchanged']=hashlib.file_digest(f,'sha256').hexdigest()==report['iso_sha256']
            (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')

if __name__=='__main__':main()
