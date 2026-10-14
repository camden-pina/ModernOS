#!/usr/bin/env python3
"""Check real image decoding, ISO assets and wallpaper settings through USB input."""
import argparse
import os
import re
import shlex
import struct
import subprocess
import tarfile
import zlib

from test_runtime import ROOT, VM, WORK, assert_success, prepare
from test_desktop import DesktopTest, settings_focus

MAGIC = 0x3150574D
ASSETS = ROOT / 'userland/desktop/assets/wallpapers'
COLORS = [(220, 40, 70), (20, 190, 90), (40, 100, 220), (230, 190, 30)]


def png(width, height, pixels):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + bytes(pixels[y * width * 4:(y + 1) * width * 4]) for y in range(height))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def bmp(width, height, pixels, top_down=False):
    stride = (width * 3 + 3) & ~3
    header = struct.pack('<2sIHHI', b'BM', 54 + stride * height, 0, 0, 54)
    header += struct.pack('<IiiHHIIiiII', 40, width, -height if top_down else height,
                          1, 24, 0, stride * height, 0, 0, 0, 0)
    rows = range(height) if top_down else reversed(range(height))
    return header + b''.join(b''.join(bytes(reversed(pixels[y * width + x])) for x in range(width))
                             + bytes(stride - width * 3) for y in rows)


def decoder_tests():
    folder = WORK / 'wallpaper-decoder'
    folder.mkdir(parents=True, exist_ok=True)
    exe = ROOT / 'userland/bin/wallpaper-decode'

    def decode(name, content, expected=None):
        path = folder / name
        path.write_bytes(content)
        result = subprocess.run([str(exe), str(path)], capture_output=True, timeout=15)
        assert len(result.stdout) >= 16, result.stderr
        magic, width, height, error = struct.unpack('<IIIi', result.stdout[:16])
        assert magic == MAGIC

        if expected is None:
            assert result.returncode and error < 0 and not width and not height and len(
                result.stdout) == 16
        else:
            w, h, colors = expected
            assert not result.returncode and not error and (width, height) == (w, h)
            assert len(result.stdout) == 16 + w * h * 4
            actual = struct.unpack('<' + 'I' * (w * h), result.stdout[16:])
            assert actual == tuple(colors), (name, actual, colors)

    rgba = [255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 7, 9, 11, 255]
    encoded = png(2, 2, rgba)
    decode('alpha.png', encoded, (2, 2, [0xffff0000, 0x8000ff00, 0x000000ff, 0xff07090b]))
    pixels = [(3, 9, 21), (201, 101, 51)]
    for top_down in (False, True):
        decode(f'padded-{top_down}.bmp', bmp(1, 2, pixels, top_down),
               (1, 2, [0xff030915, 0xffc96533]))
    decode('truncated.png', encoded[:40])
    decode('invalid.txt', b'not an image at all')
    broken = bytearray(bmp(1, 2, pixels))
    struct.pack_into('<i', broken, 18, 8193)
    decode('oversized.bmp', broken)
    decode('truncated.bmp', bmp(1, 2, pixels)[:-1])
    from PIL import Image

    jpeg = folder / 'sample.jpg'
    Image.new('RGB', (19, 7), (84, 133, 194)).save(jpeg, quality=100)
    result = subprocess.run([str(exe), str(jpeg)], capture_output=True, timeout=15, check=True)
    assert struct.unpack('<IIIi', result.stdout[:16]) == (MAGIC, 19, 7, 0)
    for value in struct.unpack('<' + 'I' * (19 * 7), result.stdout[16:]):
        channels = [(value >> shift) & 255 for shift in (16, 8, 0)]
        assert all(abs(a - b) <= 2 for a, b in zip(channels, (84, 133, 194))) and value >> 24 == 255
    decode('truncated.jpg', jpeg.read_bytes()[:100])

    for name in ('alpine-lake.png', 'desert-sunset.png'):
        path = ASSETS / name
        result = subprocess.run([str(exe), str(path)], capture_output=True, check=True, timeout=15)
        w, h = Image.open(path).size
        assert struct.unpack('<IIIi', result.stdout[:16]) == (MAGIC, w, h, 0)
        assert len(result.stdout) == 16 + w * h * 4

    print('PASS wallpaper decoder: PNG alpha, JPEG, BMP row padding/orientation, invalid and oversized images', flush=True)


def packaged_assets():
    with tarfile.open(os.environ.get('MODERNOS_INITRAMFS') or ROOT / 'build/initramfs.tar') as archive:
        assert archive.getmember('bin/wallpaper-decode').mode & 0o111
        assert archive.getmember('usr/share/wallpapers').isdir()
        for name in ('alpine-lake.png', 'desert-sunset.png'):
            assert archive.extractfile('usr/share/wallpapers/' + name).read() == (ASSETS / name).read_bytes()
    print('PASS ISO initramfs: both original sample PNGs and the native decoder', flush=True)


def guest_write(vm, path, data):
    # BusyBox ash/printf write binary fixtures through ordinary guest APIs.
    for offset in range(0, len(data), 64):
        escaped = ''.join('\\%03o' % b for b in data[offset:offset + 64])
        script = 'printf ' + shlex.quote(escaped) + (' > ' if not offset else ' >> ') + shlex.quote(path)
        command = 'build-sh -c ' + shlex.quote(script)
        assert len(command.encode()) < 511
        assert_success(vm.command(command))


def guest(cpus, iso=None):
    vm = VM(cpus, storage=False, network=False, iso=iso, label='wallpaper')
    try:
        vm.send('gui --trace\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm)
        d.screenshot(f'wallpaper-initial-{cpus}cpu')

        ident = d.launch('Settings')
        image = d.screenshot(f'wallpaper-settings-{cpus}cpu')
        width, height = d.width, d.height
        offset = (ident - 1) % 5 * 16
        sx = min(width - 680, (width - 680) // 2 + offset)
        sy = min(height - 42 - 550, (height - 42 - 550) // 2 + offset)

        # Select both sample images by clicking their actual preview cards.
        card_width = (678 - 52 - 18) // 3
        d.click(sx + 1 + 26 + card_width // 2, sy + 40 + 273 + 35)
        d.expect('WALLPAPER', 'Alpine Lake')
        focus = 21

        def clean(name):
            d.key('2', 'meta_l');
            d.expect('WORKSPACE')
            d.move(width - 20, height - 20)
            data = d.screenshot(f'wallpaper-{name}-{cpus}cpu')
            d.key('1', 'meta_l');
            d.expect('WORKSPACE')
            return data[:width * (height - 42) * 3]

        alpine = clean('alpine')
        focus = settings_focus(d, focus, 22, image=True)
        d.key('ret');
        d.expect('WALLPAPER', 'Desert Sunset')
        desert = clean('desert')
        assert alpine != desert, 'Sample selection did not change desktop pixels'
        assert len(set(tuple(desert[i:i + 3]) for i in range(0, width * 100 * 3, 3))) > 100

        # Folder buttons open real directories, including the new personal folder.
        focus = settings_focus(d, focus, 31, image=True)
        d.key('ret')
        output = d.expect('OPEN', 'wallpapers')
        file_id = int(re.findall(r'DESKTOP OPEN window=(\d+)', output)[-1])
        d.find('alpine-lake.png')
        file_offset = (file_id - 1) % 5 * 16
        left = min(width - width * 78 // 100, (width - width * 78 // 100) // 2 + file_offset)
        top = min(height - 42 - height * 788 // 1000, height * 8 // 100 + file_offset)
        d.click(left + 300, top + 217, 'right')

        # Up wraps from Open to the appended Set as wallpaper action.
        d.key('up');
        d.key('ret')
        d.expect('WALLPAPER', 'alpine-lake.png')
        d.key('f4', 'alt');
        d.expect('CLOSE')
        focus = settings_focus(d, focus, 32, image=True)
        d.key('ret');
        d.expect('OPEN', 'Wallpapers')
        d.key('f4', 'alt');
        d.expect('CLOSE')
        d.exit()
        vm.command('stat /home/user/Pictures/Wallpapers', 'mode=')
        path = '/home/user/Pictures/Wallpapers/striped.bmp'
        pixels = [COLORS[x // 8] for y in range(16) for x in range(32)]
        guest_write(vm, path, bmp(32, 16, pixels))
        alpha_path = '/home/user/Pictures/Wallpapers/transparent.png'
        guest_write(vm, alpha_path, png(2, 2, [200, 80, 20, 128] * 4))
        vm.command('write /home/user/Pictures/Wallpapers/broken.png INVALID_IMAGE')
        vm.send('gui --trace\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm);
        d.screenshot(f'wallpaper-custom-initial-{cpus}cpu')
        d.launch('Settings');
        focus = 0

        def choose(target, event, detail=''):
            nonlocal focus
            focus = settings_focus(d, focus, target, image=True)
            d.key('ret');
            d.expect(event, detail)

        def custom(value, success=True):
            nonlocal focus

            # Placement controls are disabled until the first image is selected.
            focus = settings_focus(d, focus, 23, image=focus != 0)
            d.key('ret');
            vm.send(value + '\n')
            d.expect('WALLPAPER' if success else 'WALLPAPER_ERROR', value if success else '')
            if not success:
                d.key('esc')

        custom(path)

        def pixel(data, x, y):
            at = (y * width + x) * 3
            return tuple(data[at:at + 3])

        middle = height // 2
        filled = clean('fill')
        assert pixel(filled, 20, 20) == COLORS[0]
        assert pixel(filled, width * 2 // 9,
                     middle) == COLORS[1], 'Fill failed to crop the wide image'

        choose(25, 'WALLPAPER_MODE')
        fitted = clean('fit')
        assert pixel(fitted, 20, 20) == (24, 37, 45), 'Fit lost its letterbox background'
        assert pixel(fitted, 20, middle) == COLORS[0]
        choose(29, 'WALLPAPER_BACKGROUND')
        black = clean('fit-black')
        assert pixel(black, 20, 20) == (0, 0, 0)
        choose(26, 'WALLPAPER_MODE')
        stretched = clean('stretch')
        assert pixel(stretched, width * 2 // 9,
                     middle) == COLORS[0], 'Stretch did not use the full source width'
        choose(27, 'WALLPAPER_MODE')
        centered = clean('center')
        assert pixel(centered, 20, 20) == (0, 0, 0)
        assert pixel(centered, width // 2 - 14, middle) == COLORS[0]
        assert pixel(centered, width // 2 + 12,
                     middle) == COLORS[3], 'Center changed original image size'

        custom('/home/user/Pictures/Wallpapers/missing.png', False)

        assert clean('missing-keeps-current') == centered, 'Missing file replaced the current wallpaper'
        custom('/home/user/Pictures/Wallpapers/broken.png', False)

        assert clean(
            'invalid-keeps-current') == centered, 'Malformed image replaced the current wallpaper'
        custom(alpha_path)
        choose(24, 'WALLPAPER_MODE')
        transparent = clean('alpha')
        assert pixel(transparent, 20, 20) == (100, 40, 10), 'PNG alpha was not composed over the selected color'
        d.key('s', 'ctrl');
        d.expect('CONFIG_SAVED');
        d.key('esc')
        d.exit()
        output = vm.command('cat /home/user/.config/desktop.ini', 'wallpaper_path=' + alpha_path)
        assert 'wallpaper=5' in output and 'wallpaper_mode=0' in output and 'wallpaper_background=1' in output
        vm.send('gui --trace\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm);
        d.screenshot(f'wallpaper-reload-initial-{cpus}cpu')

        assert clean('saved-reload') == transparent, 'Saved image or placement settings did not reload'
        d.exit()
        vm.command('mv ' + alpha_path + ' ' + alpha_path + '.moved')
        vm.send('gui --trace\n')
        output = vm.until(b'GUI_READY', 40)
        assert 'WALLPAPER_UNAVAILABLE' in output, 'Missing saved image had no fallback'
        d = DesktopTest(vm);
        d.screenshot(f'wallpaper-fallback-initial-{cpus}cpu')
        fallback = clean('fallback')
        assert pixel(fallback, 20, 20) != (100, 40, 10) and any(
            fallback), 'Fallback desktop stayed blank'
        d.exit()
        output = vm.command('ps')
        assert '/bin/wallpaper-decode' not in output, 'A decoder process was left behind'

        print(
            f'PASS wallpaper {cpus} CPU: sample previews, real folders, Files menu, custom BMP/PNG, placement, alpha, saved reload and error fallback', flush=True)
    finally:
        vm.close()


def persistence(cpus, iso=None):
    prepare()
    image_path = '/mnt/nvme1n1p1/Wallpapers/Desert Sunset.png'
    vm = VM(cpus, storage=True, network=False, iso=iso, label='wallpaper-persistence')
    try:
        assert_success(vm.command('mkdir -p /mnt/nvme1n1p1/Wallpapers'))
        assert_success(vm.command('cp /usr/share/wallpapers/desert-sunset.png ' + shlex.quote(image_path)))
        vm.send('gui --trace\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm);
        d.screenshot(f'wallpaper-disk-initial-{cpus}cpu')
        d.launch('Settings')
        focus = settings_focus(d, 0, 22)
        d.key('ret');
        d.expect('WALLPAPER', 'Desert Sunset')

        focus = settings_focus(d, focus, 23, image=True)
        d.key('ret');
        vm.send(image_path + '\n');
        d.expect('WALLPAPER', image_path)

        focus = settings_focus(d, focus, 25, image=True)
        d.key('ret');
        d.expect('WALLPAPER_MODE')

        settings_focus(d, focus, 30, image=True)
        d.key('ret');
        d.expect('WALLPAPER_BACKGROUND')

        d.key('s', 'ctrl');
        d.expect('CONFIG_SAVED', '/mnt/nvme1n1p1/.modernos/desktop.ini')
        d.key('esc');
        d.key('2', 'meta_l');
        d.expect('WORKSPACE')
        d.move(d.width - 20, d.height - 20)
        before = d.screenshot(f'wallpaper-disk-before-reboot-{cpus}cpu')
        before = before[:d.width * (d.height - 42) * 3]
        d.exit()

        output = vm.command('cat /mnt/nvme1n1p1/.modernos/desktop.ini')
        assert 'wallpaper=5' in output and 'wallpaper_path=' + image_path in output
        assert 'wallpaper_mode=1' in output and 'wallpaper_background=2' in output
        vm.command('sync')
    finally:
        vm.close()

    vm = VM(cpus, storage=True, network=False, iso=iso, label='wallpaper-persistence')
    try:
        vm.send('gui --trace\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm);
        d.screenshot(f'wallpaper-disk-reboot-initial-{cpus}cpu')
        d.key('2', 'meta_l');
        d.expect('WORKSPACE')
        d.move(d.width - 20, d.height - 20)
        after = d.screenshot(f'wallpaper-disk-after-reboot-{cpus}cpu')
        assert after[:d.width * (d.height - 42) * 3] == before, 'Image, Fit placement or Cream background did not survive reboot'
        d.exit()

        print(
            f'PASS wallpaper {cpus} CPU disk: saved image selection, placement and background after an actual reboot', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--host-only', action='store_true')
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--iso')
    parser.add_argument('--storage', action='store_true',
                        help='Also test an actual reboot with disposable FAT32 storage')
    args = parser.parse_args()

    decoder_tests()
    packaged_assets()

    if not args.host_only:
        guest(args.cpus, args.iso)
        if args.storage:
            persistence(args.cpus, args.iso)
