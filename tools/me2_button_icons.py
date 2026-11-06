"""The Switch button icons: replacement table for the game's own (app/me2_src/me2_button_icons_data.inc).

  python tools/me2_button_icons.py <Startup_INT.upk> <icons src dir (White/512w)> <out .inc> [more .upk ...]
  (2026-11-01: plus out/me2pkg/BioD_Nor_103aGalaxyMap.upk out/me2pkg/BioD_Nor_103bGalaxyMapObjs.upk for the galaxy
  map's ZL / ZR)

For every button icon of the game (Startup_INT: GUI_GlobalIcons.Buttons.XBox_Btn_*, 64x64, and the glowing face
buttons GUI_SF_SharedAssets.mainController_I*, 32x32; all A8R8G8B8, tiled) it writes the FNV-1a 64 hash of the texture's
bytes as the guest holds them, untiled (row by row, 4 bytes a texel in memory order A, R, G, B) and the replacement in the
same layout. Renut's ReadLevel (render/textures.cpp, g_texture_replace) hashes a small 32-bit texture the same way and
takes the replacement when it matches. The icons come from tools/me2_icon_style.py (the user's flat Switch glyphs in
the style of the game's icons).

Mapping, with ME2's input on the Switch (A and B by letter, X and Y by position): the game's X is the left button (Y on
a Switch pad), its Y the top one (X). Face buttons in Nintendo's (SNES) colours by letter: A red, B yellow, X blue,
Y green, taken from the game's own icon colours.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import me2_package as mp  # noqa: E402
import me2_texture as mt  # noqa: E402
import me2_icon_style as st  # noqa: E402
from PIL import Image  # noqa: E402

GAME_COLOURS = {'A': (93, 136, 59), 'B': (188, 68, 68), 'X': (53, 99, 123), 'Y': (127, 107, 47)}
# Nintendo's colours by letter, from the game's: A red, B yellow, X blue, Y green.
LETTER_COLOURS = {'A': GAME_COLOURS['B'], 'B': GAME_COLOURS['Y'], 'X': GAME_COLOURS['X'], 'Y': GAME_COLOURS['A']}
# game icon -> Switch glyph (file in the icon set)
GLOBAL = {'A': 'A', 'B': 'B', 'X': 'Y', 'Y': 'X', 'LB': 'L', 'RB': 'R', 'LT': 'ZL', 'RT': 'ZR', 'Back': 'Minus',
          'Start': 'Plus', 'LS': 'Left Stick', 'RS': 'Right Stick', 'L3': 'Left Stick Click',
          'R3': 'Right Stick Click', 'DPadU': 'JoyCon D-Pad Up', 'DPadD': 'JoyCon D-Pad Down',
          'DPadL': 'JoyCon D-Pad Left', 'DPadR': 'JoyCon D-Pad Right'}
# the glowing face buttons: which game button each one is (from their colours)
GLOW = {'mainController_I6': 'A', 'mainController_I10': 'B', 'mainController_IC': 'X', 'mainController_I14': 'Y'}
# 2026-11-03 (user: the B of the controller diagram's ABXY sits 1 px too far right; 2026-11-04: the whole icon, not
# just the letter): the glowing 32x32 face button moved as a whole, in output pixels, by Switch letter. Every glowing B
# uses it (the HUD's, the power wheel's, the death screen's and the mini-games' B are the same picture).
GLOW_ICON_DX = {'B': -1}
# 2026-10-27 (user): the controller diagram (GUI_SF_SharedAssets.mainController) goes by position: its bottom button
# shows the Switch's B, the right one the Switch's A. The HUD's and the power wheel's face buttons (GUI_SF_HUD, the
# in-game prompts) are the same textures byte for byte, and the replacement only sees the texels, so every glowing face
# button is replaced by letter like XBox_Btn_*; the diagram swaps its two pictures (and its ControlA / ControlB names,
# so each label stays with its letter) at run time instead (app/me2_src/me2_button_icons.cpp).
HUD = {'ME2_HUD_I89': 'A', 'ME2_PowerWheel_I105': 'A', 'ME2_PowerWheel_I101': 'B', 'ME2_PowerWheel_IF6': 'Y',
       'ME2_PowerWheel_IF9': 'X'}
# 2026-10-28 (user): the death screen (GUI_SF_GameOver) and the two hacking minigames (GUI_SF_SkillGame Bypass /
# Decryption) have glowing face buttons of their own, 32x32 like the HUD's (found in Startup_INT by their pictures).
# 2026-10-29 (user): on the death screen A is on the right and B at the bottom, as on the Switch. GameOver_I8 / _IA are
# the same texels as the hacking mini-games' A / B (MiniGameBypass_I185 / _I180, MiniGameDecryption_IA5 / _IA0), so
# they stay by letter here (swapping them here swapped the hack's buttons too); the death screen's movie swaps its two
# pictures and texts instead (app/me2_src/me2_menu.cpp, me2_gameover_swap_ab).
SCREENS = {'GUI_SF_GameOver.GameOver_I8': 'A', 'GUI_SF_GameOver.GameOver_I9': 'X', 'GUI_SF_GameOver.GameOver_IA': 'B',
           'GUI_SF_SkillGame.MiniGameBypass_I185': 'A', 'GUI_SF_SkillGame.MiniGameBypass_I180': 'B',
           'GUI_SF_SkillGame.MiniGameDecryption_IA5': 'A', 'GUI_SF_SkillGame.MiniGameDecryption_IA0': 'B'}
# 2026-10-29 (user): the power wheel's shoulder buttons and D-pad hints (GUI_SF_HUD, green glow like the HUD's faces)
# still showed the Xbox LB / RB and the Xbox D-pad: L / R (same shapes as the Switch's) and the Joy-Con's four buttons
# with the hinted direction lit (IF0: right, IF3: left), in the game's green.
WHEEL = {'ME2_PowerWheel_IEA': 'RB', 'ME2_PowerWheel_IED': 'LB', 'ME2_PowerWheel_IF0': 'DPadR',
         'ME2_PowerWheel_IF3': 'DPadL'}
HUD_GREEN = tuple(min(255, int(v * 1.25)) for v in GAME_COLOURS['A'])
# 2026-11-01 (user): the galaxy map's probe-launch legend (GUI_SF_GalaxyMap, 64x64, green like the power wheel's) still
# showed the Xbox LT / RT: ZL / ZR in the wheel's green style, centred on the original tiles (3 px up). These two live in
# the galaxy map's own packages (BioD_Nor_103aGalaxyMap: _ID5 LT, BioD_Nor_103bGalaxyMapObjs: _IDB RT), given as extra
# package arguments.
GALAXY = {'GUI_SF_GalaxyMap.GalaxyMap_ID5': 'LT', 'GUI_SF_GalaxyMap.GalaxyMap_IDB': 'RT',
          # 2026-11-01 (user: the squad screen still showed the 360's RT): the squad record's own green LT / RT
          # (Startup_INT: GUI_SF_CharacterRecord), the same style as the galaxy map's.
          'GUI_SF_CharacterRecord.CharacterRecord_IFF': 'LT', 'GUI_SF_CharacterRecord.CharacterRecord_IFC': 'RT'}


def lower_shoulder_icon(img):
    """Lower L/R/ZL/ZR by 3 px at 32x32 (6 px at 64x64), without wrapping.

    Only raster prompts use this; Conversation's Paragon/Renegade vector artwork
    is generated separately by me2_interrupt_icons.py and keeps its placement.
    """
    out = Image.new('RGBA', img.size, (0, 0, 0, 0))
    out.paste(img, (0, img.height * 3 // 32))
    return out


def wheel_icon(key, src, size):
    if key in ('LB', 'RB'):
        return lower_shoulder_icon(st.style_glow(Image.open(os.path.join(src, GLOBAL[key] + '.png')),
                                                size, HUD_GREEN, margin=0.07, bold=9))
    # the whole Joy-Con D-pad glowing, the arms other than the hinted one dimmed (their sector of the square)
    img = st.style_glow(Image.open(os.path.join(src, 'JoyCon D-Pad.png')), size, HUD_GREEN)
    right = key == 'DPadR'
    px = img.load()
    c = (size - 1) / 2
    for y in range(size):
        for x in range(size):
            dx = (x - c) if right else (c - x)
            if dx > abs(y - c):
                continue
            r, g, b, a = px[x, y]
            px[x, y] = (int(r * 0.7), int(g * 0.7), int(b * 0.7), int(a * 0.75))
    return img


def galaxy_icon(key, src, size):
    img = st.style_glow(Image.open(os.path.join(src, GLOBAL[key] + '.png')), size, HUD_GREEN, margin=0.07, bold=9)
    out = Image.new('RGBA', img.size, (0, 0, 0, 0))
    out.paste(img, (0, -img.height * 3 // 64))
    return out


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def guest_linear(t):
    m, data = t.top()
    w, h = m['x'], m['y']
    pitch = max(w, 32)
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            s = mt.tiled_offset_2d(x, y, pitch, 2)
            out[(y * w + x) * 4:(y * w + x) * 4 + 4] = data[s:s + 4]
    return w, h, bytes(out)


def to_guest(im):
    im = im.convert('RGBA')
    out = bytearray()
    for r, g, b, a in im.getdata():
        out += bytes((a, r, g, b))
    return bytes(out)


def face(letter_game, src, glow, size, fit=True):
    switch_letter = GLOBAL[letter_game]
    c = LETTER_COLOURS[switch_letter]
    tint = tuple(min(255, int(v * 1.25)) for v in c)
    img = Image.open(os.path.join(src, switch_letter + '.png'))
    if glow:
        # 2026-11-06: the letter fitted to the pixel grid (me2_icon_style.grid_fit_glow; the hacking mini-game shows these
        # texel for texel). A pre-blur tried first made it worse: the mini-game does not shrink them.
        dx = GLOW_ICON_DX.get(switch_letter, 0)
        if not fit:  # the HD sizes (HD_SIZES): drawn smaller with mip levels; the grid fit is for the 32 px one
            return st.style_glow(img, size, tint, icon_dx=dx * size // 32)
        shift = st.grid_fit_glow(img, size, tint, icon_dx=dx)
        print(f'  glow {switch_letter}: letter shift {shift} (8x canvas pixels)')
        return st.style_glow(img, size, tint, icon_dx=dx, letter_shift=shift)
    return st.style(img, size, tint, 3, letter=tuple(int(v * 0.30) for v in c))


def main():
    pkg, src, out_path = sys.argv[1:4]
    entries = []
    for pkg_path in [pkg] + sys.argv[4:]:
        scan(mp.Package(open(pkg_path, 'rb').read()), src, entries)
    write(entries, out_path)


def scan(p, src, entries):
    for i, e in enumerate(p.exports):
        full = p.full_name(i + 1)
        if p.class_name(e) != 'Texture2D':
            continue
        name = full.split('.')[-1]
        if full.startswith('GUI_GlobalIcons.Buttons.') and name.lower().startswith('xbox_btn_'):
            key = name[len('XBox_Btn_'):]
            if key not in GLOBAL:
                continue
            if key in ('A', 'B', 'X', 'Y'):
                make = lambda size, k=key: face(k, src, False, size)
            else:
                def make(size, k=key):
                    img = st.style(Image.open(os.path.join(src, GLOBAL[k] + '.png')), size, (185, 185, 185), 3)
                    return lower_shoulder_icon(img) if k in ('LB', 'RB', 'LT', 'RT') else img
        elif full.startswith('GUI_SF_SharedAssets.') and name in GLOW:
            make = lambda size, k=GLOW[name]: face(k, src, True, size, fit=size <= 32)
        elif full.startswith('GUI_SF_HUD.') and name in HUD:
            make = lambda size, k=HUD[name]: face(k, src, True, size, fit=size <= 32)
        elif full.startswith('GUI_SF_HUD.') and name in WHEEL:
            make = lambda size, k=WHEEL[name]: wheel_icon(k, src, size)
        elif full in SCREENS:
            make = lambda size, k=SCREENS[full]: face(k, src, True, size, fit=size <= 32)
        elif full in GALAXY:
            make = lambda size, k=GALAXY[full]: galaxy_icon(k, src, size)
        else:
            continue
        t = mt.Texture(p, i + 1)
        w, h, raw = guest_linear(t)
        if any(fnv1a64(raw) == e[1] for e in entries):  # the same texture under another name (HUD = diagram)
            print(full, 'same as an earlier one')
            continue
        img = make(64 if (w, h) == (64, 64) else 32)
        img = img.resize((w, h), Image.LANCZOS) if img.size != (w, h) else img
        # 2026-11-06 (user: docked, 1080p, the icons look low-resolution): the same icon rendered at the HD sizes, the
        # host texture's upper levels (renut render/textures.cpp UpgradeToHdIcon: the game's 32 / 64 texel texture held
        # at 4x / 2x with mip levels down to its own size).
        hd = []
        for size in HD_SIZES.get(w, ()) if w == h else ():
            im = make(size)
            im = im.resize((size, size), Image.LANCZOS) if im.size != (size, size) else im
            hd.append((size, to_guest(im)))
        entries.append((full, fnv1a64(raw), w, h, to_guest(img), hd))
        print(full, f'{fnv1a64(raw):016X}', w, h)


# The HD levels per game size (the first is the host texture's level 0), and RenderScale's code for the factor
# (renut render/render_scale.h: 5 = 4x, 3 = 2x).
HD_SIZES = {32: (128, 64), 64: (128,)}
HD_SCALE_CODE = {32: 5, 64: 3}


def write(entries, out_path):
    with open(out_path, 'w', newline='\n') as f:
        f.write('// Generated by tools/me2_button_icons.py: the game\'s button icons (hash of the untiled guest bytes) and '
                'their Switch replacements (same layout: A, R, G, B a texel).\n')
        for n, (full, hsh, w, h, px, hd) in enumerate(entries):
            f.write(f'// {full}\nstatic const uint8_t kIcon{n}[{len(px)}] = {{')
            for k in range(0, len(px), 32):
                f.write('\n  ' + ','.join(str(b) for b in px[k:k + 32]) + ',')
            f.write('\n};\n')
        f.write('static const IconReplacement kIcons[] = {\n')
        for n, (full, hsh, w, h, px, hd) in enumerate(entries):
            f.write(f'  {{0x{hsh:016X}ull, {w}, {h}, kIcon{n}, "{full}"}},\n')
        f.write('};\n')
    print(len(entries), 'icons ->', out_path)
    hd_path = out_path[:-4] + '_hd.inc' if out_path.endswith('.inc') else out_path + '_hd'
    with open(hd_path, 'w', newline='\n') as f:
        f.write('// Generated by tools/me2_button_icons.py: the icons at their HD sizes (same layout: A, R, G, B a '
                'texel), by the hash of the game\'s texture; level 0 first, the game-size replacement is the last '
                'level.\n')
        for n, (full, hsh, w, h, px, hd) in enumerate(entries):
            for k, (size, data) in enumerate(hd):
                f.write(f'// {full} {size}x{size}\nstatic const uint8_t kIconHd{n}_{k}[{len(data)}] = {{')
                for i in range(0, len(data), 64):
                    f.write('\n' + ','.join(str(b) for b in data[i:i + 64]) + ',')
                f.write('\n};\n')
        f.write('static const IconHd kIconsHd[] = {\n')
        for n, (full, hsh, w, h, px, hd) in enumerate(entries):
            if not hd:
                continue
            ptrs = ', '.join(f'kIconHd{n}_{k}' for k in range(len(hd)))
            f.write(f'  {{0x{hsh:016X}ull, {HD_SCALE_CODE[w]}, {hd[0][0]}, {len(hd) + 1}, {{{ptrs}}}, kIcon{n}}},\n')
        f.write('};\n')
    print('HD levels ->', hd_path)


if __name__ == '__main__':
    main()
