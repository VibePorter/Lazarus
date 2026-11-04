"""Turns flat button glyphs (white shape, letters cut out, transparent background; any size) into icons in the style of
ME2's own button icons (GUI_GlobalIcons.Buttons: grey bevelled plastic, dark engraved letters, soft shadow).

  python tools/me2_icon_style.py <in.png> <out.png> [--size 64] [--tint 200,200,200]

The shape is the glyph's alpha with its holes filled (the button body); the holes are the letters. Steps, at 4x the
output size, then downscaled: drop shadow, dark rim, body with a vertical gradient, a light bevel along the top edge
and a darker one along the bottom, letters engraved (dark, with a light lip under them).
"""
import argparse

from PIL import Image, ImageChops, ImageFilter, ImageOps


def cutout_letters(image, letters):
    """Keep glyph detail in alpha as well as RGB, like the original ME2 button icons.

    Scaleform can replace the RGB with a flat disabled/highlight tint. Opaque dark letters then disappear;
    a letter-shaped alpha cutout remains readable through tinting, fading and highlight animation.
    Apply before downsampling so the cutouts keep the same antialiasing as the rest of the icon.
    """
    image.putalpha(ImageChops.multiply(image.getchannel('A'), ImageOps.invert(letters)))
    return image


def fill_holes(mask):
    # Flood the background from the border; whatever it cannot reach (inside the shape) becomes shape.
    w, h = mask.size
    inv = ImageOps.invert(mask.point(lambda v: 255 if v > 127 else 0))
    padded = Image.new('L', (w + 2, h + 2), 255)
    padded.paste(inv, (1, 1))
    from PIL import ImageDraw
    ImageDraw.floodfill(padded, (0, 0), 128)
    outside = padded.crop((1, 1, w + 1, h + 1)).point(lambda v: 255 if v == 128 else 0)
    return ImageOps.invert(outside)


def style(src, size=64, tint=(185, 185, 185), boldness=7, halo_px=9, shadow_down=0.05, dim=None, letter=None):
    big = size * 4
    glyph = src.convert('RGBA')
    # Fit the glyph's bounding box into the canvas with a margin for the shadow.
    alpha = glyph.getchannel('A')
    box = alpha.point(lambda v: 255 if v > 20 else 0).getbbox()
    glyph = glyph.crop(box)
    margin = int(big * 0.08)
    scale = min((big - 2 * margin) / glyph.width, (big - 2 * margin) / glyph.height)
    glyph = glyph.resize((max(1, int(glyph.width * scale)), max(1, int(glyph.height * scale))), Image.LANCZOS)
    a = Image.new('L', (big, big), 0)
    a.paste(glyph.getchannel('A'), ((big - glyph.width) // 2, (big - glyph.height) // 2))
    dim_mask = None
    if dim is not None:
        # A mask in the source's coordinates (the parts drawn darker: a D-pad's arms that are not pressed), placed as
        # the glyph is.
        d = dim.crop(box).resize(glyph.size, Image.LANCZOS)
        dim_mask = Image.new('L', (big, big), 0)
        dim_mask.paste(d, ((big - glyph.width) // 2, (big - glyph.height) // 2))

    body = fill_holes(a)
    letters = ImageChops.subtract(body, a.point(lambda v: 255 if v > 127 else 0))

    out = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    # Drop shadow under the button, as the game's icons have it: a dark copy of the body moved down (shadow_down of
    # the size), slightly blurred, plus a wide soft one.
    soft = body.filter(ImageFilter.GaussianBlur(big * 0.03))
    out.alpha_composite(Image.merge('RGBA', [Image.new('L', (big, big), 0)] * 3 + [soft.point(lambda v: v * 0.5)]),
                        (0, int(big * 0.015)))
    hard = body.filter(ImageFilter.GaussianBlur(big * 0.003))
    out.alpha_composite(Image.merge('RGBA', [Image.new('L', (big, big), 0)] * 3 + [hard.point(lambda v: v * 0.95)]),
                        (0, int(big * shadow_down)))
    # Dark rim (the body grown a little).
    rim = body.filter(ImageFilter.MaxFilter(5))
    out.alpha_composite(Image.merge('RGBA', [Image.new('L', (big, big), 60)] * 3 + [rim]))
    # Body: a vertical gradient from light to darker grey.
    grad = Image.linear_gradient('L').resize((big, big))
    r0, g0, b0 = tint
    top, bottom = 1.08, 0.70
    def channel(c):
        return grad.point(lambda v: max(0, min(255, int(c * (top + (bottom - top) * v / 255)))))
    body_rgba = Image.merge('RGBA', [channel(r0), channel(g0), channel(b0), body])
    out.alpha_composite(body_rgba)
    if dim_mask is not None:
        out.alpha_composite(Image.merge('RGBA', [Image.new('L', (big, big), 20)] * 3 +
                                        [ImageChops.multiply(dim_mask, body).point(lambda v: v * 0.55)]))
    # Bevels: light along the top edge, dark along the bottom one (the body minus itself shifted).
    shift = max(2, big // 64)
    top_edge = ImageChops.subtract(body, ImageChops.offset(body, 0, shift)).filter(ImageFilter.GaussianBlur(1.5))
    bottom_edge = ImageChops.subtract(body, ImageChops.offset(body, 0, -shift)).filter(ImageFilter.GaussianBlur(1.5))
    out.alpha_composite(Image.merge('RGBA', [Image.new('L', (big, big), 255)] * 3 + [top_edge.point(lambda v: v * 0.8)]))
    out.alpha_composite(Image.merge('RGBA', [Image.new('L', (big, big), 30)] * 3 + [bottom_edge.point(lambda v: v * 0.5)]))
    # Letters engraved as in the game's icons: a little bolder than the glyph's holes, a light halo around them and a
    # light lip below, then the dark letters.
    bold = letters.filter(ImageFilter.MaxFilter(boldness)) if boldness > 1 else letters
    halo = ImageChops.multiply(bold.filter(ImageFilter.MaxFilter(halo_px)).filter(ImageFilter.GaussianBlur(2)), body)
    out.alpha_composite(Image.merge('RGBA', [Image.new('L', (big, big), 235)] * 3 + [halo.point(lambda v: v * 0.55)]))
    lip = ImageChops.multiply(ImageChops.offset(bold, 0, shift), body)
    out.alpha_composite(Image.merge('RGBA', [Image.new('L', (big, big), 250)] * 3 + [lip.point(lambda v: v * 0.6)]))
    lc = letter if letter is not None else (38, 38, 38)
    out.alpha_composite(Image.merge('RGBA', [Image.new('L', (big, big), c) for c in lc] + [bold]))
    return cutout_letters(out, bold).resize((size, size), Image.LANCZOS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--size', type=int, default=64)
    ap.add_argument('--tint', default='185,185,185')
    ap.add_argument('--bold', type=int, default=7)
    a = ap.parse_args()
    style(Image.open(a.src), a.size, tuple(int(v) for v in a.tint.split(',')), a.bold).save(a.dst)


if __name__ == '__main__':
    main()


def style_dpad(full, pressed_src, size=64, tint=(185, 185, 185)):
    """A D-pad with one direction pressed: 'full' is the solid D-pad (arrows cut out), 'pressed_src' the same D-pad
    with only the pressed arm solid and the others drawn as outlines. The arms that are not pressed come out darker."""
    fa = full.convert('RGBA').getchannel('A').point(lambda v: 255 if v > 127 else 0)
    pa = pressed_src.convert('RGBA').getchannel('A').point(lambda v: 255 if v > 127 else 0)
    w = max(3, fa.width // 40) | 1
    solid_pressed = pa.filter(ImageFilter.MinFilter(w * 3)).filter(ImageFilter.MaxFilter(w * 3))  # outlines vanish
    body = fill_holes(fa)
    pressed_arm = fill_holes(solid_pressed).filter(ImageFilter.MaxFilter(w))
    dim = ImageChops.subtract(body, pressed_arm)
    return style(full, size, tint, 3, dim=dim)


def style_glow(src, size=32, tint=(120, 200, 90), letter=None, margin=0.14, bold=3, icon_dx=0):
    """The glowing variant of the game's face buttons (GUI_SF_SharedAssets.mainController_I*): a see-through body in
    the button's colour, a bright rim, a coloured glow around it and dark letters of the same hue with a bright edge.
    icon_dx: the whole icon (body, rim, glow and letter) moved this many output pixels right (negative: left)."""
    big = size * 8
    glyph = src.convert('RGBA')
    alpha = glyph.getchannel('A')
    box = alpha.point(lambda v: 255 if v > 20 else 0).getbbox()
    glyph = glyph.crop(box)
    margin = int(big * margin)
    scale = min((big - 2 * margin) / glyph.width, (big - 2 * margin) / glyph.height)
    glyph = glyph.resize((max(1, int(glyph.width * scale)), max(1, int(glyph.height * scale))), Image.LANCZOS)
    a = Image.new('L', (big, big), 0)
    a.paste(glyph.getchannel('A'), ((big - glyph.width) // 2, (big - glyph.height) // 2))
    body = fill_holes(a)
    letters = ImageChops.subtract(body, a.point(lambda v: 255 if v > 127 else 0))
    r, g, b = tint
    bright = tuple(min(255, int(c * 1.45 + 40)) for c in tint)
    dark = letter if letter is not None else tuple(int(c * 0.28) for c in tint)
    def solid(c, mask, k=1.0):
        return Image.merge('RGBA', [Image.new('L', (big, big), v) for v in c] + [mask.point(lambda v: int(v * k))])
    out = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    out.alpha_composite(solid(bright, body.filter(ImageFilter.GaussianBlur(big * 0.06)), 0.85))      # glow
    out.alpha_composite(solid(tint, body, 0.78))                                                         # body
    grad = Image.linear_gradient('L').resize((big, big))
    out.alpha_composite(solid((255, 255, 255), ImageChops.multiply(body, ImageOps.invert(grad)), 0.18))  # sheen
    ring = ImageChops.subtract(body, body.filter(ImageFilter.MinFilter((big // 40) | 1)))
    out.alpha_composite(solid(bright, ring.filter(ImageFilter.GaussianBlur(big * 0.006)), 1.0))          # rim
    bold = letters.filter(ImageFilter.MaxFilter(bold))
    edge = ImageChops.multiply(bold.filter(ImageFilter.MaxFilter((big // 48) | 1)), body)
    out.alpha_composite(solid(bright, edge.filter(ImageFilter.GaussianBlur(big * 0.004)), 0.9))          # letter edge
    out.alpha_composite(solid(dark, bold, 1.0))                                                          # letters
    out = cutout_letters(out, bold).resize((size, size), Image.LANCZOS)
    if icon_dx:
        moved = Image.new('RGBA', (size, size), (0, 0, 0, 0))
        moved.paste(out, (icon_dx, 0))
        out = moved
    return out
