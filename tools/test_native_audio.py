#!/usr/bin/env python3
"""Native audio-only AAC/MP3/Opus playback, pause/seek and actual device PCM."""
import argparse
from array import array
from pathlib import Path
import shlex
import struct
import subprocess
from test_runtime import VM, WORK, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture

PAGE = b'''<!doctype html><meta charset=utf-8><title>Audio loading</title>
<style>body{margin:0;background:#223344}button{position:absolute;top:10px;width:100px;height:40px}</style>
<audio id=a preload=auto></audio><button style="left:0" id=p>Play</button>
<button style="left:110px" id=s>Pause</button><button style="left:220px" id=k>Seek</button>
<button style="left:330px" id=b>Busy script</button>
<script>
const kind=new URLSearchParams(location.search).get('kind');let mode='ready';
if(!a.canPlayType('audio/mp4; codecs="mp4a.40.2"') ||
   !a.canPlayType('audio/mpeg') || !a.canPlayType('audio/webm; codecs="opus"') ||
   a.canPlayType('audio/mp4; codecs="av01.0.05M.08"'))throw Error('audio codec claims');
a.src='/native-audio.'+kind;
a.onloadeddata=()=>{if(a.duration>4.9&&a.duration<5.2)document.title='Audio ready '+kind;};
a.onerror=()=>{document.title='Audio failed '+kind+' '+a.error.code;};
p.onclick=()=>{mode=a.currentTime>1.8?'resumed':'playing';a.play().catch(e=>{document.title='Audio failed '+e;});};
s.onclick=()=>{mode='paused';a.pause();const t=a.currentTime;
 setTimeout(()=>{document.title=Math.abs(a.currentTime-t)<.02?'Audio paused '+kind:'Audio clock failed';},300);};
k.onclick=()=>{mode='seek';a.pause();a.addEventListener('seeked',()=>{document.title='Audio seek '+kind;},{once:true});a.currentTime=2;};
b.onclick=()=>{mode='busy';const before=a.currentTime,until=performance.now()+800;
 while(performance.now()<until){}
 const elapsed=a.currentTime-before;
 document.title=elapsed>=0&&elapsed<.4?'Audio clock held '+kind:'Audio clock skipped '+elapsed;};
function check(){if(!a.paused&&!a.seeking&&((mode==='playing'&&a.currentTime>.5)||(mode==='resumed'&&a.currentTime>2.5)))document.title='Audio '+mode+' '+kind;requestAnimationFrame(check);}
requestAnimationFrame(check);
</script>'''


def exercise(iso, cpus, accel, audio_device):
    fixture = WebFixture();
    vm = None
    label = f'native-audio-{accel}-{audio_device}';
    capture = WORK / f'{label}-{cpus}cpu.wav'
    capture.unlink(missing_ok=True)

    try:
        fixture.assets['/native-audio'] = PAGE, 'text/html; charset=utf-8'
        for kind, mime, codec in [('m4a', 'audio/mp4', 'aac'), ('mp3', 'audio/mpeg', 'libmp3lame'), ('webm', 'audio/webm', 'libopus')]:
            path = fixture.directory / ('native-audio.' + kind)
            subprocess.run(['ffmpeg', '-v', 'error', '-y',
                            '-f', 'lavfi', '-i', 'sine=frequency=440:duration=1:sample_rate=48000',
                            '-f', 'lavfi', '-i', 'sine=frequency=880:duration=1:sample_rate=48000',
                            '-f', 'lavfi', '-i', 'sine=frequency=660:duration=3:sample_rate=48000',
                            '-filter_complex', '[0:a][1:a][2:a]concat=n=3:v=0:a=1[a]',
                            '-map', '[a]', '-ac', '2', '-c:a', codec, str(path)], check=True)
            fixture.assets['/native-audio.' + kind] = path.read_bytes(), mime

        vm = VM(cpus, storage=False, iso=iso, label=label, accel=accel,
                audio_capture=capture, audio_device=audio_device)
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        assert_success(vm.command(
            'build-sh -c ' + shlex.quote(f'curl -fsS --max-time 30 {fixture.url("ca.pem")} >> /etc/ssl/cert.pem'), timeout=40))
        vm.send('gui --trace --stats\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm);
        d.screenshot(label + '-start');

        window = d.launch('Browser')
        left = (d.width - 840) // 2 + (window - 1) % 5 * 16;
        body = (d.height - 42 - 560) // 2 + (window - 1) % 5 * 16 + 40
        for kind in ('m4a', 'mp3', 'webm'):
            d.key('l', 'ctrl');
            vm.send(fixture.url('native-audio?kind=' + kind, tls=kind == 'webm') + '\n')
            d.expect('BROWSER_TITLE', 'Audio ready ' + kind)
            d.click(left + 50, body + 30);
            d.expect('BROWSER_TITLE', 'Audio playing ' + kind)
            d.click(left + 380, body + 30);
            d.expect('BROWSER_TITLE', 'Audio clock held ' + kind)
            d.click(left + 160, body + 30);
            d.expect('BROWSER_TITLE', 'Audio paused ' + kind)
            d.click(left + 270, body + 30);
            d.expect('BROWSER_TITLE', 'Audio seek ' + kind)
            d.click(left + 50, body + 30);
            d.expect('BROWSER_TITLE', 'Audio resumed ' + kind)

        assert len(fixture.ranges) >= 3, fixture.ranges
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
        samples = array('h', data[44:]);
        left_samples = samples[::2]

        assert sum(abs(value) > 100 for value in samples) > rate
        frequencies = []
        for offset in range(0, len(left_samples) - 2048, 2048):
            chunk = left_samples[offset:offset + 2048]
            if sum(abs(value) > 100 for value in chunk) > 1900:
                frequencies.append(sum(a < 0 <= b for a, b in zip(chunk, chunk[1:])) * rate / 2048)

        assert any(abs(f - 440) < 40 for f in frequencies), frequencies
        assert any(abs(f - 660) < 40 for f in frequencies), frequencies

        print(
            f'PASS native audio-only AAC/MP3/Opus: genuine codec detection, metadata, device clock during busy JavaScript, pause/seek, HTTP/TLS ranges and captured 440/660-Hz stereo PCM ({cpus} CPUs, {accel}, {audio_device})', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--iso', required=True);
    p.add_argument('--cpus', type=int, default=4)
    p.add_argument('--accel', choices=('kvm', 'tcg'), default='tcg')
    p.add_argument('--audio-device', choices=('ac97', 'hda'), default='hda')
    a = p.parse_args();
    exercise(a.iso, a.cpus, a.accel, a.audio_device)
