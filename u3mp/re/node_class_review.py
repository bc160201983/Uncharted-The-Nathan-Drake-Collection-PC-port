import os
exec(open('work/online_task_nodes.py').read().split('for start,end')[0])
for o in [0x14c68,0x14d50,0x23468]:
 print('NODE',hex(o))
 for off in range(o,o+0xe8,4):
  val=u32(off)
  if val:print(hex(off-o),hex(val),hashes.get(val))
# Fixed-ID frontend events from handler can match strings in native DC resources.
for f in ['tasks.bin','menu.bin','bp_ucctweaks_script.bin']:
 data=(base/f).read_bytes() if False else pathlib.Path((os.environ.get('UNCHARTED_GAME_DIR','.')+'/u3data/build/main/dc1/ps4'),f).read_bytes()
 for m in re.finditer(rb'[A-Za-z%*_][A-Za-z0-9%*_.:/?! -]{2,}\0',data):
  s=m[0][:-1]
  if any(k in s.lower() for k in [b'net',b'online',b'multi',b'match',b'frontend',b'coop',b'menu']):print(f,hex(m.start()),s.decode())
