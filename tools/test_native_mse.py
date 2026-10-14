#!/usr/bin/env python3
"""Exercise genuine MSE coded frames, CPU decoding and HDA PCM in the guest."""
import argparse
from array import array
from pathlib import Path
import shlex
import struct
import subprocess
import time
from test_runtime import VM, WORK, assert_success
from test_desktop import DesktopTest
from test_native_media import expect_media_state
from web_fixture import WebFixture

PAGE = b'''<!doctype html><meta charset=utf-8><title>MSE loading</title>
<style>body{margin:0;background:#223344}video{display:block;width:320px;height:192px}
button{position:absolute;top:212px;width:90px;height:40px}canvas{position:absolute;top:270px;left:0}</style>
<video id=v preload=auto></video><button style="left:0" id=play>Play</button>
<button style="left:100px" id=pause>Pause</button><button style="left:200px" id=seek>Seek</button>
<button style="left:300px" id=restart>Restart</button><canvas id=c width=160 height=96></canvas>
<script>
let mode='ready',reported='',source=new MediaSource();v.src=URL.createObjectURL(source);
const video='video/mp4; codecs="avc1.64000b"',audio='audio/mp4; codecs="mp4a.40.2"';
if(!MediaSource.isTypeSupported(video)||!MediaSource.isTypeSupported(audio)||
   MediaSource.isTypeSupported('video/mp4; codecs="av01.0.05M.08"'))throw Error('MSE codec claims');
function operation(sb,action){return new Promise((resolve,reject)=>{
 const ok=()=>{sb.removeEventListener('error',bad);resolve();};
 const bad=()=>{sb.removeEventListener('updateend',ok);reject(Error('source buffer parse'));};
 sb.addEventListener('updateend',ok,{once:true});sb.addEventListener('error',bad,{once:true});action();
});}
const append=(sb,data)=>operation(sb,()=>sb.appendBuffer(data));
source.addEventListener('sourceopen',async()=>{try{
 const manifest=await (await fetch('/mse-manifest')).json();
 const buffers=[];
 for(const [kind,type] of [['video',video],['audio',audio]]){
  const sb=source.addSourceBuffer(type);buffers.push(sb);sb.appendWindowEnd=5;
  await append(sb,await (await fetch('/mse-'+kind+'-init')).arrayBuffer());
  for(let i=0;i<manifest[kind];i++){
   const bytes=new Uint8Array(await (await fetch('/mse-'+kind+'-'+i)).arrayBuffer());
   // Split after a full moof and inside the mdat header/body. appendBuffer
   // must retain the unpaired fragment across separate updateend events.
   let moof=new DataView(bytes.buffer).getUint32(0);
   await append(sb,bytes.slice(0,moof));
   await append(sb,bytes.slice(moof,moof+7));
   await append(sb,bytes.slice(moof+7));
  }
  if(!sb.buffered.length||sb.buffered.end(0)>5.05)throw Error('append window');
  sb.appendWindowEnd=Infinity;sb.appendWindowStart=6;sb.timestampOffset=6;
  await append(sb,await (await fetch('/mse-'+kind+'-0')).arrayBuffer());
  if(sb.buffered.length!==2||sb.buffered.start(1)<5.99)throw Error('timestamp offset');
  await operation(sb,()=>sb.remove(6,8));
  if(sb.buffered.length!==1||sb.buffered.end(0)>5.05)throw Error('remove');
 }
 source.duration=5;source.endOfStream();
 document.title='MSE appended';
}catch(e){document.title='MSE failed '+e;console.error(e);}});
v.onerror=()=>{document.title='MSE failed video '+v.error.code;};
play.onclick=()=>{mode='playing';v.play().catch(e=>{document.title='MSE play failed '+e;});};
pause.onclick=()=>{mode='paused';v.pause();};seek.onclick=()=>{mode='seek';v.pause();v.currentTime=3;};
restart.onclick=()=>{mode='restart';v.pause();v.currentTime=0;};
function sample(){
 if(v.readyState>=2){
  const ctx=c.getContext('2d');ctx.drawImage(v,0,0,160,96);
  const p=ctx.getImageData(30,30,1,1).data;
  const color=p[0]>180&&p[1]<50&&p[2]<50?'red':p[2]>180&&p[0]<50&&p[1]<50?'blue':p[1]>180&&p[0]<50&&p[2]<50?'green':'unknown';
  if(mode!=='ready'&&!v.seeking){
   document.title='MSE '+mode+' '+color;
   const state=mode+' mse '+color;
   if(state!==reported){reported=state;console.log('NATIVE_MEDIA_STATE '+state);}
  }
  else if(source.readyState==='ended'&&v.videoWidth===160&&v.videoHeight===96&&color==='red')document.title='MSE ready 160x96';
 }
 requestAnimationFrame(sample);
}requestAnimationFrame(sample);
</script>'''

BUFFERING_PAGE = b'''<!doctype html><title>MSE buffering loading</title>
<style>body{margin:0}video{width:320px;height:192px}</style><video id=v></video>
<canvas id=c width=160 height=96 style="display:none"></canvas><script>
const source=new MediaSource();v.src=URL.createObjectURL(source);let resumed=false;
const append=(sb,data)=>new Promise((resolve,reject)=>{
 sb.addEventListener('updateend',resolve,{once:true});sb.addEventListener('error',reject,{once:true});sb.appendBuffer(data);
});
source.onsourceopen=async()=>{try{
 const manifest=await(await fetch('/mse-manifest')).json(),buffers=[];
 for(const[kind,type]of[['video','video/mp4; codecs="avc1.64000b"'],['audio','audio/mp4; codecs="mp4a.40.2"']]){
  const sb=source.addSourceBuffer(type);buffers.push([kind,sb]);
  await append(sb,await(await fetch('/mse-'+kind+'-init')).arrayBuffer());
  await append(sb,await(await fetch('/mse-'+kind+'-0')).arrayBuffer());
 }
 const end=Math.min(...buffers.map(([kind,sb])=>sb.buffered.end(0)));
 await v.play();await new Promise(resolve=>setTimeout(resolve,(end+1)*1000));
 const held=v.currentTime;await new Promise(resolve=>setTimeout(resolve,350));
 if(held<end-.15||held>end+.15||Math.abs(v.currentTime-held)>.04)throw Error('clock advanced through underrun '+held+' '+v.currentTime+' end '+end);
 document.title='MSE buffering held';
 for(const[kind,sb]of buffers)for(let i=1;i<manifest[kind];i++)await append(sb,await(await fetch('/mse-'+kind+'-'+i)).arrayBuffer());
 source.endOfStream();resumed=true;
}catch(e){document.title='MSE buffering failed '+e;console.error(e);}};
function check(){if(resumed&&v.currentTime>2.3&&v.readyState>=2){
 const ctx=c.getContext('2d');ctx.drawImage(v,0,0,160,96);const p=ctx.getImageData(30,30,1,1).data;
 if(p[2]>180&&p[0]<50&&p[1]<50)document.title='MSE buffering resumed';
}requestAnimationFrame(check);}requestAnimationFrame(check);
</script>'''

EARLY_SEEK_PAGE = b'''<!doctype html><title>MSE early seek loading</title>
<style>body{margin:0}video{width:320px;height:192px}</style><video id=v></video><script>
const source=new MediaSource();v.src=URL.createObjectURL(source);let requested=false;
const append=(sb,data)=>new Promise((resolve,reject)=>{
 sb.addEventListener('updateend',resolve,{once:true});sb.addEventListener('error',reject,{once:true});sb.appendBuffer(data);
});
v.onloadedmetadata=()=>{if(!requested){requested=true;v.currentTime=.1;v.play().catch(e=>document.title='MSE early play failed '+e);}};
source.onsourceopen=async()=>{try{
 const manifest=await(await fetch('/mse-manifest')).json();
 const audio=source.addSourceBuffer('audio/mp4; codecs="mp4a.40.2"');
 const video=source.addSourceBuffer('video/mp4; codecs="avc1.64000b"');
 await append(audio,await(await fetch('/mse-audio-init')).arrayBuffer());
 await new Promise(resolve=>setTimeout(resolve,100));
 await append(video,await(await fetch('/mse-video-init')).arrayBuffer());
 for(const[kind,sb]of[['audio',audio],['video',video]])for(let i=0;i<manifest[kind];i++)
  await append(sb,await(await fetch('/mse-'+kind+'-'+i)).arrayBuffer());
 source.endOfStream();
}catch(e){document.title='MSE early seek failed '+e;console.error(e);}};
function check(){if(requested&&!v.seeking&&v.currentTime>.5&&v.readyState>=2)
 document.title='MSE early seek playing';requestAnimationFrame(check);}requestAnimationFrame(check);
</script>'''


def fragments(data):
    """Split a real muxed file into init and matched moof/mdat fragments."""
    init = bytearray();
    segments = [];
    pending = bytearray();
    offset = 0
    while offset < len(data):
        length, kind = struct.unpack_from('>I4s', data, offset)
        assert length >= 8 and offset + length <= len(data), (length, kind, offset)
        box = data[offset:offset + length];
        offset += length

        if kind in (b'ftyp', b'moov'):
            init.extend(box)
        elif kind == b'moof':
            assert not pending
            pending.extend(box)
        elif kind == b'mdat':
            assert pending
            pending.extend(box);
            segments.append(bytes(pending));
            pending.clear()

    assert init and segments and not pending
    return bytes(init), segments


def make_assets(fixture):
    command = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y']
    for color in ('red', 'blue', 'lime'):
        command += ['-f', 'lavfi', '-i', f'color=c={color}:s=160x96:r=15:d=2']
    command += ['-filter_complex', '[0:v][1:v][2:v]concat=n=3:v=1:a=0[v]',
                '-map', '[v]', '-an', '-c:v', 'libx264', '-profile:v', 'high', '-pix_fmt', 'yuv420p',
                '-g', '15', '-bf', '2', '-preset', 'fast', '-threads', '2',
                '-movflags', 'empty_moov+default_base_moof+frag_keyframe', str(fixture.directory / 'mse-video.mp4')]
    subprocess.run(command, check=True, timeout=45)
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i',
                    'sine=frequency=440:sample_rate=48000:duration=6', '-vn', '-ac', '2', '-c:a', 'aac',
                    '-movflags', 'empty_moov+default_base_moof', '-frag_duration', '1000000',
                    str(fixture.directory / 'mse-audio.mp4')], check=True, timeout=45)
    import json

    manifest = {}
    for kind in ('video', 'audio'):
        init, segments = fragments((fixture.directory / f'mse-{kind}.mp4').read_bytes())
        manifest[kind] = len(segments)
        fixture.assets[f'/mse-{kind}-init'] = init, 'video/mp4' if kind == 'video' else 'audio/mp4'
        for i, segment in enumerate(segments):
            fixture.assets[f'/mse-{kind}-{i}'] = segment, fixture.assets[f'/mse-{kind}-init'][1]

    fixture.assets['/mse-manifest'] = json.dumps(manifest).encode(), 'application/json'
    fixture.assets['/native-mse'] = PAGE, 'text/html; charset=utf-8'
    fixture.assets['/native-mse-buffering'] = BUFFERING_PAGE, 'text/html; charset=utf-8'
    fixture.assets['/native-mse-early-seek'] = EARLY_SEEK_PAGE, 'text/html; charset=utf-8'

    # Detailed CABAC/B-frame content catches errors that constant-color
    # samples cannot reveal. Compare real guest canvas pixels with an
    # independently decoded host reference at the start and after seeking.
    complex_file = fixture.directory / 'mse-complex.mp4'
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i',
                    'testsrc2=size=426x240:rate=30:duration=6', '-an', '-c:v', 'libx264', '-profile:v', 'high',
                    '-pix_fmt', 'yuv420p', '-g', '60', '-bf', '3', '-refs', '4', '-threads', '2',
                    '-movflags', 'empty_moov+default_base_moof+frag_keyframe', str(complex_file)], check=True, timeout=45)
    init, segments = fragments(complex_file.read_bytes())
    fixture.assets['/complex-init'] = init, 'video/mp4'
    for i, segment in enumerate(segments):
        fixture.assets[f'/complex-{i}'] = segment, 'video/mp4'
    coordinates = [(x, y) for y in (25, 65, 105, 145, 185, 225)
                   for x in (20, 65, 110, 155, 200, 245, 290, 335, 380, 415)]
    references = []
    first_time = float(subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                                                '-show_entries', 'stream=start_time', '-of', 'default=noprint_wrappers=1:nokey=1', str(complex_file)], timeout=30))
    for seconds in (0, 3):
        # ffmpeg -ss normalizes the file's start time; MSE retains coded PTS.
        raw = subprocess.check_output(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-ss', str(max(0, seconds - first_time)),
                                       '-i', str(complex_file), '-frames:v', '1', '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-'], timeout=30)
        assert len(raw) == 426 * 240 * 3
        references.append([list(raw[(y * 426 + x) * 3:(y * 426 + x) * 3 + 3])
                          for x, y in coordinates])

    html = '''<!doctype html><title>Complex MSE loading</title><style>body{margin:0}video{width:426px;height:240px}
button{position:absolute;left:445px;top:205px;width:80px;height:35px}</style>
<video id=v></video><button id=seek>Seek</button><canvas id=c width=426 height=240 style="display:none"></canvas>
<script>
const points=POINTS,expected=EXPECTED,source=new MediaSource();let mode=0,appended=false,frames=0,lastFrame=0;
v.requestVideoFrameCallback(function next(now,metadata){
 if(metadata.width!==426||metadata.height!==240||metadata.presentedFrames<=lastFrame||!Number.isFinite(metadata.mediaTime)||metadata.mediaTime<0||metadata.mediaTime>v.duration+.1)
  throw Error('MSE frame metadata');
 lastFrame=metadata.presentedFrames;frames++;v.requestVideoFrameCallback(next);
});
v.src=URL.createObjectURL(source);
source.onsourceopen=async()=>{try{
 const sb=source.addSourceBuffer('video/mp4; codecs="avc1.64001e"');
 for(const url of ['/complex-init',...Array.from({length:COUNT},(_,i)=>'/complex-'+i)]){
  const data=await (await fetch(url)).arrayBuffer();
  await new Promise((resolve,reject)=>{sb.addEventListener('updateend',resolve,{once:true});sb.addEventListener('error',reject,{once:true});sb.appendBuffer(data);});
 }
 source.endOfStream();appended=true;
}catch(e){document.title='Complex MSE error '+e;console.error(e);}};
seek.onclick=()=>{mode=1;v.pause();v.currentTime=3;};
function check(){if(appended&&frames>mode&&v.readyState>=2&&!v.seeking&&(mode===0||v.currentTime>=3)){
 const ctx=c.getContext('2d');ctx.drawImage(v,0,0);let error=0,maximum=0;
 for(let i=0;i<points.length;i++){
  const p=ctx.getImageData(points[i][0],points[i][1],1,1).data;
  for(let j=0;j<3;j++){const delta=Math.abs(p[j]-expected[mode][i][j]);error+=delta;maximum=Math.max(maximum,delta);}
 }
 error/=points.length*3;
 document.title=error<5&&maximum<24?'Complex MSE '+(mode?'seek correct':'ready'):'Complex MSE pixels wrong '+error.toFixed(2)+' max '+maximum;
}requestAnimationFrame(check);}requestAnimationFrame(check);
</script>'''.replace('POINTS', json.dumps(coordinates)).replace('EXPECTED', json.dumps(references)).replace('COUNT', str(len(segments)))
    fixture.assets['/native-mse-complex'] = html.encode(), 'text/html; charset=utf-8'


def exercise(cpus, iso, accel, audio_device):
    fixture = WebFixture(webcore=True);
    vm = None
    label = f'native-mse-{audio_device}-{accel}'
    capture = WORK / f'{label}-{cpus}cpu.wav';
    capture.unlink(missing_ok=True)

    try:
        make_assets(fixture)
        vm = VM(cpus, storage=False, iso=iso, label=label, accel=accel,
                audio_capture=capture, audio_device=audio_device)
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        assert_success(vm.command(
            'build-sh -c ' + shlex.quote(f'curl -fsS --max-time 30 {fixture.url("ca.pem")} >> /etc/ssl/cert.pem'), timeout=40))
        vm.send('gui --trace --stats\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm);
        d.screenshot(label + f'-start-{cpus}cpu');

        window = d.launch('Browser')
        left = (d.width - 840) // 2 + (window - 1) % 5 * 16;
        top = (d.height - 42 - 560) // 2 + (window - 1) % 5 * 16;
        body = top + 40
        d.key('l', 'ctrl');
        vm.send(fixture.url('native-mse', tls=True) + '\n')
        d.expect('BROWSER_TITLE', 'MSE ready 160x96')
        after_play = Path(vm.log.name).stat().st_size
        d.click(left + 40, body + 230)
        after_red = expect_media_state(vm, 'playing mse red', after_play)
        expect_media_state(vm, 'playing mse blue', after_red)
        d.click(left + 140, body + 230);
        d.expect('BROWSER_TITLE', 'MSE paused blue')
        before = d.screenshot(label + f'-paused-{cpus}cpu');
        time.sleep(.3);
        after = d.screenshot(label + f'-paused-again-{cpus}cpu')
        for x, y in ((40, 40), (40, 300)):
            offset = ((body + y) * d.width + left + x) * 3;
            pixel = tuple(after[offset:offset + 3])
            assert pixel[2] > 180 and max(pixel[:2]) < 50, pixel
            assert before[offset:offset + 3] == after[offset:offset + 3]

        d.click(left + 340, body + 230);
        d.expect('BROWSER_TITLE', 'MSE restart red')
        d.click(left + 240, body + 230);
        d.expect('BROWSER_TITLE', 'MSE seek blue')
        after_play = Path(vm.log.name).stat().st_size
        d.click(left + 40, body + 230);
        expect_media_state(vm, 'playing mse green', after_play)
        d.key('l', 'ctrl');
        vm.send(fixture.url('native-mse-complex', tls=True) + '\n')
        d.expect('BROWSER_TITLE', 'Complex MSE ready')
        d.screenshot(label + f'-complex-start-{cpus}cpu')
        d.click(left + 485, body + 222);
        d.expect('BROWSER_TITLE', 'Complex MSE seek correct')
        d.screenshot(label + f'-complex-seek-{cpus}cpu')
        d.key('l', 'ctrl');
        vm.send(fixture.url('native-mse-early-seek', tls=True) + '\n')
        d.expect('BROWSER_TITLE', 'MSE early seek playing')
        d.key('l', 'ctrl');
        vm.send(fixture.url('native-mse-buffering', tls=True) + '\n')
        d.expect('BROWSER_TITLE', 'MSE buffering held')
        d.expect('BROWSER_TITLE', 'MSE buffering resumed')
        d.key('f4', 'alt');
        d.expect('CLOSE', f'window={window}');
        d.exit();
        vm.close();
        vm = None
        data = capture.read_bytes();
        assert data[:4] == b'RIFF' and data[8:16] == b'WAVEfmt ' and data[36:40] == b'data'
        assert struct.unpack_from('<HH', data, 20) == (
            1, 2) and struct.unpack_from('<H', data, 34) == (16,)

        rate = struct.unpack_from('<I', data, 24)[0];
        samples = array('h', data[44:])
        assert sum(abs(value) > 100 for value in samples) > rate
        assert sum(a < 0 <= b for a, b in zip(samples[::2], samples[2::2])) > 440
        assert 'MSE frame metadata' not in (
            WORK / f'uefi-{cpus}cpu-{label}.log').read_text(errors='replace')

        print(
            f'PASS native MSE: fragmented H.264 B-frames + separate AAC, partial appends, windows/offset/remove, pixel playback/canvas, pause, seek/restart, early metadata seek, independent CABAC pixel comparison, actual video-frame callbacks, buffering clock hold/resume, actual PCM ({cpus} CPUs, {accel}, {audio_device})', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True);
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--accel', choices=['kvm', 'tcg'], default='tcg')
    parser.add_argument('--audio-device', choices=['ac97', 'hda'], default='hda')
    args = parser.parse_args()

    for cpus in args.cpus:
        exercise(cpus, args.iso, args.accel, args.audio_device)
