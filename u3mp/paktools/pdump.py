import struct
def pdump(p,page,off,n):
  """dump n ptr-size words at page:off, annotate pointers (->page:off) and strings"""
  ps=p.ptrsize;b=p.pbase(page);out=[]
  for k in range(0,n*ps,ps):
    o=off+k
    if p.is_ptr(page,o):
      dp,do=p.resolve(page,o)
      s=p.pstr(dp,do)[:50]
      ok=s and len(s)>1 and all(32<=ord(c)<127 for c in s)
      out.append('%05x: -> p%d:%05x %s'%(o,dp,do,('"%s"'%s) if ok else ''))
    else:
      if ps==8:
        a,c=struct.unpack_from(p.e+'II',p.d,b+o);out.append('%05x: %08x %08x   %s'%(o,a,c,p.d[b+o:b+o+8].hex()))
      else:
        a=p.u32(b+o);out.append('%05x: %08x   f=%g'%(o,a,struct.unpack_from(p.e+'f',p.d,b+o)[0]))
  return '\n'.join(out)

def owner(p,page,off):
  """find the item whose [hdr, data_end) contains page:off"""
  best=None
  for it in p.items:
    if it.page==page and it.off<=off<it.data_off+it.size:
      return it
  # nearest preceding item
  cands=[it for it in p.items if it.page==page and it.off<=off]
  return max(cands,key=lambda i:i.off) if cands else None
