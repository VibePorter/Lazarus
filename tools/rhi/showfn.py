import json,sys,os,re
g=json.load(open("out/callgraph.json"))
for name in sys.argv[1:]:
    i=g[name]; p=os.path.join("app/generated/default",i["file"])
    L=open(p,errors="replace").read().split("\n")
    out=[]
    for k in range(i["line"]-1,len(L)):
        if k>i["line"] and re.match(r"^(DEFINE_REX_FUNC|PPC_FUNC_IMPL|PPC_WEAK_FUNC|REX_)",L[k]): break
        out.append(L[k])
    asm=[l.strip() for l in out if re.match(r"\s*//\s*[0-9A-F]{8}",l) or "// " in l and re.search(r"\b(lwz|stw|bl|lis|addi)\b",l)]
    print("=====",name,len(out),"lines")
    print("\n".join(asm if asm else out[:200]))
