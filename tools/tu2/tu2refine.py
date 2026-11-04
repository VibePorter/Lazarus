"""Refine item map with call-site and pointer votes. usage: tu2refine.py S base.bin tu2.bin map.json out.json"""
import sys,json,bisect,struct
from collections import Counter,defaultdict
sys.path.insert(0,sys.argv[1]); from img import *
A,sa,_=load(sys.argv[2]); B,sb,_=load(sys.argv[3])
M=json.load(open(sys.argv[4])); ia=M['items_a']; ib=M['items_b']; mp={int(k):v for k,v in M['map'].items()}
dm={int(k,16):int(v,16) for k,v in M['dmap'].items()}; dk=sorted(dm)
sa_=M['secs_a']; sas=[x[0] for x in ia]; sbs=[x[0] for x in ib]
tva,tvs=sa_['.text']
def item_of(starts,items,a):
    i=bisect.bisect_right(starts,a)-1
    return i if 0<=i<len(items) and items[i][0]<=a<items[i][0]+items[i][1] else None
def btarget(b,a):
    w=word(b,a)
    if w>>26!=18: return None
    d=w&0x3FFFFFC; d=d-0x4000000 if d&0x2000000 else d
    return (a+d)&0xFFFFFFFF if not (w&2) else d&0xFFFFFFFF
def dx(a):
    if a in dm: return dm[a]
    k=bisect.bisect_right(dk,a)-1
    if k<0 or k+1>=len(dk): return None
    lo,hi=dk[k],dk[k+1]
    if dm[lo]-lo==dm[hi]-hi and hi-lo<0x400: return a+dm[lo]-lo
    return None
def secname(secs,a):
    for n,(va,vs) in secs.items():
        if va<=a<va+vs: return n
for it in range(4):
    def tx(a):
        i=item_of(sas,ia,a)
        if i is None or i not in mp: return None
        return ib[mp[i]][0]+a-ia[i][0]
    # consistency of current pairs
    good=set()
    for i,j in mp.items():
        a0,l,_=ia[i]; b0=ib[j][0]; mis=0
        for off in range(0,l,4):
            ta=btarget(A,a0+off)
            if ta is None or a0<=ta<a0+l: continue
            tb=btarget(B,b0+off); t=tx(ta)
            if t is not None and t!=tb: mis+=1
        if mis==0: good.add(i)
    votes=defaultdict(Counter)
    for i in good:
        a0,l,_=ia[i]; b0=ib[mp[i]][0]
        for off in range(0,l,4):
            ta=btarget(A,a0+off)
            if ta is None or a0<=ta<a0+l: continue
            tb=btarget(B,b0+off)
            if tb is None: continue
            ii=item_of(sas,ia,ta); jj=item_of(sbs,ib,tb)
            if ii is None or jj is None: continue
            if ta-ia[ii][0]==tb-ib[jj][0]: votes[ii][jj]+=1
    # pointer evidence in data sections
    for name in ('.rdata','.data'):
        va,vs=sa_[name]
        for p in range(va,va+vs-3,4):
            v=word(A,p)
            if not (tva<=v<tva+tvs): continue
            ii=item_of(sas,ia,v)
            if ii is None or ia[ii][0]!=v: continue
            q=dx(p)
            if q is None: continue
            try: w=word(B,q)
            except Exception: continue
            jj=item_of(sbs,ib,w)
            if jj is not None and ib[jj][0]==w: votes[ii][jj]+=1
    new={}
    for i in range(len(ia)):
        c=votes.get(i)
        if c:
            j,n=c.most_common(1)[0]; tot=sum(c.values())
            if n/tot>=0.6 and ia[i][1]==ib[j][1] or (n>=3 and n/tot>=0.8): new[i]=j; continue
        if i in mp and i in good: new[i]=mp[i]
    # keep injective
    inv=defaultdict(list)
    for i,j in new.items(): inv[j].append(i)
    for j,l in inv.items():
        if len(l)>1:
            for i in l:
                if mp.get(i)!=j: new.pop(i,None)
    changed=sum(1 for i in new if mp.get(i)!=new[i]); lost=sum(1 for i in mp if i not in new)
    mp=new; print('iter',it,'matched',len(mp),'changed',changed,'dropped',lost,'good',len(good))
M['map']={str(k):v for k,v in mp.items()}
json.dump(M,open(sys.argv[5],'w'))
