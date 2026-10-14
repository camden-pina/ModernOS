#!/usr/bin/env python3
"""Guest socket/DHCP/curl/TLS and genuine Git smart-HTTP/HTTPS tests."""
import argparse
import re
import shlex
from test_runtime import VM, WORK
from web_fixture import WebFixture, run


def exercise(cpus, iso=None, network_model='e1000e', accel='tcg'):
    WORK.mkdir(parents=True, exist_ok=True)
    fixture = WebFixture()
    vm = None
    try:
        vm = VM(cpus, storage=False, iso=iso, label='network-' + network_model + '-' + accel, network_model=network_model, accel=accel)

        def shell(script, expected=None, status=0, timeout=120):
            line = 'build-sh -c ' + shlex.quote(script)
            assert len(line.encode()) < 511, line
            output = vm.command(line, expected, timeout=timeout)
            exits = re.findall(r'pid \d+ exited (\d+)', output)
            assert (not exits) if status == 0 else exits and int(exits[-1]) == status, output
            assert 'terminated by signal' not in output, output
            return output

        vm.command('network-check', 'NETWORK CHECK PASS', timeout=120)
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
            f'curl -fsS --max-time 30 --cacert /tmp/fixture-ca.pem {https}page.html', 'ModernOS HTTP page')
        wrong = f'https://10.0.2.2:{fixture.wrong_host}/page.html'
        shell(
            f'curl -fsS --max-time 30 --cacert /tmp/fixture-ca.pem {wrong}', 'certificate', status=60)
        print('PASS networking: real loopback sockets, DHCP/DNS configuration, HTTP bodies/chunks/redirects, TLS trust and hostname rejection', flush=True)

        remote = fixture.url('repos/main.git')
        shell(f'git clone -q {remote} /tmp/http-repo', timeout=180)
        shell('git -C /tmp/http-repo rev-parse HEAD', fixture.head)
        shell('git -C /tmp/http-repo fsck --full')
        shell('git -C /tmp/http-repo config user.name "ModernOS Test"; '
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
        vm = VM(cpus, storage=False, iso=iso, label='network-offline-' + network_model + '-' + accel, network=False, accel=accel)
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
    args = parser.parse_args()
    exercise(args.cpus, args.iso, args.network_model, args.accel)
