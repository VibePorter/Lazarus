"""Test actual input merging and the Bypass patch offline; never launches the game.

Requires a C++20 compiler and the decompressed SFXGame package already used by the project's script tools.
If the pre-change snapshots exist, also run them as negative controls.
"""
from pathlib import Path
import argparse
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--cxx', default='g++')
ap.add_argument('--package', type=Path, default=root / 'out/me2pkg/SFXGame.upk')
args = ap.parse_args()
out = root / 'out/fixes/input_release_20261029/tests'
out.mkdir(parents=True, exist_ok=True)

def extract(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 2]

def run(name, source, negative=False):
    path = out / (name + '.cpp')
    path.write_text(source)
    exe = out / name
    subprocess.run([args.cxx, '-std=c++20', '-O2', '-fsanitize=undefined', str(path), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)])
    assert result.returncode == (1 if negative else 0), name

input_prefix = r'''
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
using X_RESULT = int;
constexpr int X_ERROR_SUCCESS=0, X_ERROR_DEVICE_NOT_CONNECTED=1;
constexpr unsigned kMaxGuestUsers=4;
using DeviceId = unsigned;
struct X_INPUT_GAMEPAD { uint16_t buttons=0; uint8_t lt=0,rt=0; int16_t lx=0,ly=0,rx=0,ry=0; };
static_assert(sizeof(X_INPUT_GAMEPAD)==12);
struct X_INPUT_STATE { uint32_t packet_number=0; X_INPUT_GAMEPAD gamepad; };
using ReadHook = void(*)();
static std::atomic<ReadHook> g_read_hook{nullptr};
#define SCOPE_profile_cpu_f(x) ((void)0)
static int16_t larger(int16_t a,int16_t b) { return std::abs(int(a))>=std::abs(int(b))?a:b; }
static void MergeInto(X_INPUT_STATE &a,const X_INPUT_STATE &b) {
 a.gamepad.buttons|=b.gamepad.buttons; a.gamepad.lx=larger(a.gamepad.lx,b.gamepad.lx);
 a.gamepad.ly=larger(a.gamepad.ly,b.gamepad.ly); a.gamepad.rx=larger(a.gamepad.rx,b.gamepad.rx);
 a.gamepad.ry=larger(a.gamepad.ry,b.gamepad.ry); a.packet_number=std::max(a.packet_number,b.packet_number);
}
struct Driver {
 X_INPUT_STATE state; bool connected=true;
 int GetDeviceState(DeviceId,X_INPUT_STATE *out) { *out=state; return connected?0:1; }
};
struct Assignment {
 std::array<std::vector<DeviceId>,4> ids;
 void DevicesForUser(unsigned u,std::vector<DeviceId>&out) { out=ids[u]; }
};
struct Active { void Observe(unsigned,DeviceId,const X_INPUT_GAMEPAD&) {} };
struct InputSystem {
 std::recursive_mutex mutex_; Assignment assignment; Assignment *assignment_=&assignment;
 std::array<Driver,3> drivers; Active active_devices_;
 std::array<X_INPUT_GAMEPAD,4> last_gamepads_{};
 std::array<uint32_t,4> state_packets_{};
 std::array<bool,4> state_seen_{};
 void RefreshDevices() {}
 Driver *DriverForDevice(DeviceId id) { return id<drivers.size()?&drivers[id]:nullptr; }
 int GetState(uint32_t,X_INPUT_STATE*);
};
'''
input_main = r'''
int main() {
 unsigned failed=0; X_INPUT_STATE a,b,c;
 InputSystem s; s.assignment.ids[0]={1}; s.drivers[1].state={1,{}}; s.drivers[1].state.gamepad.lx=20000;
 s.GetState(0,&a);
 s.assignment.ids[0]={1,2}; s.drivers[1].state={2,{}}; s.drivers[2].state={500,{}};
 s.GetState(0,&b);
 if(a.packet_number==b.packet_number || b.gamepad.lx!=0) ++failed; // one -> many: release must be delivered
 s.drivers[1].state.gamepad.lx=20000; s.GetState(0,&a);
 s.assignment.ids[0]={1}; s.drivers[1].state={a.packet_number,{}}; s.GetState(0,&b);
 if(a.packet_number==b.packet_number || b.gamepad.lx!=0) ++failed; // many -> one, counters collide again
 s.drivers[1].state.packet_number=999; s.GetState(0,&c);
 if(c.packet_number!=b.packet_number) ++failed; // native counter alone cannot change the guest state
 s.drivers[1].connected=false; if(s.GetState(0,&a)!=1) ++failed;
 s.drivers[1].connected=true; s.GetState(0,&a); if(a.packet_number==b.packet_number) ++failed;
 s.assignment.ids[1]={2}; s.GetState(1,&b); if(b.packet_number!=1) ++failed;
 InputSystem other; other.assignment.ids[0]={1,2}; other.GetState(0,&b); if(b.packet_number!=1) ++failed;
 for(unsigned i=0;i<10000;++i) {
   s.assignment.ids[0]=(i&1)?std::vector<unsigned>{1}:std::vector<unsigned>{1,2};
   s.drivers[1].state.gamepad.lx=20000; s.GetState(0,&a);
   s.drivers[1].state.gamepad.lx=0; s.GetState(0,&b);
   if(a.packet_number==b.packet_number || b.gamepad.lx!=0) ++failed;
 }
 std::printf("packet release: %u failures (one/many transitions, native-counter collisions, reconnect, per-user/per-instance state, 10000 releases)\n",failed);
 return failed?1:0;
}
'''
current = (root / 'sdk/src/input/input_system.cpp').read_text()
run('packets_fixed', input_prefix + extract(current, 'X_RESULT InputSystem::GetState(') + input_main)
before = root / 'out/fixes/input_release_20261029/before/input_system.cpp'
if before.exists():
    run('packets_negative', input_prefix + extract(before.read_text(), 'X_RESULT InputSystem::GetState(') + input_main, True)

sys.path.insert(0, str(root / 'tools'))
import me2_package as mp
p = mp.Package(args.package.read_bytes())
ref = next(i for i in range(1, len(p.exports) + 1) if p.full_name(i) == 'BioSkillGame_Bypass_Handler.Update')
e = p.exports[ref - 1]
code = p.d[e['offset'] + 44:e['offset'] + e['size']]
bypass_prefix = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
namespace fmt { template<class... T> std::string format(const char *,T...) {return "";} }
#define REXLOG_WARN(...) ((void)0)
#define REXLOG_INFO(...) ((void)0)
static bool ready=false;
static constexpr uint32_t fn=0x11000, data=0x12000, handler=0x10000;
static uint32_t targetClass=0x13000;
static uint32_t Load32(const uint8_t *base,uint32_t at) {
 return uint32_t(base[at])<<24|uint32_t(base[at+1])<<16|uint32_t(base[at+2])<<8|base[at+3];
}
static void Store32(uint8_t *base,uint32_t at,uint32_t v) { for(unsigned k=0;k<4;++k)base[at+k]=v>>(24-k*8); }
static bool Readable(uint32_t at,uint32_t size) { return at>=0x10000 && uint64_t(at)+size<=0x20000; }
extern "C" bool me2_object_lookup_ready() {return ready;}
extern "C" uint32_t me2_object_function(const uint8_t *base,uint32_t object,const char*,const char*) {
 return ready && Load32(base,object+0x34)==targetClass?fn:0;
}
'''
bypass_main = r'''
int main() {
 std::vector<uint8_t> base(0x20000); const uint8_t script[]={SCRIPT};
 Store32(base.data(),handler+0x34,0x13000);
 std::memcpy(base.data()+data,script,sizeof(script)); Store32(base.data(),fn+0x54,data); Store32(base.data(),fn+0x58,sizeof(script));
 PatchBypassUpdate(base.data(),handler); // metadata not ready: this must NOT cache a permanent miss
 ready=true; PatchBypassUpdate(base.data(),handler);
 unsigned diff=0; for(unsigned i=0;i<sizeof(script);++i) diff+=script[i]!=base[data+i];
 const bool ok=Load32(base.data(),data+0x4e)==0xbf800000 && diff==2;
 PatchBypassUpdate(base.data(),handler);
 std::printf("Bypass actual package script: %s (early lookup retry, only the comparison constant changed, repeated events)\n",ok?"PASS":"FAIL");
 return ok?0:1;
}
'''.replace('SCRIPT', ','.join(str(b) for b in code))
current = (root / 'app/me2_src/me2_selection.cpp').read_text()
fixed_patch = extract(current, 'void PatchBypassUpdate(')
run('bypass_fixed', bypass_prefix + fixed_patch + bypass_main)
before = root / 'out/fixes/input_release_20261029/before/me2_selection.cpp'
if before.exists():
    run('bypass_negative', bypass_prefix + extract(before.read_text(), 'void PatchBypassUpdate(') + bypass_main, True)
reuse_main = bypass_main.replace(
    'PatchBypassUpdate(base.data(),handler); // metadata not ready: this must NOT cache a permanent miss\n ready=true;',
    'ready=true; targetClass=0x13100; PatchBypassUpdate(base.data(),handler);\n Store32(base.data(),handler+0x34,targetClass);')
run('bypass_handler_reuse_fixed', bypass_prefix + fixed_patch + reuse_main)
if before.exists():
    run('bypass_handler_reuse_negative', bypass_prefix + extract(before.read_text(), 'void PatchBypassUpdate(') + reuse_main, True)

lookup_prefix = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
struct Names { bool ok=true; } g_names;
struct Layout { bool ok=true; uint32_t outer=0x28,super=0x44; } g_layout;
static uint32_t Load32(const uint8_t *base,uint32_t at) {return uint32_t(base[at])<<24|uint32_t(base[at+1])<<16|uint32_t(base[at+2])<<8|base[at+3];}
static void Store32(uint8_t *base,uint32_t at,uint32_t v) {for(unsigned k=0;k<4;++k)base[at+k]=v>>(24-k*8);}
static bool HeapPointer(uint32_t p) {return p>=0x10000 && p<0x1ff00;}
static bool NameIs(const uint8_t*,uint32_t id,const char*) {return id==1;}
static uint32_t FieldIn(const uint8_t*,const Layout&,uint32_t cls,const char*) {return cls==0x13100?0x11000:0;}
'''
lookup_main = r'''
int main() {
 std::vector<uint8_t> base(0x20000); auto b=base.data();
 Store32(b,0x10034,0x13100); Store32(b,0x1312c,1);
 unsigned failed=me2_object_function(b,0x10000,"Bypass","Update")!=0x11000;
 Store32(b,0x10034,0x13000); Store32(b,0x1302c,2); Store32(b,0x13044,0x13100);
 failed+=me2_object_function(b,0x10000,"Bypass","Update")!=0x11000; // PC/derived handler inherits the same Update
 Store32(b,0x13044,0x13000); // unrelated cyclic metadata is bounded
 failed+=me2_object_function(b,0x10000,"Bypass","Update")!=0;
 std::printf("class lookup: %u failures (base, inherited handler, bounded unrelated hierarchy)\n",failed);
 return failed?1:0;
}
'''
current = (root / 'app/me2_src/me2_cerberus.cpp').read_text()
ready_fn = extract(current, 'extern "C" bool me2_object_lookup_ready(')
run('lookup_fixed', lookup_prefix + ready_fn + extract(current, 'extern "C" uint32_t me2_object_function(') + lookup_main)
before = root / 'out/fixes/input_release_20261029/before/me2_cerberus.cpp'
if before.exists():
    run('lookup_negative', lookup_prefix + ready_fn + extract(before.read_text(), 'extern "C" uint32_t me2_object_function(') + lookup_main, True)
print('PASS offline input-release regressions')
