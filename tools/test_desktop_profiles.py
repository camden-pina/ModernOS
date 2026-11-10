#!/usr/bin/env python3
"""Exercise profile passwords, roles, private homes and logout through the UI."""
import argparse,json,os,re,time,traceback
from pathlib import Path
import test_runtime as runtime
import test_desktop as desktop
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--iso',type=Path,required=True);p.add_argument('--output-root',type=Path,required=True);p.add_argument('--qt',action='store_true');a=p.parse_args()
out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=False);runtime.WORK=desktop.WORK=out
report=dict(state='failed',checks=[]);v=d=None;guest_password='Guest-QA-only-2026!'
def raw():
 while v.selector.select(.002):
  b=os.read(v.process.stdout.fileno(),65536)
  if not b:break
  v.pending.extend(b);v.log.write(b);v.log.flush()
 return Path(v.log.name).read_text(errors='replace')
def observed(start):
 s=raw()[start:];s=re.sub(r'DESKTOP TERMINAL_OUTPUT window=\d+ workspace=\d+ ','',s)
 return re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]','',s).replace('\r','')
def wait(pattern,start=0,timeout=30):
 end=time.monotonic()+timeout
 while time.monotonic()<end:
  s=observed(start);assert 'PANIC:' not in s
  m=re.search(pattern,s)
  if m:return m
  time.sleep(.02)
 raise AssertionError('Missing '+pattern+'\n'+observed(start)[-2500:])
def passed(s):report['checks'].append(s);print('PASS '+s,flush=True)
def command(text,expected):
 start=len(raw());v.send(text+'\n');wait(expected,start);time.sleep(.15)
def control(target,count,active,selected=None):
 d.key('1','ctrl');time.sleep(.1)
 if selected is None:selected=active
 if selected!=active:
  for _ in range(selected):d.key('tab')
  d.key('ret')
 items=list(range(300,300+count))+[310,311]+([312] if selected!=active else[])+list(range(320,327))+list(range(330,335))+([335,336,337] if selected==active else[])
 current=300+selected if selected!=active else 300
 for _ in range(items.index(target)-items.index(current)):d.key('tab')
 d.key('ret');time.sleep(.15)
def secret(text):v.send(text+'\n');time.sleep(.5);raw()
try:
 v=runtime.VM(2,storage=False,network=False,memory='16G',accel='kvm',iso=a.iso,label='profiles')
 assert '\n1000\n' in v.raw_command('id -u')
 assert 'Permission denied' in v.raw_command('touch /etc/profile-bypass')
 v.user_command('mkdir -p /home/user/.config /home/user/Projects')
 v.user_command('write /home/user/private-owner.txt OWNER_PRIVATE')
 v.send('gui --trace\n');v.until(b'GUI_READY:',60);d=desktop.DesktopTest(v);d.screenshot('00-desktop')
 terminal=d.launch('Terminal');command('id',r'uid=1000\(user\)')
 start=len(raw());v.send('sudo -S -v\n');wait(r'\[sudo\] Password:',start);v.send(v.qa_password+'\n');time.sleep(.5)
 command('sudo -n id',r'uid=0\(root\)')
 settings=d.launch('Settings');d.key('up','meta_l');d.screenshot('01-profiles-first')
 start=len(raw());control(310,1,0);v.send('Guest\n');wait('DESKTOP PROFILE_CREATE[^\n]*profile-1',start)
 assert re.search(r'DESKTOP CLOSE window='+str(terminal)+r'\b',raw()[start:])
 d.screenshot('02-standard-profile');passed('Settings opens to Profiles; creating a standard profile closes the old terminal and switches to its private home')
 if a.qt:
  start=len(raw());d.launch('Qt 6 Demo');wait('QT6_READY version=6.8.3 uid=1001',start);wait('QT6_PRESENT',start);d.screenshot('02-qt6-standard-profile');d.key('f4','alt');wait(r'DESKTOP GAME_EXIT pid=\d+ status=0 reaped=1',start)
  passed('The Qt app runs and renders with the standard profile UID 1001')
 d.launch('Terminal');command('id',r'uid=1001\(profile-1\)')
 command('touch /etc/profile-bypass',r'Permission denied')
 command('cat /home/user/private-owner.txt',r'Permission denied')
 command('sudo -n id',r'administrator access and a profile password are required')
 command('printf PROFILE_PRIVATE > /home/profile-1/own.txt; cat /home/profile-1/own.txt',r'\nPROFILE_PRIVATE')
 d.launch('Settings');d.key('up','meta_l');control(330,2,1);secret(guest_password);secret(guest_password)
 wait('ACCOUNT_POLICY uid=1001 admin=0 password_set=1')
 start=len(raw());control(331,2,1);secret(guest_password);wait('DESKTOP ERROR[^\n]*Update account',start);d.key('ret')
 passed('Standard profile cannot read the owner home, write system files, run sudo or promote itself; it can save its own files and password')
 start=len(raw());control(312,2,1,0);secret('wrong-password');wait('DESKTOP ERROR[^\n]*Switch profile',start);d.key('ret')
 start=len(raw());control(312,2,1,0);secret(v.qa_password);wait('DESKTOP PROFILE_SWITCH[^\n]*default',start)
 start=len(raw());control(331,2,0,1);secret(v.qa_password);wait('ACCOUNT_POLICY uid=1001 admin=1 password_set=1',start)
 start=len(raw());control(332,2,0,1);secret(v.qa_password);wait('ACCOUNT_POLICY uid=1001 admin=1 password_set=1 timeout=0',start)
 start=len(raw());control(312,2,0,1);secret(guest_password);wait('DESKTOP PROFILE_SWITCH[^\n]*profile-1',start)
 d.screenshot('03-authorized-administrator');passed('Switching verifies the selected password; only the authenticated administrator can grant another profile sudo access and configure its timeout')
 d.launch('Terminal');command('sudo -n id',r'authentication required or access denied')
 start=len(raw());v.send('sudo -S id\n');wait(r'\[sudo\] Password:',start);v.send(guest_password+'\n');wait(r'uid=0\(root\)',start)
 command('sudo -n id',r'authentication required or access denied')
 d.exit();assert '\n1001\n' in v.raw_command('id -u')
 v.owner_console();assert '\n1000\n' in v.raw_command('id -u')
 passed('Administrator commands receive actual UID 0; Always require password prevents reuse, and leaving the desktop preserves the selected console identity')
 v.user_command('rm /home/user/.config/profiles.ini')
 v.shell_command("printf 'version=1\\nactive=profile-1\\ndefault=Default\\nprofile-1=Guest\\n' > /home/user/.config/profiles.ini",as_user=True)
 start=len(raw());v.send('gui --trace\n');v.until(b'GUI_READY:',60);wait('DESKTOP PROFILE_ACTIVE id=default[^\n]*count=2',start);d=desktop.DesktopTest(v);d.screenshot('04-forged-preferences-ignored');d.exit()
 passed('Forging the preference registry cannot change the kernel login identity')
 report['state']='passed'
except BaseException as e:
 report['error']=repr(e);(out/'exception.txt').write_text(traceback.format_exc())
 if d:
  try:d.screenshot('failure')
  except Exception:pass
 raise
finally:
 if v:v.close()
 report['owned_vm_stopped']=bool(v and v.process.poll() is not None)
 (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
