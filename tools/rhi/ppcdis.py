import json,sys,bisect,struct,capstone
BASE=0x82000000
img=open("out/default_image.bin","rb").read()
g=json.load(open("out/callgraph.json"))
starts=sorted(int(n[4:],16) for n in g if n.startswith("sub_"))
md=capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32|capstone.CS_MODE_BIG_ENDIAN)
for name in sys.argv[1:]:
    a=int(name.replace("sub_",""),16)
    i=bisect.bisect_right(starts,a); end=starts[i] if i<len(starts) else a+0x400
    end=min(end,a+4*int(__import__("os").environ.get("MAXI","400")))
    print("=====",name,"%d insns"%((end-a)//4))
    for x in range(a,end,4):
        c=img[x-BASE:x-BASE+4]; ins=next(md.disasm(c,x),None)
        print("%08X  %s"%(x, f"{ins.mnemonic} {ins.op_str}" if ins else "<vmx/unk %08X>"%struct.unpack(">I",c)[0]))
