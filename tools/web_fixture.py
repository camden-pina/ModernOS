#!/usr/bin/env python3
"""Disposable loopback HTTP/TLS/Git fixtures for real guest networking tests.

No external repositories, accounts, or services are modified. QEMU's user
network maps its host address 10.0.2.2 to these loopback-only host listeners.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import base64
import hashlib
import os
from pathlib import Path
import ssl
import struct
import subprocess
import tempfile
import threading
import zlib
from urllib.parse import urlsplit, unquote, parse_qs

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'build/web-fixture'
JPEG_TILE = base64.b64decode('/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAIBAQEBAQIBAQECAgICAgQDAgICAgUEBAMEBgUGBgYFBgYGBwkIBgcJBwYGCAsICQoKCgoKBggLDAsKDAkKCgr/2wBDAQICAgICAgUDAwUKBwYHCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgr/wAARCAAgACADAREAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDyev64P8/woAKACgAoAKACgAoAKACgAoAKACgAoAKACgD/2Q==')
WEBP_TILE = base64.b64decode('UklGRh4AAABXRUJQVlA4TBEAAAAvH8AHAAdQmEIXuP+BiOh/AAA=')


def run(*args, **kwargs):
    return subprocess.check_output([str(arg) for arg in args], stderr=subprocess.STDOUT,
                                   timeout=60, **kwargs)


class WebFixture:
    def __init__(self, webcore=False):
        WORK.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(prefix='fixture-', dir=WORK))
        self.payload = bytes((i * 31 + 7) % 256 for i in range(190123))
        self.sha256 = hashlib.sha256(self.payload).hexdigest()
        self.servers = []
        self.threads = []
        self.requests = []
        self.requests_by_port = []
        self.assets = {}
        self.ranges = []
        self.lock = threading.Lock()
        self.log = (self.directory / 'requests.log').open('w')
        self.repo = self.directory / 'repos/main.git'
        self.repo.parent.mkdir(exist_ok=True)
        run('git', 'init', '--bare', '--initial-branch=main', self.repo)
        run('git', '-C', self.repo, 'config', 'http.receivepack', 'true')
        self.commit('first fixture revision')
        self.git_backend = Path(run('git', '--exec-path').decode().strip()) / 'git-http-backend'
        self.certificates()
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, format, *args):
                with fixture.lock:
                    fixture.log.write(format % args + '\n');
                    fixture.log.flush()

            def body(self):
                if self.headers.get('Transfer-Encoding', '').lower() == 'chunked':
                    result = bytearray()
                    while True:
                        count = int(self.rfile.readline(128).split(b';', 1)[0].strip(), 16)
                        if not count:
                            while self.rfile.readline(4096).strip():
                                pass
                            return bytes(result)

                        if count > 32 * 1024 * 1024 - len(result):
                            raise ValueError('fixture request too large')
                        result.extend(self.rfile.read(count))

                        if self.rfile.read(2) != b'\r\n':
                            raise ValueError('invalid chunk terminator')

                count = int(self.headers.get('Content-Length', '0'))
                if not 0 <= count <= 32 * 1024 * 1024:
                    raise ValueError('fixture request too large')
                return self.rfile.read(count)

            def respond(self, status, body, content_type='text/plain', headers=()):
                self.send_response(status)
                self.send_header('Content-Type', content_type)
                self.send_header('Content-Length', str(len(body)))
                for key, value in headers:
                    self.send_header(key, value)
                self.end_headers()
                if self.command != 'HEAD':
                    self.wfile.write(body)

            def do_HEAD(self):
                self.do_GET()

            def do_GET(self):
                parsed = urlsplit(self.path)
                with fixture.lock:
                    fixture.requests.append((self.command, self.path))
                    fixture.requests_by_port.append(
                        (self.server.server_port, self.command, self.path))

                if parsed.path in fixture.assets:
                    data, content_type = fixture.assets[parsed.path]
                    selected = self.headers.get('Range')
                    if selected:
                        import re

                        match = re.fullmatch(r'bytes=(\d+)-(\d*)', selected)
                        if not match:
                            return self.respond(416, b'')
                        first = int(match[1]);
                        last = min(int(match[2]) if match[2] else len(data) - 1, len(data) - 1)
                        if first > last:
                            return self.respond(416, b'', headers=[('Content-Range', f'bytes */{len(data)}')])
                        with fixture.lock:
                            fixture.ranges.append(
                                (self.server.server_port, parsed.path, first, last))
                        return self.respond(206, data[first:last + 1], content_type,
                                            [('Accept-Ranges', 'bytes'), ('Content-Range', f'bytes {first}-{last}/{len(data)}')])
                    return self.respond(200, data, content_type, [('Accept-Ranges', 'bytes')])

                if parsed.path.startswith('/repos/'):
                    return self.git(parsed)

                if parsed.path == '/webcore-credentials':
                    cross = f'https://10.0.2.2:{fixture.cross_https}'
                    script = ("const cross=" + repr(cross) + ";" + r'''
function check(ok,name){if(!ok)throw Error(name);}
document.cookie='credential_seed=ready;Path=/;SameSite=Strict';
async function exerciseFetch(){
 for(const mode of ['omit','same-origin','include']){
  const text=await fetch(cross+'/webcore-credential-echo?mode='+mode,{credentials:mode}).then(r=>r.text());
  check(mode==='include'?text.includes('credential_seed=ready'):text==='','cross-origin '+mode+' cookies: '+text);
 }
}
(async()=>{
 await exerciseFetch();
 check(!document.cookie.includes('credential_private='),'cross-origin HttpOnly exposed');
 const own=await fetch('/webcore-cookies').then(r=>r.text());
 check(own.includes('credential_private=server'),'include response cookie lost');
 check(!own.includes('credential_omit=') && !own.includes('credential_same-origin='),'noncredentialed response cookie stored');
 let denied=false;try{await fetch(cross+'/webcore-credential-denied',{credentials:'include'});}catch(e){denied=e instanceof TypeError;}
 check(denied,'credentialed CORS response exposed without permission');
 const source='const cross='+JSON.stringify(cross)+';'+check.toString()+exerciseFetch.toString()+';exerciseFetch().then(()=>postMessage("ready"),e=>postMessage(String(e)))';
 const blob=URL.createObjectURL(new Blob([source],{type:'text/javascript'}));
 const worker=new Worker(blob);
 await new Promise((resolve,reject)=>{worker.onmessage=e=>e.data==='ready'?resolve():reject(Error(e.data));worker.onerror=()=>reject(Error('worker failed'));});
 worker.terminate();URL.revokeObjectURL(blob);
 document.body.style.background='#11aa33';document.title='WebCore credentials ready';
})().catch(e=>{document.body.style.background='#e02030';document.title='WebCore credentials failed: '+e;});
''').encode()
                    return self.respond(200, b'<!doctype html><title>Checking fetch credentials</title><body>Cross-origin fetch credentials<script>' + script + b'</script>', 'text/html')

                if parsed.path in ('/webcore-credential-echo', '/webcore-credential-denied'):
                    mode = parse_qs(parsed.query).get('mode', ['include'])[0]
                    if mode not in ('omit', 'same-origin', 'include'):
                        return self.respond(400, b'Invalid credentials mode')
                    origin = self.headers.get('Origin', '')
                    allowed = [f'https://10.0.2.2:{fixture.https}',
                               f'https://10.0.2.2:{fixture.cross_https}']
                    headers = []
                    if parsed.path.endswith('echo') and origin in allowed:
                        headers = [('Access-Control-Allow-Origin', origin),
                                   ('Access-Control-Allow-Credentials', 'true')]
                    cookie = 'credential_private' if mode == 'include' else 'credential_' + mode
                    headers.append(('Set-Cookie', f'{cookie}=server;Path=/;HttpOnly;SameSite=Lax'))
                    return self.respond(200, self.headers.get('Cookie', '').encode(), headers=headers)

                if parsed.path == '/webcore-services':
                    mode = parse_qs(parsed.query).get('mode', ['first'])[0]
                    script = ("const mode=" + repr(mode) + ";" + r'''
function check(ok,name){if(!ok)throw Error(name);}
(async()=>{
check(navigator.cookieEnabled,'cookies disabled');
check(typeof HTMLVideoElement==='function' && document.createElement('video') instanceof HTMLVideoElement,'media DOM absent');
check(await fetch('/webcore-user-agent').then(r=>r.text())===navigator.userAgent,'HTTP/script browser identity mismatch');
document.cookie='dom=ready;Path=/;SameSite=Strict';
check(document.cookie.includes('dom=ready') && document.cookie.includes('visible=server'),'DOM cookies');
check(!document.cookie.includes('private='),'HttpOnly exposed');
document.cookie='private=evil;Path=/';
const cookies=await fetch('/webcore-cookies').then(r=>r.text());
check(cookies.includes('private=server') && !cookies.includes('private=evil') && cookies.includes('dom=ready'),'HTTP cookies');
check(await fetch('/webcore-cookies',{credentials:'omit'}).then(r=>r.text())==='','omitted credentials sent');
const value='text\u0000\ud800\u03a9';
if(mode==='return'){check(localStorage.getItem('persist')===value,'local restore');check(sessionStorage.getItem('persist')===value,'session restore');}
else if(mode==='new-window'){check(localStorage.getItem('persist')===value,'local window persistence');check(sessionStorage.getItem('persist')===null,'session leaked to new window');}
else {check(localStorage.getItem('persist')===null && sessionStorage.getItem('persist')===null,'cross-origin storage leaked');}
localStorage.setItem('persist',value);sessionStorage.setItem('persist',value);
for(const storage of [localStorage,sessionStorage]){
 storage.setItem('','');check(storage.getItem('')==='' && storage.getItem('missing')===null,'empty/missing storage values');
 check(storage.getItem('persist')===value && storage.length===2,'storage read/length');
 check(storage.key(0)==='persist' && storage.key(1)==='','storage key order');
 storage.setItem('persist',value);check(storage.length===2,'storage replacement');
 storage.removeItem('');check(storage.getItem('')===null && storage.length===1,'storage removal');
 let quota=false;try{storage.setItem('too-large','x'.repeat(3*1024*1024));}catch(e){quota=e.name==='QuotaExceededError';}
 check(quota && storage.length===1,'storage quota');
}
document.body.style.background='#ee7722';document.title='WebCore services '+mode;
})().catch(e=>{document.title='WebCore services failed: '+e;});
''').encode()
                    body = b'<!doctype html><meta charset=utf-8><title>Checking browser services</title><body style="margin:0">Cookies and storage<script>' + script + b'</script>'
                    return self.respond(200, body, 'text/html; charset=utf-8', [
                        ('Set-Cookie', 'private=server;Path=/;HttpOnly;SameSite=Lax'),
                        ('Set-Cookie', 'visible=server;Path=/;SameSite=Lax')])

                if parsed.path == '/webcore-cookies':
                    return self.respond(200, self.headers.get('Cookie', '').encode())
                if parsed.path == '/webcore-user-agent':
                    return self.respond(200, self.headers.get('User-Agent', '').encode())
                if parsed.path == '/webcore-history':
                    return self.respond(200, br'''<!doctype html><title>WebCore history first</title><style>
body{margin:0;background:#773399}button{position:absolute;top:20px;height:40px;border:0;background:#1188bb;color:white}
#push{left:20px;width:80px}#back{left:120px;width:80px}#marker{position:absolute;left:20px;top:520px;width:40px;height:40px;background:#11aa33}
</style><button id=push>Push</button><button id=back>Back</button><div id=marker></div><div style="position:absolute;top:1400px">End</div><script>
function show(state){document.body.style.background=state?'#ee7722':'#773399';document.title='WebCore history '+(state?'second':'first');}
push.onclick=()=>{history.pushState({second:true},'','#second');show(true);};back.onclick=()=>history.back();
onpopstate=e=>show(e.state&&e.state.second);
</script>''', 'text/html; charset=utf-8')
                if parsed.path == '/webcore-interpreter':
                    return self.respond(200, br'''<!doctype html><title>Measuring interpreter</title><body style="margin:0;background:#ee7722"><script>
const samples=[];
function run() {
 let value=0;
 const started=performance.now();
 for(let i=0;i<2000000;i++) value=(Math.imul(value^i,1664525)+1013904223)|0;
 const elapsed=performance.now()-started;
 if(value!==1025211264)throw Error('interpreter arithmetic mismatch: '+value);
 samples.push(elapsed.toFixed(3));
 if(samples.length<3)setTimeout(run,0);
 else {
  document.body.style.background='#00cc44';
  document.title='WebCore interpreter ready: '+samples.join(',');
 }
}
setTimeout(run,0);
</script>''', 'text/html; charset=utf-8')

                if parsed.path == '/webcore-dense-scroll':
                    rows = ''.join(
                        f'<div class=row style="background:rgb({i*37%256},{i*53%256},{i*71%256})">Row {i}</div>' for i in range(150))
                    return self.respond(200, ('''<!doctype html><title>WebCore dense scroll</title><style>
body{margin:0;font:20px sans-serif}.row{height:40px;padding-left:100px;box-sizing:border-box}
#fixed{position:fixed;left:0;top:0;width:100%;height:32px;background:#00ccee}
#changing{position:fixed;left:270px;top:80px;width:80px;height:40px;background:#ee7722}
</style>''' + rows + '''<div id=fixed>Fixed header</div><div id=changing></div><script>
onscroll=()=>{changing.style.background=scrollY?'#00cc44':'#ee7722';};
onkeydown=e=>{if(e.key==='r'){
 document.body.style.opacity='0.5';
 setTimeout(()=>{document.body.style.opacity='1';document.title='WebCore dense repainted';},150);
}};
</script>''').encode(), 'text/html; charset=utf-8')

                if parsed.path == '/webcore-fonts':
                    return self.respond(200, br'''<!doctype html><title>Checking fonts</title><style>
body{margin:0;background:#ee7722;font:28px sans-serif}p{margin:0;line-height:40px}
.zero{font:0px "Definitely Unavailable ModernOS Font"}
</style><p id=generic>Generic fallback text</p><p class=zero id=zero>Valid zero-sized text</p><canvas id=canvas width=10 height=10></canvas><script>
const c=canvas.getContext('2d');
for(const family of ['sans-serif','serif','monospace','system-ui','Definitely Unavailable ModernOS Font']) {
 c.font='0px "'+family+'"';if(c.measureText('valid text').width!==0)throw Error('zero font size: '+family);
}
if(zero.getBoundingClientRect().height!==0)throw Error('zero layout');
c.font='20px sans-serif';if(c.measureText('Readable generic text').width<50)throw Error('generic fallback');
document.body.style.background='#00cc44';document.title='WebCore fonts ready';
</script>''', 'text/html; charset=utf-8')

                if parsed.path == '/webcore-clipped-scroll':
                    rows = ''.join(
                        f'<div style="height:40px;background:rgb({i*37%256},{i*53%256},{i*71%256})">Nested row {i}</div>' for i in range(80))
                    return self.respond(200, ('''<!doctype html><title>WebCore clipped scroll</title><style>
body{margin:0;height:6000px;font:20px sans-serif;background:linear-gradient(#ffffff,#773399)}
#panel{position:fixed;left:20px;top:200px;width:320px;height:260px;overflow:auto;border-radius:16px;box-shadow:0 0 8px #333}
#sticky{position:sticky;top:0;height:32px;background:#00ccee}
#filtered{position:fixed;left:400px;top:400px;width:80px;height:80px;background:#ee7722;filter:drop-shadow(0 0 8px #333)}
#marker{position:absolute;left:600px;top:520px;width:40px;height:40px;background:#11aa33}
</style><div id=panel><div id=sticky>Sticky header</div>''' + rows + '''</div><div id=filtered></div><div id=marker></div><script>
onkeydown=e=>{if(e.key==='r'){
 document.body.style.opacity='0.5';
 setTimeout(()=>{document.body.style.opacity='1';document.title='WebCore clipped repainted';},150);
}};
</script>''').encode(), 'text/html; charset=utf-8')

                if parsed.path == '/webcore.jpeg':
                    return self.respond(200, JPEG_TILE, 'image/jpeg')
                if parsed.path == '/webcore.webp':
                    return self.respond(200, WEBP_TILE, 'image/webp')
                if parsed.path == '/webcore.svg':
                    return self.respond(200, b'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32"><path fill="#00ccee" d="M0 0H32V32H0Z"/></svg>', 'image/svg+xml')
                if parsed.path == '/webcore-images':
                    return self.respond(200, br'''<!doctype html><title>Checking compressed images</title><style>
body{margin:0;background:#fafafa}img{position:absolute;top:20px;width:32px;height:32px;opacity:0;transition:opacity .15s}
.loaded{opacity:1}#jpeg{left:20px}#webp{left:80px}#transformed{left:140px;transform:translateZ(0);will-change:transform;border-radius:8px}
#inline-svg{position:absolute;left:20px;top:80px;width:32px;height:32px;color:#00ccee}
#svg-image{left:80px;top:80px}#shadow-svg{position:absolute;left:140px;top:80px}#clipped{position:absolute;left:200px;top:20px;width:320px;height:180px;overflow:hidden;border-radius:16px;transform:translateZ(0)}
#masked{position:absolute;left:20px;top:140px;width:32px;height:32px;background:#00ccee;mask:url(/webcore.svg) center/32px 32px no-repeat}
#clipped img{position:static;width:320px;height:180px;object-fit:cover}
</style><img id=jpeg src=/webcore.jpeg><img id=webp src=/webcore.webp><img id=transformed loading=lazy src=/webcore.jpeg>
<svg id=inline-svg viewBox="0 0 32 32"><defs><path id=icon d="M0 0H32V32H0Z"/></defs><use href="#icon" fill="currentColor"/></svg>
<img id=svg-image src=/webcore.svg><div id=shadow-svg></div><div id=masked></div><div id=clipped><img id=large loading=lazy src=/webcore.jpeg></div><script>
document.getElementById('shadow-svg').attachShadow({mode:'open'}).innerHTML='<svg viewBox="0 0 32 32" style="display:block;width:32px;height:32px;color:#00ccee"><path fill="currentColor" d="M0 0H32V32H0Z"/></svg>';
const images=[jpeg,webp,transformed,large,document.getElementById('svg-image')];
const visible=new Promise((resolve,reject)=>{
 const observer=new IntersectionObserver(entries=>{
  const e=entries[0];if(!e.rootBounds||e.rootBounds.width<200||e.rootBounds.height<100)return reject(Error('viewport bounds'));
  if(e.isIntersecting&&e.intersectionRatio>0.9){observer.disconnect();resolve();}
 });observer.observe(transformed);
});
Promise.all([...images.map(i=>i.decode()),visible]).then(()=>{
 for(const i of [jpeg,webp,transformed,large]){if(i.naturalWidth!==32||i.naturalHeight!==32)throw Error('decoded dimensions');}
 requestAnimationFrame(()=>{for(const i of images)i.classList.add('loaded');setTimeout(()=>document.title='WebCore images ready',300);});
}).catch(e=>document.title='WebCore images failed: '+e);
</script>''', 'text/html; charset=utf-8')
                if parsed.path == '/webcore-transport':
                    return self.respond(200, br'''<!doctype html><title>WebCore transport ready</title>
<body style="margin:0;background:#2040e0"><script>
onload=()=>setTimeout(()=>{
 console.log('WebCore script busy');
 const end=performance.now()+5000;while(performance.now()<end){}
 document.title='WebCore script released';
},200);
</script>''', 'text/html; charset=utf-8')
                if parsed.path == '/webcore-scheduling':
                    return self.respond(200, br'''<!doctype html><title>Checking native scheduling</title>
<body style="margin:0;background:#ee7722"><script>
function failed(e){document.title='WebCore scheduling failed: '+e;}
try {
 let frames=0,microtask=false;const end=performance.now()+1500;
 function animate(){frames++;if(performance.now()<end)requestAnimationFrame(animate);}
 requestAnimationFrame(animate);
 const cancelled=requestIdleCallback(()=>failed('cancelled callback ran'));
 cancelIdleCallback(cancelled);
 requestIdleCallback(()=>Promise.resolve().then(()=>microtask=true));
 requestIdleCallback(deadline=>{
  try {
   if(!microtask)throw Error('idle microtask order');
   const remaining=deadline.timeRemaining();
   if(deadline.didTimeout||!Number.isFinite(remaining)||remaining<0||remaining>50)throw Error('idle deadline');
   Promise.resolve().then(()=>requestAnimationFrame(()=>{
    if(!frames)return failed('animation callbacks did not run');
    document.body.style.background='#00cc44';document.title='WebCore scheduling ready';
   }));
  }catch(e){failed(e);}
 });
}catch(e){failed(e);}
</script>''', 'text/html; charset=utf-8')
                if parsed.path == '/webcore-custom-scheduling':
                    return self.respond(200, br'''<!doctype html><title>Checking custom element scheduling</title>
<body style="margin:0;background:#ee7722">
<test-iconset data-size="12"></test-iconset><test-iconset data-size="13"></test-iconset>
<test-iconset data-size="15"></test-iconset><test-iconset data-size="16"></test-iconset>
<test-iconset data-size="18"></test-iconset><test-iconset data-size="20"></test-iconset>
<test-iconset data-size="24"></test-iconset><test-iconset data-size="48"></test-iconset>
<test-iconset data-size="96"></test-iconset><script>
const sizes=[12,13,15,16,18,20,24,48,96],pending=new Map(),seen=new Set();
const complete=Promise.all(sizes.map(size=>new Promise(resolve=>pending.set(size,resolve))));
const text=document.createTextNode('0'),callbacks=[];
new MutationObserver(()=>{
 const batch=callbacks.splice(0);batch.forEach(callback=>callback());
}).observe(text,{characterData:true});
function async(callback){callbacks.push(callback);text.data=String(Number(text.data)+1);}
let installed=false;
class IconSet extends HTMLElement {
 connectedCallback(){
  const size=Number(this.getAttribute('data-size'));
  async(()=>window.dispatchEvent(new CustomEvent('test-iconset-added',{detail:{size}})));
 }
}
customElements.define('test-iconset',IconSet);
window.addEventListener('test-iconset-added',event=>{
 const size=event.detail.size;
 if(!installed||seen.has(size)||!pending.has(size))throw Error('custom element notification ordering');
 seen.add(size);pending.get(size)();
});
const end=performance.now()+80;while(performance.now()<end){}
installed=true;
complete.then(()=>requestAnimationFrame(()=>setTimeout(()=>{
 if(seen.size!==9)throw Error('lost custom element notification');
 document.body.style.background='#00cc44';document.title='WebCore custom scheduling ready';
},16)));
</script>''', 'text/html; charset=utf-8')
                if parsed.path == '/webcore-frame-scheduling':
                    return self.respond(200, br'''<!doctype html><title>Checking frame scheduling</title>
<body style="margin:0;background:#ee7722"><script>
if('requestIdleCallback' in window)throw Error('experimental idle API default');
const cancelled=requestAnimationFrame(()=>{throw Error('cancelled animation frame');});
cancelAnimationFrame(cancelled);
Promise.resolve().then(()=>requestAnimationFrame(()=>setTimeout(()=>requestAnimationFrame(()=>{
 document.body.style.background='#00cc44';document.title='WebCore frame scheduling ready';
}),16)));
</script>''', 'text/html; charset=utf-8')
                if parsed.path == '/webcore-idle-budget':
                    return self.respond(200, br'''<!doctype html><title>Checking idle continuation</title>
<body style="margin:0;background:#ee7722"><script>
function failed(e){document.title='WebCore idle failed: '+e;}
onload=()=>setTimeout(()=>{
 const started=performance.now();
 requestIdleCallback(()=>{
  const end=performance.now()+80;while(performance.now()<end){}
 });
 requestIdleCallback(d=>{
  if(d.didTimeout)return failed('ordinary idle deadline');
  if(performance.now()-started>500)return failed('lost idle continuation');
  setTimeout(()=>{
   let done=false,timed=0,cancelled;
   function pulse(){if(!done)window.postMessage('keep-native-loop-busy','*');}
   function message(e){if(e.data==='keep-native-loop-busy')pulse();}
   addEventListener('message',message);
   requestIdleCallback(()=>{
    const end=performance.now()+80;while(performance.now()<end){}
    cancelIdleCallback(cancelled);pulse();
   });
   requestIdleCallback(deadline=>{
    if(!deadline.didTimeout||deadline.timeRemaining()!==0)return failed('runnable timeout deadline');
    done=true;timed++;removeEventListener('message',message);
   },{timeout:20});
   cancelled=requestIdleCallback(()=>failed('cancelled runnable callback'),{timeout:20});
   requestIdleCallback(()=>{
    if(timed!==1)return failed('timeout callback order or duplicate');
    document.body.style.background='#00cc44';document.title='WebCore idle continuation ready';
   });
  },50);
 });
},300);
</script>''', 'text/html; charset=utf-8')
                if parsed.path == '/webcore-busy-scroll':
                    return self.respond(200, br'''<!doctype html><title>WebCore busy scroll</title><style>
body{margin:0;height:3000px;background:#fafafa}
#first{position:absolute;left:20px;top:20px;width:40px;height:40px;background:#ee7722}
#marker{position:absolute;left:20px;top:520px;width:40px;height:40px;background:#11aa33}
</style><div id=first></div><div id=marker></div><script>
onload=()=>setTimeout(()=>{
 const finish=performance.now()+10000;
 addEventListener('message',e=>{
  if(e.data!=='native-scroll-work')return;
  const end=performance.now()+20;while(performance.now()<end){}
  if(performance.now()<finish)window.postMessage('native-scroll-work','*');
 });
 console.log('WebCore background task stream active');
 window.postMessage('native-scroll-work','*');
},200);
</script>''', 'text/html; charset=utf-8')
                if parsed.path == '/webcore-frame':
                    return self.respond(200, b'<!doctype html><title>Loading frame</title><body style="margin:0">'
                                        b'<iframe src="/webcore-frame-child" style="position:absolute;left:20px;top:20px;width:140px;height:100px;border:0"></iframe>',
                                        'text/html; charset=utf-8')
                if parsed.path == '/webcore-frame-child':
                    return self.respond(200, b'<!doctype html><body style="margin:0;background:#e02030">Real child frame<script>'
                                        b'onstorage=e=>{if(e.key!=="frame-event"||e.newValue!=="ready"||e.oldValue!==null||e.storageArea!==localStorage)throw Error("storage event");'
                                        b'localStorage.removeItem("frame-event");parent.document.body.style.background="#773399";parent.document.title="WebCore frame ready";};'
                                        b'parent.localStorage.setItem("frame-event","ready");</script>',
                                        'text/html; charset=utf-8')

                if parsed.path == '/webcore.html':
                    body = (b'<!doctype html><meta charset="utf-8"><title>Loading network fixture</title>'
                            b'<link rel="stylesheet" href="/webcore.css"><script defer src="/webcore.js"></script>'
                            b'<div id="grid"><div style="background:#e02030"></div><div style="background:#2040e0"></div></div>'
                            b'<button id="change">Change</button><div id="marker"></div><canvas id="canvas" width="40" height="40"></canvas>'
                            b'<img id="image" src="/webcore.png" width="32" height="32"><p>WebCore office \xd8\xa7\xd9\x84\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85</p>'
                            b'<img src="/missing-image.png" width="20" height="20" style="position:absolute;left:280px;top:195px">'
                            b'<form action="/webcore-form" method="post"><input id="field" name="query"></form>'
                            b'<div id="scrolled"></div><div style="position:absolute;top:1400px;width:1px;height:1px"></div>')
                    return self.respond(200, body, 'text/html; charset=utf-8')

                if parsed.path == '/webcore.css':
                    return self.respond(200, b'body{margin:0;background:#fafafa;font:16px "DejaVu Sans"}'
                                        b'#grid{position:absolute;left:20px;top:20px;display:grid;grid-template-columns:40px 40px;gap:10px}#grid div{height:40px}'
                                        b'#marker{position:absolute;left:140px;top:100px;width:40px;height:40px;background:#ffcc00}'
                                        b'button{position:absolute;left:20px;top:100px;width:80px;height:40px;border:0;background:#778899}'
                                        b'canvas{position:absolute;left:220px;top:20px}#image{position:absolute;left:220px;top:80px}'
                                        b'p{position:absolute;left:20px;top:160px}#field{position:absolute;left:20px;top:145px;width:100px;height:20px;box-sizing:border-box}'
                                        b'#scrolled{position:absolute;left:20px;top:520px;width:40px;height:40px;background:#11aa33}', 'text/css')

                if parsed.path == '/webcore.js':
                    if webcore:
                        tls = self.server.server_port in (fixture.https, fixture.cross_https)
                        cross = f'{"https" if tls else "http"}://10.0.2.2:{fixture.cross_https if tls else fixture.cross_http}'
                    else:
                        cross = ''

                    body = ("const cross=" + repr(cross) + ";" + r'''
const [a,b]=[2,3];const canvas=document.getElementById('canvas').getContext('2d');
canvas.fillStyle='#d030c0';canvas.fillRect(0,0,40,40);
document.getElementById('change').addEventListener('click',()=>{document.getElementById('marker').style.background='#00cc44';document.title='WebCore clicked';});
document.getElementById('field').addEventListener('input',e=>{document.getElementById('marker').style.background='#1122ee';document.title='WebCore typed: '+(e.target.value||'<empty>');});
const image=document.getElementById('image');const readyImage=new Promise((resolve,reject)=>{if(image.complete&&image.naturalWidth)resolve();else{image.onload=resolve;image.onerror=reject;}});
const requests=[readyImage,Promise.resolve(a+b).then(v=>{if(v!==5)throw Error('promise result');}),fetch('/webcore-redirect').then(r=>r.text()).then(t=>{if(t!=='network data')throw Error('fetch body');})];
if(cross){
requests.push(fetch(cross+'/webcore-allowed').then(r=>r.text()).then(t=>{if(t!=='allowed data')throw Error('allowed CORS body');}));
requests.push(fetch(cross+'/webcore-denied').then(r=>r.text()).then(()=>{throw Error('denied CORS exposed body');}).catch(e=>{if(!(e instanceof TypeError))throw e;}));
requests.push(fetch(cross+'/webcore-preflight',{method:'POST',headers:{'X-WebCore-Check':'yes'},body:'real preflight body'}).then(r=>r.text()).then(t=>{if(t!=='preflight accepted')throw Error('preflight body');}).catch(e=>{throw Error('CORS preflight: '+e);}));
}
Promise.all(requests).then(()=>document.title='WebCore rendered').catch(e=>document.title='WebCore failed: '+e);
''').encode()
                    return self.respond(200, body, 'text/javascript')

                if parsed.path == '/webcore.png':
                    def chunk(kind, data):
                        return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data))
                    pixels = (b'\0' + bytes((0, 204, 238, 255)) * 16) * 16
                    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!2I5B', 16, 16, 8, 6, 0, 0, 0))
                    png += chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b'')
                    return self.respond(200, png, 'image/png')

                if parsed.path == '/webcore-redirect':
                    return self.respond(302, b'', headers=[('Location', '/webcore-data')])
                if parsed.path == '/webcore-data':
                    return self.respond(200, b'network data')

                if parsed.path in ('/webcore-allowed', '/webcore-denied'):
                    headers = [('Access-Control-Allow-Origin', self.headers.get('Origin', ''))
                               ] if parsed.path.endswith('allowed') else []
                    return self.respond(200, b'allowed data' if headers else b'denied secret', headers=headers)

                if parsed.path == '/ca.pem':
                    return self.respond(200, (fixture.directory / 'ca.pem').read_bytes())
                if parsed.path in ('/large.bin', '/download.bin'):
                    return self.respond(200, fixture.payload, 'application/octet-stream',
                                        [('Content-Disposition', 'attachment; filename="download.bin"')])

                if parsed.path == '/chunked':
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/octet-stream')
                    self.send_header('Transfer-Encoding', 'chunked')
                    self.end_headers()
                    for offset in range(0, len(fixture.payload), 1403):
                        chunk = fixture.payload[offset:offset + 1403]
                        self.wfile.write(f'{len(chunk):x}\r\n'.encode() + chunk + b'\r\n')
                    self.wfile.write(b'0\r\n\r\n');
                    self.wfile.flush()
                    return

                if parsed.path == '/redirect':
                    return self.respond(302, b'', headers=[('Location', '/large.bin')])
                if parsed.path == '/fail':
                    return self.respond(503, b'Intentional fixture failure\n')
                if parsed.path == '/image.png':
                    return self.respond(200, (ROOT / 'userland/runtime/usr/share/netsurf/netsurf.png').read_bytes(), 'image/png')

                if parsed.path == '/second.html':
                    body = b'<!doctype html><title>ModernOS second page</title><h1>Second real document</h1>'
                elif parsed.path == '/script.html':
                    body = (b'<!doctype html><html><head><script>'
                            b'var words=["ModernOS","JavaScript","page"];'
                            b'if (globalThis !== window || self !== window) throw new Error("Incorrect global aliases");'
                            b'var start=performance.now(), origin=performance.timeOrigin;'
                            b'performance.timeOrigin=0;'
                            b'if (performance.timeOrigin!==origin || Math.abs(Date.now()-origin-start)>1000) throw new Error("Incorrect document clock");'
                            b'document.write("<title>"+words.join(" ")+"</title>");'
                            b'</script></head><body><h1>Embedded JavaScript</h1></body></html>')
                elif parsed.path in ('/', '/page.html', '/form'):
                    body = (b'<!doctype html><html><head><title>ModernOS HTTP page</title>'
                            b'<link rel="stylesheet" href="/style.css"></head><body>'
                            b'<h1>Real HTTP, HTML and CSS</h1><p id="intro">Served from the host fixture.</p>'
                            b'<p><a href="/second.html">Second page</a></p>'
                            b'<p><a href="/download.bin">Download binary</a></p>'
                            b'<img src="/image.png" alt="NetSurf PNG" width="64" height="64">'
                            b'<form action="/form" method="get"><input name="query" value="native">'
                            b'<input type="submit" value="Submit"></form>'
                            b'<div style="height:1000px">Scrollable content</div></body></html>')
                elif parsed.path == '/style.css':
                    return self.respond(200, b'body { background:#dcebe6; color:#2d3d44; font-family:sans-serif; margin:24px } h1 { color:#347966 }', 'text/css')
                else:
                    return self.respond(404, b'Not found\n')
                return self.respond(200, body, 'text/html; charset=utf-8')

            def do_POST(self):
                parsed = urlsplit(self.path)
                with fixture.lock:
                    fixture.requests.append((self.command, self.path))
                    fixture.requests_by_port.append(
                        (self.server.server_port, self.command, self.path))

                if parsed.path.startswith('/repos/'):
                    return self.git(parsed)

                if parsed.path == '/webcore-form':
                    if self.body() != b'query=webkit':
                        return self.respond(400, b'wrong form body')
                    return self.respond(200, b'<!doctype html><title>WebCore form submitted</title>'
                                        b'<style>body{margin:0;background:#1188bb;color:white;font:20px "DejaVu Sans"}</style><p>Real keyboard form POST</p>', 'text/html; charset=utf-8')

                if parsed.path == '/webcore-preflight':
                    if self.body() != b'real preflight body' or self.headers.get('X-WebCore-Check') != 'yes':
                        return self.respond(400, b'invalid preflight request')
                    return self.respond(200, b'preflight accepted', headers=[('Access-Control-Allow-Origin', self.headers.get('Origin', ''))])
                self.respond(404, b'Not found\n')

            def do_OPTIONS(self):
                with fixture.lock:
                    fixture.requests.append((self.command, self.path))
                    fixture.requests_by_port.append(
                        (self.server.server_port, self.command, self.path))

                if urlsplit(self.path).path != '/webcore-preflight':
                    return self.respond(404, b'Not found\n')
                return self.respond(200, b'', headers=[('Access-Control-Allow-Origin', self.headers.get('Origin', '')),
                                                       ('Access-Control-Allow-Methods', 'POST'), ('Access-Control-Allow-Headers', 'X-WebCore-Check')])

            def git(self, parsed):
                path = unquote(parsed.path[len('/repos'):])
                if '..' in path.split('/') or '\0' in path:
                    return self.respond(400, b'Invalid fixture path\n')
                env = os.environ | {'GIT_PROJECT_ROOT': str(fixture.repo.parent),
                                    'GIT_HTTP_EXPORT_ALL': '1', 'REQUEST_METHOD': self.command,
                                    'PATH_INFO': path, 'QUERY_STRING': parsed.query,
                                    'REMOTE_ADDR': '127.0.0.1',
                                    'CONTENT_TYPE': self.headers.get('Content-Type', '')}
                body = self.body()
                env['CONTENT_LENGTH'] = str(len(body))
                for key, value in self.headers.items():
                    env['HTTP_' + key.upper().replace('-', '_')] = value
                output = run(fixture.git_backend, input=body, env=env)
                head, separator, data = output.partition(b'\r\n\r\n')
                assert separator, output[:300]
                status, headers = 200, []
                for line in head.decode().split('\r\n'):
                    key, value = line.split(':', 1)
                    if key.lower() == 'status':
                        status = int(value.strip().split()[0])
                    elif key.lower() != 'content-length':
                        headers.append((key, value.strip()))

                self.send_response(status)
                for key, value in headers:
                    self.send_header(key, value)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers();
                self.wfile.write(data)

        try:
            self.http = self.listen(Handler)
            self.https = self.listen(Handler, 'good')
            self.wrong_host = self.listen(Handler, 'wrong')
            self.untrusted = self.listen(Handler, 'untrusted')
            if webcore:
                self.cross_http = self.listen(Handler)
                self.cross_https = self.listen(Handler, 'good')
        except BaseException:
            self.close();
            raise

    def certificates(self):
        run('openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
            '-keyout', self.directory / 'ca.key', '-out', self.directory / 'ca.pem', '-subj', '/CN=ModernOS fixture CA',
            '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,keyCertSign,cRLSign')

        for name, san in [('good', 'IP:10.0.2.2,DNS:localhost'), ('wrong', 'IP:192.0.2.1')]:
            run('openssl', 'req', '-new', '-newkey', 'rsa:2048', '-nodes',
                '-keyout', self.directory / (name + '.key'), '-out', self.directory / (name + '.csr'), '-subj', '/CN=ModernOS fixture')
            extensions = self.directory / (name + '.extensions')
            extensions.write_text(
                'basicConstraints=critical,CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=' + san + '\n')
            run('openssl', 'x509', '-req', '-in', self.directory / (name + '.csr'), '-CA', self.directory / 'ca.pem',
                '-CAkey', self.directory / 'ca.key', '-CAcreateserial', '-out', self.directory / (name + '.pem'),
                '-days', '2', '-extfile', extensions)

        run('openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
            '-keyout', self.directory / 'untrusted.key', '-out', self.directory / 'untrusted.pem',
            '-subj', '/CN=Untrusted ModernOS fixture', '-addext', 'subjectAltName=IP:10.0.2.2')

    def listen(self, handler, certificate=None):
        server = ThreadingHTTPServer(('127.0.0.1', 0), handler)
        server.daemon_threads = True
        if certificate:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(self.directory / (certificate + '.pem'),
                                    self.directory / (certificate + '.key'))
            server.socket = context.wrap_socket(server.socket, server_side=True)

        thread = threading.Thread(target=server.serve_forever, daemon=True)
        self.servers.append(server);
        self.threads.append(thread);
        thread.start()
        return server.server_address[1]

    def commit(self, text):
        tree = self.directory / 'seed'
        if not (tree / '.git').is_dir():
            tree.mkdir(exist_ok=True)
            run('git', 'init', '--initial-branch=main', tree)
        run('git', '-C', tree, 'config', 'user.name', 'ModernOS fixture')
        run('git', '-C', tree, 'config', 'user.email', 'fixture@example.invalid')
        (tree / 'document.txt').write_text(text + '\n')
        run('git', '-C', tree, 'add', 'document.txt')
        run('git', '-C', tree, '-c', 'commit.gpgsign=false', 'commit', '--allow-empty', '-m', text)
        run('git', '-C', tree, 'push', str(self.repo), 'HEAD:main')
        self.head = run('git', '-C', tree, 'rev-parse', 'HEAD').decode().strip()

    def url(self, path='', tls=False):
        return f'{"https" if tls else "http"}://10.0.2.2:{self.https if tls else self.http}/{path.lstrip("/")}'

    def close(self):
        for server in self.servers:
            server.shutdown();
            server.server_close()
        for thread in self.threads:
            thread.join(timeout=2)
        self.log.close()
