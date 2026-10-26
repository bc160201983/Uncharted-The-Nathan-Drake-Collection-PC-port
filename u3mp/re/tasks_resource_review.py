import os
import pathlib,re,struct
base=pathlib.Path((os.environ.get('UNCHARTED_GAME_DIR','.')+'/u3data/build/main/dc1/ps4'));b=(base/'tasks.bin').read_bytes();print('size',len(b),'head',b[:64].hex(' '))
for m in re.finditer(rb'[ -~]{3,}\0',b):print(hex(m.start()),m[0][:-1].decode())
