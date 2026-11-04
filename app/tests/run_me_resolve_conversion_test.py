"""Compare separate/fused resolve passes through actual fragment rendering.

Requires Linux/WSL, DXC, spirv-tools, clang++, Vulkan and Lavapipe.
Does not run the game, link an NRO, or modify production shader caches.
"""
from pathlib import Path
import argparse, os, re, subprocess
root=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dxc',default='/root/me2deko/out/nx/dxc-linux.sh')
parser.add_argument('--cxx',default='clang++')
parser.add_argument('--icd',default='/usr/share/vulkan/icd.d/lvp_icd.json')
args=parser.parse_args()
out=root/'out/fixes/resolve_conversion_20261029/test'
out.mkdir(parents=True,exist_ok=True)
host=root/'app/src/renut_engine/gpu/shaders/host'
# Only replace descriptor indexing. Keep both production shader entry points,
# utility push layout, integer source coordinates and all pixel arithmetic.
utility=(host/'utility.hlsli').read_text().replace('g_Textures[]','g_Textures[1]').replace('g_Samplers[]','g_Samplers[1]')
utility=re.sub(r'NonUniformResourceIndex\([^)]*\)','0',utility)
(out/'utility.hlsli').write_text(utility)
for name,profile in [('present_vs','vs_6_0'),('resolve_copy_ps','ps_6_0')]:
    shader=(host/(name+'.hlsl')).read_text()
    shader=re.sub(r'NonUniformResourceIndex\([^)]*\)','0',shader)
    path=out/(name+'.hlsl')
    path.write_text(shader)
    subprocess.run([args.dxc,'-T',profile,'-E','main','-HV','2021','-all-resources-bound',
                    '-Wno-ignored-attributes','-spirv','-fvk-use-dx-layout',str(path),'-Fo',str(out/(name+'.spv'))],check=True)
    subprocess.run(['spirv-val',str(out/(name+'.spv'))],check=True)
exe=out/'gpu_test'
subprocess.run([args.cxx,'-std=c++20','-O2','-ffp-contract=off',
                str(root/'app/tests/me_resolve_conversion_gpu_test.cpp'),'-lvulkan','-o',str(exe)],check=True)
result=subprocess.run([str(exe),str(out/'present_vs.spv'),str(out/'resolve_copy_ps.spv')],
                       env=os.environ|{'VK_ICD_FILENAMES':args.icd},capture_output=True,text=True)
(out.parent/'image_tests.log').write_text(result.stdout+result.stderr)
print(result.stdout+result.stderr,end='')
result.check_returncode()
negative=subprocess.run([str(exe),str(out/'present_vs.spv'),str(out/'resolve_copy_ps.spv'),'unscaled-negative-control'],
                         env=os.environ|{'VK_ICD_FILENAMES':args.icd},capture_output=True,text=True)
(out.parent/'gpu_negative_control.log').write_text(negative.stdout+negative.stderr)
assert negative.returncode!=0 and 'DIFFERENCE' in negative.stderr, 'Unscaled draw was not detected'
line='PASS GPU negative control detected: missing fused scale\n'
with (out.parent/'image_tests.log').open('a') as log: log.write(line)
print(line,end='')
