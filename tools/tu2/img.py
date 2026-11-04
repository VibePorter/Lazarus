import struct
BASE=0x82000000
def load(p):
    b=open(p,'rb').read(); e=struct.unpack_from('<I',b,0x3c)[0]; n=struct.unpack_from('<H',b,e+6)[0]; opt=struct.unpack_from('<H',b,e+20)[0]
    o=e+24+opt; secs={}
    for i in range(n):
        name=b[o:o+8].rstrip(b'\0').decode(); vs,va=struct.unpack_from('<II',b,o+8); secs[name]=(BASE+va,vs); o+=40
    pva,pvs=secs['.pdata']; f=[]
    for i in range(pvs//8):
        beg,fl=struct.unpack_from('>II',b,pva-BASE+i*8)
        if beg: f.append((beg,((fl>>8)&0x3FFFFF)*4))
    return b,secs,sorted(f)
def word(b,a): return struct.unpack_from('>I',b,a-BASE)[0]
