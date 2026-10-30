# Draws the launcher icon (a play triangle between code brackets on a dark rounded square) at each size
# and writes a multi-size .ico with PNG entries. Needs Pillow.
# Usage: python tools/make_icon.py src/launcher.ico [preview.png]
import io, struct, sys
from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
# Xbox's colours (its green, with white), not its logo: the shape stays our own.
BG_TOP, BG_BOTTOM = (82, 176, 67), (16, 124, 16)
ACCENT = (255, 255, 255)
BRACKET = (226, 245, 220)


def render(size):
    ss = 8  # supersampling
    n = size * ss
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    # Vertical gradient clipped to a rounded square.
    grad = Image.new("RGBA", (n, n))
    gd = ImageDraw.Draw(grad)
    for y in range(n):
        t = y / (n - 1)
        c = tuple(round(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM))
        gd.line([(0, y), (n, y)], fill=c + (255,))
    mask = Image.new("L", (n, n), 0)
    inset = round(n * (0.03 if size >= 32 else 0.0))
    ImageDraw.Draw(mask).rounded_rectangle([inset, inset, n - 1 - inset, n - 1 - inset],
                                           radius=round(n * 0.22), fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)

    small = size < 32
    # Play triangle, optically centred (a little right of the middle).
    h = n * (0.50 if small else 0.38)
    cx, cy = n * (0.54 if small else 0.53), n * 0.5
    w = h * 0.87
    d.polygon([(cx - w / 2, cy - h / 2), (cx - w / 2, cy + h / 2), (cx + w / 2, cy)], fill=ACCENT + (255,))

    if not small:
        # Code brackets < > either side.
        lw = max(ss, round(n * 0.055))
        bh, bw = n * 0.30, n * 0.12
        for side in (-1, 1):
            x_tip = n / 2 + side * n * 0.36
            x_arm = x_tip - side * bw
            pts = [(x_arm, cy - bh / 2), (x_tip, cy), (x_arm, cy + bh / 2)]
            d.line(pts, fill=BRACKET + (255,), width=lw, joint="curve")
            for p in (pts[0], pts[2]):
                d.ellipse([p[0] - lw / 2, p[1] - lw / 2, p[0] + lw / 2, p[1] + lw / 2], fill=BRACKET + (255,))
    return img.resize((size, size), Image.LANCZOS)


def write_ico(path, images):
    entries, blobs = [], []
    offset = 6 + 16 * len(images)
    for im in images:
        buf = io.BytesIO()
        im.save(buf, "PNG")
        data = buf.getvalue()
        s = im.size[0]
        entries.append(struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset))
        blobs.append(data)
        offset += len(data)
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(images)))
        f.write(b"".join(entries))
        f.write(b"".join(blobs))


images = [render(s) for s in SIZES]
write_ico(sys.argv[1], images)
if len(sys.argv) > 2:
    # Preview: every size side by side on light and dark backgrounds.
    W = sum(SIZES) + 10 * (len(SIZES) + 1)
    prev = Image.new("RGBA", (W, 2 * 256 + 30), (255, 255, 255, 255))
    ImageDraw.Draw(prev).rectangle([0, 256 + 20, W, 2 * 256 + 30], fill=(32, 32, 32, 255))
    x = 10
    for s, im in zip(SIZES, images):
        prev.alpha_composite(im, (x, 5))
        prev.alpha_composite(im, (x, 256 + 25))
        x += s + 10
    prev.save(sys.argv[2])
print("wrote", sys.argv[1])
