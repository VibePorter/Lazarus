"""Local signature resolve for unresolved text addresses. usage: tu2local.py S base tu2 map2 out.json addr..."""
import sys,json,bisect
sys.path.insert(0,sys.argv[1]); from img import *
A,_,_=load(sys.argv[2]); B,_,_=load(sys.argv[3]); M=json.load(open(sys.argv[4]))
ia=M['items_a']; ib=M['items_b']; mp={int(k):v for k,v in M['map'].items()}; same=set(M['same'])
s=[x[0] for x in ia]
DF={14,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,58,62}
def nw(b,a):
    w=word(b,a); op=w>>26
    if op==18: return w&0xFC000003
    if op==16: return w&0xFFFF0003
    if op==15 or op in DF: return w&0xFFFF0000
    return w
def sig(b,a,n=8): return [nw(b,a+4*k) for k in range(n)]
tb,tl=M['secs_b']['.text']
out={}
args=[]
for x in sys.argv[6:]:
    args+= open(x[1:]).read().split() if x.startswith('@') else [x]
for x in args:
    a=int(x,16); i=bisect.bisect_right(s,a)-1
    preds=[]
    for k in list(range(i,max(i-40,-1),-1))+list(range(i+1,min(i+40,len(ia)))):
        if k in mp and k in same: preds.append(ib[mp[k]][0]-ia[k][0])
        if len(preds)>=4: break
    sg=sig(A,a); found=None
    for d in dict.fromkeys(preds):
        p=a+d
        for off in sorted(range(-0x800,0x804,4),key=abs):
            q=p+off
            if tb<=q<tb+tl-32 and sig(B,q)==sg:
                found=q; break
        if found: break
    # uniqueness: ensure no second match within window
    out[x]=('%08X'%found) if found else None
json.dump(out,open(sys.argv[5],'w'))
