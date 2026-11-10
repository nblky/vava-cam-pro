#!/usr/bin/env python3
"""Export the standalone static frontend. Never embeds API credentials."""
import argparse
import json
import shutil
from pathlib import Path
from urllib.parse import urlsplit

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('destination', type=Path, help='new static web directory')
parser.add_argument('--api-base', default='', help='management server HTTP(S) base URL or same-origin proxy path')
args = parser.parse_args()
base = args.api_base.strip().rstrip('/')
url = urlsplit(base)
if base and (url.query or url.fragment or url.username or url.password or '\\' in base or
             any(c.isspace() for c in base) or
             not ((url.scheme in ('http', 'https') and url.hostname) or
                  (base.startswith('/') and not base.startswith('//'))) or
             any(p in ('.', '..') for p in url.path.split('/'))):
    parser.error('api-base must be an HTTP(S) URL or absolute proxy path without credentials/query/fragment')
if args.destination.exists():
    parser.error('destination already exists; choose a new directory')
root = Path(__file__).resolve().parents[1]
args.destination.mkdir(parents=True)
for name in ('index.html', 'style.css', 'api-client.js', 'management.js', 'app.js'):
    shutil.copy2(root / 'vava-server/web' / name, args.destination / name)
(args.destination / 'config.js').write_text('window.VAVA_CONFIG = ' + json.dumps({'apiBase': base}) + ';\n')
print(args.destination.resolve())
