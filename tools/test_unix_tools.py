#!/usr/bin/env python3
"""Exercise installed Unix tools and console navigation in a real RAM-only VM."""
import argparse
import re

from test_runtime import VM, assert_success


APPLETS = ('less', 'nl', 'tac', 'rev', 'paste', 'comm', 'fold', 'base64',
           'split', 'clear', 'which')


def exercise(cpus=1, iso=None, accel="tcg", label="unix-tools"):
    vm = VM(cpus, storage=False, network=False, iso=iso, label=label, accel=accel)
    def shell(script, expect=None, timeout=30):
        return vm.shell_command('set -e; ' + script, expect, timeout)
    try:
        listing = set(shell('busybox --list').split())
        assert set(APPLETS) <= listing, sorted(set(APPLETS) - listing)
        shell('for tool in ' + ' '.join(APPLETS) + '; do test -x /bin/$tool || exit 1; done')
        shell('mkdir /tmp/unix-tools; cd /tmp/unix-tools; '
              'printf "one\\n\\ntwo" > input; printf "1:one\\n2:\\n3:two\\n" > expected; '
              'nl -ba -w1 -s: input | cmp - expected')
        shell('cd /tmp/unix-tools; printf "two\\none\\n" > expected; '
              'printf "one\\ntwo\\n" | tac | cmp - expected; '
              'printf "owt\\n\\neno" > expected; printf "two\\n\\none" | rev | cmp - expected')
        shell('cd /tmp/unix-tools; printf "a\\nb\\n" > left; printf "1\\n2\\n" > right; '
              'printf "a:1\\nb:2\\n" > expected; paste -d : left right | cmp - expected; '
              'printf "a,b\\n" > expected; paste -sd , left | cmp - expected')
        shell('cd /tmp/unix-tools; printf "a\\nb\\nd\\n" > left; printf "b\\nc\\nd\\n" > right; '
              'printf "b\\nd\\n" > expected; comm -12 left right | cmp - expected; '
              'printf "a\\n" > expected; comm -23 left right | cmp - expected')
        shell('cd /tmp/unix-tools; printf "abc\\ndef\\ng" > expected; '
              'printf abcdefg | fold -b -w3 | cmp - expected; '
              'if fold -w0 input >/dev/null 2>&1; then exit 1; fi')
        shell('cd /tmp/unix-tools; printf "\\000\\377binary\\n" > binary; '
              'base64 -w4 binary > encoded; base64 -d encoded | cmp - binary; '
              'test -z "$(printf "" | base64)"; '
              'if base64 /tmp/no-such-unix-file >/dev/null 2>&1; then exit 1; fi')
        shell('cd /tmp/unix-tools; split -b3 binary byte.; cat byte.* | cmp - binary; '
              'split -l2 input line.; cat line.* | cmp - input; test ! -e line.ac; '
              'if split -b nope input bad. >/dev/null 2>&1; then exit 1; fi')
        shell('cd /tmp/unix-tools; printf "\\033[H\\033[J" > expected; clear | cmp - expected')
        shell('cd /tmp/unix-tools; mkdir path; cp /bin/true path/true; chmod 755 path/true; '
              'test "$(PATH=/tmp/unix-tools/path:/bin /bin/which true)" = /tmp/unix-tools/path/true; '
              'status=0; PATH=/tmp/unix-tools/path /bin/which missing >/dev/null || status=$?; '
              'test "$status" -eq 1')
        shell('cd /tmp/unix-tools; '
              'printf "/tmp/unix-tools/path/true\\n/bin/true\\n" > expected; '
              'PATH=/tmp/unix-tools/path:/bin /bin/which -a true | cmp - expected; '
              'chmod 644 path/true; '
              'if PATH=/tmp/unix-tools/path /bin/which true >/dev/null; then exit 1; fi')
        for tool in ('nl', 'tac', 'rev', 'paste', 'comm', 'fold', 'split', 'less'):
            argument = '/tmp/no-such-unix-file'
            if tool == 'comm':
                argument += ' /tmp/no-such-unix-file'
            shell('if ' + tool + ' ' + argument + ' >/dev/null 2>&1; then exit 1; fi')
        print('PASS installed Unix tools: pipelines, exact bytes, empty/unterminated input and errors', flush=True)

        output = vm.command("coast-sh -c 'cd -'", 'no previous directory')
        assert re.search(r'pid \d+ exited 1', output), output
        assert_success(vm.command('mkdir -p /tmp/unix-cd/a /tmp/unix-cd/b'))
        assert_success(vm.command('cd /tmp/unix-cd/a'))
        assert_success(vm.command('cd /tmp/unix-cd/b'))
        vm.command('cd /tmp/no-such-unix-directory', 'No such file')
        output = vm.command('cd -', '\n/tmp/unix-cd/a\n')
        assert 'os:/tmp/unix-cd/a$ ' in output, output
        output = vm.command('cd -', '\n/tmp/unix-cd/b\n')
        assert 'os:/tmp/unix-cd/b$ ' in output, output
        assert_success(vm.command('cd /'))
        vm.command('echo x | cat', "build-sh -c 'COMMAND'")
        vm.command("echo 'x|y'", '\nx|y\n')
        print('PASS console navigation: cd - toggles, failed cd preserves history and operators explain build-sh', flush=True)

        shell('printf "CONSOLE_PAGER_FIRST\\n" > /tmp/console-less; '
              'seq 1 200 >> /tmp/console-less; printf "CONSOLE_PAGER_LAST\\n" >> /tmp/console-less')
        original_console_mode = shell('stty -a')
        assert '; icanon echo isig icrnl;' in original_console_mode
        for invocation, statusline in (
                ('less /tmp/console-less', b'/tmp/console-less'),
                ("build-sh -c 'less /tmp/console-less'", b'/tmp/console-less')):
            vm.send(invocation + '\n')
            vm.until(rb'CONSOLE_PAGER_FIRST[\s\S]*' + re.escape(statusline), 20)
            vm.send('G')
            vm.until(rb'CONSOLE_PAGER_LAST[\s\S]*\(END\)', 20)
            vm.send('q')
            vm.until_prompt(20)
            assert shell('stty -a') == original_console_mode
        print('PASS less console files: native/build-sh, immediate end/quit keys and restored mode', flush=True)

        for invocation in ("build-sh -c 'cat /tmp/console-less | less'",
                           "build-sh -c 'cat /tmp/console-less | cat | less'",
                           "build-sh -c 'seq 1 200 | less'"):
            vm.send(invocation + '\n')
            vm.until(rb'standard input',20)
            vm.send('G')
            vm.until(rb'200[\s\S]*\(END\)|CONSOLE_PAGER_LAST[\s\S]*\(END\)',20)
            vm.send('q')
            vm.until_prompt(20)
            assert shell('stty -a') == original_console_mode
        print('PASS console foreground pipelines: multiple producers, paging and terminal restoration', flush=True)
        assert_success(vm.command('echo CONSOLE_PAGER_RETURNED', '\nCONSOLE_PAGER_RETURNED\n'))
        output = vm.command('less /tmp/no-such-unix-file', 'No such file or directory')
        assert re.search(r'pid \d+ exited 1', output), output
        assert shell('stty -a') == original_console_mode
        vm.send('less /tmp/console-less\n')
        vm.until(rb'CONSOLE_PAGER_FIRST[\s\S]*/tmp/console-less', 20)
        vm.send('\x03')
        output = vm.until_prompt(20)
        assert re.search(r'terminated by signal 2', output), output
        assert shell('stty -a') == original_console_mode
        print('PASS less console cleanup: missing-file and SIGINT restore original mode', flush=True)
        shell('unix-tools-check', 'UNIX TOOLS PTY CHECK PASS', timeout=30)
        print('PASS less real PTY: file/pipe search, resize, quit, >16 KiB pressure and exact restoration', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, default=1)
    parser.add_argument('--iso')
    parser.add_argument('--accel', choices=('tcg','kvm'), default='tcg')
    parser.add_argument('--label',default='unix-tools')
    args = parser.parse_args()
    exercise(args.cpus, args.iso, args.accel, args.label)
