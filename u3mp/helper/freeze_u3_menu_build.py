"""Freeze an observed runtime test image for the manual launcher."""
from pathlib import Path
import argparse,hashlib,json,shutil
from build_u3_bootstrap_probe import parse
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('run',type=Path);p.add_argument('destination',type=Path)
a=p.parse_args()
result=json.loads((a.run/'result.json').read_text())
if result['exit_code'] not in (0,1) or result['mode']!='menu-flow':raise ValueError('not a completed non-crashing menu-flow run')
raw=(a.run/'boot-linked.bin').read_bytes();parse(raw)
digest=hashlib.sha256(raw).hexdigest()
if digest!=result['patched_sha256']:raise ValueError('run image digest mismatch')
if a.destination.exists():raise FileExistsError('refusing to replace a frozen build')
a.destination.mkdir(parents=True)
(a.destination/'boot-linked.bin').write_bytes(raw)
shutil.copyfile(a.run/'patch-manifest.json',a.destination/'patch-manifest.json')
manifest=dict(sha256=digest,source_run=str(a.run.resolve()),test_exit=result['exit_code'],test_timed_out=result['timed_out'],preserved_pages=['Multiplayer choice','Online lobby shell','Choose LAN Profile'],limitations=['No session backend','Lobby has no dynamic member/game list','LAN profile create callback unfinished'],policy='Do not overwrite this build; promote a verified successor explicitly')
(a.destination/'build.json').write_text(json.dumps(manifest,indent=2))
print(json.dumps(manifest,indent=2))
