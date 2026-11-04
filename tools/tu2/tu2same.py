import sys,json,struct,hashlib
sys.path.insert(0,sys.argv[1]); from img import *
A,_,_=load(sys.argv[2]); B,_,_=load(sys.argv[3]); M=json.load(open(sys.argv[4]))
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
    return o
bad=json.load(open(sys.argv[5]))
same=[]
for k,j in M['map'].items():
    i=int(k); a0,l,_=M['items_a'][i]; b0,l2,_=M['items_b'][j]
    if l==l2 and k not in bad and norm(A,a0,l)==norm(B,b0,l2): same.append(i)
M['same']=same; json.dump(M,open(sys.argv[4],'w')); print('same',len(same),'of',len(M['map']))
