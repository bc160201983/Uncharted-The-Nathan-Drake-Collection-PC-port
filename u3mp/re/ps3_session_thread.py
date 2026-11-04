import os
from pathlib import Path
import struct
p=Path((os.environ.get('U3MP_PROJECT_DIR','.')+'/reference/previous-project/analysis/ps3-u3-1.19/eboot-image.bin'));d=p.read_bytes();u=lambda a:struct.unpack_from('>I',d,a)[0];t=u(0x11ba558)
for off in [-0x783c,-0x7838,-0x7d80]:
 v=u(t+off);print(hex(off),hex(v),d[v:v+55].hex(),d[v:v+55])
 if v+8<len(d):print('words',hex(u(v)),hex(u(v+4)))
