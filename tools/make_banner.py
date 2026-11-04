# The README banner, in the style of the Ninja Gaiden Black Recompiled one:
# Chakra Petch Bold Italic, dark letters with a chrome lower edge, a red
# rim and a red glow, on a dark red ground. Drawn at 2x, saved at 1280x640.
#
# usage: tools/make_banner.py ChakraPetch-BoldItalic.ttf docs/images/banner.jpg
# (the font: github.com/google/fonts, ofl/chakrapetch; SIL Open Font License)
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT = sys.argv[1]
OUT = sys.argv[2]
S = 2
W, H = 1280 * S, 640 * S

def text_mask(text, size, spacing=0.0, stretch=1.0, bold=0):
    font = ImageFont.truetype(FONT, size)
    widths = [font.getbbox(c)[2] - font.getbbox(c)[0] for c in text]
    gap = size * spacing
    total = sum(widths) + gap * (len(text) - 1)
    top = min(font.getbbox(c)[1] for c in text if c != " ")
    bottom = max(font.getbbox(c)[3] for c in text if c != " ")
    pad = 110 * S
    img = Image.new("L", (int(total) + 2 * pad, bottom - top + 2 * pad), 0)
    d = ImageDraw.Draw(img)
    x = pad
    for c, w in zip(text, widths):
        d.text((x - font.getbbox(c)[0], pad - top), c, font=font, fill=255)
        x += w + gap
    if stretch != 1.0:
        img = img.resize((int(img.size[0] * stretch), img.size[1]), Image.LANCZOS)
    # More slant than the font's own italic, and heavier.
    shear = 0.22
    w, h = img.size
    img = img.transform((w + int(h * shear), h), Image.AFFINE, (1, shear, -h * shear, 0, 1, 0),
                        Image.BICUBIC)
    img = img.filter(ImageFilter.MaxFilter(2 * bold * S + 1)) if bold else img
    return img.point(lambda v: 255 if v > 110 else 0)

def vertical(h, stops):
    ys = np.linspace(0, 1, h)
    out = np.zeros((h, 3))
    for i in range(3):
        out[:, i] = np.interp(ys, [s[0] for s in stops], [s[1][i] for s in stops])
    return out

def layer(mask, stops):
    h, w = mask.size[1], mask.size[0]
    grad = vertical(h, stops)[:, None, :].repeat(w, 1)
    rgb = Image.fromarray(grad.astype(np.uint8), "RGB")
    return rgb, mask

def grow(mask, px):
    return mask.filter(ImageFilter.MaxFilter(px * 2 + 1)) if px > 0 else mask

def shrink(mask, px):
    return mask.filter(ImageFilter.MinFilter(px * 2 + 1)) if px > 0 else mask

def draw_word(canvas, glow, text, size, cx, cy, spacing=0.0, rim=6, stretch=1.0, bold=0):
    m = text_mask(text, size * S, spacing, stretch, bold)
    x0, y0 = int(cx * S - m.size[0] / 2), int(cy * S - m.size[1] / 2)
    r = rim * S // 2
    # The red glow under it all.
    g = grow(m, r * 3).filter(ImageFilter.GaussianBlur(14 * S))
    glow.paste(Image.new("L", m.size, 255), (x0, y0), g)
    # Red rim, chrome edge (dark at the top, white at the bottom), dark face.
    outer = grow(m, r + S)
    rim_rgb = Image.new("RGB", m.size, (200, 18, 20))
    canvas.paste(rim_rgb, (x0, y0), outer)
    chrome = grow(m, r)
    rows = np.where(np.array(chrome).max(axis=1) > 0)[0]
    t0, t1 = rows[0] / chrome.size[1], rows[-1] / chrome.size[1]
    k = lambda v: t0 + (t1 - t0) * v
    c_rgb, _ = layer(chrome, [(0, (90, 90, 96)), (k(0.0), (90, 90, 96)), (k(0.5), (120, 120, 128)),
                              (k(0.6), (240, 240, 245)), (k(0.85), (255, 255, 255)),
                              (k(1.0), (170, 170, 176)), (1, (170, 170, 176))])
    canvas.paste(c_rgb, (x0, y0), chrome)
    face = shrink(m, S)
    lift = max(3, rim // 2 + 1)
    # Measured on the letters' own height (the mask has padding round it).
    rows = np.where(np.array(face).max(axis=1) > 0)[0]
    t0, t1 = rows[0] / face.size[1], rows[-1] / face.size[1]
    k = lambda v: t0 + (t1 - t0) * v
    f_rgb, _ = layer(face, [(0, (58, 56, 62)), (k(0.0), (58, 56, 62)), (k(0.52), (22, 21, 25)),
                            (k(0.56), (14, 13, 16)), (k(0.6), (74, 74, 80)), (k(0.8), (34, 34, 38)),
                            (k(1.0), (14, 13, 16)), (1, (14, 13, 16))])
    # The face sits a little up, so the chrome shows as a lower edge.
    canvas.paste(f_rgb, (x0, y0 - lift * S), face)
    thin = Image.new("RGB", m.size, (150, 12, 14))
    edge = Image.fromarray(np.array(face) - np.array(shrink(face, S)), "L")
    canvas.paste(thin, (x0, y0 - lift * S), edge)

def main():
    yy, xx = np.mgrid[0:H, 0:W]
    d = np.sqrt(((xx - W / 2) / (W * 0.62)) ** 2 + ((yy - H / 2) / (H * 0.62)) ** 2)
    base = np.clip(1 - d, 0, 1)[..., None] * np.array([46, 4, 6]) + np.array([8, 5, 6])
    rng = np.random.default_rng(2)
    base += rng.normal(0, 2.2, (H, W, 1))
    canvas = Image.fromarray(np.clip(base, 0, 255).astype(np.uint8), "RGB")
    glow = Image.new("L", (W, H), 0)
    words = Image.new("RGB", (W, H), (0, 0, 0))
    words_mask = Image.new("L", (W, H), 0)
    # Each word on its own layer so the glows add up under all of them.
    layout = [
        ("NINJA", 196, 640, 140, 0.1, 9, 1.25, 4),
        ("GAIDEN", 72, 640, 286, 1.4, 5, 1.2, 1),
        ("II", 210, 640, 422, 0.08, 9, 1.6, 6),
        ("RECOMPILED", 62, 640, 570, 0.9, 5, 1.2, 1),
    ]
    for text, size, cx, cy, spacing, rim, stretch, bold in layout:
        layer_rgb = Image.new("RGB", (W, H), (0, 0, 0))
        draw_word(layer_rgb, glow, text, size, cx, cy, spacing, rim, stretch, bold)
        m = (np.array(layer_rgb).max(axis=2) > 0).astype(np.uint8) * 255
        words.paste(layer_rgb, (0, 0), Image.fromarray(m, "L"))
        words_mask = Image.fromarray(np.maximum(np.array(words_mask), m), "L")
    red = Image.new("RGB", (W, H), (230, 30, 26))
    canvas = Image.composite(red, canvas, glow.point(lambda v: int(v * 0.75)))
    # GAIDEN's red streak through the middle, as on the Black banner.
    streak = Image.new("L", (W, H), 0)
    sd = ImageDraw.Draw(streak)
    sd.rectangle([200 * S, 283 * S, 1080 * S, 286 * S], fill=255)
    streak = streak.filter(ImageFilter.GaussianBlur(3 * S))
    canvas = Image.composite(Image.new("RGB", (W, H), (255, 60, 40)), canvas, streak)
    canvas.paste(words, (0, 0), words_mask)
    canvas = canvas.resize((1280, 640), Image.LANCZOS)
    canvas.save(OUT, quality=92)

main()
