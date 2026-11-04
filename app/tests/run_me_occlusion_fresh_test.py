"""Exercise production negative-result acceptance and replay recorded wall transitions.

The fixture comes from the user's stationary Citadel run. No game is launched.
"""
from pathlib import Path
import argparse,json,re,subprocess

root=Path(__file__).resolve().parents[2]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--cxx',default='g++')
ap.add_argument('--sanitize',action='store_true')
a=ap.parse_args()
source=(root/'app/me2_src/me2_occlusion.cpp').read_text()

def structure(name):
    start=source.index('struct '+name+' {')
    return source[start:source.index('\n};',start)+3]

start=source.index('bool ApplyLatentCount(')
end=source.index('\n}\n',start)+2
apply=source[start:end]
legacy=r'''
bool ApplyLatentCount(LatentResult& res,const LatentIssue& issue,uint32_t count,uint64_t,uint32_t confirm,uint64_t) {
  if(issue.frame<res.frame)return false;
  if(!res.any||(count!=0)==(res.pixels!=0)||++res.against>=confirm) {
    res.frame=issue.frame;res.pixels=count;res.bounds=issue.bounds;res.against=0;res.any=true;return true;
  }
  return false;
}
'''
prefix='''#include <algorithm>
#include <cstdint>
#include <cstdio>
#include "me2_occlusion_bounds.h"
#include "me2_occlusion_view.h"
Me2OcclusionViewHistory g_latent_view;
'''+structure('LatentIssue')+'\n'+structure('LatentResult')+'\n'
fixture=json.loads((root/'app/tests/fixtures/citadel_umbra_wall_counts.json').read_text())
rows=','.join('{'+f'{r["frame"]},{r["age"]},{r["pixels"]}'+'}' for r in fixture['transitions'])
tests=r'''
unsigned failures=0;
void Check(bool ok,const char* msg){if(!ok){++failures;std::printf("FAIL: %s\n",msg);}}
Me2OcclusionBounds box{{-18321.01f,-17436.94f,-4252.38f},{-18318.99f,17916.94f,14860.f}};
bool Feed(LatentResult& r,uint64_t issued,uint64_t now,uint32_t count,uint32_t confirm=1) {
  return ApplyLatentCount(r,LatentIssue{issued+1,1,issued,0,box},count,now,confirm,10);
}
int main(){
  LatentResult r;
  Check(!Feed(r,0,1,0)&&!r.any,"first zero cannot hide an untested node");
  Check(!Feed(r,0,1,0)&&!r.any,"duplicate same-frame issue is not independent confirmation");
  Check(Feed(r,1,2,0)&&r.any&&r.pixels==0,"two fresh independent zeros retain real culling");
  Check(Feed(r,2,3,140000,8)&&r.pixels==140000,"positive sample restores visibility even at confirm=8");
  Check(!Feed(r,3,11,0)&&r.pixels==140000&&r.frame==2,"eight-frame-old zero cannot hide a visible node or refresh age");
  Check(!Feed(r,12,13,0)&&r.pixels!=0,"isolated fresh zero remains visible");
  Check(Feed(r,13,14,100000)&&r.pixels==100000,"positive sample cancels a negative candidate");
  Check(!Feed(r,14,15,0)&&r.pixels!=0,"first fresh negative after cancellation remains visible");
  Check(Feed(r,15,16,0)&&r.pixels==0,"second fresh negative starts culling");
  Check(Feed(r,16,24,0)&&r.pixels==0&&r.frame==16,"confirmed occlusion retains the normal older-result path");
  Check(!Feed(r,10,24,500000)&&r.pixels==0,"older out-of-order visible sample ignored");
  Check(Feed(r,25,26,60000),"new visible result accepted");
  Check(!Feed(r,26,27,0),"fresh negative starts a candidate");
  Check(Feed(r,34,35,0)&&r.pixels==0,"fresh zero results eight frames apart retain Umbra's sparse retesting culling");
  Check(Feed(r,35,36,100000)&&r.pixels!=0,"sparse confirmed occlusion still exits immediately");
  Check(!Feed(r,50,49,0),"future issue rejected");
  LatentResult expired;
  Feed(expired,0,1,0);Feed(expired,1,2,0);
  Check(!Feed(expired,20,21,0)&&expired.frame==1,"expired occlusion needs fresh confirmation again");
  Check(Feed(expired,21,22,0)&&expired.frame==21,"second fresh result confirms expired occlusion again");
  r={};
  struct Sample{uint64_t frame,age;uint32_t pixels;};
  const Sample trace[]={TRACE_ROWS};
  unsigned hidden=0;
  for(const auto& s:trace){Feed(r,s.frame-s.age,s.frame,s.pixels);hidden+=r.any&&r.pixels==0;}
  Check(hidden==0,"recorded visible wall does not become hidden on stale zero pulses");
  std::printf("recorded wall replay: %u hidden transitions; production acceptance: %u failures\n",hidden,failures);
  return failures?1:0;
}
'''.replace('TRACE_ROWS',rows)
out=root/'out/fixes/citadel_umbra_fresh_20261029/tests'
out.mkdir(parents=True,exist_ok=True)
for label,body in [('fixed',apply),('negative',legacy)]:
    cpp=out/(label+'.cpp')
    cpp.write_text(prefix+body+tests)
    exe=out/label
    flags=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'] if a.sanitize else ['-O2']
    subprocess.run([a.cxx,'-std=c++20',*flags,'-I',str(root/'app/me2_src'),str(cpp),'-o',str(exe)],check=True)
    result=subprocess.run([str(exe)])
    assert result.returncode==(0 if label=='fixed' else 1),label
print('PASS: actual production acceptance and recorded wall transitions; legacy policy fails')
