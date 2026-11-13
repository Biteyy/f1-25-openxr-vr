"""Builds the visor images for the OpenXR VR addon: helmet_visor_<visor>.png, one per visor in VISORS.

Each image is the visor's tint, one colour across the whole view: the DLL draws it head-locked over everything the
game shows, spanning +-kHelmetHalfAngleDeg (DrvOpenXR/HelmetView.cpp), so the whole game is seen through it. Blending
the tint over the game darkens it by the tint's opacity and shifts it toward the tint's colour, as a tinted visor does.

Straight (not premultiplied) alpha, sRGB colours. The visor ids must match the "visor" option's values in the addon's
swofty.json. Any PNG works in place of one of these (eg one with a darker strip along the top); it is stretched over
the same square.

Usage: python make_helmet_view.py <output folder>
"""

import os
import struct
import sys
import zlib

SIZE = 16  # one colour, so the image only needs to be big enough to sample cleanly

# id: (tint colour (linear 0..1), opacity)
VISORS = {
    "clear": ((0.0, 0.0, 0.0), 0.05),
    "light_smoke": ((0.0, 0.0, 0.0), 0.30),
    "dark_smoke": ((0.0, 0.0, 0.0), 0.55),
    "yellow": ((0.80, 0.55, 0.00), 0.25),
    "green": ((0.02, 0.35, 0.05), 0.30),
    "iridium_blue": ((0.01, 0.06, 0.35), 0.35),
    "iridium_gold": ((0.45, 0.25, 0.01), 0.35),
    "iridium_red": ((0.40, 0.03, 0.01), 0.35),
}


def to_srgb8(c):
    c = min(1.0, max(0.0, c))
    return int(round((c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055) * 255))


def write_png(path, rgba):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = (b"\x00" + bytes(rgba) * SIZE) * SIZE  # each row: filter "none", then SIZE pixels
    ihdr = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)  # 8-bit RGBA
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    os.makedirs(sys.argv[1], exist_ok=True)
    for visor, (tint, opacity) in VISORS.items():
        rgba = (to_srgb8(tint[0]), to_srgb8(tint[1]), to_srgb8(tint[2]), int(round(opacity * 255)))
        write_png(os.path.join(sys.argv[1], f"helmet_visor_{visor}.png"), rgba)
        print("wrote", visor, rgba)
