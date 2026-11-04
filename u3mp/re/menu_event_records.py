import os
import pathlib,struct
b=pathlib.Path((os.environ.get('UNCHARTED_GAME_DIR','.')+'/u3data/build/main/dc1/ps4/menu.bin')).read_bytes()
for name,val in [('BUTTON_MULTIPLAYER',0xed9d8ae1),('PAGE_MULTIPLAYER',0x53cbb5f2),('LIST_MAIN_MENU',0xf75ffe57)]:
 print(name);p=0
 while (p:=b.find(val.to_bytes(4,'little'),p))>=0:
  print(hex(p),b[max(0,p-16):p+96].hex(' '));p+=1
