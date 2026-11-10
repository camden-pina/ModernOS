#!/usr/bin/env python3
"""Inspect an upstream Electron/Code OSS desktop ELF without executing it.
This inventories prerequisites; it never establishes an application is runnable.
"""
from pathlib import Path
import argparse,hashlib,json,re,struct,subprocess,shutil

def main():
 a=argparse.ArgumentParser(description=__doc__);a.add_argument('--application-root',type=Path,required=True);a.add_argument('--kernel-root',type=Path,default=Path(__file__).resolve().parents[1]);a.add_argument('--output',type=Path,required=True);v=a.parse_args();U=v.application_root.resolve();R=v.kernel_root.resolve();exe=U/'codium';assert exe.is_file();raw=exe.read_bytes();h=struct.unpack_from('<16sHHIQQQIHHHHHH',raw);assert h[0][:6]==b'\x7fELF\x02\x01' and h[2]==62 and h[9]==56;loads=[];interpreter=None
 for i in range(h[10]):
  typ,flags,off,va,pa,fs,ms,al=struct.unpack_from('<IIQQQQQQ',raw,h[5]+i*h[9]);assert off+fs<=len(raw)
  if typ==1:loads.append({'vaddr':va,'file_bytes':fs,'memory_bytes':ms,'flags':flags,'mapped_bytes':((va+ms+4095)//4096*4096)-(va//4096*4096)})
  if typ==3:interpreter=raw[off:off+fs].rstrip(b'\0').decode()
 pkg=json.loads((U/'resources/app/package.json').read_text());prod=json.loads((U/'resources/app/product.json').read_text());dynamic=subprocess.check_output(['readelf','-d',str(exe)],text=True);needed=re.findall(r'\(NEEDED\).*?\[(.*?)\]',dynamic);kernel=(R/'include/mm/user.h').read_text();limit=re.search(r'#define USER_MAX_IMAGE \((\d+) \* 1024 \* 1024\)',kernel);assert limit;maximage=int(limit[1])*1024*1024;mapped=sum(x['mapped_bytes'] for x in loads)
 record={'purpose':'actual upstream application dependency/ELF audit, no execution or full-port claim','app_version':pkg['version'],'product_commit_unresolved':prod.get('commit'),'executable_sha256':hashlib.sha256(raw).hexdigest(),'executable_bytes':len(raw),'mapped_load_bytes':mapped,'kernel_image_limit_bytes':maximage,'exceeds_image_limit':mapped>maximage,'interpreter':interpreter,'DT_NEEDED':needed,'program_load_segments':loads,'native_dependencies':{k:x for k,x in pkg.get('dependencies',{}).items() if any(q in k for q in ['pty','watch','sqlite','kerberos','keytar','ripgrep','spdlog'])},'workspace_free_bytes':shutil.disk_usage(v.output.resolve().parent).free};v.output.write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record,indent=2))
if __name__=='__main__':main()
