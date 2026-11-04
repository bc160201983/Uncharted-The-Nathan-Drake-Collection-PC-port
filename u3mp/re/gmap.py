import os
import sys,json,collections,struct
sys.path.insert(0,(os.environ.get('U3MP_WORK_DIR','.')+''))
from funcs import load
from ps3lib import r32
P3=load('data/ps3_funcs.tsv');P4=load('data/ps4_funcs.tsv')
pairs={int(a,16):int(b,16) for a,b in json.load(open('data/pairs.json')).items()}
def res3(x):
    # resolve TOC/RO pointer slots to target globals
    if 0x111b5a8<=x<0x11bdaf0:
        v=r32(x)
        if v and (0x10d0000<=v<0x1442690): return v
    return x
def d3(a): return [res3(x) for x in P3[a]['data'] if 0x10d0000<=x<0x1442690]
def d4(b): return [x for x in P4[b]['data'] if 0x16e4000<=x<0x1df0000]
votes=collections.defaultdict(collections.Counter)
for a,b in pairs.items():
    if a not in P3 or b not in P4: continue
    A=d3(a);B=d4(b)
    A=[x for x in dict.fromkeys(A)];B=[x for x in dict.fromkeys(B)]
    if len(A)==len(B) and 0<len(A)<=12:
        for x,y in zip(A,B): votes[x][y]+=1
g={}
for x,c in votes.items():
    (y,v),*rest=c.most_common()
    if v>=1 and (not rest or rest[0][1]<v): g[x]=(y,v,sum(c.values()))
json.dump({hex(a):[hex(b),v,t] for a,(b,v,t) in g.items()},open('data/globals.json','w'))
print(len(g))
