#!/usr/bin/env python3
import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess

REMOTE = r"""import base64,glob,hashlib,json,os
root='/home/mingwei/noc_project/nmu-standalone/cosim'
os.chdir(root)
names=['SHA256SUMS','constants.yml','profile.yml','patterns/cases.list','pattern.txt']
names+=glob.glob('build/report_wave*/**/*.log',recursive=True)+glob.glob('build/report_wave*/**/source-SHA256SUMS',recursive=True)
names+=glob.glob('build/buffer-pipeline/**/*.log',recursive=True)+glob.glob('build/buffer-pipeline/**/*.json',recursive=True)+glob.glob('build/buffer-pipeline/*.csv')
names+=glob.glob('patterns/**/*.txt',recursive=True)+glob.glob('patterns/**/manifest.json',recursive=True)
names+=['topology.yml','signals.rc']
files=[]
for name in names:
    with open(name,'rb') as f:data=f.read()
    files.append(dict(path=name,sha256=hashlib.sha256(data).hexdigest(),data=base64.b64encode(data).decode()))
artifacts=[]
for name in glob.glob('build/dpi*/libnmu_cmodel.so')+glob.glob('build/vcs_wave*/simv')+glob.glob('build/report_wave*/**/*.fsdb',recursive=True):
    with open(name,'rb') as f:digest=hashlib.sha256(f.read()).hexdigest()
    artifacts.append(dict(path=name,sha256=digest,size=os.stat(name).st_size,mtime=os.stat(name).st_mtime))
print('REPORTS_JSON='+json.dumps(dict(files=files,artifacts=artifacts)))
"""
ssh=['/mnt/c/Windows/System32/OpenSSH/ssh.exe','-i',r'C:\Users\user\.ssh\noc_workstation_ed25519',
     '-o','BatchMode=yes','-o','ConnectTimeout=10','mingwei@172.16.16.16']
code=base64.b64encode(REMOTE.encode()).decode()
command='python3 -c "import base64;exec(base64.b64decode(\''+code+'\'))"'
r=subprocess.run(ssh+[command],stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True,timeout=60)
record=json.loads(next(line[len('REPORTS_JSON='):] for line in r.stdout.decode().splitlines() if line.startswith('REPORTS_JSON=')))
parser=argparse.ArgumentParser()
parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent/'remote')
out=parser.parse_args().output.resolve()
out.mkdir(exist_ok=True)
for entry in record['files']:
    target=(out/entry['path']).resolve()
    if not target.is_relative_to(out):raise ValueError('invalid report path')
    data=base64.b64decode(entry['data'])
    assert hashlib.sha256(data).hexdigest()==entry['sha256']
    target.parent.mkdir(parents=True,exist_ok=True)
    target.write_bytes(data)
(out/'artifacts.json').write_text(json.dumps(record['artifacts'],indent=2)+'\n')
(out/'REPORT_SHA256SUMS').write_text(''.join(entry['sha256']+'  '+entry['path']+'\n' for entry in record['files']))
print('Verified downloaded reports/inputs:',len(record['files']))
