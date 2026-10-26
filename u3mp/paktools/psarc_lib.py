import struct,zlib,sys,os
try: import lzma
except: lzma=None
class Psarc:
  def __init__(s,path):
    s.f=open(path,'rb');h=s.f.read(32)
    magic,ver,comp,tl,es,n,bs,fl=struct.unpack('>4sI4sIIIII',h)
    assert magic==b'PSAR',magic
    s.comp=comp;s.bs=bs;s.flags=fl
    toc=s.f.read(tl-32)
    s.entries=[]
    for i in range(n):
      e=toc[i*es:(i+1)*es]
      md5=e[:16];zi=struct.unpack('>I',e[16:20])[0]
      ln=int.from_bytes(e[20:25],'big');off=int.from_bytes(e[25:30],'big')
      s.entries.append((md5,zi,ln,off))
    nb=1;x=bs
    while x>256: x//=256;nb+=1
    zt=toc[n*es:]
    s.zs=[int.from_bytes(zt[i:i+nb],'big') for i in range(0,len(zt)-nb+1,nb)]
    s.names=s.read(0).decode(errors='replace').replace('\r','').split('\n') if s.entries else []
    if s.names and s.names[-1]=='':s.names.pop()
  def read(s,i):
    md5,zi,ln,off=s.entries[i];out=bytearray();s.f.seek(off);bi=zi
    while len(out)<ln:
      z=s.zs[bi];bi+=1
      if z==0: d=s.f.read(s.bs)
      else:
        d=s.f.read(z)
        if len(d)==min(s.bs,ln-len(out)) and d[:2] not in (b'\x78\xda',b'\x78\x9c',b'\x78\x01'): pass
        else:
          try: d=zlib.decompress(d)
          except Exception:
            if lzma:
              try: d=lzma.decompress(d)
              except Exception: pass
      out+=d
    return bytes(out[:ln])
if __name__=='__main__':
  p=Psarc(sys.argv[1]);cmd=sys.argv[2]
  if cmd=='list':
    for i,n in enumerate(p.names): print(i+1,p.entries[i+1][2],n)
  elif cmd=='extractall':
    out=sys.argv[3]
    for i,n in enumerate(p.names):
      dst=os.path.join(out,n.lstrip('/'));os.makedirs(os.path.dirname(dst),exist_ok=True)
      open(dst,'wb').write(p.read(i+1))
    print('extracted',len(p.names))
