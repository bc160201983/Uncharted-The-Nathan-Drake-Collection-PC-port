import os
import pathlib,json
base=pathlib.Path((os.environ.get('UNCHARTED_GAME_DIR','.')+'/u3data/build/main/dc1/ps4'));keys={'BUTTON_MULTIPLAYER':0xed9d8ae1,'PAGE_MULTIPLAYER':0x53cbb5f2,'online':0x9f2aeafb};rows=[]
for p in base.glob('*.bin'):
 if p.stat().st_size>16*1024*1024:continue
 b=p.read_bytes()
 for n,k in keys.items():
  token=k.to_bytes(4,'little');off=0;hits=[]
  while (off:=b.find(token,off))>=0:hits.append(hex(off));off+=1
  if hits:rows.append({'path':str(p),'name':n,'hash':hex(k),'offsets':hits});print(p.name,n,hits)
pathlib.Path('outputs/evidence/frontend-resource-hash-refs.json').write_text(json.dumps(rows,indent=2))
