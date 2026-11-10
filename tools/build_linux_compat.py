#!/usr/bin/env python3
"""Build selected unmodified upstream releases for the Linux x86-64 musl ABI.

The sources are official signed GNU releases / kernel.org checksum-pinned
util-linux. No Coast source patch or host installation is performed.
"""
from source_archive import unpack
from build_state import project_lock
from build_state import atomic_write
import argparse,hashlib,json,os,shutil,subprocess,tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/'build/linux-compat'
CROSS=ROOT/'build/compiler/cross/bin/x86_64-linux-musl-'
PACKAGES={
 'coreutils-9.7':{'archive':'coreutils-9.7.tar.xz','url':'https://ftp.gnu.org/gnu/coreutils/coreutils-9.7.tar.xz','targets':['src/cp','src/ln','src/stat','src/timeout','src/mkfifo','src/mkdir','src/chmod','src/readlink','src/sleep'],'options':['--disable-acl','--disable-xattr','--without-selinux','--disable-libcap','--without-libgmp']},
 'tar-1.35':{'archive':'tar-1.35.tar.xz','url':'https://ftp.gnu.org/gnu/tar/tar-1.35.tar.xz','targets':['src/tar'],'options':['--without-posix-acls','--without-xattrs','--without-selinux']},
 'util-linux-2.41.1':{'archive':'util-linux-2.41.1.tar.xz','url':'https://www.kernel.org/pub/linux/utils/util-linux/v2.41/util-linux-2.41.1.tar.xz','targets':['flock'],'options':['--disable-shared','--enable-static','--without-systemd','--without-udev','--without-cap-ng','--disable-libmount','--disable-libblkid','--disable-libsmartcols','--disable-libfdisk','--disable-libuuid','--disable-liblastlog2','--disable-asciidoc','--disable-poman']}}

def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def run(where,name,command,env):
 print(name,flush=True)
 with (BUILD/(name+'.log')).open('wb') as out:r=subprocess.run(command,cwd=where,env=env,stdout=out,stderr=subprocess.STDOUT,stdin=subprocess.DEVNULL)
 if r.returncode:raise SystemExit(f'{name} failed: {BUILD/(name+".log")}')

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--jobs',type=int,default=1);p.add_argument('--package',choices=PACKAGES);a=p.parse_args();assert a.jobs==1,'The bounded milestone uses one build job.'
 if not Path(str(CROSS)+'gcc').is_file():raise SystemExit('Build the pinned cross/native compiler first with python3 tools/build_gcc.py; this diagnostic corpus does not bootstrap or install host compilers.')
 provenance=json.loads((ROOT/'userland/vendor/linux-compat-sources.json').read_text());by_name={r['file']:r for r in provenance['files']};BUILD.mkdir(parents=True,exist_ok=True)
 uapi=json.loads((ROOT/'userland/vendor/linux-uapi-provenance.json').read_text())
 uapi_archive=ROOT/'userland/vendor'/uapi['archive']['path']
 assert digest(uapi_archive)==uapi['archive']['sha256'],'UAPI archive checksum mismatch'
 if not (BUILD/'linux-uapi/linux/fs.h').exists():
  unpack(uapi_archive, BUILD, filter='data')
 for item in uapi['files']:
  assert digest(BUILD/'linux-uapi'/item['path'])==item['sha256'],item['path']
 env=os.environ.copy()
 for name in ('CC','CXX','CFLAGS','CPPFLAGS','LDFLAGS','LIBS','CONFIG_SITE','MAKEFLAGS','MFLAGS','MAKEOVERRIDES','PKG_CONFIG_PATH'):env.pop(name,None)
 env.update(CC=str(CROSS)+'gcc',AR=str(CROSS)+'ar',RANLIB=str(CROSS)+'ranlib',CFLAGS='-O2 -g0 -fno-pie -fno-stack-protector -mno-avx -mno-avx2',LDFLAGS='-static -no-pie -Wl,-z,max-page-size=4096,-z,noexecstack',PKG_CONFIG_LIBDIR='/nonexistent')
 records=[]
 for name,config in PACKAGES.items():
  if a.package and a.package!=name:continue
  archive=ROOT/'userland/vendor'/config['archive'];assert digest(archive)==by_name[config['archive']]['sha256']
  source=BUILD/name
  if not source.exists():
   unpack(archive, BUILD, filter='data')
  env['CPPFLAGS']='-isystem '+str(BUILD/'linux-uapi')
  options=['./configure','--prefix=/usr','--build='+subprocess.check_output(['gcc','-dumpmachine'],text=True).strip(),'--host=x86_64-linux-musl','--disable-nls',*config['options']]
  key=hashlib.sha256(json.dumps([by_name[config['archive']]['sha256'],uapi['archive']['sha256'],options,{k:env[k] for k in ('CC','CFLAGS','CPPFLAGS','LDFLAGS')}]).encode()).hexdigest();stamp=source/'.coast-linux-configured'
  if not stamp.exists():run(source,name+'-configure',options,env);stamp.write_text(key)
  if stamp.read_text()!=key:
   shutil.copy2(source/'config.log',BUILD/(name+'-previous-config.log'))
   run(source,name+'-configure-corrected',options,env);stamp.write_text(key)
  if name.startswith('coreutils'):
   generated=BUILD/'generated-headers.mk'
   generated.write_text('.PHONY: coast-generated-headers\ncoast-generated-headers: $(BUILT_SOURCES)\n')
   run(source,name+'-generated-headers',['make','-j1','-f','Makefile','-f',str(generated),'coast-generated-headers'],env)
  run(source,name+'-build',['make','-j1','all'] if name.startswith('tar') else ['make','-j1',*config['targets']],env)
  category='coreutils' if name.startswith('coreutils') else 'tar' if name.startswith('tar') else 'util-linux';dest=ROOT/'userland/runtime/usr/lib/coast/linux-compat'/category;dest.mkdir(parents=True,exist_ok=True)
  notices=ROOT/'userland/vendor/licenses/linux-compat'/name;notices.mkdir(parents=True,exist_ok=True)
  for f in source.glob('COPYING*'):
   if f.is_file():shutil.copy2(f,notices/f.name)
  if (source/'Documentation/licenses').exists():shutil.copytree(source/'Documentation/licenses',notices/'licenses',dirs_exist_ok=True)
  for target in config['targets']:
   binary=source/target;output=dest/binary.name;shutil.copy2(binary,output)
   elf=subprocess.check_output(['readelf','-h','-l',str(output)],text=True);assert 'Advanced Micro Devices X86-64' in elf and 'INTERP' not in elf
   records.append({'program':binary.name,'upstream':name,'path':str(output.relative_to(ROOT/'userland/runtime')),'sha256':digest(output),'linkage':'static Linux x86-64 musl 1.2.5','source_modified':False,'source_archive_sha256':digest(archive),'configure':options,'cflags':env['CFLAGS'],'cppflags':env['CPPFLAGS'],'uapi_headers':json.loads((ROOT/'userland/vendor/linux-uapi-provenance.json').read_text())['package'],'ldflags':env['LDFLAGS']})
 fixtures=[]
 for name in ('abi-audit','semantic-check'):
  source=ROOT/'userland/linux-compat'/(name+'.c');output=ROOT/'userland/runtime/usr/lib/coast/linux-compat'/name
  fixture_key=hashlib.sha256(source.read_bytes()+env['CFLAGS'].encode()+uapi['archive']['sha256'].encode()).hexdigest()
  stamp=BUILD/(name+'.stamp')
  if not output.exists() or not stamp.exists() or stamp.read_text()!=fixture_key:
   run(ROOT,name+'-build',[env['CC'],*env['CFLAGS'].split(),'-isystem',str(BUILD/'linux-uapi'),'-pthread','-static','-no-pie',str(source),'-o',str(output)],env);stamp.write_text(fixture_key)
  fixtures.append({'name':name,'in_tree_semantic_fixture':True,'source_sha256':digest(source),'elf_sha256':digest(output)})
 manifest=BUILD/('manifest-'+(a.package or 'all')+'.json');atomic_write(manifest, json.dumps({'programs':records,'fixtures':fixtures,'provenance':provenance,'uapi':uapi},indent=2)+'\n');shutil.copy2(manifest,ROOT/'userland/runtime/usr/lib/coast/linux-compat'/manifest.name)
 print('Linux-targeted upstream tools built; manifest='+str(manifest),flush=True)
if __name__ == '__main__':
    with project_lock():
        main()
