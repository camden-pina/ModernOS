#!/usr/bin/env python3
"""Build the opt-in native Code OSS text-core host from existing JSC archives.
Does not download sources, build an engine, change host tooling or build an ISO.
"""
from pathlib import Path
import argparse,subprocess,json,hashlib,shutil,os
R=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,default=R/'userland/bin/codeoss-core');p.add_argument('--build-root',type=Path,default=R/'build/codeoss-native');a=p.parse_args();B=a.build_root.resolve();B.mkdir(parents=True,exist_ok=True);engine=Path(os.environ.get('COAST_JSC_BUILD',str(R/'build/modern-browser')));prefix=engine/'prefix';jsc=engine/'jsc';compiler=engine/'musl-posix-g++'
libs=[prefix/'lib64/libgcc.a',jsc/'lib/libJavaScriptCore.a',jsc/'lib/libWTF.a',prefix/'lib/libicudata.a',prefix/'lib/libicui18n.a',prefix/'lib/libicuuc.a',jsc/'lib/libbmalloc.a',prefix/'lib64/libgcc.a',prefix/'lib/libicudata.a']
for f in [compiler,*libs]:
 if not f.is_file():raise SystemExit('Existing standalone JSC build required, not rebuilt automatically: '+str(f))
flags=['-std=c++20','-O2','-g','-Wall','-Wextra','-Werror','-fno-pie','-fno-stack-protector','-mno-avx','-mno-avx2','-ffunction-sections','-fdata-sections','-I'+str(jsc/'JavaScriptCore/Headers')];objects=[];commands=[]
for name in ['runner','file-service']:
 obj=B/(name+'.o');cmd=[str(compiler),*flags,'-c',str(R/'userland/codeoss'/(name+'.cpp')),'-o',str(obj)];subprocess.run(cmd,check=True);objects.append(str(obj));commands.append(cmd)
a.output.parent.mkdir(parents=True,exist_ok=True);temporary=B/'codeoss-core.new';cmd=[str(compiler),'-static','-no-pie','-Wl,--gc-sections',*objects,'-ldl',*map(str,libs),'-ldl','-o',str(temporary)];subprocess.run(cmd,check=True);commands.append(cmd);shutil.copy2(temporary,a.output)
resource=R/'userland/runtime/usr/share/codeoss';resource.mkdir(parents=True,exist_ok=True)
for name in ['editor-cli.js','file-service-check.js','core-check-native.js','LICENSE.txt','PORT-STATUS.txt']:shutil.copy2(R/'ports/codeoss'/name,resource/name)
relink=R/'userland/runtime/usr/lib/codeoss/relink';relink.mkdir(parents=True,exist_ok=True)
for obj in objects:shutil.copy2(obj,relink/Path(obj).name)
(B/'build-commands.json').write_text(json.dumps(commands,indent=2)+'\n');shutil.copy2(B/'build-commands.json',relink/'build-commands.json')
record={'result':'built-not-yet-guest-validated','binary':str(a.output),'binary_sha256':hashlib.sha256(a.output.read_bytes()).hexdigest(),'engine_rebuilt':False,'compiler':str(compiler),'relink_objects':objects,'engine_archives':list(map(str,libs))};(B/'build.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record,indent=2))
