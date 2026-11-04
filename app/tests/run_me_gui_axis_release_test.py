"""Test the production axis hook against ME2's verified GUI forwarding rule, offline."""
from pathlib import Path
import argparse, contextlib, io, subprocess, sys
root = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--cxx', default='clang++')
a = ap.parse_args()
out = root / 'out/fixes/gui_vm_release_20261029/tests'
out.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(root / 'tools'))
import me2_package as mp
from me2_ue3_disasm import disasm
p = mp.Package((root / 'out/me2pkg/SFXGame.upk').read_bytes())
ref = next(i for i in range(1, len(p.exports) + 1) if p.full_name(i) == 'BioPlayerInput.PreProcessInput')
capture = io.StringIO()
with contextlib.redirect_stdout(capture):
    disasm(p, ref)
script = capture.getvalue()
assert '??' not in script
for first in [0, 2]:
    assert f'native132(native181(self.AxisBuffer[{first}], 0.0), native181(self.AxisBuffer[{first+1}], 0.0))' in script
assert 'if (!(native178(StickMag, self.GuiDeadzone)))' in script
assert 'self.AxisBuffer[idx] = 0.0' in script
(out / 'PreProcessInput.txt').write_text(script)

parts = __import__('json').loads((root/'app/generated/default/codegen.partition.json').read_text())['assignments']
def native(address):
    data = (root/f'app/generated/default/sa2_recomp.{parts[address]}.cpp').read_text()
    start = data.index(f'DEFINE_REX_FUNC(sub_{address}) {{')
    return data[start:data.index('\n}',start)+2]
command = native('829B7CB0')
assert '// lfs f0,0(r29)' in command
for offset in [364,368,372,376,380,384]: assert f'// stfs f0,{offset}(r31)' in command
assert command.index('// bctrl') < command.index('// lfs f0,0(r29)') < command.index('// stfs f0,364(r31)')
call_function = native('82781A90')
assert '// stw r20,84(r31)' in call_function  # new FFrame.Node at frame +4
assert 'sub_82781F70(ctx, base);' in call_function
binder = native('82764208')
assert '// lis r11,-32136' in binder and '// addi r10,r11,9160' in binder
assert ((-32136 & 65535)<<16)+9160 == 0x82781F70
assert '// stw r10,172(r26)' in binder
engine = mp.Package((root/'out/me2pkg/Engine.upk').read_bytes())
ref = next(i for i in range(1,len(engine.exports)+1) if engine.full_name(i)=='PlayerInput.PlayerInput')
capture = io.StringIO()
with contextlib.redirect_stdout(capture): disasm(engine,ref)
assert 'PreProcessInput(DeltaTime)' in capture.getvalue()
(out/'Engine.PlayerInput.PlayerInput.txt').write_text(capture.getvalue())

source = (root / 'app/me2_src/me2_selection.cpp').read_text()
assert 'REXCVAR_DEFINE_BOOL(me2_bypass_cursor_stop, false' in source
start = source.index('std::atomic<uint32_t> g_gui_preprocess_fn')
helpers = source[start:source.index('} // namespace', start)]
start = source.index('REX_HOOK_RAW(sub_82781F70)')
hook = source[start:source.index('\n}', start) + 2]
start = source.index('REX_HOOK_RAW(sub_825E9DA8)')
hook += '\n' + source[start:source.index('\n}',start)+2]
prefix = r'''
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include "me2_gui_axis_release.h"
struct Register { uint32_t u32=0; uint64_t u64=0; double f64=0; };
struct Context { Register r3,r4,r11,f1; };
#define REX_HOOK_RAW(n) void n(Context& ctx,uint8_t* base)
#define REXCVAR_GET(n) n
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
bool me2_gui_axis_release=true,me2_input_axis_trace=false;
std::atomic<uint64_t> g_sa2_game_frames{0};
static bool ready=true;static unsigned lookups=0;
static bool Readable(uint32_t p,uint32_t n){return p>=0x10000 && uint64_t(p)+n<=0x40000;}
static uint32_t Load32(const uint8_t* b,uint32_t at){return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];}
static void Store32(uint8_t* b,uint32_t at,uint32_t v){for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);}
static float LoadF(const uint8_t* b,uint32_t at){uint32_t v=Load32(b,at);float f;std::memcpy(&f,&v,4);return f;}
extern "C" bool me2_object_lookup_ready(){return ready;}
extern "C" uint32_t me2_object_float_property(const uint8_t* b,uint32_t obj,const char*,const char* name,uint32_t count){
 ++lookups;if(Load32(b,obj+0x34)!=0x30000)return 0;
 return !std::strcmp(name,"AxisBuffer") && count==6 ? 0x300 : !std::strcmp(name,"GuiDeadzone") && count==1 ? 0x320 : 0;
}
extern "C" uint32_t me2_object_function(const uint8_t*,uint32_t,const char*,const char*){return ready?0x31000:0;}
void __imp__sub_825E9DA8(Context& c,uint8_t* b){float f=LoadF(b,c.r4.u32)+float(c.f1.f64);uint32_t v;std::memcpy(&v,&f,4);Store32(b,c.r4.u32,v);}
static void (*run_script)(Context&,uint8_t*)=nullptr;
static unsigned original_calls=0;
void __imp__sub_82781F70(Context& c,uint8_t* b){++original_calls;if(run_script)run_script(c,b);c.r11.u32=0x12345678;}
'''
main = r'''
static unsigned events[4][2]{};
static float last[4][2][2]{};
static bool consume=true;
// Matches the PreProcessInput gate/deadzone/reset verified against the actual package above.
static void Script(Context& c,uint8_t* b){
 if(!consume)return;
 unsigned p=(c.r3.u32-0x10000)/0x1000;
 for(unsigned pair=0;pair<2;++pair){uint32_t addr=c.r3.u32+0x300+pair*8;
  float x=LoadF(b,addr),y=LoadF(b,addr+4),zone=LoadF(b,c.r3.u32+0x320);
  if(x!=0 || y!=0){if(std::sqrt(x*x+y*y)<zone)x=y=0;
   last[p][pair][0]=x;last[p][pair][1]=y;++events[p][pair];}}
 for(unsigned i=0;i<6;++i)Store32(b,c.r3.u32+0x300+i*4,0);
}
int main(int argc,char**) {
 me2_gui_axis_release=argc==1;
 std::vector<uint8_t> memory(0x40000);auto b=memory.data();unsigned failed=0;
 run_script=Script;Store32(b,0x310AC,0);
 for(unsigned i=0;i<4;++i){Store32(b,0x10000+i*0x1000+0x34,0x30000);StoreF(b,0x10000+i*0x1000+0x320,0.4f);}
 // Native Axis EventID=2..7: accumulate aGui* FIRST, then copy into AxisBuffer.
 // This is the missing step in the previous regression test (sub_829B7CB0 PPC).
 auto add=[&](unsigned p,unsigned axis,float delta){uint32_t obj=0x10000+p*0x1000;
  Context c;c.r3.u32=obj;c.r4.u32=obj+0x200+axis*4;c.f1.f64=delta;sub_825E9DA8(c,b);
  float result=LoadF(b,obj+0x200+axis*4);StoreF(b,obj+0x300+axis*4,result);};
 auto begin=[&](unsigned p){for(unsigned i=0;i<6;++i)StoreF(b,0x10200+p*0x1000+i*4,0);};
 auto process=[&](unsigned p){Context c;c.r3.u32=0x10000+p*0x1000;c.r4.u32=0x38000+p*0x100;Store32(b,c.r4.u32+4,0x31000);c.f1.f64=1.0/30.0;
  unsigned before=original_calls;sub_82781F70(c,b);failed+=original_calls!=before+1;
  failed+=c.r11.u32!=0x12345678 || c.r4.u32!=0x38000+p*0x100 || c.f1.f64!=1.0/30.0;++g_sa2_game_frames;};
 auto tick=[&](unsigned p,float x,float y,bool reverse=false,unsigned pair=0){begin(p);
  if(reverse){add(p,pair*2+1,y);add(p,pair*2,x);}else{add(p,pair*2,x);add(p,pair*2+1,y);}process(p);};
 auto zero=[&](unsigned p,unsigned pair=0){return last[p][pair][0]==0 && last[p][pair][1]==0;};
 tick(0,0,0);failed+=g_gui_preprocess_fn.load()!=0;
 Store32(b,0x310AC,0x82781F70); // deferred function binding becomes ready later
 for(unsigned i=0;i<600;++i)tick(0,0,0);failed+=events[0][0]!=0 || events[0][1]!=0;
 failed+=me2_gui_axis_release && g_gui_preprocess_fn.load()!=0x31000;
 for(unsigned i=0;i<300;++i)tick(0,0.8f,0.1f);failed+=last[0][0][0]!=0.8f || last[0][0][1]!=0.1f;
 tick(0,0,0);
 if(!me2_gui_axis_release){failed+=last[0][0][0]!=0.8f || last[0][0][1]!=0.1f;failed+=lookups!=0;
  std::printf("off: native copy path reproduces lost neutral, %u failures\n",failed);return failed?1:0;}
 failed+=!zero(0);unsigned before=events[0][0];
 for(unsigned i=0;i<600;++i)tick(0,0,0);failed+=events[0][0]!=before;
 tick(0,0.8f,0.2f);before=events[0][0];
 begin(0);for(unsigned i=0;i<5;++i){add(0,0,0);add(0,1,0);}process(0);
 failed+=!zero(0) || events[0][0]!=before+1;
 // Zero polling followed by real input before the script: no false release/marker contamination.
 tick(0,0.8f,0.2f);begin(0);add(0,0,0);add(0,1,0);add(0,0,0.6f);add(0,1,-0.2f);process(0);
 failed+=last[0][0][0]!=0.6f || last[0][0][1]!=-0.2f;
 tick(1,0.7f,0.5f,true);tick(1,0,0.5f,true);failed+=last[1][0][0]!=0 || last[1][0][1]!=0.5f;
 tick(1,0,0,true);failed+=!zero(1);
 tick(2,-0.5f,0.2f,false,1);tick(2,0,0,false,1);failed+=!zero(2,1);
 for(float zone:{0.1f,0.2f,0.4f,0.5f,0.00000001f,0.000000000000000000000000000001f}){
  StoreF(b,0x10320,zone);tick(0,0.8f,0.1f);tick(0,0,0);failed+=!zero(0);}
 unsigned count=events[0][0];for(unsigned i=0;i<1000;++i)tick(0,0,0);failed+=events[0][0]!=count;
 // Native GUI/movement fields are untouched. Only the dedicated final GUI buffer gets the marker.
 tick(0,0.8f,0.2f);begin(0);add(0,0,0);add(0,1,0);StoreF(b,0x10280,0.75f);process(0);
 failed+=LoadF(b,0x10200)!=0 || LoadF(b,0x10204)!=0 || LoadF(b,0x10280)!=0.75f;
 // Inactive script retains the queued marker; re-entry must not manufacture another active transition.
 tick(0,0.8f,0.2f);begin(0);add(0,0,0);add(0,1,0);consume=false;process(0);
 float marker=LoadF(b,0x10300);failed+=!(marker>0 && marker<LoadF(b,0x10320));
 process(0);failed+=LoadF(b,0x10300)!=marker;consume=true;process(0);failed+=!zero(0);
 before=events[0][0];tick(0,0,0);failed+=events[0][0]!=before;
 ready=false;tick(3,0.8f,0);tick(3,0,0);failed+=last[3][0][0]!=0.8f;
 ready=true;tick(3,0.8f,0);tick(3,0,0);failed+=!zero(3);
 // Unrelated name lookups bypass the fix and its metadata walk.
 Context unrelated;unrelated.r3.u32=0x10000;unrelated.r4.u32=0x39000;Store32(b,0x39004,0x32000);unsigned saved=lookups;
 run_script=nullptr;sub_82781F70(unrelated,b);failed+=lookups!=saved;
 std::printf("production VM hook: %u failures (native GUI copy, release, holds, idle, reverse order, repeated polls, new input, players, both sticks, dead zones, unrelated fields, inactive script, metadata retry)\n",failed);
 return failed?1:0;
}
'''

path = out / 'release.cpp'
path.write_text(prefix + helpers + hook + main)
exe = out / 'release'
subprocess.run([a.cxx, '-std=c++20', '-O2', '-fsanitize=address,undefined', '-I', str(root/'app/me2_src'), str(path), '-o', str(exe)], check=True)
for args in [[], ['--off']]:
    subprocess.run([str(exe), *args], check=True)
metadata = (root / 'app/me2_src/me2_cerberus.cpp').read_text()
start = metadata.index('extern "C" uint32_t me2_object_float_property(')
function = metadata[start:metadata.index('\n}', start) + 2]
meta_prefix = prefix[:prefix.index('extern "C" uint32_t me2_object_float_property')]
meta_prefix += r'''
static bool HeapPointer(uint32_t p){return Readable(p,32);}
static bool NameIs(const uint8_t*,uint32_t name,const char*){return name==1;}
extern "C" uint32_t me2_object_function(const uint8_t*,uint32_t,const char*,const char*){return ready?0x31000:0;}
'''
meta_main = r'''
int main(){std::vector<uint8_t> mem(0x40000);auto b=mem.data();unsigned failed=0;
 Store32(b,0x31034,0x33000);Store32(b,0x3302c,1);Store32(b,0x31044,6);Store32(b,0x31048,4);Store32(b,0x31068,0x300);
 auto get=[&]{return me2_object_float_property(b,0x10000,"BioPlayerInput","AxisBuffer",6);};
 failed+=get()!=0x300;Store32(b,0x31044,1);failed+=get()!=0;Store32(b,0x31044,6);
 Store32(b,0x31048,8);failed+=get()!=0;Store32(b,0x31048,4);
 Store32(b,0x3302c,2);failed+=get()!=0;Store32(b,0x3302c,1);
 Store32(b,0x31068,0x301);failed+=get()!=0;Store32(b,0x31068,0x10000);failed+=get()!=0;
 Store32(b,0x31068,0x300);ready=false;failed+=get()!=0;ready=true;failed+=get()!=0x300;
 std::printf("production float-property validation: %u failures\n",failed);return failed?1:0;}
'''
path = out / 'metadata.cpp'
path.write_text(meta_prefix + function + meta_main)
exe = out / 'metadata'
subprocess.run([a.cxx, '-std=c++20', '-O2', '-fsanitize=address,undefined', '-I', str(root/'app/me2_src'), str(path), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
print('PASS actual VM hooks, native GUI copy, CallFunction frame and binding, package script chain, metadata, and disabled regression')
