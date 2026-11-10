#!/usr/bin/env python3
"""Exercise the real pacman/makepkg workflow only in an owned offline guest."""
import argparse
import hashlib
import json
from pathlib import Path
import traceback
import test_runtime as runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    vm = None
    report = dict(state='failed', checks=[], iso=str(args.iso.resolve()),target_host_executed=False,
                  harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    try:
        vm = runtime.VM(4, storage=False, network=True, memory='16G', accel='kvm', iso=args.iso, label='local-repo')

        def check(name, command, expected=None, user=False, timeout=45):
            output = vm.shell_command(command, expect=expected, as_user=user, timeout=timeout)
            (out / (f'{len(report["checks"]):02d}-output.txt')).write_text(output)
            report['checks'].append(name)
            print('PASS ' + name, flush=True)
            return output
        check('DHCP and the actual host repository are ready',
              "curl -q --noproxy '*' --connect-timeout 2 --retry 12 --retry-all-errors --retry-delay 1 --max-time 30 -fsS http://10.0.2.2:8765/health", 'Brine', True)
        check('Coast kernel and hostname identity', 'uname -s; uname -n', 'Coast', True)
        check('upstream pacman 7.1.0', '/usr/bin/pacman --version', '7.1.0', True)
        check('Arch numeric version and epoch ordering', 'test "$(vercmp 1.10-1 1.2-1)" = 1 && test "$(vercmp 2:1-1 1:99-9)" = 1', user=True)
        check('installed package ownership database', '/usr/bin/pacman -Qo /usr/bin/bc /usr/bin/ninja /usr/bin/qt6-demo', 'qt6-demo 6.8.3-1', True)
        check('signed repository metadata query', '/usr/bin/pacman -Ss calculator', 'bc', True)
        check('upstream pacman-key reads the public trust store', '/usr/bin/pacman-key --list-keys', 'Coast Local Repository', True)
        check('ordinary users cannot remove packages', '/usr/bin/pacman -R --noconfirm bc; test $? -ne 0', 'unless you are root', True)
        check('upstream makepkg refuses root', '/usr/bin/makepkg --version; /usr/bin/makepkg; test $? -ne 0', 'root', False)
        check('compile real-credential fakeroot probe', '''printf '%s\\n' '#include <unistd.h>' '#include <sys/syscall.h>' '#include <stdio.h>' 'int main(){printf("FAKE %u REAL %ld\\n",getuid(),syscall(SYS_getuid));return !(getuid()==0 && syscall(SYS_getuid)==1000);}' > /home/user/fake.c; gcc-dynamic /home/user/fake.c -o /home/user/fake''', user=True, timeout=90)
        check('fakeroot preserves real kernel user ID', '/usr/bin/fakeroot /home/user/fake', 'FAKE 0 REAL 1000', True)
        check('fakeroot metadata cannot grant kernel root', "/usr/bin/fakeroot /usr/bin/bash -c 'test $(/usr/bin/id -u) = 0 || exit 97; echo x > /etc/fakeroot-escape; test $? -ne 0'", 'Permission denied', True)
        check('downloaded repository database verifies', '/usr/bin/pacman -Sy --noconfirm', 'Synchronizing', False)
        check('remove installed bc and dc', '/usr/bin/pacman -R --noconfirm bc && test ! -e /usr/bin/bc && test ! -e /usr/bin/dc', 'removing bc')
        check('reinstall bc from signed offline repository', '/usr/bin/pacman -S --noconfirm bc', 'installing bc')
        check('restored program is functional', "printf '6*7\\n' | /usr/bin/bc -q", '42', True)
        check('fetch detached package signature from the host', 'curl -fsS http://10.0.2.2:8765/repo/x86_64/bc-1.08.2-1-x86_64.pkg.tar.gz.sig -o /tmp/tampered.pkg.tar.gz.sig')
        check('tampered package signature is rejected', 'cp /var/cache/pacman/pkg/bc-1.08.2-1-x86_64.pkg.tar.gz /tmp/tampered.pkg.tar.gz; printf x >> /tmp/tampered.pkg.tar.gz; /usr/bin/pacman -U --noconfirm /tmp/tampered.pkg.tar.gz; test $? -ne 0', 'invalid or corrupted package')
        check('rejected package leaves installed files intact', '/usr/bin/pacman -Qkk bc', '0 altered files', True)
        check('tampered repository database is rejected', 'cp /var/lib/pacman/sync/coast.db /tmp/coast.db; printf x >> /var/lib/pacman/sync/coast.db; /usr/bin/pacman -Ss bc; r=$?; cp /tmp/coast.db /var/lib/pacman/sync/coast.db; test $r -ne 0', 'invalid or corrupted database')
        check('missing repository signature is rejected', 'mv /var/lib/pacman/sync/coast.db.sig /tmp/coast.sig; /usr/bin/pacman -Ss bc; r=$?; mv /tmp/coast.sig /var/lib/pacman/sync/coast.db.sig; test $r -ne 0', 'invalid or corrupted database')
        check('dependency removal is refused', '/usr/bin/pacman -R --noconfirm python; test $? -ne 0', 'required by brine')
        check('transaction lock is respected', 'touch /var/lib/pacman/db.lck; /usr/bin/pacman -S --noconfirm bc; r=$?; rm /var/lib/pacman/db.lck; test $r -ne 0', 'unable to lock database')
        check('signed source catalog query', '/usr/bin/brine search', 'hello-coast', True)
        check('signed source RPC metadata', '/usr/bin/brine rpc --type info --arg hello-coast', 'resultcount', True)
        check('source recipes refuse privileged builds', '/usr/bin/brine build hello-coast --reviewed; test $? -ne 0', 'normal user')
        check('hosted image excludes embedded mirrors and legacy helpers', 'test ! -d /var/lib/brine/repository && test ! -d /usr/share/brine/repository && test ! -e /usr/bin/aur', user=True)
        check('refresh verifies the signed host catalog', 'brine refresh', '3 recipes', True)
        check('repeated refresh is atomic and idempotent', 'brine refresh', '3 recipes', True)
        check('review fetches checksum-pinned recipe text', 'brine show hello-coast', 'pkgname=hello-coast', True)
        check('clone real PKGBUILD Git history', '/usr/bin/brine clone hello-coast', 'Recipe checkout', True)
        check('changed source fails checksum verification', 'cd /home/user/Builds/hello-coast; printf bad >> hello.c; /usr/bin/makepkg --verifysource; r=$?; /usr/bin/git checkout -- hello.c; test $r -ne 0', 'validity check', True)
        check('normal-user native source build', 'cd /home/user/Builds/hello-coast && /usr/bin/makepkg --noconfirm', 'Finished making', True, 180)
        check('source archive has root metadata without a root build', "zstd -dc /home/user/Builds/hello-coast/*.pkg.tar.zst > /home/user/hello.tar; python3 -I -c 'import tarfile; a=tarfile.open(\"/home/user/hello.tar\"); assert all(m.uid==0 and m.gid==0 for m in a.getmembers()); print(\"ROOT_ARCHIVE_METADATA\")'", 'ROOT_ARCHIVE_METADATA', True)
        check('unowned filesystem conflicts are refused', 'echo occupied > /usr/bin/hello-coast; /usr/bin/pacman -U --noconfirm /home/user/Builds/hello-coast/hello-coast-1.1-1-x86_64.pkg.tar.zst; r=$?; rm /usr/bin/hello-coast; test $r -ne 0', 'exists in filesystem')
        vm.command('true')
        check('helper installs reviewed local build through sudo', '/usr/bin/brine install hello-coast --reviewed --noconfirm', 'installing hello-coast', True, 180)
        check('locally built program runs without elevation', '/usr/bin/hello-coast', 'uid=1000', True)
        check('source-built package removes normally', '/usr/bin/pacman -R --noconfirm hello-coast && test ! -e /usr/bin/hello-coast', 'removing hello-coast')
        check('Coast Bash/helper archive ownership and file hashes match', 'pacman -Qkk bash brine', '0 altered files', True, 120)
        check('generated Bash metadata uses the Coast toolchain paths', 'grep -q coast-dynamic.specs /usr/bin/bashbug', user=True)
        check('installed package file validation', '/usr/bin/pacman -Qkk bc ninja', '0 altered files', True)
        report['state'] = 'passed'
    except BaseException as error:
        report['error'] = repr(error)
        (out / 'exception.txt').write_text(traceback.format_exc())
        raise
    finally:
        if vm:
            vm.close()
        report['owned_vm_stopped'] = bool(vm and vm.process.poll() is not None)
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
