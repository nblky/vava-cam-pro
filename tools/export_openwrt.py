#!/usr/bin/env python3
"""Export self-contained OpenWrt package sources; does not build, install or flash."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
COAP_SHA = 'a7df2e2d688f0dd21511cd6351cc97fc7cf083ea1958b125582159967787dd24'

def export(destination, component):
    if destination.exists() or destination.is_symlink():
        raise ValueError('destination already exists; choose a new directory')
    archive = ROOT/'myvava-sdk/vendor/libcoap-4.3.5b.tar.gz'
    if hashlib.sha256(archive.read_bytes()).hexdigest() != COAP_SHA:
        raise ValueError('pinned libcoap archive SHA-256 mismatch')
    destination.mkdir(parents=True)
    packages = ['vava-agent', 'luci-app-vava-agent'] if component == 'agent' else ['vava-server'] if component == 'server' else ['vava-agent', 'luci-app-vava-agent', 'vava-server']
    for name in packages:
        owner = 'vava-server' if name == 'vava-server' else 'vava-station'
        target = destination/name
        shutil.copytree(ROOT/owner/'openwrt'/name, target)
        if name.startswith('luci-'):
            continue
        source = target/'src'
        source.mkdir()
        shutil.copy2(ROOT/'CMakeLists.txt', source)
        for folder in ('shared', 'myvava-sdk', 'third_party'):
            shutil.copytree(ROOT/folder, source/folder, ignore=shutil.ignore_patterns('__pycache__', '.DS_Store'))
        (source/owner).mkdir()
        shutil.copy2(ROOT/owner/'CMakeLists.txt', source/owner/'CMakeLists.txt')
        shutil.copytree(ROOT/owner/'src', source/owner/'src')
        if name == 'vava-server':
            shutil.copytree(ROOT/owner/'web', source/owner/'web')
            (source/owner/'deploy').mkdir()
            shutil.copy2(ROOT/owner/'deploy/run-server.sh', source/owner/'deploy/run-server.sh')
    (destination/'manifest.json').write_text(json.dumps({'version': '0.10.0', 'packages': packages, 'libcoap_sha256': COAP_SHA, 'contains_credentials': False, 'contains_recordings': False}, indent=2)+'\n')
    return packages

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('destination', type=Path, help='new directory beneath OpenWrt package/, containing the requested packages')
    p.add_argument('--component', choices=('agent', 'server', 'all'), default='agent')
    a = p.parse_args()
    try:
        packages = export(a.destination, a.component)
    except (ValueError, OSError) as e:
        p.error(str(e))
    print(json.dumps({'directory': str(a.destination.resolve()), 'packages': packages}, indent=2))
