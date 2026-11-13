"""Draws the OpenXR VR addon's picture: a VR headset whose lenses are a tinted helmet visor, over a dark racing
backdrop, with the addon's name. Usage: python make_icon.py <out.png>"""

import math
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 1024  # drawn large, saved at 512
FONT = r"C:\Windows\Fonts\bahnschrift.ttf"


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def main(out):
    img = Image.new("RGB", (S, S))
    px = img.load()
    # Backdrop: dark charcoal, lighter toward the top
    for y in range(S):
        c = lerp((34, 36, 44), (10, 10, 14), y / S)
        for x in range(S):
            px[x, y] = c
    d = ImageDraw.Draw(img, "RGBA")

    # Speed streaks behind the headset
    for i, (y, w, a) in enumerate([(300, 520, 70), (360, 680, 90), (690, 600, 80), (750, 420, 60)]):
        x0 = 60 if i % 2 == 0 else S - 60 - w
        d.rounded_rectangle([x0, y, x0 + w, y + 14], 7, fill=(225, 30, 40, a))

    # Headset shell
    shell = [150, 330, S - 150, 700]
    shadow = Image.new("L", (S, S), 0)
    ImageDraw.Draw(shadow).rounded_rectangle([shell[0] + 10, shell[1] + 28, shell[2] + 10, shell[3] + 28], 150, fill=170)
    img.paste((0, 0, 0), mask=shadow.filter(ImageFilter.GaussianBlur(26)))
    d = ImageDraw.Draw(img, "RGBA")
    d.rounded_rectangle(shell, 150, fill=(236, 238, 242))
    d.rounded_rectangle([shell[0] + 18, shell[1] + 18, shell[2] - 18, shell[3] - 18], 132, fill=(28, 30, 36))
    # Strap stubs
    d.rounded_rectangle([96, 440, 170, 590], 30, fill=(200, 202, 208))
    d.rounded_rectangle([S - 170, 440, S - 96, 590], 30, fill=(200, 202, 208))

    # Visor across the front: an iridium gradient (gold to red), with a reflection band
    visor = Image.new("RGB", (S, S))
    vp = visor.load()
    for y in range(S):
        for x in range(S):
            t = min(1.0, max(0.0, (x - 200) / 620 * 0.7 + (y - 370) / 300 * 0.3))
            vp[x, y] = lerp((255, 190, 40), (215, 30, 45), t)
    mask = Image.new("L", (S, S), 0)
    m = ImageDraw.Draw(mask)
    m.rounded_rectangle([shell[0] + 46, shell[1] + 46, shell[2] - 46, shell[3] - 46], 110, fill=255)
    # Nose cut-out between the lenses
    m.pieslice([S // 2 - 80, shell[3] - 150, S // 2 + 80, shell[3] + 10], 180, 360, fill=0)
    img.paste(visor, mask=mask)
    d = ImageDraw.Draw(img, "RGBA")
    d.polygon([(330, 390), (430, 390), (300, 610), (200, 610)], fill=(255, 255, 255, 60))
    d.polygon([(470, 390), (510, 390), (380, 610), (340, 610)], fill=(255, 255, 255, 40))

    # Name
    big = ImageFont.truetype(FONT, 150)
    big.set_variation_by_axes([700, 100]) if hasattr(big, "set_variation_by_axes") else None
    small = ImageFont.truetype(FONT, 66)
    title = "OpenXR VR"
    w = d.textlength(title, font=big)
    d.text(((S - w) / 2, 770), title, font=big, fill=(245, 246, 250))
    sub = "VISOR  ·  PERFORMANCE"
    w = d.textlength(sub, font=small)
    d.text(((S - w) / 2, 935), sub, font=small, fill=(225, 30, 40))
    sub2 = "F1 25 VR"
    w = d.textlength(sub2, font=small)
    d.text(((S - w) / 2, 175), sub2, font=small, fill=(180, 184, 194))

    img.resize((512, 512), Image.LANCZOS).save(out, optimize=True)
    print("wrote", out)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
