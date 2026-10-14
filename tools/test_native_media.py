#!/usr/bin/env python3
"""Decode actual MP4/WebM frames and PCM in the Limine UEFI guest."""
import argparse
from array import array
from pathlib import Path
import subprocess
import re
import shlex
import struct
import time
import wave
from test_runtime import VM, WORK, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture

PAGE = b'''<!doctype html><meta charset=utf-8><title>Media loading</title>
<style>body{margin:0;background:#223344}video{display:block;width:320px;height:192px}
button{position:absolute;top:212px;width:90px;height:40px}canvas{position:absolute;top:270px;left:0}</style>
<video id=v preload=auto></video><button style="left:0" id=play>Play</button>
<button style="left:100px" id=pause>Pause</button><button style="left:200px" id=seek>Seek</button>
<button style="left:300px" id=restart>Restart</button><canvas id=c width=160 height=96></canvas>
<script>
const query=new URLSearchParams(location.search),kind=query.get('kind')||'mp4';
if(v.canPlayType('video/mp4; codecs="avc1.42E01E, mp4a.40.2"')!=='probably' ||
   v.canPlayType('video/webm; codecs="vp9, opus"')!=='probably' ||
   v.canPlayType('video/mp4; codecs="av01.0.05M.08"')!=='')throw Error('codec support claims');
v.src='/native-media.'+kind;
let mode='ready',frames=0,lastFrame=0,reported='';
const cancelled=v.requestVideoFrameCallback(()=>{throw Error('cancelled video frame callback ran');});
v.cancelVideoFrameCallback(cancelled);
v.requestVideoFrameCallback(function next(now,metadata){
 if(!Number.isFinite(now)||metadata.width!==v.videoWidth||metadata.height!==v.videoHeight||
    !Number.isFinite(metadata.mediaTime)||metadata.mediaTime<0||metadata.mediaTime>v.duration+.1||metadata.presentedFrames<=lastFrame||
    !Number.isFinite(metadata.presentationTime)||metadata.expectedDisplayTime<metadata.presentationTime)
   throw Error('actual video-frame metadata');
 lastFrame=metadata.presentedFrames;frames++;v.requestVideoFrameCallback(next);
});
v.onloadeddata=()=>{document.title='Media ready '+kind+' '+v.videoWidth+'x'+v.videoHeight;};
v.onerror=()=>{document.title='Media failed '+kind+' '+v.error.code;};
play.onclick=()=>{mode='playing';v.play().catch(e=>{document.title='Media play failed '+e;});};
pause.onclick=()=>{mode='paused';v.pause();};seek.onclick=()=>{mode='seek';v.pause();v.currentTime=3;};
restart.onclick=()=>{mode='restart';v.pause();v.currentTime=0;};
function sample(){
 if(v.readyState>=2){
  let ctx=c.getContext('2d');ctx.drawImage(v,0,0,160,96);
  let p=ctx.getImageData(30,30,1,1).data;
  let color=p[0]>180&&p[1]<50&&p[2]<50?'red':p[2]>180&&p[0]<50&&p[1]<50?'blue':p[1]>180&&p[0]<50&&p[2]<50?'green':'unknown';
  if(mode!=='ready'&&!v.seeking&&(mode!=='playing'||frames>=2)){
   document.title='Media '+mode+' '+kind+' '+color+' at '+v.currentTime.toFixed(3);
   const state=mode+' '+kind+' '+color;
   if(state!==reported){reported=state;console.log('NATIVE_MEDIA_STATE '+state);}
  }
 }
 requestAnimationFrame(sample);
}requestAnimationFrame(sample);
</script>'''


def expect_media_state(vm, state, after, timeout=20):
    """Observe a real canvas sample without relying on a transient title.

    WebCore may coalesce document.title updates before notifying the desktop.
    Console observations retain every sampled color transition. The action's
    serial-log offset excludes earlier playback, and also retains observations
    already read alongside a preceding match by VM.until.
    """
    pattern = (rb'DESKTOP BROWSER_LOG[^\n]* NATIVE_MEDIA_STATE ' +
               re.escape(state.encode()) + rb'\r?\n')
    observed = Path(vm.log.name).read_bytes()[after:]
    match = re.search(pattern, observed)
    if not match:
        vm.until(pattern, timeout)
        observed = Path(vm.log.name).read_bytes()[after:]
        match = re.search(pattern, observed)

    assert match, 'No media state was recorded after the triggering action: ' + state
    return after + match.end()


def make_clips(directory):
    common = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y']
    for color in ('red', 'blue', 'lime'):
        common += ['-f', 'lavfi', '-i', f'color=c={color}:s=426x240:r=15:d=2']
    common += ['-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=6',
               '-filter_complex', '[0:v][1:v][2:v]concat=n=3:v=1:a=0[v]',
               '-map', '[v]', '-map', '3:a', '-ac', '2', '-ar', '48000', '-threads', '2']
    for kind, options in [('mp4', ['-c:v', 'libx264', '-profile:v', 'baseline', '-pix_fmt', 'yuv420p',
                                   '-g', '15', '-bf', '0', '-preset', 'ultrafast', '-c:a', 'aac', '-movflags', '+faststart']),
                          ('webm', ['-c:v', 'libvpx-vp9', '-g', '15', '-deadline', 'realtime', '-cpu-used', '8', '-c:a', 'libopus'])]:
        subprocess.run(common + options + [str(directory / f'native-media.{kind}')], check=True, timeout=45)


def exercise(cpus, iso, accel, audio_device='ac97'):
    fixture = WebFixture(webcore=True);
    vm = None
    capture = WORK / f'native-media-{audio_device}-{accel}-{cpus}cpu.wav'
    WORK.mkdir(parents=True, exist_ok=True);
    capture.unlink(missing_ok=True)

    try:
        make_clips(fixture.directory)
        fixture.assets['/native-media'] = PAGE, 'text/html; charset=utf-8'
        for kind, mime in [('mp4', 'video/mp4'), ('webm', 'video/webm')]:
            fixture.assets[f'/native-media.{kind}'] = (fixture.directory / f'native-media.{kind}').read_bytes(), mime
        vm = VM(cpus, storage=False, iso=iso, label=f'native-media-{audio_device}-{accel}', accel=accel, audio_capture=capture, audio_device=audio_device)
        assert_success(vm.command('build-sh -c ' + shlex.quote('i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        assert_success(vm.command('build-sh -c ' + shlex.quote(f'curl -fsS --max-time 30 {fixture.url("ca.pem")} >> /etc/ssl/cert.pem'), timeout=40))
        vm.send('gui --trace --stats\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm);
        d.screenshot(f'native-media-start-{accel}-{cpus}cpu')

        window = d.launch('Browser')
        left = (d.width - 840) // 2 + (window - 1) % 5 * 16
        top = (d.height - 42 - 560) // 2 + (window - 1) % 5 * 16;
        body = top + 40
        for kind in ('mp4', 'webm'):
            d.key('l', 'ctrl');
            vm.send(fixture.url(f'native-media?kind={kind}', tls=kind == 'webm') + '\n')
            d.expect('BROWSER_TITLE', f'Media ready {kind} 426x240')
            after_play = Path(vm.log.name).stat().st_size
            d.click(left + 40, body + 230)
            after_red = expect_media_state(vm, f'playing {kind} red', after_play)
            expect_media_state(vm, f'playing {kind} blue', after_red)
            d.click(left + 140, body + 230);
            d.expect('BROWSER_TITLE', f'Media paused {kind} blue')

            # Pausing must stop the clock and preserve the decoded frame.
            before = d.screenshot(f'native-media-pause-{kind}-{accel}-{cpus}cpu');
            time.sleep(.3)
            after = d.screenshot(f'native-media-paused-{kind}-{accel}-{cpus}cpu')
            for x, y in [(40, 40), (40, 300)]:
                index = ((body + y) * d.width + left + x) * 3
                actual = tuple(after[index:index + 3]);
                assert actual[2] > 180 and max(actual[:2]) < 50, actual
                assert before[index:index + 3] == after[index:index + 3]

            d.click(left + 340, body + 230);
            d.expect('BROWSER_TITLE', f'Media restart {kind} red')
            d.click(left + 240, body + 230);
            d.expect('BROWSER_TITLE', f'Media seek {kind} blue')
            after_play = Path(vm.log.name).stat().st_size
            d.click(left + 40, body + 230);
            expect_media_state(vm, f'playing {kind} green', after_play)

        assert len(fixture.ranges) >= 2, fixture.ranges
        d.key('f4', 'alt');
        d.expect('CLOSE', f'window={window}');
        d.exit()
        vm.close();
        vm = None

        # QEMU's WAV backend leaves zero RIFF lengths when its VM is terminated.
        # Check the captured PCM format, then finalize lengths from actual bytes.
        recording = bytearray(capture.read_bytes())
        assert recording[:4] == b'RIFF' and recording[8:16] == b'WAVEfmt ' and recording[36:40] == b'data'
        assert struct.unpack_from('<HH', recording, 20) == (
            1, 2) and struct.unpack_from('<H', recording, 34) == (16,)
        assert (len(recording) - 44) % 4 == 0

        struct.pack_into('<I', recording, 4, len(recording) - 8)
        struct.pack_into('<I', recording, 40, len(recording) - 44)
        capture.write_bytes(recording)

        with wave.open(str(capture), 'rb') as sound:
            assert sound.getnchannels() == 2 and sound.getsampwidth() == 2
            rate = sound.getframerate();
            samples = array('h', sound.readframes(sound.getnframes()))

        # Require actual non-silent DMA output, not an audio capability flag.
        nonzero = sum(abs(value) > 100 for value in samples)
        assert nonzero > rate * 6, (rate, len(samples), nonzero)
        crossings = sum(a < 0 <= b for a, b in zip(samples[::2], samples[2::2]))
        assert crossings > 440, crossings
        log = (
            WORK / f'uefi-{cpus}cpu-native-media-{audio_device}-{accel}.log').read_text(errors='replace')
        assert 'cancelled video frame callback ran' not in log and 'actual video-frame metadata' not in log

        print(
            f'PASS native MP4/H.264/AAC and WebM/VP9/Opus: decoded video/canvas pixels, actual video-frame callbacks/cancellation, play, pause, seek/restart, HTTP/TLS ranges, real stereo PCM ({cpus} CPUs, {accel})', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--iso', required=True)
    parser.add_argument('--accel', choices=['kvm', 'tcg'], default='tcg')
    parser.add_argument('--audio-device', choices=['ac97', 'hda'], default='ac97')
    args = parser.parse_args()

    for cpus in args.cpus:
        exercise(cpus, args.iso, args.accel, args.audio_device)
