#!/usr/bin/env python3
"""Real guest input + checked file bytes; only fresh project-owned test storage."""
from pathlib import Path
import argparse,sys,os,time,re,json,traceback
from PIL import Image,ImageChops
R=Path(__file__).resolve().parents[1]
# --source-root supports running a staged test against the actual OS tool library.
p=argparse.ArgumentParser();p.add_argument('--source-root',type=Path,default=R);p.add_argument('--iso',type=Path,required=True);p.add_argument('--output-root',type=Path,required=True);p.add_argument('--label',required=True);p.add_argument('--scale',type=int,choices=[0,1,2,3],default=0);a=p.parse_args();R=a.source_root.resolve();sys.path.insert(0,str(R/'tools'));import test_runtime as rt;import test_desktop as dd
out=a.output_root.resolve()/a.label;out.mkdir(parents=True,exist_ok=False);rt.WORK=dd.WORK=out
volume=out/'preferences.volume'
with volume.open('wb') as f:f.truncate(64*1024*1024)
rt.run('mkfs.fat','-F','32','-n','PERSONAL-QA',volume);rt.run('mmd','-i',volume,'::/.coast')
legacy=out/'legacy.ini';legacy.write_text('scale='+str(a.scale)+'\nwallpaper=0\ndock_size=30\ndock_magnification=16\neditor_tab_width=4\ndark=0\n');rt.run('mcopy','-i',volume,legacy,'::/.coast/desktop.ini')
image=out/'personal-gpt.img';rt.gpt_image(volume,image,'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7');extra=['-drive',f'file={image},format=raw,if=none,id=prefs','-device','nvme,drive=prefs,serial=PERSONAL-QA']
r={'result':'running','cases':[],'scale_index':a.scale};v=None

def record(case):r['cases'].append(case);(out/'progress.json').write_text(json.dumps(r,indent=2)+'\n');print(case,flush=True)
def pump():
 if v.selector.select(.02):
  data=os.read(v.process.stdout.fileno(),65536);v.pending.extend(data);v.log.write(data);v.log.flush()
def log():pump();return Path(v.log.name).read_text(errors='replace').replace('\r','')
def mark():return len(log())
def wait(pattern,after=0,timeout=20):
 end=time.monotonic()+timeout
 while True:
  found=re.findall(pattern,log()[after:])
  if found:return found[-1]
  assert time.monotonic()<end,'Missing '+pattern+'\n'+log()[-1800:]
  time.sleep(.02)
def key(*args):d.key(*args)
def launch(name):
 start=mark();key('spc','meta_l');v.send(name);time.sleep(.1);key('ret');return int(wait(r'DESKTOP OPEN window=(\d+)[^\n]*'+re.escape(name),start))
def section(n):
 start=mark();key(str(n),'ctrl');return tuple(map(int,wait(r'DESKTOP SETTINGS_PANEL page='+str(n-1)+r' x=(\d+) y=(\d+) width=(\d+) height=(\d+) scale=(\d+)',start)))
def focus_settings():
 # Existing windows are focused through the Dock; never create a replacement for a hit-test assertion.
 values=re.findall(r'DESKTOP DOCK_ITEM role=3 x=(\d+) y=(\d+) width=(\d+) height=(\d+)',log());x,y,w,h=map(int,values[-1]);d.click(x+w//2,y+h//2)
def heading(letter,group):
 start=mark();key(letter,'alt');return tuple(map(int,wait(r'DESKTOP MENUBAR_GEOMETRY group='+str(group)+r' x=(\d+) y=(\d+) width=(\d+) height=(\d+)',start)))
def fresh(name):
 time.sleep(.8);d.screenshot(name+'-cached');start=mark();key('f12','ctrl','alt');wait(r'DESKTOP GLASS_RECOMPOSE',start);d.screenshot(name+'-fresh')
 aa=Image.open(out/(name+'-cached.png')).crop((0,S(22),d.width,d.height));bb=Image.open(out/(name+'-fresh.png')).crop((0,S(22),d.width,d.height));delta=ImageChops.difference(aa,bb);box=delta.getbbox();pixels=sum(p!=(0,0,0) for p in delta.get_flattened_data())
 if box:delta.save(out/(name+'-difference.png'))
 assert not box,name+' stale '+str(pixels)+' '+str(box)
 record(name+': zero cached/full repaint differences')
def wheel(up):d.button(True,'wheel-up' if up else 'wheel-down');d.button(False,'wheel-up' if up else 'wheel-down')
def profile_switch(target):
 section(1)
 for _ in range(target):key('tab')
 key('ret')
 for _ in range(4-target):key('tab')
 start=mark();key('ret');wait(r'DESKTOP PROFILE_SWITCH [^\n]*'+('default' if target==0 else 'profile-1'),start)
def slider_key(id,code,expected,*mods):
 start=mark();key(code,*mods);wait(r'DESKTOP SETTINGS_SLIDER id='+str(id)+r' value='+str(expected)+r'\b',start)
try:
 v=rt.VM(4,storage=False,network=False,iso=a.iso,label=a.label,extra_args=extra)
 r['bash_version_identity']=v.shell_command("linux-bash -c 'printf \"BASH=%s UID=%s EUID=%s\\n\" \"$BASH_VERSION\" \"$UID\" \"$EUID\"'")
 assert '5.3.20' in r['bash_version_identity'];record('existing genuine Bash version and identity query executed')
 v.send('gui --trace --stats\n');v.until(rb'GUI_READY:',60);d=dd.DesktopTest(v);d.screenshot('01-desktop');scale=[100,125,150,200][a.scale];S=lambda n:(n*scale+50)//100
 # Reproduce the exact standalone Super launcher, then transition keyboard/pointer/search/launch.
 key('meta_l');x,y,w,h,visible,_=map(int,wait(r'LAUNCHER x=(\d+) y=(\d+) width=(\d+) height=(\d+) visible=(\d+) scale=(\d+)'))
 for row in [1,3,5,2,4,0,visible-1]:d.move(x+w//2,y+S(52)+row*S(28)+S(13),delay=.05)
 fresh('02-launcher-hover');key('up');key('home');key('down');fresh('03-launcher-keyboard');d.move(x+w//2+1,y+S(65));fresh('04-launcher-pointer-return');key('esc');fresh('05-launcher-dismiss')
 start=mark();key('meta_l');lx,ly,lw,lh,visible,_=map(int,wait(r'LAUNCHER x=(\d+) y=(\d+) width=(\d+) height=(\d+) visible=(\d+) scale=(\d+)',start))
 d.move(lx+lw//2,ly+S(52)+S(28)+S(13));start=mark();d.click(lx+lw//2,ly+S(52)+3*S(28)+S(13));settings=int(wait(r'DESKTOP OPEN window=(\d+)[^\n]*Settings',start));record('standalone Super pointer selection launches the clicked application')
 geo=d.geometry(settings);assert geo['scale']==scale
 d.screenshot('06-profiles-default')
 # Existing menu behavior remains correct while open, closed and interrupted.
 f=heading('f',0);key('esc');view=heading('v',2);key('esc');start=mark();d.move(f[0]+f[2]//2,f[3]//2);time.sleep(.1);assert 'DESKTOP MENU ' not in log()[start:]
 d.click(f[0]+f[2]//2,f[3]//2)
 for _ in range(2):
  start=mark();d.move(view[0]+view[2]//2,view[3]//2);wait(r'DESKTOP MENU_HOVER_SWITCH [^\n]*View',start)
  start=mark();d.move(f[0]+f[2]//2,f[3]//2);wait(r'DESKTOP MENU_HOVER_SWITCH [^\n]*Preferences',start)
 start=mark();d.click(f[0]+f[2]//2,f[3]//2);wait(r'DESKTOP MENU_TOGGLE_CLOSED',start);key('f','alt');key('right');key('esc');record('closed hover stays closed; open menu hover-switch/toggle/keyboard regression')
 for n in range(1,9):section(n)
 # Original avatar, description and new-profile creation through real UI.
 x,y,w,h,_=section(1);d.click(x+S(22+2*56+22),y+S(236));d.click(x+S(110),y+S(285));wait(r'DESKTOP MODAL ');key('a','ctrl');v.send('My focused desktop');time.sleep(.3);key('ret');time.sleep(.2)
 section(1);key('tab');key('ret');wait(r'DESKTOP MODAL ');v.send('Studio');time.sleep(.3);start=mark();key('ret');wait(r'DESKTOP PROFILE_CREATE [^\n]*profile-1',start);record('avatar/description/default migration and profile creation')
 if a.scale==2:
  section(2);key('right');key('ret')
 # Wheel controls adjust in notches even when the configured scroll multiplier is high.
 x,y,w,h,_=section(6)
 for _ in range(5):key('tab')
 slider_key(410,'end',10);d.move(x+S(90),y+S(378));start=mark();wheel(False);wait(r'SETTINGS_SLIDER id=410 value=9',start);slider_key(410,'home',1);record('wheel slider remains one step per notch at maximum scroll multiplier')
 # Finely stepped slider keyboard/wheel/drag/capture bounds; preserve final Studio=64/75.
 x,y,w,h,_=section(4);slider_key(414,'end',112);slider_key(414,'home',16);slider_key(414,'right',17);slider_key(414,'right',27,'shift')
 barx=x+S(22+9);barw=w-S(44+96);bary=y+S(126)
 d.move(barx+barw//2,bary);wheel(True);wait(r'SETTINGS_SLIDER id=414 value=28')
 d.button(True);start=mark();d.move(0,bary);wait(r'SETTINGS_SLIDER id=414 value=16',start);start=mark();d.move(d.width-1,bary);wait(r'SETTINGS_SLIDER id=414 value=112',start);d.button(False);slider_key(414,'home',16)
 d.move(barx,bary);d.button(True);start=mark();d.move(barx+barw,bary);wait(r'SETTINGS_SLIDER id=414 value=112',start);start=mark();key('esc');wait(r'SETTINGS_SLIDER id=414 value=16',start);d.button(False)
 slider_key(414,'end',112);key('tab');slider_key(415,'end',100);d.move(d.width//2,d.height-S(20));time.sleep(.5);d.screenshot('07-dock-maximum');d.move(d.width//2,S(40));fresh('08-dock-maximum-return')
 # Return to a useful large but fitting Dock size with keyboard precision.
 section(4);slider_key(414,'home',16)
 for _ in range(4):key('right','shift')
 for _ in range(8):key('right')
 wait(r'SETTINGS_SLIDER id=414 value=64');key('tab');slider_key(415,'home',0)
 for _ in range(7):key('right','shift')
 for _ in range(5):key('right')
 wait(r'SETTINGS_SLIDER id=415 value=75');time.sleep(.4);record('slider drag/outside capture/Escape/wheel/arrows/Home/End; Dock range112/100')
 # Change Studio avatar, description and terminal preferences.
 x,y,w,h,_=section(1);d.click(x+S(22+5*56+22),y+S(236));d.click(x+S(100),y+S(285));wait(r'DESKTOP MODAL ');key('a','ctrl');v.send('Studio terminal');time.sleep(.3);key('ret')
 section(8);key('tab');key('ret') # Midnight palette.
 section(8)
 for _ in range(20):key('tab')
 key('ret') # genuine Bash.
 section(8)
 for _ in range(23):key('tab')
 slider_key(416,'end',19);slider_key(416,'left',17);slider_key(416,'left',15);key('tab');slider_key(417,'home',40);slider_key(417,'right',50,'shift');slider_key(417,'right',60,'shift');time.sleep(.4);d.screenshot('09-terminal-preferences');record('Terminal Midnight/Bash/native15px/live60% opacity saved per profile')
 terminal=launch('Terminal');wait(r'DESKTOP TERMINAL_SHELL [^\n]*/usr/bin/linux-bash');start=mark();v.send('printf "PERSONAL_BASH=%s\\n" "$BASH_VERSION"\n');wait(r'DESKTOP TERMINAL_OUTPUT [^\n]*PERSONAL_BASH=5.3.20',start)
 start=mark();v.send('printf "bash round trip\\n" > /home/user/personal-bash.txt\n');wait(r'DESKTOP TERMINAL_COMMAND [^\n]*personal-bash.txt',start);time.sleep(.4);d.screenshot('10-terminal-glass');d.move(d.width-5,S(35));fresh('11-terminal-glass')
 focus_settings();section(8)
 for _ in range(19):key('tab')
 key('ret');native_terminal=launch('Terminal');wait(r'DESKTOP TERMINAL_SHELL [^\n]*/bin/coast-sh')
 start=mark();v.send('nano /home/user/personal-nano.txt\n');wait(r'DESKTOP TERMINAL_MODE [^\n]*raw',start,30);v.send('native font raw PTY');time.sleep(.3);key('o','ctrl');key('ret');start=mark();key('x','ctrl');wait(r'DESKTOP TERMINAL_MODE [^\n]*canonical',start,30);record('actual Bash GUI commands/file write; native shell GNU nano raw/canonical PTY editing')
 focus_settings();section(8)
 for _ in range(20):key('tab')
 key('ret')
 focus_settings();profile_switch(0);wait(r'DESKTOP DOCK_LAYOUT [^\n]*size=30 magnification=16');d.screenshot('12-restored-default');profile_switch(1);wait(r'DESKTOP DOCK_LAYOUT [^\n]*size=64 magnification=75');d.screenshot('13-restored-studio');d.exit()
 files={}
 for name,path in [('registry','profiles.ini'),('default','profiles/default/desktop.ini'),('studio','profiles/profile-1/desktop.ini')]:
  files[name]=v.command('cat /mnt/nvme0n1p1/.coast/'+path);(out/(name+'.txt')).write_text(files[name])
 for field in ['profile_avatar=2','profile_tagline=My focused desktop','dock_size=30','terminal_palette=0','terminal_shell=0','terminal_opacity=100','terminal_text_size=11']:assert field in files['default'],field+'\n'+files['default']
 for field in ['profile_avatar=5','profile_tagline=Studio terminal','dock_size=64','dock_magnification=75','terminal_palette=1','terminal_shell=1','terminal_opacity=60','terminal_text_size=15']:assert field in files['studio'],field+'\n'+files['studio']
 if a.scale==2:assert 'dark=1' in files['studio'] and 'dark=0' in files['default']
 assert 'active=profile-1' in files['registry'];assert 'Studio' in files['registry']
 assert 'bash round trip' in v.command('cat /home/user/personal-bash.txt');assert 'native font raw PTY' in v.command('cat /home/user/personal-nano.txt');record('checked distinct profile file bytes and real Bash/nano saved bytes')
 v.close();v=None;copied=out/'legacy-after.ini';rt.run('mcopy','-i',str(image)+'@@1048576','::/.coast/desktop.ini',copied);assert copied.read_bytes()==legacy.read_bytes()
 v=rt.VM(4,storage=False,network=False,iso=a.iso,label=a.label+'-reboot',extra_args=extra);v.send('gui --trace --stats\n');v.until(rb'GUI_READY:',60);d=dd.DesktopTest(v);d.screenshot('14-reboot-studio');wait(r'DESKTOP PROFILE_ACTIVE id=profile-1 name=Studio');wait(r'DESKTOP DOCK_LAYOUT [^\n]*size=64 magnification=75');settings=launch('Settings');profile_switch(0);d.screenshot('15-reboot-default');wait(r'DESKTOP DOCK_LAYOUT [^\n]*size=30 magnification=16');d.exit();record('cold reboot and profile switching restore saved preferences; original legacy bytes unchanged');r['result']='passed'
except BaseException as e:
 r['result']='failed';r['error']=repr(e);(out/'exception.txt').write_text(traceback.format_exc())
 if v and 'd' in globals():
  try:d.screenshot('failure')
  except Exception:pass
 raise
finally:
 if v:v.close()
 (out/'report.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2),flush=True)
