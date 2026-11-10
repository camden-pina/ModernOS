#!/usr/bin/env python3
"""Opt-in Code OSS graphical model/launcher build using existing native JSC.
No downloads, engine/kernel rebuilds, host installs or ISO builds. The desktop
frontend is built by the normal bin/gui target. See CODEOSS-GRAPHICAL.md.
"""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--source-root',type=Path)
p.add_argument('--build-root',type=Path);p.add_argument('--output-root',type=Path);p.add_argument('--runtime-root',type=Path);p.add_argument('--esbuild',type=Path)
a=p.parse_args();R=a.root.resolve();S=(a.source_root or R).resolve();B=(a.build_root or R/'build/codeoss-graphical').resolve();O=(a.output_root or R/'userland/bin').resolve();runtime=(a.runtime_root or R/'userland/runtime').resolve();B.mkdir(parents=True,exist_ok=True)
def sha(f):
 with f.open('rb') as h:return hashlib.file_digest(h,'sha256').hexdigest()
engine=Path(os.environ.get('COAST_JSC_BUILD',str(R/'build/modern-browser'))).resolve();prefix=engine/'prefix';jsc=engine/'jsc';cc=engine/'musl-posix-g++';c=R/'build/compiler/cross/bin/x86_64-linux-musl-gcc'
libs=[prefix/'lib64/libgcc.a',jsc/'lib/libJavaScriptCore.a',jsc/'lib/libWTF.a',prefix/'lib/libicudata.a',prefix/'lib/libicui18n.a',prefix/'lib/libicuuc.a',jsc/'lib/libbmalloc.a',prefix/'lib64/libgcc.a',prefix/'lib/libicudata.a']
for f in [cc,c,*libs]:
 if not f.is_file():raise SystemExit('Existing local JSC/musl build required; nothing is downloaded or rebuilt: '+str(f))
bundle=S/'ports/codeoss/graphical-model.js';commands=[]
if a.esbuild:
 esbuild=a.esbuild.resolve();assert subprocess.check_output([str(esbuild),'--version'],text=True).strip()=='0.28.2','Use the pinned esbuild 0.28.2'
 bundle=B/'graphical-model.js';cmd=[str(esbuild),str(S/'ports/codeoss/graphical-model.ts'),'--bundle','--format=iife','--platform=neutral','--target=es2022','--legal-comments=inline','--outfile='+str(bundle),'--metafile='+str(B/'bundle.json')];subprocess.run(cmd,check=True);commands.append(cmd)
else:
 manifest=json.loads((S/'ports/codeoss/GRAPHICAL-SOURCES.json').read_text())
 for rel,expected in manifest['bundle_inputs'].items():assert sha(S/rel)==expected,'Graphical source/bundle changed; regenerate with --esbuild: '+rel
 assert sha(bundle)==manifest['bundle_sha256'],'Generated bundle changed; regenerate with --esbuild'
flags=['-std=c++20','-O2','-g','-Wall','-Wextra','-Werror','-fno-pie','-fno-stack-protector','-mno-avx','-mno-avx2','-ffunction-sections','-fdata-sections','-I'+str(jsc/'JavaScriptCore/Headers')];objects=[]
for name in ['model-worker','file-service']:
 obj=B/(name+'.o');cmd=[str(cc),*flags,'-c',str(S/'userland/codeoss'/(name+'.cpp')),'-o',str(obj)];subprocess.run(cmd,check=True);objects.append(obj);commands.append(cmd)
worker=B/'codeoss-model';cmd=[str(cc),'-static','-no-pie','-Wl,--gc-sections',*map(str,objects),'-ldl',*map(str,libs),'-ldl','-o',str(worker)];subprocess.run(cmd,check=True);commands.append(cmd)
launcher=B/'codeoss-launch';cmd=[str(c),'-O2','-std=c11','-Wall','-Wextra','-Werror','-fno-pie','-fno-stack-protector','-static','-no-pie','-Wl,-z,max-page-size=4096,-z,noexecstack',str(S/'userland/codeoss/launcher.c'),'-o',str(launcher)];subprocess.run(cmd,check=True);commands.append(cmd)
O.mkdir(parents=True,exist_ok=True)
for f in [worker,launcher]:
 if f.resolve()!=(O/f.name).resolve():shutil.copy2(f,O/f.name)
resources=runtime/'usr/share/codeoss';resources.mkdir(parents=True,exist_ok=True)
shutil.copy2(bundle,resources/'graphical-model.js')
for name in ['graphical-check.js','graphical-shared-check.js','LICENSE.txt','PORT-STATUS.txt','GRAPHICAL-SOURCES.json']:shutil.copy2(S/'ports/codeoss'/name,resources/name)
relink=runtime/'usr/lib/codeoss/relink-model';relink.mkdir(parents=True,exist_ok=True)
for f in objects:shutil.copy2(f,relink/f.name)
(B/'build-commands.json').write_text(json.dumps(commands,indent=2)+'\n');shutil.copy2(B/'build-commands.json',relink/'build-commands.json')
record={'state':'built; guest validation separate','upstream_commit':'211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb','worker_sha256':sha(worker),'launcher_sha256':sha(launcher),'bundle_sha256':sha(bundle),'engine_rebuilt':False,'kernel_rebuilt':False,'engine_archives':list(map(str,libs)),'relink_objects':list(map(str,objects)),'graphical_text_limit':262144,'sources':{rel:sha(S/rel) for rel in ['userland/codeoss/model-worker.cpp','userland/codeoss/launcher.c','userland/codeoss/file-service.cpp','userland/codeoss/file-service.h','userland/include/codeoss_protocol.h','ports/codeoss/graphical-model.ts','ports/codeoss/native-model.ts']}}
(B/'build.json').write_text(json.dumps(record,indent=2)+'\n');shutil.copy2(B/'build.json',resources/'graphical-build.json');print(json.dumps(record,indent=2))
