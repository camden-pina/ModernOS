#!/usr/bin/env python3
"""Execute actual Linux/musl CLI ports, file workflows and native SDK consumers."""
from pathlib import Path
import argparse,hashlib,json,re,time
from test_runtime import VM,ROOT,assert_success

def digest(p):
 with p.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--iso',type=Path,required=True);p.add_argument('--cpus',type=int,default=4);p.add_argument('--label',required=True);p.add_argument('--baseline',action='store_true');p.add_argument('--output-root',type=Path,default=ROOT/'build/continuation-20261015-phase5');a=p.parse_args();a.iso=a.iso.resolve();out=a.output_root/a.label;out.mkdir(exist_ok=False)
 result={'result':'failed','iso':str(a.iso),'iso_sha256':digest(a.iso),'cpus':a.cpus,'upstream_distribution_binaries_tested':False,'cases':[]};vm=None
 try:
  vm=VM(a.cpus,storage=False,network=False,iso=a.iso,accel='kvm',label=a.label)
  result['versions']=vm.shell_command('linux-bash --version; linux-grep --version; linux-sed --version; linux-diff --version; linux-lua -v; linux-luac -v; linux-sqlite3 --version',timeout=45)
  for version in ('5.3.20','3.12','4.10','5.5.1','3.54.0'):assert version in result['versions'],version
  result['diagnostics']=vm.shell_command('/usr/lib/coast/linux-ports/compat-diagnostics',timeout=45)
  command='/bin/build-sh /usr/share/coast/linux-ports/workflows.sh'
  vm.send(command+'\n');output=vm.until(rb'LINUX_PORT_RESULT passed=[0-9]+ failed=[0-9]+[\s\S]*os:[^\n]*\$ $',timeout=240)
  result['output']=output;result['cases']=[{'name':name,'exit':int(code)} for name,code in re.findall(r'LINUX_PORT_CASE ([a-z0-9-]+) ([0-9]+)',output)];assert len(result['cases'])==19,result
  if not a.baseline:assert_success(output);assert all(c['exit']==0 for c in result['cases']),result
  result['result']='baseline-observed' if a.baseline else 'passed';result['serial_log']=str(vm.log.name)
 finally:
  if vm:vm.close()
  result['input_unchanged']=digest(a.iso)==result['iso_sha256'];(out/'report.json').write_text(json.dumps(result,indent=2)+'\n')
 print(json.dumps(result['cases'],indent=2),flush=True)
if __name__=='__main__':main()
