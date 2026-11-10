#!/usr/bin/env python3
"""Exercise native GN graph generation, threaded Python helpers and Ninja C++."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gn', type=Path, default=Path('/usr/bin/gn'))
    parser.add_argument('--ninja', type=Path, default=Path('/usr/bin/ninja'))
    args = parser.parse_args()
    if platform.system() != 'Coast':
        parser.error('Run this integration check inside Coast')
    gn, ninja = args.gn.resolve(strict=True), args.ninja.resolve(strict=True)
    work = Path(tempfile.mkdtemp(prefix='gn-check-', dir='/tmp'))
    (work / 'build').mkdir()
    (work / '.gn').write_text('buildconfig = "//build/BUILDCONFIG.gn"\n')
    (work / 'build/BUILDCONFIG.gn').write_text('set_default_toolchain("//build:native")\n')
    (work / 'BUILD.gn').write_text('declare_args() { fail_probe = false }\n'
        'if (fail_probe) { exec_script("//probe.py", [ "fail" ], "string") }\n'
        'group("all") { deps = [ "//one:one", "//two:two" ] }\n')
    (work / 'build/BUILD.gn').write_text('''toolchain("native") {
  tool("cxx") {
    command = "g++ -std=c++17 -O2 -fno-pie -mno-avx -mno-avx2 -MMD -MF {{output}}.d -c {{source}} -o {{output}}"
    outputs = [ "{{source_out_dir}}/{{target_output_name}}.{{source_name_part}}.o" ]
    depfile = "{{output}}.d"
    depsformat = "gcc"
  }
  tool("link") {
    command = "g++ -static -no-pie -pthread -o {{output}} {{inputs}}"
    outputs = [ "{{root_out_dir}}/{{target_output_name}}" ]
  }
  tool("stamp") {
    command = "touch {{output}}"
  }
  tool("copy") {
    command = "cp -f {{source}} {{output}}"
  }
}
''')
    (work / 'probe.py').write_text('import os,sys\nfrom pathlib import Path\n'
        'expected = "failed-out" if sys.argv[1] == "fail" else "out"\n'
        'assert Path.cwd() == Path(__file__).parent / expected\n'
        'if sys.argv[1] == "fail":\n print("GN_EXPECTED_CHILD_FAILURE",file=sys.stderr);sys.exit(7)\n'
        'sys.stderr.write("x"*131072)\n'
        '(Path(__file__).parent / ("probe-"+sys.argv[1]+".pid")).write_text(str(os.getpid()))\nprint(42)\n')
    for name in ('one', 'two'):
        directory = work / name
        directory.mkdir()
        (directory / 'BUILD.gn').write_text('value = exec_script("//probe.py", [ "' + name + '" ], "trim string")\n'
            'assert(value == "42")\nexecutable("' + name + '") { sources = [ "main.cc" ] }\n')
        (directory / 'main.cc').write_text('#include <cstdio>\n#include <thread>\n'
            'int main(){int n=0;std::thread t([&]{n=42;});t.join();'
            'std::printf("GN_NATIVE_PROGRAM %d\\n",n);return n!=42;}\n')

    def run(command, success=True):
        result = subprocess.run(list(map(str, command)), cwd=work, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
        with (work / 'check.log').open('a') as log:
            log.write('$ ' + repr(command) + '\n' + result.stdout + '\n')
        if (result.returncode == 0) != success:
            raise AssertionError(result.stdout[-5000:])
        return result.stdout

    assert '3357c4f51b1a' in run([gn, '--version'])
    run([gn, 'gen', 'out', '--check', '--threads=4', '--script-executable=/usr/bin/python3'])
    assert all((work / ('probe-' + name + '.pid')).is_file() for name in ('one', 'two'))
    run([ninja, '-C', 'out', '-j2'])
    for name in ('one', 'two'):
        assert 'GN_NATIVE_PROGRAM 42' in run([work / 'out' / name])
    assert '//one/main.cc' in run([gn, 'desc', 'out', '//one:one', 'sources'])
    assert '//two:two' in run([gn, 'ls', 'out'])
    failure = run([gn, 'gen', 'failed-out', '--args=fail_probe=true',
        '--script-executable=/usr/bin/python3'], success=False)
    assert 'GN_EXPECTED_CHILD_FAILURE' in failure
    run([gn, 'format', 'BUILD.gn'])
    run([gn, 'format', '--dry-run', 'BUILD.gn'])
    checks = ['exact Chromium GN revision', 'threaded graph generation and checked includes',
        'Python helper cwd/stdout/stderr with output above pipe capacity',
        'Ninja compiles and runs two C++ thread consumers', 'graph description and target listing',
        'child execution failure propagates', 'native GN formatting']
    (work / 'report.json').write_text(json.dumps(dict(state='passed', checks=checks,
        gn_sha256=hashlib.sha256(gn.read_bytes()).hexdigest()), indent=2) + '\n')
    print('GN_ALL_PASS ' + str(len(checks)) + ' report=' + str(work / 'report.json'), flush=True)


if __name__ == '__main__':
    main()
