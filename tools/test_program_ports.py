#!/usr/bin/env python3
"""Validate actual CPython/jq in an owned diskless/networkless Coast VM."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import time
import traceback
import test_runtime as runtime
import test_desktop as desktop

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--iso',type=Path,required=True);p.add_argument('--output-root',type=Path,required=True);p.add_argument('--cpus',type=int,default=4);a=p.parse_args()
 out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=False);runtime.WORK=desktop.WORK=out
 v=None;report=dict(state='failed',iso=str(a.iso.resolve()),checks=[],harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),target_host_executed=False)
 try:
  v=runtime.VM(a.cpus,storage=False,network=False,memory='16G',accel='kvm',iso=a.iso,label='program-ports')
  v.user_command('mkdir -p /home/user/Documents')
  report['python_version']=v.shell_command('python3 --version',expect='Python 3.14.10',as_user=True)
  report['jq_version']=v.shell_command('jq --version',expect='jq-1.8.2',as_user=True)
  result=v.shell_command('python3 /usr/share/coast/program-ports/check.py /home/user/Documents',expect='PROGRAM_PORTS_ALL_PASS',as_user=True,timeout=150)
  report['checks']=[line.removeprefix('PROGRAM_PORTS_PASS ') for line in result.splitlines() if line.startswith('PROGRAM_PORTS_PASS ')]
  assert len(report['checks'])==10,result
  v.shell_command("printf '%s' '{\"values\":[1,2,3]}' | jq -ec '.values|add==6'",expect='true',as_user=True)
  report['checks'].append('real shell pipeline into jq')
  v.send("build-sh -c 'PYTHON_BASIC_REPL=1 python3 -q'\n");v.until(rb'>>> ',30)
  v.send('print("PYTHON_REPL_OK", 6*7)\n');v.until(rb'PYTHON_REPL_OK 42[\s\S]*>>> ',20)
  v.send('\x03');v.until(rb'KeyboardInterrupt[\s\S]*>>> ',20)
  v.send('exit()\n');v.until_prompt(20)
  report['checks'].append('interactive Python REPL over the actual console')
  assert '\n1000\n' in v.raw_command('id -u')
  def observed():
   while v.selector.select(.003):
    data=os.read(v.process.stdout.fileno(),65536)
    if not data:break
    v.pending.extend(data);v.log.write(data);v.log.flush()
   source=Path(v.log.name).read_text(errors='replace');output=[]
   for record in re.split(r'(?m)(?<=\n)(?=DESKTOP )',source):
    match=re.match(r'DESKTOP TERMINAL_OUTPUT window=\d+ workspace=\d+ ([\s\S]*)',record.removesuffix('\n'))
    if match:output.append(match[1])
   return re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]','', ''.join(output).replace('\r',''))
  def wait(pattern):
   end=time.monotonic()+25
   while time.monotonic()<end:
    source=observed()
    if re.search(pattern,source):return
    time.sleep(.02)
   raise AssertionError('Missing '+pattern+'\n'+observed()[-1800:])
  v.send('gui --trace\n');v.until(b'GUI_READY:',60);d=desktop.DesktopTest(v);d.screenshot('00-desktop');d.launch('Terminal')
  v.send("printf '{\"n\":[1,2,3]}' | jq -c '.n|map(.*2)' > /home/user/Documents/jq-pipeline.json; printf '\\n%s%d\\n' JQ_PIPELINE_STATUS $?\n")
  wait(r'\nJQ_PIPELINE_STATUS0\n')
  v.send('PYTHON_BASIC_REPL=1 python3 -q\n');wait(r'>>> ')
  v.send('print("PYTHON_TERMINAL_OK", 7*6)\n');wait(r'\nPYTHON_TERMINAL_OK 42\n')
  d.screenshot('01-python-terminal');v.send('exit()\n');time.sleep(.2);d.exit()
  v.shell_command("test \"$(cat /home/user/Documents/jq-pipeline.json)\" = '[2,4,6]'",as_user=True)
  report['checks'].append('Python REPL and a real GNU Bash-to-jq pipeline inside desktop Terminal')
  report['state']='passed'
  for check in report['checks']:print('PASS '+check,flush=True)
 except BaseException as error:
  report['error']=repr(error);(out/'exception.txt').write_text(traceback.format_exc());raise
 finally:
  if v:v.close()
  report['owned_vm_stopped']=bool(v and v.process.poll() is not None)
  (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
