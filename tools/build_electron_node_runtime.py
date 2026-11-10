#!/usr/bin/env python3
"""Compile selected native runtime sources and relink cached Electron inputs.

Original sources, objects, executable and ThinLTO cache stay read-only. Only
the selected compiler/linker run; the guest Electron binary is never executed.
"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def sha(p):
    with p.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--artifacts',type=Path,required=True)
    p.add_argument('--output-root',type=Path,required=True)
    a=p.parse_args();artifacts=a.artifacts.resolve();out=a.output_root.resolve()
    out.mkdir(parents=True,exist_ok=False)
    electron=artifacts.parent/'codeoss-embedder-e2-build'
    cached=electron/'state/gn1-graph04/out'
    units=[]
    for relative in ['shell/app/node_main.cc','shell/browser/javascript_environment.cc']:
        source=ROOT/'userland/electron-runtime'/Path(relative).name
        if source.is_file():
            units.append(dict(relative=relative,source=source,
                              original=electron/'checkout/src/electron'/relative,
                              target='obj/electron/electron_lib/'+source.stem+'.o',
                              output=out/(source.stem+'.o')))
    config_path=electron/'config/APP-LINK-04.json'
    config=json.loads(config_path.read_text())
    baseline_paths=[cached/'electron',cached/'args.gn']
    for unit in units:baseline_paths += [unit['original'],unit['source'],cached/unit['target']]
    baseline={str(p):sha(p) for p in baseline_paths}
    (out/'BASELINE.json').write_text(json.dumps(baseline,indent=2)+'\n')
    patch=[]
    for unit in units:
        patch.extend(difflib.unified_diff(unit['original'].read_text().splitlines(True),unit['source'].read_text().splitlines(True),fromfile='original/'+unit['relative'],tofile='coast/'+unit['relative']))
    (out/'SOURCE.patch').write_text(''.join(patch))
    # Preserve every logical linker path and argument, including ./ prefixes
    # in split-debug and cache keys. All physical writes land in private COW
    # copies; generated headers, objects and libraries are remounted read-only.
    view=out/'cached-view'
    subprocess.run(['cp','-a','--reflink=always',str(cached),str(view)],check=True)
    conf=list(config['confinement'])
    for i in range(len(conf)-2):
        if conf[i]=='--bind':conf[i]='--ro-bind'
    at=next(i for i in range(len(conf)-1) if conf[i:i+2]==['--remount-ro','/'])
    conf[at:at]=['--dir',str(artifacts),'--ro-bind',str(ROOT),str(ROOT)]
    conf+=['--bind',str(out),str(out),'--bind',str(view),str(cached)]
    for path in view.iterdir():
        if path.name not in ['electron','electron.rsp','electron-dwo','thinlto-cache']:
            conf+=['--ro-bind',str(path),str(cached/path.name)]
    for unit in units:conf+=['--ro-bind',str(unit['source']),str(unit['original'])]
    conf+=['--chdir',str(cached)]
    assert '--unshare-net' in conf and '--disable-userns' in conf
    env=dict(HOME='/home/camdenp',PATH='/usr/bin:/bin',LANG='C.UTF-8',LC_ALL='C.UTF-8',TMPDIR=str(out),PYTHONDONTWRITEBYTECODE='1',PYTHONNOUSERSITE='1')
    def command(target):
        return shlex.split(subprocess.check_output(['ninja','-t','commands','-s',target],cwd=cached,text=True))
    compiles={}
    for unit in units:
        original_command=command(unit['target']);compile_cmd=original_command.copy()
        compile_cmd[compile_cmd.index('-o')+1]=str(unit['output'])
        compile_cmd[compile_cmd.index('-MF')+1]=str(unit['output'])+'.d'
        compiles[unit['relative']]=dict(original=original_command,selected=compile_cmd)
        (out/'COMPILE.json').write_text(json.dumps(compiles,indent=2)+'\n')
        print('Compiling selected source:',unit['relative'],flush=True)
        subprocess.run(conf+['--',*compile_cmd],env=env,stdin=subprocess.DEVNULL,check=True)
    lines=(cached/'obj/electron/electron_app.ninja').read_text().splitlines()
    line=next(line for line in lines if line.startswith('build ./electron: link '))
    inputs=shlex.split(line.split(': link ',1)[1].split(' | ',1)[0].split(' || ',1)[0])
    for unit in units:
        if inputs.count(unit['target'])!=1:raise RuntimeError('Selected object must be one explicit link input: '+unit['target'])
    inventory={name:dict(size=(cached/name).stat().st_size,mtime_ns=(cached/name).stat().st_mtime_ns) for name in inputs}
    rsp=view/'electron.rsp';rsp.write_text('\n'.join('"'+name.replace('\\','\\\\').replace('"','\\"')+'"' for name in inputs)+'\n')
    link_original=command('electron');link=link_original.copy()
    link_conf=conf.copy()
    selected_objects={}
    for unit in units:
        link_conf+=['--ro-bind',str(unit['output']),str(cached/unit['target'])]
        selected_objects[unit['target']]=dict(path=str(unit['output']),sha256=sha(unit['output']))
    (out/'LINK.json').write_text(json.dumps(dict(original=link_original,selected=link,inputs=inventory,response_sha256=sha(rsp),selected_objects=selected_objects,confinement=link_conf),indent=2)+'\n')
    print('Relinking cached Electron inputs with selected objects',flush=True)
    subprocess.run(link_conf+['--',*link],env=env,stdin=subprocess.DEVNULL,check=True)
    strip=electron/'checkout/src/third_party/llvm-build/Release+Asserts/bin/llvm-strip'
    subprocess.run(conf+['--',str(strip),'--strip-all','-o',str(out/'electron'),str(view/'electron')],env=env,stdin=subprocess.DEVNULL,check=True)
    for name,expected in baseline.items():
        if sha(Path(name))!=expected:raise RuntimeError('A preserved input changed: '+name)
    if (out/'electron').stat().st_size>256*1024*1024:raise RuntimeError('Runtime exceeds the existing image bound')
    record=dict(state='built; guest runtime validation pending',binary=str(out/'electron'),binary_sha256=sha(out/'electron'),unstripped=str(view/'electron'),unstripped_sha256=sha(view/'electron'),sources={str(unit['source']):sha(unit['source']) for unit in units},original_inputs=baseline,config=str(config_path),config_sha256=sha(config_path),compile_commands=compiles,link_command=link,confinement=link_conf,host_editor_execution=False,scope='Selected source compilations; original linker command in a private reflinked build view; input objects/headers/libraries read-only; original source/object/runtime unchanged',link_inventory='Explicit input paths/sizes/mtimes captured; inherited cached compilation provenance retained separately')
    (out/'BUILD.json').write_text(json.dumps(record,indent=2)+'\n')
    print('Runtime ready:',out/'electron',record['binary_sha256'],flush=True)


if __name__=='__main__':main()
