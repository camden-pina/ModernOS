#!/usr/bin/env python3
"""Check native console output does not execute page object coercion/getters."""
import argparse
from pathlib import Path
import shlex
import time
from test_runtime import VM, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture


PROBE = '''
function consoleProbe() {
 let calls=0;
 const object={toString(){++calls;return 'CONSOLE_COERCION_PRIVATE';},
  [Symbol.toPrimitive](){++calls;return 'CONSOLE_COERCION_PRIVATE';}};
 const error=new Error();
 for(const name of ['name','message','stack'])
  Object.defineProperty(error,name,{get(){++calls;return 'CONSOLE_COERCION_PRIVATE';}});
 const proxy=new Proxy({}, {get(){++calls;throw Error('console proxy trap');},
  getPrototypeOf(){++calls;throw Error('console prototype trap');}});
 const revoked=Proxy.revocable({},{});revoked.revoke();
 const array=[object];
 for(const method of ['log','info','warn','error','debug','dir','table','trace']) {
  console[method](object);
  console[method]('CONSOLE_PRIMITIVES',42,true,null,undefined,17n,object,error,proxy,revoked.proxy,array);
 }
 console.assert(false,'CONSOLE_ASSERT',object,error);
 console.log('CONSOLE_ERROR',new TypeError('console safe error'));
 return calls;
}
'''


def exercise(iso, cpus, accel, label):
    fixture = WebFixture(webcore=True)
    vm = None
    try:
        child_origin = fixture.url('', tls=True).rstrip('/')
        fixture.assets['/console-child'] = ((
            '<!doctype html><script>' + PROBE +
            'parent.postMessage({consoleCalls:consoleProbe()},"*");</script>').encode(), 'text/html')
        fixture.assets['/console-main'] = ((
            '<!doctype html><title>Console loading</title><body style="margin:0;background:#dd2233"><script>' + PROBE +
            'const calls=consoleProbe();let childDone=false,timerDone=false,workerDone=false;'
            'const workerURL=URL.createObjectURL(new Blob([' + repr(
                PROBE + 'postMessage(consoleProbe());') + '],{type:"text/javascript"}));'
            'const worker=new Worker(workerURL);worker.onmessage=e=>{'
            'if(e.data){document.title="Console failed worker coercion "+e.data;return;}'
            'worker.terminate();URL.revokeObjectURL(workerURL);workerDone=true;finish();};'
            'worker.onerror=e=>{document.title="Console failed worker "+e.message;};'
            'function finish(){if(!childDone||!timerDone||!workerDone)return;'
            'document.body.style.background="#11aa33";document.title="Console ready";}'
            'onmessage=e=>{if(e.origin!==' + repr(child_origin) + ')return;'
            'if(calls||e.data.consoleCalls){document.title="Console failed coercion "+calls+"/"+e.data.consoleCalls;return;}'
            'childDone=true;finish();};setTimeout(()=>{timerDone=true;finish();},50);'
            'onload=()=>{const f=document.createElement("iframe");f.src=' +
            repr(child_origin + '/console-child') + ';document.body.appendChild(f);};</script>').encode(), 'text/html')

        vm = VM(cpus, storage=False, iso=iso, label=label, accel=accel)
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do '
            'sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            f'curl -fsS --max-time 30 {fixture.url("ca.pem")} >> /etc/ssl/cert.pem')))
        vm.send('gui --trace --stats\n');
        vm.until(b'GUI_READY', 40)

        desktop = DesktopTest(vm);
        desktop.screenshot(label + '-start');

        window = desktop.launch('Browser')
        desktop.key('l', 'ctrl');
        vm.send(fixture.url('console-main') + '\n')

        try:
            desktop.expect('BROWSER_TITLE', 'Console ready')
        except AssertionError:
            desktop.screenshot(label + '-failed');
            raise

        left = (desktop.width - 840) // 2 + (window - 1) % 5 * 16
        top = (desktop.height - 42 - 560) // 2 + (window - 1) % 5 * 16 + 40

        # The child covers the upper-left corner; check the parent outside it.
        at = ((top + 200) * desktop.width + left + 400) * 3
        deadline = time.monotonic() + 5
        while True:
            pixels = desktop.screenshot(label + '-ready')
            if pixels[at:at + 3] == bytes((17, 170, 51)) or time.monotonic() >= deadline:
                break
            time.sleep(.05)

        assert pixels[at:at + 3] == bytes((17, 170, 51)
                                          ), 'Console page did not render after timers and foreign-frame messages'

        desktop.key('f4', 'alt');
        desktop.expect('CLOSE');
        desktop.exit()

        log = Path(vm.log.name).read_text(errors='replace')
        for marker in ('CONSOLE_PRIMITIVES 42 true null undefined 17', 'CONSOLE_ERROR TypeError: console safe error', 'CONSOLE_ASSERT'):
            assert marker in log, marker
        assert 'CONSOLE_COERCION_PRIVATE' not in log, 'Object coercion executed or its private result was printed'
        for marker in ('Console failed', 'PANIC:', 'Uncaught error in JS:', 'BROWSER_FORCED_CLOSE'):
            assert marker not in log, marker

        print(
            f'PASS native console: no object/getter/proxy coercion, primitive/error output, workers, timers and isolated frames ({cpus} CPUs, {accel})', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True);
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--label', default='native-console')
    args = parser.parse_args();
    exercise(args.iso, args.cpus, args.accel, args.label)
