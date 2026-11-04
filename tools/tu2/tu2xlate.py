import json,bisect,re,sys
from collections import Counter,defaultdict
M=json.load(open(__import__("os").environ.get("TU2MAP") or sys.argv[1]))
ia=M['items_a']; ib=M['items_b']; mp={int(k):v for k,v in M['map'].items()}; same=set(M.get('same',[]))
dm={int(k,16):int(v,16) for k,v in M['dmap'].items()}; dk=sorted(dm)
sa=M['secs_a']; starts=[x[0] for x in ia]
def sec(a):
    for n,(va,vs) in sa.items():
        if va<=a<va+max(vs,1): return n
    return None
MANUAL={0x8295BD40:0x82A73208,0x82449E50:0x824499C8,0x829F0EF0:0x829FA268,0x82798238:0x82798098,0x82DCE3B8:0x82DDE180,0x82DCE470:0x82DDE238,0x823A5030:0x828B5BE8,0x82924488:0x825DB270,0x82C3B1A0:0x82C4B2D8,0x82C3C770:0x82C4C8A0}
def xlate(a):
    if a in MANUAL: return MANUAL[a],'text'
    s=sec(a)
    if s is None: return None,'outside'
    if s=='.text':
        i=bisect.bisect_right(starts,a)-1
        if i in mp and i in same: return ib[mp[i]][0]+(a-ia[i][0]),'text'
        if i in mp and a==ia[i][0]: return ib[mp[i]][0],'text-start-CHANGEDCODE(%08X/%x->%x %s)'%(ia[i][0],ia[i][1],ib[mp[i]][1],ia[i][2])
        if i in mp: return None,'text-mid-CHANGEDCODE(%08X/%x %s ->%08X)'%(ia[i][0],ia[i][1],ia[i][2],ib[mp[i]][0])
        return None,'text-UNMATCHED(%08X/%x %s)'%(ia[i][0],ia[i][1],ia[i][2])
    if a in dm: return dm[a],'data-exact'
    k=bisect.bisect_right(dk,a)-1
    lo=dk[k] if k>=0 else None; hi=dk[k+1] if k+1<len(dk) else None
    dlo=dm[lo]-lo if lo is not None and sec(lo)==s else None
    dhi=dm[hi]-hi if hi is not None and sec(hi)==s else None
    if dlo is not None and dlo==dhi: return a+dlo,'data-bracket'
    if dlo is not None: return a+dlo,'data-below?(hi %s)'%(hex(dhi) if dhi is not None else None)
    return None,'data-none'
if __name__=='__main__':
    refs=defaultdict(set)
    for line in open(sys.argv[2],encoding='utf-8',errors='replace'):
        fn=line.split(':',1)[0]
        if 'mgr' in fn or 'codegen_flavor' in fn: continue
        refs[int(re.search(r'(8[23][0-9A-Fa-f]{6})$',line.strip()).group(1),16)].add(fn)
    c=Counter(); rows=[]
    for a in sorted(refs):
        n,k=xlate(a); kk=k.split('(')[0]; c[kk]+=1; rows.append(('%08X'%a,'%08X'%n if n else '-',k,sorted(refs[a])))
    print(c)
    json.dump(rows,open(sys.argv[3],'w'),indent=0)
    for r in rows:
        if 'CHANGED' in r[2] or '?' in r[2] or 'none' in r[2]:
            fs=[f for f in r[3] if 'overrides.toml' not in f and 'cerberus_trace' not in f]
            if fs: print(r[0],r[1],r[2],fs[:4])
