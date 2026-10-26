import os
import sys,json,re,collections
sys.path.insert(0,(os.environ.get('U3MP_WORK_DIR','.')+''))
from funcs import load
P3=load('data/ps3_funcs.tsv');P4=load('data/ps4_funcs.tsv')
def norm(strs):
    out=[]
    for s in strs:
        if 'd:\\' in s.lower() or 'd:/' in s.lower(): s=s.replace('\\','/').split('/')[-1]
        out.append(s)
    return tuple(sorted(set(out)))
pairs={}
# 1. unique string-set anchors
def index(F):
    idx=collections.defaultdict(list)
    for a,f in F.items():
        k=norm(f['strs'])
        if len(k)>=1 and sum(len(x) for x in k)>=12: idx[k].append(a)
    return idx
i3,i4=index(P3),index(P4)
for k,v in i3.items():
    if len(v)==1 and len(i4.get(k,[]))==1: pairs[v[0]]=i4[k][0]
print('string anchors',len(pairs))
# 2. seed pairs
seed=json.load(open('data/seed_pairs.json')) if __import__('os').path.exists('data/seed_pairs.json') else {}
for a,b in seed.items(): pairs[int(a,16)]=int(b,16)
# callers index
def callers(F):
    c=collections.defaultdict(list)
    for a,f in F.items():
        for t in f['calls']: c[t].append(a)
    return c
c3,c4=callers(P3),callers(P4)
def align(A,B):
    # pair sequences when same length after removing already-known mismatches
    res=[]
    if len(A)==len(B):
        res=list(zip(A,B))
    return res
changed=True;it=0
inv={}
while changed and it<20:
    changed=False;it+=1
    inv={b:a for a,b in pairs.items()}
    new={}
    for a,b in list(pairs.items()):
        A=[x for x in P3.get(a,{'calls':[]})['calls'] if x in P3];B=[x for x in P4.get(b,{'calls':[]})['calls'] if x in P4]
        for x,y in align(A,B):
            if x in pairs or y in inv: continue
            # sanity: callee count similarity
            nx,ny=len(P3[x]['calls']),len(P4[y]['calls'])
            if abs(nx-ny)>max(2,0.3*max(nx,ny)): continue
            new.setdefault(x,set()).add(y)
    for x,ys in new.items():
        if len(ys)==1:
            y=ys.pop()
            if y not in inv and x not in pairs: pairs[x]=y;inv[y]=x;changed=True
    print('iter',it,len(pairs))
json.dump({hex(a):hex(b) for a,b in pairs.items()},open('data/pairs.json','w'))
