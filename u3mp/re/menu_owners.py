import os
import pathlib,struct,json
p=pathlib.Path((os.environ.get('UNCHARTED_GAME_DIR','.')+'/u3data/build/main/dc1/ps4/menu.bin'));b=p.read_bytes();u32=lambda o:struct.unpack_from('<I',b,o)[0];u64=lambda o:struct.unpack_from('<Q',b,o)[0]
for i in range(u32(0x14)):print('export',hex(0x18+i*16),b[0x18+i*16:0x28+i*16].hex(' '))
for target in [0x5b30,0x5c68,0x5fa8,0x6690]:
 print('BUTTON',hex(target));v=0
 while (v:=b.find(target.to_bytes(8,'little'),v))>=0:print('PTRREF',hex(v),b[max(0,v-32):v+40].hex(' '));v+=1
print('HEAD',b[:256].hex(' '))
