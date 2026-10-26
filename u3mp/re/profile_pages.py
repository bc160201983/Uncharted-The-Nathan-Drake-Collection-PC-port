exec(open('work/menu_layout.py').read().split('for stride')[0])
for i in range(12):
 o=0x50+i*0x80;name=s(u64(o));c=u64(o+0x48);count=u32(c);p=u64(c+8)
 print(hex(o),name,'count',count, 'items',[(s(u64(p+j*0x68)),s(u64(p+j*0x68+0x20))) for j in range(count)])
