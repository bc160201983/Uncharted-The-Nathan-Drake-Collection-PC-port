import os
import struct,re,json
D=open((os.environ.get('U3MP_WORK_DIR','.')+'/img/ps3_eboot.elf'),'rb').read()
TOC=0x11c2070
def v2f(v):
  if 0x10000<=v<0x10000+0x10b71e8: return v-0x10000
  if 0x10d0000<=v<0x10d0000+0xfe7b8: return v-0x10d0000+0x10c0000
  return None
def r32(v):
  f=v2f(v)
  return struct.unpack_from('>I',D,f)[0] if f is not None else None
def opd(v):
  c=r32(v);return c
def lines(a,b):
  out=[]
  for line in open((os.environ.get('U3MP_WORK_DIR','.')+'/ps3.s')):
    m=re.match(r'\s+([0-9a-f]+):\s+(\S+)\s*(.*)',line)
    if not m:continue
    x=int(m[1],16)
    if x<a:continue
    if x>=b:break
    out.append((x,m[2],m[3]))
  return out
def calls(a,b,targets=None):
  """Yield (addr,target,regs) for bl in [a,b) tracking li/lis/ori/oris and TOC-relative r30 loads."""
  regs={};r30=None
  for x,op,args in lines(a,b):
    p=[y.strip() for y in args.split(',')]
    try:
      if op=='lwz' and p[0]=='30' and p[1].endswith('(2)'):
        r30=r32(TOC+int(p[1][:-3]));continue
      if op=='li':regs[p[0]]=int(p[1])&0xffffffff
      elif op=='lis':regs[p[0]]=(int(p[1])<<16)&0xffffffff
      elif op=='ori' and p[1] in regs:regs[p[0]]=regs[p[1]]|(int(p[2])&0xffff)
      elif op=='oris' and p[1] in regs:regs[p[0]]=regs[p[1]]|((int(p[2])&0xffff)<<16)
      elif op=='addi' and p[1] in regs:regs[p[0]]=(regs[p[1]]+int(p[2]))&0xffffffff
      elif op=='mr' and p[1] in regs:regs[p[0]]=regs[p[1]]
      elif op=='lwz' and p[1].endswith('(30)') and r30:
        regs[p[0]]=r32(r30+int(p[1][:-4]))
      elif op=='lwz' and p[1].endswith('(2)'):
        regs[p[0]]=r32(TOC+int(p[1][:-3]))
      elif op=='bl':
        t=int(p[0].split()[0],16)
        if targets is None or t in targets: yield x,t,dict(regs)
    except (ValueError,TypeError): pass
