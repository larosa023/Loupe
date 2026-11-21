#!/usr/bin/env python3
"""Makes the images CI's smoke test opens: every format Loupe supports that a stock Windows
runner can decode, plus EXIF, orientation, transparency, animation, a fake RAW and a broken file.

Usage: make_test_images.py <out dir>
"""
import io, math, os, struct, sys
from PIL import Image, ImageDraw
from PIL.TiffImagePlugin import IFDRational as R

out = sys.argv[1]
os.makedirs(out, exist_ok=True)


def scene(w, h, hue=0):
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(0, h, max(1, h // 200)):
        t = y / h
        d.rectangle([0, y, w, y + h // 200 + 1], fill=(int(70 + 120 * t), int(130 + 80 * t), int(230 - 60 * t)))
    d.ellipse([w * 0.66, h * 0.12, w * 0.82, h * 0.12 + w * 0.16], fill=(255, 210, 90))
    d.polygon([(0, h), (w * 0.3, h * 0.45), (w * 0.55, h * 0.8), (w * 0.75, h * 0.55), (w, h)], fill=(50 + hue, 140, 95))
    # fine detail so 100% zoom is visibly sharper than the fit view
    for i in range(0, w, max(8, w // 300)):
        d.line([(i, h * 0.9), (i + 4, h)], fill=(30, 90, 60), width=1)
    d.text((w * 0.05, h * 0.05), "Loupe test %dx%d" % (w, h), fill=(255, 255, 255))
    return img


def exif_bytes(orientation=1, full=True):
    ex = Image.Exif()
    ex[0x0112] = orientation
    if full:
        ex[0x010F] = "Canon"
        ex[0x0110] = "Canon EOS R6"
        ex[0x0131] = "Loupe test"
        ex[0x0132] = "2026:11:15 16:12:30"
        ifd = ex.get_ifd(0x8769)
        ifd[0x9003] = "2026:11:15 16:12:30"
        ifd[0x8827] = 400
        ifd[0x829A] = R(1, 250)
        ifd[0x829D] = R(28, 10)
        ifd[0x920A] = R(35, 1)
        ifd[0xA434] = "RF24-105mm F4 L IS USM"
        gps = ex.get_ifd(0x8825)
        gps[1] = "N"
        gps[2] = (R(40, 1), R(24, 1), R(3600, 100))
        gps[3] = "W"
        gps[4] = (R(74, 1), R(10, 1), R(1200, 100))
    return ex.tobytes()


scene(1600, 1067).save(os.path.join(out, "exif-landscape.jpg"), quality=90, exif=exif_bytes())
scene(6000, 4000, 20).save(os.path.join(out, "large-6000.jpg"), quality=85)
# stored landscape, EXIF says rotate 90 degrees clockwise: shows as portrait
scene(1200, 800, 40).save(os.path.join(out, "rot6-portrait.jpg"), quality=90, exif=exif_bytes(6, False))

t = Image.new("RGBA", (800, 600), (0, 0, 0, 0))
d = ImageDraw.Draw(t)
d.ellipse([100, 100, 500, 500], fill=(255, 90, 90, 220))
d.rectangle([350, 200, 700, 520], fill=(80, 140, 255, 160))
t.save(os.path.join(out, "transparent.png"))

frames = []
for i in range(12):
    f = Image.new("RGBA", (240, 160), (0, 0, 0, 0))
    fd = ImageDraw.Draw(f)
    x = 20 + i * 15
    y = 120 - abs(math.sin(i / 11 * math.pi)) * 90
    fd.ellipse([x, y - 20, x + 40, y + 20], fill=(255, 170, 40, 255))
    frames.append(f)
frames[0].save(os.path.join(out, "anim.gif"), save_all=True, append_images=frames[1:], duration=80, loop=0, disposal=2, transparency=0)
frames[0].save(os.path.join(out, "anim-apng.png"), save_all=True, append_images=frames[1:], duration=80, loop=0, default_image=False)
frames[0].save(os.path.join(out, "anim.webp"), save_all=True, append_images=frames[1:], duration=80, loop=0, lossless=True)

scene(1280, 853, 60).save(os.path.join(out, "still.webp"), quality=85)
scene(1024, 683, 80).save(os.path.join(out, "photo.bmp"))
scene(1024, 683, 100).save(os.path.join(out, "photo.tiff"), compression="tiff_lzw")
ico = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
ImageDraw.Draw(ico).rounded_rectangle([16, 16, 240, 240], radius=50, fill=(79, 140, 255, 255))
ico.save(os.path.join(out, "icon.ico"), sizes=[(16, 16), (32, 32), (48, 48), (256, 256)])

with open(os.path.join(out, "vector.svg"), "w") as f:
    f.write('<svg xmlns="http://www.w3.org/2000/svg" width="640" height="480" viewBox="0 0 640 480">'
            '<rect width="640" height="480" fill="#e8f1ff"/>'
            '<circle cx="470" cy="120" r="60" fill="#ffcf5a"/>'
            '<path d="M0 480 L200 220 L330 380 L440 290 L640 480 Z" fill="#34966e"/>'
            '<text x="30" y="60" font-family="Segoe UI" font-size="36" fill="#1f3b70">SVG</text></svg>')


def fake_raw(path, make, model):
    """A TIFF-structured RAW (like DNG/NEF): IFD0 with camera tags and a JPEG preview."""
    buf = io.BytesIO()
    scene(1620, 1080, 120).save(buf, "JPEG", quality=88)
    jpg = buf.getvalue()
    entries = []
    strings = b""
    base_after_ifd = 8 + 2 + 7 * 12 + 4

    def ascii_tag(tag, s):
        nonlocal strings
        data = s.encode() + b"\0"
        off = base_after_ifd + len(strings)
        strings += data
        return struct.pack("<HHII", tag, 2, len(data), off)

    entries.append(struct.pack("<HHII", 0x00FE, 4, 1, 1))
    entries.append(ascii_tag(0x010F, make))
    entries.append(ascii_tag(0x0110, model))
    entries.append(struct.pack("<HHIHH", 0x0112, 3, 1, 1, 0))
    jpg_off = base_after_ifd + len(strings) + 0  # strings are fixed now
    entries.append(struct.pack("<HHII", 0x0201, 4, 1, 0))   # patched below
    entries.append(struct.pack("<HHII", 0x0202, 4, 1, len(jpg)))
    entries.append(struct.pack("<HHII", 0xC612, 1, 4, 0x00000401))   # DNGVersion 1.4
    entries.sort(key=lambda e: struct.unpack("<H", e[:2])[0])
    jpg_off = base_after_ifd + len(strings)
    entries = [e if struct.unpack("<H", e[:2])[0] != 0x0201 else struct.pack("<HHII", 0x0201, 4, 1, jpg_off) for e in entries]
    data = b"II*\0" + struct.pack("<I", 8) + struct.pack("<H", len(entries)) + b"".join(entries) + struct.pack("<I", 0) + strings + jpg
    with open(path, "wb") as f:
        f.write(data)


fake_raw(os.path.join(out, "raw-preview.dng"), "Adobe", "Loupe test DNG")
fake_raw(os.path.join(out, "raw-preview.nef"), "NIKON CORPORATION", "NIKON Z 6")

with open(os.path.join(out, "broken.png"), "wb") as f:
    f.write(b"\x89PNG\r\n\x1a\n" + os.urandom(4096))
print("\n".join(sorted(os.listdir(out))))
