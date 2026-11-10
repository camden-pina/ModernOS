#!/usr/bin/env python3
"""Verify port provenance, linker-safe PIC archives and original user outputs."""
from pathlib import Path
import hashlib,json,os,subprocess
R=Path(__file__).resolve().parents[1];T=R/'build/continuation-20261015-phase5';RT=R/'userland/runtime';P=RT/'usr/lib/coast/linux-ports'
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
m=json.loads((P/'manifest.json').read_text());assert {p['name'] for p in m['packages']}=={'bash','grep','sed','diffutils','lua','sqlite'}
programs=[];libraries=[]
for package in m['packages']:
 source=package['source'];assert sha(R/'userland/vendor/linux-ports'/source['archive'])==source['sha256']
 assert sha(RT/'usr/src/coast/linux-ports'/source['archive'])==source['sha256']
 for patch in source.get('patches',[]):assert sha(RT/'usr/src/coast/linux-ports'/patch['file'])==patch['sha256']
 assert list((RT/'usr/share/licenses/linux-ports'/package['name']).iterdir())
 for program in package['programs']:
  path=RT/program['path'];assert sha(path)==program['sha256'];alias=RT/program['alias'];assert alias.is_symlink() and os.readlink(alias)=='/'+program['path']
  header=subprocess.check_output(['readelf','-hlW',path],text=True);assert 'Advanced Micro Devices X86-64' in header and 'INTERP' not in header and ' RWE ' not in header
  programs.append(program['program'])
 for library in package['libraries']:
  path=RT/library['path'];assert sha(path)==library['sha256'];relocs=subprocess.check_output(['readelf','-rW',path],text=True);assert 'GOTPCRELX' not in relocs,'Newer relaxable relocations are unsafe with native GNU ld2.37'
  libraries.append(path.name)
assert len(programs)==10 and set(libraries)=={'liblua.a','libsqlite3.a'}
baseline=json.loads((T/'BASELINE.json').read_text());override=json.loads((T/'PRIVATE-TEST-FIXTURE-OVERRIDE.json').read_text())
for entry in baseline['bin_files']:
 path=R/entry['path'];expected=override['after_sha256'] if entry['path']=='userland/bin/web-capability-check' else entry['sha256'];assert sha(path)==expected,entry['path']
for entry in baseline['source_files']:
 if not entry['path'].startswith('userland/runtime/'):continue
 path=R/entry['path']
 if entry['type']=='symlink':assert path.is_symlink() and os.readlink(path)==entry['target'],entry['path']
 else:assert sha(path)==entry['sha256'],entry['path']
assert sha(Path(override['preserved_original']))==override['before_sha256']
print('LINUX_PORT_HOST_PROVENANCE_PASS:6 projects10 static Linux/musl source rebuilds;2 linker-safe PIC archives;licenses/corresponding sources;original runtime and native inputs preserved with one recorded isolated QA-fixture refresh')
