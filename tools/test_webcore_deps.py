#!/usr/bin/env python3
"""Guest checks for new real WebCore libraries; these do not assert browser support."""
import argparse
from test_runtime import VM, WORK, assert_success
from web_fixture import WebFixture


def exercise(cpus, iso=None, soup_only=False):
    WORK.mkdir(parents=True, exist_ok=True)
    fixture = WebFixture()
    vm = None
    try:
        vm = VM(cpus, storage=False, iso=iso, label='webcore-deps')
        if not soup_only:
            assert_success(vm.command('webcore-deps-check',
                           'WEBCORE DEPENDENCIES CHECK PASS', timeout=120))
        assert_success(vm.command('build-sh -c \'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do '
                                  'sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf && cat /etc/resolv.conf\'',
                                  'nameserver', timeout=45))
        urls = [fixture.url(), fixture.url(tls=True),
                f'https://10.0.2.2:{fixture.wrong_host}/', f'https://10.0.2.2:{fixture.untrusted}/']
        output = vm.command('web-soup-check ' + ' '.join(urls),
                            'SOUP BROKER CHECK PASS', timeout=160)
        assert_success(output)

        assert 'PASS real Soup broker HTTP' in output and output.count(
            'PASS rejected certificate:') == 3, output
        assert 'PASS genuine statically registered OpenSSL' in output, output

        print(
            f'PASS new font/image/data libraries and restricted Soup/TLS broker ({cpus} CPUs); browser rendering remains separate', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--iso')
    parser.add_argument('--soup-only', action='store_true')
    args = parser.parse_args()
    for cpus in args.cpus:
        exercise(cpus, args.iso, args.soup_only)
