#!/usr/bin/env python3
"""Run, inspect and back up a portable rootless Coast community server."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import tempfile
import fcntl
import time
from urllib.parse import urlsplit

ROOT=Path(__file__).resolve().parent


def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def run(command,**kwargs):
    return subprocess.run(command,check=True,**kwargs)


def settings(path):
    config=json.loads(path.read_text())
    if config.get('version')!=1:raise ValueError('unsupported server configuration')
    if not re.fullmatch(r'coast-community[-a-z0-9]*',config['name']):raise ValueError('invalid owned container name')
    if config.get('bind') not in ('127.0.0.1','0.0.0.0'):
        raise ValueError('bind must be an explicit IPv4 loopback or wildcard address')
    for key in ('web_port','ssh_port'):
        if type(config[key]) is not int or not 1024<=config[key]<=65535:raise ValueError('ports must be unprivileged')
    if config['web_port']==config['ssh_port']:raise ValueError('web and SSH ports must differ')
    origin=urlsplit(config['public_origin'])
    if origin.scheme not in ('http','https') or origin.username or origin.password or origin.path not in ('','/') or origin.query or origin.fragment:
        raise ValueError('public_origin must be a plain HTTP(S) origin')
    if not origin.hostname or not re.fullmatch('[A-Za-z0-9.-]+',origin.hostname):raise ValueError('invalid public host')
    if config['bind']!='127.0.0.1' and origin.scheme!='https':raise ValueError('remote accounts require a TLS reverse proxy and an HTTPS public origin')
    state=Path(config['state']);state=state if state.is_absolute() else path.parent/state
    if state.is_symlink():raise ValueError('state directory cannot be a symbolic link')
    state=state.resolve()
    if state in (Path('/'),Path.home(),ROOT):raise ValueError('choose a dedicated state directory')
    config['state']=str(state)
    config['_backup_marker']=str(path.with_name('.'+path.name+'.backup-active'))
    return config


def running(engine,name):
    query=subprocess.run([engine,'inspect','--format','{{.State.Running}}',name],text=True,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
    return query.returncode==0 and query.stdout.strip()=='true'


def owned(engine,name):
    result=subprocess.run([engine,'inspect','--format','{{index .Config.Labels "org.coast.community"}}',name],
                          text=True,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
    if result.returncode==0 and result.stdout.strip()!='portable':
        raise ValueError('refusing to manage an existing container without the Coast ownership label')


def load(engine,manifest):
    image=ROOT/manifest['image_archive']
    if not image.is_file() or digest(image)!=manifest['image_sha256']:raise ValueError('portable image checksum mismatch')
    run([engine,'load','--input',str(image)],stdout=subprocess.DEVNULL)
    query=run([engine,'image','inspect','--format','{{.Id}}',manifest['image_id']],text=True,stdout=subprocess.PIPE)
    if query.stdout.strip().removeprefix('sha256:')!=manifest['image_id'].removeprefix('sha256:'):
        raise ValueError('loaded image identity mismatch')


def start(engine,config,manifest):
    owned(engine,config['name'])
    if running(engine,config['name']):print('Coast community is already running.');return
    image=subprocess.run([engine,'image','exists',manifest['image_id']],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    if image.returncode:load(engine,manifest)
    state=Path(config['state']);state.mkdir(parents=True,exist_ok=True,mode=0o700)
    if state.stat().st_uid!=os.getuid():raise ValueError('state must be owned by the current user')
    subprocess.run([engine,'rm',config['name']],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=False)
    args=[engine,'run','-d','--name',config['name'],'--label','org.coast.community=portable',
        '--read-only','--security-opt','no-new-privileges','--cap-drop','NET_RAW','--cap-drop','MKNOD',
        '--cap-drop','NET_BIND_SERVICE','--pids-limit','128','--memory','768m',
        '--publish',config['bind']+':'+str(config['web_port'])+':8000',
        '--publish',config['bind']+':'+str(config['ssh_port'])+':2222',
        '--tmpfs','/run:rw,nosuid,nodev,size=64m,mode=755','--tmpfs','/tmp:rw,nosuid,nodev,noexec,size=64m',
        '--mount','type=bind,src='+str(state)+',dst=/data',
        '--env','COAST_PUBLIC_ORIGIN='+config['public_origin'],
        '--env','COAST_SSH_PORT='+str(config.get('public_ssh_port',config['ssh_port'])),manifest['image_id']]
    run(args,stdout=subprocess.DEVNULL)
    print('Coast community started: '+config['public_origin'])
    print('Initial administrator credentials: '+str(state/'ADMIN-CREDENTIALS.txt'))


def process_stamp(pid):
    try:return Path('/proc/'+str(pid)+'/stat').read_text().rsplit(')',1)[1].split()[19]
    except (OSError,IndexError):return None


def backup_active(config):
    try:
        value=json.loads(Path(config['_backup_marker']).read_text())
        return process_stamp(value['pid'])==value['start']
    except (OSError,ValueError,KeyError):return False


def backup(engine,config,destination):
    state=Path(config['state']);destination=destination.resolve()
    if destination.is_relative_to(state):raise ValueError('backup must be outside the live state directory')
    if destination.exists():raise ValueError('backup destination already exists')
    marker=Path(config['_backup_marker'])
    if backup_active(config):raise ValueError('another community backup is in progress')
    marker.unlink(missing_ok=True)
    with marker.open('x') as stream:
        os.chmod(marker,0o600)
        json.dump(dict(pid=os.getpid(),start=process_stamp(os.getpid())),stream)
    was_running=running(engine,config['name'])
    try:
        if was_running:run([engine,'stop','--time','30',config['name']],stdout=subprocess.DEVNULL)
        descriptor,name=tempfile.mkstemp(prefix='.coast-backup-',dir=destination.parent);os.close(descriptor)
        try:
            run([engine,'unshare','tar','--numeric-owner','-cf',name,'-C',str(state),'.'])
            os.chmod(name,0o600);os.replace(name,destination)
        finally:Path(name).unlink(missing_ok=True)
    finally:
        try:
            if was_running and not running(engine,config['name']):run([engine,'start',config['name']],stdout=subprocess.DEVNULL)
        finally:marker.unlink(missing_ok=True)
    print('Private, consistent database/Git backup: '+str(destination))


def restore(engine,config,archive):
    state=Path(config['state'])
    if running(engine,config['name']):raise ValueError('stop the community container before restoring')
    if state.exists() and any(state.iterdir()):raise ValueError('restore requires an empty state directory')
    seen=set();links=set()
    with tarfile.open(archive,'r:*') as source:
        for item in source:
            path=PurePosixPath(item.name)
            if path.is_absolute() or '..' in path.parts or str(path) in seen:raise ValueError('unsafe archive member')
            seen.add(str(path))
            if item.issym() or item.islnk():links.add(str(path))
            if item.issym() and item.linkname=='/opt/coast/git-update' and str(path)=='git/aur.git/hooks/update':continue
            if item.issym() or item.islnk():
                target=PurePosixPath(item.linkname)
                if target.is_absolute() or '..' in target.parts:raise ValueError('unsafe archive link')
            elif not item.isdir() and not item.isfile():raise ValueError('unsupported archive member type')
    if any(str(parent) in links for name in seen for parent in PurePosixPath(name).parents):
        raise ValueError('archive writes through a symbolic link')
    state.mkdir(parents=True,exist_ok=True,mode=0o700)
    run([engine,'unshare','tar','--numeric-owner','--same-owner','-xf',str(archive.resolve()),'-C',str(state)])
    print('Restored private state; start the service to verify it.')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--config',type=Path,default=ROOT/'server.json')
    p.add_argument('--engine',default='podman')
    sub=p.add_subparsers(dest='operation',required=True)
    for command in ('start','serve','stop','status','load'):sub.add_parser(command)
    for command in ('backup','restore'):sub.add_parser(command).add_argument('archive',type=Path)
    mail=sub.add_parser('mail');mail.add_argument('filename',nargs='?',help='show this private .eml; omit to list names')
    args=p.parse_args()
    if not shutil.which(args.engine):raise ValueError('install rootless Podman first')
    if os.geteuid()==0:raise ValueError('run this server as your normal user with rootless Podman')
    rootless=run([args.engine,'info','--format','{{.Host.Security.Rootless}}'],text=True,stdout=subprocess.PIPE)
    if rootless.stdout.strip()!='true':raise ValueError('a rootless Podman engine is required')
    config=settings(args.config.resolve());manifest=json.loads((ROOT/'bundle.json').read_text())
    owned(args.engine,config['name'])
    lock=None
    if args.operation in ('start','serve','stop','backup','restore'):
        lock=open(args.config.resolve().with_name('.community-operation.lock'),'a')
        os.chmod(lock.name,0o600);fcntl.flock(lock,fcntl.LOCK_EX)
    if args.operation=='start':start(args.engine,config,manifest)
    elif args.operation=='serve':
        start(args.engine,config,manifest)
        fcntl.flock(lock,fcntl.LOCK_UN);lock.close()
        while True:
            result=run([args.engine,'wait',config['name']],text=True,stdout=subprocess.PIPE)
            while backup_active(config):time.sleep(.2)
            if not running(args.engine,config['name']):raise SystemExit(int(result.stdout.strip()))
    elif args.operation=='load':load(args.engine,manifest)
    elif args.operation=='stop':run([args.engine,'stop','--time','30',config['name']],stdout=subprocess.DEVNULL)
    elif args.operation=='status':
        print('running' if running(args.engine,config['name']) else 'stopped')
        print('URL: '+config['public_origin']);print('Private state: '+config['state'])
    elif args.operation=='backup':backup(args.engine,config,args.archive)
    elif args.operation=='restore':restore(args.engine,config,args.archive)
    elif args.operation=='mail':
        if not running(args.engine,config['name']):raise ValueError('start the server to access its local outbox')
        if args.filename:
            if not re.fullmatch(r'[A-Za-z0-9_.-]+\.eml',args.filename):raise ValueError('invalid local message filename')
            command=['cat','/data/outbox/'+args.filename]
        else:command=['find','/data/outbox','-maxdepth','1','-type','f','-printf','%f\n']
        run([args.engine,'exec','--user','aur',config['name'],*command])


if __name__=='__main__':
    try:main()
    except (ValueError,OSError,KeyError,subprocess.CalledProcessError) as error:raise SystemExit('coast-community: '+str(error))
