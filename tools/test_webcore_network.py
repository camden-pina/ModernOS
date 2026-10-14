#!/usr/bin/env python3
"""Actual brokered guest WebCore pages and pixels, including CORS/preflight.

This development embedder does not establish native UI, general navigation,
cookies/storage, cross-origin frames, or public website compatibility.
"""
import argparse
from test_runtime import VM, WORK, assert_success
from web_fixture import WebFixture


def exercise(cpus, iso):
    WORK.mkdir(parents=True, exist_ok=True)
    fixture = WebFixture(webcore=True)
    vm = None
    try:
        vm = VM(cpus, storage=False, iso=iso, label='webcore-network')
        assert_success(vm.command('build-sh -c \'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do '
                                  'sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf && cat /etc/resolv.conf\'',
                                  'nameserver', timeout=45))
        http = fixture.url() + 'webcore.html'
        https = fixture.url(tls=True) + 'webcore.html'
        output = vm.command(
            f'web-broker --check {http} {https} --fixture-ca {fixture.url()}ca.pem', timeout=240)
        assert_success(output)

        assert 'WEBCORE NETWORK PAGE CHECK PASS' in output, output
        assert output.count(
            'PASS real brokered HTML/CSS/JS/fetch/PNG pixels and click') == 2, output
        assert output.count('editing/backspace, wheel pixels and form POST/navigation') == 2, output
        assert output.count('PASS zero-sized empty-typeface font cache') == 2, output
        assert output.count(
            'PASS selected interpreter allocations and full GC preserve') == 2, output
        assert output.count(
            'PASS real WebCore idle deadlines, microtasks, continuation, runnable timeout and cancellation') == 2, output
        for port in (fixture.http, fixture.https):
            for path in ('/webcore.html', '/webcore.css', '/webcore.js', '/webcore.png', '/webcore-redirect', '/webcore-data'):
                assert (port, 'GET', path) in fixture.requests_by_port, (port,
                                                                         path, fixture.requests_by_port)
            assert (port, 'POST', '/webcore-form') in fixture.requests_by_port, fixture.requests_by_port
        for port in (fixture.cross_http, fixture.cross_https):
            for method, path in (('GET', '/webcore-allowed'), ('GET', '/webcore-denied'), ('OPTIONS', '/webcore-preflight'), ('POST', '/webcore-preflight')):
                assert (port, method, path) in fixture.requests_by_port, (port,
                                                                          method, path, fixture.requests_by_port)

        print(
            f'PASS real guest WebCore HTTP/TLS HTML/CSS/JS/PNG/fetch pixels, click and CORS/preflight ({cpus} CPUs); native/public browser unfinished', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--iso', required=True)
    args = parser.parse_args()
    for cpus in args.cpus:
        exercise(cpus, args.iso)
