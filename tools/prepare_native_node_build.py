#!/usr/bin/env python3
"""Prepare a guest-only Node CLI from a completed, readonly V8 runtime graph."""
from build_state import project_lock
from build_state import atomic_write
import argparse,hashlib,json,re,shutil,subprocess
from pathlib import Path

def sha(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--parent',type=Path,required=True)
    parser.add_argument('--cache-parent',type=Path,help='Completed private graph to reuse; original R6 assets still come from --parent')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();parent=args.parent.resolve();out=args.output.resolve()
    built=json.loads((parent/'BUILD.json').read_text())
    runtime_plan=json.loads((parent/'PLAN.json').read_text())
    cache_parent=args.cache_parent.resolve() if args.cache_parent else parent
    record=json.loads((cache_parent/'PLAN.json').read_text())
    assert sha(built['binary'])==built['binary_sha256']
    assert built['features']['jitless_compiled'] and built['features']['drumbrake_explicit_bounds_checks']
    if shutil.disk_usage(out.parent).free<32*1024**3:
        raise RuntimeError('Insufficient disk headroom before copying the private Node build graph')
    out.mkdir(parents=True,exist_ok=False);view=out/'cached-view'
    subprocess.run(['cp','-a','--reflink=always',record['output_view'],str(view)],check=True)
    root=Path(record['gn_command'][0]).parents[2]
    source_dir=Path(__file__).resolve().parents[1]/'userland/electron-runtime'
    overlays=[]
    for relative,name in [('src/node_main.cc','node_cli_main.cc'),
                          ('unofficial.gni','node_cli_unofficial.gni'),
                          ('src/node_v8_platform-inl.h','node_v8_platform-inl.h'),
                          ('lib/child_process.js','node_child_process.js')]:
        original=root/'third_party/electron_node'/relative
        policy=source_dir/name;frozen=out/name;frozen.write_bytes(policy.read_bytes())
        overlays.append((original,policy,frozen))
    confinement=['--ro-bind' if item=='--bind' else item for item in record['confinement']]
    confinement+=['--bind',str(out),str(out),'--bind',str(view),record['logical_build_dir']]
    # Reuse CLI objects while selecting the chosen runtime's V8 source overlay.
    # The cache can belong to an earlier runtime; it supplies no source policy.
    for original,frozen in runtime_plan['source_overlays'].items():
        confinement+=['--ro-bind',str(frozen),str(original)]
    for original,policy,frozen in overlays:confinement+=['--ro-bind',str(frozen),str(original)]
    environment={**record['environment'],'TMPDIR':str(out),'TMP':str(out),'TEMP':str(out)}
    gn=[item.replace('--root-pattern=//electron:electron_app','--root-pattern=//third_party/electron_node:node')
            .replace('--root-target=//electron:electron_app','--root-target=//third_party/electron_node:node')
        for item in record['gn_command']]
    ninja=record['build_command'][0]
    commands=[('gn',gn),('gn-stamp',['/usr/bin/touch','build.ninja.stamp']),
              ('gn-restat',[ninja,'-t','restat','build.ninja.stamp']),
              ('ninja-plan',[ninja,'-n','-v','-j2','node'])]
    for name,command in commands:
        with (out/(name+'.log')).open('xb') as f:
            subprocess.run(confinement+['--',*command],env=environment,stdin=subprocess.DEVNULL,
                           stdout=f,stderr=subprocess.STDOUT,check=True)
    plan=(out/'ninja-plan.log').read_text();steps=re.findall(r'^\[(\d+)/(\d+)\] (.*)$',plan,re.M)
    compiler_steps=sum('clang' in item[2] and ' -c ' in item[2] for item in steps)
    assert 0<len(steps)<=64 and compiler_steps<=32,(len(steps),compiler_steps)
    # Only pinned host build generators may execute; the linked CLI is guest-only.
    assert not re.search(r'(?:^|\s)(?:\./)?node(?:\s|$)', '\n'.join(x[2] for x in steps))
    inputs={**record['original_inputs'],**record.get('source_overlay_hashes',{}),
            **runtime_plan['original_inputs'],**runtime_plan['source_overlay_hashes'],
            str(parent/'BUILD.json'):sha(parent/'BUILD.json'),built['binary']:built['binary_sha256'],
            str(cache_parent/'PLAN.json'):sha(cache_parent/'PLAN.json'),
            str(cache_parent/'cached-view/args.gn'):sha(cache_parent/'cached-view/args.gn')}
    for original,policy,frozen in overlays:
        inputs.update({str(original):sha(original),str(policy):sha(policy),str(frozen):sha(frozen)})
    for asset in built['sidecars'].values():inputs[asset['path']]=asset['sha256']
    output={'state':'prepared; native guest CLI not yet built','parent':str(parent),
            'original_inputs':inputs,'output_view':str(view),'logical_build_dir':record['logical_build_dir'],
            'confinement':confinement,'environment':environment,'gn_command':gn,
            'build_command':[ninja,'-j2','node'],'steps':len(steps),'compiler_steps':compiler_steps,
            'gn_args_sha256':sha(view/'args.gn'),'plan_sha256':sha(out/'ninja-plan.log'),
            'source_overlays':[{'original':str(original),'frozen':str(frozen),'sha256':sha(frozen)} for original,policy,frozen in overlays],
            'features':{**built['features'],'allocator_zero_segment_contract':True,
                        'standalone_node_child_process':True},'guest_execution_on_host':False}
    atomic_write(out/'PLAN.json', json.dumps(output,indent=2)+'\n')
    print('Prepared native Node CLI:',len(steps),'steps,',compiler_steps,'compiler steps',flush=True)

if __name__ == '__main__':
    with project_lock():
        main()
