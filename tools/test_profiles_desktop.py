#!/usr/bin/env python3
"""Verify category/profile/menu/cursor behavior with real guest input and file bytes.
Only newly created disposable images under --output-root are writable; no host raw
storage, network, host accounts or host settings are used.
"""
from pathlib import Path
import sys,time,re,json,argparse,traceback,hashlib,os
from PIL import Image,ImageChops
R=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(R/'tools'));import test_runtime as rt;import test_desktop as dd
p=argparse.ArgumentParser();p.add_argument('--iso',type=Path,required=True);p.add_argument('--label',required=True);p.add_argument('--scale',type=int,choices=[0,1,2,3],default=2);p.add_argument('--output-root',type=Path,default=R/'build/coast-profiles-tests/qa');a=p.parse_args()
out=a.output_root.resolve()/a.label;out.mkdir(parents=True,exist_ok=False);rt.WORK=dd.WORK=out
vol=out/'profiles.volume'
with vol.open('wb') as f:f.truncate(64*1024*1024)
rt.run('mkfs.fat','-F','32','-n','PROFILES-QA',vol);rt.run('mmd','-i',vol,'::/.coast')
legacy=out/'legacy.ini';legacy.write_text('scale='+str(a.scale)+'\nwallpaper=0\ndock_size=30\neditor_tab_width=4\ndark=0\n');rt.run('mcopy','-i',vol,legacy,'::/.coast/desktop.ini')
image=out/'profiles-gpt.img';rt.gpt_image(vol,image,'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7')
extra=['-drive',f'file={image},format=raw,if=none,id=prefs','-device','nvme,drive=prefs,serial=PROFILE-QA']
v=None;r={'result':'failed','scale_index':a.scale,'cases':[]};work_scale=0 if a.scale==3 else 3

def pump():
 if v.selector.select(.03):
  data=os.read(v.process.stdout.fileno(),65536);v.pending.extend(data);v.log.write(data);v.log.flush()
def log():pump();return Path(v.log.name).read_text(errors='replace').replace('\r','')
def mark():return len(log())
def wait(pattern,after=0,timeout=20):
 end=time.monotonic()+timeout
 while True:
  found=re.findall(pattern,log()[after:])
  if found:return found[-1]
  assert time.monotonic()<end,'Missing '+pattern+'\n'+log()[-1500:]
  time.sleep(.02)
def key(*args):d.key(*args)
def launch(name):
 start=mark();key('spc','meta_l');v.send(name);time.sleep(.12);key('ret');return int(wait(r'DESKTOP OPEN window=(\d+)[^\n]*'+name,start))
def dock(role):
 x,y,w,h=map(int,wait(r'DESKTOP DOCK_ITEM role='+str(role)+r' x=(\d+) y=(\d+) width=(\d+) height=(\d+)'));d.click(x+w//2,y+h//2)
def section(n):
 start=mark();key(str(n),'ctrl');return tuple(map(int,wait(r'DESKTOP SETTINGS_PANEL page='+str(n-1)+r' x=(\d+) y=(\d+) width=(\d+) height=(\d+) scale=(\d+)',start)))
def heading(letter,group):
 start=mark();key(letter,'alt');value=wait(r'DESKTOP MENUBAR_GEOMETRY group='+str(group)+r' x=(\d+) y=(\d+) width=(\d+) height=(\d+)',start);return tuple(map(int,value))
def screenshot(name):return d.screenshot(name)
def fresh(name):
 time.sleep(.7);d.screenshot(name+'-cached');key('f12','ctrl','alt');wait(r'DESKTOP GLASS_RECOMPOSE');d.screenshot(name+'-fresh')
 first=Image.open(out/(name+'-cached.png'));second=Image.open(out/(name+'-fresh.png'));top=round(22*d.geometry(editor if 'editor' in globals() else 1)['scale']/100)
 delta=ImageChops.difference(first.crop((0,top,d.width,d.height)),second.crop((0,top,d.width,d.height)))
 box=delta.getbbox()
 if box:delta.save(out/(name+'-difference.png'))
 assert box is None,name+' stale pixels: '+str(box)
 r['cases'].append(name+': exact cached/full repaint equivalence')
def profile_switch(target):
 panel=section(7);x,y,w,h,scale=panel;S=lambda n:(n*scale+50)//100
 d.click(x+S(80),y+S(108+target*38+16));start=mark()
 if S(140+2*38+30)>h:
  for _ in range(4-target):key('tab')
  key('ret')
 else:d.click(x+S(22)+2*((w-S(44))//3)+((w-S(44))//3-S(7))//2,y+S(140+2*38+15))
 wait(r'DESKTOP PROFILE_SWITCH [^\n]*'+('default' if target==0 else 'profile-1'),start)
try:
 v=rt.VM(4,storage=False,network=False,iso=a.iso,label=a.label,extra_args=extra);v.send('gui --trace --stats\n');v.until(rb'GUI_READY:',60);d=dd.DesktopTest(v);screenshot('01-initial')
 # No open menu: hovering alone must stay closed. Open menu: hover another heading switches.
 f=heading('f',0);key('esc');view=heading('v',2);key('esc');start=mark();d.move(f[0]+f[2]//2,f[3]//2);time.sleep(.2);assert not re.findall(r'DESKTOP MENU ',log()[start:])
 start=mark();d.click(f[0]+f[2]//2,f[3]//2);wait(r'DESKTOP MENU [^\n]*File',start)
 for _ in range(3):
  start=mark();d.move(view[0]+view[2]//2,view[3]//2);wait(r'DESKTOP MENU_HOVER_SWITCH [^\n]*View',start)
  start=mark();d.move(f[0]+f[2]//2,f[3]//2);wait(r'DESKTOP MENU_HOVER_SWITCH [^\n]*File',start)
 start=mark();d.click(f[0]+f[2]//2,f[3]//2);wait(r'DESKTOP MENU_TOGGLE_CLOSED',start)
 start=mark();d.move(view[0]+view[2]//2,view[3]//2);time.sleep(.15);assert not re.findall(r'DESKTOP MENU ',log()[start:])
 key('f','alt');key('right');wait(r'DESKTOP MENU [^\n]*Edit');key('esc')
 r['cases'].append('passive closed hover; open hover switches3cycles; same heading click closes; keyboard navigation/dismissal')
 settings=launch('Settings');d.geometry(settings)
 for page in range(1,8):section(page);screenshot('category-'+str(page))
 # Test real popup item hit after hover switching into Settings View.
 f=heading('f',0);key('esc');view=heading('v',2);key('esc');d.click(f[0]+f[2]//2,f[3]//2);start=mark();d.move(view[0]+view[2]//2,view[3]//2);wait(r'DESKTOP MENU_HOVER_SWITCH [^\n]*View',start)
 scale=d.geometry(settings)['scale'];S=lambda n:(n*scale+50)//100;start=mark();d.click(view[0]+S(70),S(22+6+2*24+12));wait(r'DESKTOP SETTINGS_SECTION [^\n]*Dock',start)
 key('ret');wait(r'DESKTOP DOCK_LAYOUT [^\n]*size=24')
 editor=launch('Text Editor');key('comma','ctrl');wait(r'DESKTOP EDITOR_PREFERENCES');key('tab');key('left');wait(r'DESKTOP EDITOR_PREF row=1 value=0');key('esc');dock(3)
 # Create Work from current preferences; generated directory IDs do not depend on name.
 section(7);key('tab');key('ret');wait(r'DESKTOP MODAL ');v.send('Work');time.sleep(.2);start=mark();key('ret');wait(r'DESKTOP PROFILE_CREATE [^\n]*profile-1',start)
 section(3)
 for _ in range(3):key('right')
 key('ret');wait(r'DESKTOP DOCK_LAYOUT [^\n]*size=42')
 section(1);key('right');key('ret')
 section(2);key('right');key('right');start=mark();key('ret');wait(r'DESKTOP WALLPAPER_READY index=2',start)
 dock(4);key('comma','ctrl');key('tab');key('right');key('right');wait(r'DESKTOP EDITOR_PREF row=1 value=2');
 # Reproduce the old artifact interaction; remain inside the same editor window.
 geo=d.geometry(editor);scale=geo['scale'];S=lambda n:(n*scale+50)//100;bx,by,bw,bh=geo['x'],geo['y']+geo['body'],geo['width'],geo['height']-geo['body'];width=min(S(100),(bw-S(225))//3)
 d.click(bx+S(204)+width+S(6)+width//2,by+S(65+3*38)+S(13));d.move(bx+bw-S(35),by+bh-S(45));fresh('editor-hover-reproduction-fixed')
 key('esc');dock(3)
 section(4)
 for _ in range(work_scale):key('right')
 key('ret');wait(r'DESKTOP SCALE ')
 # First/last sidebar categories remain visible and mouse-selectable in short HiDPI windows.
 geo=d.geometry(settings);scale=geo['scale'];S=lambda n:(n*scale+50)//100;body_y=geo['y']+geo['body'];body_h=geo['height']-geo['body'];step=max(S(20),min(S(36),(body_h-S(122))//7));height=min(S(30),step-S(2))
 for page in [0,6]:
  start=mark();d.click(geo['x']+S(65),body_y+S(58)+page*step+height//2);wait(r'DESKTOP SETTINGS_SECTION [^\n]*'+('Appearance' if page==0 else 'Profiles'),start)
 screenshot('sidebar-after-dpi-change')
 # Rename active Work to Studio through a display-name-only dialog.
 section(7);key('tab');key('tab');key('tab');key('ret');wait(r'DESKTOP MODAL ');key('a','ctrl');v.send('Studio');time.sleep(.2);start=mark();key('ret');wait(r'DESKTOP PROFILE_RENAME',start);screenshot('02-profiles-studio')
 profile_switch(0);assert d.geometry(settings)['scale']==[100,125,150,200][a.scale]
 wait(r'DESKTOP DOCK_LAYOUT [^\n]*size=24');dock(4);key('comma','ctrl');start=mark();key('left');line=wait(r'DESKTOP EDITOR_PREF row=0[^\n]*',start);assert 'tab=2' in line,line;key('esc');dock(3)
 profile_switch(1);assert d.geometry(settings)['scale']==[100,125,150,200][work_scale];wait(r'DESKTOP DOCK_LAYOUT [^\n]*size=42')
 # Cursor/hover stress across clipped edges, menus, Dock and background changes.
 d.move(d.width-1,0);d.move(0,d.height-1);d.move(d.width-1,d.height-1);d.move(d.width//2,d.height//2);fresh('screen-edge-cursor')
 heading('v',2);d.move(d.width//2,d.height//2);fresh('popup-cursor');key('esc')
 for role in [0,3,4,0]:dock(role)
 d.move(d.width-30,d.height//2);fresh('dock-cursor-return')
 dock(3);section(2)
 for _ in range(3):key('right')
 key('ret');key('left');key('left');start=mark();key('ret')
 for i in range(25):d.move(10+(i*97)%(d.width-50),40+(i*53)%(d.height-100),delay=.006)
 wait(r'DESKTOP WALLPAPER_READY index=1',start);fresh('asynchronous-wallpaper-cursor')
 section(2);key('right');key('right');start=mark();key('ret');wait(r'DESKTOP WALLPAPER_READY index=2',start)
 key('s','ctrl');wait(r'DESKTOP MODAL ');key('esc');screenshot('03-final-studio');d.exit()
 registry=v.command('cat /mnt/nvme0n1p1/.coast/profiles.ini');default=v.command('cat /mnt/nvme0n1p1/.coast/profiles/default/desktop.ini');studio=v.command('cat /mnt/nvme0n1p1/.coast/profiles/profile-1/desktop.ini')
 for field in ['active=profile-1','default=Default','profile-1=Studio']:assert field in registry,registry
 for field in ['dock_size=24','editor_tab_width=2','dark=0','wallpaper=0','scale='+str(a.scale)]:assert field in default,field+'\n'+default
 for field in ['dock_size=42','editor_tab_width=8','dark=1','wallpaper=2','scale='+str(work_scale)]:assert field in studio,field+'\n'+studio
 for name,value in [('registry',registry),('default',default),('studio',studio)]: (out/(name+'.txt')).write_text(value)
 r['cases'].append('legacy migration; profile create/rename/switch; distinct Dock/editor/theme/wallpaper/DPI preferences; active registry')
 v.close();v=None
 old=out/'legacy-after.ini';rt.run('mcopy','-i',str(image)+'@@1048576','::/.coast/desktop.ini',old);assert old.read_bytes()==legacy.read_bytes();r['legacy_original_unchanged']=True
 v=rt.VM(4,storage=False,network=False,iso=a.iso,label=a.label+'-reboot',extra_args=extra);v.send('gui --trace --stats\n');v.until(rb'GUI_READY:',60);d=dd.DesktopTest(v);screenshot('04-reboot-studio');wait(r'DESKTOP PROFILE_ACTIVE id=profile-1 name=Studio');assert d.geometry(1)['scale']==[100,125,150,200][work_scale]
 settings=launch('Settings');profile_switch(0);wait(r'DESKTOP PROFILE_ACTIVE id=default name=Default');screenshot('05-reboot-switch-default');d.exit();r['cases'].append('cold reboot restores active Studio and switch back restores Default')
 r['result']='passed'
except BaseException as e:
 r['error']=repr(e);(out/'exception.txt').write_text(traceback.format_exc())
 if v and 'd' in globals():
  try:d.screenshot('failure')
  except Exception:pass
 raise
finally:
 if v:v.close()
 (out/'report.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2))
