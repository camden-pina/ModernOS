#!/usr/bin/env python3
"""Owned diskless/offline guest Bash job-control regression; never controls other VMs."""
from pathlib import Path
import sys,time,json,traceback,re,os,argparse
R=Path(__file__).resolve().parents[1];sys.path.insert(0,str(R/'tools'));import test_runtime as rt;import test_desktop as dd
p=argparse.ArgumentParser();p.add_argument('--iso',type=Path,required=True);p.add_argument('--label',required=True);p.add_argument('--scale',type=int,choices=range(4),default=0);p.add_argument('--work-dir',type=Path,required=True);p.add_argument('--memory',default='4G');p.add_argument('--accel',choices=['tcg','kvm'],default='tcg');a=p.parse_args();W=a.work_dir.resolve();out=W/'qa'/a.label;out.mkdir(parents=True,exist_ok=False);rt.WORK=dd.WORK=out;v=None;d=None;r={'result':'failed','checks':[],'scale':a.scale}
def raw():
 while v.selector.select(.005):
  data=os.read(v.process.stdout.fileno(),65536)
  if not data:break
  v.pending.extend(data);v.log.write(data);v.log.flush()
 return Path(v.log.name).read_bytes().decode(errors='replace')
def log(z=0):
 # Console trace framing adds CRLF; PTY's real newlines acquire CRCRLF.
 # Reassemble payload chunks before matching, including metadata interleaves.
 output=[];metadata=[]
 for record in re.split(r'(?m)(?<=\n)(?=DESKTOP )',raw()[z:]):
  if record.endswith('\r\n'):record=record[:-2]
  m=re.match(r'DESKTOP TERMINAL_OUTPUT window=\d+ workspace=\d+ ([\s\S]*)',record)
  if m:output.append(m[1])
  else:metadata.append(record+'\n')
 t=(''.join(output)+'\n'+''.join(metadata)).replace('\r','')
 return re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]','',t)
def mark():return len(raw())
def wait(pattern,z=0,timeout=20):
 end=time.monotonic()+timeout
 while True:
  m=list(re.finditer(pattern,log(z)))
  if m:return m[-1]
  assert time.monotonic()<end,'Missing '+pattern+'\n'+log()[-2200:]
  time.sleep(.02)
def key(k,*mods):z=mark();d.key(k,*mods);return z
def command(text,token,timeout=20):
 z=mark();v.send(text+'\n');wait(r'\n'+re.escape(token)+r'\n',z,timeout);time.sleep(.12);return log(z)
def check(s):r['checks'].append(s);print('PASS '+s,flush=True)
def launch_job(text):
 z=mark();v.send(text+'\n');wait(r'TERMINAL_MODE[^\n]*canonical',z);time.sleep(.1);return z
def suspend(z):
 start=time.monotonic();d.key('z','ctrl');wait(r'\n[^\n]*Stopped[^\n]*\n',z);r.setdefault('suspend_seconds',[]).append(time.monotonic()-start)
def resume_and_interrupt():
 z=launch_job('fg %+');d.key('c','ctrl');command("printf '%s\\n' R9_FG_INTERRUPTED",'R9_FG_INTERRUPTED')
try:
 v=rt.VM(4,storage=False,network=False,memory=a.memory,iso=a.iso,label=a.label,accel=a.accel)
 if a.scale:
  v.command('mkdir -p /home/user/.config');v.shell_command("printf 'scale="+str(a.scale)+"\\nterminal_shell=1\\n' > /home/user/.config/desktop.ini")
 v.send('gui --trace --stats\n');v.until(rb'GUI_READY:',60);d=dd.DesktopTest(v);d.screenshot('00-desktop');z=key('t','meta_l');wait(r'TERMINAL_SHELL[^\n]*linux-bash',z);wait(r'TERMINAL_MODE[^\n]*raw',z)
 r['tty']=command("job-control-check --tty-info; printf '%s\\n' R9_TTY_READY",'R9_TTY_READY');assert re.search(r'JOB_TTY[^\n]*vsusp=26 isig=1',r['tty']);check('fresh image GNU Bash selection and real suspend character/foreground group')
 z=launch_job('sleep 30');suspend(z);j=command("jobs -l; printf '%s\\n' R9_STOPPED_JOB",'R9_STOPPED_JOB');assert 'Stopped' in j;d.screenshot('01-stopped-job')
 j=command("bg %+; jobs -l; printf '%s\\n' R9_BG_RUNNING",'R9_BG_RUNNING');assert 'Running' in j;z=launch_job('fg %+');suspend(z);resume_and_interrupt();check('Ctrl-Z jobs/bg/fg repeated suspend and foreground Ctrl-C')
 z=launch_job('sleep 30 | build-sh -c cat');suspend(z);command("jobs -l; bg %+; printf '%s\\n' R9_PIPE_BG",'R9_PIPE_BG');resume_and_interrupt();check('all foreground pipeline processes stop, resume and interrupt')
 z=mark();v.send("build-sh -c 'read value; printf \"R9_READ=%s\\n\" \"$value\"' &\n");time.sleep(.2);j=command("jobs -l; printf '%s\\n' R9_TTIN_STOPPED",'R9_TTIN_STOPPED');assert 'Stopped' in j
 z=launch_job('fg %+');v.send('foreground-read-ok\n');wait(r'\nR9_READ=foreground-read-ok\n',z);command("printf '%s\\n' R9_READ_RETURN",'R9_READ_RETURN');check('background terminal read stops with TTIN, then retries after fg')
 z=mark();v.send('job-control-check --background-mode &\n');time.sleep(.2);j=command("jobs -l; printf '%s\\n' R9_MODE_STOPPED",'R9_MODE_STOPPED');assert 'Stopped' in j;z=mark();v.send('fg %+\n');wait(r'\nR9_MODE_FOREGROUND_OK\n',z);command("printf '%s\\n' R9_TTOU_RETURN",'R9_TTOU_RETURN');check('background mode change stops with TTOU and succeeds only after authorized fg')
 z=mark();v.send('job-control-check --deny-mode & wait\n');wait(r'\nR9_BACKGROUND_MODE_DENIED\n',z);command("printf '%s\\n' R9_MODE_GUARD",'R9_MODE_GUARD')
 z=mark();v.send('job-control-check --deny-read & wait\n');wait(r'\nR9_BACKGROUND_READ_DENIED\n',z);command("printf '%s\\n' R9_READ_GUARD",'R9_READ_GUARD')
 z=mark();v.send('terminal-job-check --deny-foreground & wait\n');wait(r'\nGUARD_REJECTED_FOREGROUND_TAKEOVER\n',z);command("printf '%s\\n' R9_FG_GUARD",'R9_FG_GUARD');check('ignored TTIN/TTOU cannot bypass access checks; background foreground takeover denied')
 j=command("job-control-check --tostop; printf '%s\\n' R9_TOSTOP_DONE",'R9_TOSTOP_DONE');assert 'R9_TOSTOP_CHILD_OUTPUT' in j and 'R9_TOSTOP_RESTORED' in j;check('TOSTOP background output stops and retries after parent-controlled foreground transfer')
 command("export R9_OUTER_PID=$$; printf '%s\\n' R9_OUTER",'R9_OUTER');z=mark();v.send('/usr/bin/linux-bash --noprofile -i\n');time.sleep(.2);command("test \"$$\" != \"$R9_OUTER_PID\" && printf '%s\\n' R9_INNER",'R9_INNER')
 z=launch_job('sleep 30');suspend(z);command("bg %+; printf '%s\\n' R9_INNER_BG",'R9_INNER_BG');resume_and_interrupt();v.send('exit\n');time.sleep(.2);command("test \"$$\" = \"$R9_OUTER_PID\" && printf '%s\\n' R9_OUTER_RESTORED",'R9_OUTER_RESTORED');check('nested interactive Bash jobs and outer foreground restoration')
 z=mark();v.send('nano /home/user/r9-nano.txt\n');wait(r'TERMINAL_MODE[^\n]*raw',z);time.sleep(.4);v.send('before suspend ');time.sleep(.2);key('t','ctrl');z=key('z','ctrl');wait(r'\n[^\n]*Stopped[^\n]*\n',z);d.screenshot('02-nano-stopped');z=mark();v.send('fg %+\n');wait(r'GNU nano 8[.]7',z);time.sleep(.3);v.send('after resume');time.sleep(.2);d.key('right','meta_l');time.sleep(.2);d.key('o','ctrl');d.key('ret');time.sleep(.2);d.key('x','ctrl');command("printf '%s\\n' R9_NANO_RETURN",'R9_NANO_RETURN');check('nano suspend/fg preserves unsaved buffer, raw input, resize and save')
 d.screenshot('03-jobs-returned');z=launch_job('sleep 30');suspend(z);j=command("jobs -l; printf '%s\\n' R9_CLOSE_STOPPED",'R9_CLOSE_STOPPED');m=re.search(r'\[\d+\]\+\s+(\d+)\s+Stopped',j);assert m,j;closing_pid=int(m[1]);z=key('f4','alt');wait(r'DESKTOP CLOSE window=2\b',z);z=key('t','meta_l');wait(r'TERMINAL_SHELL[^\n]*linux-bash',z);wait(r'TERMINAL_MODE[^\n]*raw',z);command("printf '%s\\n' R9_NEW_TERMINAL",'R9_NEW_TERMINAL');d.exit();r['processes_after_close']=v.command('ps');assert not re.search(r'(?m)^\s*'+str(closing_pid)+r'\s',r['processes_after_close']);check('closing a Terminal cleans up its stopped child; new Terminal input remains usable');r['nano']=v.command('cat /home/user/r9-nano.txt');assert 'before suspend after resume' in r['nano'];r['result']='passed'
except Exception as e:
 r['error']=str(e);(out/'exception.txt').write_text(traceback.format_exc())
 if d:
  try:d.screenshot('failure')
  except Exception:pass
 raise
finally:
 if v:v.close()
 (out/'report.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps({k:v for k,v in r.items() if k not in ('tty','nano')},indent=2),flush=True)
