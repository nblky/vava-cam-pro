#!/usr/bin/env python3
"""Check distributable source files without printing matched credentials.

Git mode includes tracked and non-ignored new files, using working-tree contents.
Source exports can be checked without Git. This is a limited pattern check,
not a substitute for manual review or a dedicated secret scanner.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
COAP_PATH = 'myvava-sdk/vendor/libcoap-4.3.5b.tar.gz'
COAP_SHA256 = 'a7df2e2d688f0dd21511cd6351cc97fc7cf083ea1958b125582159967787dd24'
PRIVATE_DIRS = {'.git', '.venv', 'venv', '.deps', 'artifacts', 'run', 'dist', 'private'}
PRIVATE_NAMES = {'devices.keys', 'server-key', 'api-session', 'initial-admin-password'}
PRIVATE_SUFFIXES = {'.key', '.pem', '.p12', '.pfx', '.bin', '.ipk', '.apk',
                    '.pcap', '.pcapng', '.sealed', '.mp4', '.h264', '.aac', '.webm', '.mkv'}
PATTERNS = {
    'private key': re.compile(r'^-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY-----\s*$', re.M),
    'literal bearer credential': re.compile(r'\bBearer [a-fA-F0-9]{64}\b'),
    'PSK table row': re.compile(r'^[A-Za-z0-9_.-]{1,64}[ \t]+[a-fA-F0-9]{64}[ \t]*$', re.M),
    'AWS access key': re.compile(r'\bAKIA[0-9A-Z]{16}\b'),
    'GitHub token': re.compile(r'\b(?:gh[pousr]_[A-Za-z0-9]{36,}|github_pat_[A-Za-z0-9_]{40,})\b'),
}
LOCAL_IDENTIFIERS = re.compile(r'/(?:Users|Volumes)/[^\s]+|192\.168\.118\.\d+|\b64XI[A-Z0-9]{20,}\b')
LINK = re.compile(r'(?<!!)\[([^\]\n]+)\]\(([^)\n]+)\)')


def source_files(root):
    if (root / '.git').exists():
        raw = subprocess.check_output(['git', '-C', str(root), 'ls-files',
                                       '--cached', '--others', '--exclude-standard', '-z'])
        candidates = {Path(p.decode('utf-8')) for p in raw.split(b'\0') if p}
        return sorted(p for p in candidates if (root / p).exists() or (root / p).is_symlink())
    files = []
    for directory, dirs, names in os.walk(root, followlinks=False):
        rel = Path(directory).relative_to(root)
        for name in list(dirs):
            if name in {'__pycache__', '.idea', '.vscode'} or (
                    rel == Path('.') and (name in PRIVATE_DIRS or name.startswith('build'))):
                dirs.remove(name)
            elif (Path(directory) / name).is_symlink():
                files.append(rel / name)
                dirs.remove(name)
        files.extend(rel / name for name in names if name != '.DS_Store')
    return sorted(files)


def secret_findings(data):
    try:
        value = data.decode('utf-8')
    except UnicodeError:
        return []
    return [name for name, pattern in PATTERNS.items() if pattern.search(value)]


def check_source(root, files):
    errors = []
    total = 0
    for rel in files:
        path = root / rel
        name = rel.as_posix()
        if path.is_symlink() or not path.is_file():
            errors.append(f'{name}: symlink or non-regular file')
            continue
        if (rel.parts[0] in PRIVATE_DIRS or rel.parts[0].startswith('build')
                or path.name in PRIVATE_NAMES or path.suffix.lower() in PRIVATE_SUFFIXES
                or re.search(r'\.(?:sqlite3|db)(?:-(?:wal|shm)|-journal)?$', path.name)
                or (path.name.startswith('.env') and path.name != '.env.example')):
            errors.append(f'{name}: runtime data, credentials or generated file')
        size = path.stat().st_size
        total += size
        if size > 5 * 1024 * 1024:
            errors.append(f'{name}: exceeds 5 MiB source-file budget')
            continue
        data = path.read_bytes()
        errors.extend(f'{name}: {kind}' for kind in secret_findings(data))
        try:
            value = data.decode('utf-8')
        except UnicodeError:
            if name != COAP_PATH and path.suffix.lower() not in {'.png', '.jpg', '.jpeg', '.ico', '.webp'}:
                errors.append(f'{name}: unexpected binary')
            continue
        if LOCAL_IDENTIFIERS.search(value):
            errors.append(f'{name}: local machine or device identifier; use an example')
        if path.suffix == '.md':
            for _, target in LINK.findall(value):
                target = target.split(' "', 1)[0].strip('<>')
                if re.match(r'^[a-zA-Z][a-zA-Z0-9+.-]*:', target):
                    continue
                dest = unquote(target.split('#', 1)[0])
                if dest and (Path(dest).is_absolute() or not (path.parent / dest).exists()):
                    errors.append(f'{name}: missing or machine-local Markdown link')
    archive = root / COAP_PATH
    if not archive.is_file() or hashlib.sha256(archive.read_bytes()).hexdigest() != COAP_SHA256:
        errors.append(f'{COAP_PATH}: missing or changed pinned dependency')
    return errors, total


def verify_manifest(root, files):
    manifest = root / 'SOURCE_MANIFEST.json'
    if not manifest.is_file():
        return ['SOURCE_MANIFEST.json: missing manifest']
    try:
        expected = json.loads(manifest.read_text())['files']
        actual = {p.as_posix(): hashlib.sha256((root / p).read_bytes()).hexdigest()
                  for p in files if p.as_posix() != manifest.name and (root / p).is_file()
                  and not (root / p).is_symlink()}
        if expected != actual:
            return ['SOURCE_MANIFEST.json: exported file set or SHA-256 differs']
    except (KeyError, TypeError, ValueError):
        return ['SOURCE_MANIFEST.json: invalid manifest']
    return []


def check_history(root):
    if not (root / '.git').exists():
        raise ValueError('--history requires a Git checkout')
    objects = subprocess.check_output(['git', '-C', str(root), 'rev-list', '--objects', '--all']).splitlines()
    names = dict(row.decode().split(' ', 1) for row in objects if b' ' in row)
    batch = subprocess.check_output(['git', '-C', str(root), 'cat-file', '--batch'],
                                    input=b''.join(oid.encode() + b'\n' for oid in names))
    offset = 0
    scanned = 0
    identifiers = 0
    errors = []
    while offset < len(batch):
        end = batch.index(b'\n', offset)
        oid, kind, size = batch[offset:end].decode().split()
        size = int(size)
        data = batch[end + 1:end + 1 + size]
        offset = end + size + 2
        if kind != 'blob':
            continue
        scanned += 1
        errors.extend(f'history {oid[:12]}:{names[oid]}: {label}' for label in secret_findings(data))
        try:
            identifiers += bool(LOCAL_IDENTIFIERS.search(data.decode('utf-8')))
        except UnicodeError:
            pass
    return errors, scanned, identifiers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--history', action='store_true', help='also scan reachable Git blobs for credential patterns')
    parser.add_argument('--verify-manifest', action='store_true', help='verify an unmodified source export against its SHA-256 manifest')
    args = parser.parse_args()
    try:
        root = args.root.resolve()
        files = source_files(root)
        errors, total = check_source(root, files)
        if args.verify_manifest:
            errors.extend(verify_manifest(root, files))
        if args.history:
            history_errors, scanned, identifiers = check_history(root)
            errors.extend(history_errors)
            print(f'History: {scanned} blobs checked; {identifiers} contain local identifiers (history unchanged).')
        for error in errors:
            print(f'FAIL {error}')
        print(f'Source: {len(files)} files, {total:,} bytes; {len(errors)} problem(s).')
        return bool(errors)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Repository check failed: {type(error).__name__}\n')


if __name__ == '__main__':
    raise SystemExit(main())
