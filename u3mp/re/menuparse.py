import os
import struct,json
def load(fn,e,ps):
  d=open(fn,'rb').read()
  u=lambda o:struct.unpack_from(e+'I',d,o)[0]
  p=(lambda o:struct.unpack_from(e+'Q',d,o)[0]) if ps==8 else u
  return d,u,p
def s(d,o):
  if not o or o>=len(d):return None
  e=d.find(b'\0',o);return d[o:e].decode(errors='replace')
def pages_ps4():
  d,u,p=load((os.environ.get('U3MP_WORK_DIR','.')+'/data/menu.bin'),'<',8)
  n=u(0x40);arr=p(0x48);out={}
  for i in range(n):
    e=arr+i*0x80;out[s(d,p(e))]=dict(id=u(e+8),off=e)
  return out
def pages_ps3():
  d,u,p=load((os.environ.get('U3MP_WORK_DIR','.')+'/data/ps3_menu.bin'),'>',4)
  v=u(0x1c+4) if False else None
  return d,u,p
