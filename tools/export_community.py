#!/usr/bin/env python3
"""Export the actual aurweb OCI image and complete Coast/upstream source, excluding private state."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]


def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',required=True)
    p.add_argument('--upstream',type=Path,required=True)
    p.add_argument('--output-root',type=Path,required=True)
    a=p.parse_args();out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=False)
    source=a.upstream.resolve()
    commit=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
    assert commit=='ed420d423d20a69ed5b9bd77aea6b81538d16248','review a changed aurweb revision before exporting'
    assert not subprocess.check_output(['git','-C',str(source),'status','--porcelain'])
    metadata=json.loads(subprocess.check_output(['podman','image','inspect',a.image]))[0]
    image=out/'brine-community.oci.tar'
    subprocess.run(['podman','save','--format','oci-archive','--output',str(image),a.image],check=True)
    for name in ('manage.py','server.example.json','README.txt'):
        shutil.copyfile(ROOT/'deployment/community'/name,out/name)
    (out/'manage.py').chmod(0o755)
    shutil.copyfile(out/'server.example.json',out/'server.json')
    shutil.copytree(ROOT/'deployment/community',out/'source/brine')
    upstream=out/'source/aurweb.tar.gz'
    subprocess.run(['git','-C',str(source),'archive','--format=tar.gz','--prefix=aurweb/',
                    '--output='+str(upstream),'HEAD'],check=True)
    shutil.copyfile(source/'COPYING',out/'source/aurweb-COPYING')
    shutil.copyfile(ROOT/'tools/export_community.py',out/'source/export_community.py')
    manifest=dict(version=1,name='Brine Community',image_archive=image.name,image_sha256=sha(image),image_id=metadata['Id'],
        architecture=metadata['Architecture'],upstream=dict(project='aurweb',version='6.5.1',commit=commit,
        url='https://gitlab.archlinux.org/archlinux/aurweb',license='GPL-2.0-only',source='source/aurweb.tar.gz',source_sha256=sha(upstream)),
        private_state_included=False,signing_key_included=False,files={str(path.relative_to(out)):sha(path)
            for path in out.rglob('*') if path.is_file() and path!=image})
    (out/'bundle.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('Exported portable Brine community image and source: '+str(out))


if __name__=='__main__':main()
