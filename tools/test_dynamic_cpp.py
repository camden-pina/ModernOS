#!/usr/bin/env python3
"""Execute the real shared C++ ABI and native shared C++ compiler in guests."""
import argparse
import shlex
from test_runtime import VM, WORK, assert_success

def exercise(cpus, iso, accel):
    WORK.mkdir(parents=True, exist_ok=True)
    vm = VM(cpus, storage=False, iso=iso, accel=accel, label='dynamic-cpp')
    try:
        output = vm.command('/usr/lib/coast/dynamic-cpp/cpp-check', 'DYNAMIC CPP CHECK PASS', timeout=100)
        assert_success(output)
        for marker in ('CPP LIB CTOR PASS','CPP CALLBACK EXCEPTION PASS','CPP APPLICATION RTTI EXCEPTIONS PASS',
                       'CPP PLUGIN CTOR PASS','CPP TLS DLOPEN ERRORS PASS','CPP CONCURRENT UNWIND PASS','CPP PLUGIN DTOR PASS','CPP LIB DTOR PASS'):
            assert marker in output, (marker,output)
        missing=vm.shell_command('/usr/lib/coast/dynamic-cpp/cpp-missing; result=$?; test "$result" -ne 0 && echo CPP_MISSING_DEPENDENCY_PASS','CPP_MISSING_DEPENDENCY_PASS')
        assert_success(missing)
        assert 'Error loading shared library libcpp-required.so' in missing,missing
        output = vm.shell_command('cd /tmp; g++-dynamic -std=c++17 /usr/src/coast/dynamic-cpp/hello.cpp -o cpp-native && ./cpp-native && readelf -d cpp-native',
                                  'DYNAMIC CPP NATIVE COMPILE PASS',timeout=180)
        assert_success(output)
        assert 'libstdc++.so.6' in output and 'libgcc_s.so.1' in output,output
        script='mkdir -p /tmp/cpp-sdk; cp /usr/src/coast/dynamic-cpp/*.cpp /usr/src/coast/dynamic-cpp/*.hpp /usr/src/coast/dynamic-cpp/Makefile /tmp/cpp-sdk; cd /tmp/cpp-sdk; PATH=/usr/bin:/bin make -j1 check'
        output=vm.shell_command(script,'DYNAMIC CPP CHECK PASS',timeout=240)
        assert_success(output)
        assert 'CPP LIB DTOR PASS' in output and 'CPP PLUGIN DTOR PASS' in output,output
        print(f'PASS shared C++ ABI, exceptions/RTTI/TLS, applications and native SDK ({cpus} CPUs, {accel})',flush=True)
    finally:
        vm.close()

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cpus',type=int,nargs='+',default=[1,4]);p.add_argument('--iso',required=True)
    p.add_argument('--accel',choices=('tcg','kvm'),default='tcg');a=p.parse_args()
    for count in a.cpus:exercise(count,a.iso,a.accel)
