#!/usr/bin/env python3
"""Run real guest clipboard, scoped foreground and existing private PTY checks."""
import argparse
from test_runtime import VM,assert_success

def exercise(cpus,iso,accel,label,cli_only=False,memory='2G'):
    vm=VM(cpus,storage=False,network=False,iso=iso,accel=accel,label=label,memory=memory)
    try:
        for command,marker,timeout in ([] if cli_only else [('clipboard-check','CLIPBOARD CHECK PASS',90),('foreground-check','FOREGROUND CHECK PASS',90),('unix-tools-check','UNIX TOOLS PTY CHECK PASS',90),('web-capability-check','WEB CAPABILITY CHECK PASS',180)]):
            output=vm.shell_command(command,marker,timeout) if command=='unix-tools-check' else vm.command(command,marker,timeout);assert_success(output)
            print('PASS real guest '+command,flush=True)
        scripts=[
            "set -e; printf 'first\\nsecond\\n' >/tmp/clipboard-expected; clip -c </tmp/clipboard-expected; clip -p >/tmp/clipboard-actual; cmp /tmp/clipboard-expected /tmp/clipboard-actual",
            "set -e; if printf '\\033bad' | clip -c; then exit 1; fi; clip -p | cmp - /tmp/clipboard-expected",
            "set -e; head -c65536 /dev/zero | tr '\\000' A | clip -c; clip -p >/tmp/clipboard-large; test \"$(wc -c </tmp/clipboard-large)\" -eq 65536",
            "set -e; if head -c65537 /dev/zero | tr '\\000' B | clip -c; then exit 1; fi; clip -p | cmp - /tmp/clipboard-large; clip --clear; test -z \"$(clip -p)\"",
        ]
        for script in scripts:vm.shell_command(script,timeout=90)
        # Exercise ash's optimized direct exec/redirection, without the probe
        # wrapper creating a managed subshell first.
        assert_success(vm.command('clip --clear'))
        assert_success(vm.command("build-sh -c 'clip -c < /tmp/clipboard-expected'"))
        output=vm.command('clip -p','first\nsecond\n');assert_success(output)
        vm.shell_command('gcc /usr/src/coast/examples/clipboard-example.c -o /tmp/clipboard-sdk && /tmp/clipboard-sdk',timeout=120)
        vm.command('clip -p','NATIVE SDK CLIPBOARD\n')
        print(f'PASS {cpus}CPU {accel} native clipboard CLI/app boundaries, exact64KiB, malformed/oversized preservation and clear',flush=True)
    finally:vm.close()
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--iso',required=True);p.add_argument('--label',required=True)
    p.add_argument('--cli-only',action='store_true');p.add_argument('--cpus',type=int,nargs='+',default=[1,4]);p.add_argument('--accel',choices=('tcg','kvm'),default='tcg')
    p.add_argument('--memory',default='2G');a=p.parse_args()
    for n in a.cpus:exercise(n,a.iso,a.accel,a.label,a.cli_only,a.memory)
