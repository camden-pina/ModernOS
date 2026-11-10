#!/usr/bin/env python3
"""Disposable guest regression for real Bash foreground handoff; no host disks/network."""
from pathlib import Path
import sys,time,json,traceback,re,os,argparse
R=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(R/'tools'));import test_runtime as rt;import test_desktop as dd
p=argparse.ArgumentParser();p.add_argument('--iso',type=Path,required=True);p.add_argument('--label',required=True);p.add_argument('--scale',type=int,default=0);p.add_argument('--work-dir',type=Path,required=True);a=p.parse_args();W=a.work_dir.resolve()
out=W/'qa'/a.label;out.mkdir(parents=True,exist_ok=False);rt.WORK=dd.WORK=out;v=None;d=None;r={'result':'failed','checks':[]}
def pump():
 if v.selector.select(.01):
  data=os.read(v.process.stdout.fileno(),65536);v.pending.extend(data);v.log.write(data);v.log.flush()
def log():
 pump();text=Path(v.log.name).read_text(errors='replace').replace('\r','');return re.sub(r'DESKTOP TERMINAL_OUTPUT window=\d+ workspace=\d+ ', '',text)
def mark():return len(log())
def wait(pattern,after=0,timeout=20):
 end=time.monotonic()+timeout
 while True:
  m=re.findall(pattern,log()[after:])
  if m:return m[-1]
  assert time.monotonic()<end,'Missing '+pattern+'\n'+log()[-1600:]
  time.sleep(.02)
def action(key,*mods):
 z=mark();d.key(key,*mods);return z
def command(text,token):
 z=mark();v.send(text+'\n');wait(r'\n'+re.escape(token)+r'\n',z);time.sleep(.15)
def pos(g,x,y):return(g['x']+S(x),g['y']+g['body']+S(y))
def click(g,x,y):d.click(*pos(g,x,y))
def check(name):r['checks'].append(name);print('PASS '+name,flush=True)
try:
 v=rt.VM(4,storage=False,network=False,memory='4G',iso=a.iso,label=a.label)
 import shlex
 config='scale='+str(a.scale)+'\\nterminal_shell=1\\n'
 v.shell_command('mkdir -p /home/user/.config; printf %b '+shlex.quote(config)+' > /home/user/.config/desktop.ini')
 v.send('gui --trace --stats\n');v.until(rb'GUI_READY:',60);d=dd.DesktopTest(v);d.screenshot('00-desktop');z=action('t','meta_l');wait(r'TERMINAL_MODE[^\n]*raw',z)
 z=mark();v.send('terminal-job-check\n');wait(r'TTY_PROBE fd=2[^\n]*\n',z);r['foreground_probe']=log()[z:];m=re.search(r'TTY_PROBE before pid=(\d+)[^\n]*pgid=(\d+)',r['foreground_probe']);fg=re.search(r'foreground=(\d+)',r['foreground_probe']);assert m and fg and m[2]==fg[1],r['foreground_probe'];check('Bash child group is actual PTY foreground group')
 r['interrupt_seconds']=[]
 for iteration in range(5):
  z=mark();start=time.monotonic();v.send('sleep 20\n');wait(r'TERMINAL_MODE[^\n]*canonical',z);time.sleep(.1);d.key('c','ctrl');command("printf '%s\\n' child_interrupt_"+str(iteration),'child_interrupt_'+str(iteration));elapsed=time.monotonic()-start;r['interrupt_seconds'].append(elapsed);assert elapsed<3,elapsed
 check('five repeated Bash foreground child Ctrl-C and shell returns')
 z=mark();v.send("build-sh -c 'read answer; printf \"CHILDREAD=%s\\n\" \"$answer\"'\n");wait(r'TERMINAL_MODE[^\n]*canonical',z);time.sleep(.15);v.send('stdin_ok\n');wait(r'\nCHILDREAD=stdin_ok\n',z);check('foreground child interactive stdin')
 command("printf 'pipe_ok\\n' | build-sh -c cat",'pipe_ok');command("value=$(printf sub_ok); printf '%s\\n' \"$value\"",'sub_ok');check('pipeline and command substitution complete without pgrp-pipe deadlock')
 z=mark();start=time.monotonic();v.send('sleep 20 | build-sh -c cat\n');wait(r'TERMINAL_MODE[^\n]*canonical',z);time.sleep(.2);d.key('c','ctrl');command("printf '%s\\n' pipeline_interrupt_ok",'pipeline_interrupt_ok');r['pipeline_interrupt_seconds']=time.monotonic()-start;assert r['pipeline_interrupt_seconds']<3;check('Ctrl-C interrupts foreground pipeline group')
 command("sleep 1 & printf '%s\\n' background_keeps_prompt",'background_keeps_prompt');command("wait; printf '%s\\n' background_wait_done",'background_wait_done');check('background launch and wait keep shell foreground')
 z=mark();v.send('terminal-job-check --deny-foreground & wait\n');wait(r'\nGUARD_REJECTED_FOREGROUND_TAKEOVER\n',z);check('background child cannot promote its own group; kernel enforcement unchanged')
 command("export OUTER_PID=$$; printf '%s\\n' outer_ready",'outer_ready')
 z=mark();v.send('/usr/bin/linux-bash --noprofile -i\n');wait(r'TERMINAL_MODE[^\n]*raw',z);command("test \"$$\" != \"$OUTER_PID\" && printf '%s\\n' nested_ready",'nested_ready')
 z=mark();start=time.monotonic();v.send('sleep 20\n');wait(r'TERMINAL_MODE[^\n]*canonical',z);time.sleep(.1);d.key('c','ctrl');command("printf '%s\\n' nested_interrupt_ok",'nested_interrupt_ok');r['nested_interrupt_seconds']=time.monotonic()-start;assert r['nested_interrupt_seconds']<3
 v.send('exit\n');time.sleep(.2);command("test \"$$\" = \"$OUTER_PID\" && printf '%s\\n' outer_restored",'outer_restored');check('nested interactive shell child Ctrl-C and outer foreground restoration')
 z=mark();v.send('codeoss-core /home/user/bash-codeoss.txt\n');wait(r'CODEOSS_CORE_READY',z,timeout=60)
 v.send('insert 0 "Bash CodeOSS\\r\\n"\n');wait(r'"dirty":true',z);v.send('save\n');wait(r'SAVED /home/user/bash-codeoss.txt',z);v.send('quit\n');wait(r'CODEOSS_CORE_CLOSED',z);command("printf '%s\\n' codeoss_return_ok",'codeoss_return_ok');check('genuine Code OSS upstream core interactive edit/save through Bash foreground PTY')
 z=mark();v.send('nano /home/user/bash-r8.txt\n');wait(r'TERMINAL_MODE[^\n]*raw',z,timeout=5);time.sleep(.4);d.screenshot('01-bash-nano');d.key('right','meta_l');time.sleep(.3);d.screenshot('02-nano-resized');v.send('Bash foreground nano saved');time.sleep(.3);d.key('o','ctrl');d.key('ret');time.sleep(.3);d.key('x','ctrl');command("printf '%s\\n' nano_return_ok",'nano_return_ok');check('Bash nano raw mode/resize/edit/save/return')
 d.screenshot('02-bash-returned');d.exit();r['nano_file']=v.command('cat /home/user/bash-r8.txt');assert 'Bash foreground nano saved' in r['nano_file'];r['codeoss_bytes']=v.shell_command('od -An -tx1 /home/user/bash-codeoss.txt');assert '42 61 73 68 20 43 6f 64 65 4f 53 53 0d 0a' in r['codeoss_bytes'];r['result']='passed'
except Exception as e:
 r['error']=str(e);(out/'exception.txt').write_text(traceback.format_exc())
 if d:
  try:d.screenshot('failure')
  except Exception:pass
 raise
finally:
 if v:v.close()
 (out/'report.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2),flush=True)
