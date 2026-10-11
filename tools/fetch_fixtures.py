#!/usr/bin/env python3
"""Fetch opted-in CC0 integration fixtures outside the repository, verifying SHA-256."""
import argparse, hashlib, json, pathlib, urllib.request
p=argparse.ArgumentParser()
p.add_argument('destination', type=pathlib.Path)
p.add_argument('--ids', nargs='+', type=int, default=[2010,3098])
a=p.parse_args()
manifest=json.loads((pathlib.Path(__file__).resolve().parents[1]/'tests/fixtures.json').read_text())
a.destination.mkdir(parents=True, exist_ok=True)
for f in manifest['fixtures']:
    if f['id'] not in a.ids: continue
    dest=a.destination/f['filename']
    if not dest.exists():
        url=urllib.request.quote(f['url'], safe=':/')
        with urllib.request.urlopen(url, timeout=60) as response:
            data=response.read()
        if hashlib.sha256(data).hexdigest()!=f['sha256']: raise SystemExit('Checksum mismatch: '+f['filename'])
        dest.write_bytes(data)
    if hashlib.sha256(dest.read_bytes()).hexdigest()!=f['sha256']: raise SystemExit('Checksum mismatch: '+str(dest))
    print(dest)
