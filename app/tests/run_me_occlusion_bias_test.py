"""Test the production visibility-test bias policy without a GPU or game launch."""
from pathlib import Path
import argparse,subprocess
root=Path(__file__).resolve().parents[2]
ap=argparse.ArgumentParser();ap.add_argument('--cxx',default='g++');ap.add_argument('--sanitize',action='store_true');a=ap.parse_args()
out=root/'out/fixes/citadel_query_bias_20261030/tests';out.mkdir(parents=True,exist_ok=True)
code=r'''
#include <cstdio>
#include "renut_engine/gpu/render/occlusion_bias.h"
using renut::gpu::OcclusionTestBias;
unsigned failures=0;
void Check(bool v,const char* s){if(!v){++failures;std::printf("FAIL: %s\n",s);}}
int main(){
  constexpr int32_t units=16384;constexpr float clamp=2e-4f;
  for(unsigned f:{4u,6u}) {
    const auto b=OcclusionTestBias(true,0,2|(f<<4),units,clamp);
    Check(b&&b->units==units&&b->clamp==clamp,"reversed-depth test leans toward passing");
  }
  for(unsigned f:{1u,3u}) {
    const auto b=OcclusionTestBias(true,0,2|(f<<4),units,clamp);
    Check(b&&b->units==-units&&b->clamp==-clamp,"forward-depth test leans toward passing");
  }
  // Enumerate the render-state boundary: no colour/depth/stencil-writing pass
  // may inherit query bias, even when wrapped in an engine query.
  for(unsigned mask=0;mask<16;++mask)for(unsigned flags=0;flags<8;++flags) {
    const bool applies=bool(OcclusionTestBias(true,mask,0x60|flags,units,clamp));
    Check(applies==(mask==0&&flags==2),"bias isolated from colour, prepass and stencil draws");
  }
  Check(!OcclusionTestBias(false,0,0x62,units,clamp),"PC/Vulkan keeps its original policy");
  for(unsigned f:{0u,2u,5u,7u})
    Check(!OcclusionTestBias(true,0,2|(f<<4),units,clamp),"non-ordering depth comparisons unchanged");
  const auto off=OcclusionTestBias(true,0,0x62,0,0);
  Check(off&&off->units==0&&off->clamp==0,"query bias can be disabled independently");
  Check(!OcclusionTestBias(true,15,0x66,units,clamp),"eye colour/depth pass gets no query bias");
  Check(!OcclusionTestBias(true,0,0x63,units,clamp),"stencil-only eye mask gets no query bias");
  std::printf("production occlusion bias: %u failures\n",failures);return failures?1:0;
}
'''
cpp=out/'policy.cpp';cpp.write_text(code)
exe=out/'policy'
flags=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'] if a.sanitize else ['-O2']
subprocess.run([a.cxx,'-std=c++20',*flags,'-I',str(root/'app/src'),str(cpp),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
