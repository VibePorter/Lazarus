import sys,difflib,struct,capstone
sys.path.insert(0,sys.argv[1]); from img import *
A,_,_=load(sys.argv[2]); B,_,_=load(sys.argv[3])
md=capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32|capstone.CS_MODE_BIG_ENDIAN)
def dis(b,a0,l):
    r=[]
    for a in range(a0,a0+l,4):
        c=b[a-BASE:a-BASE+4]; i=next(md.disasm(c,a),None)
        t=f'{i.mnemonic} {i.op_str}' if i else '.long 0x%08X'%struct.unpack('>I',c)[0]
        r.append(t)
    return r
for spec in sys.argv[4:]:
    a,b,la,lb=[int(x,16) for x in spec.split(':')]
    da=dis(A,a,la); db=dis(B,b,lb)
    import re
    na=[re.sub(r'0x[0-9a-f]{5,8}','ADDR',x) for x in da]; nb=[re.sub(r'0x[0-9a-f]{5,8}','ADDR',x) for x in db]
    print('=====',spec)
    for tag,i1,i2,j1,j2 in difflib.SequenceMatcher(None,na,nb,autojunk=False).get_opcodes():
        if tag=='equal': continue
        print(f'-- {tag} base +{i1*4:X}..+{i2*4:X}  tu2 +{j1*4:X}..+{j2*4:X}')
        for k in range(i1,min(i2,i1+14)): print('   - %08X %s'%(a+4*k,da[k]))
        for k in range(j1,min(j2,j1+14)): print('   + %08X %s'%(b+4*k,db[k]))
