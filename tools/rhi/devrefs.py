# Functions that reference the global D3D device pointer 0x833D074C (lis rX,0x833D / lwz|addi rY,0x74C(rX))
import json,bisect,struct,collections
BASE=0x82000000
img=open("out/default_image.bin","rb").read()
g=json.load(open("out/callgraph.json"))
starts=sorted(int(n[4:],16) for n in g if n.startswith("sub_"))
hi=0x833D; lo=0x074C
hits=collections.defaultdict(int)
text_lo,text_hi=0x82200000,0x83100000
lisreg={}
for a in range(text_lo,text_hi,4):
    w=struct.unpack_from(">I",img,a-BASE)[0]
    op=w>>26
    if op==15:  # addis / lis
        rt=(w>>21)&31; ra=(w>>16)&31; imm=w&0xffff
        if ra==0 and imm==hi: lisreg[rt]=a
    elif op in (32,14,48,36):  # lwz, addi, lfs, stw
        rt=(w>>21)&31; ra=(w>>16)&31; d=w&0xffff
        if d==lo and ra in lisreg and a-lisreg[ra]<0x400:
            i=bisect.bisect_right(starts,a)-1
            hits[starts[i]]+=1
reg=collections.Counter()
for f in hits: reg[f>>20]+=1
print(len(hits),"functions reference GDevice")
for r,n in sorted(reg.items()): print(hex(r<<20),n)
json.dump(sorted("%08X"%f for f in hits),open(__import__("sys").argv[1]+"/devrefs.json","w"))
