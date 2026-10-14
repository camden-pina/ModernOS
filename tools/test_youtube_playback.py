#!/usr/bin/env python3
"""Optional live YouTube test: real guest player pixels and captured HDA sound."""
import argparse
from array import array
from pathlib import Path
import shlex
import struct
import time
from diagnose_browser_sites import capture_navigation
from test_runtime import VM, WORK, assert_success
from test_desktop import DesktopTest


def video_patch(pixels, width, height):
    # The maximized watch page places the player above titles/recommendations.
    # Keep the right edge inside the player: recommendation thumbnails start
    # around 71% of the viewport and can otherwise look like decoded video.
    return b''.join(pixels[(y * width + int(width * .30)) * 3:(y * width + int(width * .65)) * 3]
                    for y in range(int(height * .14), int(height * .60), 12))


def rendered_play_button(pixels, width, height):
    # The 19-second fixture can finish while TCG is still loading site scripts.
    # Recognize YouTube's actual dark center button with a white play triangle.
    cx = int(width * .571);
    cy = int(height * .514)
    for y in range(cy - 28, cy + 29, 4):
        for x in range(cx - 28, cx + 29, 4):
            if min(pixels[(y * width + x) * 3:(y * width + x) * 3 + 3]) < 220:
                continue
            samples = [pixels[(py * width + px) * 3:(py * width + px) * 3 + 3]
                       for py in range(y - 20, y + 21, 2) for px in range(x - 30, x + 31, 2)]
            dark = sum(max(p) < 85 for p in samples);
            white = sum(min(p) > 220 for p in samples)
            if dark > len(samples) * .82 and 10 < white < 150:
                return x, y
    return None


def exercise(iso, cpus, accel, url, clip='short'):
    label = f'youtube-{clip}-{accel}';
    capture = WORK / f'{label}-{cpus}cpu.wav'
    capture.unlink(missing_ok=True)
    vm = VM(cpus, storage=False, iso=iso, label=label,
            accel=accel, audio_capture=capture, audio_device='hda')
    try:
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        vm.send('gui --trace --stats\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm);
        d.screenshot(label + '-start');

        window = d.launch('Browser')
        left = (d.width - 840) // 2 + (window - 1) % 5 * 16;
        top = (d.height - 42 - 560) // 2 + (window - 1) % 5 * 16
        d.click(left + 797, top + 20);
        d.expect('MAXIMIZE');
        d.key('l', 'ctrl');
        vm.pending.clear();
        vm.send(url + '\n')
        row = capture_navigation(vm, window, 150 if accel == 'tcg' else 45,
                                 minimum_settle=1, expected_title='YouTube')
        assert row.get('title', '').endswith('YouTube') and not row.get('browser_error'), row

        # Snapshots sample the video's interior, away from controls, title,
        # channel and recommendation columns. A static poster cannot pass.
        # TCG executes the same multi-megabyte site scripts with software CPU
        # translation. Wait for actual video detail before measuring motion.
        deadline = time.monotonic() + (150 if accel == 'tcg' else 60)
        while time.monotonic() < deadline:
            pixels = d.screenshot(label + '-waiting')
            patch = video_patch(pixels, d.width, d.height)
            if len({patch[offset:offset + 3] for offset in range(0, len(patch), 24)}) > 128:
                break
            capture_navigation(vm, window, 1, expect_loading=False, minimum_settle=0)
        else:
            raise AssertionError('No decoded detail appeared inside the actual video player')

        button = rendered_play_button(pixels, d.width, d.height)
        if button:
            d.click(*button)
            print(f'Clicked the actual rendered player button at {button}', flush=True)

        # Video's first image can be displayed while the separate audio
        # SourceBuffer is still processing. Require actual device output
        # before measuring sustained decoded motion. No script or clock is
        # altered, and a loading animation cannot establish playback.
        deadline = time.monotonic() + (180 if accel == 'tcg' else 60)
        while capture.stat().st_size < 44 and time.monotonic() < deadline:
            capture_navigation(vm, window, 1, expect_loading=False, minimum_settle=0)
        header = capture.read_bytes()[:44]
        assert len(
            header) == 44 and header[:4] == b'RIFF', 'The real audio device produced no recording'
        recording_rate = struct.unpack_from('<I', header, 24)[0]
        minimum_audio = 44 + recording_rate * 4 * 5
        while capture.stat().st_size < minimum_audio and time.monotonic() < deadline:
            capture_navigation(vm, window, 1, expect_loading=False, minimum_settle=0)

        assert capture.stat().st_size >= minimum_audio, 'The real audio device did not sustain five seconds of playback'
        patches = [];
        details = [];
        moving = 0
        deadline = time.monotonic() + (90 if accel == 'tcg' else 30)
        for i in range(120):
            pixels = d.screenshot(label + f'-frame-{i}')
            patch = video_patch(pixels, d.width, d.height)
            palette = {patch[offset:offset + 3] for offset in range(0, len(patch), 24)}

            # Intro fades and loading surfaces can change many pixels while
            # remaining nearly monochrome. Require detail in both frames so
            # these transitions cannot end observation before decoded motion.
            if patches and details[-1] > 128 and len(palette) > 128 and sum(a != b for a, b in zip(patches[-1], patch)) > len(patch) // 40:
                moving += 1
            patches.append(patch);
            details.append(len(palette))

            if i >= 9 and (moving >= 3 or time.monotonic() >= deadline):
                break
            time.sleep(.7)

        assert max(details) > 128, details
        assert moving >= 3, (moving, details)
        d.screenshot(label + '-playing')
        d.key('f4', 'alt');
        d.expect('CLOSE', f'window={window}');
        d.exit();
        log_path = Path(vm.log.name);
        vm.close();
        vm = None
        data = capture.read_bytes()
        assert data[:4] == b'RIFF' and data[8:16] == b'WAVEfmt ' and data[36:40] == b'data'
        assert struct.unpack_from('<HH', data, 20) == (
            1, 2) and struct.unpack_from('<H', data, 34) == (16,)

        rate = struct.unpack_from('<I', data, 24)[0];
        sound = array('h', data[44:])
        assert sum(abs(value) > 100 for value in sound) > rate
        log = log_path.read_text(errors='replace')
        assert 'googlevideo.com/videoplayback' in log and 'DecodeError' not in log

        print(f'PASS live YouTube: {row["title"]}, {moving}/{len(patches)-1} decoded player changes after five seconds of actual HDA stereo sound; recorded {len(sound)/2/rate:.2f} seconds ({cpus} CPUs, {accel})', flush=True)
    finally:
        if vm:
            vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True);
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--accel', choices=['kvm', 'tcg'], default='kvm')
    parser.add_argument('--url', default='https://www.youtube.com/watch?v=jNQXAC9IVRw')
    parser.add_argument('--clip', choices=('short', 'long'), default='short',
                        help='Distinct artifact label; --url selects the video')
    args = parser.parse_args();

    exercise(args.iso, args.cpus, args.accel, args.url, args.clip)
