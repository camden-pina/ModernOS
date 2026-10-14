#!/usr/bin/env python3
"""Boot upstream GNU nano, edit/save using console and desktop PTY, verify bytes."""
import argparse
import hashlib
import time
from test_runtime import VM, WORK, prepare, run
from test_desktop import DesktopTest


def exercise(cpus, storage=False, dark=False):
    source_hash = None
    if storage:
        prepare()
        source_hash = hashlib.sha256((WORK / 'linux.img').read_bytes()).digest()
    base = '/mnt/nvme1n1p1' if storage else '/tmp'
    console, graphical = base + '/nano-console.txt', base + '/nano-gui.txt'
    v = VM(cpus, storage=storage, label='nano')
    try:
        v.command('check', 'USERLAND CHECK PASS')
        v.command('posix-check', 'POSIX CHECK PASS')
        print('PASS static musl runtime and syscall validation', flush=True)
        v.command('nano --help', 'Usage: nano [OPTIONS]')
        v.command('nano --version', 'GNU nano, version 8.7')
        v.send('nano ' + console + '\n')
        v.until(rb'GNU nano 8.7', 15)
        time.sleep(.3)
        v.send('NATIVE_NANO_CONSOLE_OK\r')
        time.sleep(.2)
        v.send('\x0f')
        v.until(rb'Write to File:', 15)
        v.send('\r')
        v.until(rb'Wrote', 15)
        v.send('\x18')
        v.until_prompt(15)
        v.command('cat ' + console, 'NATIVE_NANO_CONSOLE_OK')
        print('PASS upstream GNU nano console: open, type, save, exit, verify', flush=True)

        if storage:
            v.send('nano /mnt/nvme0n1p1/note.txt\n')
            v.until(rb'is unwritable', 20)
            time.sleep(.2);
            v.send('\x0f')
            v.until(rb'Write to File:', 15);
            v.send('\r')
            v.until(rb'Error writing[^\n]*Read-only file system', 15)
            v.send('\x18');
            v.until_prompt(15)

            print('PASS upstream GNU nano: read-only filesystem reports actual save failure', flush=True)

        if dark:
            config = base + '/.modernos' if storage else '/home/user/.config'
            v.command('mkdir -p ' + config)
            v.command('write ' + config + '/desktop.ini dark=1')

        v.send('gui --trace --stats\n')
        v.until(rb'GUI_READY', 30)
        d = DesktopTest(v)
        d.screenshot(f'nano-desktop-start-{cpus}cpu')
        d.launch('Terminal')
        time.sleep(.3)
        v.send('nano ' + graphical + '\n')
        output = d.expect('TERMINAL_COMMAND', graphical)
        if 'GNU nano 8.7' not in output:
            v.until(rb'GNU nano 8.7', 20)
        time.sleep(.2)

        # A real SIGWINCH reaches upstream nano and it redraws the larger PTY.
        x, y = (d.width - 840) // 2 + 16, (d.height - 42 - 560) // 2 + 16
        d.move(x + 838, y + 558);
        d.button(True)
        d.move(x + 938, y + 598);
        d.button(False);
        d.expect('GEOMETRY')
        time.sleep(.3)

        # Cancel the close warning without terminating nano or its shell.
        d.key('f4', 'alt')
        image = d.screenshot(f'nano-close-confirmation-{cpus}cpu')
        at = (((d.height - 42 - 228) // 2 + 80) * d.width + (d.width - 500) // 2 + 12) * 3
        assert tuple(image[at:at + 3]) == ((25, 37, 45) if dark else (255,
                                                                      255, 255)), 'Running nano was closed without confirmation'
        d.key('esc')
        d.key('f1')

        # Curses may retain the unchanged "xt" suffix from the old filename
        # rather than retransmitting the complete help title. Verify the new
        # help body instead of assuming terminal output is a full screen dump.
        v.until(rb'Display this help text', 15)
        d.screenshot(f'nano-help-{cpus}cpu')
        d.key('x', 'ctrl')
        time.sleep(.2)
        v.send('NATIVE_NANO_GUI_OK\n')
        time.sleep(.2)
        d.key('o', 'ctrl')
        v.until(rb'Write to File:', 15)
        d.key('ret')
        v.until(rb'Wrote', 15)
        image = d.screenshot(f'nano-terminal-{cpus}cpu')
        at = ((y + 120) * d.width + x + 100) * 3
        assert tuple(image[at:at + 3]) == ((23, 35, 43) if dark else (250,
                                                                      252, 251)), 'Nano terminal appearance is wrong'
        d.key('x', 'ctrl')
        time.sleep(.5)
        v.send('stty size\n')
        output = d.expect('TERMINAL_COMMAND', 'stty size')
        if '30 130' not in output:
            v.until(rb'30 130', 15)
        d.exit()
        v.command('cat ' + graphical, 'NATIVE_NANO_GUI_OK')

        print('PASS upstream GNU nano desktop PTY: resize, cancel close, function-key help, type, save, exit, verify' + (' (dark)' if dark else ''), flush=True)
    finally:
        v.close()

    if storage:
        assert hashlib.sha256((WORK / 'linux.img').read_bytes()).digest() == source_hash
        image = str(WORK / 'fat.img') + '@@' + str(2048 * 512)
        for name, data in (('nano-console.txt', b'NATIVE_NANO_CONSOLE_OK\n\n'),
                           ('nano-gui.txt', b'NATIVE_NANO_GUI_OK\n\n')):
            exported = WORK / 'nano-export.bin'
            run('mcopy', '-D', 'o', '-i', image, '::/' + name, exported)
            assert exported.read_bytes() == data, name

        checked = WORK / 'nano-checked-fat.volume'
        with (WORK / 'fat.img').open('rb') as disk, checked.open('wb') as out:
            disk.seek(2048 * 512)
            out.write(disk.read((WORK / 'fat.volume').stat().st_size))
        run('fsck.fat', '-n', checked)
        v = VM(cpus, storage=True, label='nano')
        try:
            v.command('cat ' + console, 'NATIVE_NANO_CONSOLE_OK')
            v.command('cat ' + graphical, 'NATIVE_NANO_GUI_OK')
        finally:
            v.close()

        print('PASS upstream GNU nano FAT32: exact saved bytes, reboot persistence, source unchanged, fsck', flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser();
    p.add_argument('--cpus', type=int, default=1)
    p.add_argument('--storage', action='store_true')
    p.add_argument('--dark', action='store_true')
    a = p.parse_args();
    exercise(a.cpus, a.storage, a.dark)
