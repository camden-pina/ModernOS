#!/usr/bin/env python3
"""Private diskless/network-disabled VMs; only each owned VM is controlled/closed."""
from pathlib import Path
import sys,time,json,traceback,re,os,argparse
R=Path(__file__).resolve().parents[1];W=R/'build/codeoss-shared-tests'
sys.path.insert(0,str(R/'tools'));import test_runtime as rt;import test_desktop as dd
p=argparse.ArgumentParser();p.add_argument('--iso',type=Path,required=True);p.add_argument('--label',required=True);p.add_argument('--scale',type=int,default=100);p.add_argument('--dark',action='store_true');p.add_argument('--output-root',type=Path,default=W/'qa');p.add_argument('--model-checks',action='store_true');p.add_argument('--power-review',action='store_true');p.add_argument('--loader-check',action='store_true');p.add_argument('--baseline',action='store_true');p.add_argument('--cycles',type=int,default=6);a=p.parse_args();S=lambda n:(n*a.scale+50)//100
out=a.output_root.resolve()/a.label;out.mkdir(parents=True,exist_ok=False);rt.WORK=dd.WORK=out;v=None;d=None;r={'result':'failed','checks':[],'scale':a.scale,'dark':a.dark}
def log():
 while v.selector.select(.005):
  data=os.read(v.process.stdout.fileno(),65536);v.pending.extend(data);v.log.write(data);v.log.flush()
 return re.sub(r'DESKTOP TERMINAL_OUTPUT window=\d+ workspace=\d+ ', '', Path(v.log.name).read_text(errors='replace').replace('\r',''))
def mark():return len(log())
def wait(pattern,z=0,timeout=30):
 end=time.monotonic()+timeout
 while True:
  m=list(re.finditer(pattern,log()[z:]))
  if m:return m[-1]
  assert time.monotonic()<end,'Missing '+pattern+'\n'+log()[-2300:]
  time.sleep(.02)
def check(s):r['checks'].append(s);print('PASS '+s,flush=True)
def key(k,*mods):z=mark();d.key(k,*mods);return z
def ack(action,z=0,wid=None,error=0):
 m=wait(r'CODE_CORE_ACK window='+(str(wid) if wid else r'\d+')+r' sequence=(\d+) action='+str(action)+r' error=(\d+)[^\n]*\n',z);assert int(m[2])==error,m[0];return m

def focus(wid):
 slots={}
 for event,value in re.findall(r'DESKTOP (OPEN|CLOSE) window=(\d+)',log()):
  value=int(value)
  if event=='CLOSE':slots.pop(value,None)
  elif value not in slots:slots[value]=next(i for i in range(32) if i not in slots.values())
 order=sorted(slots,key=slots.get);assert wid in order
 z=key('w','meta_l');wait(r'DESKTOP WINDOW_LIST',z)
 for _ in range(order.index(wid)):d.key('down')
 z=key('ret');wait(r'DESKTOP COMMAND[^\n]*',z);return d.geometry(wid)

def terminal(script,token):
 z=mark();d.key('t','meta_l');m=wait(r'DESKTOP OPEN window=(\d+)[^\n]*Terminal',z);tid=int(m[1]);wait(r'TERMINAL_MODE[^\n]*raw',z)
 z=mark();v.send("build-sh -c '"+script+" && printf \"\\n%s\\n\" "+token+"'\n");wait(r'\n'+token+r'\n',z);time.sleep(.1);d.key('f4','alt');wait(r'DESKTOP CLOSE window='+str(tid));return log()[z:]

def open_file(path):
 z=key('o','ctrl');v.send(path+'\n');m=wait(r'DESKTOP OPEN window=(\d+)[^\n]*'+re.escape(Path(path).name),z);wid=int(m[1]);ready=wait(r'CODE_CORE_READY window='+str(wid)+r' pid=(\d+) latency_ms=(\d+) text_bytes=(\d+)',z);return wid,int(ready[1]),int(ready[2]),int(ready[3])

def launch_app(name):
 z=mark();d.key('spc','meta_l');v.send(name);time.sleep(.15);d.key('ret');m=wait(r'DESKTOP OPEN window=(\d+)[^\n]*'+re.escape(name),z);return int(m[1])

def launch_code():return launch_app('Code OSS Core Preview')

def save_as(path,wid,error=0):
 z=key('s','ctrl','shift');v.send(path+'\n');return ack(7,z,wid,error)

def equal(name):
 d.move(d.width-10,d.height-90);time.sleep(.45);before=d.screenshot(name+'-cached');z=key('f12','ctrl','alt');wait(r'GLASS_RECOMPOSE',z);after=d.screenshot(name+'-fresh');top=S(22)*d.width*3;assert before[top:]==after[top:],'stale material/cursor pixels: '+name
try:
 v=rt.VM(4,storage=False,network=False,memory='4G',iso=a.iso,label=a.label)
 if a.model_checks:
  v.command('mkdir -p /home/user/model-check')
  r['model_checks']=v.command('codeoss-model --check /usr/share/codeoss/graphical-check.js /home/user/model-check',timeout=60);assert 'CODEOSS_GRAPHICAL_MODEL_CHECK_PASS' in r['model_checks']
  r['shared_checks']=v.command('codeoss-model --check /usr/share/codeoss/graphical-shared-check.js /home/user/model-check',timeout=60);assert 'CODEOSS_SHARED_CHECKS=26' in r['shared_checks'] and 'CODEOSS_GRAPHICAL_MODEL_CHECK_PASS' in r['shared_checks']
  r['legacy_core_check']=v.command('codeoss-core --script /usr/share/codeoss/core-check-native.js',timeout=60);assert 'CODEOSS_CORE_CHECK_PASS' in r['legacy_core_check']
 if a.loader_check:
  r['loader_check']=v.command('loader-lock-check',timeout=30);assert 'LOADER_LOCK_CHECK_DONE' in r['loader_check']
 v.command('mkdir -p /home/user/.config')
 v.shell_command("printf 'scale="+str([100,125,150,200].index(a.scale))+"\\ndark="+str(int(a.dark))+"\\n' > /home/user/.config/desktop.ini")
 v.shell_command("printf '\\357\\273\\277warm source \\303\\251\\360\\237\\230\\200\\r\\n' > /home/user/source.txt")
 v.send('gui --trace --stats\n');v.until(rb'GUI_READY:',60);d=dd.DesktopTest(v);d.screenshot('00-desktop');z=mark();A=launch_code();m=wait(r'CODE_CORE_READY window='+str(A)+r' pid=(\d+) latency_ms=(\d+) text_bytes=0',z);first_pid=int(m[1]);r['first_start_ms']=int(m[2]);r['model_pids']=[first_pid];r['reopen_ms']=[]
 v.send('A-start\n');save_as('/home/user/a.txt',A)
 z=key('n','ctrl');m=wait(r'DESKTOP OPEN window=(\d+)[^\n]*Code OSS Core Preview',z);B=int(m[1]);m=wait(r'CODE_CORE_READY window='+str(B)+r' pid=(\d+) latency_ms=(\d+)',z);r['model_pids'].append(int(m[1]));r['new_document_ms']=int(m[2]);v.send('B-only\n');save_as('/home/user/b.txt',B);check('two distinct document models saved independently')
 for i in range(a.cycles):
  focus(A);z=mark();v.send('x');ack(1,z,A)
  C,pid,ms,n=open_file('/home/user/source.txt');r['model_pids'].append(pid);r['reopen_ms'].append(ms)
  z=key('f4','alt');wait(r'DESKTOP CLOSE window='+str(C)+r'\b',z)
 focus(A);z=key('s','ctrl');ack(6,z,A);check('repeated open/close while another document retains its edits')
 focus(B);z=key('f4','alt');wait(r'DESKTOP CLOSE window='+str(B)+r'\b',z);focus(A);z=key('f4','alt');wait(r'DESKTOP CLOSE window='+str(A)+r'\b',z)
 z=mark();empty=launch_code();m=wait(r'CODE_CORE_READY window='+str(empty)+r' pid=(\d+) latency_ms=(\d+) text_bytes=0',z);r['model_pids'].append(int(m[1]));r['after_all_closed_ms']=int(m[2]);check('new document after all windows close has no former text/history')
 if not a.baseline:
  assert set(r['model_pids'])=={first_pid},r['model_pids'];check('all repeated windows reuse one engine process, including an idle service')
  A,pid,ms,n=open_file('/home/user/a.txt');B,pid,ms,n=open_file('/home/user/b.txt');focus(empty);z=key('f4','alt');wait(r'DESKTOP CLOSE window='+str(empty)+r'\b',z)
  focus(B);z=key('end','ctrl');ack(2,z,B);v.send('unsaved-B');z=key('end','ctrl');ack(2,z,B)
  if a.power_review:
   system=launch_app('System');z=key('tab');d.key('ret');wait(r'DESKTOP POWER_REVIEW',z);z=key('ret');wait(r'DESKTOP POWER_WAIT window='+str(B)+r'\b',z);d.key('esc');time.sleep(.2)
   review=log()[z:];assert 'DESKTOP POWER_REQUEST' not in review and not re.search(r'CODE_CORE_ACK[^\n]*action=15',review) and not re.search(r'DESKTOP CLOSE window=('+str(A)+'|'+str(B)+r')\b',review),review
   focus(system);z=key('f4','alt');wait(r'DESKTOP CLOSE window='+str(system)+r'\b',z);check('cancelled power approval preserves clean and dirty models without Release or power request')
  focus(A);z=key('end','ctrl');ack(2,z,A);v.send('unsaved-A');z=key('end','ctrl');ack(2,z,A);d.screenshot('01-two-distinct-dirty-models');check('multiple live models retain independent queued edits')
  worker=int(wait(r'CODE_MODEL_CHILD launcher='+str(first_pid)+r' worker=(\d+)')[1]);terminal('/usr/bin/linux-bash --noprofile -c "kill -9 '+str(worker)+'"','KILLED_SHARED_WORKER')
  wait(r'CODE_CORE_ERROR window='+str(A)+r'\b');wait(r'CODE_CORE_ERROR window='+str(B)+r'\b');d.screenshot('02-shared-failure');check('forced own service failure retains every window snapshot')
  recovered=[]
  for wid,name in [(A,'recovered-a.txt'),(B,'recovered-b.txt')]:
   g=focus(wid);z=mark();d.click(g['x']+S(50),g['y']+g['body']+S(20));m=wait(r'CODE_CORE_READY window='+str(wid)+r' pid=(\d+) latency_ms=(\d+)',z);recovered.append(int(m[1]));ack(13,z,wid);save_as('/home/user/'+name,wid)
  assert recovered[0]==recovered[1] and recovered[0]!=first_pid,recovered;r['recovery_pids']=recovered;d.screenshot('03-recovered-models');equal('04-recovery-repaint');check('each recovered document saves separately through one fresh shared service')
  focus(B);z=key('f4','alt');wait(r'DESKTOP CLOSE window='+str(B)+r'\b',z);focus(A);z=key('f4','alt');wait(r'DESKTOP CLOSE window='+str(A)+r'\b',z)
 else:
  z=key('f4','alt');wait(r'DESKTOP CLOSE window='+str(empty)+r'\b',z)
 d.exit()
 suffix='x'*a.cycles
 script="printf 'A-start\\n"+suffix+"' > /home/user/expect-a; printf 'B-only\\n' > /home/user/expect-b; cmp /home/user/a.txt /home/user/expect-a && cmp /home/user/b.txt /home/user/expect-b"
 r['saved_files']=v.shell_command(script+' && printf VERIFIED_INDEPENDENT_FILES');assert 'VERIFIED_INDEPENDENT_FILES' in r['saved_files']
 if not a.baseline:
  script="printf 'A-start\\n"+suffix+"unsaved-A' > /home/user/expect-ra; printf 'B-only\\nunsaved-B' > /home/user/expect-rb; cmp /home/user/recovered-a.txt /home/user/expect-ra && cmp /home/user/recovered-b.txt /home/user/expect-rb"
  r['recovered_files']=v.shell_command(script+' && printf VERIFIED_RECOVERED_FILES');assert 'VERIFIED_RECOVERED_FILES' in r['recovered_files']
  stats=wait(r'CODE_SERVICE_STATS generations=(\d+) max_cold_ms=(\d+) max_warm_ms=(\d+) unreaped=(\d+) queued_bytes=(\d+)');r['service_stats']=dict(zip(['generations','max_cold_ms','max_warm_ms','unreaped','queued_bytes'],map(int,stats.groups())));assert r['service_stats']['generations']==2 and r['service_stats']['unreaped']==r['service_stats']['queued_bytes']==0
 text=log();r['window_tick_max_ms']=[int(x) for x in re.findall(r'CODE_CORE_STATS[^\n]*max_tick_ms=(\d+)',text)];r['baseline']=a.baseline;check('independent guest byte comparison and owned process cleanup');r['result']='passed'
except Exception as e:
 r['error']=str(e);(out/'exception.txt').write_text(traceback.format_exc())
 if d:
  try:d.screenshot('failure')
  except Exception:pass
 raise
finally:
 if v:v.close()
 (out/'report.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps({k:v for k,v in r.items() if k not in ('saved_files','recovered_files','model_checks','shared_checks','legacy_core_check','loader_check')},indent=2),flush=True)
