#!/usr/bin/env python3
"""Optional public-network smoke test for the real guest resolver/TLS/apps.

Unlike the disposable fixtures, this needs Internet access. It reads public
GNU pages and repository refs; it does not push or change a remote repository.
"""
import argparse
import re
import shlex

from test_runtime import VM, WORK, assert_success
from test_desktop import DesktopTest


def exercise(cpus, iso=None):
    WORK.mkdir(parents=True, exist_ok=True)
    vm = VM(cpus, storage=False, iso=iso, label='web-live')
    try:
        def shell(script, expected=None, timeout=90):
            return assert_success(vm.command('build-sh -c ' + shlex.quote(script),
                                             expected, timeout=timeout))

        shell('i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do '
              'sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf', timeout=45)
        shell('curl -fsS --max-time 45 https://gcc.gnu.org/ -o /tmp/public.html && '
              'grep -F "GNU Compiler Collection" /tmp/public.html && '
              'printf "LIVE_HTTPS_OK\\n"', 'LIVE_HTTPS_OK')
        print('PASS live DNS and HTTPS: real GNU page, production CA bundle', flush=True)
        output = vm.command('git ls-remote https://git.savannah.gnu.org/git/hello.git HEAD',
                            timeout=120)
        assert_success(output)

        assert re.search(r'\n[0-9a-f]{40,64}\s+HEAD\n', output), output
        print('PASS live Git HTTPS: public GNU repository refs', flush=True)

        vm.send('gui --trace --stats\n')
        vm.until(b'GUI_READY', 40)
        desktop = DesktopTest(vm)
        desktop.screenshot('browser-live-start')
        desktop.launch('Browser')
        desktop.expect('BROWSER_READY', 'ModernOS Browser')
        desktop.key('l', 'ctrl')
        vm.send('https://gcc.gnu.org/\n')
        vm.until(rb'DESKTOP BROWSER_READY[^\n]*GNU Compiler Collection[^\n]*\n', 90)
        desktop.screenshot('browser-live-gcc')
        desktop.key('f4', 'alt')
        desktop.expect('CLOSE')
        desktop.exit()

        print('PASS live Browser: genuine public HTTPS page in shared native window', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--iso')
    args = parser.parse_args()
    exercise(args.cpus, args.iso)
