import os
from pathlib import Path
import struct
p=Path((os.environ.get('U3MP_PROJECT_DIR','.')+'/reference/previous-project/analysis/ps3-u3-1.19/eboot-image.bin'));d=p.read_bytes();u=lambda a:struct.unpack_from('>I',d,a)[0];t=u(0x11c2070-0x7a50)
for off in [-0x8000,-0x7ff0,-0x7ff4]:
 v=u(t+off);print(hex(t),hex(off),hex(v),d[v:min(v+50,len(d))].split(b'\0')[0])
