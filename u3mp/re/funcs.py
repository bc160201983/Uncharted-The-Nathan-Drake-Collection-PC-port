import re
def load(path):
    F={};cur=None
    for l in open(path,encoding='utf-8',errors='replace'):
        l=l.rstrip('\n')
        if re.match(r'^[0-9a-f]+\t\d+\t',l):
            if cur:F[cur[0]]=cur[1]
            p=l.split('\t');cur=(int(p[0],16),p)
        elif cur: cur[1][-1]+=' '+l
    if cur:F[cur[0]]=cur[1]
    out={}
    for a,p in F.items():
        while len(p)<5:p.append('')
        out[a]=dict(size=int(p[1]),strs=[x for x in p[2].split('\x01') if x],calls=[int(x,16) for x in p[3].split(',') if re.match(r'^[0-9a-f]+$',x)],data=[int(x,16) for x in p[4].split(',') if re.match(r'^[0-9a-f]+$',x)])
    return out
