exec(open('work/frontend_hash_review.py').read().split('need=')[0])
for name,value in [('BUTTON_MULTIPLAYER',0xed9d8ae1),('PAGE_MULTIPLAYER_CHOICE',0x1e66a744),('MENU_PROFILE',0)]:
 token=value.to_bytes(4,'little');p=0;hits=[]
 while value and (p:=b.find(token,p))>=0:hits.append(hex(p));p+=1
 print(name,hits)
