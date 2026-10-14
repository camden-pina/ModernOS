#!/usr/bin/env python3
"""Exercise upstream Vim, build tools and process/file ABI in the real OS.

All disk images are disposable fixtures. No host block device is accessed.
"""
import argparse
import hashlib
import re
import shlex
import time

from test_runtime import VM, WORK, prepare, run
from test_desktop import DesktopTest


def exercise(cpus, storage=False, dark=False):
    WORK.mkdir(parents=True, exist_ok=True)
    source_hash = None
    if storage:
        prepare()
        source_hash = hashlib.sha256((WORK / 'linux.img').read_bytes()).digest()
    base = '/mnt/nvme1n1p1' if storage else '/tmp'
    console, graphical = base + '/vim-console.txt', base + '/vim-gui.txt'
    vm = VM(cpus, storage=storage, label='ports')

    def command(text, expected=None, status=0):
        output = vm.command(text, expected, timeout=60)
        exits = re.findall(r'pid \d+ exited (\d+)', output)
        assert not exits if not status else exits and int(exits[-1]) == status, output
        assert not re.search(r'pid \d+ terminated by signal', output), output
        return output

    def shell(script, expected=None, status=0):
        return command('build-sh -c ' + shlex.quote(script), expected, status)

    try:
        command('check', 'USERLAND CHECK PASS')
        command('posix-check', 'POSIX CHECK PASS')
        command('build-check', 'BUILD CHECK PASS')
        command('build-check --files /tmp', 'BUILD FILE CHECK PASS')
        if storage:
            command('build-check --files ' + base, 'BUILD FILE CHECK PASS')
        print('PASS process, timer, static-musl, display and real filesystem ABI', flush=True)
        command('vim --version', 'VIM - Vi IMproved 9.2')
        command('make --version', 'GNU Make 4.4.1')
        command('gawk --version', 'GNU Awk 5.4.0')
        command('awk --version', 'GNU Awk 5.4.0')
        program = r'BEGIN { s="C++"; gsub("\\+", "\\+", s); if (!(" C++ " ~ (" " s " "))) exit 1; print "GNU_AWK_ESCAPING_OK" }'
        shell('gawk ' + shlex.quote(program), '\nGNU_AWK_ESCAPING_OK\n')
        command('busybox --list', 'patch')
        shell('printf "b\\na\\na\\n" | sort | uniq | grep -E "^[ab]$"', '\na\nb\n')
        shell('printf "abc\\n" | tr a-z A-Z | sed s/ABC/TOOLS_OK/', '\nTOOLS_OK\n')
        shell('printf "a 2\\nb 3\\n" | awk "{sum += \\$2} END {print sum}"', '\n5\n')
        shell('printf "one\\ntwo\\nthree\\n" | head -n 2 | tail -n 1', '\ntwo\n')
        output = shell('printf "one two\\n" | wc -lwc')
        assert re.search(r'\n\s*1\s+2\s+8\s*\n', output), output
        shell('printf "printf SCRIPT_OK\\\\n\\n" > /tmp/native-script.sh')
        command('build-sh /tmp/native-script.sh', 'SCRIPT_OK')
        shell('ln -s native-script.sh /tmp/native-link; readlink /tmp/native-link', '\nnative-script.sh\n')
        print('PASS upstream shell: scripts, pipes, regex, fields, stdin tools and RAM symlinks', flush=True)

        # Configure probes and make recipes use real timestamps and child status.
        shell('mkdir -p ' + base + '/native-build; cd ' + base + '/native-build; '
              'printf "all: result\\nresult: a b\\n\\tcat a b > result\\na:\\n\\tprintf A > a\\nb:\\n\\tprintf B > b\\n" > Makefile')
        shell('cd ' + base + '/native-build; make -j2; cat result', '\nAB')
        shell('cd ' + base + '/native-build; touch -d @1700000000 a b; touch -d @1700000002 result; make -q')
        shell('cd ' + base + '/native-build; touch -d @1700000004 a; make -q', status=1)
        shell('cd ' + base + '/native-build; make -j2; make -q')

        # A recursive make must inherit the real pipe jobserver descriptors.
        shell(r'mkdir /tmp/recursive && cd /tmp/recursive && '
              r'printf "all:\n\t\044(MAKE) -f Child.mk\n" > Makefile && '
              r'printf "all: a b\na:\n\tprintf A > a\nb:\n\tprintf B > b\n" > Child.mk && '
              r'make -j2 && test "$(cat a b)" = AB && printf "RECURSIVE_MAKE_OK\n"',
              '\nRECURSIVE_MAKE_OK\n')

        # A failed recipe must retain its exit status and stop the dependent rule.
        shell('cd ' + base + '/native-build; printf "fail:\\n\\tfalse\\n" > Failfile; make -f Failfile', status=2)
        print('PASS native GNU make: parallel/recursive pipe jobserver, recipes, incremental timestamps and failure status', flush=True)

        shell('mkdir -p /tmp/patch-test; cd /tmp/patch-test; '
              'printf "old\\n" > patched; printf "PATCHED\\n" > revised; '
              'diff -u patched revised > change.patch || test "$?" -eq 1; patch -p0 patched < change.patch')
        command('cat /tmp/patch-test/patched', '\nPATCHED\n')
        shell('sed -i s/PATCHED/INPLACE_OK/ /tmp/patch-test/patched')
        command('cat /tmp/patch-test/patched', '\nINPLACE_OK\n')

        if storage:
            shell('cp /tmp/patch-test/change.patch ' + base + '/change.patch; '
                  'printf "old\\n" > ' + base + '/patched; cd ' + base + '; patch -p0 patched < change.patch', status=1)
            command('cat ' + base + '/patched', '\nold\n')

        print('PASS diff/patch and sed replacement; unavailable FAT replacement reports an error', flush=True)

        shell('mkdir -p ' + base + '/archive/tree ' + base + '/archive/unpack; '
              'printf "ARCHIVE_CONTENT_OK\\n" > ' + base + '/archive/tree/proof.txt')
        shell('cd ' + base + '/archive; tar -cf source.tar tree; gzip -c source.tar > source.tgz; '
              'gunzip -c source.tgz | cmp - source.tar; tar -xf source.tar --no-same-owner -C unpack')
        command('cat ' + base + '/archive/unpack/tree/proof.txt', '\nARCHIVE_CONTENT_OK\n')
        output = shell('sha256sum ' + base + '/archive/unpack/tree/proof.txt')
        assert hashlib.sha256(b'ARCHIVE_CONTENT_OK\n').hexdigest() in output, output
        shell('cd ' + base + '/archive; bzip2 -c source.tar > source.bz2; bzcat source.bz2 | cmp - source.tar')
        print('PASS tar extraction, gzip/bzip2 round trips and exact SHA-256 data', flush=True)

        # Use the default installed vimrc/runtime. No tiny replacement/editor demo.
        vm.send('vim -n ' + console + '\n')
        output = vm.until(rb'\[New(?: File)?\]', 20)
        time.sleep(.2)
        vm.send('iNATIVE_VIM_CONSOLE_OK\r');
        time.sleep(.15)
        vm.send('\x1b');
        time.sleep(.15)

        vm.send('oREMOVE_ME');
        time.sleep(.15)
        vm.send('\x1b');
        time.sleep(.15)
        vm.send('u');
        time.sleep(.15)

        vm.send(':help\r');
        output += vm.until(rb'help.txt', 20)
        vm.send(':q\r');
        time.sleep(.15)
        vm.send(':wq\r');
        output += vm.until_prompt(20)
        assert not re.search(r'E\d{3,}:', output), output[-2000:]
        command('cat ' + console, '\nNATIVE_VIM_CONSOLE_OK\n')

        output = command('vim -n -es ' + console + " -c '%!tr A-Z a-z' -c wq")
        command('cat ' + console, '\nnative_vim_console_ok\n')
        command('vim -n -es ' + console + " -c '%s/native_vim_console_ok/NATIVE_VIM_CONSOLE_OK/' -c wq")
        command('cat ' + console, '\nNATIVE_VIM_CONSOLE_OK\n')
        print('PASS upstream Vim console: insert, undo, bundled help, repeated save and real shell filter', flush=True)

        if storage:
            vm.send('vim -n /mnt/nvme0n1p1/note.txt\n')
            output = vm.until(rb'\[readonly\]', 20)
            vm.send(':w\r');
            output += vm.until(rb'E45:[^\n]*readonly', 20)
            vm.send(':w!\r');
            output += vm.until(rb'E212:', 20)
            vm.send(':q!\r');
            vm.until_prompt(20)
            command('cat /mnt/nvme0n1p1/note.txt', '\nSOURCE_EXT4_READ_OK\n')

            print('PASS upstream Vim: read-only warning and forced save failure preserve the source', flush=True)

        if dark:
            config = base + '/.modernos' if storage else '/home/user/.config'
            command('mkdir -p ' + config)
            command('write ' + config + '/desktop.ini dark=1')

        vm.send('gui --trace --stats\n');
        vm.until(rb'GUI_READY', 30)
        d = DesktopTest(vm)
        d.screenshot(f'vim-desktop-start-{cpus}cpu')
        d.launch('Terminal')
        vm.send('vim -n ' + graphical + '\n')
        output = d.expect('TERMINAL_COMMAND', graphical)
        if not re.search(r'\[New(?: File)?\]', output):
            output += vm.until(rb'\[New(?: File)?\]', 20)
        time.sleep(.2)
        x, y = (d.width - 840) // 2 + 16, (d.height - 42 - 560) // 2 + 16
        d.move(x + 838, y + 558);
        d.button(True)
        d.move(x + 938, y + 598);
        d.button(False);
        d.expect('GEOMETRY')

        d.key('f4', 'alt')
        image = d.screenshot(f'vim-close-confirmation-{cpus}cpu')
        at = (((d.height - 42 - 228) // 2 + 80) * d.width + (d.width - 500) // 2 + 12) * 3
        assert tuple(image[at:at + 3]) == ((25, 37, 45) if dark else (255, 255, 255))
        d.key('esc')

        vm.send('iNATIVE_VIM_GUI_OK\n');
        time.sleep(.15)
        d.key('esc');
        vm.send(':w\n')
        output = vm.until(rb'written', 20)
        assert not re.search(r'E\d{3,}:', output), output[-2000:]
        d.screenshot(f'vim-terminal-{cpus}cpu')

        vm.send(':q\n');
        time.sleep(.4)

        vm.send('stty size\n')
        output = d.expect('TERMINAL_COMMAND', 'stty size')
        if '30 130' not in output:
            vm.until(rb'30 130', 15)
        d.exit()
        command('cat ' + graphical, '\nNATIVE_VIM_GUI_OK\n')

        print('PASS upstream Vim desktop PTY: modal input, resize, cancel close, save and exit', flush=True)
    finally:
        vm.close()

    if storage:
        assert hashlib.sha256((WORK / 'linux.img').read_bytes()).digest() == source_hash
        image = str(WORK / 'fat.img') + '@@' + str(2048 * 512)
        for name, data in (('vim-console.txt', b'NATIVE_VIM_CONSOLE_OK\n\n'),
                           ('vim-gui.txt', b'NATIVE_VIM_GUI_OK\n\n')):
            exported = WORK / 'vim-export.bin'
            run('mcopy', '-D', 'o', '-i', image, '::/' + name, exported)
            assert exported.read_bytes() == data, name

        checked = WORK / 'vim-checked-fat.volume'
        with (WORK / 'fat.img').open('rb') as disk, checked.open('wb') as out:
            disk.seek(2048 * 512)
            out.write(disk.read((WORK / 'fat.volume').stat().st_size))
        run('fsck.fat', '-n', checked)
        vm = VM(cpus, storage=True, label='ports')
        try:
            vm.command('cat ' + console, 'NATIVE_VIM_CONSOLE_OK')
            vm.command('cat ' + graphical, 'NATIVE_VIM_GUI_OK')
            vm.command('build-sh -c ' + shlex.quote('cd ' + base + '/native-build; make -q'))
        finally:
            vm.close()

        print('PASS FAT32: exact saved documents, timestamps after reboot, source unchanged and fsck', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, default=1)
    parser.add_argument('--storage', action='store_true')
    parser.add_argument('--dark', action='store_true')
    args = parser.parse_args()
    exercise(args.cpus, args.storage, args.dark)
