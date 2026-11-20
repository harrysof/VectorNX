from PIL import Image, ImageDraw, ImageFilter, ImageOps
import math

W, H = 1920, 1080
base = Image.new("RGB", (W, H))
px = base.load()
for y in range(H):
    t = y / (H - 1)
    # near-black top to slightly blue-charcoal bottom
    r = int(7 + 8 * t)
    g = int(10 + 11 * t)
    b = int(15 + 15 * t)
    for x in range(W):
        # subtle horizontal falloff
        e = 1.0 - 0.25 * abs((x / (W - 1)) - 0.5) * 2
        px[x, y] = (int(r * e), int(g * e), int(b * e))

# soft colour glows
glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
gd = ImageDraw.Draw(glow)
gd.ellipse((1180, 280, 1900, 980), fill=(255, 96, 48, 46))
gd.ellipse((-250, -300, 850, 600), fill=(32, 210, 180, 34))
glow = glow.filter(ImageFilter.GaussianBlur(160))
base = Image.alpha_composite(base.convert("RGBA"), glow)

# speed lines: lower-left to upper-right, orange and teal
lines = Image.new("RGBA", (W, H), (0, 0, 0, 0))
ld = ImageDraw.Draw(lines)
for i in range(54):
    off = i * 46 - 620
    alpha = int(12 + (i % 7) * 6)
    color = (255, 106, 61, alpha) if i % 3 else (45, 212, 191, alpha)
    width = 1 + (i % 5)
    ld.line([(-300, 1050 - off), (W + 300, 120 - off)], fill=color, width=width)
lines = lines.filter(ImageFilter.GaussianBlur(2.2))
base = Image.alpha_composite(base, lines)

# runner silhouette, right third, low contrast
sil = Image.new("RGBA", (W, H), (0, 0, 0, 0))
sd = ImageDraw.Draw(sil)
C = (4, 6, 9, 235)
O = (255, 106, 61, 60)

def limb(points, width, fill):
    for a, b in zip(points, points[1:]):
        sd.line([a, b], fill=fill, width=width)

# rim first, offset slightly
rim = Image.new("RGBA", (W, H), (0, 0, 0, 0))
rd = ImageDraw.Draw(rim)
def rlimb(points, width):
    for a, b in zip(points, points[1:]):
        rd.line([(a[0] + 5, a[1] + 4), (b[0] + 5, b[1] + 4)], fill=O, width=width)

body = [(1480, 330), (1525, 335), (1560, 560), (1495, 585), (1445, 560)]
sd.polygon(body, fill=C)
sd.ellipse((1472, 252, 1548, 328), fill=C)
arms = [
    [(1478, 370), (1370, 455), (1280, 405)],
    [(1522, 370), (1640, 440), (1735, 382)],
]
legs = [
    [(1478, 565), (1390, 715), (1270, 770)],
    [(1535, 565), (1625, 705), (1735, 650)],
]
for a in arms:
    rlimb(a, 44)
    limb(a, 38, C)
for l in legs:
    rlimb(l, 50)
    limb(l, 44, C)
rim = rim.filter(ImageFilter.GaussianBlur(7))
sil = sil.filter(ImageFilter.GaussianBlur(1.2))
base = Image.alpha_composite(base, rim)
base = Image.alpha_composite(base, sil)

# vignette
vig = Image.new("L", (W, H), 0)
vd = ImageDraw.Draw(vig)
vd.ellipse((-W * 0.25, -H * 0.35, W * 1.25, H * 1.35), fill=255)
vig = ImageOps.invert(vig).filter(ImageFilter.GaussianBlur(180))
black = Image.new("RGBA", (W, H), (0, 0, 0, 255))
black.putalpha(vig.point(lambda v: int(v * 0.72)))
base = Image.alpha_composite(base, black)

# subtle noise
noise = Image.effect_noise((W, H), 7).convert("L").point(lambda v: int(v * 0.10))
base = Image.alpha_composite(base, Image.merge("RGBA", (noise, noise, noise, noise)))

base.convert("RGB").save("launcher/assets/background.png", optimize=True)
print("wrote launcher/assets/background.png", base.size)
