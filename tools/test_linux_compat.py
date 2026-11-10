#!/usr/bin/env python3
"""Execute provenance-pinned, unmodified Linux/musl upstream programs in a guest."""
import argparse,json,pathlib,re,shlex,time,uuid,hashlib
from test_runtime import VM,ROOT

def probe(vm,script,timeout=45):
 token='LNX'+uuid.uuid4().hex[:10]
 wrapped="printf '\\n%sB\\n' "+token+"; (set -e; "+script+"); r=$?; printf '\\n%sE%d\\n' "+token+' "$r"'
 command='build-sh -c '+shlex.quote(wrapped)
 assert len(command.encode())<511,command
 vm.send(command+'\n');pattern=rb'\n'+token.encode()+rb'B\n([\s\S]*)\n'+token.encode()+rb'E([0-9]+)\n[\s\S]*os:[^\n]*\$ $';out=vm.until(pattern,timeout);match=re.search(pattern,out.encode());return {'exit':int(match[2]),'output':match[1].decode(errors='replace')}

def exercise(args):
 out=args.output_root/args.label;out.mkdir(exist_ok=False)
 programs=ROOT/'userland/runtime/usr/lib/coast/linux-compat'
 record={'result':'failed','purpose':'baseline observations' if args.baseline else 'required supported-program regression','iso':str(args.iso),'iso_sha256':hashlib.sha256(args.iso.read_bytes()).hexdigest(),'cpus':args.cpus,'accel':args.accel,'linkage':'static x86-64 Linux/musl1.2.5, unmodified upstream application source','untouched_prebuilt_distro_binaries_tested':False,'cases':[],'physical_hardware_tested':False}
 vm=None
 C='/usr/lib/coast/linux-compat/coreutils';T='/usr/lib/coast/linux-compat/tar/tar';F='/usr/lib/coast/linux-compat/util-linux/flock'
 cases=[
 ('abi-inventory','/usr/lib/coast/linux-compat/abi-audit'),('semantic-check','/usr/lib/coast/linux-compat/semantic-check'),('window-semantics','/bin/window-check'),('coreutils-version',C+'/stat --version | head -n1'),('tar-version',T+' --version | head -n1'),('flock-version',F+' --version'),
 ('copy-bytes-mode',f'{C}/cp --reflink=never --sparse=never /tmp/linux-src /tmp/linux-copy; cmp /tmp/linux-src /tmp/linux-copy; test "$(stat -c %a /tmp/linux-copy)" = 640'),
 ('chmod-at',C+'/chmod 600 /tmp/linux-copy; test "$(stat -c %a /tmp/linux-copy)" = 600'),
 ('hard-link',f'{C}/ln /tmp/linux-src /tmp/linux-hard; test /tmp/linux-src -ef /tmp/linux-hard; printf EXTRA >>/tmp/linux-hard; cmp /tmp/linux-src /tmp/linux-hard'),
 ('symlink-readlink',f'{C}/ln -s linux-src /tmp/linux-sym; test "$({C}/readlink /tmp/linux-sym)" = linux-src'),
 ('stat-metadata',C+'/stat --format="%F %a %h %s" /tmp/linux-src'),
 ('stat-filesystem',C+'/stat --file-system --format="%T %s %l" /tmp'),
 ('tar-regular',f'mkdir -p /tmp/linux-tarin /tmp/linux-tarout; cp /tmp/linux-src /tmp/linux-tarin/file; {T} -cf /tmp/linux.tar -C /tmp/linux-tarin .; {T} --no-same-owner -xf /tmp/linux.tar -C /tmp/linux-tarout; cmp /tmp/linux-src /tmp/linux-tarout/file'),
 ('tar-hardlink-archive',f'mkdir -p /tmp/linux-hardout; {T} --no-same-owner -xf /usr/lib/coast/linux-compat/fixtures/hardlinks.tar -C /tmp/linux-hardout; test /tmp/linux-hardout/file -ef /tmp/linux-hardout/hard; test "$(readlink /tmp/linux-hardout/symbolic)" = file'),
 ('tar-default-owner',f'mkdir -p /tmp/linux-ownerout; {T} -xf /usr/lib/coast/linux-compat/fixtures/hardlinks.tar -C /tmp/linux-ownerout; cmp /tmp/linux-ownerout/file /tmp/linux-hardout/file'),
 ('copy-preserve',f'{C}/cp -p --reflink=never /tmp/linux-src /tmp/linux-preserved; cmp /tmp/linux-src /tmp/linux-preserved'),
 ('mkdir-parents',C+'/mkdir -p -m 750 /tmp/linux-deep/a/b; test -d /tmp/linux-deep/a/b'),
 ('flock-exclusive',F+' -x /tmp/linux.lock /bin/true'),
 ('flock-contention',F+' -x /tmp/linux.lock build-sh -c '+shlex.quote(F+' -n /tmp/linux.lock /bin/true; r=$?; test "$r" = 1')),
 ('fifo-probe',C+'/mkfifo /tmp/linux.fifo; test -p /tmp/linux.fifo'),
 ('sleep-clock',C+'/sleep .01'),
 ('timeout-foreground',C+'/timeout --foreground .05 '+C+'/sleep 2; r=$?; printf "TIMEOUT_EXIT=%s\\n" "$r"; test "$r" = 124'),
 ]
 try:
  vm=VM(args.cpus,storage=False,network=False,iso=args.iso,accel=args.accel,label=args.label)
  prep=probe(vm,"printf 'LINUX_COMPAT_BYTES\\n' >/tmp/linux-src; chmod 640 /tmp/linux-src");assert prep['exit']==0,prep
  for name,script in cases:
   # timeout's intentional status must be observed before set-e terminates it.
   if name in ('timeout-foreground','flock-contention'):script='set +e; '+script
   result=probe(vm,script,timeout=25 if name=='timeout-foreground' else 45);result['case']=name;record['cases'].append(result);print(('PASS' if not result['exit'] else 'UNSUPPORTED' if name=='fifo-probe' and 'Function not implemented' in result['output'] else 'FAIL')+' '+name+' rc='+str(result['exit']),flush=True)
  fifo=next(c for c in record['cases'] if c['case']=='fifo-probe')
  if not args.baseline and fifo['exit']:assert fifo['exit']==1 and 'Function not implemented' in fifo['output'],fifo
  supported=[c for c in record['cases'] if c['case']!='fifo-probe']
  if not args.baseline:assert all(c['exit']==0 for c in supported),[c for c in supported if c['exit']]
  record['result']='observed-baseline' if args.baseline else 'passed';record['remaining_fifo_support']='unsupported' if next(c for c in record['cases'] if c['case']=='fifo-probe')['exit'] else 'supported'
 finally:
  if vm:vm.close()
  (out/'guest-report.json').write_text(json.dumps(record,indent=2)+'\n')
 print('Linux compatibility '+record['result']+' '+str(out/'guest-report.json'),flush=True)
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--iso',type=pathlib.Path,required=True);p.add_argument('--label',required=True);p.add_argument('--output-root',type=pathlib.Path,default=ROOT/'build/continuation-20261015-phase3');p.add_argument('--cpus',type=int,default=4);p.add_argument('--accel',choices=('kvm','tcg'),default='kvm');p.add_argument('--baseline',action='store_true');a=p.parse_args();a.iso=a.iso.resolve();exercise(a)
