#!/usr/bin/env python3
"""Build bounded official CLI ports with the existing stock Linux/musl toolchain.
No host install, source downloads, native-command replacement or kernel stubs.
"""
from source_archive import unpack, unpack_zip
from build_state import project_lock
from build_state import atomic_write
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess,tarfile,zipfile,datetime,fcntl,time
ROOT=Path(__file__).resolve().parents[1];BUILD=ROOT/'build/linux-ports';VENDOR=ROOT/'userland/vendor/linux-ports';PREFIX=ROOT/'userland/runtime/usr/lib/coast/linux-ports';CROSS=ROOT/'build/compiler/cross/bin/x86_64-linux-musl-'
FLAGS=['-O2','-g0','-fno-pie','-fno-stack-protector','-mno-avx','-mno-avx2'];LINK=['-static','-no-pie','-Wl,-z,max-page-size=4096,-z,noexecstack']
def digest(p):
 with p.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def run(where,label,command,env):
 print('BUILD '+label,flush=True)
 log=BUILD/(label+'.log')
 if log.exists():log.rename(BUILD/(label+'-previous-'+str(time.time_ns())+'.log'))
 with log.open('wb') as out:subprocess.run(list(map(str,command)),cwd=where,env=env,stdout=out,stderr=subprocess.STDOUT,check=True)
def no_guests():
 assert not (ROOT/'build/browser-guests-active').exists(),'Guest marker blocks compiler'
 for p in Path('/proc').iterdir():
  if not p.name.isdecimal():continue
  try:args=(p/'cmdline').read_bytes().split(b'\0')
  except OSError:continue
  assert not (args and Path(args[0].decode(errors='replace')).name.startswith('qemu-system')),'A QEMU is active'
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--package',choices=['bash','grep','sed','diffutils','lua','sqlite']);parser.add_argument('--checks-only',action='store_true');parser.add_argument('--jobs',type=int,default=1);args=parser.parse_args();assert args.jobs==1
 no_guests();assert Path(str(CROSS)+'gcc').is_file(),'Existing pinned cross compiler required'
 manifest=json.loads((ROOT/'userland/vendor/linux-ports-sources.json').read_text());assert manifest['result']=='passed'
 BUILD.mkdir(parents=True,exist_ok=True);PREFIX.mkdir(parents=True,exist_ok=True)
 lock=(BUILD/'build.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
 env=os.environ.copy()
 for key in ('CC','CXX','AR','RANLIB','CFLAGS','CPPFLAGS','LDFLAGS','LIBS','CONFIG_SITE','MAKEFLAGS','MFLAGS','MAKEOVERRIDES','PKG_CONFIG_PATH'):env.pop(key,None)
 env.update(CC_FOR_BUILD='gcc -std=gnu11',CFLAGS_FOR_BUILD='-O2 -std=gnu11',CC=str(CROSS)+'gcc',AR=str(CROSS)+'ar',RANLIB=str(CROSS)+'ranlib',CFLAGS=' '.join(FLAGS),CPPFLAGS='-isystem '+str(ROOT/'build/linux-compat/linux-uapi'),LDFLAGS=' '.join(LINK),PKG_CONFIG_LIBDIR='/nonexistent')
 (BUILD/'generated.mk').write_text('.PHONY: coast-generated\ncoast-generated: $(BUILT_SOURCES)\n')
 existing=json.loads((BUILD/'manifest.json').read_text()) if (BUILD/'manifest.json').exists() else {'packages':[]};records={p['name']:p for p in existing['packages']}
 for package in manifest['packages']:
  name=package['name']
  if args.checks_only or (args.package and args.package!=name):continue
  no_guests();archive=VENDOR/package['archive'];assert digest(archive)==package['sha256']
  version=package['version'];directory='sqlite-amalgamation-'+package['archive'].split('-')[-1].split('.')[0] if name=='sqlite' else name+'-'+version;source=BUILD/directory
  if not source.exists():
   if archive.suffix=='.zip':
    unpack_zip(archive, BUILD)
   else:
    unpack(archive, BUILD, filter='data')
  assert source.is_dir(),source
  for patch in package.get('patches',[]):
   path=VENDOR/patch['file'];assert digest(path)==patch['sha256'];stamp=source/('.applied-'+patch['file'])
   if not stamp.exists():
    run(source,patch['file'],['patch','--batch','-p0','-i',path],env);stamp.write_text(patch['sha256']+'\n')
   else:assert stamp.read_text().strip()==patch['sha256']
  programs=[];libraries=[];options=[]
  if name in ('bash','grep','sed','diffutils'):
   options=['./configure','--prefix=/usr/lib/coast/linux-ports/'+name,'--build='+subprocess.check_output(['gcc','-dumpmachine'],text=True).strip(),'--host=x86_64-linux-musl','--disable-nls']
   options+= {'bash':['--without-bash-malloc','--disable-readline','--enable-static-link'],'grep':['--disable-perl-regexp'],'sed':['--disable-acl','--without-selinux'],'diffutils':[]}[name]
   # diffutils3.12's generated probe lacks a usable cross branch. Validate
   # actual pinned target-library code on the Linux host before caching it.
   if name=='diffutils':
    probe=ROOT/'userland/linux-ports/configure-strcasecmp-check.c';binary=BUILD/'configure-strcasecmp-check'
    run(ROOT,'configure-strcasecmp-build',[env['CC'],*FLAGS,*LINK,probe,'-o',binary],env)
    run(ROOT,'configure-strcasecmp-target',[binary],env)
    env['gl_cv_func_strcasecmp_works']='yes'
   key=hashlib.sha256(json.dumps([package,options,env['CFLAGS'],env['CPPFLAGS'],env['LDFLAGS'],env['CC_FOR_BUILD'],env['CFLAGS_FOR_BUILD'],env.get('gl_cv_func_strcasecmp_works')]).encode()).hexdigest();stamp=source/'.coast-configured'
   if not stamp.exists() or stamp.read_text()!=key:run(source,name+'-configure',options,env);stamp.write_text(key)
   if name!='bash':run(source,name+'-generated',['make','-j1','-f','Makefile','-f',str(BUILD/'generated.mk'),'coast-generated'],env)
   targets={'bash':['bash'],'grep':['src/grep'],'sed':['sed/sed'],'diffutils':['src/diff','src/cmp','src/diff3','src/sdiff']}[name]
   run(source,name+'-build',['make','-j1',*(['bash'] if name=='bash' else ['all'])],env);programs=[source/p for p in targets]
  elif name=='lua':
   # The packaged native GNU ld2.37 does not understand newer relaxable
   # PUSH-GOTPCRELX objects. Plain GOTPCREL preserves the instruction semantics.
   picflags=' -fPIC -Wa,-mrelax-relocations=no '
   stamp=source/'.coast-pic-profile';key=hashlib.sha256(picflags.encode()).hexdigest()
   if not stamp.exists() or stamp.read_text()!=key:
    run(source/'src','lua-clean',['make','-j1','clean'],env);stamp.write_text(key)
   options=['PIC SDK archive','assembler relax-relocations=no for native GNU ld2.37']
   run(source/'src','lua-build',['make','-j1','linux','CC='+env['CC'],'AR='+env['AR']+' rcu','RANLIB='+env['RANLIB'],'MYCFLAGS='+env['CFLAGS']+picflags+env['CPPFLAGS'],'MYLDFLAGS='+env['LDFLAGS']],env)
   programs=[source/'src/lua',source/'src/luac'];libraries=[(source/'src/liblua.a','liblua.a')]
  else:
   defines=['-DSQLITE_THREADSAFE=1','-DSQLITE_ENABLE_FTS5','-DSQLITE_ENABLE_MATH_FUNCTIONS'];options=defines+['-fPIC','-Wa,-mrelax-relocations=no']
   run(source,'sqlite-library',[env['CC'],*FLAGS,'-fPIC','-Wa,-mrelax-relocations=no',*defines,'-I'+str(source),'-c',source/'sqlite3.c','-o',source/'sqlite3.o'],env)
   run(source,'sqlite-archive',[env['AR'],'rcs',source/'libsqlite3.a',source/'sqlite3.o'],env)
   run(source,'sqlite-shell',[env['CC'],*FLAGS,*defines,'-I'+str(source),source/'shell.c',source/'libsqlite3.a',*LINK,'-pthread','-lm','-ldl','-o',source/'sqlite3'],env)
   programs=[source/'sqlite3'];libraries=[(source/'libsqlite3.a','libsqlite3.a')]
  dest=PREFIX/name;dest.mkdir(exist_ok=True);installed=[]
  for binary in programs:
   target=dest/binary.name;shutil.copy2(binary,target);elf=subprocess.check_output(['readelf','-h','-l',str(target)],text=True);assert 'Advanced Micro Devices X86-64' in elf and 'INTERP' not in elf and ' RWE ' not in elf
   alias=ROOT/'userland/runtime/usr/bin'/('linux-'+binary.name);relative='/usr/lib/coast/linux-ports/'+name+'/'+binary.name
   if alias.is_symlink():assert os.readlink(alias)==relative
   else:assert not alias.exists();alias.symlink_to(relative)
   installed.append({'program':binary.name,'path':str(target.relative_to(ROOT/'userland/runtime')),'sha256':digest(target),'alias':str(alias.relative_to(ROOT/'userland/runtime'))})
  sdk=PREFIX/'sdk';(sdk/'lib').mkdir(parents=True,exist_ok=True);(sdk/'include').mkdir(exist_ok=True)
  librecords=[]
  for binary,filename in libraries:
   target=sdk/'lib'/filename;shutil.copy2(binary,target);librecords.append({'path':str(target.relative_to(ROOT/'userland/runtime')),'sha256':digest(target),'build':'PIC static library, Linux/musl1.2.5 C ABI'})
  if name=='lua':
   include=sdk/'include/lua5.5';include.mkdir(exist_ok=True)
   for header in ('lua.h','luaconf.h','lualib.h','lauxlib.h','lua.hpp'):shutil.copy2(source/'src'/header,include/header)
  if name=='sqlite':
   for header in ('sqlite3.h','sqlite3ext.h'):shutil.copy2(source/header,sdk/'include'/header)
  notices=ROOT/'userland/runtime/usr/share/licenses/linux-ports'/name;notices.mkdir(parents=True,exist_ok=True)
  licenses=[p for p in source.glob('COPYING*') if p.is_file()]
  if name=='lua':licenses=[source/'doc/readme.html']
  if name=='sqlite':licenses=[source/'sqlite3.h']
  for path in licenses:shutil.copy2(path,notices/path.name)
  corresponding=ROOT/'userland/runtime/usr/src/coast/linux-ports';corresponding.mkdir(parents=True,exist_ok=True);shutil.copy2(archive,corresponding/archive.name)
  for patch in package.get('patches',[]):shutil.copy2(VENDOR/patch['file'],corresponding/patch['file'])
  records[name]={'name':name,'version':version,'official_patch_count':len(package.get('patches',[])),'source_modified':bool(package.get('patches')),'coast_source_patches':False,'source':package,'linkage':'rebuilt static x86-64 Linux/musl1.2.5, not a distribution binary','options':options,'compiler':env['CC'],'cflags':FLAGS,'ldflags':LINK,'programs':installed,'libraries':librecords}
  result={'recorded_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'result':'built-not-yet-guest-validated','packages':list(records.values()),'host_installations':False};atomic_write(BUILD/'manifest.json', json.dumps(result,indent=2)+'\n');shutil.copy2(BUILD/'manifest.json',PREFIX/'manifest.json')
 sdk=PREFIX/'sdk'
 share=ROOT/'userland/runtime/usr/share/coast/linux-ports';share.mkdir(parents=True,exist_ok=True)
 examples=ROOT/'userland/runtime/usr/src/coast/linux-ports/examples';examples.mkdir(parents=True,exist_ok=True)
 for filename in ('workflows.sh','memory.sql','sqlite-writer.sql','sqlite-concurrency.sh','wait-release.sh','persistent-create.sql','persistent-export.sql'):shutil.copy2(ROOT/'userland/linux-ports'/filename,share/filename)
 for filename in ('library-check.c','configure-strcasecmp-check.c','record-lock-check.c','compat-diagnostics.c','sqlite-concurrency-check.c'):shutil.copy2(ROOT/'userland/linux-ports'/filename,examples/filename)
 if {'lua','sqlite'} <= records.keys():
  no_guests()
  run(ROOT,'record-lock-check-build',[env['CC'],'-O2','-g','-std=gnu11','-Wall','-Wextra','-Werror',*LINK,'-pthread',ROOT/'userland/linux-ports/record-lock-check.c','-o',PREFIX/'record-lock-check'],env)
  run(ROOT,'compat-diagnostics-build',[env['CC'],*FLAGS,*LINK,'-I'+str(sdk/'include'),ROOT/'userland/linux-ports/compat-diagnostics.c','-L'+str(sdk/'lib'),'-lsqlite3','-lm','-ldl','-pthread','-o',PREFIX/'compat-diagnostics'],env)
  run(ROOT,'sqlite-concurrency-check-build',[env['CC'],*FLAGS,*LINK,'-I'+str(sdk/'include'),ROOT/'userland/linux-ports/sqlite-concurrency-check.c','-L'+str(sdk/'lib'),'-lsqlite3','-lm','-ldl','-pthread','-o',PREFIX/'sqlite-concurrency-check'],env)
  run(ROOT,'sandbox-record-check-build',[str(ROOT/'build/native-musl/bin/musl-gcc'),'-O2','-std=gnu11','-Wall','-Wextra','-Werror','-fno-pie','-fno-stack-protector','-mno-avx','-mno-avx2','-pthread',*LINK,ROOT/'userland/web-capability-check.c','-o',PREFIX/'sandbox-record-check'],env)
  fixture={'result':'built-not-yet-guest-validated','fixtures':[{'path':str(p.relative_to(ROOT/'userland/runtime')),'sha256':digest(p)} for p in (PREFIX/'record-lock-check',PREFIX/'compat-diagnostics',PREFIX/'sqlite-concurrency-check',PREFIX/'sandbox-record-check')],'sources':['record-lock-check.c','compat-diagnostics.c','sqlite-concurrency-check.c']}
  atomic_write(BUILD/'checks-manifest.json', json.dumps(fixture,indent=2)+'\n');shutil.copy2(BUILD/'checks-manifest.json',PREFIX/'checks-manifest.json')
 print('BUILT '+', '.join(records),flush=True)
if __name__ == '__main__':
    with project_lock():
        main()
