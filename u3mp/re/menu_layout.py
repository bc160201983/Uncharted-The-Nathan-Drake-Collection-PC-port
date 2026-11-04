exec(open('work/menu_owners.py').read().split('for i in range')[0])
def s(o):
 if o>=len(b):return ''
 q=b[o:b.find(b'\0',o)]
 return q.decode() if q and all(32<=c<127 for c in q) else ''
for stride in [0x80,0x88]:
 print('STRIDE',hex(stride))
 for i in range(8):o=0x50+i*stride;print(hex(o),hex(u64(o)),s(u64(o)))
for start in [0x4ad8,0x5b00,0x5c40,0x5f80,0x6660]:
 print('REGION',hex(start))
 for o in range(start,start+0x80,8):print(hex(o),hex(u64(o)),s(u64(o)))
