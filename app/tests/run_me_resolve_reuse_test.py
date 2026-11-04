"""Exercise actual resolve/reuse/lifetime functions with a queued GPU model.

No game, NRO, driver or shader build. Requires clang++ and uses ASan/UBSan.
"""
from pathlib import Path
import argparse, subprocess

root = Path(__file__).resolve().parents[2]
out = root/'out/fixes/resolve_reuse_20261029/tests'
out.mkdir(parents=True, exist_ok=True)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx', default='clang++')
args = parser.parse_args()
source = (root/'app/src/renut_engine/gpu/render/textures.cpp').read_text()
header = (root/'app/src/renut_engine/gpu/render/textures.h').read_text()
backend = (root/'app/src/renut_engine/gpu/backend/backend.cpp').read_text()

def block(text, start):
    begin = text.index(start)
    pos = text.index('{', begin)+1
    nesting = 1
    while nesting:
        nesting += (text[pos] == '{')-(text[pos] == '}')
        pos += 1
    return text[begin:pos]

prefix = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <vector>
#include "renut_engine/gpu/render/render_scale.h"
using renut::gpu::RenderScale;
using u32=uint32_t;
#define PLUME_DEKO3D 1
#define REXCVAR_GET(name) name
#define RGPU_INFO(...) ((void)0)
#define RGPU_WARN(...) ((void)0)
#define RGPU_ERROR(...) ((void)0)
int me2_renut_resolve_pool_mb=32;
bool deko=true;
bool IsDeko3D() { return deko; }
uint64_t next_id=0, creates=0, discards=0, destroys=0, queries=0;
uint64_t read_to_write=0;
uint64_t reserve_bytes=192ull<<20;
uint64_t allocation_override=0;
bool fail_create=false;
uint32_t next_slot=0, slots_used=0;
constexpr uint32_t kInvalidDescriptor=UINT32_MAX-1, kEvictPressureSlots=3000;
namespace plume {
enum class RenderFormat { UNKNOWN, COLOR, HDR, DEPTH };
enum class RenderTextureLayout { UNKNOWN, COLOR_WRITE, SHADER_READ, COPY_DEST };
struct RenderTexture {
 uint64_t id=++next_id;
 bool valid=true, ownsMemory=true;
 struct { uint32_t size=0; } alloc;
 uint32_t value=0;
 ~RenderTexture() { ++destroys; }
};
using Deko3DTexture=RenderTexture;
struct RenderTextureView { RenderTexture *texture=nullptr; };
struct RenderTextureViewDesc {};
struct RenderFramebuffer { RenderTexture *texture=nullptr; };
struct RenderCommandList {
 std::vector<std::function<void()>> queue;
 // deko3d's actual discardTexture is a no-op; do not pretend it clears pixels.
 void discardTexture(RenderTexture *) { ++discards; }
 void execute() { for(auto &work:queue) work(); queue.clear(); }
};
}
constexpr auto kDepthResolveFormat=plume::RenderFormat::DEPTH;
enum class TextureDimension { Tex2D, Tex3D, Cube };
enum class GpuTextureFormat { Fmt8_8_8_8 };
struct TextureFormatInfo {};
struct DepthImage {};
struct HostSurface {};
'''
host_texture = block(header,'struct HostTexture {')+';\n'
support = r'''
struct FakeState { std::unique_ptr<int> device=std::make_unique<int>(1); } state;
FakeState &State() { return state; }
struct GpuMemoryUsage { bool valid=true; uint64_t local_used=256ull<<20,local_budget=1024ull<<20; } memory;
GpuMemoryUsage QueryGpuMemory(int*) { ++queries; return memory; }
namespace banjo::renut_gpu { uint64_t TextureReserveBytes() { return reserve_bytes; } }
plume::RenderCommandList commands;
std::vector<std::unique_ptr<plume::RenderTexture>> retired_textures;
std::vector<std::unique_ptr<plume::RenderTextureView>> retired_views;
std::vector<std::unique_ptr<plume::RenderFramebuffer>> retired_frames;
std::vector<uint32_t> retired_slots;
namespace Video {
 uint32_t TextureSlotsInUseLocked() { return slots_used; }
 void FreeTextureSlotLocked(uint32_t slot) { retired_slots.push_back(slot); }
 void RetireLocked(std::unique_ptr<plume::RenderTexture> t) { if(t) retired_textures.push_back(std::move(t)); }
 void RetireLocked(std::unique_ptr<plume::RenderTextureView> t) { if(t) retired_views.push_back(std::move(t)); }
 void RetireLocked(std::unique_ptr<plume::RenderFramebuffer> t) { if(t) retired_frames.push_back(std::move(t)); }
 plume::RenderCommandList *CommandListLocked() { return &commands; }
}
std::unordered_map<u32,std::unique_ptr<HostTexture>> g_textures;
std::unordered_map<u32,HostTexture*> g_resolve_targets,g_depth_words;
uint64_t g_frame=1, g_texture_epoch=1;
struct TextureMemo { u32 va=0; } memo;
TextureMemo &TextureMemoFor(u32) { return memo; }
auto &TextureSlot(u32 va) { return g_textures[va]; }
void DepthHandOverForgetLocked(HostTexture &t) { t.handed_from=nullptr; t.held.reset(); }
namespace d3d {
 struct TextureFetchConstant { u32 dword[6]={2,0x100000,640|(360u<<16),0,0,0}; };
 struct BaseTexture { TextureFetchConstant Format; };
}
struct TextureFetch {
 u32 dword[6]{};
 static TextureFetch FromHeader(const d3d::TextureFetchConstant &h) {
  TextureFetch f; std::memcpy(f.dword,h.dword,sizeof(f.dword)); return f;
 }
 bool IsTexture() const { return (dword[0]&3)==2; }
 u32 Swizzle() const { return (dword[3]>>1)&0xfff; }
 u32 BasePage() const { return dword[1]>>12; }
};
template<class T> T *GuestPtr(u32) { return nullptr; }
void MaskedDesc(const TextureFetch &f,u32 desc[6]) { std::memcpy(desc,f.dword,sizeof(f.dword)); }
bool Create(HostTexture &t,const TextureFetch &f,bool rt,plume::RenderFormat format,
            bool,RenderScale scale,bool) {
 ++creates; ++g_texture_epoch;
 t.render_target=rt; t.host_format=format; t.scale=scale;
 t.width=f.dword[2]&0xffff; t.height=f.dword[2]>>16;
 t.alloc_width=scale.Size(t.width); t.alloc_height=scale.Size(t.height);
 if(fail_create) return false;
 t.texture=std::make_unique<plume::RenderTexture>();
 t.texture->alloc.size=allocation_override ? allocation_override : t.alloc_width*t.alloc_height*4;
 t.view=std::make_unique<plume::RenderTextureView>(); t.view->texture=t.texture.get();
 t.slot=next_slot++; ++slots_used;
 return true;
}
void TransitionTextureLocked(HostTexture *t,plume::RenderTextureLayout layout) {
 // The previous layout survives pooling, so a read -> write transition is observable here.
 if(t->layout==plume::RenderTextureLayout::SHADER_READ && layout==plume::RenderTextureLayout::COLOR_WRITE) ++read_to_write;
 if(t->layout!=layout) { auto *image=t->texture.get(); commands.queue.push_back([image]{assert(image->valid);}); }
 t->layout=layout;
}
void ReleaseHostTextureLocked(u32);
'''
pool = source[source.index('// Allocations only:'):source.index('// Two descriptions of the same data:')]
functions = '\n'.join(block(source,sig) for sig in [
    'void Retire(HostTexture &t)',
])+'\n'+pool+'\n'+'\n'.join(block(source,sig) for sig in [
    'HostTexture *ResolveTarget(', 'void ReleaseResolvePoolLocked()',
    'void ReleaseHostTextureLocked(u32 texture_va)', 'void ReleaseHostTexturesInMemoryLocked(u32 physical, u32 size)',
])
tests = r'''
using Format=plume::RenderFormat;
HostTexture *resolve(u32 va,const d3d::TextureFetchConstant &h,RenderScale s={},Format fmt=Format::COLOR,bool depth=false) {
 TextureFetch fetch; return ResolveTarget(va,fmt,s,&h,depth,fetch);
}
uint32_t spare_count() { uint32_t n=0; for(auto &s:g_resolve_spares) n+=bool(s.target); return n; }
void fence() {
 commands.execute();
 retired_frames.clear(); retired_views.clear(); retired_textures.clear();
 slots_used-=retired_slots.size(); retired_slots.clear();
}
void reset() {
 ReleaseResolvePoolLocked();
 for(auto &[va,t]:g_textures) if(t) Retire(*t);
 g_textures.clear(); g_resolve_targets.clear(); g_depth_words.clear();
 fence(); assert(g_resolve_spare_bytes==0); assert(slots_used==0);
 ++g_frame; memory={}; reserve_bytes=192ull<<20; deko=true; me2_renut_resolve_pool_mb=32;
 allocation_override=0; fail_create=false; g_resolve_pool_stats={};
 creates=discards=destroys=queries=read_to_write=0;
}
void write_and_sample(HostTexture *t,u32 page,uint32_t value) {
 assert(g_resolve_targets.at(page)==t);
 auto *texture=t->texture.get(); auto *view=t->view.get();
 commands.queue.push_back([texture,value]{texture->value=value;});
 commands.queue.push_back([texture,view,value]{ assert(view->texture==texture); assert(texture->value==value); });
 t->layout=plume::RenderTextureLayout::SHADER_READ;
 t->resolve_signature=value+1; t->resolves=99; t->copied_resolve=7;
}
int main() {
 d3d::TextureFetchConstant h;
 constexpr u32 va=0x40052270;
 constexpr RenderScale low{6}, high{0};
 // Queue all work before GPU execution: old views/descriptors must stay alive,
 // every reacquisition must invalidate skip state, and every sample sees its write.
 for(int enabled:{0,32}) {
  reset(); me2_renut_resolve_pool_mb=enabled;
  std::array<uint64_t,2> ids{};
  for(uint32_t i=0;i<10000;++i) {
   auto *t=resolve(va,h,(i&1)?high:low);
   assert(t && t->resolve_signature==0 && t->resolves==0 && t->copied_resolve==0);
   assert(t->gpu_written && t->uploaded && t->used_frame==g_frame);
   const unsigned index=i&1;
   if(enabled && ids[index]) assert(ids[index]==t->texture->id);
   ids[index]=t->texture->id;
   write_and_sample(t,h.dword[1]>>12,i);
   assert(g_resolve_targets.size()==1);
   if(i%30==29) ++g_frame;
  }
  assert(creates==uint64_t(enabled?2:10000));
  assert(discards==10000);
  assert(read_to_write==uint64_t(enabled?9998:0));
  assert(!destroys); // Even evictions must wait for the queued work's fence.
  fence();
  std::cout << "10,000 alternating resolves, pool " << enabled << " MB: " << creates << " allocations; queued pixels/views/aliases PASS\n";
 }
 // Same-scale repeated resolves must keep their current content/signature.
 auto *active=g_textures.at(va).get(); active->resolve_signature=123;
 const auto old_discards=discards;
 assert(resolve(va,h,high)==active && active->resolve_signature==123 && discards==old_discards);
 // Two independent guest targets alternating in every frame (scene and HUD).
 reset(); auto second=h; second.dword[1]=0x200000;
 for(u32 i=0;i<1000;++i) {
  ++g_frame;
  for(auto scale:{low,high}) {
   write_and_sample(resolve(va,h,scale),h.dword[1]>>12,i);
   write_and_sample(resolve(va+4,second,scale),second.dword[1]>>12,i+1);
  }
 }
 assert(creates==4 && spare_count()==2 && g_resolve_targets.size()==2);
 fence();
 std::cout << "Two targets, 4,000 resolves: 4 allocations; independent queued output PASS\n";
 // The allocation may keep expensive derived views and framebuffers, never its stale resolve signature.
 reset(); auto *first=resolve(va,h,low);
 first->copy_framebuffer=std::make_unique<plume::RenderFramebuffer>();
 first->copy_framebuffer->texture=first->texture.get();
 first->level0_view=std::make_unique<plume::RenderTextureView>(); first->level0_slot=next_slot++; ++slots_used;
 auto *frame=first->copy_framebuffer.get(); auto *level=first->level0_view.get();
 first->swizzle_views.emplace_back(); first->swizzle_views.back().slot=next_slot++; ++slots_used;
 first->swizzle_views.back().view=std::make_unique<plume::RenderTextureView>();
 const auto old_slot=first->slot;
 resolve(va,h,high); auto *again=resolve(va,h,low);
 assert(again==first && again->copy_framebuffer.get()==frame && again->level0_view.get()==level && again->slot==old_slot);
 assert(again->swizzle_views.size()==1 && retired_slots.empty());
 // VA release clears its inactive allocation too, and delayed retire keeps queued reads alive.
 write_and_sample(again,h.dword[1]>>12,42);
 ReleaseHostTextureLocked(va); assert(!spare_count() && g_textures.empty() && g_resolve_targets.empty());
 assert(destroys==0); fence(); assert(destroys==2);
 // A parked allocation must disappear even when there is no active map entry.
 reset(); resolve(va,h,low); resolve(va,h,high);
 auto &slot=g_textures.at(va); Retire(*slot); g_textures.erase(va);
 ReleaseHostTextureLocked(va); assert(!spare_count());
 // Parking removes all alias registrations before another destination is published.
 reset(); resolve(va,h,low);
 g_resolve_targets[999]=g_textures.at(va).get();
 assert(ParkResolveTarget(g_textures.at(va)) && g_resolve_targets.empty());
 g_textures.erase(va); // Park is normally followed immediately by publishing the next active target.
 ReleaseHostTextureLocked(va); assert(!spare_count());
 // Guest physical-memory release catches both a base page and a nonzero mip page.
 for(bool mip:{false,true}) {
  reset(); auto hh=h; if(mip) hh.dword[5]=0x300000;
  resolve(va,hh,low); resolve(va,hh,high);
  ReleaseHostTexturesInMemoryLocked(mip?0x300000:0x100000,4096);
  assert(!spare_count() && g_textures.empty() && g_resolve_targets.empty());
 }
 // Exact description, format and VA matching; never borrow a different logical target.
 reset(); resolve(va,h,low); resolve(va,h,high);
 u32 desc[6]; auto fetch=TextureFetch::FromHeader(h); MaskedDesc(fetch,desc);
 for(int field=0;field<6;++field) {
  auto changed=std::array<u32,6>{}; std::copy(desc,desc+6,changed.begin()); changed[field]^=0x100;
  assert(!TakeResolveSpare(va,changed.data(),Format::COLOR,low));
 }
 assert(!TakeResolveSpare(va+4,desc,Format::COLOR,low));
 assert(!TakeResolveSpare(va,desc,Format::HDR,low));
 assert(!TakeResolveSpare(va,desc,Format::COLOR,high));
 // Changed headers/format and depth requests retain the original rebuild path.
 auto h2=h; h2.dword[2]++;
 auto before=creates; resolve(va,h2,low); assert(creates==before+1);
 before=creates; resolve(va,h2,high,Format::HDR); assert(creates==before+1);
 before=creates; resolve(va,h2,low,Format::DEPTH,true); assert(creates==before+1);
 auto *d=g_textures.at(va).get(); assert(!ReusableResolveTarget(*d));
 d->host_format=Format::COLOR; d->words=std::make_unique<HostTexture>(); assert(!ReusableResolveTarget(*d));
 d->words.reset(); d->dimension=TextureDimension::Cube; assert(!ReusableResolveTarget(*d));
 d->dimension=TextureDimension::Tex2D; d->stacked=true; assert(!ReusableResolveTarget(*d));
 d->stacked=false; d->render_target=false; assert(!ReusableResolveTarget(*d));
 // Strict retained-byte and entry bounds, oldest eviction and oversize rejection.
 reset(); allocation_override=8ull<<20;
 for(u32 i=0;i<12;++i) { ++g_frame; resolve(va+i*4,h,low); resolve(va+i*4,h,high); }
 assert(g_resolve_spare_bytes==(32ull<<20) && spare_count()==4);
 assert(g_resolve_pool_stats.evicted==8);
 assert(!TakeResolveSpare(va,desc,Format::COLOR,low));
 auto newest=TakeResolveSpare(va+11*4,desc,Format::COLOR,low);
 assert(newest); Retire(*newest); newest.reset();
 reset(); allocation_override=64*1024;
 for(u32 i=0;i<12;++i) { ++g_frame; resolve(va+i*4,h,low); resolve(va+i*4,h,high); }
 assert(spare_count()==8 && g_resolve_spare_bytes==8*64*1024);
 reset(); allocation_override=33ull<<20; resolve(va,h,low); resolve(va,h,high); assert(!spare_count());
 // Exact allocation size reporting, including invalid, non-owned and other backend exclusions.
 auto *image=g_textures.at(va)->texture.get(); assert(TextureAllocationBytes(image)==(33ull<<20));
 image->ownsMemory=false; assert(!TextureAllocationBytes(image)); image->ownsMemory=true;
 image->valid=false; assert(!TextureAllocationBytes(image)); image->valid=true;
 deko=false; assert(!TextureAllocationBytes(image)); deko=true; assert(!TextureAllocationBytes(nullptr));
 // Pressure and unknown-budget fallback; cached allocations yield before active resources.
 for(int reason=0;reason<5;++reason) {
  reset(); resolve(va,h,low); resolve(va,h,high); assert(spare_count()==1);
  ++g_frame;
  if(reason==0) memory.local_budget=memory.local_used+(63ull<<20);
  if(reason==1) memory.valid=false;
  if(reason==2) memory.local_budget=0;
  if(reason==3) slots_used=kEvictPressureSlots;
  if(reason==4) memory.local_used=memory.local_budget+1;
  TrimResolveSpares(); assert(!spare_count() && g_textures.at(va)->texture);
  if(reason==3) slots_used=2; // Mock pressure came from unrelated active descriptors.
 }
 reset(); resolve(va,h,low); resolve(va,h,high);
 ++g_frame; auto count=queries; assert(ResolvePoolAllowed()); assert(ResolvePoolAllowed()); assert(queries==count+1);
 slots_used=kEvictPressureSlots; assert(!ResolvePoolAllowed()); slots_used=2;
 // Idle expiry is based on time parked, not the target's last content use.
 g_frame+=119; TrimResolveSpares(); assert(!spare_count());
 reset(); resolve(va,h,low); resolve(va,h,high);
 g_frame+=119; TrimResolveSpares(); assert(spare_count()==1);
 ++g_frame; TrimResolveSpares(); assert(!spare_count());
 // Non-deko, disabled and allocation-failure paths remain safe.
 for(bool other:{false,true}) {
  reset(); if(other) deko=false; else me2_renut_resolve_pool_mb=0;
  resolve(va,h,low); resolve(va,h,high); resolve(va,h,low); assert(creates==3 && !spare_count());
 }
 reset(); resolve(va,h,low); fail_create=true; assert(!resolve(va,h,high));
 fail_create=false; assert(resolve(va,h,high));
 reset(); resolve(va,h,low); resolve(va,h,high); ReleaseResolvePoolLocked();
 assert(!g_resolve_spare_bytes && !spare_count());
 reset();
 std::cout << "PASS: production resolve pool bounds, freshness, aliases, lifetime, release, pressure and fallback regressions\n";
}
'''
text = prefix+host_texture+support+block(backend,'uint64_t TextureAllocationBytes(')+'\n'+functions+tests
test = out/'resolve_reuse.cpp'
test.write_text(text)
exe = out/'resolve_reuse'
subprocess.run([args.cxx,'-std=c++23','-O2','-g','-fsanitize=address,undefined',
                '-fno-sanitize-recover=all','-I'+str(root/'app/src'),str(test),'-o',str(exe)],check=True)
result=subprocess.run([str(exe)],capture_output=True,text=True)
log=result.stdout+result.stderr
print(log,end='')
if result.returncode:
    (out.parent/'test.log').write_text(log)
    raise SystemExit(result.returncode)
# Deliberately break freshness, ownership and the byte cap. Each must fail;
# otherwise the lifetime/contents tests would be giving false reassurance.
for name,old,new in [
    ('stale_signature','target->resolve_signature = 0;','/* stale resolve signature */'),
    ('early_destruction','Retire(*spare.target);','/* destroy before queued work finishes */'),
    ('unbounded_bytes','g_resolve_spare_bytes + bytes <= cap','true'),
]:
    assert text.count(old)==1, name
    mutated=out/(name+'.cpp')
    mutated.write_text(text.replace(old,new))
    negative=out/name
    subprocess.run([args.cxx,'-std=c++23','-O1','-g','-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all','-I'+str(root/'app/src'),str(mutated),'-o',str(negative)],check=True)
    failure=subprocess.run([str(negative)],capture_output=True,text=True)
    (out/(name+'.log')).write_text(failure.stdout+failure.stderr)
    assert failure.returncode != 0, name+' was not detected'
    line='PASS negative control detected: '+name+'\n'
    log+=line
    print(line,end='')
(out.parent/'test.log').write_text(log)
