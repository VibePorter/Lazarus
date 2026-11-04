import pickle,sys,re
db=pickle.load(open(sys.argv[1],'rb'))
mode=sys.argv[2]
if mode=='show':
    a=int(sys.argv[3],16); n=int(sys.argv[4]) if len(sys.argv)>4 else 400
    print('\n'.join(db[a][:n]))
elif mode=='first':  # functions whose first k instrs match regexes
    pats=[re.compile(p) for p in sys.argv[3:]]
    for a,L in db.items():
        ins=[x for x in L if not x.startswith('loc_')]
        if len(ins)>=len(pats) and all(p.search(ins[i]) for i,p in enumerate(pats)):
            print(hex(a),len(ins))
elif mode=='has':  # all regexes appear somewhere; optional max len via env
    import os
    mx=int(os.environ.get('MAXN','100000')); mn=int(os.environ.get('MINN','0'))
    pats=[re.compile(p) for p in sys.argv[3:]]
    for a,L in db.items():
        ins=[x for x in L if not x.startswith('loc_')]
        if not (mn<=len(ins)<=mx): continue
        if all(any(p.search(x) for x in ins) for p in pats):
            print(hex(a),len(ins))
