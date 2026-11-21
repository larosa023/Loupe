#!/usr/bin/env python3
"""Draws Loupe's icon (res/loupe.ico + res/loupe.png). Run once; the outputs are committed."""
from PIL import Image, ImageDraw, ImageFilter
import os

def draw(size):
    s = size * 4  # supersample
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    # rounded square with a diagonal gradient
    grad = Image.new("RGBA", (s, s))
    gp = grad.load()
    a, b = (46, 196, 196), (79, 110, 255)
    for y in range(s):
        for x in range(s):
            t = (x + y) / (2 * s - 2)
            gp[x, y] = tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3)) + (255,)
    mask = Image.new("L", (s, s), 0)
    m = int(s * 0.04)
    ImageDraw.Draw(mask).rounded_rectangle([m, m, s - m, s - m], radius=int(s * 0.22), fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    # lens
    cx, cy, r = s * 0.44, s * 0.42, s * 0.235
    lens = Image.new("L", (s, s), 0)
    ImageDraw.Draw(lens).ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    scene = Image.new("RGBA", (s, s), (255, 255, 255, 0))
    sd = ImageDraw.Draw(scene)
    sd.rectangle([0, 0, s, s], fill=(220, 240, 255, 255))
    sd.ellipse([cx + r * 0.05, cy - r * 0.62, cx + r * 0.5, cy - r * 0.17], fill=(255, 205, 90, 255))
    sd.polygon([(cx - r * 1.1, cy + r), (cx - r * 0.35, cy - r * 0.05), (cx + r * 0.15, cy + r * 0.45), (cx + r * 0.45, cy + r * 0.15), (cx + r * 1.2, cy + r)], fill=(52, 150, 110, 255))
    img.paste(scene, (0, 0), lens)
    ring = s * 0.055
    d.ellipse([cx - r - ring / 2, cy - r - ring / 2, cx + r + ring / 2, cy + r + ring / 2], outline=(255, 255, 255, 255), width=int(ring))
    # handle
    import math
    ang = math.radians(45)
    x0, y0 = cx + (r + ring * 0.4) * math.cos(ang), cy + (r + ring * 0.4) * math.sin(ang)
    x1, y1 = cx + r * 2.05 * math.cos(ang), cy + r * 2.05 * math.sin(ang)
    d.line([x0, y0, x1, y1], fill=(255, 255, 255, 255), width=int(s * 0.085))
    d.ellipse([x1 - s * 0.0425, y1 - s * 0.0425, x1 + s * 0.0425, y1 + s * 0.0425], fill=(255, 255, 255, 255))
    return img.resize((size, size), Image.LANCZOS)

here = os.path.dirname(os.path.abspath(__file__))
res = os.path.join(here, "..", "res")
big = draw(256)
big.save(os.path.join(res, "loupe.png"))
sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
imgs = [draw(z) for z in sizes]
imgs[-1].save(os.path.join(res, "loupe.ico"), sizes=[(z, z) for z in sizes], append_images=imgs[:-1])
print("ok")
