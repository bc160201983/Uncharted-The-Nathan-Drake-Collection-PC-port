from pathlib import Path
import struct,sys,hashlib
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from build_u3_profile_probe import build,IMPORTS,EXPECTED
from build_u3_bootstrap_probe import parse
root=Path(__file__).resolve().parents[2]
raw=(root/'outputs/builds/menu-shell-v1/boot-linked.bin').read_bytes();blob=(root/'work/u3_lan_profiles.bin').read_bytes()
output,m=build(raw,blob);size,ns,sp,np,ip,segs=parse(output);image=output[ip:]
assert hashlib.sha256(raw).hexdigest()==EXPECTED
assert segs[-2][2]==5 and segs[-1][2]==6
old=parse(raw);oldnr,oldni=struct.unpack_from('<2Q',raw,32);nr,ni=struct.unpack_from('<2Q',output,32)
assert nr==oldnr+len(IMPORTS)+4 and ni==oldni+len(IMPORTS)
assert output[np:np+oldni*128]==raw[old[3]:old[3]+oldni*128]
assert output[np+ni*128:np+ni*128+oldnr*32]==raw[old[3]+oldni*128:old[4]]
for wrapper in m['wrappers']:
 site=int(wrapper['site'],16);target=int(wrapper['address'],16)
 assert image[site]==0xe9 and site+5+struct.unpack_from('<i',image,site+1)[0]==target
 code=bytes.fromhex(wrapper['wrapper_hex']);assert image[target:target+len(code)]==code
state=int(m['state'],16)
for i in range(len(IMPORTS)):
 r=struct.unpack_from('<4Q',output,np+ni*128+(oldnr+i)*32)
 assert r==(state+i*8,1,oldni+i,0)
for i,target in enumerate(m['native_bindings']):
 r=struct.unpack_from('<4Q',output,np+ni*128+(oldnr+len(IMPORTS)+i)*32)
 assert r==(state+648+i*8,0,int(target,16),0)
modified=bytearray(raw);modified[-1]^=1
try:build(bytes(modified),blob)
except ValueError:pass
else:raise AssertionError('changed frozen input accepted')
print('PASS: all original import/relocation indices preserved, new provider/native bindings valid, hook destinations correct, RX/RW separated, changed input rejected')