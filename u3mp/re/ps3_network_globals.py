import os
from pathlib import Path
import struct
p=Path((os.environ.get('U3MP_PROJECT_DIR','.')+'/reference/previous-project/analysis/ps3-u3-1.19/eboot-image.bin'));d=p.read_bytes()
u=lambda a:struct.unpack_from('>I',d,a)[0]
t=u(0x11ba558)
for off in [-0x7e08,-0x7d80,-0x7de0,-0x7dc4,-0x7dd8,-0x7d2c]:print(hex(off),hex(u(t+off)))
