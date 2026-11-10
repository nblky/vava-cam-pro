#!/usr/bin/env python3
"""Create a new private server/agent credential bundle; never touch a running device."""
import argparse
import json
import os
from pathlib import Path
import re
import secrets
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('destination', type=Path)
    p.add_argument('--device-id', required=True)
    p.add_argument('--server-binary', type=Path, default=ROOT/'build/vava-server/vava-server')
    a = p.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_.:-]{1,31}', a.device_id):
        p.error('device-id must contain 1–31 ASCII letters, digits or _ . : -')
    if a.destination.exists() or a.destination.is_symlink():
        p.error('destination already exists; existing credentials are never replaced')
    if not a.server_binary.is_file():
        p.error('build vava-server first or specify --server-binary')
    os.umask(0o077)
    a.destination.mkdir(parents=True, mode=0o700)
    for sub in ('config', 'data', 'data/state', 'data/recordings', 'agent'):
        (a.destination/sub).mkdir(mode=0o700)
    key = secrets.token_hex(32)
    (a.destination/'config/devices.keys').write_text(a.device_id+' '+key+'\n')
    (a.destination/'config/initial-admin-password').write_text(secrets.token_urlsafe(24)+'\n')
    (a.destination/'agent/server-key').write_text(key+'\n')
    subprocess.run([str(a.server_binary.resolve()), '--storage-key-create', str((a.destination/'data/record-keys').resolve())], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([str(a.server_binary.resolve()), '--keys-check', str((a.destination/'config/devices.keys').resolve())], check=True, stdout=subprocess.DEVNULL)
    print(json.dumps({'directory': str(a.destination.resolve()), 'device_id': a.device_id,
                      'initial_password_file': str((a.destination/'config/initial-admin-password').resolve()),
                      'agent_key_file': str((a.destination/'agent/server-key').resolve()),
                      'services_changed': False}, ensure_ascii=False, indent=2))
if __name__ == '__main__':
    main()
