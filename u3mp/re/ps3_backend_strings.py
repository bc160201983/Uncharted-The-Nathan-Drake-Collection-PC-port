import os
from pathlib import Path
import re
p=Path((os.environ.get('U3MP_PROJECT_DIR','.')+'/reference/previous-project/analysis/ps3-u3-1.19/eboot-image.bin'));d=p.read_bytes()
for m in re.finditer(rb'[ -~]{5,}',d):
 s=m[0]
 if any(x in s.lower() for x in [b'matchsession',b'match-session',b'sessionlan',b'net-session',b'matchmaking',b'sessionpsn']):
  print(hex(m.start()),s[:190].decode())
