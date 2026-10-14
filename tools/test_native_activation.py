#!/usr/bin/env python3
"""Verify genuine WebCore user activation and isolated-frame propagation."""
import argparse
from pathlib import Path
import shlex
import re
from test_runtime import VM, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture


STATE = '''
function state(w=window){
 const a=w.navigator.userActivation;
 if(!a||!(a instanceof w.UserActivation)||a!==w.navigator.userActivation)throw Error('User Activation API missing');
 return [a.hasBeenActive,a.isActive];
}
'''


def exercise(iso, cpus, accel, label):
    fixture = WebFixture(webcore=True)
    vm = None
    top_origin = fixture.url('', tls=True).rstrip('/')
    middle_origin = f'https://10.0.2.2:{fixture.cross_https}'
    leaf = '''<!doctype html><meta charset=utf-8><body style="margin:0;background:#11aa33">
<button id=b style="margin:5px;width:100px;height:35px">Leaf</button><script>STATE
b.onclick=e=>{if(e.isTrusted)parent.postMessage({kind:'clicked',who:'leaf'},MIDDLE);};
onmessage=e=>{
 if(e.source!==parent||e.origin!==MIDDLE)return;
 try{
  if(e.data.kind==='synthetic')b.click();
  parent.postMessage({kind:'state',who:'leaf',round:e.data.round,state:state()},MIDDLE);
 }catch(x){parent.postMessage({kind:'failed',message:String(x)},MIDDLE);}
};
setInterval(()=>parent.postMessage({kind:'ready',who:'leaf'},MIDDLE),250);
</script>'''.replace('STATE', STATE).replace('MIDDLE', repr(middle_origin))
    middle = '''<!doctype html><meta charset=utf-8><body style="margin:0;background:#00ccff">
<button id=b style="margin:5px;width:100px;height:35px">Middle</button><input id=p type=file hidden>
<iframe id=f src=LEAF style="position:absolute;left:0;top:60px;width:300px;height:90px;border:0"></iframe>
<script>STATE
b.onclick=e=>{if(e.isTrusted)parent.postMessage({kind:'clicked',who:'middle'},TOP);};
onkeydown=e=>{
 if(e.key==='c'&&e.isTrusted){
  try{p.showPicker();parent.postMessage({kind:'consumed'},TOP);}
  catch(x){parent.postMessage({kind:'failed',message:String(x)},TOP);}
 }
};
onmessage=e=>{
 if(e.source===f.contentWindow&&e.origin===TOP){parent.postMessage(e.data,TOP);return;}
 if(e.source!==parent||e.origin!==TOP)return;
 try{
  if(e.data.kind==='synthetic')b.click();
  parent.postMessage({kind:'state',who:'middle',round:e.data.round,state:state()},TOP);
  f.contentWindow.postMessage(e.data,TOP);
 }catch(x){parent.postMessage({kind:'failed',message:String(x)},TOP);}
};
</script>'''.replace('STATE', STATE).replace('LEAF', repr(top_origin + '/activation-leaf')).replace('TOP', repr(top_origin))
    top = '''<!doctype html><meta charset=utf-8><title>Activation loading</title>
<body style="margin:0;background:white">
<iframe id=f src=MIDDLE style="position:absolute;left:20px;top:60px;width:300px;height:180px;border:0"></iframe>
<button id=b style="position:absolute;left:20px;top:260px;width:100px;height:40px">Top</button>
<iframe id=s src="/activation-local" style="display:none"></iframe>
<script>STATE
let stage=0,round=0,states={},expected,title;
function fail(x){document.title='Activation failed '+x;console.error(x);}
function check(){
 if(!states.middle||!states.leaf)return;
 const actual=[states.top,states.middle,states.leaf,states.local];
 console.log('ACTIVATION_REPORT '+JSON.stringify({title,actual,expected}));
 if(JSON.stringify(actual)!==JSON.stringify(expected))return fail(title+' '+JSON.stringify(actual));
 document.title=title;
 if(stage===0){stage=1;b.click();query('synthetic',[[false,false],[false,false],[false,false],[false,false]],'Activation synthetic unchanged');}
}
function query(kind,want,name){
 expected=want;title=name;states={};round++;
 try{states.top=state();states.local=state(s.contentWindow);f.contentWindow.postMessage({kind,round},ORIGIN);}
 catch(x){fail(x);}
}
b.onclick=e=>{if(e.isTrusted){stage=4;query('snapshot',[[true,true],[true,false],[true,true],[true,true]],'Activation parent');}};
onmessage=e=>{
 if(e.source!==f.contentWindow||e.origin!==ORIGIN)return;
 const m=e.data;
 if(m.kind==='failed')return fail(m.message);
 if(m.kind==='ready'&&stage===0&&!round)query('snapshot',[[false,false],[false,false],[false,false],[false,false]],'Activation ready');
 else if(m.kind==='state'&&m.round===round){states[m.who]=m.state;check();}
 else if(m.kind==='clicked'&&m.who==='middle'){
  stage=2;query('snapshot',[[true,true],[true,true],[false,false],[false,false]],'Activation child');
 }else if(m.kind==='consumed'){
  stage=3;query('snapshot',[[true,false],[true,false],[false,false],[false,false]],'Activation consumed');
 }else if(m.kind==='clicked'&&m.who==='leaf'){
  stage=5;query('snapshot',[[true,true],[true,true],[true,true],[true,true]],'Activation leaf');
  setTimeout(()=>query('snapshot',[[true,false],[true,false],[true,false],[true,false]],'Activation expired'),6500);
 }
};
</script>'''.replace('STATE', STATE).replace('MIDDLE', repr(middle_origin + '/activation-middle')).replace('ORIGIN', repr(middle_origin))
    for path, html in (('/activation-top', top), ('/activation-middle', middle), ('/activation-leaf', leaf)):
        fixture.assets[path] = html.encode(), 'text/html; charset=utf-8'
    fixture.assets['/activation-local'] = b'<!doctype html><title>Local activation sibling</title>', 'text/html; charset=utf-8'
    try:
        vm = VM(cpus, storage=False, iso=iso, label=label, accel=accel)
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do '
            'sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            f'curl -fsS --max-time 30 {fixture.url("ca.pem")} >> /etc/ssl/cert.pem')))
        vm.send('gui --trace --stats\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm);
        d.screenshot(label + '-start');

        window = d.launch('Browser')
        left = (d.width - 840) // 2 + (window - 1) % 5 * 16
        body = (d.height - 42 - 560) // 2 + (window - 1) % 5 * 16 + 40
        d.key('l', 'ctrl');
        vm.send(fixture.url('activation-top', tls=True) + '\n')

        def title(expected):
            return vm.until(rb'DESKTOP BROWSER_TITLE window=\d+ workspace=\d+ ' +
                            re.escape(expected.encode()) + rb'\r?\n', 20)

        try:
            title('Activation synthetic unchanged')
            d.click(left + 70, body + 85);
            title('Activation child')
            d.key('c');
            title('Activation consumed')
            d.click(left + 70, body + 280);
            title('Activation parent')
            d.click(left + 70, body + 140);
            title('Activation leaf')
            title('Activation expired')
        except AssertionError:
            d.screenshot(label + '-failed');
            raise

        d.screenshot(label + '-expired');
        d.key('f4', 'alt');
        d.expect('CLOSE');
        d.exit()
        log = Path(vm.log.name).read_text(errors='replace')
        for marker in ('PANIC:', 'GLib-CRITICAL', 'BROWSER_FORCED_CLOSE', 'Uncaught error in JS:', 'Activation failed'):
            assert marker not in log, marker

        print(f'PASS native activation: real upstream API, synthetic click exclusion, '
              f'ancestors and same-origin descendants across two isolation boundaries, '
              f'picker consumption, sticky state and real timer expiry ({cpus} CPUs, {accel})', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True)
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--label', default='native-activation')
    args = parser.parse_args();
    exercise(args.iso, args.cpus, args.accel, args.label)
