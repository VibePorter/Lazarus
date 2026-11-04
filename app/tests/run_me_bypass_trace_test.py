"""Exercise the actual Bypass boundary diagnostic without launching the game."""
from pathlib import Path
import argparse, subprocess
root = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--cxx', default='clang++')
a = ap.parse_args()
out = root / 'out/fixes/bypass_drift_20261029/tests'
out.mkdir(parents=True, exist_ok=True)
s = (root / 'app/me2_src/me2_selection.cpp').read_text()
start = s.index('REX_HOOK_RAW(sub_8273BE00)')
hook = s[start:s.index('\n}', start) + 2]
prefix = r'''
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#define REX_HOOK_RAW(n) void n(Context& ctx,uint8_t* base)
#define REXCVAR_GET(n) enabled
#define REXLOG_INFO(...) (++logs)
#define REXLOG_WARN(...) (++logs)
static bool enabled=true;
static unsigned logs=0,calls=0,reads=0;
struct Reg { uint32_t u32=0; };
struct Context { Reg r1,r3,r4; };
std::atomic<uint64_t> g_sa2_game_frames{1};
static bool Readable(uint32_t at,uint32_t n) {++reads;return at>=0x10000 && uint64_t(at)+n<=0x20000;}
static uint32_t Load32(const uint8_t* b,uint32_t at) {return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];}
static void Store32(uint8_t* b,uint32_t at,uint32_t v) {for(unsigned k=0;k<4;++k)b[at+k]=v>>(24-8*k);}
static float LoadF(const uint8_t* b,uint32_t at) {uint32_t v=Load32(b,at);float f;std::memcpy(&f,&v,4);return f;}
static void StoreF(uint8_t* b,uint32_t at,float f) {uint32_t v;std::memcpy(&v,&f,4);Store32(b,at,v);}
void __imp__sub_8273BE00(Context& c,uint8_t* b) {
 ++calls; const uint32_t input=Load32(b,c.r4.u32),output=0x16000;
 Store32(b,c.r3.u32,output);Store32(b,c.r3.u32+4,5);
 for(unsigned i=0;i<5;++i){double f=LoadF(b,input+i*28+8);uint64_t v;std::memcpy(&v,&f,8);
  Store32(b,output+i*12,3);Store32(b,output+i*12+4,uint32_t(v>>32));Store32(b,output+i*12+8,uint32_t(v));}
 c.r3.u32=0x1234; // original call's volatile register effect must be retained
}
'''
main = r'''
int main(int argc,char**) {
 enabled=argc==1;std::vector<uint8_t> mem(0x20000);auto b=mem.data();
 Context c;c.r1.u32=0x10100;c.r4.u32=0x13000;
 const char name[]="SetCursorPositions";
 for(unsigned i=0;i<sizeof(name);++i)b[0x12000+2*i+1]=name[i];
 Store32(b,c.r1.u32+96,0x12000);Store32(b,c.r1.u32+100,sizeof(name));
 Store32(b,0x13000,0x14000);Store32(b,0x13004,5);
 for(unsigned i=0;i<5;++i)b[0x14000+i*28]=1;
 StoreF(b,0x14008,0.8f);StoreF(b,0x14000+28+8,-0.9f);StoreF(b,0x14000+4*28+8,1.0f/30);
 unsigned failed=0;
 auto invoke=[&]{c.r3.u32=0x15000;sub_8273BE00(c,b);failed+=c.r3.u32!=0x1234;};
 invoke();failed+=logs!=(enabled?1u:0u);invoke();failed+=logs!=(enabled?1u:0u);
 StoreF(b,0x14008,0);StoreF(b,0x14000+28+8,0);++g_sa2_game_frames;
 invoke();failed+=logs!=(enabled?2u:0u);
 g_sa2_game_frames+=30;invoke();failed+=logs!=(enabled?3u:0u);
 b[0x12001]='X';invoke();failed+=logs!=(enabled?3u:0u); // other methods are ignored
 failed+=calls!=5;if(!enabled)failed+=reads!=0;
 std::printf("trace %s: %u failures (release, repeated zero, unrelated method, original context, off-path reads)\n",enabled?"on":"off",failed);
 return failed?1:0;
}
'''
path = out / 'trace.cpp'
path.write_text(prefix + hook + main)
exe = out / 'trace'
subprocess.run([a.cxx, '-std=c++20', '-O2', '-fsanitize=address,undefined', str(path), '-o', str(exe)], check=True)
for args in [[], ['--off']]:
    subprocess.run([str(exe), *args], check=True)
print('PASS actual production boundary hook')
