#!/usr/bin/env python3
"""Check secure-context inheritance in real workers inside isolated frames."""
import argparse
from pathlib import Path
import shlex
from test_runtime import VM, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture


WORKER = '''
onmessage=async e=>{
 try{
  const result={secure:isSecureContext,subtle:typeof crypto.subtle!=='undefined',origin:origin};
  if(result.subtle){
   const bytes=await crypto.subtle.digest('SHA-256',new TextEncoder().encode('abc'));
   result.digest=Array.from(new Uint8Array(bytes)).map(v=>v.toString(16).padStart(2,'0')).join('');
  }
  postMessage(result);
 }catch(e){postMessage({error:String(e)});}
};
'''

CHECK = '''
async function checkWorkers(expected){
 for(const blob of [false,true]){
  const url=blob?URL.createObjectURL(new Blob([WORKER],{type:'text/javascript'})):'/worker-context.js';
  const result=await new Promise((resolve,reject)=>{
   const w=new Worker(url),timer=setTimeout(()=>{w.terminate();reject(Error('worker timeout'));},8000);
   w.onmessage=e=>{clearTimeout(timer);w.terminate();resolve(e.data);};
   w.onerror=e=>{clearTimeout(timer);w.terminate();reject(Error(e.message));};w.postMessage('check');
  });
  if(blob)URL.revokeObjectURL(url);
  if(result.error||result.secure!==expected||result.subtle!==expected||result.origin!==location.origin)
   throw Error((blob?'Blob':'Network')+' worker context '+JSON.stringify(result)+' expected secure='+expected);
  if(expected&&result.digest!=='ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')
   throw Error('worker SHA-256 result');
 }
}
'''.replace('WORKER', repr(WORKER))


def exercise(iso, cpus, accel, label):
    fixture = WebFixture(webcore=True)
    vm = None
    child_origin = f'https://10.0.2.2:{fixture.cross_https}'
    fixture.assets['/worker-context.js'] = WORKER.encode(), 'application/javascript'
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
        for secure in (True, False):
            parent_origin = fixture.url('', tls=secure).rstrip('/')
            expected = 'true' if secure else 'false'

            # Both child documents use HTTPS. The HTTP top-level document must
            # keep their workers insecure, even across two renderer boundaries.
            nested_url = fixture.url('worker-context-nested', tls=True)
            nested = '''<!doctype html><meta charset=utf-8><body style="margin:0;background:#11aa33"><script>''' + CHECK + '''
checkWorkers(EXPECTED).then(()=>parent.postMessage('nested ready','*'))
 .catch(e=>{console.error(e);parent.postMessage('failed '+e,'*');});
</script>'''
            child = '''<!doctype html><meta charset=utf-8><body style="margin:0;background:#11aa33">
<iframe id=n style="width:100px;height:60px;border:0" src="NESTED"></iframe><script>''' + CHECK + '''
let own=false,nested=false;function finish(){if(own&&nested)parent.postMessage('child ready',PARENT);}
onmessage=e=>{if(e.source!==n.contentWindow)return;if(e.data==='nested ready'){nested=true;finish();}else parent.postMessage(e.data,PARENT);};
checkWorkers(EXPECTED).then(()=>{own=true;finish();}).catch(e=>{console.error(e);parent.postMessage('failed '+e,PARENT);});
</script>'''
            main = '''<!doctype html><meta charset=utf-8><title>Worker contexts loading</title>
<body style="margin:0;background:#ffffff"><iframe id=f style="position:absolute;left:20px;top:60px;width:300px;height:160px;border:0" src="CHILD"></iframe><script>''' + CHECK + '''
let own=false,child=false;function finish(){if(own&&child)document.title='Worker contexts ready MODE';}
onmessage=e=>{if(e.source!==f.contentWindow)return;if(e.data==='child ready'){child=true;finish();}else {document.title='Worker contexts failed';console.error(e.data);}};
checkWorkers(EXPECTED).then(()=>{own=true;finish();}).catch(e=>{document.title='Worker contexts failed';console.error(e);});
</script>'''
            for path, html in (('/worker-context-main', main), ('/worker-context-child', child), ('/worker-context-nested', nested)):
                fixture.assets[path] = html.replace('EXPECTED', expected).replace('PARENT', repr(parent_origin)).replace(
                    'NESTED', nested_url).replace('CHILD', child_origin + '/worker-context-child').replace(
                    'MODE', 'HTTPS' if secure else 'HTTP').encode(), 'text/html; charset=utf-8'

            d.key('l', 'ctrl');
            vm.send(fixture.url('worker-context-main', tls=secure) + '\n')

            try:
                d.expect('BROWSER_TITLE', 'Worker contexts ready ' + ('HTTPS' if secure else 'HTTP'))
            except AssertionError:
                d.screenshot(label + '-failed');
                raise

            pixels = d.screenshot(label + ('-https' if secure else '-http'))
            left = (d.width - 840) // 2 + (window - 1) % 5 * 16;
            top = (d.height - 42 - 560) // 2 + (window - 1) % 5 * 16 + 40
            at = ((top + 150) * d.width + left + 200) * 3
            assert pixels[at:at + 3] == bytes((17, 170, 51)), 'Worker document was not rendered'

        d.key('f4', 'alt');
        d.expect('CLOSE');
        d.exit()
        log = Path(vm.log.name).read_text(errors='replace')
        for marker in ('PANIC:', 'GLib-CRITICAL', 'BROWSER_FORCED_CLOSE', 'Uncaught error in JS:'):
            assert marker not in log, marker

        print(f'PASS native worker contexts: top-level and two isolated HTTPS frames, '
              f'network/Blob workers, actual SHA-256 and insecure HTTP ancestry ({cpus} CPUs, {accel})', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True)
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--label', default='native-worker-context')
    args = parser.parse_args();
    exercise(args.iso, args.cpus, args.accel, args.label)
