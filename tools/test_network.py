#!/usr/bin/env python3
"""Guest socket/DHCP/curl/TLS and genuine Git smart-HTTP/HTTPS tests."""
import argparse
import json
from pathlib import Path
import re
import shlex
import test_runtime as runtime
import web_fixture
from test_runtime import VM, WORK
from web_fixture import WebFixture, run


def exercise(cpus, iso=None, network_model='e1000e', accel='tcg', memory='2G', network_backend='user', skip_compiler_check=False):
    WORK.mkdir(parents=True, exist_ok=True)
    fixture = WebFixture()
    vm = None
    try:
        vm = VM(cpus, storage=False, iso=iso, label='network-' + network_model + '-' + accel, network_model=network_model, accel=accel, memory=memory, network_backend=network_backend)

        def shell(script, expected=None, status=0, timeout=120):
            line = 'build-sh -c ' + shlex.quote(script)
            assert len(line.encode()) < 511, line
            output = vm.command(line, expected, timeout=timeout)
            exits = re.findall(r'pid \d+ exited (\d+)', output)
            assert (not exits) if status == 0 else exits and int(exits[-1]) == status, output
            assert 'terminated by signal' not in output, output
            return output

        vm.command('network-check', 'NETWORK CHECK PASS', timeout=120)
        if not skip_compiler_check:
            vm.command('compiler-check', 'COMPILER ACCOUNTING CHECK PASS', timeout=120)
        vm.command('netinfo', 'IPv4', timeout=30)
        vm.command('curl --version', 'OpenSSL/3.5.9')
        shell('i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do '
              'sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf && cat /etc/resolv.conf', 'nameserver')
        vm.command('netinfo', 'link up; DHCP')
        http = fixture.url()
        shell(
            f'curl -fsS --max-time 30 {http}large.bin -o /tmp/http.bin && sha256sum /tmp/http.bin', fixture.sha256)
        shell(
            f'curl -fsS --max-time 30 {http}chunked -o /tmp/chunked.bin && sha256sum /tmp/chunked.bin', fixture.sha256)
        shell(
            f'curl -fLsS --max-time 30 {http}redirect -o /tmp/redirect.bin && sha256sum /tmp/redirect.bin', fixture.sha256)
        shell(f'curl -fsS --max-time 30 {http}fail', '503', status=22)
        shell(f'curl -fsS --max-time 30 {http}ca.pem -o /tmp/fixture-ca.pem')
        https = fixture.url(tls=True)
        shell(f'curl -fsS --max-time 30 {https}page.html', 'certificate', status=60)
        shell(
            f'curl -fsS --max-time 30 --cacert /tmp/fixture-ca.pem {https}page.html', 'Coast HTTP page')
        wrong = f'https://10.0.2.2:{fixture.wrong_host}/page.html'
        shell(
            f'curl -fsS --max-time 30 --cacert /tmp/fixture-ca.pem {wrong}', 'certificate', status=60)
        print('PASS networking: real loopback sockets, DHCP/DNS configuration, HTTP bodies/chunks/redirects, TLS trust and hostname rejection', flush=True)

        remote = fixture.url('repos/main.git')
        shell(f'git clone -q {remote} /tmp/http-repo', timeout=180)
        shell('git -C /tmp/http-repo rev-parse HEAD', fixture.head)
        shell('git -C /tmp/http-repo fsck --full')
        shell('git -C /tmp/http-repo config user.name "Coast Test"; '
              'git -C /tmp/http-repo config user.email test@example.invalid')
        shell('cd /tmp/http-repo; git checkout -b native; printf "NATIVE_GIT_PUSH_OK\\n" > native.txt; '
              'git add native.txt && git -c commit.gpgsign=false commit -m native && git push origin native', 'native', timeout=180)

        assert run('git', '-C', fixture.repo, 'show',
                   'native:native.txt') == b'NATIVE_GIT_PUSH_OK\n'
        fixture.commit('host update fetched by native Git')
        shell('git -C /tmp/http-repo fetch origin main && git -C /tmp/http-repo rev-parse FETCH_HEAD',
              fixture.head, timeout=180)
        remote = fixture.url('repos/main.git', tls=True)
        shell(f'git clone -q {remote} /tmp/rejected-repo', 'certificate', status=128, timeout=180)
        shell('test ! -e /tmp/rejected-repo')
        shell(
            f'git -c http.sslCAInfo=/tmp/fixture-ca.pem clone -q {remote} /tmp/https-repo', timeout=180)
        shell('git -C /tmp/https-repo fsck --full && git -C /tmp/https-repo rev-parse HEAD', fixture.head)
        print('PASS native Git network transports: smart HTTP clone/fetch/push, HTTPS clone, fsck and untrusted-certificate rejection', flush=True)
        vm.close();
        vm = None

        # A missing supported physical NIC must retain working local sockets
        # and produce an honest connection error for external addresses.
        vm = VM(cpus, storage=False, iso=iso, label='network-offline-' + network_model + '-' + accel, network=False, accel=accel, memory=memory)
        vm.command('network-check', 'NETWORK CHECK PASS', timeout=120)
        vm.command('netinfo', 'no supported', timeout=30)
        shell(f'curl -fsS --max-time 5 --connect-timeout 2 {http}page.html', status=7)

        print('PASS offline networking: loopback works; unsupported NIC/external access fails explicitly', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--iso')
    parser.add_argument('--network-model', choices=('e1000e', 'e1000', 'igb'), default='e1000e')
    parser.add_argument('--accel', choices=('tcg', 'kvm'), default='tcg')
    parser.add_argument('--memory', default='2G')
    parser.add_argument('--network-backend', choices=('user', 'passt'), default='user')
    parser.add_argument('--output-root', type=Path)
    parser.add_argument('--skip-compiler-check', action='store_true', help='run network qualification without the separate legacy compiler ABI fixture')
    args = parser.parse_args()
    if args.output_root:
        WORK = args.output_root.resolve(); WORK.mkdir(parents=True, exist_ok=False)
        runtime.WORK = WORK; web_fixture.WORK = WORK / 'fixtures'
        (WORK / 'HARNESS.py').write_bytes(Path(__file__).read_bytes())
    report = dict(state='failed', iso=str(Path(args.iso).resolve()) if args.iso else None,
                  cpus=args.cpus, memory=args.memory, network_backend=args.network_backend,
                  compiler_check_included=not args.skip_compiler_check)
    try:
        exercise(args.cpus, args.iso, args.network_model, args.accel, args.memory, args.network_backend, args.skip_compiler_check)
        report['state'] = 'passed'
    finally:
        if args.output_root: (WORK / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
