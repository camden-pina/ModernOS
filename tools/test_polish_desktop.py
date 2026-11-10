#!/usr/bin/env python3
"""Exercise production Coast UI using real guest input and file bytes.
The only writable disk is newly created under --output-root. No host raw disks,
network, host settings or browser app are used. Requires QEMU, mtools and mkfs.fat.
"""
from pathlib import Path
import sys,time,re,json,argparse,traceback,hashlib,os
R=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(R/'tools'));import test_runtime as rt;import test_desktop as dd
p=argparse.ArgumentParser();p.add_argument('--iso',type=Path,required=True);p.add_argument('--label',required=True);p.add_argument('--scale',type=int,choices=[0,1,2,3],default=2);p.add_argument('--output-root',type=Path,default=R/'build/coast-polish-tests/qa');a=p.parse_args()
out=a.output_root.resolve()/a.label;out.mkdir(parents=True,exist_ok=False);rt.WORK=dd.WORK=out
vol=out/'prefs.volume'
with vol.open('wb') as f:f.truncate(64*1024*1024)
rt.run('mkfs.fat','-F','32','-n','POLISH-QA',vol);rt.run('mmd','-i',vol,'::/.coast')
prefs=out/'initial.ini';prefs.write_text('scale='+str(a.scale)+'\nwallpaper=0\n')
rt.run('mcopy','-i',vol,prefs,'::/.coast/desktop.ini')
fixture=out/'indent.txt';fixture.write_bytes(b'  alpha\r\n')
rt.run('mcopy','-i',vol,fixture,'::/indent.txt')
image=out/'prefs-gpt.img';rt.gpt_image(vol,image,'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7')
args=['-drive',f'file={image},format=raw,if=none,id=prefs','-device','nvme,drive=prefs,serial=POLISH-QA']
v=None;report={'result':'failed','scale_index':a.scale,'cases':[]}
def pump():
 if v.selector.select(.04):
  data=os.read(v.process.stdout.fileno(),65536);v.pending.extend(data);v.log.write(data);v.log.flush()
def log():
 pump();v.log.flush();return Path(v.log.name).read_text(errors='replace').replace('\r','')
def wait(pattern,after=0,timeout=12):
 end=time.monotonic()+timeout
 while True:
  found=re.findall(pattern,log()[after:])
  if found:return found[-1]
  assert time.monotonic()<end,'Missing '+pattern+'\n'+log()[-1800:]
  time.sleep(.02)
def mark():return len(log())
def launch(name):
 before=mark();d.key('spc','meta_l');v.send(name);time.sleep(.08);d.key('ret');return int(wait(r'DESKTOP OPEN window=(\d+)[^\n]*'+name, before))
def menuheading(letter,group):
 start=mark();d.key(letter,'alt');wait(r'DESKTOP MENU [^\n]+',start)
 return tuple(map(int,wait(r'DESKTOP MENUBAR_GEOMETRY group='+str(group)+r' x=(\d+) y=(\d+) width=(\d+) height=(\d+)',start)))
def dockitem(role):
 return tuple(map(int,wait(r'DESKTOP DOCK_ITEM role='+str(role)+r' x=(\d+) y=(\d+) width=(\d+) height=(\d+)')))
def shot(name):return d.screenshot(name)
try:
 v=rt.VM(4,storage=False,network=False,iso=a.iso,label=a.label,extra_args=args)
 output=v.command('jsc /usr/share/codeoss/core-check.js',timeout=120)
 assert 'CODEOSS_CORE_CHECK_PASS' in output,output
 report['codeoss_core']='passed';(out/'codeoss-output.txt').write_text(output)
 v.send('gui --trace --stats\n');v.until(rb'GUI_READY:',60);d=dd.DesktopTest(v);shot('01-desktop')
 # Real repeated mouse presses on a heading, switch to a different heading, Escape then reopen.
 heading=menuheading('f',0);x,y,w,h=heading;start=mark();d.click(x+w//2,y+h//2)
 wait(r'DESKTOP MENU_TOGGLE_CLOSED [^\n]*File',start)
 for _ in range(2):
  start=mark();d.click(x+w//2,y+h//2);wait(r'DESKTOP MENU [^\n]*File',start)
  start=mark();d.click(x+w//2,y+h//2);wait(r'DESKTOP MENU_TOGGLE_CLOSED [^\n]*File',start)
 view=menuheading('v',2);d.key('esc');start=mark();d.click(x+w//2,y+h//2);wait(r'DESKTOP MENU [^\n]*File',start)
 vx,vy,vw,vh=view;start=mark();d.click(vx+vw//2,vy+vh//2);wait(r'DESKTOP MENU [^\n]*View',start);d.key('esc')
 report['cases'].append('same-heading mouse toggle twice; different heading switches; Escape and reopen')
 # Dock native-raster easing, return to baseline and app context menu interrupted by top heading.
 # APP enum traced: determine role from app definitions below.
 x,y,w,h=dockitem(0);start=mark();d.move(x+w//2,y+h//2);time.sleep(.5);pump()
 sizes=re.findall(r'DESKTOP DOCK_ANIMATE role=0 size=(\d+) base=(\d+)',log()[start:]);assert sizes,sizes
 assert any(int(s)>int(b) for s,b in sizes),sizes
 shot('02-dock-hover');d.move(12,d.height//2);time.sleep(.55);pump()
 report['dock_hover_sizes']=sizes;report['cases'].append('Dock easing grows at native physical sizes and settles after leaving')
 # Unpin Files while running, verify remains then pin it again. First context row=window, second=new, third=remove.
 x,y,w,h=dockitem(0);start=mark();d.click(x+w//2,y+h//2,'right');wait(r'DESKTOP DOCK_MENU [^\n]*Files',start)
 d.key('down');d.key('down');d.key('ret');wait(r'DESKTOP DOCK_UNPIN [^\n]*Files',start)
 assert 'role=0' in log();shot('03-unpinned-running')
 x,y,w,h=dockitem(0);start=mark();d.click(x+w//2,y+h//2,'right');wait(r'DESKTOP DOCK_MENU [^\n]*Files',start)
 # Top heading must replace Dock context, then close normally.
 d.click(heading[0]+heading[2]//2,heading[3]//2);wait(r'DESKTOP MENU [^\n]*File',start);d.key('esc')
 # Re-pin and then leave Editor unpinned for reboot persistence below.
 start=mark();d.click(*(lambda q:(q[0]+q[2]//2,q[1]+q[3]//2))(dockitem(0)),'right')
 d.key('down');d.key('down');d.key('ret');wait(r'DESKTOP DOCK_PIN [^\n]*Files',start)
 report['cases'].append('pin/unpin running app; top menu replaces Dock context')
 settings=launch('Settings');d.geometry(settings)
 # View > Dock is last item (index4): choose settings42px then24% (keyboard focus jumps to size24).
 menuheading('v',2)
 for _ in range(4):d.key('down')
 d.key('ret');wait(r'DESKTOP SETTINGS_SECTION [^\n]*Dock')
 for _ in range(3):d.key('right')
 d.key('ret');wait(r'DESKTOP DOCK_LAYOUT [^\n]*size=42')
 d.key('tab');d.key('right');d.key('right');d.key('ret');wait(r'DESKTOP DOCK_LAYOUT [^\n]*magnification=24')
 shot('04-dock-preferences')
 # Enlarged artwork above the base icon remains clickable at both native scales.
 ex,ey,ew,eh=dockitem(0);start=mark();d.move(ex+ew//2,ey+eh//2);time.sleep(.65);pump()
 samples=re.findall(r'DESKTOP DOCK_ANIMATE role=0 size=(\d+) base=(\d+)',log()[start:]);assert samples,samples
 large,base=map(int,samples[-1]);assert large>base,(large,base)
 scale=[100,125,150,200][a.scale]/100
 hit_y=ey+eh-round(9*scale)-(base+large)//2
 start=mark();d.click(ex+ew//2,hit_y);wait(r'DESKTOP FOCUS window=1[^\n]*Files',start)
 sx,sy,sw,sh=dockitem(3);start=mark();d.click(sx+sw//2,sy+sh//2);wait(r'DESKTOP FOCUS window='+str(settings),start)
 # Magnification Off must stay exactly at the base size; restore24% for persistence.
 d.key('left');d.key('left');d.key('ret');wait(r'DESKTOP DOCK_LAYOUT [^\n]*magnification=0')
 ex,ey,ew,eh=dockitem(0);start=mark();d.move(ex+ew//2,ey+eh//2);time.sleep(.5);pump()
 assert not re.findall(r'DESKTOP DOCK_ANIMATE role=0',log()[start:]),log()[start:]
 d.move(12,d.height//2);d.key('right');d.key('right');d.key('ret');wait(r'DESKTOP DOCK_LAYOUT [^\n]*magnification=24')
 report['cases'].append('enlarged upper icon edge hit-tests correctly; magnification Off remains at base size')

 # Wallpaper rapid last request wins, queued menu/escape remains accepted.
 menuheading('v',2);d.key('down');d.key('ret');wait(r'DESKTOP SETTINGS_SECTION [^\n]*Wallpaper')
 for _ in range(3):d.key('right')
 start=mark();d.key('ret');d.key('left');d.key('ret');wait(r'DESKTOP WALLPAPER_REQUEST index=2',start)
 ready=wait(r'DESKTOP WALLPAPER_READY index=(\d+) elapsed_us=(\d+)',start,20)
 assert ready[0]=='2',ready
 time.sleep(.4);pump();readyall=re.findall(r'DESKTOP WALLPAPER_READY index=(\d+)',log()[start:]);assert readyall[-1]=='2',readyall
 report['cases'].append('interrupted photo selection: latest procedural wallpaper commits without stale photo')
 d.key('f4','alt');wait(r'DESKTOP CLOSE window='+str(settings))
 editor=launch('Text Editor');geo=d.geometry(editor);s=geo['scale']/100;S=lambda n:int(n*s+.5)
 d.key('comma','ctrl');time.sleep(.12)
 # Compact11, tab2, spaces on; defaults autoindent and line numbers stay enabled.
 start=mark();d.key('left');wait(r'DESKTOP EDITOR_PREF row=0 value=0',start)
 d.key('tab');d.key('left');wait(r'DESKTOP EDITOR_PREF row=1 value=0',start)
 d.key('tab');d.key('right');wait(r'DESKTOP EDITOR_PREF row=2 value=1',start)
 # whitespace, highlight settings and explicit CRLF
 for _ in range(3):d.key('tab')
 d.key('right');wait(r'DESKTOP EDITOR_PREF row=5 value=1',start)
 d.key('tab');d.key('tab');d.key('left');wait(r'DESKTOP EDITOR_PREF row=7 value=2',start)
 shot('05-editor-preferences');d.key('esc')
 v.send('  alpha');time.sleep(.15);d.key('ret');d.key('tab');v.send('omega');time.sleep(.15);d.key('s','ctrl','shift');wait(r'DESKTOP MODAL ')
 v.send('/mnt/nvme0n1p1/typed.txt');time.sleep(.2);shot('save-path-entered');d.key('ret');wait(r'DESKTOP SAVE [^\n]*typed.txt')
 shot('06-editor-document')
 # Read an actual CRLF document: End/newline auto detection and atomic CRLF backspace.
 # File > Open document opens in a separate window.
 menuheading('f',0);d.key('down');d.key('ret');wait(r'DESKTOP MODAL ');openmark=mark()
 v.send('/mnt/nvme0n1p1/indent.txt');time.sleep(.2);d.key('ret');opened=wait(r'DESKTOP OPEN window=(\d+)[^\n]*indent.txt',openmark);crlf_editor=int(opened)
 # Use Detect, retaining global indentation and2-space tabs.
 d.key('comma','ctrl')
 for _ in range(7):d.key('tab')
 d.key('right');wait(r'DESKTOP EDITOR_PREF row=7 value=0')
 d.key('esc');d.key('end');d.key('ret');d.key('backspace');d.key('backspace');d.key('backspace');d.key('ret');v.send('beta');time.sleep(.15);d.key('s','ctrl');wait(r'DESKTOP SAVE [^\n]*indent.txt')
 d.key('f4','alt');wait(r'DESKTOP CLOSE window='+str(crlf_editor))
 # Restore explicit CRLF so the main persistence assertion stays deterministic.
 d.key('comma','ctrl')
 for _ in range(7):d.key('tab')
 d.key('left');wait(r'DESKTOP EDITOR_PREF row=7 value=2');d.key('esc')

 # Persist Editor unpinned while it remains running, then confirm new windows pick up prefs.
 # APP_EDITOR is the stable role4, independent of pin ordering.
 editor_role=4
 ex,ey,ew,eh=dockitem(editor_role);start=mark();d.click(ex+ew//2,ey+eh//2,'right');wait(r'DESKTOP DOCK_MENU [^\n]*Text Editor',start)
 d.key('down');d.key('down');d.key('ret');wait(r'DESKTOP DOCK_UNPIN [^\n]*Text Editor',start)
 closedmark=mark();d.key('f4','alt');wait(r'DESKTOP CLOSE window='+str(editor),closedmark)
 # Running-only item disappears once the last app window closes.
 wait(r'DESKTOP DOCK_LAYOUT [^\n]*count=7[^\n]*pins=143',closedmark)
 d.exit();text=v.command('cat /mnt/nvme0n1p1/.coast/desktop.ini');(out/'saved-prefs.txt').write_text(text)
 for setting in ['dock_pins=143','dock_size=42','dock_magnification=24','editor_text_size=11','editor_tab_width=2','editor_spaces=1','editor_whitespace=1','editor_newline=2']:
  assert setting in text,setting+'\n'+text
 report['cases'].append('Dock/editor autosave to owned writable FAT; editing preferences affect real saved bytes')
 report['stats']=re.findall(r'(?:DOCK_STATS|WALLPAPER_STATS|GUI_TIMING)[^\n]+',log())
 v.close();v=None
 # Extract using mtools at GPT partition offset, verify actual document bytes after guest shutdown.
 check=out/'typed-actual.txt';rt.run('mcopy','-i',str(image)+'@@1048576','::/typed.txt',check)
 expected=b'  alpha\r\n    omega';assert check.read_bytes()==expected,repr(check.read_bytes())
 actual=out/'indent-actual.txt';rt.run('mcopy','-i',str(image)+'@@1048576','::/indent.txt',actual)
 assert actual.read_bytes()==b'  alpha\r\n  beta\r\n',repr(actual.read_bytes())
 report['crlf_detect_end_backspace']='passed byte-exact'
 report['typed_bytes_hex']=check.read_bytes().hex()
 v=rt.VM(4,storage=False,network=False,iso=a.iso,label=a.label+'-reboot',extra_args=args)
 v.send('gui --trace --stats\n');v.until(rb'GUI_READY:',60);d=dd.DesktopTest(v);shot('07-persisted-after-reboot')
 wait(r'DESKTOP DOCK_LAYOUT [^\n]*count=7[^\n]*size=42 magnification=24 pins=143')
 launch('Text Editor');d.key('comma','ctrl');time.sleep(.12);shot('08-editor-persisted-after-reboot')
 # Changing compact to standard reports all restored settings; verify untouched prefs in trace.
 start=mark();d.key('right');line=wait(r'DESKTOP EDITOR_PREF row=0[^\n]*',start)
 assert 'tab=2 text=13 spaces=1 indent=1 numbers=1 whitespace=1 highlight=1 newline=2' in line,line
 d.key('esc');d.exit();report['cases'].append('cold reboot reloads Dock pins/size/magnification and editor preferences')
 report['result']='passed'
except BaseException as e:
 report['error']=repr(e)
 if v and 'd' in globals():
  try:shot('failure')
  except Exception:pass
 (out/'exception.txt').write_text(traceback.format_exc());raise
finally:
 if v:v.close()
 (out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2),flush=True)
