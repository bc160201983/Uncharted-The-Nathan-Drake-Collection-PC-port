"""Build an experimental U3 bootstrap image; originals are read-only.

Adds a separate RX segment, avoiding guessed code caves. Requires the known
1.02 U3 factory, loader, and natural-transition signatures. This is not a LAN
implementation. A UD2 marker stops execution if net cannot register its data.
"""
import argparse,hashlib,json,struct,re
from pathlib import Path

FACTORY=0x66d8c0
LOADER=0x11f85e0
LOOKUP=0x11f2bf0
PROLOGUE=bytes.fromhex('554889e565488b042500000000')
LOADER_PROLOGUE=bytes.fromhex('554889e54157415641554154534883ec18')
NATURAL=bytes.fromhex('498b87b0000000488b80c00000008b7818c745dc8fb4516e')

def parse(raw):
 if raw[:8]!=b'G3PROBE5':raise ValueError('requires G3PROBE5 prepared PC-runtime image')
 size,entry,ns,nr,ni,flags=struct.unpack_from('<6Q',raw,8)
 pos=96
 nm=struct.unpack_from('<Q',raw,pos)[0];pos+=8+nm*56
 nb=struct.unpack_from('<Q',raw,pos)[0];pos+=8+nb*24
 segpos=pos;namespos=segpos+ns*24
 imagepos=namespos+ni*128+nr*32
 if imagepos+size!=len(raw):raise ValueError('invalid prepared image layout')
 segs=[struct.unpack_from('<3Q',raw,segpos+i*24) for i in range(ns)]
 for va,length,prot in segs:
  if va+length>size:raise ValueError('segment exceeds image')
 return size,ns,segpos,namespos,imagepos,segs

def build(raw,entry="natural",pool_blob=None):
 size,ns,segpos,namespos,imagepos,segs=parse(raw)
 image=bytearray(raw[imagepos:])
 if image[FACTORY:FACTORY+len(PROLOGUE)]!=PROLOGUE:raise ValueError('incompatible U3 factory')
 if image[LOADER:LOADER+len(LOADER_PROLOGUE)]!=LOADER_PROLOGUE:raise ValueError('incompatible blocking loader')
 if image.count(NATURAL)!=1:raise ValueError('natural transition signature must match exactly once')
 natural=image.index(NATURAL)
 if not any(va<=FACTORY and FACTORY+len(PROLOGUE)<=va+n and flags&1 for va,n,flags in segs):raise ValueError('factory not executable')
 base=(size+0x3fff)&~0x3fff
 code=bytearray();labels={};fix=[];calls=[]
 def emit(h):code.extend(bytes.fromhex(h))
 def label(name):labels[name]=len(code)
 def call(addr):
  at=base+len(code);emit('e8');code.extend(struct.pack('<i',addr-(at+5)));calls.append(dict(site=hex(at),target=hex(addr)))
 def jump(op,name):
  emit(op);fix.append((len(code),name));code.extend(b'\0'*4)
 def check():
  emit('bfb9b33bbd');call(LOOKUP);emit('4885c0');jump('0f84','need_load')
  emit('48833800');jump('0f85','resume')
 # SysV entry RSP is 8 mod 16. 24-byte frame aligns every nested call.
 # This zero-argument factory reads TLS, not incoming argument registers.
 emit('4883ec18');check();label('need_load')
 emit('c7042412000000 bf2eef4042 4889e6');call(LOADER)
 # Do not accept wrapper return alone: shutdown can terminate its wait.
 emit('bfb9b33bbd');call(LOOKUP);emit('4885c0');jump('0f84','failed')
 emit('48833800');jump('0f84','failed')
 label('resume')
 if pool_blob is not None:
  if not pool_blob or len(pool_blob)>0x3000:raise ValueError('invalid pool helper size')
  at=base+len(code);emit('488d3d');code.extend(struct.pack('<i',-(at+7)))
  call(base+0x200);emit('85c0');jump('0f84','failed')
 emit('4883c418');code.extend(PROLOGUE)
 at=base+len(code);emit('e9');code.extend(struct.pack('<i',FACTORY+len(PROLOGUE)-(at+5)))
 label('failed');emit('0f0b')
 for at,name in fix:struct.pack_into('<i',code,at,labels[name]-(at+4))
 patch=b'\xe9'+struct.pack('<i',base-(FACTORY+5))+b'\x90'*(len(PROLOGUE)-5)
 image[FACTORY:FACTORY+len(PROLOGUE)]=patch
 if entry=='natural':
  image[natural:natural+17]=bytes.fromhex('bf67c9b3b7')+b'\x90'*12
 elif entry=='early':
  def replace(pattern,offset,old,new):
   tokens=pattern.split();regex=b''.join(b'.' if token=='??' else re.escape(bytes.fromhex(token)) for token in tokens)
   hits=list(re.finditer(regex,bytes(image),re.S))
   if len(hits)!=1:raise ValueError('early-entry pattern must match once: '+pattern)
   at=hits[0].start()+offset
   if bytes(image[at:at+len(old)])!=old or len(old)!=len(new):raise ValueError('early-entry bytes mismatch')
   image[at:at+len(old)]=new
  replace('4c8d35 ?? ?? ?? ?? 31ffba010000004c89f641ff174189c4 488d35 ?? ?? ?? ?? 31ffba0100000041ff17 488d0d3efbffff48890d ?? ?? ?? ??',0x28,bytes.fromhex('488d0d3efbffff'),bytes.fromhex('488d0d8e8b0f00'))
  replace('488d0589630701488b00448b400c488d058b780601',0,bytes.fromhex('488d0589630701488b00448b400c'),bytes.fromhex('448b05ad790601')+b'\x90'*7)
  tail='488b034889df5b415c415e415f5dffa0b0010000488b034889df5b415c415e415f5dffa0b8010000'
  # Find once, then update both state selectors together.
  at=bytes(image).find(bytes.fromhex(tail))
  if at<0 or bytes(image).count(bytes.fromhex(tail))!=1:raise ValueError('early state-selection tail not unique')
  image[at+0x10:at+0x14]=bytes.fromhex('f8010000');image[at+0x24:at+0x28]=bytes.fromhex('f8010000')
 else:raise ValueError('unknown entry mode')
 if len(code)>=0x200:raise ValueError('wrapper overlaps helper')
 segment=bytearray(0x4000);segment[:len(code)]=code
 if pool_blob is not None:segment[0x200:0x200+len(pool_blob)]=pool_blob
 image.extend(b'\0'*(base-len(image)));image.extend(segment)
 header=bytearray(raw[:namespos])+struct.pack('<3Q',base,0x4000,5)+raw[namespos:imagepos]
 struct.pack_into('<Q',header,8,len(image));struct.pack_into('<Q',header,24,ns+1)
 output=bytes(header+image)
 newsize,newns,_,_,newpos,newsegs=parse(output)
 assert newns==ns+1 and newsize==len(image) and newsegs[-1]==(base,0x4000,5)
 assert output[newpos+base:newpos+base+len(code)]==code
 assert FACTORY+5+struct.unpack_from('<i',image,FACTORY+1)[0]==base
 for c in calls:
  site=int(c['site'],16);assert site+5+struct.unpack_from('<i',image,site+1)[0]==int(c['target'],16)
 manifest=dict(entry_mode=entry,status='experimental; runtime behavior unverified',input_sha256=hashlib.sha256(raw).hexdigest(),output_sha256=hashlib.sha256(output).hexdigest(),input_size=len(raw),output_size=len(output),factory=hex(FACTORY),loader=hex(LOADER),symbol_lookup=hex(LOOKUP),net_resource_hash='0x4240ef2e',required_symbol_hash='0xbd3bb3b9',allocation_tag='0x12',factory_original=PROLOGUE.hex(),factory_patch=patch.hex(),natural_transition=hex(natural),segment=dict(address=hex(base),size='0x4000',permissions='RX'),stub_hex=code.hex(),labels={k:hex(base+v) for k,v in labels.items()},calls=calls,validation=['container length/count/range checks','unique natural-transition signature','exact factory and loader signatures','new RX segment readback','factory jump target','every injected call target'],limitations=['No transport, matchmaking, authentication or signaling implemented','No game execution success claimed','Existing runtime baseline fails address reservation with Windows error 487','Resource bootstrap may reveal additional missing frontend dependencies','Experimental module lifetime is not integrated with the online teardown path'])
 manifest['limitations'].remove('Existing runtime baseline fails address reservation with Windows error 487')
 if pool_blob is not None:
  assert output[newpos+base+0x200:newpos+base+0x200+len(pool_blob)]==pool_blob
  manifest['pool_helper']=dict(address=hex(base+0x200),size=len(pool_blob),sha256=hashlib.sha256(pool_blob).hexdigest(),npc_capacity=16,pickup_capacity=256,allocation_context='native scope 5, paired push/pop; network ownership lifetime unverified')
 return output,manifest,bytes(code)

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('input',type=Path);p.add_argument('--out',type=Path,required=True);p.add_argument('--entry',choices=['natural','early'],default='early');p.add_argument('--pool-blob',type=Path);a=p.parse_args()
 if a.out.exists():raise FileExistsError('refusing to overwrite output')
 if a.input.resolve()==a.out.resolve():raise ValueError('input/output must differ')
 output,manifest,stub=build(a.input.read_bytes(),entry=a.entry,pool_blob=a.pool_blob.read_bytes() if a.pool_blob else None);a.out.parent.mkdir(parents=True,exist_ok=True)
 a.out.write_bytes(output);a.out.with_suffix('.json').write_text(json.dumps(manifest,indent=2));a.out.with_suffix('.stub.bin').write_bytes(stub)
 print(json.dumps(manifest,indent=2))
if __name__=='__main__':main()
