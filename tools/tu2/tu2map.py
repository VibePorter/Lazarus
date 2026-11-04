"""Build base->TU2 address map. Items = .pdata functions + gaps; align by normalized code hash
(sequence alignment + unique hash); data map from lis/D-form xrefs in matched items."""
import sys,json,hashlib,difflib,bisect,struct
from collections import defaultdict,Counter
sys.path.insert(0,sys.argv[1]); from img import *
A,sa,fa=load(sys.argv[2]); B,sb,fb=load(sys.argv[3]); out=sys.argv[4]
def items(b,secs,f):
    va,vs=secs['.text']; r=[]; cur=va
    for beg,ln in f:
        if beg>cur: r.append((cur,beg-cur,'gap'))
        r.append((beg,ln,'f')); cur=max(cur,beg+ln)
    if cur<va+vs: r.append((cur,va+vs-cur,'gap'))
    return r
ia=items(A,sa,fa); ib=items(B,sb,fb)
DFORM={14,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,58,62}
def norm(b,beg,ln):
    o=[]
    for a in range(beg,beg+ln,4):
        w=word(b,a); op=w>>26
        if op==18: w&=0xFC000003
        elif op==16:
            d=w&0xFFFC; d=d-0x10000 if d&0x8000 else d
            if not (beg<=a+d<beg+ln): w&=0xFFFF0003
        elif op==15 or op in DFORM: w&=0xFFFF0000
        o.append(w)
    return hashlib.md5(struct.pack('>%dI'%len(o),*o)).hexdigest()[:16]+'/%x'%ln
ha=[norm(A,a,l) for a,l,_ in ia]; hb=[norm(B,a,l) for a,l,_ in ib]
mp={}
for tag,i1,i2,j1,j2 in difflib.SequenceMatcher(None,ha,hb,autojunk=False).get_opcodes():
    if tag=='equal':
        for k in range(i2-i1): mp[i1+k]=j1+k
used=set(mp.values()); da=defaultdict(list); db=defaultdict(list)
for i in range(len(ia)):
    if i not in mp: da[ha[i]].append(i)
for j in range(len(ib)):
    if j not in used: db[hb[j]].append(j)
for h,l in da.items():
    if len(l)==1 and len(db.get(h,()))==1: mp[l[0]]=db[h][0]
print('items',len(ia),len(ib),'matched',len(mp))
# data xrefs
votes=defaultdict(Counter)
def xrefs(b,beg,ln):
    hi={}; r=[]
    for a in range(beg,beg+ln,4):
        w=word(b,a); op=w>>26; rt=(w>>21)&31; ra=(w>>16)&31; imm=w&0xFFFF; simm=imm-0x10000 if imm&0x8000 else imm
        if op==15 and ra==0: hi[rt]=(imm<<16)&0xFFFFFFFF; r.append(None); continue
        if (op in DFORM) and ra in hi and ra!=0:
            ea=(hi[ra]+simm)&0xFFFFFFFF; r.append(ea)
            if op==14 and rt!=ra: pass
            if op in (14,15): hi.pop(rt,None)
            elif op in (32,33,34,35,40,41,42,43,58) : hi.pop(rt,None)
            continue
        r.append(None)
        if op==18 or op==16 or op==19: hi.clear()
    return r
for i,j in mp.items():
    a,l,_=ia[i]; b2,l2,_=ib[j]
    for x,y in zip(xrefs(A,a,l),xrefs(B,b2,l2)):
        if x is not None and y is not None and 0x82000000<=x<0x84000000: votes[x][y]+=1
dmap={x:c.most_common(1)[0][0] for x,c in votes.items()}
print('data anchors',len(dmap))
json.dump({'items_a':ia,'items_b':ib,'map':{str(k):v for k,v in mp.items()},'dmap':{'%08X'%k:'%08X'%v for k,v in dmap.items()},
           'secs_a':sa,'secs_b':sb},open(out,'w'))
