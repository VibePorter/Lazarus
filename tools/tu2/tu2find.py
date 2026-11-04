"""Find TU2 candidates for base addresses. usage: tu2find.py S base tu2 map2 addr..."""
import sys,json,bisect
from collections import Counter,defaultdict
sys.path.insert(0,sys.argv[1]); from img import *
A,_,_=load(sys.argv[2]); B,_,_=load(sys.argv[3]); M=json.load(open(sys.argv[4]))
ia=M['items_a']; ib=M['items_b']; mp={int(k):v for k,v in M['map'].items()}; same=set(M['same'])
dm={int(k,16):int(v,16) for k,v in M['dmap'].items()}
sas=[x[0] for x in ia]; sbs=[x[0] for x in ib]
def item(starts,items,a):
    i=bisect.bisect_right(starts,a)-1
    return i if 0<=i<len(items) and items[i][0]<=a<items[i][0]+items[i][1] else None
def bl_list(b,a0,l):
    r=[]
    for off in range(0,l,4):
        w=word(b,a0+off)
        if w>>26==18 and not (w&2):
            d=w&0x3FFFFFC; d=d-0x4000000 if d&0x2000000 else d
            t=(a0+off+d)&0xFFFFFFFF
            if not (a0<=t<a0+l): r.append((off,t))
    return r
# caller index base
callers=defaultdict(list)
for i,(a0,l,_) in enumerate(ia):
    for k,(off,t) in enumerate(bl_list(A,a0,l)): callers[t].append((i,k,off))
def xrefs(b,a0,l):
    hi={}; r=[]
    for a in range(a0,a0+l,4):
        w=word(b,a); op=w>>26; rt=(w>>21)&31; ra=(w>>16)&31; imm=w&0xFFFF; s=imm-0x10000 if imm&0x8000 else imm
        if op==15 and ra==0: hi[rt]=imm<<16; continue
        if op in (14,32,34,36,38,40,44,48,50,52,54) and ra in hi: r.append((hi[ra]+s)&0xFFFFFFFF)
    return r
# data ref index tu2
drefs=defaultdict(set)
for j,(b0,l,_) in enumerate(ib):
    for x in xrefs(B,b0,l): drefs[x].add(j)
for s in sys.argv[5:]:
    T=int(s,16); i=item(sas,ia,T); a0,l,kind=ia[i]
    print('==',s,'item %08X/%x %s'%(a0,l,kind),'mapped' if i in mp else '', 'same' if i in same else '')
    votes=Counter()
    for (ci,k,off) in callers[T]:
        if ci in mp:
            b0,lb,_=ib[mp[ci]]; bl=bl_list(B,b0,lb)
            if ci in same: 
                for (o2,t2) in bl:
                    if o2==off: votes[t2]+=3
            elif k<len(bl): votes[bl[k][1]]+=1
    print("  callers",len(callers[T]),'votes',[('%08X'%t,n) for t,n in votes.most_common(4)])
    dv=Counter()
    for x in set(xrefs(A,a0,l)):
        if x in dm:
            for j in drefs.get(dm[x],()): dv[j]+=1
    print('  data-ref votes',[('%08X/%x'%(ib[j][0],ib[j][1]),n) for j,n in dv.most_common(4)],'of',len(set(xrefs(A,a0,l))))
