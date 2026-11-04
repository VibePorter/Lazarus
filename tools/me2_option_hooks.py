"""Keep ME2's startup-only option hook reachable after every codegen flavour.

Only changes calls to the verified startup playback function. The shared
movie player, cinematic decoder and other guest functions stay unchanged.
"""
from pathlib import Path
import argparse, re

def route(directory):
    changed=[]
    for path in sorted(Path(directory).glob('sa2_recomp.*.cpp')):
        data=path.read_bytes()
        patched,count=re.subn(rb'\b__imp__sub_822F7FA0(?=\s*\(ctx,\s*base\)\s*;)',b'sub_822F7FA0',data)
        if count:
            path.write_bytes(patched)
            changed.append({'path':str(path),'calls':count})
    return changed

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gen',type=Path,required=True)
    args=parser.parse_args()
    changes=route(args.gen)
    print('ME2 option hooks: '+str(sum(c['calls'] for c in changes))+' calls rerouted in '+str(len(changes))+' files')
