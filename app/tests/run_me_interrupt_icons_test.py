"""Validate Conversation vector edits against real assets and the production C++ patch.

Requires a host C++ compiler. Does not launch the game or build an NRO.
"""
from pathlib import Path
import argparse
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools'))
import me2_interrupt_icons as icons

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--cxx', default='clang++')
ap.add_argument('--package', type=Path, default=root / 'out/me2pkg/Startup_INT.upk')
args = ap.parse_args()
out = root / 'out/fixes/interrupt_icons_20261029/test'
out.mkdir(parents=True, exist_ok=True)
movie = icons.conversation(args.package)
table = root / 'app/me2_src/me2_interrupt_icons_data.inc'
assert table.read_text() == icons.generate(movie), 'stale generated vector table'
expected = bytearray(movie)
changed = set()
for at, old, new in icons.edits(movie):
    expected[at:at + len(old)] = new
    changed.update(range(at, at + len(old)))
    # Decode generated edges again: all contours must close and stay inside the unchanged bounds.
    _, _, before = icons.read_shape(movie, at - 6)
    _, _, after = icons.read_shape(expected, at - 6)
    xmin, xmax, ymin, ymax = icons.Bits(expected, at + 2).rect()
    assert before[0] == after[0], 'timing ring changed'
    for path in after:
        x, y = path['start']
        assert xmin <= x <= xmax and ymin <= y <= ymax
        for edge in path['edges']:
            if edge[0] == 'L':
                x, y = x + edge[1], y + edge[2]
            else:
                cx, cy = x + edge[1], y + edge[2]
                assert xmin <= cx <= xmax and ymin <= cy <= ymax
                x, y = x + edge[1] + edge[3], y + edge[2] + edge[4]
            assert xmin <= x <= xmax and ymin <= y <= ymax
        assert (x, y) == path['start'], 'open fill/stroke contour'
assert len(expected) == len(movie)
assert all(a == b for i, (a, b) in enumerate(zip(movie, expected)) if i not in changed)
assert bytes(expected) != movie  # old movie is the negative control
(out / 'original.gfx').write_bytes(movie)

source = (root / 'app/me2_src/me2_button_icons.cpp').read_text()
start = source.index('extern "C" bool Me2PatchConversationTriggers(')
end = source.index('\n// The movie\'s bytes', start)
function = source[start:end]
prefix = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
bool enabled = true;
#define REXCVAR_GET(...) enabled
#define REXLOG_INFO(...) ((void)0)
'''
prefix += table.read_text() + '\n' + function
prefix += r'''
int main(int argc, char** argv) {
  assert(argc == 3);
  std::ifstream in(argv[1], std::ios::binary);
  std::vector<uint8_t> original((std::istreambuf_iterator<char>(in)), {});
  assert(original.size() == 62130);
  auto m = original;
  enabled = false;
  assert(Me2PatchConversationTriggers(m.data(), m.size()) && m == original);
  enabled = true;
  uint8_t tiny = 0;
  assert(!Me2PatchConversationTriggers(&tiny, 0));
  assert(!Me2PatchConversationTriggers(m.data(), m.size() - 1) && m == original);
  m[0] ^= 1;
  auto corrupt = m;
  assert(!Me2PatchConversationTriggers(m.data(), m.size()) && m == corrupt);
  for (uint32_t offset : {kInterruptOffset0, kInterruptOffset1}) {
    m = original;
    m[offset + 10] ^= 1;
    corrupt = m;
    assert(!Me2PatchConversationTriggers(m.data(), m.size()) && m == corrupt);
  }
  m = original;
  assert(Me2PatchConversationTriggers(m.data(), m.size()) && m != original);
  auto fixed = m;
  assert(Me2PatchConversationTriggers(m.data(), m.size()) && m == fixed);
  // A partly patched movie can be safely completed, in either direction.
  std::memcpy(m.data() + kInterruptOffset0, kInterruptOld0, sizeof(kInterruptOld0));
  assert(Me2PatchConversationTriggers(m.data(), m.size()) && m == fixed);
  std::memcpy(m.data() + kInterruptOffset1, kInterruptOld1, sizeof(kInterruptOld1));
  assert(Me2PatchConversationTriggers(m.data(), m.size()) && m == fixed);
  std::ofstream output(argv[2], std::ios::binary);
  output.write(reinterpret_cast<const char*>(m.data()), m.size());
}
'''
(out / 'test.cpp').write_text(prefix)
subprocess.run([args.cxx, '-std=c++20', '-O1', '-g', '-fsanitize=undefined',
                str(out / 'test.cpp'), '-o', str(out / 'test')], check=True)
subprocess.run([str(out / 'test'), str(out / 'original.gfx'), str(out / 'actual.gfx')], check=True)
assert (out / 'actual.gfx').read_bytes() == expected, 'production C++ differs from generated edit'
router = (root / 'app/me2_src/me2_menu.cpp').read_text()
assert re.search(r'if \(size == 62130 && Readable\(data, size\)\).*?Me2PatchConversationTriggers\(base \+ data, size\)',
                 router, re.S), 'patch is not wired into the GFx loader'
print('PASS: real asset, deterministic vectors, closed contours, unchanged ring/movie layout, '
      'production C++ patch, toggle, bounds/header/corruption guards, atomicity, idempotence and partial reloads')
