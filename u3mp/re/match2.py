import sys,json,collections,os
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
def index(F):
    idx=collections.defaultdict(list)
    for a,f in F.items():
        k=norm(f['strs'])
        if k and sum(len(x) for x in k)>=12: idx[k].append(a)
    return idx
i3,i4=index(P3),index(P4)
for k,v in i3.items():
    if len(v)==1 and len(i4.get(k,[]))==1: pairs[v[0]]=i4[k][0]
for a,b in json.load(open('data/seed_pairs.json')).items(): pairs[int(a,16)]=int(b,16)
def c3list(a): return [x for x in P3[a]['calls'] if x in P3] if a in P3 else []
def c4list(b): return [x for x in P4[b]['calls'] if x in P4] if b in P4 else []
callers3=collections.defaultdict(list);callers4=collections.defaultdict(list)
for a,f in P3.items():
    for t in f['calls']:callers3[t].append(a)
for a,f in P4.items():
    for t in f['calls']:callers4[t].append(a)
def sim(x,y,inv):
    if x in pairs: return 6 if pairs[x]==y else -6
    if y in inv: return -6
    nx,ny=len(P3[x]['calls']),len(P4[y]['calls'])
    s=0
    if abs(nx-ny)<=max(1,0.25*max(nx,ny)): s+=1
    else: s-=2
    sx,sy=P3[x]['size'],P4[y]['size']
    r=(sx+16)/(sy+16)
    if 0.4<r<3.0: s+=1
    else: s-=2
    if norm(P3[x]['strs'])==norm(P4[y]['strs']) and P3[x]['strs']: s+=2
    return s
def dp(A,B,inv):
    n,m=len(A),len(B)
    if n*m>40000: return []
    G=-1
    S=[[0]*(m+1) for _ in range(n+1)];T=[[0]*(m+1) for _ in range(n+1)]
    for i in range(1,n+1):S[i][0]=i*G;T[i][0]=1
    for j in range(1,m+1):S[0][j]=j*G;T[0][j]=2
    for i in range(1,n+1):
        for j in range(1,m+1):
            d=S[i-1][j-1]+sim(A[i-1],B[j-1],inv)
            u=S[i-1][j]+G;l=S[i][j-1]+G
            if d>=u and d>=l:S[i][j]=d;T[i][j]=0
            elif u>=l:S[i][j]=u;T[i][j]=1
            else:S[i][j]=l;T[i][j]=2
    i,j=n,m;out=[]
    while i>0 and j>0:
        t=T[i][j]
        if t==0:out.append((A[i-1],B[j-1]));i-=1;j-=1
        elif t==1:i-=1
        else:j-=1
    return out
for it in range(12):
    inv={b:a for a,b in pairs.items()}
    votes=collections.defaultdict(collections.Counter)
    for a,b in list(pairs.items()):
        for x,y in dp(c3list(a),c4list(b),inv):
            if x in pairs or y in inv: continue
            if sim(x,y,inv)>=2: votes[x][y]+=1
        # callers too
        for x,y in dp(sorted(callers3.get(a,[])),sorted(callers4.get(b,[])),inv):
            if x in pairs or y in inv: continue
            if sim(x,y,inv)>=2: votes[x][y]+=0.5
    added=0;claimed=collections.Counter()
    cands=[]
    for x,c in votes.items():
        (y,v),*rest=c.most_common()
        if rest and rest[0][1]==v: continue
        cands.append((v,x,y))
    cands.sort(reverse=True)
    for v,x,y in cands:
        if y in inv or x in pairs: continue
        pairs[x]=y;inv[y]=x;added+=1
    print('iter',it,'added',added,'total',len(pairs))
    if not added:break
json.dump({hex(a):hex(b) for a,b in pairs.items()},open('data/pairs.json','w'))
