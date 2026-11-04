"""Regression for the actual pipeline-key reuse predicate, without launching the game.

Tests polygon offset changes with all other inputs unchanged, plus every change-mask bit.
Runs the pre-fix predicate as a negative control. Requires a C++20 host compiler.
"""
from pathlib import Path
import argparse
import re
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx', default='clang++')
args = parser.parse_args()
out = root / 'out/fixes/flicker_20261029/reuse_test'
out.mkdir(parents=True, exist_ok=True)
draw = (root / 'app/src/renut_engine/gpu/render/draw.cpp').read_text()
header = (root / 'app/src/renut_engine/gpu/render/draw.h').read_text()
layout = (root / 'app/src/renut_engine/gpu/d3d/d3d.h').read_text()
point_offset = int(re.search(r'offsetof\(Device, m_PointPacket\) == (0x[0-9A-Fa-f]+)', layout)[1], 16)
point_size = int(re.search(r'm_PointPacket\[(\d+)\]', layout)[1])
start = header.index('struct ChainHint {')
end = header.index('\nstruct ClearRect {', start)
chain_code = header[start:end].replace('extern ChainHint g_chain_hint;', '')
start = draw.index('  bool reused = reuse_on && hint.seq')
end = draw.index(';', start)
predicate = draw[start:end].split('=', 1)[1].strip()
assert predicate.count('!PipelineKeyStateChanged(hint)') == 1
old_predicate = predicate.replace('!PipelineKeyStateChanged(hint)', '!ChainChanged(hint, 10112, 10599)')
prefix = r'''
#include <cstddef>
#include <cstdint>
#include <cstdio>
using u32 = uint32_t;
namespace d3d { struct Device { uint8_t pad[POINT_OFFSET]; uint8_t m_PointPacket[POINT_SIZE]; }; }
'''.replace('POINT_OFFSET', str(point_offset)).replace('POINT_SIZE', str(point_size))
prefix += chain_code
prefix += r'''
struct Shader { const void *entry = nullptr; };
struct Declaration { uint64_t layout_hash = 0; };
struct Geometry { bool per_instance = false; };
struct Shared { u32 alpha_test_func = 0; };
struct Targets { u32 color_count = 0; const void *depth = nullptr; };
struct Reuse {
  uint64_t seq = 10, layout_hash = 0, surface_epoch = 0;
  const void *vs_entry = nullptr, *ps_entry = nullptr, *depth = nullptr;
  u32 primitive = 0, alpha_test_func = 0, color_count = 0;
  bool per_instance = false; int ab_off = 0;
};
struct Call { u32 primitive = 0; };
__attribute__((noinline)) bool Reuses(const ChainHint &hint) {
  const bool reuse_on = true;
  const Reuse reuse;
  const Shader shader; const Shader *vs = &shader;
  const void *ps_entry = nullptr;
  const Declaration decl; const Declaration *declaration = &decl;
  const Geometry geometry;
  const Shared sc; const Targets targets; const Call call;
  const uint64_t surface_epoch = 0; const int ab_off = 0;
  return PREDICATE;
}
'''
main = r'''
int main() {
  ChainHint hint; hint.seq = 11;
  unsigned failures = 0;
  if (!Reuses(hint)) ++failures; // unchanged draws retain the fast path
  for (u32 bit = 0; bit < 128; ++bit) {
    hint.changed[0] = hint.changed[1] = 0;
    hint.changed[bit / 64] = uint64_t(1) << (bit % 64);
    bool relevant = bit >= ChainChunkOf(10112) && bit <= ChainChunkOf(10599);
    relevant |= bit >= ChainChunkOf(POINT_OFFSET) && bit <= ChainChunkOf(POINT_OFFSET + POINT_SIZE - 1);
    if (Reuses(hint) == relevant) ++failures;
  }
  // Four floats: front slope/bias and back slope/bias. These can change while the camera and control bits stay put.
  for (u32 byte = POINT_OFFSET; byte < POINT_OFFSET + 16; byte += 4) {
    hint.changed[0] = hint.changed[1] = 0;
    const u32 bit = ChainChunkOf(byte);
    hint.changed[bit / 64] = uint64_t(1) << (bit % 64);
    if (Reuses(hint)) ++failures;
  }
  hint.changed[0] = hint.changed[1] = 0;
  hint.seq = 12; if (Reuses(hint)) ++failures; // a gap is still a miss
  hint.seq = 0; if (Reuses(hint)) ++failures;
  std::printf("TAG: %u failures (128 change-mask bits, four polygon-offset floats, unchanged draw, sequence gaps)\n", failures);
  return failures ? 1 : 0;
}
'''.replace('POINT_OFFSET', str(point_offset)).replace('POINT_SIZE', str(point_size))
for tag, expr in [('fixed', predicate), ('negative', old_predicate)]:
    path = out / (tag + '.cpp')
    path.write_text(prefix.replace('PREDICATE', expr) + main.replace('TAG', tag))
    exe = out / tag
    subprocess.run([args.cxx, '-std=c++20', '-O2', str(path), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)])
    assert result.returncode == (0 if tag == 'fixed' else 1)
print('PASS: actual production predicate; pre-fix negative control reproduces stale polygon-offset reuse')
