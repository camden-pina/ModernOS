#!/usr/bin/env python3
"""Check real AUR v5 reads, search ranking and untrusted recipe boundaries."""
import argparse
import gzip
import importlib.util
import json
import os
from pathlib import Path
import urllib.request

ROOT=Path(__file__).resolve().parents[1]


def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--server',default='http://127.0.0.1:8770')
    p.add_argument('--package',required=True)
    p.add_argument('--output-root',type=Path,required=True)
    a=p.parse_args();out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=False)
    client=module('client',ROOT/'userland/packages/client.py');community=client._community_module
    report=dict(state='failed',checks=[],server=a.server,recipes_executed_on_host=False)
    def check(name,value):
        assert value,name
        report['checks'].append(name);print('PASS '+name,flush=True)
    def rejected(name,call):
        try:call()
        except (ValueError,OSError,client.Error):check(name,True)
        else:raise AssertionError(name)
    def fetch(path,maximum):
        with urllib.request.urlopen(a.server+path,timeout=10) as r:
            data=r.read(maximum+1)
        assert len(data)<=maximum
        return data
    try:
        info=community.info(fetch,[a.package]);check('exact lookup uses the real community v5 API',list(info)==[a.package] and info[a.package]['Community'])
        check('long partial names find actual submissions',any(q['Name']==a.package for q in community.search(fetch,['community-check'])))
        candidates=community.search(fetch,['g'])
        candidates.extend([dict(Name='gcc',Version='1',Description='GNU compiler',Commands=['gcc','g++'],Repository='coast'),
                           dict(Name='qt6-demo',Version='1',Description='graphical widgets',Repository='coast'),
                           dict(Name='coast-build',Version='1',Description='build tools',Provides=['g++','gcc'],Repository='coast')])
        g=client.search(candidates,['g'])
        check('combined one-letter search keeps GCC first and Qt absent',g[0]['Name']=='gcc' and not any(q['Name']=='qt6-demo' for q in g))
        sample=info[a.package]
        for key,value in [('Name','../escape'),('PackageBase','bad/name'),('Description','hidden\x1b[2J'),('Depends',['line\nfeed'])]:
            rejected('untrusted metadata rejects invalid '+key,lambda key=key,value=value:community.metadata(dict(sample,**{key:value})))
        bomb=gzip.compress(b' '*(community.MAX_JSON+1))
        rejected('compressed metadata cannot exceed its expanded bound',lambda:community.search(lambda path,maximum:bomb,['g']))
        rejected('exact lookup cannot accept a partial or path',lambda:community.info(fetch,['../']))
        recipe=out/'recipe';recipe.mkdir();(recipe/'PKGBUILD').write_text('exit 99\n');(recipe/'.SRCINFO').write_text('pkgbase = example\n')
        client.validate_checkout(recipe);check('review validation accepts bounded regular recipe files without executing them',True)
        (recipe/'PKGBUILD').write_text('echo visible\x1b[2J\u202eevil\n')
        text=client.printable_recipe(recipe/'PKGBUILD')
        check('terminal controls and bidirectional overrides stay visible during review','\x1b' not in text and '\u202e' not in text and '\\u001b' in text and '\\u202e' in text)
        (recipe/'PKGBUILD').unlink();(recipe/'PKGBUILD').symlink_to('/etc/passwd')
        rejected('review refuses a symlink PKGBUILD',lambda:client.validate_checkout(recipe))
        (recipe/'PKGBUILD').unlink();(recipe/'PKGBUILD').write_text('exit 99\n')
        external=out/'outside';external.mkdir();(external/'post.sh').write_text('exit 99\n')
        (recipe/'scripts').symlink_to(external,target_is_directory=True)
        (recipe/'.SRCINFO').write_text('pkgbase = example\n\tinstall = scripts/post.sh\n')
        rejected('review refuses an install script reached through an escaping directory link',lambda:client.validate_checkout(recipe))
        root=out/'settings';(root/'etc/coast').mkdir(parents=True)
        settings=root/'etc/coast/packages.json';original=dict(version=1,url='http://binary:8765',signing_fingerprint='A'*40)
        settings.write_text(json.dumps(original))
        client.apply_community(a.server,root,os.getuid());changed=json.loads(settings.read_text())
        check('community selection preserves signed binary origin and key',changed==dict(original,community_url=a.server))
        text=client.server_config_texts('https://binary.example',settings.read_text(),'[coast]\nServer = http://binary:8765/repo/$arch\n','')
        check('changing the binary server preserves the community origin',json.loads(text[0])['community_url']==a.server)
        client.apply_community('off',root,os.getuid());check('community queries can be explicitly disabled',json.loads(settings.read_text())['community_url'] is None)
        report['state']='passed'
    finally:(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':main()
