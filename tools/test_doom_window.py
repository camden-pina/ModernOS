#!/usr/bin/env python3
"""Validate Doom in managed software windows, real input and HDA capture."""
import argparse,hashlib,json,pathlib,re,time,os
import test_desktop as desktop
from test_desktop import DesktopTest
from test_runtime import VM,ROOT,assert_success,ANSI
from test_doom import frame,key,event,capture_check
from test_doom_music import music_state,capture,score_hash
def decode_terminal_stream(raw,window):
 # Human GUI traces wrap arbitrary PTY read chunks. Remove only that framing,
 # preserving logical Doom lines across split printf/writev calls and repaint
 # diagnostics. This fixture's application never emits reserved DESKTOP tags.
 plain=ANSI.sub(b'',raw).replace(b'\r',b'')
 plain=re.sub(rb'(?m)^(?:D|DE|DES|DESK|DESKT|DESKTO|DESKTOP(?: [A-Z_]*)?)$',b'',plain)
 headers=list(re.finditer(rb'(?m)^DESKTOP ([A-Z_]+) ',plain));parts=[]
 for i,head in enumerate(headers):
  if head[1]!=b'TERMINAL_OUTPUT':continue
  start=head.end();end=headers[i+1].start() if i+1<len(headers) else len(plain)
  meta=re.match(rb'window=([0-9]+) workspace=([0-9]+) ',plain[start:end])
  if not meta or int(meta[1])!=window:continue
  payload=plain[start+meta.end():end]
  # trace() appends exactly one newline, in addition to any application LF.
  if payload.endswith(b'\n'):payload=payload[:-1]
  parts.append(payload)
 return b''.join(parts)

class WindowVM(VM):
 def __init__(self,*args,**kwargs):
  self.decode_terminal=False;self.terminal_window=0;self.terminal_cursor=0
  super().__init__(*args,**kwargs)
 def terminal_bytes(self):
  self.log.flush();return decode_terminal_stream(pathlib.Path(self.log.name).read_bytes(),self.terminal_window)
 def until(self,pattern,timeout=30):
  if not self.decode_terminal or not isinstance(pattern,bytes) or b'DOOM_' not in pattern:
   result=super().until(pattern,timeout)
   if self.decode_terminal:
    data=self.terminal_bytes();self.terminal_cursor=data.rfind(b'\n')+1
   return result
  deadline=time.monotonic()+timeout;plain=b''
  while time.monotonic()<deadline:
   data=self.terminal_bytes();plain=data[self.terminal_cursor:]
   if re.search(pattern,plain):
    self.terminal_cursor=data.rfind(b'\n')+1;self.pending.clear()
    return plain.decode(errors='replace')
   if b'PANIC:' in self.pending or self.process.poll() is not None:
    raise AssertionError(self.name+': guest stopped: '+bytes(self.pending[-8192:]).decode(errors='replace'))
   if self.selector.select(.1):
    chunk=os.read(self.process.stdout.fileno(),65536);self.pending.extend(chunk);self.log.write(chunk);self.log.flush()
  raise AssertionError(self.name+': missing reconstructed PTY '+repr(pattern)+'\n'+plain[-16000:].decode(errors='replace'))

VIEW=re.compile(r'DESKTOP GAME_VIEW window=(\d+) x=(\d+) y=(\d+) width=(\d+) height=(\d+) body_x=(\d+) body_y=(\d+) body_w=(\d+) body_h=(\d+) source=320x200\n')
def _wait_view(vm,previous=None,timeout=25):
 # OPEN/GEOMETRY waits can consume any prefix of the following GAME_VIEW.
 # Reassemble complete records from the persistent log, draining one line at
 # a time; a matching prefix need not remain in VM.pending.
 deadline=time.monotonic()+timeout
 while True:
  vm.log.flush();matches=list(VIEW.finditer(pathlib.Path(vm.log.name).read_text(errors='replace')))
  if matches:
   v=dict(zip(('window','x','y','width','height','body_x','body_y','body_w','body_h'),map(int,matches[-1].groups())))
   assert abs(v['width']*3-v['height']*4)<=4,v
   if previous is None or v!=previous:return v
  remaining=deadline-time.monotonic()
  assert remaining>0,('Missing complete changed viewport geometry',previous)
  vm.until(rb'\n',remaining)

def current_view(vm):
 return _wait_view(vm)

def changed_view(vm,previous):
 return _wait_view(vm,previous,20)

def exercise(a):
 out=a.output_root/a.label;out.mkdir(exist_ok=False)
 wav=out/'hda.wav';vm=None;report={'result':'failed','mode':a.mode,'iso':str(a.iso),'iso_sha256':hashlib.sha256(a.iso.read_bytes()).hexdigest(),'cpus':a.cpus,'accel':a.accel,'physical_gpu_tested':False,'cases':[]}
 def passed(name):report['cases'].append(name);print('PASS '+name,flush=True)
 try:
  vm=WindowVM(a.cpus,storage=False,network=False,iso=a.iso,accel=a.accel,label=a.label,audio_capture=wav,audio_device='hda',audio_capture_rate=48000)
  vm.send('gui --trace --stats\n');vm.until(rb'GUI_READY: Coast desktop',45)
  d=DesktopTest(vm);desktop.WORK=out;d.screenshot('desktop-before-doom');vm.terminal_window=d.launch('Terminal');vm.decode_terminal=True;vm.terminal_cursor=vm.terminal_bytes().rfind(b'\n')+1
  command='doom --trace -warp 1 -skill 1 -nomonsters'+(' -nosfx' if a.mode!='gameplay' else '')+(' -nomusic' if a.mode=='silent' else '')
  vm.send(command+'\n');vm.until(rb'DOOM_READY window=[1-9][0-9]* engine=320x200 aspect=4:3 software compositor',45)
  initial=frame(vm,timeout=45);vm.until(rb'DESKTOP GAME_FRAME[^\n]*sequence=[1-9][0-9]*',25)
  v=current_view(vm);report['initial']=initial;report['initial_view']=v;d.screenshot('doom-managed-initial');passed('desktop Terminal default Doom creates managed copied software window')
  if a.mode=='gameplay':
   # Doom's startup wipe draws frames before it advances simulation/input.
   deadline=time.monotonic()+30
   while initial['tic']<35:
    assert time.monotonic()<deadline
    initial=frame(vm)
   report['ready_for_input']=initial
   begin=time.monotonic();vm.qmp('input-send-event',{'events':[event('w',True)]})
   vm.until(rb'DOOM_INPUT hid=26 down=1',15);latency=time.monotonic()-begin
   assert latency<2,latency
   report['observed_key_latency_seconds']=latency
   time.sleep(.8);vm.qmp('input-send-event',{'events':[event('w',False)]});vm.until(rb'DOOM_INPUT hid=26 down=0',15)
   moved=frame(vm);CHECK_MOVE=(moved['x'],moved['y'])!=(initial['x'],initial['y']);assert CHECK_MOVE,(initial,moved);report['moved']=moved;passed('responsive keyboard movement and advancing rendered frames')
   # Focus loss releases held movement even when the release goes to Terminal.
   vm.qmp('input-send-event',{'events':[event('w',True)]});vm.until(rb'DOOM_INPUT hid=26 down=1',15)
   d.key('tab','alt');vm.until(rb'DOOM_INPUT focus=0 released-held-keys',20)
   vm.qmp('input-send-event',{'events':[event('w',False)]});d.key('tab','alt');vm.until(rb'DOOM_INPUT focus=1',20)
   # Doom retains world momentum after key release; inspect its actual
   # simulation command and decaying momentum instead of freezing position.
   detail=rb'DOOM_FRAME [^\n]*forward=0 side=0 momx=(-?[0-9]+) momy=(-?[0-9]+)\n'
   first=vm.until(detail,20);m1=tuple(map(int,re.findall(rb'momx=(-?[0-9]+) momy=(-?[0-9]+)',first.encode())[-1]))
   second=vm.until(detail,20);m2=tuple(map(int,re.findall(rb'momx=(-?[0-9]+) momy=(-?[0-9]+)',second.encode())[-1]))
   assert sum(abs(x) for x in m2)<=sum(abs(x) for x in m1),(m1,m2)
   report['focus_release']={'forward':0,'side':0,'momentum_before':m1,'momentum_after':m2}
   passed('focus transfer releases actual movement command and momentum decays')
   d.key('up','meta_l');d.expect('MAXIMIZE');large=changed_view(vm,v);assert large['width']>v['width'] or large['height']>v['height'],(v,large);report['maximized_view']=large;d.screenshot('doom-maximized-aspect')
   d.key('down','meta_l');d.expect('RESTORE');v=changed_view(vm,large)
   edge=(v['body_x']+v['body_w'],v['body_y']+v['body_h']);d.move(*edge);d.button(True);d.move(edge[0]+45,edge[1]+25);d.button(False);d.expect('GEOMETRY');resized=changed_view(vm,v);assert resized['body_w']!=v['body_w'] or resized['body_h']!=v['body_h'];report['resized_view']=resized;passed('maximize, restore and border resize preserve 4:3 viewport')
   v=resized;d.click(v['x']+v['width']//2,v['y']+v['height']//2);d.expect('GAME_CAPTURE','active')
   before=frame(vm);vm.qmp('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':24}}]});vm.until(rb'DOOM_INPUT mouse[^\n]*',20);turned=frame(vm);assert before['angle']!=turned['angle'],(before,turned);report['turned']=turned
   d.key('f10');d.expect('GAME_CAPTURE','released');passed('relative pointer capture, turn and explicit F10 release')
   fired=key(vm,'ctrl',hold=.7);assert re.search(r'DOOM_SFX start=[0-9]+ name=pistol\b',fired,re.I),fired
   shot=frame(vm);assert shot['ammo']<turned['ammo'],(shot,turned);passed('existing sound effects and firing retained in window')
  if a.mode!='silent':
   before=music_state(vm,0);report['music_before']=before
   key(vm,'pause',observe=False);p1=music_state(vm,1);p2=music_state(vm,1);assert p1['frames']==p2['frames'],(p1,p2)
   key(vm,'pause',observe=False);after=music_state(vm,0);assert after['frames']>p2['frames'];report['music_after']=after;passed('actual Freedoom score, music pause and resume in window')
  else:
   start=frame(vm);deadline=time.monotonic()+15
   while frame(vm)['tic']<start['tic']+70:assert time.monotonic()<deadline
  # Normal engine menu exit returns its Terminal's prompt and releases window.
  key(vm,'esc');frame(vm,menu=1);key(vm,'q');key(vm,'ret');key(vm,'y',observe=False)
  vm.until(rb'DOOM_EXIT[^\n]*returned to desktop[\s\S]*os:[^\n]*\$ ',30);passed('normal Doom menu exit restores Terminal while desktop stays active')
  if a.mode=='gameplay':
   vm.decode_terminal=False
   launch_offset=pathlib.Path(vm.log.name).stat().st_size;launched=d.launch('Doom')
   vm.until(rb'DESKTOP GAME_FRAME window='+str(launched).encode()+rb'[^\n]*sequence=[1-9]',45)
   vm.log.flush();assert re.search(rb'DOOM_READY window=[1-9]',pathlib.Path(vm.log.name).read_bytes()[launch_offset:])
   d.screenshot('doom-launched-from-desktop')
   d.key('f4','alt');vm.until(rb'DESKTOP GAME_EXIT pid=[0-9]+ status=0 reaped=1',25);passed('desktop launcher starts Doom and window close reaps cleanly')
   reopened=d.launch('Doom');vm.until(rb'DESKTOP GAME_FRAME window='+str(reopened).encode()+rb'[^\n]*sequence=[1-9]',45);d.key('f4','alt');vm.until(rb'DESKTOP GAME_EXIT pid=[0-9]+ status=0 reaped=1',25);passed('relaunch reuses released graphics/input/audio resources')
  # Game is closed; this desktop shortcut now exits the desktop normally.
  d.exit();assert_success(vm.command('echo DOOM_WINDOW_SHELL_RESTORED','DOOM_WINDOW_SHELL_RESTORED\n'))
  assert_success(vm.command('check --display-exit',timeout=20));info=vm.command('doom --info');assert_success(info);assert 'queued=0' in info,info
  vm.log.flush();report['serial_log']=str(vm.log.name);serial=pathlib.Path(vm.log.name).read_text(errors='replace')
  if a.mode!='silent':assert 'DOOM_MUSIC score hash='+score_hash() in decode_terminal_stream(pathlib.Path(vm.log.name).read_bytes(),vm.terminal_window).decode(errors='replace')
  if a.mode!='gameplay':assert 'DOOM_SFX start=' not in serial
  report['result']='passed'
 finally:
  if vm:vm.close()
  if wav.exists() and report['result']=='passed':
   try:report['hda_capture']=capture_check(wav) if a.mode=='gameplay' else capture(wav,a.mode=='music')
   except BaseException as error:
    report['result']='failed';report['capture_error']=str(error)
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n');raise
  (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
 print('Window Doom regression '+report['result']+' '+str(out/'report.json'),flush=True)
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--iso',type=pathlib.Path,required=True);p.add_argument('--label',required=True);p.add_argument('--output-root',type=pathlib.Path,default=ROOT/'build/continuation-20261015-phase3');p.add_argument('--mode',choices=['gameplay','music','silent'],default='gameplay');p.add_argument('--cpus',type=int,default=4);p.add_argument('--accel',choices=['kvm','tcg'],default='kvm');a=p.parse_args();a.iso=a.iso.resolve();exercise(a)
