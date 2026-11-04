"""Offline regression for Switch glyphs under the UI's flat color transforms and fades.

Run with the project's Pillow-enabled Python. No renderer or game process is started.
"""
from pathlib import Path
import argparse
import re
import subprocess
import sys

from PIL import Image

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools'))
import me2_button_icons as icons
import me2_icon_style as style

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--src', type=Path, default=root.parent / 'icons_src/White/512w')
ap.add_argument('--package', type=Path, default=root / 'out/me2pkg/Startup_INT.upk')
args = ap.parse_args()
out = root / 'out/fixes/icon_states_20261029'
out.mkdir(parents=True, exist_ok=True)

def transformed_contrast(image, tint, opacity):
    # Flat RGB color transform over a dark UI background: only the texture's alpha can preserve the glyph.
    alpha = image.getchannel('A').point(lambda a: round(a * opacity))
    flat = Image.new('RGBA', image.size, tint + (255,))
    flat.putalpha(alpha)
    background = Image.new('RGBA', image.size, (32, 32, 32, 255))
    result = Image.alpha_composite(background, flat).convert('L')
    w, h = result.size
    lo, hi = result.crop((round(w * .30), round(h * .30), round(w * .70), round(h * .70))).getextrema()
    return hi - lo

cases = 0
for game_letter in 'ABXY':
    for glow, size in ((False, 64), (True, 32)):
        image = icons.face(game_letter, str(args.src), glow, size)
        for tint in ((100, 180, 90), (130, 130, 130)):
            for opacity in (.15, .35, .60, 1.0):
                contrast = transformed_contrast(image, tint, opacity)
                assert contrast >= 5, (game_letter, glow, tint, opacity, contrast)
                cases += 1
        # The load-time replacement's ARGB bytes must carry the same alpha as the generated PNG.
        raw = icons.to_guest(image)
        assert raw[0::4] == image.getchannel('A').tobytes()

# Negative control: omit exactly the alpha cutout, reproducing the old generator's opaque lettering.
cutout = style.cutout_letters
try:
    style.cutout_letters = lambda image, letters: image
    for glow, size in ((False, 64), (True, 32)):
        old = icons.face('A', str(args.src), glow, size)
        assert transformed_contrast(old, (130, 130, 130), .35) < 5
finally:
    style.cutout_letters = cutout

# Check the generated table, not just a PNG preview. All mappings and match keys must stay intact.
generated = out / 'regenerated.inc'
subprocess.run([sys.executable, str(root / 'tools/me2_button_icons.py'), str(args.package), str(args.src),
                str(generated)], check=True, capture_output=True, text=True)
committed = (root / 'app/me2_src/me2_button_icons_data.inc').read_bytes()
assert generated.read_bytes() == committed, 'committed icon table is not the generator output'
text = committed.decode()
entries = re.findall(r'\{0x([0-9A-F]+)ull, (\d+), (\d+), kIcon(\d+), "([^"]+)"\}', text)
assert len(entries) == 29
for _, width, height, index, name in entries:
    match = re.search(r'static const uint8_t kIcon' + index + r'\[\d+\] = \{([^}]+)\};', text)
    raw = bytes(int(v) for v in re.findall(r'\d+', match[1]))
    assert len(raw) == int(width) * int(height) * 4, name
before = out / 'before/me2_button_icons_data.inc'
if before.exists():
    old_entries = re.findall(r'\{0x([0-9A-F]+)ull, (\d+), (\d+), kIcon(\d+), "([^"]+)"\}', before.read_text())
    assert entries == old_entries, 'texture hashes, mappings, dimensions or table order changed'
print(f'PASS: {cases} tinted/faded glyph states, ARGB alpha, old-generator negative control, deterministic table and 29 unchanged mappings')
