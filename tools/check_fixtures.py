#!/usr/bin/env python3
"""Run licensed fixture integration QA; write metadata, timings and inspectable previews."""
import argparse, hashlib, json, pathlib, subprocess
p=argparse.ArgumentParser()
p.add_argument('engine',type=pathlib.Path)
p.add_argument('fixtures',type=pathlib.Path)
p.add_argument('output',type=pathlib.Path)
a=p.parse_args();engine=str(a.engine.resolve());a.output.mkdir(parents=True,exist_ok=True)
manifest=json.loads((pathlib.Path(__file__).resolve().parents[1]/'tests/fixtures.json').read_text())
results=[]
for f in manifest['fixtures']:
    source=a.fixtures/f['filename']
    if not source.exists():continue
    if hashlib.sha256(source.read_bytes()).hexdigest()!=f['sha256']:raise SystemExit('Checksum mismatch: '+str(source))
    metadata=json.loads(subprocess.check_output([engine,'inspect',str(source)]))
    timings=json.loads(subprocess.check_output([engine,'benchmark',str(source),'--iterations','3']))
    previews={}
    # Compare neutral to a small explicit tonal edit, keeping expensive full-resolution denoise disabled.
    for name,recipe in [('neutral',{}),('edited',{'adjustments':{'exposure':.5,'highlights':-15,'shadows':10,'vibrance':10}})]:
        recipepath=a.output/(str(f['id'])+'-'+name+'.json');recipepath.write_text(json.dumps(recipe))
        destination=a.output/(str(f['id'])+'-'+name+'.png')
        subprocess.run([engine,'render',str(source),str(destination),'--recipe',str(recipepath),'--max-dimension','1200'],check=True)
        previews[name]=destination.name
    results.append({'fixture':f['id'],'sha256':f['sha256'],'metadata':metadata,'benchmark':timings,'previews':previews})
if not results:raise SystemExit('No downloaded fixtures found')
(a.output/'report.json').write_text(json.dumps(results,indent=2))
rows=[]
for r in results:
    rows.append(f"<h2>{r['metadata']['make']} {r['metadata']['model']}</h2><p>Neutral / edited</p><div>"+''.join(f'<img width="48%" src="{path}" alt="{name}">' for name,path in r['previews'].items())+'</div>')
(a.output/'index.html').write_text('<!doctype html><meta charset="utf-8"><title>RawEngine fixture QA</title><style>body{max-width:1500px;margin:32px auto;font:16px system-ui;background:#eee;color:#222}img{margin:4px}</style><h1>RawEngine fixture QA</h1><p>Visual checks, not a measured color-accuracy report.</p>'+''.join(rows))
print(a.output/'report.json')
