"""Offline regression for the production GNames lookup across package loads.

Compiles the actual lookup helpers; never runs the game or builds an NRO.
"""
from pathlib import Path
import argparse
import subprocess

root = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--cxx', default='clang++')
args = ap.parse_args()
out = root / 'out/fixes/followup_20261029/tests'
out.mkdir(parents=True, exist_ok=True)

def extract(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 2]

prefix = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
struct Names { uint32_t data=0,num=0,str_off=0,array=0; bool ok=false; } g_names;
static bool Readable(uint32_t p,uint32_t n) {return p>=0x10000 && uint64_t(p)+n<=0x20000;}
static bool HeapPointer(uint32_t p) {return Readable(p,32);}
static uint32_t Load32(const uint8_t* b,uint32_t p) {
  return uint32_t(b[p])<<24|uint32_t(b[p+1])<<16|uint32_t(b[p+2])<<8|b[p+3];
}
static void Store32(uint8_t* b,uint32_t p,uint32_t v) {
  for(unsigned k=0;k<4;++k)b[p+k]=v>>(24-k*8);
}
'''
main = r'''
int main() {
  std::vector<uint8_t> mem(0x20000); auto b=mem.data(); unsigned failed=0;
  g_names={0x12000,2,8,0x11000,true};
  Store32(b,0x11000,0x12000); Store32(b,0x11004,2); Store32(b,0x11008,4);
  Store32(b,0x12000,0x16000); Store32(b,0x12004,0x16100);
  std::strcpy(reinterpret_cast<char*>(b+0x16008),"InitCerberus");
  std::strcpy(reinterpret_cast<char*>(b+0x16108),"Update");
  std::strcpy(reinterpret_cast<char*>(b+0x16208),"BioSkillGame_Bypass_Handler");
  failed+=!NameIs(b,0,"InitCerberus"); failed+=NameIndex(b,"Update")!=1;
  Store32(b,0x12008,0x16200); Store32(b,0x11004,3); // growth within capacity
  failed+=!NameIs(b,2,"BioSkillGame_Bypass_Handler");
  failed+=NameIndex(b,"BioSkillGame_Bypass_Handler")!=2;
  std::memcpy(b+0x14000,b+0x12000,12); Store32(b,0x11000,0x14000);
  std::memset(b+0x12000,0,16); // backing allocation moved; old pointers no longer valid
  failed+=!NameIs(b,0,"InitCerberus"); failed+=NameIndex(b,"Update")!=1;
  Store32(b,0x11004,5); // invalid count/capacity must fail closed
  failed+=NameIs(b,0,"InitCerberus"); failed+=NameIndex(b,"Update")!=-1;
  Store32(b,0x11004,3); Store32(b,0x11000,0xfffffff0);
  failed+=NameIs(b,0,"InitCerberus"); failed+=NameIndex(b,"Update")!=-1;
  Store32(b,0x11000,0x14001); failed+=NameIs(b,0,"InitCerberus");
  Store32(b,0x11000,0x14000);
  failed+=!NameIs(b,2,"BioSkillGame_Bypass_Handler"); // recover after transient bad header
  failed+=NameIs(b,3,"missing");
  g_names.ok=false; failed+=NameIs(b,0,"InitCerberus"); failed+=NameIndex(b,"Update")!=-1;
  std::printf("GNames growth, relocation, invalid headers, recovery: %u failures\n",failed);
  return failed?1:0;
}
'''
current = (root / 'app/me2_src/me2_cerberus.cpp').read_text()
before = root / 'out/fixes/followup_20261029/before/app/me2_src/me2_cerberus.cpp'
for name, text, negative in [('fixed', current, False)] + ([('negative', before.read_text(), True)] if before.exists() else []):
    helpers = extract(text, 'bool RefreshNames(') if 'bool RefreshNames(' in text else ''
    helpers += extract(text, 'int32_t NameIndex(') + extract(text, 'bool NameIs(')
    source = out / (name + '.cpp')
    source.write_text(prefix + helpers + main)
    exe = out / name
    subprocess.run([args.cxx, '-std=c++20', '-O2', '-fsanitize=undefined,address', str(source), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)])
    assert result.returncode == (1 if negative else 0), name
print('PASS: production lookup and failing pre-change control')
