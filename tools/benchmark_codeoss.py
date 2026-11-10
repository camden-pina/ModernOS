#!/usr/bin/env python3
"""Measure real USB-key-to-pixel latency in an isolated Code OSS guest.

A small observer replaces the optional QA module only inside this disposable
guest. It supplies an empty file and reports UI readiness without the expensive
full-workbench DOM snapshots. Both comparison images use this same observer.
"""
import argparse
import base64
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import time
import traceback
from PIL import Image, ImageChops
import test_desktop as desktop
import test_runtime as runtime

OBSERVER = r'''
import {app} from 'electron';
import fs from 'node:fs';
fs.writeFileSync('/home/port-test/speed.txt','',{mode:0o600});
app.on('web-contents-created', (_event, contents) => {
    let last='';
    const timer=setInterval(async () => {
        try {
            const state=await contents.executeJavaScript(`JSON.stringify({ready:document.readyState,fonts:document.fonts.status,
                workbenchReady:document.body?.innerText.includes('Show All Commands'),
                text:[...document.querySelectorAll('.view-lines .view-line')].map(e=>e.textContent).join('\\n'),
                inputs:[...document.querySelectorAll('input')].map(e=>e.value),
                quick:!!document.querySelector('.quick-input-widget:not([style*="display: none"])'),
                files:[...document.querySelectorAll('.quick-input-list .monaco-list-row')].map(e=>e.textContent)})`);
            if(state!==last){last=state;console.error('BENCH_STATE '+state);}
        } catch {}
    },250);
    contents.on('destroyed',()=>clearInterval(timer));
});
'''


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--iso',type=Path,required=True)
    p.add_argument('--output-root',type=Path,required=True)
    p.add_argument('--samples',type=int,default=24)
    a=p.parse_args();iso=a.iso.resolve();out=a.output_root.resolve()
    out.mkdir(parents=True,exist_ok=False);runtime.WORK=desktop.WORK=out
    (out/'observer.mjs').write_text(OBSERVER)
    (out/'HARNESS.py').write_bytes(Path(__file__).read_bytes())
    result=dict(state='failed',iso=str(iso),network=False,extra_disks=False,
                samples=[],key_hold_ms=50,warmup_settle_seconds=10,observer_sha256=hashlib.sha256(OBSERVER.encode()).hexdigest(),harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
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
            s=raw()[start:];m=re.search(pattern,s)
            if m:return m
            if time.monotonic()>=end:raise AssertionError('Missing '+pattern+'\n'+s[-2500:])
            time.sleep(.01)

    def type_text(text):
        names={'/':'slash','.':'dot','-':'minus',' ':'spc'}
        for ch in text:d.key(names.get(ch,ch))

    def screen():
        path=out/'measurement.ppm'
        vm.qmp('screendump',{'filename':str(path)})
        with Image.open(path) as im:return im.crop(crop).copy()

    def key(name):
        def event(down):return {'type':'key','data':{'down':down,'key':{'type':'qcode','data':name}}}
        began=time.monotonic()
        vm.qmp('input-send-event',{'events':[event(True)]})
        time.sleep(.05)
        vm.qmp('input-send-event',{'events':[event(False)]})
        return began

    def cpu_seconds():
        # QEMU is our owned process. Account for its vCPU and device threads
        # over the existing settle interval to catch idle rendering loops.
        fields=Path('/proc/'+str(vm.process.pid)+'/stat').read_text().split(') ',1)[1].split()
        return (int(fields[11])+int(fields[12]))/os.sysconf('SC_CLK_TCK')

    lock=Path(str(iso)+'.lock').open('a');fcntl.flock(lock,fcntl.LOCK_SH|fcntl.LOCK_NB)
    try:
        with iso.open('rb') as f:result['iso_sha256']=hashlib.file_digest(f,'sha256').hexdigest()
        vm=runtime.VM(2,storage=False,network=False,memory='16G',accel='kvm',iso=iso,label='speed')
        result['qemu_pid']=vm.process.pid
        encoded=base64.b64encode(OBSERVER.encode()).decode()
        for i in range(0,len(encoded),192):
            vm.shell_command("printf '%s' '"+encoded[i:i+192]+"' "+('>' if i==0 else '>>')+' /tmp/bench.b64')
        vm.shell_command('/bin/busybox base64 -d /tmp/bench.b64 > /usr/share/codeoss-workbench/out/coast-diagnostics.mjs')
        vm.command('mkdir -p /home/user/.config')
        vm.shell_command("printf 'scale=3\\nterminal_shell=1\\n' > /home/user/.config/desktop.ini")
        vm.send("build-sh -c 'COAST_CODEOSS_QA=1 exec /bin/gui --trace --stats'\n")
        vm.until(rb'GUI_READY:',60);d=desktop.DesktopTest(vm);d.screenshot('00-desktop')
        start=len(raw());began=time.monotonic();ident=d.launch('Code OSS')
        wait('ELECTRON_CLIENT_PIXELS_PRESENTED',start,90)
        result['first_pixels_ms']=(time.monotonic()-began)*1000
        wait(r'BENCH_STATE[^\n]*"ready":"complete"[^\n]*"fonts":"loaded"[^\n]*"workbenchReady":true',start,90)
        time.sleep(3)
        start=len(raw());d.key('p','ctrl');type_text('/home/port-test/speed.txt')
        # A typed path precedes the asynchronous provider result. Accept the
        # actual file row before Enter, so startup load cannot leave the test
        # typing into an unresolved Quick Open widget.
        wait(r'BENCH_STATE[^\n]*"quick":true,"files":\["speed[.]txt/home/port-testfile results"\]',start)
        d.key('ret');time.sleep(1)
        view=re.findall(r'DESKTOP GAME_VIEW window='+str(ident)+r' x=(\d+) y=(\d+) width=(\d+) height=(\d+)[^\n]*native=1',raw())[-1]
        x,y,w,h=map(int,view);crop=(x+w//4,y+40,x+w-40,y+min(h-50,560));result['pixel_crop']=crop
        # Warm up the actual editor and font caches before measuring.
        for _ in range(4):key('m');time.sleep(.15)
        idle_cpu=cpu_seconds();idle_started=time.monotonic();time.sleep(10)
        result['idle_qemu_cpu_percent']=100*(cpu_seconds()-idle_cpu)/(time.monotonic()-idle_started)
        d.screenshot('01-before-measurement')
        text='mmmm'
        for index in range(a.samples):
            before=screen();began=time.monotonic();deadline=began+5;observed=None
            # Observe while the physical key is held: waiting for its release
            # first would impose an artificial 50-ms floor on every sample.
            def event(down):return {'type':'key','data':{'down':down,'key':{'type':'qcode','data':'m'}}}
            vm.qmp('input-send-event',{'events':[event(True)]});released=False
            while time.monotonic()<deadline:
                if not released and time.monotonic()-began>=.05:
                    vm.qmp('input-send-event',{'events':[event(False)]});released=True
                current=screen();box=ImageChops.difference(before,current).getbbox()
                # A blinking caret changes a narrow vertical strip. Require
                # actual glyph-width pixel changes instead of that idle blink.
                if box and box[2]-box[0]>=8 and box[3]-box[1]>=10:
                    observed=(time.monotonic()-began)*1000;break
                raw();time.sleep(.005)
            if not released:
                time.sleep(max(0,.05-(time.monotonic()-began)))
                vm.qmp('input-send-event',{'events':[event(False)]})
            assert observed is not None,'No visible glyph update for sample '+str(index)
            result['samples'].append(observed);text+='m';time.sleep(.04)
        d.key('s','ctrl');time.sleep(.4);d.screenshot('02-typed')
        start=len(raw());d.key('f4','alt')
        wait(r'C1_RUNTIME_WAIT status=0 exit=0 signal=0 timeout=0',start)
        wait(r'DESKTOP GAME_EXIT pid=\d+ status=0 reaped=1',start)
        d.exit()
        encoded=vm.shell_command('/bin/busybox base64 /home/port-test/speed.txt')
        result['saved_file']=base64.b64decode(''.join(encoded.split()),validate=True).decode()
        assert text==result['saved_file'],'Every injected key must be saved exactly, without repeats'
        values=sorted(result['samples'])
        result.update(state='passed real USB typing/pixels and exact saved-file benchmark',
                      median_ms=statistics.median(values),mean_ms=statistics.mean(values),
                      p95_ms=values[min(len(values)-1,int(len(values)*.95))],maximum_ms=max(values))
        s=raw();faults=re.findall(r'ELECTRON_USER_FAULT[^\n]*',s)
        assert all(re.search(r'pending_signal=(?:9|15)\b',fault) for fault in faults),'An Electron process faulted without pending termination'
        result['terminating_fault_receipts']=faults
        result['gui_stats']=re.findall(r'GUI_[A-Z_]+[^\n]*',s)[-12:]
        result['native_timing']=re.findall(r'NATIVE_TIMING_MAX[^\n]*',s)
        result['native_damage']=re.findall(r'(?:NATIVE_COPY_STATS|DESKTOP NATIVE_DAMAGE)[^\n]*',s)
        print(json.dumps({k:result[k] for k in ['state','first_pixels_ms','median_ms','p95_ms','maximum_ms','gui_stats']},indent=2),flush=True)
    except BaseException as error:
        result['error']=repr(error);(out/'exception.txt').write_text(traceback.format_exc())
        if d:
            try:d.screenshot('failure')
            except Exception:pass
        raise
    finally:
        if vm:
            raw();vm.close();result['owned_vm_stopped']=vm.process.poll() is not None
        with iso.open('rb') as stream:result['image_unchanged']=hashlib.file_digest(stream,'sha256').hexdigest()==result['iso_sha256']
        (out/'report.json').write_text(json.dumps(result,indent=2)+'\n');lock.close()


if __name__=='__main__':main()
