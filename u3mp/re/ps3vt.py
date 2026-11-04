import sys,re,struct;sys.path.insert(0,(os.environ.get('U3MP_WORK_DIR','.')+''))
from ps3lib import *
def f2v(f): return f+0x10000 if f<0x10b71e8 else f-0x10c0000+0x10d0000
_starts=None
def starts():
  global _starts
  if _starts is None:
    _starts=[]
    prev=[]
    for line in open((os.environ.get('U3MP_WORK_DIR','.')+'/ps3.s')):
      m=re.match(r'\s+([0-9a-f]+):\s+(\S+)\s*(.*)',line)
      if not m:continue
    # use OPD table: entries (func,TOC)
    tb=struct.pack('>I',TOC)
    i=D.find(tb)
    s=set()
    while i!=-1:
      if i>=4:
        f=struct.unpack_from('>I',D,i-4)[0]
        if 0x10000<=f<0x10b0000: s.add((f,f2v(i-4)))
      i=D.find(tb,i+1)
    _starts=sorted(s)
  return _starts
def func_of(a):
  import bisect
  st=starts();i=bisect.bisect_right(st,(a,1<<40))-1
  return st[i]
def refs(v):
  t=struct.pack('>I',v);j=D.find(t);out=[]
  while j!=-1: out.append(f2v(j));j=D.find(t,j+1)
  return out
if __name__=='__main__':
  for x in sys.argv[1:]:
    a=int(x,16);f,o=func_of(a);print(hex(a),'func',hex(f),'opd',hex(o))
    for r in refs(o):
      print('  ref',hex(r),[hex(r32(r+4*k) or 0) for k in range(-3,10)])
