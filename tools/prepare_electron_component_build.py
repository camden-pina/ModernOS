#!/usr/bin/env python3
"""Prepare a small private rebuild over a completed readonly Electron graph."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path

def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--parent',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--refresh-policies',action='store_true',help='recompile all current native policy overlays over the preserved graph')
    args=parser.parse_args();parent=args.parent.resolve();out=args.output.resolve()
    built=json.loads((parent/'BUILD.json').read_text());record=json.loads((parent/'PLAN.json').read_text())
    assert sha(built['binary'])==built['binary_sha256']
    assert sha(built['unstripped'])==built['unstripped_sha256']
    assert built['features']['jitless_compiled'] and built['features']['drumbrake_explicit_bounds_checks']
    preserved = {**record['original_inputs'], **record['source_overlay_hashes']}
    policies = Path(__file__).resolve().parents[1]/'userland/electron-runtime'
    original_inputs = {}
    for name,digest in preserved.items():
        path=Path(name)
        if args.refresh_policies and path.parent==policies:
            # Working policies may advance. Prove the parent used the frozen
            # bytes recorded in its graph, then inventory the new policy below.
            assert digest in record['source_overlay_hashes'].values(),name
        else:
            assert sha(path)==digest,name
            original_inputs[name]=digest
    out.mkdir(parents=True,exist_ok=False);view=out/'cached-view'
    subprocess.run(['cp','-a','--reflink=always',record['output_view'],str(view)],check=True)
    source=Path(record['gn_command'][0]).parents[2]
    original=source/'v8/src/heap/heap.cc'
    policy=Path(__file__).resolve().parents[1]/'userland/electron-runtime/v8_heap.cc'
    frozen=out/'source-overlays/heap.cc';frozen.parent.mkdir();frozen.write_bytes(policy.read_bytes())
    overlays={**record['source_overlays'],str(original):str(frozen)}
    if args.refresh_policies:
        names={'node_main.cc':'node_main.cc','javascript_environment.cc':'javascript_environment.cc',
               'v8_initializer.cc':'gin_v8_initializer.cc','builtins-x64.cc':'v8_builtins_x64.cc',
               'BUILD.gn':'v8_BUILD.gn','heap.cc':'v8_heap.cc'}
        for target in list(overlays):
            current=policies/names[Path(target).name]
            replacement=out/'source-overlays'/Path(target).name
            replacement.write_bytes(current.read_bytes())
            overlays[target]=str(replacement)
            original_inputs[str(current)]=sha(current)
    conf=['--ro-bind' if item=='--bind' else item for item in record['confinement']]
    conf+=['--bind',str(out),str(out),'--bind',str(view),record['logical_build_dir'],
           '--ro-bind',str(frozen),str(original)]
    if args.refresh_policies:
        for target,replacement in overlays.items():conf+=['--ro-bind',replacement,target]
    assert '--unshare-net' in conf and '--disable-userns' in conf
    environment={**record['environment'],'TMPDIR':str(out),'TMP':str(out),'TEMP':str(out)}
    ninja=record['build_command'][0]
    if args.refresh_policies:
        for name,command in [('gn',record['gn_command']),('gn-stamp',['/usr/bin/touch','build.ninja.stamp']),('gn-restat',[ninja,'-t','restat','build.ninja.stamp'])]:
            with (out/(name+'.log')).open('xb') as stream:
                subprocess.run(conf+['--',*command],env=environment,stdin=subprocess.DEVNULL,stdout=stream,stderr=subprocess.STDOUT,check=True)
    command=[ninja,'-n','-v','-j2','electron']
    with (out/'ninja-plan.log').open('xb') as stream:
        subprocess.run(conf+['--',*command],env=environment,stdin=subprocess.DEVNULL,stdout=stream,stderr=subprocess.STDOUT,check=True)
    steps=re.findall(r'^\[(\d+)/(\d+)\] (.*)$',(out/'ninja-plan.log').read_text(),re.M)
    compiler_steps=sum('clang' in item[2] and ' -c ' in item[2] for item in steps)
    assert 0<len(steps)<=64 and compiler_steps<=(32 if args.refresh_policies else 16),(len(steps),compiler_steps)
    assert not any(re.search(r'(?:^|\s)(?:\./)?electron(?:\s|$)',item[2]) for item in steps)
    inputs={**original_inputs,str(parent/'BUILD.json'):sha(parent/'BUILD.json'),
            str(parent/'PLAN.json'):sha(parent/'PLAN.json'),built['binary']:built['binary_sha256'],
            built['unstripped']:built['unstripped_sha256'],str(original):sha(original),str(policy):sha(policy)}
    prepared={**record,'state':'prepared bounded component graph; guest validation pending',
              'original_inputs':inputs,'confinement':conf,'environment':environment,
              'source_overlays':overlays,'source_overlay_hashes':{value:sha(value) for value in overlays.values()},
              'steps':len(steps),'compiler_steps':compiler_steps,'output_view':str(view),
              'ninja_plan_command':command,'plan_sha256':sha(out/'ninja-plan.log'),
              'resume_from':{'plan':str(parent/'PLAN.json'),'sha256':sha(parent/'PLAN.json'),
                             'native_build_log':str(parent/'native-build.log'),
                             'native_build_log_sha256':sha(parent/'native-build.log'),
                             'reason':'Recompile native platform policies over the preserved runtime; retain all consistency checks.'}}
    (out/'PLAN.json').write_text(json.dumps(prepared,indent=2)+'\n')
    print('Prepared bounded Electron component:',len(steps),'steps;',compiler_steps,'compilers',flush=True)

if __name__=='__main__':main()
