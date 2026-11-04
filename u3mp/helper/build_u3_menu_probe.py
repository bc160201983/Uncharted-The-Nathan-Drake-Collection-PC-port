"""Isolated native menu-item diagnostic; no online factory substitution."""
import hashlib,struct
from build_u3_bootstrap_probe import parse,PROLOGUE,LOADER_PROLOGUE
FACTORY=0x6496ec
def build(raw,blob,callback=False,flow_blob=None):
 size,ns,segpos,namespos,imagepos,segs=parse(raw)
 image=bytearray(raw[imagepos:])
 original=bytes.fromhex('e83f97ba00')
 if image[FACTORY-5:FACTORY]!=bytes.fromhex('bfebceff74') or image[FACTORY:FACTORY+5]!=original:raise ValueError('menu construction lookup signature mismatch')
 if image[0x11f85e0:0x11f85e0+len(LOADER_PROLOGUE)]!=LOADER_PROLOGUE:raise ValueError('loader mismatch')
 if not blob or len(blob)>0x3000:raise ValueError('helper size')
 base=(size+0x3fff)&~0x3fff
 # Intercept the symbol lookup before menu controls are cloned from DC data.
 # Keep its original return value and normal call ABI; 24-byte frame aligns calls.
 code=bytearray.fromhex('4883ec18')
 at=base+len(code);code.extend(b'\xe8'+struct.pack('<i',0x11f2e30-(at+5)))
 code.extend(bytes.fromhex('48890424 488d3d'))
 code.extend(struct.pack('<i',-(base+len(code)+4)))
 at=base+len(code);code.extend(b'\xe8'+struct.pack('<i',base+0x200-(at+5)))
 code.extend(bytes.fromhex('85c075020f0b488b04244883c418c3'))
 patch=b'\xe8'+struct.pack('<i',base-(FACTORY+5))
 image[FACTORY:FACTORY+5]=patch
 segment=bytearray(0x4000);segment[:len(code)]=code;segment[0x200:0x200+len(blob)]=blob
 if callback:
  expected=bytes.fromhex('c36666666666662e0f1f840000000000')
  if image[0x66e500:0x66e510]!=expected:raise ValueError('Multiplayer callback signature mismatch')
  # Match retained native ShowPage callers. No network-success flag is changed.
  cb=bytearray.fromhex('488d3d');cb.extend(struct.pack('<i',0x17bd080-(base+0x100+7)))
  cb.extend(bytes.fromhex('be44a7661e31d231c9'))
  at=base+0x100+len(cb);cb.extend(b'\xe9'+struct.pack('<i',0x651940-(at+5)))
  segment[0x100:0x100+len(cb)]=cb
  image[0x66e500:0x66e505]=b'\xe9'+struct.pack('<i',base+0x100-(0x66e500+5))
 if flow_blob is not None:
  if not callback or not flow_blob or len(flow_blob)>0x2000:raise ValueError('invalid flow helper')
  if image[0x66e510:0x66e520]!=expected:raise ValueError('LAN callback signature mismatch')
  segment[0x1000:0x1000+len(flow_blob)]=flow_blob
  for site,offset,lan in [(0x66e500,0x100,0),(0x66e510,0x180,1)]:
   at=base+offset
   wrapper=bytearray.fromhex('488d3d');wrapper.extend(struct.pack('<i',-(at+7)))
   wrapper.extend(b'\xbe'+struct.pack('<I',lan))
   jump=at+len(wrapper);wrapper.extend(b'\xe9'+struct.pack('<i',base+0x1000-(jump+5)))
   segment[offset:offset+len(wrapper)]=wrapper
   image[site:site+5]=b'\xe9'+struct.pack('<i',at-(site+5))
  # An empty lobby has no backend/player identity. Decline its player-action
  # callback until a backend exists, instead of dereferencing absent player data.
  guard_site=0x671d00
  displaced=bytes.fromhex('554889e54156534883ec60')
  if image[guard_site:guard_site+len(displaced)]!=displaced:raise ValueError('player action signature mismatch')
  at=base+0x800;guard=bytearray.fromhex('488b05')
  guard.extend(struct.pack('<i',0x17d6428-(at+7)))
  guard.extend(bytes.fromhex('4885c07501c3'));guard.extend(displaced)
  jump=at+len(guard);guard.extend(b'\xe9'+struct.pack('<i',guard_site+len(displaced)-(jump+5)))
  segment[0x800:0x800+len(guard)]=guard
  image[guard_site:guard_site+len(displaced)]=b'\xe9'+struct.pack('<i',at-(guard_site+5))+b'\x90'*(len(displaced)-5)
 image.extend(b'\0'*(base-len(image)));image.extend(segment)
 header=bytearray(raw[:namespos])+struct.pack('<3Q',base,0x4000,5)+raw[namespos:imagepos]
 struct.pack_into('<Q',header,8,len(image));struct.pack_into('<Q',header,24,ns+1)
 result=bytes(header+image);parsed=parse(result)
 assert parsed[0]==len(image) and parsed[1]==ns+1
 assert FACTORY+5+struct.unpack_from('<i',image,FACTORY+1)[0]==base
 return result,dict(status='page flow dependency diagnostic; no session backend' if flow_blob else 'menu callback to retained choice page; networking unfinished' if callback else 'menu visibility diagnostic; Multiplayer callbacks remain stripped',callback_enabled=callback,flow_helper_sha256=hashlib.sha256(flow_blob).hexdigest() if flow_blob else None,input_sha256=hashlib.sha256(raw).hexdigest(),output_sha256=hashlib.sha256(result).hexdigest(),factory=hex(FACTORY),helper=hex(base+0x200),segment=hex(base),stub_hex=code.hex(),helper_sha256=hashlib.sha256(blob).hexdigest())
