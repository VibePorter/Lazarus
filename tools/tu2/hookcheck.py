import re,os,sys,glob
def defined(root):
    s=set()
    for f in [root+'/app/generated/default/sa2_init.cpp']:
        for m in re.finditer(rb'\{ 0x([0-9A-F]{8}), sub_',open(f,'rb').read()): s.add(m.group(1).decode())
    return s
def used(root):
    u={}
    pat=re.compile(r'(?:sub_|ME_HOT_HOOK(?:_OWN)?\(|ME2_GC_CALLER\(|ME2_CERBERUS_TRACE\(|SA2_HOT_HOOK\w*\()([0-9A-F]{8})\b')
    for dp,dn,fn in os.walk(root+'/app'):
        dn[:]=[d for d in dn if d not in ('generated','thirdparty','out')]
        for f in fn:
            if not f.endswith(('.cpp','.h','.inc')) or 'mgr' in f: continue
            p=os.path.join(dp,f)
            for i,l in enumerate(open(p,encoding='utf-8',errors='replace')):
                code=l.split('//')[0]
                for m in pat.finditer(code): u.setdefault(m.group(1),set()).add(os.path.relpath(p,root))
    return u
for root in sys.argv[1:]:
    d=defined(root); u=used(root)
    miss={a:f for a,f in u.items() if a not in d}
    print(root,'defined',len(d),'used',len(u),'missing',len(miss))
    for a,f in sorted(miss.items()): print('  ',a,sorted(f)[:3])
