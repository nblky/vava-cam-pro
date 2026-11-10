#!/usr/bin/env python3
"""Export checked working-tree source, including non-ignored new files, without Git history."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

from check_repository import ROOT, check_source, source_files, verify_manifest


def export(destination):
    files = source_files(ROOT)
    errors, _ = check_source(ROOT, files)
    if errors:
        raise ValueError('source check failed:\n' + '\n'.join(errors))
    if destination.exists() or destination.is_symlink():
        raise ValueError('destination already exists; choose a new directory')
    destination.mkdir(parents=True)
    manifest = {}
    for rel in files:
        if rel.as_posix() == 'SOURCE_MANIFEST.json':
            continue
        target = destination / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / rel, target)
        manifest[rel.as_posix()] = hashlib.sha256(target.read_bytes()).hexdigest()
    (destination / 'SOURCE_MANIFEST.json').write_text(json.dumps(
        {'format': 1, 'files': manifest}, ensure_ascii=False, indent=2) + '\n')
    exported_files = source_files(destination)
    errors, _ = check_source(destination, exported_files)
    errors.extend(verify_manifest(destination, exported_files))
    if errors:
        raise ValueError('export verification failed:\n' + '\n'.join(errors))
    return len(manifest)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path, help='a new directory for source without .git or private data')
    args = parser.parse_args()
    try:
        count = export(args.destination.expanduser().absolute())
    except (ValueError, OSError) as error:
        parser.error(str(error))
    print(f'Exported {count} source files to {args.destination.resolve()}')
