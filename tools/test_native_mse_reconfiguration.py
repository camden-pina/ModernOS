#!/usr/bin/env python3
"""Verify retained MSE samples decode with their original AVC configuration."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
from test_native_mse import fragments
from test_runtime import VM, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture


def exercise(iso, cpus, accel, label):
    fixture = WebFixture(webcore=True)
    vm = None
    try:
        counts, expected = {}, {}
        for name, color, size in [('a', 'red', '160x96'), ('b', 'blue', '320x192')]:
            path = fixture.directory / (name + '.mp4')
            subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y',
                            '-f', 'lavfi', '-i', f'color={color}:size={size}:rate=30:duration=3',
                            '-an', '-c:v', 'libx264', '-profile:v', 'high', '-level:v', '3.0',
                            '-pix_fmt', 'yuv420p', '-g', '30', '-bf', '3', '-threads', '2',
                            '-movflags', 'empty_moov+default_base_moof+frag_keyframe', str(path)],
                           check=True, timeout=45)
            init, segments = fragments(path.read_bytes())
            counts[name] = len(segments)
            fixture.assets[f'/{name}-init'] = init, 'video/mp4'
            for i, data in enumerate(segments):
                fixture.assets[f'/{name}-{i}'] = data, 'video/mp4'
            raw = subprocess.check_output(['ffmpeg', '-hide_banner', '-loglevel', 'error',
                                           '-ss', '1', '-i', str(
                                               path), '-frames:v', '1', '-pix_fmt', 'rgb24',
                                           '-f', 'rawvideo', '-'], timeout=30)
            expected[name] = list(raw[:3])

        page = '''<!doctype html><title>Reconfiguration loading</title>
<style>body{margin:0;background:#223344}video{width:320px;height:192px}
canvas{position:absolute;left:0;top:280px}button{position:absolute;top:210px;width:90px;height:40px}</style>
<video id=v></video><button id=a style="left:0">A</button><button id=b style="left:100px">B</button>
<canvas id=c width=160 height=96></canvas><script>
const counts=COUNTS,expected=EXPECTED,source=new MediaSource();let mode='',appended=false;
v.src=URL.createObjectURL(source);
const append=(sb,data)=>new Promise((resolve,reject)=>{
 sb.addEventListener('updateend',resolve,{once:true});sb.addEventListener('error',reject,{once:true});sb.appendBuffer(data);
});
source.onsourceopen=async()=>{try{
 const sb=source.addSourceBuffer('video/mp4; codecs="avc1.64001e"');
 for(const name of ['a','b']){
  sb.timestampOffset=name==='a'?0:3;
  for(const url of ['/'+name+'-init',...Array.from({length:counts[name]},(_,i)=>'/'+name+'-'+i)])
   await append(sb,await(await fetch(url)).arrayBuffer());
 }
 source.endOfStream();appended=true;document.title='Reconfiguration ready';
}catch(e){document.title='Reconfiguration error '+e;console.error(e);}};
v.onerror=()=>{document.title='Reconfiguration video error '+v.error.code;};
a.onclick=()=>{mode='a';v.pause();v.currentTime=1;};b.onclick=()=>{mode='b';v.pause();v.currentTime=4;};
function check(){if(appended&&mode&&v.readyState>=2&&!v.seeking){
 const ctx=c.getContext('2d');ctx.drawImage(v,0,0,160,96);
 const pixel=ctx.getImageData(30,30,1,1).data;
 const ok=expected[mode].every((value,i)=>Math.abs(value-pixel[i])<6);
 document.title='Reconfiguration '+mode+' '+(ok?'correct':'wrong '+[...pixel].join(','));
}requestAnimationFrame(check);}requestAnimationFrame(check);
</script>'''.replace('COUNTS', json.dumps(counts)).replace('EXPECTED', json.dumps(expected))
        fixture.assets['/reconfiguration'] = page.encode(), 'text/html; charset=utf-8'
        vm = VM(cpus, storage=False, iso=iso, label=label, accel=accel)
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do '
            'sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        vm.send('gui --trace --stats\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm);
        d.screenshot(label + '-start');

        window = d.launch('Browser')
        d.key('l', 'ctrl');
        vm.send(fixture.url('reconfiguration') + '\n')
        d.expect('BROWSER_TITLE', 'Reconfiguration ready')
        left = (d.width - 840) // 2 + (window - 1) % 5 * 16
        top = (d.height - 42 - 560) // 2 + (window - 1) % 5 * 16 + 40
        for i, name in enumerate(('a', 'b', 'a', 'b')):
            d.click(left + (40 if name == 'a' else 140), top + 230)
            d.expect('BROWSER_TITLE', 'Reconfiguration ' + name + ' correct')
            pixels = d.screenshot(label + f'-seek-{i}-{name}')
            at = ((top + 310) * d.width + left + 30) * 3
            assert all(abs(pixels[at + c] - expected[name][c]) < 6 for c in range(3)), name

        d.key('f4', 'alt');
        d.expect('CLOSE');
        d.exit()
        log = Path(vm.log.name).read_text(errors='replace')
        assert 'error while decoding' not in log and 'Invalid NAL' not in log

        print(
            f'PASS MSE reconfiguration: two genuine AVC resolutions, retained samples, repeated old/new seeks, independent decoded canvas pixels ({cpus} CPUs, {accel})', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True);
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--label', default='mse-reconfiguration')
    args = parser.parse_args();
    exercise(args.iso, args.cpus, args.accel, args.label)
