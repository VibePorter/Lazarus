# Map functions of base image -> TU2 image by .pdata order + normalized code hash.
import struct,sys,hashlib,difflib,json
BASE=0x82000000
def load(p):
    b=open(p,'rb').read()
    e=struct.unpack_from('<I',b,0x3c)[0]; n=struct.unpack_from('<H',b,e+6)[0]; opt=struct.unpack_from('<H',b,e+20)[0]
    o=e+24+opt; secs={}
    for i in range(n):
        name=b[o:o+8].rstrip(b'\0').decode(); vs,va=struct.unpack_from('<II',b,o+8); secs[name]=(va,vs); o+=40
    pva,pvs=secs['.pdata']; funcs=[]
    for i in range(pvs//8):
        beg,fl=struct.unpack_from('>II',b,pva+i*8)
        if beg==0: continue
        ln=((fl>>8)&0x3FFFFF)*4
        funcs.append((beg,ln))
    return b,secs,funcs
def norm(b,beg,ln):
    out=[]
    for a in range(beg,beg+ln,4):
        w=struct.unpack_from('>I',b,a-BASE)[0]; op=w>>26
        if op==18: w&=0xFC000003            # b/bl: displacement
        elif op==16:                         # bc: keep if target inside function
            d=w&0xFFFC; d=d-0x10000 if d&0x8000 else d
            t=a+d if not (w&2) else d
            if not (beg<=t<beg+ln): w&=0xFFFF0003
        elif op==15 and (w&0xFFFF)>=0x8200 and (w&0xFFFF)<0x8400: w&=0xFFFF0000  # lis hi of image address
        elif op in (14,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,58,62) : w&=0xFFFF0000  # D-form imm (may be address lo16)
        out.append(w)
    return hashlib.md5(struct.pack('>%dI'%len(out),*out)).hexdigest()[:16]
def exact(b,beg,ln): return hashlib.md5(b[beg-BASE:beg-BASE+ln]).hexdigest()[:16]
A,sa,fa=load(sys.argv[1]); B,sb,fb=load(sys.argv[2])
ha=[norm(A,*f)+'%x'%f[1] for f in fa]; hb=[norm(B,*f)+'%x'%f[1] for f in fb]
sm=difflib.SequenceMatcher(None,ha,hb,autojunk=False)
mp={}; stats={'equal':0,'replace':0,'delete':0,'insert':0}
changed=[];
for tag,i1,i2,j1,j2 in sm.get_opcodes():
    stats[tag]+=max(i2-i1,j2-j1)
    if tag=='equal':
        for k in range(i2-i1): mp[fa[i1+k][0]]=fb[j1+k][0]
    else:
        changed.append((tag,[ '%08X/%x'%f for f in fa[i1:i2]],['%08X/%x'%f for f in fb[j1:j2]]))
print('base funcs',len(fa),'tu2 funcs',len(fb),stats)
same_addr=sum(1 for k,v in mp.items() if k==v); print('matched',len(mp),'same address',same_addr)
# delta histogram
from collections import Counter
c=Counter(v-k for k,v in mp.items()); print('top deltas',[(hex(d),n) for d,n in c.most_common(12)])
json.dump({'%08X'%k:'%08X'%v for k,v in mp.items()},open(sys.argv[3],'w'))
json.dump(changed,open(sys.argv[3]+'.changed.json','w'),indent=0)
print('changed blocks',len(changed))
for ch in changed[:60]: print(ch[0],ch[1][:6],'->',ch[2][:6])
# pass 2: unique normalized hash among unmatched
from collections import defaultdict
ua=[i for i,f in enumerate(fa) if f[0] not in mp]; mb=set(mp.values()); ub=[j for j,f in enumerate(fb) if f[0] not in mb]
da=defaultdict(list); db=defaultdict(list)
for i in ua: da[ha[i]].append(i)
for j in ub: db[hb[j]].append(j)
p2=0
for h,l in da.items():
    if len(l)==1 and len(db.get(h,[]))==1: mp[fa[l[0]][0]]=fb[db[h][0]][0]; p2+=1
print('pass2 unique-hash matches',p2,'total',len(mp),'of',len(fa))
unm_a=[f for f in fa if f[0] not in mp]; mb=set(mp.values()); unm_b=[f for f in fb if f[0] not in mb]
print('unmatched base',len(unm_a),'unmatched tu2',len(unm_b))
# exact-bytes identical check among matched? count functions whose normalized hash equal (all by construction)
json.dump({'%08X'%k:'%08X'%v for k,v in mp.items()},open(sys.argv[3],'w'))
json.dump({'base':['%08X/%x'%f for f in unm_a],'tu2':['%08X/%x'%f for f in unm_b]},open(sys.argv[3]+'.unmatched.json','w'),indent=0)
