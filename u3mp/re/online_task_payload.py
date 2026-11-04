exec(open('work/taskgraph_decode.py').read().split('rows=[]')[0])
for start,size in [(0x648,0x40),(0xb78,0xa0),(0x908,0x40),(0xde0,0x80)]:
 print('REGION',hex(start))
 for o in range(start,start+size,8):
  v=u64(o);desc=''
  if v<len(b) and v>0:
   end=b.find(b'\0',v)
   s=b[v:end]
   if s and all(32<=c<127 for c in s):desc=s.decode()
  print(hex(o),hex(v),desc)
