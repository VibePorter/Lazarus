"""Offline differential tests for the traversal optimizations; no game/NRO build.

Uses actual production texture routines and actual generated PPC frustum code.
Requires clang++ (or --cxx) and the project's SIMDe headers.
"""
from pathlib import Path
import argparse
import re
import subprocess

root = Path(__file__).resolve().parents[2]
out = root / 'out/fixes/traversal_perf_20261029/test'
out.mkdir(parents=True, exist_ok=True)
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--cxx', default='clang++')
args = ap.parse_args()


def function(text, signature):
    start = text.index(signature)
    body = text.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def run(name, text, extra=()):
    path = out / (name + '.cpp')
    path.write_text(text)
    exe = out / name
    subprocess.run([args.cxx, '-std=c++23', '-O2', '-ffp-contract=off', '-fsanitize=undefined',
                    '-fno-sanitize-recover=all', '-I' + str(root / 'sdk/include'),
                    '-I' + str(root / 'sdk/thirdparty/simde'),
                    '-I' + str(root / 'sdk/thirdparty/fmt/include'),
                    *extra, str(path), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


texture = (root / 'app/src/renut_engine/gpu/render/textures.cpp').read_text()
before = (root / 'app/tests/fixtures/me_read_level_before_fused.inc').read_text()
layout = (root / 'app/src/renut_engine/gpu/render/texture_layout.cpp').read_text()
layout_h = (root / 'app/src/renut_engine/gpu/render/texture_layout.h').read_text()
format_cpp = (root / 'app/src/renut_engine/gpu/render/format.cpp').read_text()
prefix = r'''
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using u32 = uint32_t;
using u8 = uint8_t;
using namespace renut::gpu;
static unsigned scratch_allocations = 0;
template<class T> struct TrackingVector : std::vector<T> {
  explicit TrackingVector(size_t n) : std::vector<T>(n) { ++scratch_allocations; }
};
enum class TextureDimension { Tex2D, Tex3D };
struct TextureFetch {
  bool tiled; unsigned endian; TextureDimension dimension;
  bool Tiled()const{return tiled;}
  unsigned Endian()const{return endian;}
  TextureDimension Dimension()const{return dimension;}
};
enum class TexelConversion {Copy, Other};
struct TextureFormatInfo {
  TexelConversion conversion = TexelConversion::Copy;
  unsigned block_size = 1, guest_block_bytes = 4, host_block_bytes = 4;
  bool decoded_blocks = false;
};
unsigned conversion_calls = 0;
void ConvertTexels(TexelConversion, const uint8_t*, uint8_t*, uint32_t) { ++conversion_calls; }
void DecodeBlockRow(TexelConversion, const uint8_t*, uint8_t*, uint32_t, uint32_t) { ++conversion_calls; }
bool (*g_texture_replace)(uint32_t,uint32_t,uint8_t*) = nullptr;
u32 AlignMask(u32 v,u32 a){return (v+a-1)&~(a-1);}
'''
header = '#include "' + str(root / 'app/src/renut_engine/gpu/render/texture_copy.h') + '"\n'
parts = [header, prefix, function(before, 'struct LevelSource') + ';',
         function(layout_h, 'constexpr u32 TiledRunBlocks'),
         function(layout, 'u32 TiledOffset2D'), function(layout, 'u32 TiledOffset3D'),
         function(format_cpp, 'void SwapEndian'),
         function(before, 'void ReadLevel').replace('ReadLevel(', 'Reference(', 1),
         function(texture, 'template <uint32_t Endian>\nvoid ReadCopy2D'),
         function(texture, 'void ReadLevel')]
parts = [p.replace('std::vector<uint8_t> row', 'TrackingVector<uint8_t> row') for p in parts]
parts.append(r'''
int main() {
  std::mt19937 rng(413);
  std::vector<uint8_t> source(8*1024*1024);
  for(auto& v:source)v=uint8_t(rng());
  unsigned cases=0;
  for(unsigned k=0;k<12000;++k) {
    const unsigned sizes[]={1,2,4,8,16};
    unsigned gb=sizes[k%5], w=1+rng()%129, h=1+rng()%65;
    unsigned depth=k%13==0?2:1, x=rng()%33, y=rng()%33;
    TextureFetch fetch{bool(k&1),k%4,depth>1?TextureDimension::Tex3D:TextureDimension::Tex2D};
    TextureFormatInfo info;info.guest_block_bytes=info.host_block_bytes=gb;
    info.block_size=k%3==0?4:1;
    LevelSource src{source.data()+(k%7),256,128,x,y,k%3};
    unsigned pitch=((w*gb+255)/256)*256, slice=pitch*h;
    std::vector<uint8_t> a(slice*depth+32,0xCD),b=a;
    g_texture_replace = k%7==0 ? +[](uint32_t width,uint32_t height,uint8_t* p) {
      for(size_t i=0;i<size_t(width)*height*4;i+=11)p[i]^=0x65;
      return true;
    } : nullptr;
    Reference(info,fetch,src,w,h,depth,a.data()+3,pitch,slice);
    ReadLevel(info,fetch,src,w,h,depth,b.data()+3,pitch,slice);
    if(a!=b){std::printf("texture mismatch case %u gb %u w %u h %u endian %u\n",k,gb,w,h,fetch.endian);return 1;}
    ++cases;
  }
  TextureFormatInfo info;info.guest_block_bytes=info.host_block_bytes=16;info.block_size=4;
  TextureFetch fetch{true,1,TextureDimension::Tex2D};
  LevelSource src{source.data(),256,128,0,0,0};
  std::vector<uint8_t> a(128*128*16),b=a;
  g_texture_replace=nullptr;scratch_allocations=0;
  Reference(info,fetch,src,128,128,1,a.data(),2048,a.size());
  assert(scratch_allocations==1);scratch_allocations=0;
  ReadLevel(info,fetch,src,128,128,1,b.data(),2048,b.size());
  assert(scratch_allocations==0 && a==b);
  info.conversion=TexelConversion::Other;
  for(bool decoded:{false,true}) {
    info.decoded_blocks=decoded;conversion_calls=0;
    ReadLevel(info,fetch,src,128,128,1,b.data(),2048,b.size());
    assert(conversion_calls==128);
  }
  std::printf("PASS: %u production texture cases (all endian modes, tiled/linear, mip-tail offsets, "
              "unaligned data, padding, 3D and icon callbacks); zero row allocations on DXT path; conversion fallback\n",cases);
}
''')
run('textures', '\n'.join(parts))

# Compile the real generated original, with only memory/marshalling macros supplied
# by this isolated harness. SIMD and FPSCR operations come from the actual SDK.
generated = next(p for p in (root / 'app/generated/default').glob('sa2_recomp.*.cpp')
                 if 'DEFINE_REX_FUNC(sub_822CDFF0)' in p.read_text())
original = function(generated.read_text(), 'DEFINE_REX_FUNC(sub_822CDFF0)').replace(
    'DEFINE_REX_FUNC(sub_822CDFF0)', 'void Reference(PPCContext& ctx, uint8_t* base)')
prefix = r'''
#include <cassert>
#include <bit>
#include <cstdio>
#include <random>
#include <vector>
#include <rex/ppc/context.h>
#include <rex/ppc/intrinsics.h>
namespace rex::syscount { void CensusSite(unsigned int) {} }
#define REX_FUNC_PROLOGUE() ((void)0)
#define REX_LEAF_LOCAL(T,N,F) T N=ctx.F
#define REX_LEAF_STORE(N,F) ctx.F=N
#define REX_LOOP_BARRIER() ((void)0)
#define REX_RAW_ADDR(A) (base+uint32_t(A))
#define REX_BSWAP128(V) simde_mm_shuffle_epi8((V),simde_mm_load_si128((simde__m128i*)VectorMaskL))
#define REX_SHUFFLE_TABLE(V,T) simde_mm_shuffle_epi8((V),simde_mm_load_si128((simde__m128i*)(T)))
uint32_t L32(const uint8_t* b,uint32_t a){uint32_t v;std::memcpy(&v,b+a,4);return __builtin_bswap32(v);}
void S32(uint8_t* b,uint32_t a,uint32_t v){v=__builtin_bswap32(v);std::memcpy(b+a,&v,4);}
#define REX_LOAD_D_U32(A,D) L32(base,uint32_t((A)+(D)))
#define REX_STORE_D_U32(A,D,V) S32(base,uint32_t((A)+(D)),(V))
'''
prefix += '\n#include "' + str(root / 'app/me2_src/hot/n/n_822CDFF0.h') + '"\n' + original
prefix += r'''
int main() {
  std::mt19937 rng(20413);
  std::vector<uint8_t> memory(0x40000,0);
  unsigned cases=0,visible=0,hidden=0;
  for(unsigned k=0;k<150000;++k) {
    auto bits=[&](){
      if(k%5==0)return uint32_t(rng());
      return std::bit_cast<uint32_t>(float(int(rng()%20001)-10000)/32.f);
    };
    PPCContext a{},b{};
    a.r1.u64=0x30000;a.r3.u64=0x4000;a.r4.u64=0x8000+(k%16);
    a.r0.u64=rng();a.r5.u64=rng();a.r6.u64=rng();
    const int count=int(k%22)-2;
    S32(memory.data(),0x4010,uint32_t(count));S32(memory.data(),0x400C,0x10000+(k%16));
    for(unsigned i=0;i<3;++i)S32(memory.data(),a.r4.u32+4*i,bits());
    for(unsigned i=0;i<80;++i)S32(memory.data(),0x10000+4*i,bits());
    a.f1.f64=std::bit_cast<float>(bits());
    if(k%23==0)a.f1.f64=1e100;
    using FP=rex::platform::FPSCRPlatform;
    auto csr=(a.fpscr.getcsr() & ~(FP::FlushMask | FP::RoundMaskVal)) | FP::GuestToHost[k%4];
    if(k&1)csr|=FP::FlushMask;
    a.fpscr.setcsr(csr);
    a.fpscr.csr=a.fpscr.getcsr();b=a;
    Reference(a,memory.data());const auto fp_a=a.fpscr.getcsr();
    b.fpscr.setcsr(csr);
    me::hot::n_822CDFF0::Native(b,memory.data());const auto fp_b=b.fpscr.getcsr();
    if(a.r3.u64!=b.r3.u64 || a.r0.u64!=b.r0.u64 || a.r5.u64!=b.r5.u64 || a.r6.u64!=b.r6.u64 ||
       ((fp_a^fp_b)&(FP::FlushMask | FP::RoundMaskVal)) || a.fpscr.csr!=b.fpscr.csr) {
      std::printf("frustum mismatch at %u count %d result %llu/%llu fp %x/%x\n",k,count,
                  (unsigned long long)a.r3.u64,(unsigned long long)b.r3.u64,fp_a,fp_b);return 1;
    }
    if(a.r3.u32)++visible;else ++hidden;
    ++cases;
  }
  std::printf("PASS: %u actual generated PPC / native frustum cases (%u visible, %u hidden): "
              "unaligned centers/plane pointers, signed counts, NaN/Inf/denormals, return/live registers and flush modes\n",
              cases,visible,hidden);
}
'''
run('frustum', prefix)
print('PASS: traversal performance differential regressions')
