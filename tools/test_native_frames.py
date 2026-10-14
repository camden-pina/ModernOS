#!/usr/bin/env python3
"""Exercise real isolated WebCore frames, structured clones and input in UEFI."""
import argparse
import re
import shlex
import time
from test_runtime import VM, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture


def exercise(cpus, iso, accel):
    fixture = WebFixture(webcore=True)
    vm = None
    child_origin = fixture.url('', tls=True).rstrip('/')
    parent_origin = fixture.url('').rstrip('/')
    child = '''<!doctype html><meta charset=utf-8><style>
body{margin:0;background:#00ccff}button{margin:5px;width:70px;height:32px;background:#ee2244}
</style><button id=b>Verify</button><script>
let ready=false,display;
try{parent.document.body;throw Error('foreign parent DOM exposed');}
catch(e){if(e.name!=='SecurityError')throw e;}
onmessage=e=>{
 if(e.source!==parent || e.origin!==PARENT)return;
 if(e.data.kind==='ping'){
  if(e.data.bytes[1]!==17 || e.data.map.get('answer')!==42)throw Error('structured clone');
  if(screen.width!==e.data.display.width || screen.height!==e.data.display.height ||
     screen.availHeight!==e.data.display.available || screen.width<=innerWidth || screen.height<=innerHeight)
    throw Error('isolated frame screen metadata');
  display=e.data.display;
  if(outerWidth!==display.outerWidth || outerHeight!==display.outerHeight || screenX!==display.x || screenY!==display.y)
    throw Error('isolated frame outer window geometry');
  ready=true;parent.postMessage({kind:'ready'},PARENT);
 }else if(e.data.kind==='wrong')parent.postMessage({kind:'wrong-delivered'},PARENT);
};
b.onclick=e=>{if(ready){
 if(!e.isTrusted || Math.abs(e.screenX-(display.x+55))>1 || Math.abs(e.screenY-(display.y+120))>1)
   throw Error('isolated pointer screen coordinates');
 document.body.style.background='#22cc44';parent.postMessage({kind:'clicked'},PARENT);}};
setInterval(()=>parent.postMessage({kind:'init'},PARENT),250);
</script>'''.replace('PARENT', repr(parent_origin))
    parent = '''<!doctype html><meta charset=utf-8><title>Frame loading</title><style>
body{margin:0;background:#ffffff}iframe{position:absolute;left:20px;top:60px;width:180px;height:120px;border:0}
button{position:absolute;top:220px;width:90px;height:35px}
</style><iframe id=f src=CHILD></iframe><button id=resize style="left:20px">Resize</button>
<button id=remove style="left:120px">Remove</button><script>
let loaded=false,ready=false,transition=false,wasm=false;
const display={width:screen.width,height:screen.height,available:screen.availHeight,
 outerWidth,outerHeight,x:screenX,y:screenY};
if(display.width!==SCREEN_WIDTH || display.height!==SCREEN_HEIGHT || display.available!==SCREEN_AVAILABLE)
 throw Error('actual desktop screen metadata');
if(display.outerWidth!==840 || display.outerHeight!==560 || display.x!==WINDOW_X || display.y!==WINDOW_Y)
 throw Error('actual desktop window geometry');
f.onload=()=>{loaded=true;finish();};
function finish(){if(loaded&&ready&&transition&&wasm)document.title='Isolated frame ready';}
onmessage=e=>{
 if(e.source!==f.contentWindow || e.origin!==ORIGIN)throw Error('wrong message identity');
 if(e.data.kind==='init'&&!ready){
  try{f.contentWindow.document;throw Error('foreign child DOM exposed');}
  catch(x){if(x.name!=='SecurityError')throw x;}
  let channel=new MessageChannel(),rejected=false;
  try{f.contentWindow.postMessage('transfer',ORIGIN,[channel.port1]);}catch(x){rejected=x.name==='DataCloneError';}
  if(!rejected)throw Error('unsupported port transfer was accepted');
  channel.port1.close();channel.port2.close();
  f.contentWindow.postMessage({kind:'wrong'},'https://wrong.invalid');
  f.contentWindow.postMessage({kind:'ping',display,bytes:new Uint8Array([9,17]),map:new Map([['answer',42]])},ORIGIN);
 }else if(e.data.kind==='ready'){ready=true;finish();}
 else if(e.data.kind==='clicked')document.title='Isolated frame clicked';
 else if(e.data.kind==='wrong-delivered')throw Error('targetOrigin was ignored');
};
resize.onclick=()=>{f.style.width='260px';f.style.height='160px';document.title='Isolated frame resized';};
remove.onclick=()=>{f.remove();document.title='Isolated frame removed';};
(async()=>{
 let changed=false,t=document.startViewTransition(()=>{changed=true;});
 let rejected=false;await t.ready.catch(e=>{rejected=e.name==='NotSupportedError';});
 await t.updateCallbackDone;await t.finished;
 if(!changed||!rejected)throw Error('CPU view transition promises');
 transition=true;finish();
})();
(async()=>{
 const result=await WebAssembly.instantiateStreaming(fetch('/wasm-add.wasm'));
 if(result.instance.exports.add(17,25)!==42)throw Error('Wasm interpreter arithmetic');
 const imported=await WebAssembly.instantiate(new Uint8Array([0,97,115,109,1,0,0,0,
  1,6,1,96,1,127,1,127,2,11,1,3,101,110,118,3,105,110,99,0,0,3,2,1,0,
  7,10,1,6,100,111,117,98,108,101,0,1,10,11,1,9,0,32,0,16,0,65,2,108,11]),{env:{inc:n=>n+1}});
 if(imported.instance.exports.double(9)!==20)throw Error('Wasm/JavaScript call bridge');
 const memory=new WebAssembly.Memory({initial:1,maximum:3}),old=new Uint8Array(memory.buffer);old[65535]=165;
 if(memory.grow(1)!==1||old.byteLength||memory.buffer.byteLength!==131072||new Uint8Array(memory.buffer)[65535]!==165)throw Error('Wasm memory growth');
 let bounded=false;try{memory.grow(3);}catch(e){bounded=e instanceof RangeError;}
 if(!bounded||WebAssembly.validate(new Uint8Array([0,1,2])))throw Error('Wasm bounds/validation');
 const blob=URL.createObjectURL(new Blob(['onmessage=e=>postMessage({answer:e.data+1});'],{type:'text/javascript'}));
 if(!(await fetch(blob).then(r=>r.text())).includes('postMessage'))throw Error('local blob resource');
 await new Promise((resolve,reject)=>{
  const worker=new Worker(blob),timeout=setTimeout(()=>{worker.terminate();reject(Error('blob worker timed out'));},4000);
  worker.onerror=e=>{clearTimeout(timeout);worker.terminate();reject(Error('blob worker '+e.message));};
  worker.onmessage=e=>{clearTimeout(timeout);worker.terminate();e.data.answer===42?resolve():reject(Error('worker clone result'));};
  worker.postMessage(41);
 });
 URL.revokeObjectURL(blob);
 wasm=true;finish();
})().catch(e=>{document.title='Wasm failed '+e;console.error(e);});
</script>'''.replace('CHILD', repr(fixture.url('isolated-child', tls=True))).replace('ORIGIN', repr(child_origin))
    fixture.assets['/isolated-parent'] = parent.encode(), 'text/html; charset=utf-8'
    fixture.assets['/isolated-child'] = child.encode(), 'text/html; charset=utf-8'
    fixture.assets['/wasm-add.wasm'] = bytes([0, 97, 115, 109, 1, 0, 0, 0, 1, 7, 1, 96, 2, 127, 127, 1, 127, 3, 2, 1, 0,
                                              7, 7, 1, 3, 97, 100, 100, 0, 0, 10, 9, 1, 7, 0, 32, 0, 32, 1, 106, 11]), 'application/wasm'
    try:
        vm = VM(cpus, storage=False, iso=iso, label=f'native-frames-{accel}', accel=accel)
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        assert_success(vm.command(
            'build-sh -c ' + shlex.quote(f'curl -fsS --max-time 30 {fixture.url("ca.pem")} >> /etc/ssl/cert.pem'), timeout=40))
        vm.send('gui --trace --stats\n')
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm)
        d.screenshot(f'native-frames-start-{accel}-{cpus}cpu')

        window = d.launch('Browser')
        left = (d.width - 840) // 2 + (window - 1) % 5 * 16
        body = (d.height - 42 - 560) // 2 + (window - 1) % 5 * 16 + 40
        fixture.assets['/isolated-parent'] = parent.replace('SCREEN_WIDTH', str(d.width)).replace(
            'SCREEN_HEIGHT', str(d.height)).replace('SCREEN_AVAILABLE', str(d.height - 42)).replace(
            'WINDOW_X', str(left)).replace('WINDOW_Y', str(body - 40)).encode(), 'text/html; charset=utf-8'
        d.key('l', 'ctrl')
        vm.send(fixture.url('isolated-parent') + '\n')

        try:
            d.expect('BROWSER_TITLE', 'Isolated frame ready')
        except AssertionError:
            d.screenshot(f'native-frames-failed-{accel}-{cpus}cpu')
            raise

        def pixel(x, y, expected):
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                shot = d.screenshot(f'native-frames-{accel}-{cpus}cpu')
                offset = ((body + y) * d.width + left + x) * 3
                if tuple(shot[offset:offset + 3]) == expected:
                    return
                time.sleep(.05)
            raise AssertionError((x, y, expected, tuple(shot[offset:offset + 3])))

        pixel(100, 140, (0, 204, 255))
        pixel(210, 140, (255, 255, 255))
        d.click(left + 55, body + 80)
        d.expect('BROWSER_TITLE', 'Isolated frame clicked')
        pixel(100, 140, (34, 204, 68))
        d.click(left + 55, body + 235)
        d.expect('BROWSER_TITLE', 'Isolated frame resized')
        pixel(250, 200, (34, 204, 68))
        d.key('r', 'ctrl')
        d.expect('BROWSER_TITLE', 'Isolated frame ready')
        pixel(100, 140, (0, 204, 255))
        d.click(left + 160, body + 235)
        d.expect('BROWSER_TITLE', 'Isolated frame removed')
        pixel(100, 140, (255, 255, 255))
        d.key('f4', 'alt')
        d.expect('CLOSE', f'window={window}')
        d.exit()

        assert not re.search(r'\bweb-(?:broker|renderer)\b', vm.command('ps'))

        print(
            f'PASS isolated frames: actual display/window/pointer geometry, TLS child pixels, SOP, event.source/origin, structured clones, targetOrigin/transfer rejection, click/resize/reload/cleanup, CPU transitions, actual streamed Wasm arithmetic/imports/memory/validation and blob workers ({accel}, {cpus} CPUs)', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True)
    parser.add_argument('--cpus', type=int, nargs='+', default=[1])
    parser.add_argument('--accel', choices=['kvm', 'tcg'], default='kvm')
    args = parser.parse_args()
    for cpus in args.cpus:
        exercise(cpus, args.iso, args.accel)
