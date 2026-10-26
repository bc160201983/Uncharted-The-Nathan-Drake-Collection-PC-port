exec(open('work/menu_layout.py').read().split('for stride')[0])
rows=[]
for i in range(u32(0x40)):
 o=0x50+i*0x80;col=u64(o+0x48);name=s(u64(o))
 if col>=len(b) or col==0:continue
 count=u32(col);ptr=u64(col+8)
 if count>200 or ptr+count*0x68>len(b):continue
 for j in range(count):
  item=ptr+j*0x68;iname=s(u64(item));iid=u32(item+8);title=s(u64(item+0x20))
  if iid==0xed9d8ae1 or name in ['PAGE_ROOT_MAIN','PAGE_MAIN_MENU','PAGE_MULTIPLAYER','PAGE_MULTIPLAYER_CHOICE','PAGE_COOP_CHOICE']:
   r={'page':name,'page_offset':hex(o),'collection':hex(col),'count':count,'item':j,'item_offset':hex(item),'item_name':iname,'item_id':hex(iid),'title':title,'flags_0x10':hex(u64(item+0x10)),'field_0x18':hex(u64(item+0x18)),'condition_0x40':hex(u32(item+0x40)),'raw':b[item:item+0x68].hex()};rows.append(r);print(r)
pathlib.Path('outputs/evidence/frontend-menu-ownership.json').write_text(json.dumps(rows,indent=2))

