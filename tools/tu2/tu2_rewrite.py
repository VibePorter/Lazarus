"""Rewrite base-XEX guest addresses to TU2 addresses in the TU2 working copy.
usage: tu2_rewrite.py <map.json> <root> <log.json> [--apply]"""
import os,re,sys,json
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
import tu2xlate as X
X_M=None
mapjson,root,logp=sys.argv[1:4]; apply='--apply' in sys.argv
EXT={'.cpp','.h','.hpp','.inc','.c','.py','.toml','.txt','.ps1','.sh','.cmake','.hlsl','.glsl','.md','.bat','.json','.ld'}
SKIPDIR={'sdk','assets','generated','thirdparty','out','.git','__pycache__','node_modules'}
SKIPFILE=lambda p: 'mgr' in os.path.basename(p).lower()
TOK=re.compile(r'(?<![0-9A-Za-z])(8[23][0-9A-Fa-f]{6})(?![0-9A-Fa-f])')
log=[]; renames=[]; touched=0
def conv(m,path,ln):
    old=m.group(1); a=int(old,16); n,k=X.xlate(a)
    ok = n is not None and (k=='text' or k.startswith('text-start-CHANGEDCODE') or k in ('data-exact','data-bracket'))
    log.append({'file':path,'line':ln,'old':old,'new':('%08X'%n if n is not None else None),'kind':k,'applied':ok and n!=a})
    if not ok: return old
    s='%08X'%n
    return s.lower() if old==old.lower() and old!=old.upper() else s
for dp,dns,fns in os.walk(root):
    dns[:]=[d for d in dns if d not in SKIPDIR and not os.path.islink(os.path.join(dp,d))]
    for fn in fns:
        p=os.path.join(dp,fn); rel=os.path.relpath(p,root).replace(os.sep,'/')
        if os.path.splitext(fn)[1].lower() not in EXT or SKIPFILE(p): continue
        raw=open(p,'rb').read()
        try: txt=raw.decode('utf-8')
        except UnicodeDecodeError: txt=raw.decode('latin-1'); enc='latin-1'
        else: enc='utf-8'
        out=[]
        for i,line in enumerate(txt.splitlines(keepends=True),1):
            out.append(TOK.sub(lambda m: conv(m,rel,i),line))
        new=''.join(out)
        if new!=txt:
            touched+=1
            if apply: open(p,'wb').write(new.encode(enc))
        m=TOK.search(fn)
        if m:
            n,k=X.xlate(int(m.group(1),16))
            if n is not None and (k=='text' or k.startswith('text-start')):
                renames.append((rel, fn.replace(m.group(1),'%08X'%n)))
for rel,nfn in renames:
    p=os.path.join(root,rel)
    if apply: os.rename(p,os.path.join(os.path.dirname(p),nfn))
json.dump({'log':log,'renames':renames},open(logp,'w'),indent=0)
from collections import Counter
print('files touched',touched,'tokens',len(log),'applied',sum(l['applied'] for l in log))
print(Counter(l['kind'].split('(')[0] for l in log))
print('renames',renames)
