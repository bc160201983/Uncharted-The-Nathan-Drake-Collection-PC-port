import os
exec(open('work/frontend_hash_review.py').read().split('need=')[0])
x=(pathlib.Path((os.environ.get('UNCHARTED_GAME_DIR','.')+'/u3data/build/main/dc1/ps4/tasks.bin'))).read_bytes();u32=lambda o:struct.unpack_from('<I',x,o)[0];u64=lambda o:struct.unpack_from('<Q',x,o)[0]
for start,end in [(0x2680,0x26b0),(0x3b50,0x3ba0)]:
 for o in range(start,end,4):
  val=u32(o);print(hex(o),hex(val),hashes.get(val))
for o in [u64(0x2688),u64(0x2690)]:
 print('NODE',hex(o))
 for off in range(o,o+0x70,8):
  val=u64(off);desc=''
  if val<len(x) and val>0:
   s=x[val:x.find(b'\0',val)]
   if s and all(32<=c<127 for c in s):desc=s.decode()
  print(hex(off),hex(val),desc,hashes.get(val&0xffffffff))

