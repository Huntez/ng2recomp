#!/usr/bin/env python3
# The README banner, in the style of the Ninja Gaiden Black Recompiled one:
# NINJA / G A I D E N / II / R E C O M P I L E D in Chakra Petch Bold Italic,
# stretched wide and slanted hard, each letter a near-black face with a chrome
# band round it (lit from below: a light top edge, black sides, white along
# the lower edges), a thin red line between face and band, and a red glow
# that also fills the counters; a dark red vignette with fine noise behind.
# Drawn at 2x and brought down to 1280x640.
#
# usage: tools/make_banner.py ChakraPetch-BoldItalic.ttf docs/images/banner.jpg
# (the font: github.com/google/fonts, ofl/chakrapetch; SIL Open Font License)
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT = sys.argv[1]
OUT = sys.argv[2]
S = 2                       # supersampling
W, H = 1280 * S, 640 * S
SHEAR = 0.5                 # the slant of the finished letters (dx per dy)
FONT_SLANT = 0.177          # Chakra Petch Italic's own slant
STRETCH = 1.5               # horizontal stretch of the glyphs
THIN = 0.01                 # erosion of the glyphs, as a share of the cap height
CAP = 0.7                   # cap height of Chakra Petch, in em

GLOW = np.array([255, 33, 23], float)
RIM = np.array([195, 20, 25], float)
FACE_TOP = np.array([46, 41, 45], float)
FACE_BOTTOM = np.array([15, 15, 15], float)

# Chrome band brightness down the height of a line of letters (0 = top of the
# band, 1 = bottom): a light top edge, black sides, then the white underside.
BAND_CURVE = [(0.0, 0.80), (0.105, 0.15), (0.13, 0.0), (0.55, 0.0), (0.57, 0.05),
              (0.59, 0.25), (0.63, 0.55), (0.67, 0.75), (0.71, 0.89), (0.75, 0.98),
              (0.78, 1.0), (0.85, 1.0), (0.88, 0.83), (0.92, 0.67), (0.96, 0.45),
              (0.99, 0.12), (1.0, 0.05)]


def glyph_strip(text, size, gap):
    """The word as a binary image: glyphs side by side with `gap` px between
    their ink boxes, then stretched and sheared. Returns (array, cap height)."""
    font = ImageFont.truetype(FONT, size)
    boxes = [font.getbbox(c) for c in text]
    widths = [b[2] - b[0] for b in boxes]
    top = min(b[1] for b in boxes)
    bottom = max(b[3] for b in boxes)
    pad = size // 2
    img = Image.new("L", (int(sum(widths) + gap * (len(text) - 1)) + 2 * pad,
                          bottom - top + 2 * pad), 0)
    d = ImageDraw.Draw(img)
    x = pad
    for c, b, w in zip(text, boxes, widths):
        d.text((x - b[0], pad - top), c, font=font, fill=255)
        x += w + gap
    return finish_strip(img, pad, bottom - top)


def finish_strip(img, pad, cap):
    """Stretch and slant a glyph strip; keep ink `pad` px from the top edge."""
    w, h = img.size
    img = img.resize((int(w * STRETCH), h), Image.LANCZOS)
    w = img.size[0]
    extra = SHEAR - FONT_SLANT * STRETCH
    img = img.transform((w + int(h * extra), h), Image.AFFINE,
                        (1, extra, -h * extra, 0, 1, 0), Image.BICUBIC)
    return np.array(img) > 128, pad, cap


def bars_strip(n, cap, width, gap):
    """n slanted bars (the roman numeral), cap px tall, width px wide each."""
    pad = cap // 2
    img = Image.new("L", (n * width + (n - 1) * gap + int(cap * SHEAR) + 2 * pad,
                          cap + 2 * pad), 0)
    d = ImageDraw.Draw(img)
    x = pad
    for _ in range(n):
        d.polygon([(x + cap * SHEAR, pad), (x + cap * SHEAR + width, pad),
                   (x + width, pad + cap), (x, pad + cap)], fill=255)
        x += width + gap
    return np.array(img) > 128, pad, cap


def place(strip, pad, cap, cx, top):
    """Put a strip on a full-size canvas, ink top at `top`, centred on cx."""
    out = np.zeros((H, W), bool)
    cols = np.where(strip.any(axis=0))[0]
    x0 = int(cx - (cols[0] + cols[-1]) / 2)
    y0 = int(top - pad)
    sh, sw = strip.shape
    ys, xs = slice(max(y0, 0), min(y0 + sh, H)), slice(max(x0, 0), min(x0 + sw, W))
    out[ys, xs] = strip[ys.start - y0:ys.stop - y0, xs.start - x0:xs.stop - x0]
    return out


class Dilator:
    """Dilation of one mask by an ellipse, through FFT convolution (there is
    no scipy to lean on). The ellipse is rx wide, ry tall and sits dy px
    above the letter's own outline, all as it would be before the slant: the
    kernel is sheared like the letters, so a stem's two sides get the same
    width while the top edge gets rx + dy and the bottom rx - dy."""
    def __init__(self, mask):
        self.f = np.fft.rfft2(mask.astype(np.float32))

    def __call__(self, rx, ry=None, dy=0.0):
        ry = rx if ry is None else ry
        n = int(np.ceil(ry + abs(dy)))
        m = int(np.ceil(rx + SHEAR * n))
        yy, xx = np.mgrid[-n:n + 1, -m:m + 1]
        u, v = xx + SHEAR * yy, yy + dy
        ell = ((u / rx) ** 2 + (v / ry) ** 2 <= 1).astype(np.float32)
        k = np.zeros((H, W), np.float32)
        k[:2 * n + 1, :2 * m + 1] = ell
        k = np.roll(k, (-n, -m), axis=(0, 1))
        return np.fft.irfft2(self.f * np.fft.rfft2(k), s=(H, W)) > 0.5


def blur(mask, sigma):
    img = Image.fromarray((mask * 255).astype(np.uint8), "L")
    return np.array(img.filter(ImageFilter.GaussianBlur(float(sigma)))).astype(np.float32) / 255


def row_curve(mask, curve):
    """Per-row value from a (t, v) curve over the rows the mask covers."""
    rows = np.where(mask.any(axis=1))[0]
    t = (np.arange(H) - rows[0]) / max(rows[-1] - rows[0], 1)
    return np.interp(t, [c[0] for c in curve], [c[1] for c in curve])


class Word:
    def __init__(self, mask, band, top_light, rim, thin=0.0):
        """band: (rx, ry, dy) of the chrome band's ellipse, px at 1x.
        top_light: how bright the band's top edge is (the big letters have a
        light strip there, the small ones stay black). rim: the red line's
        width along the sides, px at 1x."""
        if thin > 0:
            # Erode: dilate the outside. Bold Italic stretched this wide is a
            # little heavier than the Black banner's letters.
            mask = ~Dilator(~mask)(thin * S)
        self.mask = mask
        self.curve = [(t, v if t > 0.2 else v * top_light / 0.8) for t, v in BAND_CURVE]
        dil = Dilator(mask)
        rx, ry, dy = band
        self.outer = dil(rx * S, ry * S, dy * S)
        # The red line between face and band: a px or two along the sides and
        # bottom, deeper along the top edges, with a black line above it.
        self.red = dil(rim * S, (rim + 1.5) * S, 1.5 * S)
        self.dark = dil(rim * S, (rim + 2.7) * S, 3.0 * S)

    def glow(self):
        """Measured on the Black banner: a tight blur the same size for every
        line, and wider ones that grow with the height of the letters."""
        rows = np.where(self.outer.any(axis=1))[0]
        k = (rows[-1] - rows[0]) / S / 153
        return (0.69 * blur(self.outer, 8 * S) + 0.35 * k * blur(self.outer, 16 * S)
                + 0.07 * k * blur(self.outer, 32 * S))

    def paint(self, canvas):
        band = row_curve(self.outer, self.curve)[:, None] * 255
        canvas[self.outer] = np.repeat(band, W, axis=1)[self.outer][:, None]
        canvas[self.dark] = 0
        canvas[self.red] = RIM
        t = row_curve(self.mask, [(0, 0), (1, 1)])[:, None, None]
        face = FACE_TOP + (FACE_BOTTOM - FACE_TOP) * t
        canvas[self.mask] = np.repeat(face, W, axis=1)[self.mask]


def word(text, cap, width, cx, top, band, top_light, rim):
    """A word with the given cap height and overall ink width (1x px)."""
    size = int(round(cap * S / CAP))
    strip, pad, _ = glyph_strip(text, size, 0)
    ink = np.where(strip.any(axis=0))[0]
    natural = (ink[-1] - ink[0]) / S
    gap = (width - natural) / (len(text) - 1) * S / STRETCH
    strip, pad, _ = glyph_strip(text, size, max(gap, 0))
    return Word(place(strip, pad, cap, cx * S, top * S), band, top_light, rim, THIN * cap)


def numeral(n, cap, bar, gap, cx, top, band, top_light, rim):
    strip, pad, _ = bars_strip(n, cap * S, bar * S, gap * S)
    return Word(place(strip, pad, cap, cx * S, top * S), band, top_light, rim)


def background():
    yy, xx = np.mgrid[0:H, 0:W]
    d = np.sqrt(((xx - W / 2) / (W * 0.56)) ** 2 + ((yy - H / 2) / (H * 0.9)) ** 2)
    v = np.clip(1 - d, 0, 1) ** 1.2
    base = v[..., None] * np.array([48, 2, 2]) + np.array([3, 3, 5])
    rng = np.random.default_rng(7)
    base = base + rng.normal(0, 3.0, (H, W, 1)) + rng.normal(0, 1.2, (H, W, 3))
    return base


def streak(canvas, x0, x1, ys):
    """GAIDEN's double red line, soft, running behind the letters."""
    for y, thick in ys:
        m = np.zeros((H, W), np.float32)
        m[int((y - thick / 2) * S):int((y + thick / 2) * S), x0 * S:x1 * S] = 1
        core = blur(m, 1.5 * S)
        halo = blur(m, 6 * S)
        canvas += (core * 0.5 + halo * 0.25)[..., None] * np.array([230, 40, 30])
    return canvas


def main():
    canvas = background()
    # Letter tops and cap heights follow the Black banner: four tightly stacked
    # lines, the chrome band and glow scaled with each line's height.
    words = [
        word("NINJA", 126, 868, 640, 90, (11, 15.5, 7.5), 0.8, 1.0),
        word("GAIDEN", 42, 948, 640, 271, (4, 4, 0), 0.0, 1.8),
        numeral(2, 130, 56, 44, 640, 362, (11, 15.5, 7.5), 0.8, 1.0),
        word("RECOMPILED", 35, 1020, 640, 547, (4, 4, 0), 0.0, 1.8),
    ]
    for w in words:
        canvas += w.glow()[..., None] * GLOW
    cols = np.where(words[1].outer.any(axis=0))[0]
    canvas = streak(canvas, int(cols[0] / S), int(cols[-1] / S), [(282, 4), (294, 3.5)])
    for w in words:
        w.paint(canvas)
    img = Image.fromarray(np.clip(canvas, 0, 255).astype(np.uint8), "RGB")
    img = img.resize((W // S, H // S), Image.LANCZOS)
    # Fine grain at the final size, as on the Black banner.
    grain = np.random.default_rng(3).normal(0, 1.6, (H // S, W // S, 1))
    img = Image.fromarray(np.clip(np.array(img, float) + grain, 0, 255).astype(np.uint8), "RGB")
    img.save(OUT, quality=93)


main()
