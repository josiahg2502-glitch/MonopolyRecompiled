"""Draws the app icon (original art: a green house and a die on a red tile).

usage: make_icon.py <out_dir>   writes monopoly.ico (16-256 px) and monopoly_256.png
Small sizes drop the die so the house stays readable.
"""
import os, sys
from PIL import Image, ImageDraw, ImageFilter

SS = 4  # supersampling factor


def tile(d, s):
    m, r = 8 * s, 52 * s
    top, bottom = (229, 57, 53), (160, 20, 24)
    grad = Image.new("RGBA", (256 * s, 256 * s))
    gd = ImageDraw.Draw(grad)
    for y in range(256 * s):
        t = y / (256 * s)
        gd.line([(0, y), (256 * s, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    mask = Image.new("L", grad.size)
    ImageDraw.Draw(mask).rounded_rectangle([m, m, 256 * s - m, 256 * s - m], r, fill=255)
    d.paste(grad, (0, 0), mask)
    ImageDraw.Draw(d).rounded_rectangle([m + 6 * s, m + 6 * s, 256 * s - m - 6 * s, 256 * s - m - 6 * s],
                                        r - 6 * s, outline=(255, 255, 255, 70), width=3 * s)


def house(d, s, cx, base, w):
    """House centred on cx, sitting on y=base, body width w (all in 256-space)."""
    k = s
    body_h, roof_h = w * 0.62, w * 0.5
    x0, x1 = cx - w / 2, cx + w / 2
    y_body = base - body_h
    green, dark, light = (46, 160, 80), (18, 84, 40), (110, 205, 130)
    dr = ImageDraw.Draw(d)
    out = max(2, w * 0.045) * k
    # chimney, roof, body
    dr.rectangle([(x1 - w * 0.3) * k, (y_body - roof_h * 0.85) * k, (x1 - w * 0.14) * k, (y_body - roof_h * 0.3) * k],
                 fill=green, outline=dark, width=int(out))
    dr.polygon([((x0 - w * 0.1) * k, y_body * k), (cx * k, (y_body - roof_h) * k), ((x1 + w * 0.1) * k, y_body * k)],
               fill=green, outline=dark, width=int(out))
    dr.rectangle([x0 * k, y_body * k, x1 * k, base * k], fill=green, outline=dark, width=int(out))
    # roof highlight and door
    dr.line([((x0 - w * 0.02) * k, (y_body - 2) * k), (cx * k, (y_body - roof_h + w * 0.08) * k)],
            fill=light, width=int(out * 0.9))
    dw, dh = w * 0.24, body_h * 0.55
    dr.rectangle([(cx - dw / 2) * k, (base - dh) * k, (cx + dw / 2) * k, base * k], fill=(250, 246, 235))


def die(size, s):
    img = Image.new("RGBA", (size * s, size * s))
    dr = ImageDraw.Draw(img)
    dr.rounded_rectangle([2 * s, 2 * s, (size - 2) * s, (size - 2) * s], size * 0.2 * s,
                         fill=(252, 250, 244), outline=(40, 40, 40), width=3 * s)
    pip = size * 0.1
    for fx, fy in ((0.28, 0.28), (0.72, 0.28), (0.5, 0.5), (0.28, 0.72), (0.72, 0.72)):
        cx, cy = fx * size * s, fy * size * s
        dr.ellipse([cx - pip * s, cy - pip * s, cx + pip * s, cy + pip * s], fill=(30, 30, 30))
    return img.rotate(-14, resample=Image.BICUBIC, expand=True)


def shadow(img, s, offset=6, blur=6):
    a = img.split()[3].point(lambda v: v * 0.45)
    sh = Image.new("RGBA", img.size, (0, 0, 0, 0))
    sh.putalpha(a)
    return sh.filter(ImageFilter.GaussianBlur(blur * s)), offset * s


def render(full):
    s = SS
    img = Image.new("RGBA", (256 * s, 256 * s))
    tile(img, s)
    if full:
        layer = Image.new("RGBA", img.size)
        house(layer, s, 108, 196, 116)
        sh, off = shadow(layer, s)
        img.alpha_composite(sh, (off, off))
        img.alpha_composite(layer)
        d = die(84, s)
        sh, off = shadow(d, s)
        pos = (148 * s, 138 * s)
        img.alpha_composite(sh, (pos[0] + off, pos[1] + off))
        img.alpha_composite(d, pos)
    else:
        layer = Image.new("RGBA", img.size)
        house(layer, s, 128, 212, 150)
        img.alpha_composite(layer)
    return img


def main(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    full, small = render(True), render(False)
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    images = [(small if n <= 32 else full).resize((n, n), Image.LANCZOS) for n in sizes]
    images[-1].save(os.path.join(out_dir, "monopoly_256.png"))
    images[-1].save(os.path.join(out_dir, "monopoly.ico"), format="ICO",
                    sizes=[(n, n) for n in sizes], append_images=images[:-1])
    check = Image.open(os.path.join(out_dir, "monopoly.ico"))
    assert {(n, n) for n in sizes} <= set(check.info["sizes"]), check.info["sizes"]
    print("wrote", out_dir)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
