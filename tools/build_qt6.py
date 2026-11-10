#!/usr/bin/env python3
"""Build pinned Qt Core/Gui/Widgets and the Coast QPA port with static musl.

Host generators are built separately. Target executables never run on the host.
Compilation has no network and only the selected build directory is writable.
"""
from source_archive import unpack
from build_state import project_lock
from build_state import atomic_write
import argparse,hashlib,json,os,re,shutil,subprocess,tarfile,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
VERSION='6.8.3'
SOURCE_SHA256='56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80'
SOURCE_URL=f'https://download.qt.io/archive/qt/6.8/{VERSION}/submodules/qtbase-everywhere-src-{VERSION}.tar.xz'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output-root',type=Path,required=True);p.add_argument('--jobs',type=int,default=12);p.add_argument('--qt-only',action='store_true');p.add_argument('--install-runtime',action='store_true');p.add_argument('--sdk-build',type=Path,help='reuse a verified Qt SDK while compiling the QPA/app into a separate output');p.add_argument('--compiler-build',type=Path,help='source-built POSIX musl GCC receipt');p.add_argument('--reconfigure',action='store_true',help='accept builder-only fixes with unchanged inputs');a=p.parse_args()
 out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=True)
 compiler_receipt=None
 if a.compiler_build:
  from build_desktop import verify_receipt
  compiler_receipt=a.compiler_build.resolve();compiler=verify_receipt(compiler_receipt)
  if compiler.get('thread_model')!='posix' or compiler.get('target_host_executed') is not False:raise SystemExit('Qt requires the source-built POSIX-threaded musl compiler')
  cross=compiler_receipt.parent/'cross/bin/x86_64-linux-musl-';cxx=Path(str(cross)+'g++');prefix=compiler_receipt.parent/'cross/x86_64-linux-musl';sysroot=compiler_receipt.parent/'sysroot'
 else:
  cross=ROOT/'build/compiler/cross/bin/x86_64-linux-musl-';cxx=ROOT/'build/modern-browser/musl-posix-g++';prefix=ROOT/'build/modern-browser/prefix';sysroot=ROOT/'build/compiler/sysroot'
 compiler_inputs={str(path):sha(path) for path in [Path(str(cross)+'gcc'),cxx] if path.is_file()}
 for required in [cxx,Path(str(cross)+'gcc'),sysroot/'usr/lib/libc.a']:
  if not required.is_file():raise SystemExit('The existing Coast static musl/threaded C++ toolchain is required: '+str(required))
 recipe_inputs={str(path.relative_to(ROOT)):sha(path) for path in [Path(__file__),ROOT/'tools/linux_uapi.py',ROOT/'userland/vendor/linux-uapi-provenance.json']}
 identity=dict(compiler_receipt_sha256=sha(compiler_receipt) if compiler_receipt else None,compiler_inputs=compiler_inputs,source_sha256=recipe_inputs,sdk_build_sha256=sha(a.sdk_build.resolve()/'BUILD.json') if a.sdk_build else None)
 stamp=out/'CONFIGURATION.json'
 if stamp.exists() and json.loads(stamp.read_text())!=identity:
  previous=json.loads(stamp.read_text())
  def stable(record):return {**record,'source_sha256':{k:v for k,v in record['source_sha256'].items() if k!='tools/build_qt6.py'}}
  if not a.reconfigure or stable(previous)!=stable(identity):raise SystemExit('Qt build inputs changed; choose a fresh output directory or --reconfigure for builder-only fixes')
  history=out/'configuration-history';history.mkdir(exist_ok=True);shutil.copyfile(stamp,history/(sha(stamp)+'.json'))
 atomic_write(stamp, json.dumps(identity,indent=2)+'\n')
 archive=ROOT/f'build/qt6/vendor/qtbase-everywhere-src-{VERSION}.tar.xz'
 from fetch_sources import fetch
 fetch(dict(archive=archive.name, url=SOURCE_URL, sha256=SOURCE_SHA256), archive.parent)
 if sha(archive)!=SOURCE_SHA256:raise SystemExit('Qt source checksum mismatch: '+str(archive))
 base=a.sdk_build.resolve() if a.sdk_build else out
 sdk_record=None
 if a.sdk_build:
  if base==out:raise SystemExit('--sdk-build must differ from the output directory')
  sdk_record=json.loads((base/'BUILD.json').read_text())
  if sdk_record['version']!=VERSION or sdk_record['target_host_executed'] is not False:raise SystemExit('Incompatible SDK build')
  if sdk_record.get('compiler_build_sha256')!=identity['compiler_receipt_sha256']:raise SystemExit('Qt SDK uses a different compiler')
  for name,digest in sdk_record['libraries_sha256'].items():
   if sha(Path(sdk_record['sdk'])/name)!=digest:raise SystemExit('SDK library changed: '+name)
 source=Path(sdk_record['source_directory']) if sdk_record else base/f'qtbase-everywhere-src-{VERSION}'
 if not source.exists():
  unpack(archive, out, filter='data')
 from linux_uapi import prepare as prepare_uapi
 headers=prepare_uapi(out/'linux-uapi')
 conf=['bwrap','--unshare-all','--die-with-parent','--new-session','--ro-bind','/','/','--proc','/proc','--dev','/dev','--tmpfs','/tmp','--bind',str(out),str(out),'--chdir',str(out)]
 pkg=out/'pkgconfig-empty';pkg.mkdir(exist_ok=True)
 env=dict(PATH='/usr/bin:/bin',HOME='/tmp',TMPDIR='/tmp',LANG='C.UTF-8',PKG_CONFIG_LIBDIR=str(pkg))
 commands=[]
 def run(name,args):
  args=list(map(str,args))
  commands.append(args);print(name,flush=True)
  with (out/(name+'.log')).open('w') as log:subprocess.run([*conf,'--',*map(str,args)],env=env,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,check=True)
 host=base/'host';target=base/'target';sdk=base/'sdk'
 if sdk_record:
  sdk=Path(sdk_record['sdk']);host=sdk.parent/'host';target=sdk.parent/'target'
 common=['-GNinja','-DCMAKE_BUILD_TYPE=Release','-DBUILD_SHARED_LIBS=OFF','-DQT_BUILD_EXAMPLES=OFF','-DQT_BUILD_TESTS=OFF','-DFEATURE_icu=OFF','-DFEATURE_glib=OFF','-DFEATURE_dbus=OFF','-DFEATURE_network=OFF','-DFEATURE_sql=OFF','-DFEATURE_testlib=OFF','-DFEATURE_concurrent=OFF','-DFEATURE_xml=OFF','-DFEATURE_opengl=OFF','-DFEATURE_vulkan=OFF','-DFEATURE_system_pcre2=OFF']
 if not a.sdk_build and not (host/'.host-tools-complete').exists():
  run('host-configure',['cmake','-S',source,'-B',host,*common,'-DFEATURE_gui=OFF','-DFEATURE_widgets=OFF','-DFEATURE_developer_build=ON','-DWARNINGS_ARE_ERRORS=OFF'])
  run('host-tools',['cmake','--build',host,'--target','host_tools','-j',a.jobs]);(host/'.host-tools-complete').touch()
 toolchain=out/'musl-toolchain.cmake'
 toolchain.write_text(f'''set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "{cross}gcc")
set(CMAKE_CXX_COMPILER "{cxx}")
set(CMAKE_C_FLAGS_INIT "-O2 -g0 -fno-pie -fno-stack-protector -idirafter {headers}")
set(CMAKE_CXX_FLAGS_INIT "-O2 -g0 -fno-pie -fno-stack-protector -idirafter {headers}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -no-pie")
set(CMAKE_FIND_ROOT_PATH "{Path(sdk_record['sdk']) if sdk_record else base/'sdk'};{prefix};{sysroot}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
''')
 if not a.sdk_build and not (target/'.target-complete').exists():
  features=['xcb','eglfs','linuxfb','vnc','libinput','libudev','fontconfig','mtdev','xkbcommon','system_freetype','system_harfbuzz','system_png','system_jpeg','system_zlib','system_libb2','system_doubleconversion','zstd','openssl','pkg_config','cups','printsupport','evdev','tslib','gtk3','f16c','vaes','opengl_desktop','opengles2','opengles3','opengles31','opengles32','opengl_dynamic']
  features+=re.findall(r'qt_feature\("(avx[^"]*)"',(source/'configure.cmake').read_text())
  flags=f'-O2 -g0 -fno-pie -fno-stack-protector -idirafter {headers}'
  run('target-configure',['cmake','-S',source,'-B',target,*common,f'-DCMAKE_TOOLCHAIN_FILE={toolchain}',f'-DCMAKE_C_FLAGS={flags}',f'-DCMAKE_CXX_FLAGS={flags}',f'-DCMAKE_INSTALL_PREFIX={sdk}',f'-DQT_HOST_PATH={host}','-DQT_QPA_DEFAULT_PLATFORM=coast','-DINPUT_opengl=no','-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=OFF',*[f'-DFEATURE_{x}=OFF' for x in features]])
  run('target-build',['cmake','--build',target,'-j',a.jobs])
  run('target-install',['cmake','--install',target]);(target/'.target-complete').touch()
 record=dict(state='Qt libraries compiled',version=VERSION,source=dict(url=SOURCE_URL,sha256=SOURCE_SHA256),source_directory=str(source),confinement=conf,commands=commands,sdk=str(sdk),target_host_executed=False,source_sha256=recipe_inputs,compiler_build_sha256=identity['compiler_receipt_sha256'],compiler_inputs=compiler_inputs,linux_uapi_sha256={str(p.relative_to(headers)):sha(p) for p in sorted(headers.rglob('*')) if p.is_file()})
 if a.sdk_build:record['sdk_build']=dict(path=str(base/'BUILD.json'),sha256=sha(base/'BUILD.json'))
 if not a.qt_only:
  port=ROOT/'userland/qt6'
  def inputs_now():return {str(p.relative_to(ROOT)):sha(p) for p in sorted([*(p for p in port.rglob('*') if p.is_file()),ROOT/'include/uapi/window.h',ROOT/'include/uapi/keyboard.h',ROOT/'include/uapi/clipboard.h'])}
  inputs=inputs_now()
  run('port-configure',['cmake','-S',port,'-B',out/'port','-GNinja',f'-DCMAKE_TOOLCHAIN_FILE={toolchain}',f'-DCMAKE_PREFIX_PATH={sdk}',f'-DQT_HOST_PATH={host}','-DCMAKE_BUILD_TYPE=Release'])
  run('port-build',['cmake','--build',out/'port','-j',a.jobs])
  assert inputs==inputs_now(),'Port sources changed during compilation'
  binary=out/'port/qt6-demo';record.update(state='compiled; guest qualification required',binary=str(binary),binary_sha256=sha(binary),source_sha256={**recipe_inputs,**inputs})
 record['libraries_sha256']={str(p.relative_to(sdk)):sha(p) for p in sorted((sdk/'lib').glob('*.a'))}
 record['target_configuration_sha256']=sha(target/'CMakeCache.txt')
 if not a.qt_only:
  runtime=out/'runtime';dest=runtime/'usr/bin/qt6-demo';dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(binary,dest);dest.chmod(0o755)
  licenses=runtime/'usr/share/licenses/qt6';licenses.mkdir(parents=True,exist_ok=True)
  for path in (source/'LICENSES').iterdir():
   if path.is_file():shutil.copy2(path,licenses/path.name)
  sources=runtime/'usr/src/coast/qt6';sources.mkdir(parents=True,exist_ok=True)
  for path in port.iterdir():
   if path.is_file():shutil.copy2(path,sources/path.name)
  upstream=runtime/'usr/src/qt6';upstream.mkdir(parents=True,exist_ok=True);shutil.copy2(archive,upstream/archive.name)
  record['files']={str(path.relative_to(runtime)):dict(path=str(path),sha256=sha(path),mode=path.stat().st_mode&0o777) for path in sorted(runtime.rglob('*')) if path.is_file()}
 if a.install_runtime:
  if a.qt_only:raise SystemExit('--install-runtime requires the completed demo build')
  runtime=ROOT/'userland/runtime';dest=runtime/'usr/bin/qt6-demo';dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(binary,dest);dest.chmod(0o755)
  licenses=runtime/'usr/share/licenses/qt6';licenses.mkdir(parents=True,exist_ok=True)
  for path in (source/'LICENSES').iterdir():
   if path.is_file():shutil.copy2(path,licenses/path.name)
  sources=runtime/'usr/src/coast/qt6';sources.mkdir(parents=True,exist_ok=True)
  for path in port.iterdir():
   if path.is_file():shutil.copy2(path,sources/path.name)
  upstream=runtime/'usr/src/qt6';upstream.mkdir(parents=True,exist_ok=True);shutil.copy2(archive,upstream/archive.name)
 if recipe_inputs!={name:sha(ROOT/name) for name in recipe_inputs} or any(sha(Path(name))!=value for name,value in compiler_inputs.items()):raise SystemExit('Qt build inputs changed during compilation')
 atomic_write(out/'BUILD.json', json.dumps(record,indent=2)+'\n');print(record['state'],flush=True)
if __name__ == '__main__':
    with project_lock():
        main()
