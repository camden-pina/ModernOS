#!/usr/bin/env python3
"""Check document focus across native isolated frames using real guest input."""
import argparse
from pathlib import Path
import shlex
from test_runtime import VM, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture


def exercise(iso, cpus, accel, label):
    fixture = WebFixture(webcore=True)
    vm = None
    parent_origin = fixture.url('', tls=True).rstrip('/')
    child_origin = f'https://10.0.2.2:{fixture.cross_https}'
    child = '''<!doctype html><meta charset=utf-8>
<body style="margin:0;background:#00ccff">
<button id=b style="margin:5px;width:100px;height:40px">Child</button><script>
function report(kind){parent.postMessage({kind,focused:document.hasFocus()},PARENT);}
b.onclick=e=>{if(!e.isTrusted)throw Error('untrusted click');clearInterval(readyTimer);report('clicked');};
onkeydown=e=>{if(e.key==='k'){if(!e.isTrusted||document.activeElement!==b)throw Error('keyboard focus');report('keyed');}};
onmessage=e=>{if(e.origin===PARENT&&e.source===parent)report(e.data);};
onfocus=()=>report('focus');onblur=()=>report('blur');
const readyTimer=setInterval(()=>report('ready'),250);
</script>'''.replace('PARENT', repr(parent_origin))
    parent = '''<!doctype html><meta charset=utf-8><title>Frame focus loading</title>
<body style="margin:0;background:white">
<iframe id=f src=CHILD style="position:absolute;left:20px;top:60px;width:300px;height:160px;border:0"></iframe>
<button id=b style="position:absolute;left:20px;top:240px;width:100px;height:40px">Parent</button>
<script>
let stage=0;
function fail(message){document.title='Frame focus failed '+message;console.error(message);}
onmessage=e=>{
 if(e.source!==f.contentWindow||e.origin!==ORIGIN)return;
 console.log('FOCUS_REPORT '+JSON.stringify({stage,kind:e.data.kind,child:e.data.focused,parent:document.hasFocus(),owner:document.activeElement===f}));
 if(e.data.kind==='ready'&&stage===0){
  if(e.data.focused||!document.hasFocus())return fail('initial');
  stage=1;document.title='Frame focus ready';
 }else if(e.data.kind==='clicked'&&stage===1){
  if(!e.data.focused||!document.hasFocus()||document.activeElement!==f)return fail('child click');
  stage=2;document.title='Frame focus child';
 }else if(e.data.kind==='parent-check'&&stage===2){
  if(e.data.focused||!document.hasFocus()||document.activeElement!==b)return fail('parent click');
  stage=3;document.title='Frame focus parent';
 }else if(e.data.kind==='clicked'&&stage===3){
  if(!e.data.focused||!document.hasFocus()||document.activeElement!==f)return fail('return click');
  stage=4;document.title='Frame focus returned';
 }else if(e.data.kind==='keyed'&&stage===2){
  if(!e.data.focused||!document.hasFocus()||document.activeElement!==f)return fail('keyboard');
  document.title='Frame focus child keyed';
 }
};
b.onclick=e=>{if(!e.isTrusted)return fail('untrusted parent');f.contentWindow.postMessage('parent-check',ORIGIN);};
</script>'''.replace('CHILD', repr(child_origin + '/focus-child')).replace('ORIGIN', repr(child_origin))
    fixture.assets['/focus-parent'] = parent.encode(), 'text/html; charset=utf-8'
    fixture.assets['/focus-child'] = child.encode(), 'text/html; charset=utf-8'

    # Return to the top origin across a second isolation boundary. All three
    # documents have independent Pages and focus controllers.
    middle = '''<!doctype html><meta charset=utf-8><body style="margin:0;background:#00ccff">
<iframe id=f src=LEAF style="width:300px;height:160px;border:0"></iframe><script>
onmessage=e=>{
 if(e.source===f.contentWindow&&e.origin===TOP){
  parent.postMessage({...e.data,middleFocused:document.hasFocus(),middleOwner:document.activeElement===f},TOP);
 }else if(e.source===parent&&e.origin===TOP)f.contentWindow.postMessage(e.data,TOP);
};
</script>'''.replace('LEAF', repr(parent_origin + '/focus-leaf')).replace('TOP', repr(parent_origin))
    nested_parent = parent.replace(child_origin + '/focus-child', child_origin + '/focus-middle').replace(
        "if(e.data.kind==='ready'&&stage===0){", """if('middleFocused' in e.data){
  const active=e.data.kind==='clicked'||e.data.kind==='keyed'||e.data.kind==='focus';
  if(e.data.middleFocused!==active||(active&&!e.data.middleOwner))return fail('middle');
 }
 if(e.data.kind==='ready'&&stage===0){""")
    fixture.assets['/focus-nested-parent'] = nested_parent.encode(), 'text/html; charset=utf-8'
    fixture.assets['/focus-middle'] = middle.encode(), 'text/html; charset=utf-8'
    fixture.assets['/focus-leaf'] = child.replace(repr(parent_origin),
                                                  repr(child_origin)).encode(), 'text/html; charset=utf-8'
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
        try:
            for path in ('focus-parent', 'focus-nested-parent'):
                d.key('l', 'ctrl');
                vm.send(fixture.url(path, tls=True) + '\n')
                d.expect('BROWSER_TITLE', 'Frame focus ready')
                d.click(left + 70, body + 85);
                d.expect('BROWSER_TITLE', 'Frame focus child')
                d.key('k');
                d.expect('BROWSER_TITLE', 'Frame focus child keyed')
                d.click(left + 70, body + 260);
                d.expect('BROWSER_TITLE', 'Frame focus parent')
                d.click(left + 70, body + 85);
                d.expect('BROWSER_TITLE', 'Frame focus returned')
        except AssertionError:
            d.screenshot(label + '-failed');
            raise

        d.screenshot(label + '-returned');
        d.key('f4', 'alt');
        d.expect('CLOSE');
        d.exit()

        log = Path(vm.log.name).read_text(errors='replace')
        for marker in ('PANIC:', 'GLib-CRITICAL', 'BROWSER_FORCED_CLOSE', 'Uncaught error in JS:'):
            assert marker not in log, marker

        print(f'PASS isolated frame focus: one/two isolation boundaries, initial state, '
              f'real child/parent/child clicks, trusted keyboard routing, ancestor focus '
              f'and active iframe ({cpus} CPUs, {accel})', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True)
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--label', default='native-frame-focus')
    args = parser.parse_args();
    exercise(args.iso, args.cpus, args.accel, args.label)
