"""Validate the isolated menu callback container against the installed source."""
from pathlib import Path
import struct,sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from build_u3_bootstrap_probe import parse
from build_u3_menu_probe import build,FACTORY
raw=Path(sys.argv[1]).read_bytes();blob=Path(sys.argv[2]).read_bytes()
built,manifest=build(raw,blob,callback=True)
_,_,_,_,pos,segs=parse(built);base=segs[-1][0];image=built[pos:]
assert image[FACTORY]==0xe8
assert FACTORY+5+struct.unpack_from('<i',image,FACTORY+1)[0]==base
assert image[0x66e500]==0xe9
assert 0x66e505+struct.unpack_from('<i',image,0x66e501)[0]==base+0x100
assert base+0x107+struct.unpack_from('<i',image,base+0x103)[0]==0x17bd080
assert image[base+0x107:base+0x110]==bytes.fromhex('be44a7661e31d231c9')
assert base+0x115+struct.unpack_from('<i',image,base+0x111)[0]==0x651940
assert image[base+0x200:base+0x200+len(blob)]==blob
assert raw[parse(raw)[4]+0x66e500]==0xc3
for address in (FACTORY,0x66e500):
 changed=bytearray(raw);changed[parse(raw)[4]+address]^=1
 try:build(bytes(changed),blob,callback=True)
 except ValueError:pass
 else:raise AssertionError('altered instruction signature accepted')
print('PASS: native call/branch targets, helper readback, source preservation, incompatible signature rejection')
