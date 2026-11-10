#!/usr/bin/env python3
"""Qt/native UTF-8 clipboard round trips and focused-window authority checks."""
import argparse,base64,gzip,hashlib,json,os,re,subprocess,time,traceback
from pathlib import Path
from source_tree import digest
import test_runtime as runtime
import test_desktop as desktop
ROOT=Path(__file__).resolve().parents[1]

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--iso',type=Path,required=True);p.add_argument('--output-root',type=Path,required=True);a=p.parse_args()
 out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=False);runtime.WORK=desktop.WORK=out
 v=d=None;image_sha256=digest(a.iso);report=dict(state='failed',checks=[],iso=str(a.iso.resolve()),iso_sha256=image_sha256,target_host_executed=False)
 binary=out/'clipboard-window-check';source=ROOT/'tools/clipboard-window-check.c'
 conf=['bwrap','--unshare-all','--die-with-parent','--new-session','--ro-bind','/','/','--proc','/proc','--dev','/dev','--tmpfs','/tmp','--bind',str(out),str(out),'--']
 cc=ROOT/'build/compiler/cross/bin/x86_64-linux-musl-gcc'
 manifest=a.iso.with_name('MANIFEST.json')
 if manifest.is_file():
  image=json.loads(manifest.read_text())
  if image.get('wider_integrations'):
   compiler=Path(image['components']['compiler_build']['path'])
   if digest(compiler)!=image['components']['compiler_build']['sha256']:raise ValueError('Compiler receipt changed')
   cc=compiler.parent/'cross/bin/x86_64-linux-musl-gcc'
 subprocess.run(conf+[str(cc),'-O2','-static','-no-pie','-idirafter',str(ROOT/'include'),str(source),'-o',str(binary)],check=True,stdin=subprocess.DEVNULL)
 encoded=base64.b64encode(gzip.compress(binary.read_bytes(),mtime=0)).decode();report['fixture_sha256']=hashlib.sha256(binary.read_bytes()).hexdigest()
 def raw():
  while v.selector.select(.003):
   b=os.read(v.process.stdout.fileno(),65536)
   if not b:break
   v.pending.extend(b);v.log.write(b);v.log.flush()
  return Path(v.log.name).read_text(errors='replace')
 def observed(start=0):
  s=raw()[start:];kernel=r'\x1b\[0;32m\[\s*\d+[.]\d+\][^\n]*\n';s=re.sub(kernel,'',s);output=[];other=[]
  for r in re.split(r'(?m)(?<=\n)(?=DESKTOP )',s):
   m=re.match(r'DESKTOP TERMINAL_OUTPUT window=\d+ workspace=\d+ ([\s\S]*)',r.removesuffix('\n'))
   if m:output.append(m[1])
   else:other.append(r)
  return re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]','',(''.join(output)+'\n'+''.join(other)).replace('\r',''))
 def wait(pattern,start=0,timeout=30):
  end=time.monotonic()+timeout
  while time.monotonic()<end:
   s=observed(start);assert 'PANIC:' not in s;m=re.search(pattern,s)
   if m:return m
   time.sleep(.02)
  raise AssertionError('Missing '+pattern+'\n'+observed(start)[-3000:])
 def passed(s):report['checks'].append(s);print('PASS '+s,flush=True)
 try:
  v=runtime.VM(2,storage=False,network=False,memory='16G',accel='kvm',iso=a.iso,label='qt-clipboard')
  v.user_command('mkdir -p /home/user/Documents')
  for i in range(0,len(encoded),400):v.user_command(('write' if i==0 else 'append')+' /home/user/clipboard-check.b64 '+encoded[i:i+400])
  v.shell_command('base64 -d /home/user/clipboard-check.b64 | gzip -d > /home/user/clipboard-window-check; chmod 755 /home/user/clipboard-window-check',as_user=True)
  v.shell_command("printf 'Native café 日本語' | clip -c",as_user=True)
  v.send('gui --trace\n');v.until(b'GUI_READY:',60);d=desktop.DesktopTest(v);d.screenshot('00-desktop')
  start=len(raw());qt=d.launch('Qt 6 Demo');wait('QT6_READY version=6.8.3 uid=1000',start);wait('QT6_PRESENT',start)
  d.key('v','ctrl');d.key('ret');wait('QT6_ADD text=Native café 日本語 count=4',start)
  passed('Native UTF-8 clipboard pastes into the real Qt line editor')
  for c in 'Qt clipboard':d.key('spc' if c==' ' else c.lower(),*(['shift'] if c.isupper() else []));raw()
  d.key('a','ctrl');d.key('c','ctrl');d.screenshot('01-qt-copied')
  d.launch('Terminal');wait('TERMINAL_MODE[^\n]*raw');start=len(raw())
  v.send("clip -p > /home/user/Documents/qt-clipboard.txt && printf '%s\\n' EXPORTED_QT_CLIPBOARD\n");wait(r'\nEXPORTED_QT_CLIPBOARD\n',start)
  start=len(raw());v.send("printf 'Terminal replacement' | clip -c && printf '%s\\n' REPLACED_CLIPBOARD\n");wait(r'\nREPLACED_CLIPBOARD\n',start)
  start=len(raw());d.key('tab','alt');wait('DESKTOP FOCUS window='+str(qt)+r'\b',start);d.key('a','ctrl');d.key('v','ctrl');d.key('ret');wait('QT6_ADD text=Terminal replacement count=5',start)
  passed('Qt copy reaches the native clipboard; a later Terminal copy replaces the Qt cache after focus returns')
  d.key('f4','alt');wait(r'DESKTOP GAME_EXIT pid=\d+ status=0 reaped=1',start)
  d.launch('Terminal');start=len(raw())
  v.send("clip -p </dev/null >/home/user/Documents/background-clipboard.txt & wait $!; printf '\\n%s%d\\n' BACKGROUND_CLIP_STATUS $?\n")
  result=wait(r'\nBACKGROUND_CLIP_STATUS([01])\n',start)
  if result[1]!='1':
   point=len(raw());v.send("/home/user/clipboard-window-check --terminal-info </dev/null >/home/user/Documents/clip-job-info.txt & wait $!; cat /home/user/Documents/clip-job-info.txt; printf '\\n%s\\n' CLIP_JOB_END\n");wait(r'\nCLIP_JOB_END\n',point)
   raise AssertionError('Background clipboard command unexpectedly succeeded: '+observed(point))
  passed('A background clipboard command remains denied after the bounded foreground-handoff retries')
  start=len(raw());v.send('/home/user/clipboard-window-check /home/user/Documents/clipboard-authority.txt\n')
  try:wait(r'DESKTOP OPEN window=\d+ workspace=\d+ \d+\n',start,timeout=8)
  except AssertionError:
   point=len(raw());v.send("cat /home/user/Documents/clipboard-authority.txt; printf '%s\\n' PROBE_LOG_END\n");wait(r'\nPROBE_LOG_END\n',point,timeout=20)
   raise AssertionError('Native clipboard probe did not create a window: '+observed(point))
  time.sleep(.5)
  d.screenshot('02-native-authority-probe');d.launch('Files');time.sleep(.6);raw();d.exit()
  v.shell_command("test \"$(cat /home/user/Documents/qt-clipboard.txt)\" = 'Qt clipboard'",as_user=True)
  result=v.user_command('cat /home/user/Documents/clipboard-authority.txt')
  for marker in ['CLIP_WINDOW_INITIAL_DENIED','CLIP_WINDOW_FOCUSED_ALLOWED','CLIP_WINDOW_UNFOCUSED_DENIED','CLIP_WINDOW_CHECK_PASS']:assert marker in result,result
  passed('A native client without terminal descriptors is denied before focus, allowed while focused, and denied immediately after focus leaves')
  report['state']='passed'
 except BaseException as e:
  report['error']=repr(e);(out/'exception.txt').write_text(traceback.format_exc())
  if d:
   try:d.screenshot('failure')
   except Exception:pass
  raise
 finally:
  if v:v.close()
  report['owned_vm_stopped']=bool(v and v.process.poll() is not None);report['image_unchanged']=digest(a.iso)==image_sha256;(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
