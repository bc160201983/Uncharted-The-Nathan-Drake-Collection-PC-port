import os
import re,sys
def scan(want):
  lis={};hits={}
  for line in open((os.environ.get('U3MP_WORK_DIR','.')+'/ps3.s')):
    m=re.match(r'\s+([0-9a-f]+):\s+(\S+)\s+(.*)',line)
    if not m:continue
    a=int(m[1],16);op=m[2];args=m[3]
    p=[x.strip() for x in args.split(',')]
    if op=='lis' and len(p)==2:
      try: lis[p[0]]=((int(p[1])&0xffff)<<16,a)
      except: pass
    elif op in('ori','addi') and len(p)==3 and p[1] in lis and a-lis[p[1]][1]<80:
      hi=lis[p[1]][0]
      try: lo=int(p[2])
      except: continue
      v=(hi|(lo&0xffff)) if op=='ori' else (hi+lo)&0xffffffff
      if v in want:hits.setdefault(v,[]).append(a)
    elif op=='li' and len(p)==2:
      pass
  return hits
