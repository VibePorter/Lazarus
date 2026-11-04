import sys,re,os,glob,struct,collections,pickle
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from gcc_crc import pid
GEN=__import__('os').path.join(__import__('os').path.dirname(__file__),'..','..','app','generated','default')
def parse(path):
    d=open(path,'rb').read(); p=16; recs={}; cur=None
    while p+8<=len(d):
        tag,ln=struct.unpack_from('<Ii',d,p); p+=8
        if tag==0: break
        if tag==0x01000000 and ln>0:
            cur=struct.unpack_from('<I',d,p)[0]; recs[cur]=[0,0]
        elif tag==0x01a10000 and ln>0:
            n=ln//8; v=struct.unpack_from('<%dQ'%n,d,p); recs[cur][0]+=sum(v); recs[cur][1]=max(v) if v else 0
        if ln>0: p+=ln
    return recs
tot={}; miss=0
for f in glob.glob('pgo/*.gcda'):
    m=re.search(r'(sa2_recomp\.\d+\.cpp)',f)
    if not m: continue
    recs=parse(f)
    for n in re.findall(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]+)\)',open(os.path.join(GEN,m.group(1)),encoding='utf-8',errors='ignore').read(),re.M):
        r=recs.get(pid('__imp__'+n))
        if r is None: miss+=1; continue
        tot[n]=r
pickle.dump(tot,open('pgo_counts.pkl','wb'))
db=pickle.load(open('me2db.pkl','rb'))
s=sum(v[0] for v in tot.values()); acc=0
print('# functions',len(tot),'missing',miss)
for n,v in sorted(tot.items(),key=lambda kv:-kv[1][0])[:int(sys.argv[1])]:
    acc+=v[0]; a=int(n[4:],16); ins=[x for x in db.get(a,[]) if not x.startswith('loc_')]
    print('%s %6.2f%% cum %5.1f%% max %12d len %5d  %s'%(n,100*v[0]/s,100*acc/s,v[1],len(ins),' | '.join(ins[4:9])[:90]))
