#!/usr/bin/env python3
"""Build a reviewed private DrumBrake graph, retaining the original runtime.

Host build tools execute inside the network-unshared build namespace. The
guest Electron application never executes on the host. Resource limits stop
only this build's supervised process; partial artifacts remain for inspection.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def memory_available():
    for line in Path('/proc/meminfo').read_text().splitlines():
        if line.startswith('MemAvailable:'):return int(line.split()[1])*1024
    raise RuntimeError('Host memory telemetry unavailable')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--plan',type=Path,required=True)
    p.add_argument('--minimum-free-gib',type=int,choices=range(16,36),default=35,
                   help='Lower values require a bounded incremental plan')
    a=p.parse_args();plan_path=a.plan.resolve();out=plan_path.parent
    record=json.loads(plan_path.read_text());view=Path(record['output_view'])
    for name,digest in {**record['original_inputs'],**record['source_overlay_hashes']}.items():
        if sha(Path(name))!=digest:raise RuntimeError('Prepared input changed: '+name)
    if sha(view/'args.gn')!=record['gn_args_sha256']:raise RuntimeError('Prepared GN arguments changed')
    if sha(out/'ninja-plan.log')!=record['plan_sha256']:raise RuntimeError('Prepared work plan changed')
    if (record['steps']<100 and not record.get('resume_from')) or not record['lite_mode']:raise RuntimeError('A complete reviewed lite-mode work plan is required')
    if record.get('resume_from'):
        parent=record['resume_from']
        if sha(Path(parent['plan']))!=parent['sha256'] or sha(Path(parent['native_build_log']))!=parent['native_build_log_sha256']:
            raise RuntimeError('Incremental parent provenance changed')
    bounded=bool(record.get('resume_from')) and record['steps']<=64 and record['compiler_steps']<=16
    if a.minimum_free_gib<35 and not bounded:raise RuntimeError('Reduced headroom requires a bounded incremental plan')
    minimum_free=a.minimum_free_gib*1024**3;maximum_drop=(4 if bounded else 16)*1024**3
    started=time.monotonic();initial_free=shutil.disk_usage(out).free
    if initial_free<minimum_free or memory_available()<8*1024**3:raise RuntimeError('Insufficient host build headroom')
    log_path=out/'native-build.log'
    with log_path.open('xb') as log:
        child=subprocess.Popen(record['confinement']+['--',*record['build_command']],
                               env=record['environment'],stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT)
        try:
            while child.poll() is None:
                free=shutil.disk_usage(out).free
                if free<minimum_free or initial_free-free>maximum_drop:
                    raise RuntimeError('Private build disk headroom limit reached')
                if memory_available()<8*1024**3:raise RuntimeError('Host memory headroom limit reached')
                if time.monotonic()-started>7200:raise RuntimeError('Private build duration limit reached')
                time.sleep(2)
            if child.returncode:raise RuntimeError('Native build failed; see '+str(log_path))
        except BaseException:
            if child.poll() is None:
                child.terminate()
                try:child.wait(timeout=10)
                except subprocess.TimeoutExpired:child.kill();child.wait()
            raise
    source=Path(next(iter(record['source_overlays']))).parents[3]
    # Locate the same pinned LLVM strip through the build graph's source root.
    gn=Path(record['gn_command'][0]);source=gn.parents[2]
    binary=out/'electron'
    subprocess.run(record['confinement']+['--',str(source/'third_party/llvm-build/Release+Asserts/bin/llvm-strip'),
                    '--strip-all','-o',str(binary),str(Path(record['logical_build_dir'])/'electron')],
                   env=record['environment'],stdin=subprocess.DEVNULL,check=True)
    if binary.stat().st_size>256*1024**2:raise RuntimeError('Candidate exceeds the existing Electron image limit')
    for name,digest in {**record['original_inputs'],**record['source_overlay_hashes']}.items():
        if sha(Path(name))!=digest:raise RuntimeError('Preserved input changed: '+name)
    sidecars={}
    for name in ['snapshot_blob.bin','resources.pak','chrome_100_percent.pak','chrome_200_percent.pak','icudtl.dat']:
        path=view/name
        if path.is_file():sidecars[name]={'path':str(path),'sha256':sha(path),'size':path.stat().st_size}
    if 'snapshot_blob.bin' not in sidecars:raise RuntimeError('Matching V8 snapshot is required')
    result=dict(state='built; guest interpreter/runtime validation pending',binary=str(binary),binary_sha256=sha(binary),
                unstripped=str(view/'electron'),unstripped_sha256=sha(view/'electron'),sidecars=sidecars,
                plan=str(plan_path),plan_sha256=sha(plan_path),resume_from=record.get('resume_from'),original_inputs=record['original_inputs'],
                sources=record['source_overlay_hashes'],gn_args_sha256=sha(view/'args.gn'),
                build_command=record['build_command'],confinement=record['confinement'],
                native_build_log_sha256=sha(log_path),elapsed_seconds=time.monotonic()-started,
                features={'jitless_compiled':True,'javascript_jit_compilers':False,'wasm_interpreter':True,'v8_sandbox':True,'drumbrake_explicit_bounds_checks':bool(record.get('drumbrake_explicit_bounds_checks'))},
                host_editor_execution=False,scope='Private reflinked GN/Ninja build; original runtime/source/objects unchanged')
    result.update(minimum_free_gib=a.minimum_free_gib,maximum_disk_drop_gib=maximum_drop//1024**3)
    (out/'BUILD.json').write_text(json.dumps(result,indent=2)+'\n')
    print('Runtime ready:',binary,result['binary_sha256'],flush=True)

if __name__=='__main__':main()
