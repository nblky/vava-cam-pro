#!/usr/bin/env python3
"""Offline configuration snapshots and credential staging; never changes running services."""
import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import stat

STATE_FILES={'registry.bin','settings.bin','policies.bin','profiles.bin','pairing.bin','nas-config.bin','retention.bin'}
LIMIT=32*1024*1024

def private_directory(path):
    s=path.lstat()
    if not stat.S_ISDIR(s.st_mode) or s.st_uid!=os.getuid() or s.st_mode&0o077:
        raise ValueError(f'private owned directory required: {path}')

def read_private(path):
    fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK)
    try:
        before=os.fstat(fd)
        if not stat.S_ISREG(before.st_mode) or before.st_uid!=os.getuid() or before.st_mode&0o077 or before.st_nlink!=1 or before.st_size>LIMIT:
            raise ValueError(f'unsafe or oversized file: {path}')
        data=bytearray()
        while len(data)<=LIMIT:
            b=os.read(fd,min(65536,LIMIT+1-len(data)))
            if not b:break
            data+=b
        after=os.fstat(fd)
        if len(data)!=before.st_size or (before.st_size,before.st_mtime_ns,before.st_ino)!=(after.st_size,after.st_mtime_ns,after.st_ino):
            raise ValueError(f'file changed during read: {path}')
        return bytes(data)
    finally:os.close(fd)

def write_new(path,data):
    path.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
    fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    try:
        with os.fdopen(fd,'wb') as f:f.write(data);f.flush();os.fsync(f.fileno())
    except BaseException:raise
    d=os.open(path.parent,os.O_RDONLY|os.O_DIRECTORY)
    try:os.fsync(d)
    finally:os.close(d)

@contextlib.contextmanager
def stopped_state(path):
    private_directory(path)
    fd=os.open(path/'daemon.lock',os.O_RDWR|os.O_CREAT|os.O_NOFOLLOW,0o600)
    try:
        s=os.fstat(fd)
        if not stat.S_ISREG(s.st_mode) or s.st_uid!=os.getuid() or s.st_mode&0o077 or s.st_nlink!=1:raise ValueError('unsafe daemon lock')
        try:fcntl.lockf(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError:raise ValueError('stop the station daemon before taking a configuration snapshot') from None
        yield
    finally:os.close(fd)

def snapshot(state,credentials,output):
    private_directory(credentials)
    with stopped_state(state):
        files={}
        for name in sorted(STATE_FILES):
            p=state/name
            if p.exists() or p.is_symlink():files['state/'+name]=read_private(p)
        for root,dirs,names in os.walk(credentials,followlinks=False):
            private_directory(Path(root))
            for name in dirs:private_directory(Path(root)/name)
            for name in names:
                p=Path(root)/name;files['credentials/'+str(p.relative_to(credentials))]=read_private(p)
            if len(files)>1024 or sum(map(len,files.values()))>LIMIT:raise ValueError('configuration snapshot limit exceeded')
        output.mkdir(mode=0o700)
        for name,data in files.items():write_new(output/name,data)
        manifest={'format':'myvava-configuration-snapshot/1','files':{n:{'sha256':hashlib.sha256(b).hexdigest(),'bytes':len(b)} for n,b in files.items()}}
        write_new(output/'manifest.json',(json.dumps(manifest,indent=2,sort_keys=True)+'\n').encode())
    return {'ok':True,'snapshot':str(output.resolve()),'files':len(files),'includes_recordings':False}

def restore(source,output):
    private_directory(source);manifest=json.loads(read_private(source/'manifest.json'))
    if manifest.get('format')!='myvava-configuration-snapshot/1' or not isinstance(manifest.get('files'),dict) or len(manifest['files'])>1024:raise ValueError('unsupported snapshot')
    files={}
    for name,entry in manifest['files'].items():
        p=Path(name)
        if str(p)!=name or p.is_absolute() or any(x in ('','..','.') for x in p.parts) or len(p.parts)<2 or p.parts[0] not in ('state','credentials'):raise ValueError('invalid snapshot path')
        if p.parts[0]=='state' and (len(p.parts)!=2 or p.parts[1] not in STATE_FILES):raise ValueError('unexpected station state file')
        parent=source
        for part in p.parts[:-1]:parent/=part;private_directory(parent)
        data=read_private(source/p)
        if len(data)!=entry['bytes'] or hashlib.sha256(data).hexdigest()!=entry['sha256']:raise ValueError('snapshot checksum mismatch')
        files[name]=data
        if sum(map(len,files.values()))>LIMIT:raise ValueError('snapshot size limit exceeded')
    output.mkdir(mode=0o700)
    for name,data in files.items():write_new(output/name,data)
    return {'ok':True,'restored_to_new_directory':str(output.resolve()),'files':len(files),'activation':'keep service stopped; configure the new state and credential paths before restart'}

def credential_bundle(registry,device,output):
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,63}',device):raise ValueError('invalid device identity')
    raw=read_private(registry).decode('ascii');entries={}
    for line in raw.splitlines():
        parts=line.split()
        if not parts:continue
        if len(parts)!=2 or not re.fullmatch(r'[A-Za-z0-9_-]{1,63}',parts[0]) or not re.fullmatch(r'[0-9a-fA-F]{64}',parts[1]) or parts[0] in entries:raise ValueError('invalid registry')
        entries[parts[0]]=parts[1].lower()
    if len(entries)>16 or len(set(entries.values()))!=len(entries):raise ValueError('invalid or duplicate device secrets')
    replacing=device in entries
    if not replacing and len(entries)==16:raise ValueError('device limit reached')
    key=secrets.token_hex(32);entries[device]=key;output.mkdir(mode=0o700)
    write_new(output/'station-key',(key+'\n').encode())
    write_new(output/'server-keys',''.join(f'{d} {k}\n' for d,k in sorted(entries.items())).encode())
    write_new(output/'deployment.json',(json.dumps({'device':device,'replacing':replacing,'previous_registry_sha256':hashlib.sha256(raw.encode()).hexdigest(),'steps':['Stop the selected station service.','Install station-key as its configured private server key file.','Run myvava-server --keys-check on server-keys, then replace the registry atomically and SIGHUP the server.','Restart the selected station and verify its authenticated connection.','Old upload targets stay quarantined; do not silently retarget historical cloud tasks.']},indent=2)+'\n').encode())
    return {'ok':True,'device':device,'staged_bundle':str(output.resolve()),'replacing':replacing,'services_changed':False}

def main():
    parser=argparse.ArgumentParser(description=__doc__);sub=parser.add_subparsers(dest='command',required=True)
    p=sub.add_parser('snapshot');p.add_argument('--state-dir',type=Path,required=True);p.add_argument('--credentials-dir',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p=sub.add_parser('restore');p.add_argument('snapshot',type=Path);p.add_argument('--output',type=Path,required=True)
    p=sub.add_parser('stage-credential');p.add_argument('--registry',type=Path,required=True);p.add_argument('--device',required=True);p.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();os.umask(0o077)
    try:
        result=snapshot(args.state_dir,args.credentials_dir,args.output) if args.command=='snapshot' else restore(args.snapshot,args.output) if args.command=='restore' else credential_bundle(args.registry,args.device,args.output)
        print(json.dumps(result,ensure_ascii=False,indent=2));return 0
    except (OSError,ValueError,KeyError,TypeError) as exc:parser.exit(1,str(exc)+'\n')
if __name__=='__main__':raise SystemExit(main())
