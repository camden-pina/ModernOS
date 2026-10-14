#!/usr/bin/env python3
"""Generate actual GCC tables in ModernOS; compare with the same native GNU awk
running on the build host. No compiler tables are supplied to the guest.
"""
import argparse
import hashlib
from pathlib import Path
import shlex
import subprocess
import tarfile

from test_runtime import ROOT, VM, WORK, assert_success

OPT_FILES = [
    'ada/gcc-interface/lang.opt', 'd/lang.opt', 'fortran/lang.opt', 'go/lang.opt',
    'lto/lang.opt', 'm2/lang.opt', 'rust/lang.opt', 'c-family/c.opt', 'common.opt',
    'params.opt', 'analyzer/analyzer.opt', 'config/fused-madd.opt',
    'config/i386/i386.opt', 'config/gnu-user.opt', 'config/linux.opt',
    'config/linux-android.opt',
]
GENERATORS = [
    ('options.h', 'opth-gen.awk', None),
    ('options.cc', 'optc-gen.awk', 'config.h system.h coretypes.h options.h tm.h'),
    ('options-save.cc', 'optc-save-gen.awk', 'config.h system.h coretypes.h tm.h'),
    ('options-urls.cc', 'options-urls-cc-gen.awk', 'config.h system.h coretypes.h tm.h'),
]


def exercise(cpus, iso=None):
    directory = WORK / ('gcc-generators-' + str(cpus))
    directory.mkdir(parents=True, exist_ok=True)
    scripts = ['opt-gather.awk', 'opt-functions.awk', 'opt-read.awk'] + [g[1] for g in GENERATORS]
    inputs = OPT_FILES + [f + '.urls' for f in OPT_FILES]
    files = inputs + scripts + ['Makefile.in']
    with tarfile.open(ROOT / 'userland/vendor/gcc-14.2.0.tar.gz') as archive:
        for name in files:
            member = archive.extractfile('gcc-14.2.0/gcc/' + name)
            target = directory / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(member.read())

    awk = str(ROOT / 'userland/bin/gawk')
    with (directory / 'optionlist').open('wb') as output:
        subprocess.run([awk, '-f', 'opt-gather.awk'] + inputs,
                       cwd=directory, stdout=output, check=True)
    expected = {'optionlist': hashlib.sha256((directory / 'optionlist').read_bytes()).hexdigest()}
    for name, generator, header in GENERATORS:
        arguments = [awk, '-f', 'opt-functions.awk', '-f', 'opt-read.awk', '-f', generator]
        if header:
            arguments += ['-v', 'header_name=' + header]
        with (directory / 'optionlist').open('rb') as source, (directory / name).open('wb') as output:
            subprocess.run(arguments, cwd=directory, stdin=source, stdout=output, check=True)
        expected[name] = hashlib.sha256((directory / name).read_bytes()).hexdigest()

    vm = VM(cpus, storage=False, network=False, iso=iso, label='gcc-generators')
    try:
        def shell(script, timeout=120):
            output = vm.command('build-sh -ec ' + shlex.quote(script), timeout=timeout)
            assert_success(output)
            return output

        vm.command('gawk --version', 'GNU Awk 5.4.0')
        vm.command('awk --version', 'GNU Awk 5.4.0')
        shell('cd /usr/src/modernos; sha256sum -c SHA256SUMS; '
              '/bin/sh -n prepare-gcc-options.sh; /bin/sh -n /usr/bin/bootstrap-toolchain')
        shell('mkdir /tmp/gcc-generators')

        # Extract only upstream generator inputs, not GCC's large source tree.
        # Split the names into a file to respect the console line-input bound.
        names = ['gcc-14.2.0/gcc/' + f for f in files] + ['gcc-14.2.0/move-if-change']
        for index, name in enumerate(names):
            vm.command(('write' if not index else 'append') + ' /tmp/gcc-generator-files ' + shlex.quote(name))
        shell('tar -xJf /usr/src/modernos/gcc-14.2.0.tar.xz --no-same-owner '
              '-C /tmp/gcc-generators -T /tmp/gcc-generator-files', timeout=180)
        prefix = 'cd /tmp/gcc-generators/gcc-14.2.0/gcc; '

        # A loop supplies the long input list without exceeding console input.
        shell(prefix + 'set --; while read name; do case "$name" in *.opt|*.urls) '
              'set -- "$@" "${name#gcc-14.2.0/gcc/}";; esac; done < /tmp/gcc-generator-files; '
              'gawk -f opt-gather.awk "$@" > optionlist')

        for name, generator, header in GENERATORS:
            script = prefix + 'gawk -f opt-functions.awk -f opt-read.awk -f ' + generator
            if header:
                script += ' -v ' + shlex.quote('header_name=' + header)
            shell(script + ' < optionlist > ' + name)

        for name, digest in expected.items():
            output = shell(prefix + 'sha256sum ' + name)
            assert digest + '  ' + name in output, output
        print(f'PASS {cpus} CPU native GNU awk: actual GCC option list, declarations, '
              'language flags, saved options and URLs match byte for byte', flush=True)

        # Exercise resume with the real upstream GNU make recipes and actual
        # malformed BusyBox output, rather than fabricating compiler tables.
        build = '/tmp/gcc-option-resume'
        source = '/tmp/gcc-generators/gcc-14.2.0/gcc'
        runtime = 'x86_64-linux-musl'
        targets = 'optionlist options.h options.cc options-save.cc options-urls.cc'
        shell('mkdir -p ' + build + '/gcc ' + build + '/' + runtime + '/libgcc ' +
              build + '/' + runtime + '/libstdc++-v3')
        shell('cd ' + build + '/gcc; printf "%s\\n" ' +
              shlex.quote('srcdir=' + source) + ' STAMP=touch > Makefile')
        shell('cd ' + build + '/gcc; while read name; do case "$name" in *.opt) '
              'printf \'ALL_OPT_FILES += $(srcdir)/%s\\n\' "${name#gcc-14.2.0/gcc/}";; '
              '*.urls) printf \'ALL_OPT_URL_FILES += $(srcdir)/%s\\n\' "${name#gcc-14.2.0/gcc/}";; '
              'esac; done < /tmp/gcc-generator-files >> Makefile')
        shell('cd ' + build + '/gcc; sed -n \'/^optionlist: /,/^dumpvers:/p\' ' +
              source + '/Makefile.in | sed \'$d\' >> Makefile')
        shell('cd ' + build + '/gcc; make -j2 "AWK=/bin/busybox awk" ' + targets, timeout=300)
        before = shell('sha256sum ' + build + '/gcc/options.cc')
        assert expected['options.cc'] not in before, 'BusyBox no longer reproduces the generator fault'
        shell('touch ' + build + '/' + runtime + '/libgcc/cache-marker ' +
              build + '/' + runtime + '/libstdc++-v3/cache-marker')
        helper = '/bin/sh /usr/src/modernos/prepare-gcc-options.sh ' + build + ' ' + runtime + ' gawk'
        shell(helper)
        shell('test ! -e ' + build + '/' + runtime + '/libgcc; test ! -e ' +
              build + '/' + runtime + '/libstdc++-v3')
        shell('cd ' + build + '/gcc; make -j2 AWK=gawk ' + targets, timeout=300)
        for name, digest in expected.items():
            output = shell('sha256sum ' + build + '/gcc/' + name)
            assert digest + '  ' + build + '/gcc/' + name in output, output
        shell('mkdir ' + build + '/' + runtime + '/libgcc; touch ' +
              build + '/' + runtime + '/libgcc/cache-marker')
        tracked = 's-options s-options-h ' + targets
        shell('cd ' + build + '/gcc; stat -c "%y" ' + tracked + ' > /tmp/gcc-cache-before')
        shell(helper)
        shell('test -f ' + build + '/' + runtime + '/libgcc/cache-marker; '
              'cd ' + build + '/gcc; make -j2 AWK=gawk ' + targets, timeout=300)

        # GCC's real stamp rules can execute harmless @true recipes on every
        # invocation. Compare nanosecond timestamps instead of make -q status.
        shell('cd ' + build + '/gcc; stat -c "%y" ' + tracked + ' > /tmp/gcc-cache-after; '
              'cmp /tmp/gcc-cache-before /tmp/gcc-cache-after')

        print(f'PASS {cpus} CPU native resume: old BusyBox tables regenerated by actual GCC make recipes; '
              'stale runtimes retired and matching cache retained', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--iso')
    args = parser.parse_args()
    exercise(args.cpus, args.iso)
