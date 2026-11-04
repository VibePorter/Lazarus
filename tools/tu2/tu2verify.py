import sys,json,bisect,struct
from collections import Counter
sys.path.insert(0,sys.argv[1]); from img import *
A,_,_=load(sys.argv[2]); B,_,_=load(sys.argv[3])
M=json.load(open(sys.argv[4])); ia=M['items_a']; ib=M['items_b']; mp={int(k):v for k,v in M['map'].items()}
starts=[x[0] for x in ia]
def tx(a):
    i=bisect.bisect_right(starts,a)-1
    if 0<=i<len(ia) and ia[i][0]<=a<ia[i][0]+ia[i][1] and i in mp: return ib[mp[i]][0]+a-ia[i][0]
    return None
bad={}; stat=Counter()
for i,j in mp.items():
    a0,l,_=ia[i]; b0,_,_=ib[j]; ok=0; mis=0
    for off in range(0,l,4):
        wa=word(A,a0+off); wb=word(B,b0+off)
        if wa>>26==18:
            da=wa&0x3FFFFFC; da=da-0x4000000 if da&0x2000000 else da
            db=wb&0x3FFFFFC; db=db-0x4000000 if db&0x2000000 else db
            ta=(a0+off+da)&0xFFFFFFFF; tb=(b0+off+db)&0xFFFFFFFF
            if a0<=ta<a0+l: continue
            t=tx(ta)
            if t is None: continue
            if t==tb: ok+=1
            else: mis+=1
    stat['ok' if mis==0 else 'bad']+=1
    if mis: bad[i]=(ok,mis)
print(stat)
json.dump({str(k):v for k,v in bad.items()},open(sys.argv[5],'w'))
far=[(ia[i][0],ib[j][0]) for i,j in mp.items() if abs(ib[j][0]-ia[i][0])>0x40000]
print('far matches',len(far), [('%08X'%a,'%08X'%b) for a,b in far[:10]])
for i,(ok,mis) in sorted(bad.items(),key=lambda x:-x[1][1])[:15]: print('%08X->%08X len %x ok %d mis %d'%(ia[i][0],ib[mp[i]][0],ia[i][1],ok,mis))
