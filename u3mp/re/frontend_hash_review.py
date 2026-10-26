import os
import pathlib,struct,re,json
old=pathlib.Path((os.environ.get('U3MP_PROJECT_DIR','.')+'/reference/previous-project'))
b=(old/'analysis/ps4-u3/big3-memory-image.bin').read_bytes();tbl=struct.unpack_from('<256I',b,0x13a6ff0)
def h(s):
 v=0
 for c in s:v=((v<<8)&0xffffffff)^tbl[c^(v>>24)]
 return v
hashes={}
for m in re.finditer(rb'[ -~]{3,200}\0',b):
 s=m[0][:-1];hashes.setdefault(h(s),[]).append((hex(m.start()),s.decode()))
for p in pathlib.Path((os.environ.get('UNCHARTED_GAME_DIR','.')+'/u3data/build/main/dc1/ps4')).glob('*.dci'):
 for s in re.findall(rb'[^\s()]+',p.read_bytes()):hashes.setdefault(h(s),[]).append((str(p.name),s.decode()))
need=[0x6e51b48f,0xeef9ed7b,0xa69a074d,0x37656388,0x53cbb5f2,0x1190ddfa,0x27c0c70a,0x3cbd26ae,0x3ce8ca72,0x6618e562]
for k in need:print(hex(k),hashes.get(k))
f=old/'analysis/ghidra/ps4-u3-bd3bb3b9-users.c';txt=f.read_text();a=txt.index('void FUN_0064ce90');z=txt.find('==========',a);txt=txt[a:z]
rows=[]
for line in txt.splitlines():
 for sign,s in re.findall(r'(-?)(0x[0-9a-f]+)',line):
  val=int(s,16)*(-1 if sign else 1)&0xffffffff
  for found in hashes.get(val,[]):
   row={'line':line.strip(),'hash':hex(val),'name':found[1],'source':found[0]}
   if row not in rows:rows.append(row)
pathlib.Path('outputs/evidence/frontend-event-hash-matches.json').write_text(json.dumps(rows,indent=2))
for r in rows:
 if any(n in r['name'].lower() for n in ['multi','online','frontend','menu','play','coop','signin','login','net-']):print(r)
