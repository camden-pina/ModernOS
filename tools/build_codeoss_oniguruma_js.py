#!/usr/bin/env python3
"""Translate the pinned Oniguruma WASM into ordinary JS for jitless Code OSS.

Uses a verified Binaryen host tool only inside the existing read-only,
network-unshared build confinement. Does not execute any guest executable.
Changes one packed ASAR module; all other file contents stay byte-identical.
"""
from build_state import project_lock
from build_state import atomic_write
from pathlib import Path
import argparse,copy,hashlib,json,struct,subprocess,tarfile

ROOT=Path(__file__).resolve().parents[1]
BUNDLE_SHA='61223b74f9e40834117311ec4177cb386d09b84c1aae4f9e674c96580569cef3'
ASAR_SHA='94ffe626e582947f9f0528b1d6553632127a350b867a6d7e0304eb245bf270dc'
WASM_SHA='fd885c2d12e5951e59d761ebd4a006e06254b1491fd6f530c92b69fb4d8d77d9'
TOOL_SHA='52597efec0ad2416f8b5c245864960e775b8ffc94c751cc0b016c4b4986d3bcb'
TARGET='vscode-oniguruma/release/main.js'
def sha(data):return hashlib.sha256(data).hexdigest()
def file_sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def parse_asar(data):
    first,size,payload,strlen=struct.unpack_from('<IIII',data)
    if first!=4 or size!=payload+4 or strlen>payload-4:raise ValueError('Invalid ASAR pickle')
    return json.loads(data[16:16+strlen]),8+size
def entries(header,prefix=''):
    for name,item in header['files'].items():
        path=prefix+name
        if 'files' in item:yield from entries(item,path+'/')
        elif 'offset' in item and not item.get('unpacked'):yield path,item
def repack(original,replacement):
    original_header,start=parse_asar(original);header=copy.deepcopy(original_header);parts=[];offset=0;changed=0
    for path,item in sorted(entries(header),key=lambda row:int(row[1]['offset'])):
        size=item['size'];old_offset=int(item['offset']);data=original[start+old_offset:start+old_offset+size]
        if len(data)!=size:raise ValueError('Truncated ASAR file: '+path)
        if path==TARGET:
            data=replacement;changed+=1;item['size']=len(data)
            if 'integrity' in item:
                integrity=item['integrity'];block=integrity['blockSize']
                if integrity['algorithm']!='SHA256' or not isinstance(block,int) or block<=0:raise ValueError('Unknown ASAR integrity')
                integrity['hash']=sha(data);integrity['blocks']=[sha(data[i:i+block]) for i in range(0,len(data),block)]
        item['offset']=str(offset);parts.append(data);offset+=len(data)
    if changed!=1:raise ValueError('Expected exactly one Oniguruma module')
    text=json.dumps(header,ensure_ascii=False,separators=(',',':')).encode();padded=text+b'\0'*(-len(text)%4)
    result=struct.pack('<IIII',4,len(padded)+8,len(padded)+4,len(text))+padded+b''.join(parts)
    checked,after=parse_asar(result);before_entries=dict(entries(original_header))
    for path,item in entries(checked):
        actual=result[after+int(item['offset']):after+int(item['offset'])+item['size']]
        old=before_entries[path];expected=replacement if path==TARGET else original[start+int(old['offset']):start+int(old['offset'])+old['size']]
        if actual!=expected:raise ValueError('ASAR round-trip mismatch: '+path)
    return result,len(before_entries)
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifacts',type=Path,required=True)
    parser.add_argument('--binaryen-vendor',type=Path,required=True)
    parser.add_argument('--output-root',type=Path,required=True)
    args=parser.parse_args();artifacts=args.artifacts.resolve();vendor=args.binaryen_vendor.resolve();out=args.output_root.resolve()
    if out.exists():parser.error('An unused output directory is required')
    bundle=artifacts.parent/'codeoss-app-a0/vendor/VSCodium-linux-x64-1.140.06880.tar.gz'
    if file_sha(bundle)!=BUNDLE_SHA:parser.error('Workbench archive provenance changed')
    tool=vendor/'extracted/binaryen-version_133/bin/wasm2js';provenance=json.loads((vendor/'SOURCE.json').read_text());license_path=vendor/'LICENSE'
    if file_sha(tool)!=TOOL_SHA or file_sha(license_path)!=provenance['license_sha256']:parser.error('Binaryen tool/license provenance changed')
    out.mkdir(parents=True)
    with tarfile.open(bundle) as archive:
        members={entry.name.removeprefix('./'):entry for entry in archive}
        original=archive.extractfile(members['resources/app/node_modules.asar']).read()
        wasm=archive.extractfile(members['resources/app/node_modules.asar.unpacked/vscode-oniguruma/release/onig.wasm']).read()
    if sha(original)!=ASAR_SHA or sha(wasm)!=WASM_SHA:raise ValueError('Pinned ASAR/WASM differ')
    header,start=parse_asar(original);item=dict(entries(header))[TARGET]
    original_module=original[start+int(item['offset']):start+int(item['offset'])+item['size']].decode()
    (out/'onig.wasm').write_bytes(wasm)
    config=artifacts.parent/'codeoss-embedder-e2-build/config/APP-LINK-04.json';confinement=list(json.loads(config.read_text())['confinement'])
    for i in range(len(confinement)-2):
        if confinement[i]=='--bind':confinement[i]='--ro-bind'
    pos=next(i for i in range(len(confinement)-1) if confinement[i:i+2]==['--remount-ro','/'])
    confinement[pos:pos]=['--dir',str(artifacts),'--dir',str(out)]
    confinement+=['--ro-bind',str(artifacts),str(artifacts),'--bind',str(out),str(out),'--chdir',str(out)]
    if '--unshare-net' not in confinement or '--disable-userns' not in confinement:raise ValueError('Build confinement changed')
    command=[str(tool),str(out/'onig.wasm'),'--emscripten','-O2','-o',str(out/'onig-asm.js')]
    result=subprocess.run(confinement+['--']+command,capture_output=True,text=True)
    (out/'translator.log').write_text(result.stdout+result.stderr)
    if result.returncode:raise RuntimeError('Binaryen failed; see translator.log')
    if file_sha(out/'onig.wasm')!=WASM_SHA or file_sha(tool)!=TOOL_SHA:raise ValueError('Build inputs changed')
    assembly=(out/'onig-asm.js').read_text()
    if not assembly.startswith('function instantiate(info) {') or not assembly.rstrip().endswith('return asmFunc(info);\n}'):raise ValueError('Unexpected translator wrapper')
    loader_path=ROOT/'userland/electron-runtime/oniguruma-js-loader.js';loader=loader_path.read_text()
    if loader.count('@WASM_SHA256@')!=1:raise ValueError('Loader digest placeholder changed')
    loader=loader.replace('@WASM_SHA256@',WASM_SHA)
    prelude='// Coast: pinned Oniguruma translated ahead of time for the JS interpreter.\nconst coastOnigInstantiate = (() => {\n'+assembly+'\nreturn instantiate;\n})();\n'+loader+'\n'
    substitutions={
        'n.loadWASM=function(t){':"n.loadWASM=function(t){if(typeof WebAssembly==='undefined'&&!(t&&typeof t.instantiator==='function')){const input=t;t={print:t&&t.print,instantiator:imports=>coastOnigVerifiedInstance(input,imports)};}",
        '"object"!=typeof WebAssembly&&k("no native wasm support detected")':'"object"!=typeof WebAssembly&&!r.instantiateWasm&&k("no native wasm support detected")',
        'new WebAssembly.RuntimeError(t)':'("object"==typeof WebAssembly?new WebAssembly.RuntimeError(t):new Error(t))',
    }
    module=original_module
    for before,after in substitutions.items():
        if module.count(before)!=1:raise ValueError('Pinned Oniguruma loader changed')
        module=module.replace(before,after)
    module=(prelude+module).encode();(out/'coast-main.cjs').write_bytes(module)
    rebuilt,count=repack(original,module);(out/'node_modules.asar').write_bytes(rebuilt);(out/'Binaryen-LICENSE').write_bytes(license_path.read_bytes())
    record=dict(state='built; guest verification pending',bundle_sha256=BUNDLE_SHA,original_asar_sha256=ASAR_SHA,wasm_sha256=WASM_SHA,
        asar=str(out/'node_modules.asar'),asar_sha256=sha(rebuilt),module_sha256=sha(module),original_module_sha256=sha(original_module.encode()),
        translator_sha256=TOOL_SHA,translator_source=provenance,translator_command=command,confinement=confinement,
        loader_sha256=file_sha(loader_path),packed_files_verified=count,changed_packed_files=[TARGET],
        license=str(out/'Binaryen-LICENSE'),license_sha256=file_sha(license_path),host_guest_executable_execution=False)
    atomic_write(out/'BUILD.json', json.dumps(record,indent=2)+'\n')
    print('Built interpreter Oniguruma ASAR:',record['asar_sha256'],'verified packed files:',count)
if __name__ == '__main__':
    with project_lock():
        main()
