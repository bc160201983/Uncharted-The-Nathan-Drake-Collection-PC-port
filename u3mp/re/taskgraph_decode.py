import os
import pathlib,struct,re,json
base=pathlib.Path((os.environ.get('UNCHARTED_GAME_DIR','.')+'/u3data/build/main/dc1/ps4'));b=(base/'tasks.bin').read_bytes();u32=lambda o:struct.unpack_from('<I',b,o)[0];u64=lambda o:struct.unpack_from('<Q',b,o)[0]
print('graph',b[0x1b8:0x1e0].hex(' '));root=u64(0x1b8);count=u32(root);ids=u64(root+8);nodes=u64(root+16);print(count,hex(ids),hex(nodes))
rows=[]
for i in range(count):
 node=u64(nodes+i*8)
 if node>=len(b):continue
 nameptr=u64(node);name=b[nameptr:b.find(b'\0',nameptr)].decode('ascii','replace') if nameptr<len(b) else ''
 row={'index':i,'id':hex(u32(ids+i*4)),'offset':hex(node),'name':name,'raw':b[node:node+0x38].hex()};rows.append(row)
 if any(w in name.lower() for w in ['menu','multi','online','top','game-start']):print(row)
pathlib.Path('outputs/evidence/taskgraph-node-inventory.json').write_text(json.dumps(rows,indent=2))

