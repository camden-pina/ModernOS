#!/usr/bin/env python3
"""Prepare a private DrumBrake build graph and record its dry-run work.

Only pinned host GN/Ninja execute. Original sources/build outputs are mounted
read-only; generated files land in an exact-path private reflinked build view.
This preparation never builds or launches the guest application.
"""
from build_state import project_lock
from build_state import atomic_write
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]
def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--artifacts',type=Path,required=True)
    p.add_argument('--output-root',type=Path,required=True)
    p.add_argument('--lite-mode',action='store_true',help='compile out the unused JS JIT compilers while retaining the V8 sandbox and Wasm interpreter')
    a=p.parse_args();artifacts=a.artifacts.resolve();out=a.output_root.resolve()
    out.mkdir(parents=True,exist_ok=False)
    electron=artifacts.parent/'codeoss-embedder-e2-build'
    source=electron/'checkout/src';cached=electron/'state/gn1-graph04/out'
    config_path=electron/'config/APP-LINK-04.json';config=json.loads(config_path.read_text())
    overlays={source/'electron/shell/app/node_main.cc':ROOT/'userland/electron-runtime/node_main.cc',
              source/'electron/shell/browser/javascript_environment.cc':ROOT/'userland/electron-runtime/javascript_environment.cc',
              source/'gin/v8_initializer.cc':ROOT/'userland/electron-runtime/gin_v8_initializer.cc'}
    if a.lite_mode:
        overlays[source/'v8/src/builtins/x64/builtins-x64.cc']=ROOT/'userland/electron-runtime/v8_builtins_x64.cc'
        overlays[source/'v8/BUILD.gn']=ROOT/'userland/electron-runtime/v8_BUILD.gn'
    baseline={str(path):sha(path) for path in [cached/'electron',cached/'args.gn',*overlays,*overlays.values()]}
    atomic_write(out/'BASELINE.json', json.dumps(baseline,indent=2)+'\n')
    frozen=out/'source-overlays';frozen.mkdir()
    for target,replacement in list(overlays.items()):
        path=frozen/replacement.name;path.write_bytes(replacement.read_bytes());overlays[target]=path
    view=out/'cached-view'
    subprocess.run(['cp','-a','--reflink=always',str(cached),str(view)],check=True)
    extra='\nv8_enable_drumbrake = true\nv8_enable_webassembly = true\nv8_enable_sandbox = true\nv8_drumbrake_bounds_checks = true\n'
    if a.lite_mode:extra+='v8_enable_lite_mode = true\nv8_enable_turbofan = false\nv8_enable_sparkplug = false\nv8_enable_maglev = false\n'
    (view/'args.gn').write_text((cached/'args.gn').read_text()+extra)
    conf=list(config['confinement'])
    for i in range(len(conf)-2):
        if conf[i]=='--bind':conf[i]='--ro-bind'
    at=next(i for i in range(len(conf)-1) if conf[i:i+2]==['--remount-ro','/'])
    conf[at:at]=['--dir',str(artifacts),'--ro-bind',str(ROOT),str(ROOT)]
    conf+=['--bind',str(out),str(out),'--bind',str(view),str(cached)]
    for target,replacement in overlays.items():conf+=['--ro-bind',str(replacement),str(target)]
    conf+=['--chdir',str(cached)]
    assert '--unshare-net' in conf and '--disable-userns' in conf
    env=dict(config['environment'])
    env.update(TMPDIR=str(out),TMP=str(out),TEMP=str(out),PYTHONDONTWRITEBYTECODE='1',PYTHONNOUSERSITE='1')
    gn=[str(source/'buildtools/linux64/gn'),'--root='+str(source),
        '--root-pattern=//electron:electron_app','--root-target=//electron:electron_app',
        '--script-executable='+str(electron/'tools/cpython311/bin/python3.11'),'gen','.']
    ninja=[str(source/'third_party/ninja/ninja'),'-n','-v','-j2','electron']
    # This preserved graph lists GN's own generated log as a regeneration
    # dependency. Stamp the completed private generation and refresh only that
    # Ninja restat entry; compiler command hashes still force every real change.
    for name,command in [('gn',gn),('gn-complete',['/usr/bin/touch','build.ninja.stamp']),
                         ('gn-restat',[ninja[0],'-t','restat','build.ninja.stamp']),('ninja-plan',ninja)]:
        with (out/(name+'.log')).open('wb') as log:
            result=subprocess.run(conf+['--',*command],env=env,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT)
        if result.returncode:raise SystemExit(name+' failed; see '+str(out/(name+'.log')))
    for name,digest in baseline.items():
        if sha(Path(name))!=digest:raise RuntimeError('Preserved input changed: '+name)
    steps=re.findall(r'^\[(\d+)/(\d+)\] (.*)$',(out/'ninja-plan.log').read_text(),re.M)
    commands=[step[2] for step in steps]
    record=dict(state='prepared dry run only; no guest runtime build or execution',
                original_inputs=baseline,confinement=conf,environment=env,gn_command=gn,
                ninja_plan_command=ninja,build_command=[str(source/'third_party/ninja/ninja'),'-j2','electron'],
                output_view=str(view),logical_build_dir=str(cached),source_overlays={str(k):str(v) for k,v in overlays.items()},
                steps=len(steps),declared_steps=int(steps[-1][1]) if steps else 0,
                compiler_steps=sum('clang' in command and ' -c ' in command for command in commands),
                host_editor_execution=False,gn_args_sha256=sha(view/'args.gn'),
                lite_mode=a.lite_mode,drumbrake_explicit_bounds_checks=True,source_overlay_hashes={str(v):sha(v) for v in overlays.values()},
                plan_sha256=sha(out/'ninja-plan.log'))
    atomic_write(out/'PLAN.json', json.dumps(record,indent=2)+'\n')
    print('Prepared',len(steps),'steps; compilers',record['compiler_steps'],flush=True)

if __name__ == '__main__':
    with project_lock():
        main()
