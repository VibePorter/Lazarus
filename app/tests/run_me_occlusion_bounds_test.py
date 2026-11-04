"""Exercise production latent collection/answer code with deterministic GPU counts.

No game is launched. A negative control removes the volume-identity guard.
"""
from pathlib import Path
import argparse
import re
import subprocess

root = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--cxx', default='clang++')
ap.add_argument('--sanitize', action='store_true')
a = ap.parse_args()
out = root / 'out/fixes/citadel_wall_ryujinx_20261029/tests'
out.mkdir(parents=True, exist_ok=True)
source = (root / 'app/me2_src/me2_occlusion.cpp').read_text()

def function(text, name):
    match = re.search(r'(?m)^[A-Za-z_][^\n;{}]*\b' + name + r'\([^;{}]*\)\s*\{', text)
    assert match, name
    depth = 0
    for at in range(match.end() - 1, len(text)):
        if text[at] == '{': depth += 1
        elif text[at] == '}':
            depth -= 1
            if depth == 0: return text[match.start():at + 1]
    raise AssertionError(name)

def structure(name):
    start = source.index('struct ' + name + ' {')
    return source[start:source.index('\n};', start) + 3]

prefix = r'''
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <unordered_map>
#include "me2_occlusion_bounds.h"
#include "me2_occlusion_view.h"
Me2OcclusionViewHistory g_latent_view;
#define REXCVAR_GET(name) name
constexpr int me2_occlusion_scale=1, me2_occlusion_umbra_latent_confirm=2;
constexpr int me2_occlusion_umbra_latent_frames=10, me2_occlusion_umbra_latent_still_frames=0;
constexpr float me2_occlusion_umbra_latent_still_rot=0.15f, me2_occlusion_umbra_latent_still_move=200;
constexpr int me2_occlusion_umbra_latent_key=3;
constexpr bool me2_occlusion_umbra_flip_trace=false;
constexpr uint32_t kRecords=8, kClosed=1, kBroken=2, kVisible=0x100000;
struct Record { std::atomic<uint64_t> seq{0}; std::atomic<uint32_t> state{0},count{0}; } g_records[kRecords];
std::atomic<uint64_t> st_cpu_cmp[2][2], st_cpu_miss[9], st_stale_check[4][3][2];
std::atomic<uint64_t> st_lat_held,st_lat_collected,st_lat_dropped,st_lat_none,st_lat_moved;
std::atomic<uint64_t> st_lat_visible,st_lat_hidden,st_lat_bounds_rejected,st_lat_still,st_lat_age[5];
std::atomic<uint64_t> g_sa2_game_frames;
thread_local uint64_t t_latent_key;
thread_local uint32_t t_cpu_index=0;
std::string (*g_cpu_info_text)()=nullptr;
enum class Why { kNoSlot,kNoCount,kTooOld,kMoved,kCount,kStill,kCpu,kBounds };
bool CameraDelta(uint64_t,uint64_t,float& r,float& m) {r=m=0;return true;}
bool CameraStill(uint64_t,uint64_t) {return true;}
bool CameraNear(uint64_t,uint64_t,float,float) {return true;}
'''
prefix += '\n'.join(structure(s) for s in ('LatentIssue', 'LatentResult', 'LatentSlot', 'IndexKey'))
prefix += r'''
struct {
  std::deque<LatentIssue> pending;
  std::unordered_map<uint64_t,uint64_t> last_seen[4];
  std::unordered_map<uint32_t,uint64_t> key_by_index;
  std::unordered_map<uint32_t,Me2OcclusionBounds> bounds_by_index;
  std::unordered_map<uint32_t,uint64_t> epoch_by_index;
} g_latent;
constexpr uint32_t kIndexKeys=1u<<13;
IndexKey g_index_keys[kIndexKeys];
std::unordered_map<uint64_t,LatentSlot> slots;
std::unordered_map<uint64_t,uint32_t> gpu_counts;
LatentSlot& LatentClaim(uint64_t key) { auto& s=slots[key];s.key=key;return s; }
LatentSlot* LatentFind(uint64_t key) { auto i=slots.find(key);return i==slots.end()?nullptr:&i->second; }
bool PreviousCount(uint64_t seq,uint32_t,int32_t,uint32_t& n) {
  auto i=gpu_counts.find(seq);if(i==gpu_counts.end())return false;n=i->second;return true;
}
void TraceFlip(LatentSlot*,uint64_t,uint64_t,bool,Why,uint64_t,uint32_t,const std::string&) {}
void WallTraceCollect(const LatentIssue&,uint64_t,bool,uint32_t,const LatentResult&,const LatentResult&,bool) {}
'''
prefix += '\n'.join(function(source, name) for name in ('SetIndexKey','IndexEpoch','IndexBounds','CurrentLatentBounds','CentreKey','ApplyLatentCount')) + '\n'

tests = r'''
unsigned failures=0;
void Check(bool ok,const char* what) {if(!ok){++failures;std::printf("FAIL: %s\n",what);}}
Me2OcclusionBounds Box(float cx,float r) {
  float v[8][3];
  for(unsigned i=0;i<8;++i)for(unsigned c=0;c<3;++c)v[i][c]=(c==0?cx:0)+((i&(1u<<c))?r:-r);
  return Me2OcclusionBounds::FromCorners(v);
}
void Issue(uint64_t seq,uint64_t key,uint64_t frame,uint32_t count,Me2OcclusionBounds b) {
  gpu_counts[seq]=count;g_latent.pending.push_back({seq,key,frame,0,b,g_latent_view.epoch});LatentCollect(frame);
}
uint32_t Read(uint64_t key,uint64_t frame,Me2OcclusionBounds b) {
  g_sa2_game_frames=frame;SetIndexKey(t_cpu_index,key,false,b);return LatentAnswer(key);
}
int main() {
  const auto small=Box(0,1),big=Box(0,20),inner=Box(0,0.5f),shifted=Box(2,1);
  const uint64_t key=CentreKey(small);
  Check(small.Valid()&&big.Valid(),"valid eight-corner boxes");
  Check(key==CentreKey(big)&&key==CentreKey(shifted),"negative control really shares one centre cell");
  Issue(0,key,0,0,small);Issue(1,key,1,0,small);
  Check(Read(key,2,small)==0,"same box still culled");
  Check(Read(key,2,inner)==0,"contained box still culled");
  Check(Read(key,2,big)!=0,"small hidden proxy must not hide a larger volume");
  Check(Read(key,2,shifted)!=0,"quantized-centre neighbour must not reuse hidden count");
  Check(Read(key,2,{})!=0,"invalid or unavailable bounds stay visible");
  Check(Read(key,20,small)!=0,"expired count still rejected");
  // A visible sample immediately restores visibility, including confirm=2.
  Issue(2,key,9,123,big);
  Check(slots[key].res.frame==9,"positive sample accepted immediately");
  Check(Read(key,12,small)!=0,"positive sample restores visibility immediately");
  Issue(3,key,10,123,big);
  Check(Read(key,11,small)!=0,"confirmed visible sample wins");
  Check(Read(key,11,big)!=0,"confirmed visible large box");
  // Fresh zero result for the large proxy can cull that proxy and its subset.
  Issue(4,key,12,0,big);Issue(5,key,13,0,big);
  Check(Read(key,14,big)==0&&Read(key,14,small)==0,"fresh large result retains culling");
  t_cpu_index=kIndexKeys+7;
  Check(Read(key,14,small)==0,"large record index uses the production map fallback");
  auto invalid=Box(0,1); invalid.lo[0]=NAN;
  Check(CentreKey(invalid)==0&&!invalid.Contains(small),"non-finite bounds fail visible");
  Check(CentreKey(Box(8388608.0f,1))==0,"distant centre does not wrap onto nearby volumes");
  float bad[8][3] = {};
  Check(!Me2OcclusionBounds::FromCorners(bad).Valid(),"degenerate corners rejected");
  for(unsigned i=0;i<8;++i)for(unsigned c=0;c<3;++c)bad[i][c]=(i&(1u<<c))?1.f:-1.f;
  bad[0][0]=0.f;
  Check(!Me2OcclusionBounds::FromCorners(bad).Valid(),"non-box volume not treated as an AABB");
  // Exercise the actual collector and answer across camera cuts, including
  // late GPU completion and a partial negative confirmation from the old shot.
  slots.clear();g_latent.pending.clear();g_latent_view={};t_cpu_index=7;
  Me2OcclusionView camera;
  for(unsigned i=0;i<4;++i)camera.view[i*5]=1;
  camera.projection[0]=camera.projection[5]=1;
  camera.projection[10]=0.999f;camera.projection[11]=1;camera.projection[14]=-9.99f;
  Check(!g_latent_view.Observe(camera,100),"first valid view is not a camera cut");
  Issue(100,key,100,0,small);Issue(101,key,101,0,small);
  Check(Read(key,102,small)==0,"confirmed results cull within their original shot");
  const uint64_t old_epoch=g_latent_view.epoch;
  auto cut=camera;
  cut.view[0]=cut.view[10]=std::cos(0.5f);cut.view[2]=-std::sin(0.5f);cut.view[8]=std::sin(0.5f);
  Check(g_latent_view.Observe(cut,102)&Me2OcclusionViewHistory::kRotation,"abrupt rotation starts a new camera epoch");
  Check(Read(key,102,small)!=0,"new shot cannot reuse the old hidden answer");
  gpu_counts[102]=0;g_latent.pending.push_back({102,key,101,0,small,old_epoch});LatentCollect(103);
  Check(Read(key,103,small)!=0,"late old-shot GPU completion cannot revive occlusion");
  Issue(103,key,103,0,small);
  Check(Read(key,103,small)!=0,"first new-shot zero still needs confirmation");
  auto zoom=cut;zoom.projection[0]*=1.2f;zoom.projection[5]*=1.2f;
  Check(g_latent_view.Observe(zoom,104)&Me2OcclusionViewHistory::kProjection,"same-pose FOV cut starts a new epoch");
  Issue(104,key,104,0,small);
  Check(Read(key,104,small)!=0,"negative confirmation cannot span two camera shots");
  Issue(105,key,105,0,small);
  Check(Read(key,106,small)==0,"two fresh new-shot results restore real culling");
  Check(!g_latent_view.Observe(zoom,105),"unchanged camera keeps the epoch");
  const auto stable_epoch=g_latent_view.epoch;
  zoom.view[12]-=20;zoom.projection[8]+=0.0001f;
  Check(!g_latent_view.Observe(zoom,106)&&g_latent_view.epoch==stable_epoch,"walking and small projection jitter preserve culling");
  Check(Read(key,106,small)==0,"continuous shot keeps its confirmed hidden result");
  Check(g_latent_view.Observe(camera,107)!=0&&Read(key,107,small)!=0,"returning to an old angle cannot revive its old epoch");
  auto nearby=camera;nearby.view[12]=-500;
  Check(g_latent_view.Observe(nearby,107)&Me2OcclusionViewHistory::kPosition,"different view in the same frame is isolated");
  SetIndexKey(kIndexKeys+7,key,false,small);
  Check(IndexEpoch(kIndexKeys+7)==g_latent_view.epoch,"large index fallback retains issue epoch");
  Check(g_latent_view.Observe(nearby,110)&Me2OcclusionViewHistory::kGap,"view history gap resets reuse");
  nearby.projection[0]=NAN;
  Check(g_latent_view.Observe(nearby,111)&Me2OcclusionViewHistory::kInvalid,"invalid camera fails conservatively");
  std::printf("production latent code: %u failures\n",failures);
  return failures?1:0;
}
'''
collector = function(source, 'LatentCollect')
answer = function(source, 'LatentAnswer')
guard_start = answer.index('  static const bool centre_key =')
guard_end = answer.index('  Why why =', guard_start)
legacy_answer = answer[:guard_start] + answer[guard_end:]
legacy_camera_answer = answer.replace(' || slot->res.epoch != g_latent_view.epoch', '')
assert legacy_camera_answer != answer
for label, collect_code, answer_code in [('fixed', collector, answer), ('negative', collector, legacy_answer),
                                        ('negative_camera', collector, legacy_camera_answer)]:
    cpp = out / (label + '.cpp')
    cpp.write_text(prefix + collect_code + '\n' + answer_code + tests)
    exe = out / (label + ('.exe' if __import__('os').name == 'nt' else ''))
    flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if a.sanitize else ['-O2']
    subprocess.run([a.cxx, '-std=c++20', *flags, '-I', str(root/'app/me2_src'), str(cpp), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)])
    assert result.returncode == (0 if label == 'fixed' else 1), label
print('PASS: production collector and answer; removing volume guard reproduces false occlusion')
