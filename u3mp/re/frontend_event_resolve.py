import os
exec(open('work/frontend_hash_review.py').read().split('need=')[0])
base=pathlib.Path((os.environ.get('UNCHARTED_GAME_DIR','.')+'/u3data/build/main/dc1/ps4'))
for f in ['menu.bin','tasks.bin']:
 for m in re.finditer(rb'[ -~]{3,}\0',(base/f).read_bytes()):
  s=m[0][:-1];hashes.setdefault(h(s),[]).append((f+':'+hex(m.start()),s.decode()))
text=(old/'analysis/ghidra/ps4-u3-bd3bb3b9-users.c').read_text();a=text.index('void FUN_0064ce90');z=text.find('==========',a);text=text[a:z]
rows=[]
for no,line in enumerate(text.splitlines(),1):
 for sign,s in re.findall(r'(-?)(0x[0-9a-f]+)',line):
  val=int(s,16)*(-1 if sign else 1)&0xffffffff
  for src,name in hashes.get(val,[]):
   if any(w in name.lower() for w in ['multi','online','menu','coop','lan','deathmatch']):
    r={'line_offset_from_function':no,'line':line.strip(),'hash':hex(val),'name':name,'source':src}
    if r not in rows:rows.append(r);print(r)
pathlib.Path('outputs/evidence/frontend-event-hash-matches.json').write_text(json.dumps(rows,indent=2))
for n in ['BUTTON_MULTIPLAYER','MENU_PLAY_ONLINE','MENU_MULTIPLAYER','BUTTON_MULTIPLAYER_LAN','MENU_LAN_PARTY','PAGE_MULTIPLAYER','PAGE_MULTIPLAYER_CHOICE','LIST_MAIN_MENU']:
 print(n,hex(h(n.encode())))
