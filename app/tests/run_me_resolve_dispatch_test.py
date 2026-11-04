"""Test actual resolve dispatch/signature code with recorded API operations.

Offline only; ASan/UBSan; no driver, game, NRO or production shader cache.
"""
from pathlib import Path
import argparse, subprocess
root=Path(__file__).resolve().parents[2]
out=root/'out/fixes/resolve_conversion_20261029/dispatch'
out.mkdir(parents=True,exist_ok=True)
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx',default='clang++')
args=parser.parse_args()
source=(root/'app/src/renut_engine/gpu/render/draw.cpp').read_text()
def block(start):
    begin=source.index(start)
    end=source.index('{',begin)+1
    depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}')
        end+=1
    return source[begin:end]
prefix=r'''
#include <plume_render_interface.h>
#include "renut_engine/gpu/render/resolve_conversion.h"
#include "renut_engine/gpu/render/render_scale.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <set>
using namespace renut::gpu;
using u32=uint32_t; using i32=int32_t;
#define REXCVAR_GET(name) name
#define RGPU_INFO(...) ((void)0)
bool me2_renut_resolve_exp_bias=true,me2_renut_resolve_fuse_exp_bias=true,me2_renut_msaa4x_downsample=true;
bool draw_enabled=true,skip_enabled=true,fail_pipeline=false,fail_framebuffer=false,fail_slot=false,quad=false;
int backend=0;
bool IsDeko3D() { return backend==0; } bool IsVulkan() { return backend==1; }
constexpr u32 kInvalidDescriptor=UINT32_MAX,kPointClampSamplerSlot=0;
struct Ops { unsigned draws=0,copies=0,resolves=0,scales=0,skips=0,mips=0,quads=0; } ops;
struct UtilityPush { float scale[2]={},translate[2]={}; u32 texture_index=0,sampler_index=0; float param0=0,param1=0; } last_push;
struct Texture:plume::RenderTexture {
 std::unique_ptr<plume::RenderTextureView> createTextureView(const plume::RenderTextureViewDesc&) const override { return nullptr; }
 void setName(const std::string&) override {}
};
struct Shader:plume::RenderShader { void setName(const std::string&) override {} };
struct Pipeline:plume::RenderPipeline {
 void setName(const std::string&) override {}
 plume::RenderPipelineProgram getProgram(const std::string&) const override { return {}; }
};
struct Frame:plume::RenderFramebuffer { u32 getWidth() const override { return 64; } u32 getHeight() const override { return 64; } };
struct Device {
 std::unique_ptr<plume::RenderPipeline> createGraphicsPipeline(const plume::RenderGraphicsPipelineDesc&) {
  return fail_pipeline?nullptr:std::make_unique<Pipeline>();
 }
 std::unique_ptr<plume::RenderFramebuffer> createFramebuffer(const plume::RenderFramebufferDesc&) {
  return fail_framebuffer?nullptr:std::make_unique<Frame>();
 }
};
struct Commands {
 plume::RenderRect scissor;
 template<class...T> void setFramebuffer(T&&...) {}
 template<class...T> void setViewports(T&&...) {}
 void setScissors(plume::RenderRect r) { scissor=r; }
 template<class...T> void setGraphicsPipelineLayout(T&&...) {}
 template<class...T> void setPipeline(T&&...) {}
 template<class...T> void setGraphicsDescriptorSet(T&&...) {}
 void setGraphicsPushConstants(u32,const void *p,u32,u32 size) { assert(size==sizeof(last_push)); std::memcpy(&last_push,p,size); }
 void drawInstanced(u32 n,u32 instances,u32,u32) { assert(n==3 && instances==1); ++ops.draws; }
 template<class...T> void copyTextureRegion(T&&...) { ++ops.copies; }
 template<class...T> void resolveTextureRegion(T&&...) { ++ops.resolves; }
} commands;
struct StateData {
 std::unique_ptr<Device> device=std::make_unique<Device>();
 std::unique_ptr<Shader> resolve_copy_ps=std::make_unique<Shader>(),present_vs=std::make_unique<Shader>();
 std::unique_ptr<plume::RenderPipelineLayout> utility_layout=std::make_unique<plume::RenderPipelineLayout>();
 std::unique_ptr<int> texture_set=std::make_unique<int>(),sampler_set=std::make_unique<int>();
 std::unordered_map<u32,std::unique_ptr<plume::RenderPipeline>> resolve_copy_pipelines;
 Commands *command_list=&commands;
} state;
auto &State() { return state; }
struct Bound { plume::RenderFramebuffer *framebuffer=nullptr; plume::RenderPipeline *pipeline=nullptr; bool layout=true,viewport_set=true; } g_bound;
enum class TextureDimension { Tex2D,Tex3D,Cube };
struct HostTexture {
 std::unique_ptr<Texture> texture=std::make_unique<Texture>();
 std::unique_ptr<plume::RenderFramebuffer> copy_framebuffer;
 bool render_target=true,stacked=false; TextureDimension dimension=TextureDimension::Tex2D;
 plume::RenderFormat host_format=plume::RenderFormat::R16G16B16A16_FLOAT;
 u32 width=64,height=64,alloc_width=64,alloc_height=64,extra_levels=0;
 uint64_t resolve_signature=0;
} target,staging;
struct HostSurface {
 std::unique_ptr<Texture> texture=std::make_unique<Texture>();
 plume::RenderFormat format=plume::RenderFormat::R16G16B16A16_FLOAT;
 plume::RenderSampleCounts samples=plume::RenderSampleCount::COUNT_1;
 u32 width=64,height=64,format_word=123; RenderScale scale;
 uint64_t generation=1,writes=2,write_seq=3;
} surface_data,backing_data;
struct ClearRect { i32 x1,y1,x2,y2; };
namespace d3d { struct TextureFetchConstant {}; }
namespace gpu_profile { u32 Label(const char*) { return 1; } void Mark(StateData&,u32) {} }
namespace banjo::renut_gpu {
 bool DrawResolvesOn() { return draw_enabled; } bool ResolveSkipOn() { return skip_enabled; }
 void CountDrawnResolve() {} void CountSkippedResolve(bool) { ++ops.skips; }
}
void TransitionSurfaceLocked(HostSurface*,plume::RenderTextureLayout) {}
void TransitionTextureLocked(HostTexture*,plume::RenderTextureLayout) {}
u32 SurfaceSampleSlotLocked(HostSurface*) { return fail_slot?kInvalidDescriptor:1; }
HostSurface *Msaa4xBackingLocked(const HostSurface*) { return quad?&backing_data:nullptr; }
HostTexture *GetResolveTextureLocked(u32,plume::RenderFormat,RenderScale,const d3d::TextureFetchConstant*) { return &target; }
HostTexture *ResolveStagingLocked(HostTexture*) { return &staging; }
bool ResolveFromMsaa4xBackingLocked(HostSurface*,HostTexture*,const plume::RenderRect&,u32,u32) {
 if(!quad) return false; ++ops.quads; return true;
}
bool ScaleTextureRegionLocked(HostTexture*,const plume::RenderRect&,float) { ++ops.scales; return true; }
void UpdateResolveMipsLocked(HostTexture*,const plume::RenderRect&) { ++ops.mips; }
bool LogLimited(u32 &n,u32 limit) { return n++<limit; }
plume::RenderRect ScaleRect(plume::RenderRect r,RenderScale s) {
 return plume::RenderRect(s.Coord(r.left),s.Coord(r.top),s.Coord(r.right),s.Coord(r.bottom));
}
'''
draw=block('bool DrawResolveCopy(')
# Bind the unchanged API calls to the recording command-list stand-in.
# Only this local pointer type changes; eligibility, pushes and dispatch do not.
draw=draw.replace('plume::RenderCommandList *cmd = s.command_list;','Commands *cmd = s.command_list;')
signature=block('static uint64_t ResolveSignature(')
fit=block('  auto fit = [&](const HostTexture *dest, ClearRect &src)')+';'
color=block('    } else if (HostTexture *dest = GetResolveTextureLocked(').replace('} else if','if',1)
wrapper=r'''
void ColorResolve(u32 flags,const ClearRect *source_rect=nullptr,i32 dest_x=0,i32 dest_y=0,u32 dest_level=0,u32 dest_slice=0) {
 auto &s=State(); auto *cmd=s.command_list; auto *surface=&surface_data;
 const u32 kProfileLabel=1,dest_texture_va=0x123;
 const d3d::TextureFetchConstant *dest_format=nullptr;
'''+fit+'\n'+color+'\n}\n'
tests=r'''
u32 Flags(int bias) { return u32(bias)<<26; }
void Reset() {
 state.resolve_copy_ps=std::make_unique<Shader>(); state.resolve_copy_pipelines.clear();
 target=HostTexture{}; surface_data=HostSurface{};
 ops={}; last_push={}; g_bound={};
 me2_renut_resolve_exp_bias=me2_renut_resolve_fuse_exp_bias=me2_renut_msaa4x_downsample=true;
 draw_enabled=skip_enabled=true; fail_pipeline=fail_framebuffer=fail_slot=quad=false; backend=0;
}
int main() {
 using F=plume::RenderFormat;
 const std::array allowed={F::R8G8B8A8_UNORM,F::B8G8R8A8_UNORM,F::R16G16B16A16_UNORM,F::R16G16_UNORM,
   F::R8G8_UNORM,F::R8_UNORM,F::R16_UNORM,F::R16G16B16A16_FLOAT,F::R16G16_FLOAT,F::R16_FLOAT};
 for(int i=0;i<200;++i) assert(CanFuseResolveExponentBias(F(i))==(std::find(allowed.begin(),allowed.end(),F(i))!=allowed.end()));
 for(int bias=-32;bias<32;++bias) {
  assert(ResolveExponentBias(Flags(bias)|0x123456)==bias);
  Reset(); ColorResolve(Flags(bias));
  assert(ops.draws==1 && !ops.copies && !ops.scales);
  assert(last_push.param0==std::ldexp(1.f,bias));
  assert(!g_bound.layout && !g_bound.viewport_set && !g_bound.framebuffer && !g_bound.pipeline);
  auto sig=target.resolve_signature; ColorResolve(Flags(bias));
  assert(ops.skips==1 && ops.draws==1 && target.resolve_signature==sig);
  // Another bias on the same source must never reuse the previously scaled result.
  ColorResolve(Flags(bias==31?-32:bias+1)); assert(ops.draws==2 && ops.skips==1 && target.resolve_signature!=sig);
 }
 // Every bias has a distinct nonzero signature for otherwise identical resolves.
 Reset(); ClearRect rect{0,0,64,64}; std::set<uint64_t> signatures;
 for(int bias=-32;bias<32;++bias) signatures.insert(ResolveSignature(&surface_data,rect,0,0,0,0,bias));
 assert(signatures.size()==64 && !signatures.contains(0));
 // Exponent application and fusion switches are independent.
 Reset(); me2_renut_resolve_fuse_exp_bias=false; ColorResolve(Flags(-3));
 assert(ops.draws==1 && ops.scales==1 && last_push.param0==1.f);
 Reset(); me2_renut_resolve_exp_bias=false; ColorResolve(Flags(-3));
 assert(ops.draws==1 && !ops.scales && last_push.param0==1.f);
 auto unbiased=target.resolve_signature; me2_renut_resolve_exp_bias=true; ColorResolve(Flags(-3));
 assert(ops.draws==2 && target.resolve_signature!=unbiased && last_push.param0==0.125f);
 // Draw-disabled, other backend, FP32, sRGB, arrays/volumes and resource failures keep the fallback.
 for(int mode=0;mode<12;++mode) {
  Reset();
  if(mode==0) draw_enabled=false;
  if(mode==1) backend=1;
  if(mode==2) backend=2;
  if(mode==3) surface_data.format=target.host_format=F::R32_FLOAT;
  if(mode==4) surface_data.format=target.host_format=F::R8G8B8A8_UNORM_SRGB;
  if(mode==5) target.stacked=true;
  if(mode==6) target.dimension=TextureDimension::Tex3D;
  if(mode==7) state.resolve_copy_ps.reset();
  if(mode==8) fail_pipeline=true;
  if(mode==9) fail_framebuffer=true;
  if(mode==10) fail_slot=true;
  if(mode==11) target.host_format=F::R16_UNORM;
  ColorResolve(Flags(-3)); assert(ops.scales==1);
  if(mode==3) assert(ops.draws==1 && !ops.copies && last_push.param0==1.f);
  else assert(!ops.draws && ops.copies==1);
 }
 Reset(); surface_data.samples=plume::RenderSampleCount::COUNT_4; ColorResolve(Flags(-3));
 assert(ops.resolves==1 && ops.scales==1 && !ops.draws);
 Reset(); surface_data.samples=plume::RenderSampleCount::COUNT_4; quad=true; ColorResolve(Flags(-3));
 assert(ops.quads==1 && ops.scales==1 && !ops.draws && !ops.resolves);
 Reset(); backend=1; surface_data.samples=plume::RenderSampleCount::COUNT_4; target.extra_levels=2; ColorResolve(Flags(-3));
 assert(ops.resolves==1 && ops.copies==1 && ops.scales==1 && ops.mips==1);
 // Host mip construction follows the biased resolve, including when fused.
 Reset(); target.extra_levels=2; ColorResolve(Flags(-3)); assert(ops.draws==1 && ops.mips==1 && !ops.scales);
 // Offset, clipping, fractional scale and subresource fallback use the original fit path.
 Reset(); ClearRect part{7,11,38,40}; ColorResolve(Flags(-3),&part,3,5);
 assert(last_push.translate[0]==4 && last_push.translate[1]==6);
 assert(commands.scissor.left==3 && commands.scissor.top==5 && commands.scissor.right==34 && commands.scissor.bottom==34);
 Reset(); surface_data.scale={6}; target.alloc_width=target.alloc_height=32; ColorResolve(Flags(-6),&part,3,5);
 assert(ops.draws==1 && !ops.scales && last_push.param0==1.f/64);
 Reset(); ColorResolve(Flags(-3),nullptr,0,0,1); assert(ops.draws==1 && ops.scales==1 && last_push.param0==1.f);
 Reset(); ColorResolve(Flags(-3),nullptr,0,0,0,1); assert(ops.copies==1 && !ops.draws && !ops.scales);
 Reset(); ClearRect empty{0,0,0,0}; ColorResolve(Flags(-3),&empty); assert(!ops.draws && !ops.copies && !ops.scales);
 // Proof that naively fusing the 2x2 averaging path would change half-float pixels.
 volatile _Float16 tiny=0x1p-24f;
 float average=(float(tiny)+float(tiny))*0.25f;
 _Float16 intermediate=(_Float16)average;
 _Float16 separate=(_Float16)(float(intermediate)*2.f),naive=(_Float16)(average*2.f);
 assert(float(separate)!=float(naive));
 std::cout << "PASS: actual resolve dispatch, all biases, bias-aware skip, switches, rectangles, mips, failures and MSAA/format fallbacks\n";
}
'''
text=prefix+draw+'\n'+signature+'\n'+wrapper+tests
includes=['-I'+str(root/'app/src'),'-I'+str(root/'app/thirdparty/plume')]
def run(name,text):
    path=out/(name+'.cpp'); path.write_text(text); exe=out/name
    subprocess.run([args.cxx,'-std=c++23','-O2','-g','-ffp-contract=off',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all',*includes,str(path),'-o',str(exe)],check=True)
    return subprocess.run([str(exe)],capture_output=True,text=True)
result=run('dispatch',text)
log=result.stdout+result.stderr
print(log,end='')
if result.returncode:
    (out.parent/'dispatch_tests.log').write_text(log)
    raise SystemExit(result.returncode)
for name,before,after in [
    ('missing_bias_signature','mix(uint32_t(exponent_bias));','/* bias omitted */ (void)exponent_bias;'),
    ('double_bias','const bool scaled = bias_fused ||','const bool scaled = false ||'),
]:
    assert text.count(before)==1
    failure=run(name,text.replace(before,after))
    (out/(name+'.log')).write_text(failure.stdout+failure.stderr)
    assert failure.returncode!=0,name+' was not detected'
    line='PASS negative control detected: '+name+'\n'; log+=line; print(line,end='')
(out.parent/'dispatch_tests.log').write_text(log)
