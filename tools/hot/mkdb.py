import os,re,sys,pickle
d=sys.argv[1]; out=sys.argv[2]
db={}
fre=re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]+)\)')
cur=None
for fn in sorted(os.listdir(d)):
    if not (fn.startswith('sa2_recomp.') and fn.endswith('.cpp')): continue
    for ln in open(os.path.join(d,fn),encoding='utf-8',errors='replace'):
        m=fre.match(ln)
        if m: cur=int(m.group(1),16); db[cur]=[]; continue
        if cur is None: continue
        if ln.startswith('\t// ') and not ln.startswith('\t// ME') :
            db[cur].append(ln[4:].rstrip())
        elif ln.startswith('loc_'):
            db[cur].append(ln.rstrip())
pickle.dump(db,open(out,'wb'))
print(len(db), sum(len(v) for v in db.values()))
