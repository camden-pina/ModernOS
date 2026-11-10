#!/usr/bin/env python3
"""Exercise console/desktop completion using real guest files and input."""
import argparse,json,pathlib,re,time,shlex,hashlib
import test_desktop as desktop
from test_desktop import DesktopTest
from test_runtime import VM,ROOT,assert_success

def exercise(a):
 out=a.output_root/a.label;out.mkdir(exist_ok=False)
 report={'result':'failed','iso':str(a.iso),'iso_sha256':hashlib.sha256(a.iso.read_bytes()).hexdigest(),'cpus':a.cpus,'accel':a.accel,'cases':[]};vm=None
 def passed(name):report['cases'].append(name);print('PASS '+name,flush=True)
 try:
  vm=VM(a.cpus,storage=False,network=False,iso=a.iso,accel=a.accel,label=a.label)
  scripts=["mkdir -p '/tmp/completion/dir one'", "cd /tmp/completion; printf DATA_ALPHA >alpha.txt; printf DATA_ALPINE >alpine.txt; printf DATA_SPACE >'a space.txt'", "cd /tmp/completion; printf DATA_QUOTE >\"quote'one.txt\"; printf DATA_UTF8 >αbeta; touch αcharlie", "cp /usr/lib/coast/linux-compat/semantic-check /tmp/completion/zz-command; chmod 755 /tmp/completion/zz-command"]
  for s in scripts:assert_success(vm.shell_command(s))
  vm.send('env PATH=/tmp/completion:/bin:/usr/bin /bin/coast-sh\n');vm.until_prompt()
  assert_success(vm.command('cd /tmp/completion'))
  def cancel():
   vm.send('\x03');vm.until(rb'\^C\nos:[^\n]*\$ $',15)
  def ctab(source,expected,arrows=0):
   vm.send(source+('\x1b[D'*arrows)+'\t');vm.until(re.escape(('os:/tmp/completion$ '+expected).encode())+b'$',20)
  ctab('pw','pwd ');cancel();passed('console authoritative builtin completion')
  ctab('zz-com','zz-command ');cancel();assert_success(vm.shell_command('test ! -e /tmp/completion-executed'));passed('console PATH completion never executes')
  for source,expected in [('cat al','cat alp'),('cd dir','cd dir\\ one/'),('cat a\\ sp','cat a\\ space.txt '),("cat 'a sp","cat 'a space.txt' "),('cat "a sp','cat "a space.txt" '),('cat quote','cat quote\\\'one.txt '),('cat αb','cat αbeta '),('cat ./missing','cat ./missing'),('echo x | pw','echo x | pwd '),('cat > al','cat > alp')]:ctab(source,expected);cancel()
  ctab('cat al.txt','cat alp.txt',4);cancel();passed('console paths, prefixes/list, directory/quote/Unicode, no-match, middle cursor and pipeline/redirection context')
  assert_success(vm.command('echo COMPLETION_HISTORY','COMPLETION_HISTORY\n'))
  vm.send('cat a\\ sp\x1b[A\x1b[B\t');vm.until(re.escape(b'os:/tmp/completion$ cat a\\ space.txt ')+b'$',20);cancel();passed('console history preserves unfinished draft')
  ctab('zz-com','zz-command ');vm.send('--completion-command\n');vm.until(rb'COMPLETION_EXECUTED_ON_ENTER\n[\s\S]*os:[^\n]*\$ $',30)
  assert_success(vm.shell_command('test -f /tmp/completion-executed; rm /tmp/completion-executed'));passed('console completed command executes only after Enter')
  assert_success(vm.command('exit'))
  assert_success(vm.shell_command("printf 'cat αb' | clip -c"))
  vm.send('env PATH=/tmp/completion:/bin:/usr/bin gui --trace --stats\n');vm.until(rb'GUI_READY: Coast desktop',45)
  d=DesktopTest(vm);desktop.WORK=out;d.screenshot('completion-desktop-initial');d.launch('Terminal')
  def term(command,expect=None):
   vm.send(command+'\n');vm.until(rb'DESKTOP TERMINAL_COMMAND[^\n]*'+re.escape(command.encode())+rb'[\s\S]*os:[^\n]*\$ ',30)
   if expect:vm.until(re.escape(expect.encode()),20)
  term('cd /tmp/completion')
  def clear():d.key('a','ctrl');d.key('backspace')
  def gtab(source,expected,arrows=0):
   if any(ord(c)>127 for c in source):
    # The desktop's legacy serial transport is ASCII; use its real UTF8 paste path.
    d.key('v','ctrl','shift');d.expect('CLIPBOARD_PASTE','Terminal command field')
   else:vm.send(source)
   time.sleep(.2)
   for _ in range(arrows):d.key('left')
   d.key('tab');d.expect('TERMINAL_COMPLETE',expected)
  gtab('pw','pwd ');clear();gtab('zz-com','zz-command ');clear();passed('desktop builtin and inherited PATH completion')
  for source,expected in [('cat al','cat alp'),('cd dir','cd dir\\ one/'),('cat a\\ sp','cat a\\ space.txt '),("cat 'a sp","cat 'a space.txt' "),('cat "a sp','cat "a space.txt" '),('cat αb','cat αbeta '),('cat ./missing','cat ./missing'),('echo x | pw','echo x | pwd '),('cat > al','cat > alp')]:gtab(source,expected);clear()
  gtab('cat al.txt','cat alp.txt',4);clear();passed('desktop actual child cwd, quote/Unicode, directories, no-match, cursor suffix and command contexts')
  term('echo COMPLETION_GUI_HISTORY')
  vm.send('cat a\\ sp');time.sleep(.2);d.key('up');d.key('down');d.key('tab');d.expect('TERMINAL_COMPLETE','cat a\\ space.txt ');clear();passed('desktop history restores unfinished draft before completion')
  vm.send('build-sh\n')
  # Upstream ash owns a raw-mode editor; GUI forwards Tab through VT to it.
  vm.until(rb'DESKTOP TERMINAL_COMMAND[^\n]*build-sh[\s\S]*DESKTOP TERMINAL_OUTPUT[\s\S]*# ',25)
  vm.send('cat a\\ sp');time.sleep(.2);d.key('tab');d.key('ret');vm.until(rb'DATA_SPACE[\s\S]*# ',25)
  vm.send('exit\n');vm.until(rb'DESKTOP TERMINAL_OUTPUT[\s\S]*os:/tmp/completion\$ ',25);passed('nested interactive build-sh raw-mode upstream completion')
  gtab('zz-com','zz-command ');clear();d.screenshot('completion-results')
  d.exit();assert_success(vm.shell_command('test ! -e /tmp/completion-executed'));passed('desktop completion never executes command')
  report['result']='passed'
 finally:
  if vm:vm.close()
  (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
 print('Completion regression '+report['result']+' '+str(out/'report.json'),flush=True)
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--iso',type=pathlib.Path,required=True);p.add_argument('--cpus',type=int,default=4);p.add_argument('--accel',choices=['kvm','tcg'],default='kvm');p.add_argument('--label',required=True);p.add_argument('--output-root',type=pathlib.Path,default=ROOT/'build/continuation-20261015-phase3');a=p.parse_args();a.iso=a.iso.resolve();exercise(a)
