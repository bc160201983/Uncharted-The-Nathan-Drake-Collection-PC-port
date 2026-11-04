import os
from pathlib import Path
import re,json,hashlib
old=Path((os.environ.get('U3MP_PROJECT_DIR','.')+'/reference/previous-project'))
ps3=(old/'analysis/ps3-u3-1.19/eboot-image.bin').read_bytes();ps4=(old/'analysis/ps4-u3/big3-memory-image.bin').read_bytes()
controls=[b'NetMatchmakingClientHello %i',b'NetMatchmakingServerHello %i',b'NetMatchmakingRoomCreate %i',b'NetMatchmakingSetRoomFlags %i',b'NetMatchmakingUpdatedRoomFlags %i',b'net-session.cpp']
rows=[]
for needle in controls:
 def hits(d):return [hex(m.start()) for m in re.finditer(re.escape(needle),d)]
 rows.append({'needle':needle.decode(),'ps3':hits(ps3),'ps4':hits(ps4)})
print(json.dumps(rows,indent=2))
imports=json.loads((old/'analysis/ps3-u3-1.19/imports.json').read_text());wanted={0xd7fc34,0xd7fc74,0xd7fb54,0xd7fbd4,0xd7fd34,0xd7fb94,0xd7fc54}
selected=[i for i in imports if int(i['stub'],16) in wanted]
out={'ps3_sha256':hashlib.sha256(ps3).hexdigest(),'ps4_sha256':hashlib.sha256(ps4).hexdigest(),'bounded_string_comparison':rows,'ps3_transport_imports':selected,'limitations':['String presence/absence does not establish callable backend availability.','No PS4 project opened; string-only comparison.']}
Path('outputs/evidence/ps3-network-service-bootstrap-string-comparison.json').write_text(json.dumps(out,indent=2));print(selected)
