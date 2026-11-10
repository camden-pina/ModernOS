#!/usr/bin/env python3
"""Build the standalone guest Node target; never execute it on the host."""
from build_state import project_lock
from build_state import atomic_write
import argparse,hashlib,json,os,re,shutil,subprocess,time
from pathlib import Path

def sha(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def available_memory():
    for line in Path('/proc/meminfo').read_text().splitlines():
        if line.startswith('MemAvailable:'):return int(line.split()[1])*1024
    raise RuntimeError('Missing host memory telemetry')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan',type=Path,required=True)
    parser.add_argument('--minimum-free-gib',type=int,choices=range(16,33),default=32,
                        help='Reserved disk headroom for this bounded incremental component build')
    args=parser.parse_args();plan=args.plan.resolve();out=plan.parent
    record=json.loads(plan.read_text());view=Path(record['output_view'])
    def verify():
        for name,digest in record['original_inputs'].items():
            if sha(name)!=digest:raise RuntimeError('Preserved build input changed: '+name)
        assert sha(view/'args.gn')==record['gn_args_sha256']
        assert sha(out/'ninja-plan.log')==record['plan_sha256']
    verify()
    assert record['steps']<=64 and record['compiler_steps']<=32
    free_start=shutil.disk_usage(out).free;started=time.monotonic()
    minimum_free=args.minimum_free_gib*1024**3
    if free_start<minimum_free or available_memory()<8*1024**3:
        raise RuntimeError('Insufficient bounded component-build headroom')
    log=out/'native-build.log'
    with log.open('xb') as stream:
        process=subprocess.Popen(record['confinement']+['--',*record['build_command']],
            env=record['environment'],stdin=subprocess.DEVNULL,stdout=stream,stderr=subprocess.STDOUT)
        try:
            while process.poll() is None:
                free=shutil.disk_usage(out).free
                if free<minimum_free or free_start-free>2*1024**3:
                    raise RuntimeError('Node component-build disk limit reached')
                if available_memory()<8*1024**3 or time.monotonic()-started>3600:
                    raise RuntimeError('Node component-build memory/time limit reached')
                time.sleep(2)
            if process.returncode:raise RuntimeError('Native Node build failed; inspect '+str(log))
        except BaseException:
            if process.poll() is None:
                process.terminate()
                try:process.wait(timeout=10)
                except subprocess.TimeoutExpired:process.kill();process.wait()
            raise
    root=Path(record['gn_command'][0]).parents[2];binary=out/'node'
    subprocess.run(record['confinement']+['--',str(root/'third_party/llvm-build/Release+Asserts/bin/llvm-strip'),
        '--strip-all','-o',str(binary),str(Path(record['logical_build_dir'])/'node')],
        env=record['environment'],stdin=subprocess.DEVNULL,check=True)
    assert binary.stat().st_size<=256*1024**2
    verify()
    versions=subprocess.check_output(['readelf','--version-info',str(binary)],text=True)
    imports=sorted({tuple(map(int,v.split('.'))) for v in re.findall(r'GLIBC_([0-9.]+)',versions)})
    assert not imports or max(imports)<=(2,36),imports
    parent=json.loads((Path(record['parent'])/'BUILD.json').read_text())
    assets={name:parent['sidecars'][name] for name in ['snapshot_blob.bin','icudtl.dat']}
    license_path=root/'third_party/electron_node/LICENSE'
    result={'state':'built; genuine guest CLI and inspector validation pending','binary':str(binary),
        'binary_sha256':sha(binary),'binary_bytes':binary.stat().st_size,
        'unstripped':str(view/'node'),'unstripped_sha256':sha(view/'node'),
        'plan':str(plan),'plan_sha256':sha(plan),'parent_electron_sha256':parent['binary_sha256'],
        'sidecars':assets,'license':{'path':str(license_path),'sha256':sha(license_path)},
        'glibc_imports':imports,'features':record['features'],'native_build_log_sha256':sha(log),
        'elapsed_seconds':time.monotonic()-started,'guest_execution_on_host':False,
        'minimum_free_gib':args.minimum_free_gib,'maximum_disk_drop_gib':2,
        'scope':'Standalone Node main, compiled readonly jitless, existing R6 explicit-bounds V8; readonly original graph and network-unshared build'}
    atomic_write(out/'BUILD.json', json.dumps(result,indent=2)+'\n')
    print('Built guest Node CLI:',result['binary_sha256'],result['binary_bytes'],'bytes',flush=True)

if __name__ == '__main__':
    with project_lock():
        main()
