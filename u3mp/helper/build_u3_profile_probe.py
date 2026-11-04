"""Add native IME/file-provider bindings and restored LAN profile callbacks.

Input is the frozen menu-shell-v1 prepared image. Originals remain read-only.
"""
from pathlib import Path
import struct,hashlib
from build_u3_bootstrap_probe import parse
IMPORTS=[('NUeBrN7hzf0','sceImeDialogInit','libSceImeDialog'),('IADmD4tScBY','sceImeDialogGetStatus','libSceImeDialog'),('x01jxu+vxlc','sceImeDialogGetResult','libSceImeDialog'),('gyTyVn+bXMw','sceImeDialogTerm','libSceImeDialog'),('1G3lF1Gg1k8','sceKernelOpen','libkernel'),('Cg4srZ6TKbU','sceKernelRead','libkernel'),('4wSze92BhLI','sceKernelWrite','libkernel'),('UK2Tl2DWUns','sceKernelClose','libkernel'),('52NcYU9+lEo','sceKernelRename','libkernel')]
# Native PS4 functions exposed to the helper as ProfileState.natives[i] (u3_lan_profiles.c NATIVE_*).
NATIVES=[0x623660,0x7503c0,0x720610,0x750a90,0xf32ae0,0x752cf0,0x753330,0x66e9b0,0x72eca0,0x11fd4c0,0x66b1d0,0x66b510,0x7a2360,0x11f2e30,0x64ce90,0x76bbf0,0x7699e0,0x76a580,0x11f2170,0x5ada60,0x1189430,0x781ea0,0x753b70,0x6882b0,0x7bb470,0x7bba70,0x751b30,0x7270f0,0x780220,0x7596a0,0x687d30,0x775d30,0x7bda50,0x74eba0,0x7800d0,0x7bca00,0x7bd6f0,0x7bdc70,0xf32e00,0x771cf0,0x10e95b0,0x10eb990,0x10eb1d0,0x761ea0,0x7508f0,0x10f25b0,0x5b7020,0x5b4e60]
# Bluepoint gated every non-whitelisted menu datasource (all but 0x63/0xcf/0xe1) behind
# net-game+9 (0x17d5c61, global multiplayer-active flag, never set on PS4). Gate them on
# net-game+8 (0x17d5c60, LAN flag, set when LAN Party starts) instead: cmp byte [reg+9],0 -> [reg+8].
DS_GATES=[int(x,16) for x in '0x764045 0x764191 0x7642f5 0x7644e9 0x764639 0x7647b5 0x764901 0x764a51 0x764ba5 0x764cf9 0x764e57 0x764fa9 0x7650ff 0x765249 0x7653b9 0x765519 0x76568f 0x7657d5 0x765925 0x765a81 0x765be9 0x765d31 0x765e79 0x765fbb 0x766105 0x7703b2 0x7704d5'.split()]
EXPECTED='8979cc0d2e0590d61bdf7935ab8c29e617010aa2aa096584cd868d004058554b'
def build(raw,blob,pool_blob=None):
 if hashlib.sha256(raw).hexdigest()!=EXPECTED:raise ValueError('requires frozen menu-shell-v1 input')
 if not blob or len(blob)>0xe000:raise ValueError('profile helper size')
 if pool_blob is None:pool_blob=(Path(__file__).resolve().parent.parent/'work/u3_pool_init.bin').read_bytes()
 if not pool_blob or len(pool_blob)>0x1000:raise ValueError('pool helper size')
 size,ns,sp,np,ip,segs=parse(raw);nr,ni=struct.unpack_from('<2Q',raw,32)
 rx=(size+0x3fff)&~0x3fff;rw=rx+0x10000;state=rw+0x100;helper=rx+0x1000;pool=rx+0xf000
 image=bytearray(raw[ip:]);code=bytearray(0x10000)
 code[0x1000:0x1000+len(blob)]=blob
 code[0xf000:0xf000+len(pool_blob)]=pool_blob
 guards={0x672c60:bytes.fromhex('c36666666666662e0f1f840000000000'),0x673450:bytes.fromhex('c36666666666662e0f1f840000000000'),0x76f4d0:bytes.fromhex('554889e541574156415453'),0x672c30:bytes.fromhex('c36666666666662e0f1f840000000000'),0x66e560:bytes.fromhex('c36666666666662e0f1f840000000000'),0x66edf0:bytes.fromhex('c36666666666662e0f1f840000000000'),0x672c50:bytes.fromhex('c36666666666662e0f1f840000000000'),0x672450:bytes.fromhex('c36666666666662e0f1f840000000000')}
 wrappers=[]
 for site,offset,kind in [(0x672c60,0,0),(0x673450,0x80,1),(0x76f4d0,0x100,2),(0x672c30,0x200,3),(0x66e560,0x280,6),(0x66edf0,0x300,9),(0x672c50,0x800,11),(0x672450,0x880,12)]:
  expected=guards[site]
  if image[site:site+len(expected)]!=expected:raise ValueError('profile hook signature mismatch '+hex(site))
  at=rx+offset;w=bytearray.fromhex('4883ec18 48893c24 4889fe')
  here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',-(here+7)))
  w.extend(b'\xba'+struct.pack('<I',kind))
  here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7)))
  here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',helper-(here+5)))
  if kind==2:
   w.extend(bytes.fromhex('85c0'));branch=len(w);w.extend(b'\x0f\x85'+bytes(4))
   w.extend(bytes.fromhex('488b3c244883c418'));w.extend(expected)
   here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',site+len(expected)-(here+5)))
   struct.pack_into('<i',w,branch+2,len(w)-(branch+6))
  w.extend(bytes.fromhex('4883c418c3'))
  if len(w)>0x80:raise ValueError('wrapper overlap')
  code[offset:offset+len(w)]=w
  image[site:site+5]=b'\xe9'+struct.pack('<i',at-(site+5))
  wrappers.append(dict(site=hex(site),address=hex(at),kind=kind,original=expected.hex(),wrapper_hex=w.hex()))

 # WaitReady (update 0x67f240) starts a Cinema film recording through stream 0x17c3ba8; the PS4
 # film path setup is missing (path "/-4851"), the open fails and 0x6a2110 dereferences the null
 # bitstream. Skip film recording: the function only builds the film title and starts it.
 if image[0x67f240:0x67f244]!=bytes.fromhex('554889e5'):raise ValueError('film recording hook mismatch')
 image[0x67f240]=0xc3

 # HUD-net (class "David", vtable 0x16dafe0) slot 3 = PS3 0x6603f4: draws a script-set HUD image for the
 # local player. Bluepoint removed the draw and left int 0x41 at 0xc3b822, reached whenever a local
 # net player tracker exists. Replace the trap with a 2-byte NOP (nothing drawn).
 if image[0xc3b822:0xc3b824]!=b'\xcd\x41':raise ValueError('hud trap mismatch')
 image[0xc3b822:0xc3b824]=b'\x66\x90'

 # MP entity-spawn filter 0x72fbc0 binary-searches the entity's tag table (entity+0x68: {data,count})
 # without a count check. PS4 level data stores empty tables with a null data pointer (PS3 had a valid
 # pointer past the end), so MP level loads fault at 0x72fd62. Point empty tables at a one-entry
 # sentinel (key 0xffffffff, never matches) before the call; count stays 0, so PS3 semantics are kept.
 tgt=0x72fbc0;at=rx+0x420;sent=state+0x2100
 w=bytearray.fromhex('488b4668 4885c0 7416 48833800 7510')
 here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',sent-(here+7)))
 w.extend(bytes.fromhex('c701ffffffff 488908'))
 here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',tgt-(here+5)))
 assert w[7]==0x74 and w[13]==0x75 and 9+0x16==len(w)-5 and 15+0x10==len(w)-5
 code[0x420:0x420+len(w)]=w
 for site in (0x56561c,0x722939,0x72f9ea,0x730eeb,0x732d09,0x73a6e3,0x73b0d4,0x73b21a,0x73ed30,0x744578,0x777513,0x7778a2):
  if image[site]!=0xe8 or site+5+struct.unpack_from('<i',image,site+1)[0]!=tgt:raise ValueError('tag filter site mismatch '+hex(site))
  image[site:site+5]=b'\xe8'+struct.pack('<i',at-(site+5))
 wrappers.append(dict(site='12 sites',address=hex(at),kind='empty-tag-table',wrapper_hex=w.hex()))

 # DIAGNOSTIC (temporary): along the PS4 respawn/spawn chain record {count, low32(ret), esi arg} at
 # state+0x2200+i*16. Works on every thread (debug-register watches only see the main thread).
 # 0 0x6b4c0f->0x70d180 local respawn request   1 0x70d71f->0x6b41e0 send RespawnPlayer
 # 2 0x6e9e9c->0x732bd0 / 3 0x6e9eae->0x730a00 spawner pick   4 0x6e8dc4->0x7854c0 net player spawn
 # 5..11 callers of NetPlayer spawn 0x787a00   12 0x787cf2->0x7838b0 GetSpawnLocator
 # 13/14 process spawns 0x119b250 inside net player update 0x789a60 (0x78aed2 = character)
 diag=((0x6b4c0f,0x70d180),(0x70d71f,0x6b41e0),(0x6e9e9c,0x732bd0),(0x6e9eae,0x730a00),(0x6e8dc4,0x7854c0),
  (0x7248a4,0x787a00),(0x7388aa,0x787a00),(0x738bc8,0x787a00),(0x738dbc,0x787a00),(0x739cfb,0x787a00),(0x739d2b,0x787a00),(0x73c532,0x787a00),
  (0x787cf2,0x7838b0),(0x78a32b,0x119b250),(0x78aed2,0x119b250))
 for i,(site,tgt) in enumerate(diag):
  if image[site]!=0xe8 or site+5+struct.unpack_from('<i',image,site+1)[0]!=tgt:raise ValueError('diag site mismatch '+hex(site))
  at=rx+0x600+i*0x20;cnt=state+0x2200+i*16
  w=bytearray.fromhex('4883ec08')
  here=at+len(w);w.extend(b'\x89\x35'+struct.pack('<i',cnt+8-(here+6)))
  here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',tgt-(here+5)))
  here=at+len(w);w.extend(b'\xff\x05'+struct.pack('<i',cnt-(here+6)))
  here=at+len(w);w.extend(b'\x89\x05'+struct.pack('<i',cnt+4-(here+6)))
  w.extend(bytes.fromhex('4883c408c3'));assert len(w)<=0x20
  code[0x600+i*0x20:0x600+i*0x20+len(w)]=w
  image[site:site+5]=b'\xe8'+struct.pack('<i',at-(site+5))

 # Late-join check 0x7462b0 indexes the team table with the leading-team result of 0x737eb0
 # without checking for -1 (tie, e.g. 0-0 at match start). On a tie the leading score equals any
 # team's score, so map -1 to team 0 at that call site (0x74654e).
 site=0x74654e;tgt=0x737eb0
 if image[site]!=0xe8 or site+5+struct.unpack_from('<i',image,site+1)[0]!=tgt:raise ValueError('tie guard hook mismatch')
 at=rx+0x400;w=bytearray.fromhex('4883ec08')
 here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',tgt-(here+5)))
 w.extend(bytes.fromhex('83f8ff 7502 31c0 4883c408 c3'))
 code[0x400:0x400+len(w)]=w
 image[site:site+5]=b'\xe8'+struct.pack('<i',at-(site+5))
 wrappers.append(dict(site=hex(site),address=hex(at),kind='tie-guard',wrapper_hex=w.hex()))

 # Guard the emblem setup call in match setup (0x72c17c call 0x66b1d0) through helper kind 10.
 site=0x72c17c
 if image[site]!=0xe8 or site+5+struct.unpack_from('<i',image,site+1)[0]!=0x66b1d0:raise ValueError('emblem hook mismatch')
 at=rx+0x3c0;w=bytearray.fromhex('4883ec18 48893c24 4889fe')
 here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',-(here+7)))
 w.extend(b'\xba'+struct.pack('<I',10))
 here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7)))
 here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',helper-(here+5)))
 w.extend(bytes.fromhex('4883c418c3'))
 if len(w)>0x40:raise ValueError('emblem wrapper size')
 code[0x3c0:0x3c0+len(w)]=w
 image[site:site+5]=b'\xe8'+struct.pack('<i',at-(site+5))
 wrappers.append(dict(site=hex(site),address=hex(at),kind=10,wrapper_hex=w.hex()))

 # Tick the restored net clock (state+0x2000) with the engine clocks: the last per-frame
 # Clock::Update call (clock 9, 0x10ff75c) goes through a stub that updates clock 9 then the net clock.
 site=0x10ff75c;tick=0x1197090
 if image[site]!=0xe8 or site+5+struct.unpack_from('<i',image,site+1)[0]!=tick:raise ValueError('clock tick hook mismatch')
 at=rx+0x380;w=bytearray.fromhex('4883ec08')
 here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',tick-(here+5)))
 here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',state+0x2000-(here+7)))
 here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',tick-(here+5)))
 w.extend(bytes.fromhex('4883c408c3'))
 code[0x380:0x380+len(w)]=w
 image[site:site+5]=b'\xe8'+struct.pack('<i',at-(site+5))
 wrappers.append(dict(site=hex(site),address=hex(at),kind='net-clock-tick',wrapper_hex=w.hex()))

 # Wrap the already restored menu lookup before the native controls are built.
 site=0x6496ec;old_target=site+5+struct.unpack_from('<i',image,site+1)[0]
 if image[site]!=0xe8 or old_target!=0x2050000:raise ValueError('frozen menu construction hook mismatch')
 at=rx+0x500;w=bytearray.fromhex('4883ec18')
 here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',old_target-(here+5)))
 w.extend(bytes.fromhex('48890424 4889c6'))
 here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',-(here+7)))
 w.extend(b'\xba'+struct.pack('<I',8))
 here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7)))
 here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',helper-(here+5)))
 w.extend(bytes.fromhex('488b04244883c418c3'));code[0x500:0x500+len(w)]=w
 image[site:site+5]=b'\xe8'+struct.pack('<i',at-(site+5))
 # v4.20: the datasource gate patch (net-game+9 -> +8) is no longer applied. Since v4.6 the helper raises
 # the real multiplayer-active byte (net-game+9) at LAN start, as PS3 NetInit 0x290ca0 does, so the
 # original dispatcher check passes unmodified. DS_GATES is kept above only as documentation.
 image.extend(bytes(rx-len(image)));image.extend(code);image.extend(bytes(0x4000))
 oldnames=raw[np:np+ni*128];oldrelocs=raw[np+ni*128:ip]
 assert len(oldrelocs)==nr*32
 names=b''.join((nid+'#'+library).encode().ljust(128,b'\0') for nid,_,library in IMPORTS)
 native_bindings=[0x651940,0x11f2bf0,0x11f85e0,pool]
 natives=NATIVES
 relocs=b''.join(struct.pack('<4Q',state+i*8,1,ni+i,0) for i in range(len(IMPORTS)))+b''.join(struct.pack('<4Q',state+648+i*8,0,target,0) for i,target in enumerate(native_bindings))+b''.join(struct.pack('<4Q',state+768+i*8,0,target,0) for i,target in enumerate(natives))
 added_relocs=len(IMPORTS)+len(native_bindings)+len(natives)
 header=bytearray(raw[:sp]+raw[sp:np]+struct.pack('<3Q',rx,0x10000,5)+struct.pack('<3Q',rw,0x4000,6)+oldnames+names+oldrelocs+relocs)
 struct.pack_into('<Q',header,8,len(image));struct.pack_into('<Q',header,24,ns+2);struct.pack_into('<Q',header,32,nr+added_relocs);struct.pack_into('<Q',header,40,ni+len(IMPORTS))
 output=bytes(header+image);new=parse(output)
 assert new[0]==len(image) and new[1]==ns+2 and new[5][-2:]==[(rx,0x10000,5),(rw,0x4000,6)]
 return output,dict(status='LAN profile to resource-loading to local lobby experiment; no session backend',source_sha256=EXPECTED,sha256=hashlib.sha256(output).hexdigest(),helper_sha256=hashlib.sha256(blob).hexdigest(),code=hex(rx),state=hex(state),native_bindings=[hex(v) for v in native_bindings],natives=[hex(v) for v in natives],pool_helper_sha256=hashlib.sha256(pool_blob).hexdigest(),imports=[dict(nid=n,name=s,library=l) for n,s,l in IMPORTS],wrappers=wrappers,profile_file='/data/u3-lan-profiles.v1',limitations=['Profile list adapter is restored; native session identity initialization still pending','Local PC profile persistence uses a versioned format; PS3 save data is not transplanted'])