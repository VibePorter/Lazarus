# For each function referencing GDevice: inline stores through the device pointer (linear, crude register tracking).
import json,bisect,struct,collections,sys
BASE=0x82000000
img=open("out/default_image.bin","rb").read()
g=json.load(open("out/callgraph.json"))
S=sys.argv[1]
starts=sorted(int(n[4:],16) for n in g if n.startswith("sub_"))
funcs=[int(x,16) for x in json.load(open(S+"/devrefs.json"))]
fanin=collections.Counter()
for f,i in g.items():
    for c in i["calls"]: fanin[c]+=1
STORES={36:"stw",37:"stwu",38:"stb",39:"stbu",44:"sth",45:"sthu",52:"stfs",53:"stfsu",54:"stfd",55:"stfdu",62:"std"}
res={}
for f in funcs:
    i=bisect.bisect_right(starts,f); end=starts[i] if i<len(starts) else f+0x1000
    lis={}; dev=set(); offs=collections.Counter(); calls=0
    for a in range(f,end,4):
        w=struct.unpack_from(">I",img,a-BASE)[0]; op=w>>26
        rt=(w>>21)&31; ra=(w>>16)&31; d=w&0xffff
        if op==15 and ra==0:
            lis[rt]=d; dev.discard(rt); continue
        if op==32 and d==0x74c and lis.get(ra)==0x833D:
            dev.add(rt); continue
        if op==31 and ((w>>1)&0x3ff)==444 and rt==((w>>11)&31):  # mr ra,rs (or rA,rS,rS)
            if rt in dev: dev.add(ra)
            else: dev.discard(ra)
            continue
        if op in STORES and ra in dev:
            off=d if op!=62 else d&0xfffc
            offs[off]+=1; continue
        if op==18 and (w&1): calls+=1; dev-= {3,4,5,6,7,8,9,10,11,12}  # bl clobbers volatiles
        # any other write to a tracked reg kills it
        if op in (14,15,32,34,40,42,58,24,21,20,31,28,25) and rt in dev: dev.discard(rt)
        if op in (24,25,28,21,20) and ra in dev: dev.discard(ra)
    res["%08X"%f]=offs
inside=lambda a: 0x827D8000<=int(a,16)<0x827E5000
w_out={k:v for k,v in res.items() if v and not inside(k)}
w_in={k:v for k,v in res.items() if v and inside(k)}
print("inline device writers: inside RHI",len(w_in),"outside",len(w_out))
allo=collections.Counter()
for k,v in sorted(w_out.items()):
    allo.update(v.keys())
    print(k,"in=%d"%fanin["sub_"+k]," ".join("+%X"%o for o in sorted(v)))
print("offsets written outside:"," ".join("+%X:%d"%(o,n) for o,n in sorted(allo.items())))
