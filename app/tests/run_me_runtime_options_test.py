"""Offline tests of actual startup hook, guest skip branch and frame pacing.

No NRO/game launch. Run under WSL/Linux with clang++ and UBSan.
"""
from pathlib import Path
import argparse, importlib.util, subprocess, tempfile, tomllib
root=Path(__file__).resolve().parents[2]
out=root/'out/fixes/runtime_options_20261029/tests'
out.mkdir(parents=True,exist_ok=True)
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx',default='clang++')
args=parser.parse_args()

def block(text,start):
    begin=text.index(start)
    end=text.index('{',begin)+1
    depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}')
        end+=1
    return text[begin:end]

hooks=(root/'app/me2_src/me2_hooks.cpp').read_text()
pacing=(root/'app/src/renut_engine/gpu/sa2_renut_pacing.cpp').read_text()
guest=(root/'app/generated/default/sa2_recomp.187.cpp').read_text()
prefix=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
#define REXCVAR_GET(x) x
#define REXLOG_INFO(...) ((void)0)
bool me2_skip_intro_videos=false,me2_unlock_framerate=false,sa2_renut_vblank_pacing=true,mgr_renut_vsync_relaxed=true;
union PPCRegister { uint64_t u64=0; int64_t s64; uint32_t u32; int32_t s32; };
struct PPCXERRegister {};
struct PPCCRRegister {
 bool eq=false,lt=false,gt=false;
 template<class T> void compare(T a,T b,PPCXERRegister&) { eq=a==b; lt=a<b; gt=a>b; }
};
struct PPCContext { PPCRegister r1,r3,r4,r5,r6,r7,r10; uint64_t lr=0x1234; };
uint32_t read32(uint8_t *base,uint32_t at) { uint32_t n; std::memcpy(&n,base+at,4); return __builtin_bswap32(n); }
uint64_t read64(uint8_t *base,uint32_t at) { uint64_t n; std::memcpy(&n,base+at,8); return __builtin_bswap64(n); }
void write32(uint8_t *base,uint32_t at,uint32_t v) { v=__builtin_bswap32(v); std::memcpy(base+at,&v,4); }
void write64(uint8_t *base,uint32_t at,uint64_t v) { v=__builtin_bswap64(v); std::memcpy(base+at,&v,8); }
#define DEFINE_REX_FUNC(name) void __imp__##name(PPCContext &ctx,uint8_t *base)
#define REX_HOOK_RAW(name) void name(PPCContext &ctx,uint8_t *base)
#define REX_FUNC_PROLOGUE()
#define REX_LR_READ(r) ((r).u64=ctx.lr)
#define REX_LR_WRITE(r) (ctx.lr=(r).u64)
#define REX_DEAD_LR_SAVE(x) x
#define REX_DEAD_SAVE(x) x
#define REX_CHAIN_STORE(x) x
#define REX_STORE_D_U32(at,off,v) write32(base,uint32_t(at)+(off),v)
#define REX_STORE_D_U64(at,off,v) write64(base,uint32_t(at)+(off),v)
#define REX_STORE_U32(at,v) write32(base,at,v)
#define REX_LOAD_D_U32(at,off) read32(base,uint32_t(at)+(off))
#define REX_LOAD_D_U64(at,off) read64(base,uint32_t(at)+(off))
bool command_skip=false;
unsigned movies=0,original_calls=0;
void __imp__sub_82795198(PPCContext &ctx,uint8_t*) {
 assert(ctx.r4.u32==0x820d7fcc); // Actual UTF-16 nostartupmovies string in this image.
 ++original_calls; ctx.r3.u64=command_skip?1:0;
}
void movie(PPCContext &ctx,uint8_t*,uint32_t address) {
 assert(address==0x1111 && ctx.r3.u32==0x2000 && ctx.r4.u32==1 && ctx.r5.u32==0x5000);
 assert(!ctx.r6.u64 && !ctx.r7.u64); ++movies; ctx.r3.u64=42;
}
#define REX_CALL_INDIRECT_FUNC_AT(site,ordinal,address) movie(ctx,base,address)
'''
startup=prefix+block(guest,'DEFINE_REX_FUNC(sub_822F7FA0)')+'\n'+block(hooks,'REX_HOOK_RAW(sub_822F7FA0)')+r'''
int main(int argc,char **argv) {
 assert(argc==3); me2_skip_intro_videos=std::atoi(argv[1]); command_skip=std::atoi(argv[2]);
 std::vector<uint8_t> memory(65536); auto *base=memory.data();
 write32(base,0x2000,0x3000); write32(base,0x3018,0x1111);
 write32(base,0x207c,1); write32(base,0x2078,0x4000);
 write32(base,0x4004,7); write32(base,0x4000,0x5000);
 auto before=memory;
 PPCContext ctx; ctx.r1.u64=0x8000; ctx.r3.u64=0x2000;
 sub_822F7FA0(ctx,base);
 assert(ctx.r1.u64==0x8000 && ctx.lr==0x1234);
 assert(std::memcmp(base+0x2000,before.data()+0x2000,0x4000)==0);
 if(me2_skip_intro_videos) {
  assert(!movies && !original_calls && ctx.r3.u64==1 && memory==before);
 } else if(command_skip) {
  assert(!movies && original_calls==1 && ctx.r3.u64==1);
 } else {
  assert(movies==1 && original_calls==1 && ctx.r3.u64==42);
 }
 std::cout << "PASS startup skip=" << me2_skip_intro_videos << " existing command-line skip=" << command_skip << '\n';
}
'''
pace_prefix=r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#define REXCVAR_GET(x) x
bool me2_unlock_framerate=false,sa2_renut_vblank_pacing=true,mgr_renut_vsync_relaxed=true;
struct Clock {
 using duration=std::chrono::nanoseconds;
 using time_point=std::chrono::time_point<Clock,duration>;
 static inline time_point current{};
 static time_point now() { return current; }
};
constexpr uint32_t kDeviceOffsetPresentationInterval=13596;
constexpr auto kRefresh=std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0/60.0));
Clock::time_point g_grid,g_due;
bool g_started=false;
uint64_t g_display_previous_ns=0;
using be_u32=uint32_t;
uint32_t interval=2;
unsigned reads=0,starts=0,releases=0,waits=0;
Clock::duration waited{};
namespace renut::gpu { template<class T> const T *GuestPtr(uint32_t address) {
 assert(address==0x1000+kDeviceOffsetPresentationInterval); ++reads; return &interval;
} }
namespace banjo::display_sync {
 void Start() { ++starts; }
 uint64_t NextRelease(uint64_t,uint64_t,uint64_t,bool*) { return 0; }
 void NoteRelease(uint64_t,bool) {}
}
void NoteRelease(Clock::time_point,Clock::time_point,bool) { ++releases; }
void SleepUntil(Clock::time_point t) { assert(t>=Clock::current); waited+=t-Clock::current; Clock::current=t; ++waits; }
'''
pace=pace_prefix+block(pacing,'uint32_t RefreshesPerFrame(')+'\n'+block(pacing,'void PaceSwap(')+r'''
int main(int argc,char **argv) {
 assert(argc==4); me2_unlock_framerate=std::atoi(argv[1]); sa2_renut_vblank_pacing=std::atoi(argv[2]);
 interval=std::strtoul(argv[3],nullptr,0);
 for(int i=0;i<5;++i) { PaceSwap(0x1000); Clock::current+=std::chrono::milliseconds(5); }
 if(me2_unlock_framerate || !sa2_renut_vblank_pacing) {
  assert(!reads && !starts && !releases && !waits && waited.count()==0);
 } else {
  assert(reads==5 && starts==5 && releases==5);
  auto refreshes=RefreshesPerFrame(interval);
  assert(waits==(refreshes?4u:0u));
  if(refreshes) assert(waited==kRefresh*refreshes*4-std::chrono::milliseconds(20));
 }
 std::cout << "PASS pacing unlock=" << me2_unlock_framerate << " low-level pacing=" << sa2_renut_vblank_pacing << " interval=" << interval << '\n';
}
'''
logs=[]
for name,text,cases in [
    ('startup',startup,[(str(a),str(b)) for a in range(2) for b in range(2)]),
    ('pacing',pace,[(str(a),str(b),str(c)) for a in range(2) for b in range(2) for c in [0,1,2,4,0x80000000]]),
]:
    path=out/(name+'.cpp'); path.write_text(text); exe=out/name
    subprocess.run([args.cxx,'-std=c++20','-O2','-fsanitize=undefined','-fno-sanitize-recover=all',str(path),'-o',str(exe)],check=True)
    for case in cases:
        result=subprocess.run([str(exe),*case],capture_output=True,text=True,check=True)
        logs.append(result.stdout)

# Default-off config and cvar registration agree.
config=tomllib.loads((root/'app/me2_deko_switch.toml').read_text())
for key in ['me2_unlock_framerate','me2_skip_intro_videos']:
    assert config[key] is False
    assert 'REXCVAR_DEFINE_BOOL('+key+', false,' in hooks
# The existing guest skip check really refers to nostartupmovies, not a global movie-disable flag.
image=(root/'out/default_image.bin').read_bytes()
expected='nostartupmovies\0'.encode('utf-16-be')
assert image[0xd8174:0xd8174+len(expected)].lower()==expected
# Reachability in the current tree and after classic/optimised post-codegen.
route_path=root/'tools/me2_option_hooks.py'
spec=importlib.util.spec_from_file_location('me2_option_hooks',route_path)
module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
with tempfile.TemporaryDirectory() as folder:
    path=Path(folder)/'sa2_recomp.1.cpp'
    original=b'__imp__sub_822F7FA0(ctx, base);\r\n__imp__sub_822FAF68(ctx, base);\r\n'
    path.write_bytes(original)
    assert sum(c['calls'] for c in module.route(folder))==1
    assert path.read_bytes()==original.replace(b'__imp__sub_822F7FA0',b'sub_822F7FA0')
    assert module.route(folder)==[]
assert 'sub_822F7FA0(ctx, base);' in (root/'app/generated/default/sa2_recomp.227.cpp').read_text()
assert '__imp__sub_822F7FA0(ctx, base);' not in (root/'app/generated/default/sa2_recomp.227.cpp').read_text()
assert "run(tool('me2_option_hooks.py')" in (root/'tools/post_codegen.py').read_text()
logs.append('PASS config defaults, guest startup string, direct hook routing and idempotent codegen repair\n')
text=''.join(logs)
(out.parent/'test.log').write_text(text)
print(text,end='')
