#!/usr/bin/env python3
"""SQLite rollback-journal data on disposable FAT, verified after a fresh boot."""
import argparse,hashlib,json,pathlib,sqlite3,subprocess
from test_runtime import ROOT,VM,gpt_image,assert_success
P='/usr/lib/coast/linux-ports/sqlite/sqlite3 -batch -list -noheader';M='/mnt/nvme0n1p1'
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def main():
 p=argparse.ArgumentParser();p.add_argument('--iso',type=pathlib.Path,required=True);p.add_argument('--cpus',type=int,choices=(1,4),required=True);p.add_argument('--label',required=True);p.add_argument('--output-root',type=pathlib.Path,default=ROOT/'build/continuation-20261015-phase5');a=p.parse_args();a.iso=a.iso.resolve();out=a.output_root/a.label;out.mkdir(exist_ok=False)
 volume=out/'fat.volume';disk=out/'fat.img';size=96*1024*1024
 with volume.open('wb') as f:f.truncate(size)
 subprocess.run(['mkfs.fat','-F','32','-n','PORTSTORE',volume],check=True,stdout=subprocess.DEVNULL);gpt_image(volume,disk,'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7')
 expected=hashlib.sha256(b'Ab'*65536+b'\n').hexdigest();v={'result':'failed','iso':str(a.iso),'iso_sha256':sha(a.iso),'cpus':a.cpus,'payload_bytes':131072,'payload_sha256_including_export_newline':expected,'fresh_guest_reopen':False,'boots':[],'physical_disks_used':False};vm=None
 try:
  extra=['-drive',f'file={disk},format=raw,if=none,id=ports','-device','nvme,drive=ports,serial=PORTSTORE']
  for boot in ('create','fresh-reopen'):
   vm=VM(a.cpus,storage=False,network=False,iso=a.iso,accel='kvm',label=a.label+'-'+boot,extra_args=extra)
   b={'purpose':boot,'serial_log':str(vm.log.name)};v['boots'].append(b)
   if boot=='create':b['create']=vm.shell_command(P+' -bail '+M+'/ports.db < /usr/share/coast/linux-ports/persistent-create.sql',expect='ok',timeout=90)
   b['export']=vm.shell_command(P+' -bail '+M+'/ports.db < /usr/share/coast/linux-ports/persistent-export.sql',timeout=90);assert '131072\n420\nok' in b['export'],b
   b['payload']=vm.shell_command('sha256sum '+M+'/payload.txt',expect=expected,timeout=45)
   if boot=='fresh-reopen':
    b['unmount']=vm.command('umount '+M);assert_success(b['unmount'])
    b['readonly_mount']=vm.command('mount -t fat32 -o ro nvme0n1p1 '+M);assert_success(b['readonly_mount'])
    script=P+' -bail '+M+'/ports.db "INSERT INTO persisted VALUES(2,123);" >/tmp/sqlite-readonly 2>&1; r=$?; cat /tmp/sqlite-readonly; test "$r" = 1 && /usr/lib/coast/linux-ports/grep/grep -q readonly /tmp/sqlite-readonly && echo SQLITE_READONLY_REJECTED_PASS'
    b['readonly_rejected']=vm.shell_command(script,expect='SQLITE_READONLY_REJECTED_PASS',timeout=45)
    b['readonly_read']=vm.shell_command(P+' -readonly -bail '+M+'/ports.db "SELECT count(*) FROM persisted;PRAGMA integrity_check;"',timeout=45);assert '1\nok' in b['readonly_read'],b
    v['fresh_guest_reopen']=True
   b['sync']=vm.command('sync');assert_success(b['sync']);vm.close();vm=None
  for name in ('ports.db','payload.txt'):subprocess.run(['mcopy','-i',str(disk)+'@@1048576','::/'+name,out/name],check=True)
  assert sha(out/'payload.txt')==expected
  with sqlite3.connect('file:'+str(out/'ports.db')+'?mode=ro',uri=True) as db:
   assert db.execute('PRAGMA integrity_check').fetchall()==[('ok',)]
   rows=db.execute('SELECT n,content FROM persisted').fetchall();assert rows==[(1,'Ab'*65536)]
   assert db.execute('PRAGMA user_version').fetchone()==(420,)
  v['host_database_integrity']='passed';v['host_complete_payload']='all131072 bytes matched';v['database_sha256']=sha(out/'ports.db');v['result']='passed'
 finally:
  if vm:vm.close()
  v['iso_unchanged']=sha(a.iso)==v['iso_sha256'];(out/'report.json').write_text(json.dumps(v,indent=2)+'\n')
 print('LINUX_PORT_STORAGE_PASS '+str(out/'report.json'),flush=True)
if __name__=='__main__':main()
