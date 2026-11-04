import os
from pathlib import Path
import struct,json
p=Path((os.environ.get('U3MP_PROJECT_DIR','.')+'/reference/previous-project/analysis/ps3-u3-1.19/eboot-image.bin'));d=p.read_bytes();u=lambda a:struct.unpack_from('>I',d,a)[0]
rows=[]
for label,outer,off in [('online',-0x7a4c,-0x7f70),('lan',-0x7a50,-0x7ff8)]:
 t=u(0x11c2070+outer);v=u(t+off);methods=[]
 for i in range(28):
  desc=u(v+i*4)
  if not 0<desc<len(d)-8:break
  code,toc=u(desc),u(desc+4)
  methods.append({'slot':hex(i*4),'descriptor':hex(desc),'code':hex(code),'toc':hex(toc)})
 rows.append({'mode':label,'toc':hex(t),'vtable':hex(v),'methods':methods})
print(json.dumps(rows,indent=2));Path('work/ps3-backend-vtables.json').write_text(json.dumps(rows,indent=2))
