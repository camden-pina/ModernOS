#!/usr/bin/env python3
"""Publish the reviewed, signed Coast example recipes through actual aurweb SSH."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import shlex
import subprocess
import urllib.request


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--data',type=Path,required=True)
    p.add_argument('--repository',type=Path,required=True)
    p.add_argument('--output-root',type=Path,required=True)
    p.add_argument('--container',default='coast-community-r1')
    a=p.parse_args();out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=False,mode=0o700)
    data=a.data.resolve();key=data/'MAINTAINER-SSH-KEY';credentials=data/'MAINTAINER-CREDENTIALS.txt'
    if not key.exists():subprocess.run(['ssh-keygen','-q','-t','ed25519','-N','','-f',str(key)],check=True)
    key.chmod(0o600)
    password=secrets.token_urlsafe(30)
    if not credentials.exists():
        with credentials.open('x') as stream:
            os.chmod(credentials,0o600);stream.write('Coast community recipe maintainer\nUsername: coastports\nPassword: '+password+'\n')
    pub=' '.join(key.with_suffix('.pub').read_text().split()[:2])
    code='''import json,sys
from aurweb import db
from aurweb.models import User,AccountType,SSHPubKey
from aurweb.models.ssh_pub_key import get_fingerprint
v=json.load(sys.stdin)
with db.begin():
 user=db.query(User,User.Username=='coastports').first()
 if not user:
  user=db.create(User,Username='coastports',Email='ports@coast.invalid',Passwd=v['password'],
    AccountType=db.query(AccountType,AccountType.ID==1).one(),EmailVerified=True)
 found=db.query(SSHPubKey,SSHPubKey.PubKey==v['pub']).first()
 if found:assert found.UserID==user.ID
 else:db.create(SSHPubKey,User=user,PubKey=v['pub'],Fingerprint=get_fingerprint(v['pub']))
'''
    subprocess.run(['podman','exec','-i','--user','aur',a.container,'python','-c',code],input=json.dumps(dict(password=password,pub=pub)),text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True)
    host_key=subprocess.check_output(['podman','exec',a.container,'cat','/data/ssh/ssh_host_ed25519_key.pub']).decode().split()
    known=out/'known_hosts';known.write_text('[127.0.0.1]:2222 '+' '.join(host_key[:2])+'\n')
    ssh=['ssh','-i',str(key),'-p','2222','-o','IdentitiesOnly=yes','-o','BatchMode=yes',
         '-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+str(known)]
    env=dict(os.environ,GIT_SSH_COMMAND=shlex.join(ssh),GIT_TERMINAL_PROMPT='0',GIT_CONFIG_NOSYSTEM='1',GIT_CONFIG_GLOBAL='/dev/null')
    recipes=json.loads(a.repository.read_text())['recipes'];published=[]
    for recipe in recipes:
        name=recipe['Name'];assert name in ('hello-coast','bc','ninja')
        target=out/name
        commands=[['clone','http://127.0.0.1:8765'+recipe['URLPath'],str(target)],
                  ['-C',str(target),'push','ssh://aur@127.0.0.1:2222/'+name+'.git','master']]
        with (out/(name+'-git.log')).open('w') as log:
            subprocess.run(['git','-c','core.hooksPath=/dev/null','-c','http.proxy=',*commands[0]],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
            commit=subprocess.check_output(['git','-C',str(target),'rev-parse','HEAD'],env=env,text=True).strip()
            assert commit==recipe['Commit'],'source must match the reviewed signed catalog'
            subprocess.run(['git','-c','core.hooksPath=/dev/null',*commands[1]],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
        with urllib.request.urlopen('http://127.0.0.1:8770/rpc/v5/info?arg[]='+name) as response:info=json.load(response)['results'][0]
        assert info['Maintainer']=='coastports' and info['Version']==recipe['Version']
        published.append(dict(name=name,commit=commit,version=info['Version']))
        print('Published reviewed community recipe: '+name,flush=True)
    (out/'report.json').write_text(json.dumps(dict(state='passed',maintainer='coastports',recipes=published,
        submission='real SSH aurweb hooks',target_programs_executed_on_host=False),indent=2)+'\n')


if __name__=='__main__':main()
