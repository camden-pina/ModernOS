from pathlib import Path
import json,hashlib,tarfile,subprocess,os,time,argparse
a=argparse.ArgumentParser(description='Build verified cached GNU Bash with bundled Readline; no downloads or global install.');a.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1]);a.add_argument('--build-root',type=Path,required=True);a.add_argument('--jobs',type=int,default=2);a.add_argument('--parent-patch',type=Path);args=a.parse_args();R=args.root.resolve();B=args.build_root.resolve();B.mkdir(parents=True,exist_ok=True);vendor=R/'userland/vendor/linux-ports';record=next(p for p in json.loads((R/'userland/vendor/linux-ports-sources.json').read_text())['packages'] if p['name']=='bash')
def digest(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
archive=vendor/record['archive'];assert digest(archive)==record['sha256'];src=B/'bash-5.3'
if not src.exists():
 with tarfile.open(archive) as t:t.extractall(B,filter='data')
for patch in record['patches']:
 p=vendor/patch['file'];assert digest(p)==patch['sha256'];stamp=B/('.'+patch['file'])
 if not stamp.exists():
  with (B/(patch['file']+'.log')).open('wb') as log:subprocess.run(['patch','--batch','-p0','-i',str(p)],cwd=src,stdout=log,stderr=subprocess.STDOUT,check=True)
  stamp.write_text(patch['sha256'])
parent_patch=args.parent_patch or R/'ports/bash/COAST-PARENT-TTY.patch'
assert parent_patch.is_file(),'Coast parent-controlled PTY patch required: '+str(parent_patch)
expected_jobs='''ea8914be42b734addddb3a72e1bd8e39a607e3422eeb7eb98ce9ad4ee7886a12'''
patch_hash=digest(parent_patch);patch_stamp=B/'.coast-parent-tty'
if patch_stamp.exists():
 assert json.loads(patch_stamp.read_text())['patch_sha256']==patch_hash,'Port patch changed; use a fresh build root'
else:
 assert digest(src/'jobs.c')==expected_jobs,'Unexpected GNU jobs.c after official patches'
 subprocess.run(['patch','--batch','-p1','-i',str(parent_patch.resolve())],cwd=src,check=True)
 patch_stamp.write_text(json.dumps({'patch_sha256':patch_hash,'base_jobs_sha256':expected_jobs}))
env=os.environ.copy()
for k in ['CC','CXX','AR','RANLIB','CFLAGS','CPPFLAGS','LDFLAGS','LIBS','CONFIG_SITE','MAKEFLAGS','MFLAGS','MAKEOVERRIDES','PKG_CONFIG_PATH']:env.pop(k,None)
cross=R/'build/compiler/cross/bin/x86_64-linux-musl-';ncurses=R/'build/vendor/ncurses-6.5'
env.update(CC=str(cross)+'gcc',AR=str(cross)+'ar',RANLIB=str(cross)+'ranlib',CFLAGS='-O2 -g0 -fno-pie -fno-stack-protector -mno-avx -mno-avx2 -DCOAST_PARENT_TTY=1',CPPFLAGS='-idirafter '+str(ncurses/'include')+' -isystem '+str(R/'build/linux-compat/linux-uapi'),LDFLAGS='-static -no-pie -Wl,-z,max-page-size=4096,-z,noexecstack -L'+str(ncurses/'lib'),LIBS='-lncursesw',CC_FOR_BUILD='gcc -std=gnu11',PKG_CONFIG_LIBDIR='/nonexistent')
configure=['./configure','--prefix=/usr/lib/coast/linux-ports/bash','--build='+subprocess.check_output(['gcc','-dumpmachine'],text=True).strip(),'--host=x86_64-linux-musl','--disable-nls','--without-bash-malloc','--enable-readline','--with-curses','--enable-static-link']
if not (B/'configured').exists():
 with (B/'configure.log').open('wb') as log:subprocess.run(configure,cwd=src,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
 (B/'configured').write_text(json.dumps(configure))
if not (B/'cleaned-header-order').exists():
 subprocess.run(['make','clean'],cwd=src,env=env,stdout=subprocess.DEVNULL,stderr=subprocess.STDOUT,check=True);(B/'cleaned-header-order').write_text('Bash config.h must precede ncurses empty include/config.h')
with (B/'build.log').open('wb') as log:subprocess.run(['make','-j'+str(args.jobs),'bash'],cwd=src,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
result={'state':'built, guest PTY verification pending','source':record,'coast_parent_tty_patch_sha256':patch_hash,'base_jobs_sha256':expected_jobs,'configure':configure,'environment':{k:env[k] for k in ['CC','AR','RANLIB','CFLAGS','CPPFLAGS','LDFLAGS','LIBS']},'binary':str(src/'bash'),'binary_sha256':digest(src/'bash'),'host_installations':False};(B/'build.json').write_text(json.dumps(result,indent=2)+'\n');print('Built GNU Bash5.3.20 with Readline and parent-controlled Coast PTY adaptation; guest verification is separate, runtime cache not replaced.',flush=True)
