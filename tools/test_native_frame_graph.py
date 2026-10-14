#!/usr/bin/env python3
"""Check actual WindowProxy sibling order and names across an isolated frame."""
import argparse
import json
from pathlib import Path
import re
import shlex
from test_runtime import ROOT, VM, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture


def extended_fixture(fixture, parent_origin, child_origin):
    peer_port = fixture.listen(fixture.servers[0].RequestHandlerClass, 'good')
    peer_origin = f'https://10.0.2.2:{peer_port}'
    templates = {
        '/graph-mixed-local': '''<!doctype html><body style="background:#11aa33"><script>
onmessage=e=>{
 if(e.origin===CHILD&&e.source===parent.foreign.contentWindow&&e.data.kind==='local-ping')
  e.source.postMessage({kind:'local-pong',token:e.data.token},CHILD);
 else if(e.origin===PEER&&e.source===parent.foreign.contentWindow[0][1]&&e.data.kind==='leaf-local-ping')
  e.source.postMessage({kind:'leaf-local-pong',token:e.data.token},PEER);
};</script>''',
        '/graph-peer': '''<!doctype html><body style="background:#eeaa33"><script>
onmessage=e=>{
 if(e.source===parent&&e.origin===TOP&&e.data.kind==='rename-peer'){window.name='peer-renamed';return;}
 if(e.origin!==CHILD||e.source!==parent[1])return;
 if(e.data.kind==='peer-ping')e.source.postMessage({kind:'peer-pong',token:e.data.token},CHILD);
 else if(e.data.kind==='wrong-origin')parent.postMessage({kind:'failed',message:'wrong targetOrigin delivered'},TOP);
};</script>''',
        '/graph-mixed-child': '''<!doctype html><body style="margin:0;background:#00ccff">Graph child<script>
const savedParent=parent,savedSelf=self;let savedLocal,savedPeer,savedRemoved,localPong=false,peerPong=false,started=false;
function failed(message){let index=-1;for(let i=0;i<parent.length;i++)if(parent[i]===self)index=i;
 parent.postMessage({kind:'failed',message,childCount:parent.length,childIndex:index},TOP);}
function check(value,message){if(!value)throw Error(message);}
function sop(target){let blocked=false;try{target.document.body;}catch(e){blocked=e.name==='SecurityError';}check(blocked,'SOP');}
function report(kind){let index=-1;for(let i=0;i<parent.length;i++)if(parent[i]===self)index=i;
 parent.postMessage({kind,name:window.name,count:parent.length,index,sourceParentStable:parent===savedParent,
 selfStable:self===savedSelf},TOP);}
function wait(checker,done){let tries=0;const timer=setInterval(()=>{try{
 if(checker()){clearInterval(timer);done();}else if(++tries>100){clearInterval(timer);failed('graph update timeout');}
 }catch(e){clearInterval(timer);failed(e.message);}},30);}
function stableName(name,kind){wait(()=>window.name===name,()=>{let ticks=0;const timer=setInterval(()=>{try{
 check(window.name===name,'name reverted after synchronization');if(++ticks===20){clearInterval(timer);report(kind);}
 }catch(e){clearInterval(timer);failed(e.message);}},25);});}
onmessage=e=>{try{
 if(e.source===savedLocal&&e.origin===TOP&&e.data.kind==='local-pong'){check(e.data.token===41,'local token');localPong=true;}
 else if(e.source===savedPeer&&e.origin===PEER&&e.data.kind==='peer-pong'){check(e.data.token===42,'peer token');peerPong=true;}
 else if(e.source===parent&&e.origin===TOP){
  if(e.data.kind==='rename-child'){window.name='child-renamed';stableName('child-renamed','child-renamed');}
  else if(e.data.kind==='rename-parent'){
   check(window.name==='child-renamed','owner attribute unexpectedly renamed existing navigable');
   window.name='child-requested';stableName('child-requested','parent-renamed');
  }
  else if(e.data.kind==='insert'){const wanted=e.data.expected;wait(()=>parent.length===wanted.count&&parent[wanted.foreign]===self,()=>{
   check(parent[wanted.local]===savedLocal&&parent[wanted.peer]===savedPeer,'insert changed surviving proxies');savedRemoved=parent[wanted.added];report('inserted');});}
  else if(e.data.kind==='remove'){const wanted=e.data.expected;wait(()=>parent.length===wanted.count&&parent[wanted.foreign]===self,()=>{
   check(parent[wanted.local]===savedLocal&&parent[wanted.peer]===savedPeer,'remove changed surviving proxies');
   savedRemoved.postMessage({kind:'late-removed-frame'},TOP);report('removed');});}
  else if(e.data.kind==='nested'){
   const holder=document.createElement('iframe');holder.name='graph-holder';holder.src=CHILD+'/graph-holder';
   holder.style='width:250px;height:110px;border:0';document.body.appendChild(holder);
  }
 }
 if(localPong&&peerPong&&started){started=false;report('mixed-ready');}
}catch(e){failed(e.message);}};
wait(()=>parent.length===3&&parent[1]===self,()=>{try{
 check(parent===top&&window.name==='graph-child','initial ancestry/name');savedLocal=parent[0];savedPeer=parent[2];
 sop(parent);sop(savedLocal);sop(savedPeer);started=true;
 const ping=setInterval(()=>{if(localPong&&peerPong){clearInterval(ping);return;}
  if(!localPong)savedLocal.postMessage({kind:'local-ping',token:41},TOP);
  if(!peerPong)savedPeer.postMessage({kind:'peer-ping',token:42},PEER);
  savedPeer.postMessage({kind:'wrong-origin'},'https://wrong.invalid');},100);
}catch(e){failed(e.message);}});
</script>''',
        '/graph-holder': '''<!doctype html><body style="margin:0;background:#aaccee">
<iframe name=holder-local src="/graph-added" style="width:20px;height:20px;border:0"></iframe>
<iframe name=graph-leaf src=LEAF style="width:180px;height:80px;border:0"></iframe>''',
        '/graph-leaf': '''<!doctype html><body style="margin:0;background:#8844dd">Nested leaf<script>
function check(value,message){if(!value)throw Error(message);}
function failed(message){top.postMessage({kind:'failed',message},TOP);}
let tries=0;const timer=setInterval(()=>{try{
 if(++tries>100){clearInterval(timer);return failed('nested graph timeout');}
 if(top.length!==3||parent.length!==2||parent[1]!==self||top[1].length!==1)return;
 clearInterval(timer);check(top!==parent&&parent.parent===top[1]&&top[1][0]===parent,'nested actual ancestry');
 check(top.parent===top&&window.name==='graph-leaf','nested top/name');
 for(const target of [top,parent]){let blocked=false;try{target.document.body;}catch(e){blocked=e.name==='SecurityError';}check(blocked,'nested SOP');}
 onmessage=e=>{if(e.origin===TOP&&e.source===top[0]&&e.data.kind==='leaf-local-pong'){
  check(e.data.token===43,'leaf reply token');top.postMessage({kind:'nested-ready',name:window.name,
   trueTop:top!==parent,holderIsParent:top[1][0]===parent,selfIsSecond:parent[1]===self},TOP);
 }};
 top[0].postMessage({kind:'leaf-local-ping',token:43},TOP);
}catch(e){clearInterval(timer);failed(e.message);}},30);
</script>''',
        '/graph-mixed-parent': '''<!doctype html><meta charset=utf-8><title>Graph mixed loading</title>
<body style="margin:0;background:white"><iframe id=local name=graph-local src="/graph-mixed-local"
 style="position:absolute;left:20px;top:60px;width:140px;height:120px;border:0"></iframe>
<iframe id=foreign name=graph-child src=CHILD_PAGE style="position:absolute;left:180px;top:60px;width:280px;height:150px;border:0"></iframe>
<iframe id=peer name=graph-peer src=PEER_PAGE style="position:absolute;left:480px;top:60px;width:140px;height:120px;border:0"></iframe>
<button id=step style="position:absolute;left:20px;top:260px;width:200px;height:44px">Continue graph check</button>
<script>
let stage=0,inserted,stableForeign=foreign.contentWindow;
function positions(){const result={count:window.length,local:-1,foreign:-1,peer:-1,added:-1};
 for(let i=0;i<window.length;i++){if(window[i]===local.contentWindow)result.local=i;
  if(window[i]===stableForeign)result.foreign=i;if(window[i]===peer.contentWindow)result.peer=i;
  if(inserted&&window[i]===inserted.contentWindow)result.added=i;}return result;}
function mutation(kind,count){let tries=0;const timer=setInterval(()=>{
 if(window.length===count){clearInterval(timer);const expected=positions();console.log('FRAME_GRAPH_EXPECT '+JSON.stringify({kind,...expected}));
  foreign.contentWindow.postMessage({kind,expected},CHILD);}
 else if(++tries>100){clearInterval(timer);fail('parent graph mutation timeout');}},30);}
function fail(message){console.error('FRAME_GRAPH_FAILED '+message);document.title='Graph failed';}
function check(value,message){if(!value)throw Error(message);}
function named(name,done){let tries=0;const timer=setInterval(()=>{if(window.frames[name]===stableForeign){clearInterval(timer);done();}
 else if(++tries>100){clearInterval(timer);fail('named WindowProxy timeout '+name);}},30);}
onmessage=e=>{try{
 if(e.data.kind==='failed'){let index=-1;for(let i=0;i<window.length;i++)if(window[i]===stableForeign)index=i;
  return fail(JSON.stringify({...e.data,parentCount:window.length,parentIndex:index}));}
 if(e.data.kind==='nested-ready'){
  check(stage===5&&e.origin===PEER&&e.source===foreign.contentWindow[0][1],'nested source/origin');
  check(e.data.name==='graph-leaf'&&e.data.trueTop&&e.data.holderIsParent&&e.data.selfIsSecond,'nested graph values');
  document.title='Graph nested ready';console.log('FRAME_GRAPH_STAGE nested');return;
 }
 if(e.source!==stableForeign||e.origin!==CHILD)return;
 check(e.data.sourceParentStable&&e.data.selfStable,'surviving child proxies');
 if(e.data.kind==='mixed-ready'&&stage===0){check(e.data.count===3&&e.data.name==='graph-child','mixed graph');document.title='Graph mixed ready';}
 else if(e.data.kind==='child-renamed'&&stage===1){check(e.data.name==='child-renamed','child rename');
  named('child-renamed',()=>{document.title='Graph child renamed';console.log('FRAME_GRAPH_STAGE child-name');});}
 else if(e.data.kind==='parent-renamed'&&stage===2){check(e.data.name==='child-requested'&&foreign.name==='parent-attribute-name','requested rename/attribute reflection');
  named('child-requested',()=>{document.title='Graph requested rename ready';console.log('FRAME_GRAPH_STAGE requested-name');});}
 else if(e.data.kind==='inserted'&&stage===3){check(e.data.count===4&&e.data.index===positions().foreign,'insert ordering');
  document.title='Graph inserted ready';console.log('FRAME_GRAPH_STAGE insert');}
 else if(e.data.kind==='removed'&&stage===4){check(e.data.count===3&&e.data.index===positions().foreign,'remove ordering');
  document.title='Graph removed ready';console.log('FRAME_GRAPH_STAGE remove');}
}catch(e){fail(e.message);}};
step.onclick=e=>{try{check(e.isTrusted,'untrusted graph control');++stage;
 if(stage===1){foreign.contentWindow.postMessage({kind:'rename-child'},CHILD);peer.contentWindow.postMessage({kind:'rename-peer'},PEER);}
 else if(stage===2){foreign.name='parent-attribute-name';foreign.contentWindow.postMessage({kind:'rename-parent'},CHILD);}
 else if(stage===3){inserted=document.createElement('iframe');inserted.name='graph-added';inserted.src='/graph-added';
  inserted.style='position:absolute;left:650px;top:60px;width:80px;height:80px;border:0';document.body.insertBefore(inserted,foreign);
  mutation('insert',4);}
 else if(stage===4){inserted.remove();mutation('remove',3);}
 else if(stage===5)foreign.contentWindow.postMessage({kind:'nested'},CHILD);
}catch(e){fail(e.message);}};
</script>''',
        '/graph-opaque': '''<!doctype html><body style="background:#11aa33"><script>
let blocked=false;try{parent.document.body;}catch(e){blocked=e.name==='SecurityError';}
if(!blocked)throw Error('opaque parent DOM exposed');
parent.postMessage({kind:'opaque-default'});
parent.postMessage({kind:'opaque-slash'},'/');
onmessage=e=>{if(e.source===parent&&e.origin===TOP&&e.data==='probe')parent.postMessage({name:window.name,
 count:parent.length,selfIsFirst:parent[0]===self,parentIsTop:parent===top},'*');};
setInterval(()=>parent.postMessage({kind:'opaque-init'},'*'),100);
</script>''',
        '/graph-opaque-parent': '''<!doctype html><title>Graph opaque loading</title>
<iframe id=f name=graph-opaque sandbox="allow-scripts" src=OPAQUE_PAGE></iframe><script>
onmessage=e=>{if(e.source!==f.contentWindow)return;
 if(e.data.kind==='opaque-default'||e.data.kind==='opaque-slash'){
  console.error('FRAME_GRAPH_FAILED opaque target origin delivered');document.title='Graph failed';return;}
 if(e.origin!=='null'){console.error('FRAME_GRAPH_FAILED opaque origin upgraded');document.title='Graph failed';return;}
 if(e.data.kind==='opaque-init'){let blocked=false;try{f.contentWindow.document.body;}catch(x){blocked=x.name==='SecurityError';}
  if(!blocked){console.error('FRAME_GRAPH_FAILED opaque child DOM exposed');document.title='Graph failed';return;}
  f.contentWindow.postMessage('probe','*');return;}
 if(e.data.name!=='graph-opaque'||e.data.count!==1||!e.data.selfIsFirst||!e.data.parentIsTop){
  console.error('FRAME_GRAPH_FAILED opaque graph');document.title='Graph failed';return;}
 document.title='Graph opaque ready';console.log('FRAME_GRAPH_STAGE opaque');};</script>''',
    }
    replacements = {
        'CHILD_PAGE': json.dumps(child_origin + '/graph-mixed-child'),
        'PEER_PAGE': json.dumps(peer_origin + '/graph-peer'),
        'OPAQUE_PAGE': json.dumps(peer_origin + '/graph-opaque'),
        'LEAF': json.dumps(peer_origin + '/graph-leaf'),
        'CHILD': json.dumps(child_origin), 'PEER': json.dumps(peer_origin), 'TOP': json.dumps(parent_origin),
    }
    for path, html in templates.items():
        for key, value in replacements.items():
            html = html.replace(key, value)
        fixture.assets[path] = html.encode(), 'text/html; charset=utf-8'
    fixture.assets['/graph-added'] = b'<!doctype html><body style="background:#ddeeff">Graph local', 'text/html'


def exercise(iso, cpus, accel, label):
    fixture = WebFixture(webcore=True)
    vm = None
    parent_origin = fixture.url('', tls=True).rstrip('/')
    child_origin = f'https://10.0.2.2:{fixture.cross_https}'
    child = '''<!doctype html><meta charset=utf-8>
<body style="margin:0;background:#00ccff">Isolated graph child<script>
function indexed(index){try{return parent[index]===self;}catch(e){return e.name;}}
function report(){parent.postMessage({length:parent.length,firstIsSelf:indexed(0),
 secondIsSelf:indexed(1),parentIsTop:parent===top,name:window.name},PARENT);}
onmessage=e=>{if(e.source===parent&&e.origin===PARENT&&e.data==='report')report();};
setInterval(report,250);
</script>'''.replace('PARENT', repr(parent_origin))
    parent = '''<!doctype html><meta charset=utf-8><title>Frame graph loading</title>
<body style="margin:0;background:white">
<iframe id=local name=graph-local src="/graph-local" style="width:180px;height:80px;border:0"></iframe>
<iframe id=foreign name=graph-child src=CHILD style="width:260px;height:120px;border:0"></iframe>
<script>
onmessage=e=>{
 if(e.source!==foreign.contentWindow||e.origin!==ORIGIN)return;
 const result={...e.data,parentLength:window.length,parentFirstIsLocal:window[0]===local.contentWindow,
  parentSecondIsForeign:window[1]===foreign.contentWindow,sourceIsForeign:e.source===foreign.contentWindow};
 console.log('FRAME_GRAPH_REPORT '+JSON.stringify(result));document.title='Frame graph observed';
};
</script>'''.replace('CHILD', repr(child_origin + '/graph-child')).replace('ORIGIN', repr(child_origin))
    fixture.assets['/graph-parent'] = parent.encode(), 'text/html; charset=utf-8'
    fixture.assets['/graph-child'] = child.encode(), 'text/html; charset=utf-8'
    fixture.assets['/graph-local'] = b'<!doctype html><body style="background:#11aa33">Local graph sibling', 'text/html; charset=utf-8'
    extended_fixture(fixture, parent_origin, child_origin)
    expected = dict(length=2, firstIsSelf=False, secondIsSelf=True, parentIsTop=True,
                    name='graph-child', parentLength=2, parentFirstIsLocal=True,
                    parentSecondIsForeign=True, sourceIsForeign=True)
    result_path = ROOT / 'build/runtime-tests' / (label + '-result.json')
    if result_path.exists():
        raise SystemExit('Choose a unique label; result already exists: ' + str(result_path))
    result_path.parent.mkdir(parents=True, exist_ok=True)
    (result_path.parent / (label + '-test-source.py')).write_bytes(Path(__file__).read_bytes())

    try:
        vm = VM(cpus, storage=False, iso=iso, label=label, accel=accel)
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do '
            'sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            f'curl -fsS --max-time 30 {fixture.url("ca.pem")} >> /etc/ssl/cert.pem')))
        vm.send('gui --trace --stats\n');
        vm.until(b'GUI_READY', 40)
        desktop = DesktopTest(vm)
        desktop.screenshot(label + '-start')

        window = desktop.launch('Browser')
        desktop.key('l', 'ctrl');
        vm.send(fixture.url('graph-parent', tls=True) + '\n')
        desktop.expect('BROWSER_TITLE', 'Frame graph observed')
        desktop.screenshot(label + '-observed')
        desktop.expect('BROWSER_LOG', 'FRAME_GRAPH_REPORT')
        log = Path(vm.log.name).read_text(errors='replace')
        reports = [json.loads(match) for match in re.findall(
            r'FRAME_GRAPH_REPORT (\{[^\r\n]*\})', log)]
        assert reports, 'No graph report captured'
        observed = reports[-1]
        result = dict(iso=str(iso), cpus=cpus, accel=accel, log=str(vm.log.name),
                      expected=expected, observed=observed, initial_passed=observed == expected, passed=False)
        result_path.write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2), flush=True)

        for marker in ('PANIC:', 'GLib-CRITICAL', 'BROWSER_FORCED_CLOSE', 'Uncaught error in JS:'):
            assert marker not in log, marker
        assert observed == expected, 'Isolated frame WindowProxy graph differs from its actual parent'

        desktop.key('l', 'ctrl');
        vm.send(fixture.url('graph-mixed-parent', tls=True) + '\n')
        desktop.expect('BROWSER_TITLE', 'Graph mixed ready')
        left = (desktop.width - 840) // 2 + (window - 1) % 5 * 16
        body = (desktop.height - 42 - 560) // 2 + (window - 1) % 5 * 16 + 40
        for title in ('Graph child renamed', 'Graph requested rename ready', 'Graph inserted ready', 'Graph removed ready', 'Graph nested ready'):
            desktop.click(left + 70, body + 280);
            desktop.expect('BROWSER_TITLE', title)
        desktop.screenshot(label + '-nested')
        desktop.key('l', 'ctrl');
        vm.send(fixture.url('graph-opaque-parent', tls=True) + '\n')
        desktop.expect('BROWSER_TITLE', 'Graph opaque ready')
        desktop.screenshot(label + '-opaque')
        desktop.key('f4', 'alt');
        desktop.expect('CLOSE');
        desktop.exit()
        log = Path(vm.log.name).read_text(errors='replace')
        assert 'FRAME_GRAPH_FAILED' not in log, 'A graph stage failed'
        for marker in ('PANIC:', 'GLib-CRITICAL', 'BROWSER_FORCED_CLOSE', 'Uncaught error in JS:'):
            assert marker not in log, marker

        result['extended_stages'] = sorted(set(re.findall(r'FRAME_GRAPH_STAGE ([a-z-]+)', log)))
        assert result['extended_stages'] == ['child-name', 'insert',
                                             'nested', 'opaque', 'remove', 'requested-name'], result
        result['passed'] = True
        result_path.write_text(json.dumps(result, indent=2) + '\n')

        print(f'PASS native frame graph: actual mixed/nested siblings, local ancestor and true top, '
              f'stable WindowProxy identities, actual name synchronization/owner attribute reflection, insertion/removal, '
              f'SOP/opaque origins and real ancestor/local/foreign sibling message routing ({cpus} CPUs, {accel})', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True)
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--label', default='native-frame-graph')
    args = parser.parse_args()
    exercise(args.iso, args.cpus, args.accel, args.label)
