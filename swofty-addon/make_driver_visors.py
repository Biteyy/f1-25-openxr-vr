"""Builds driver_visors.h for the OpenXR VR addon's plugin: the visor tint of every driver helmet in the game's files,
by the helmet's name (its folder, eg 2026-ferrari_lewis_hamilton, which is also the name of the driver customisation
and the helmet row in the game's database).

Each helmet with a visor of its own has a visor material whose colour is a texture, eg
character_package/drivers/male/2025-ferrari_lewis_hamilton/textures/lewis_hamilton_visor_d.tif (BC1). This reads every
helmet archive under character_package/drivers, averages the visor texture's top mip and turns it into a tint: a
coloured visor tints the view its colour, a grey one darkens it by how dark it is. Helmets without one use the game's
generic (clear) visor and are left out. The plugin finds the player's helmet in the game's database at run time.

Usage: python make_driver_visors.py <Swofty tools dir with erp.py> <F1 25 folder> <output .h> [previews dir]
Needs the zstandard module (the archives are zstd-compressed).
"""

import colorsys
import importlib.util
import os
import struct
import sys

import zstandard


def load_erp(tools):
    spec = importlib.util.spec_from_file_location("erp", os.path.join(tools, "erp.py"))
    erp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(erp)
    return erp


def resource(erp, path, suffix):
    meta, res = erp.parse(path)
    for name, rtype, unk, frags, h in res:
        if name.endswith(suffix):
            out = []
            with open(path, "rb") as f:
                for a, o, size, fl, comp, packed in frags:
                    f.seek(meta["data_off"] + o)
                    raw = f.read(packed)
                    if comp == 17:
                        raw = zstandard.ZstdDecompressor().decompress(raw, max_output_size=size)
                    out.append(raw)
            return name, out
    return None, None


def bc1_pixels(data, w, h):
    """The top mip's pixels as (r, g, b) tuples, 8-bit sRGB."""
    out = []
    o = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            c0, c1, bits = struct.unpack_from("<HHI", data, o)
            o += 8

            def rgb(c):
                return ((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)

            a, b = rgb(c0), rgb(c1)
            if c0 > c1:
                pal = [a, b, tuple((2 * x + y) // 3 for x, y in zip(a, b)), tuple((x + 2 * y) // 3 for x, y in zip(a, b))]
            else:
                pal = [a, b, tuple((x + y) // 2 for x, y in zip(a, b)), None]
            for i in range(16):
                p = pal[bits >> (2 * i) & 3]
                if p is not None:
                    out.append(p)
    return out


def tint(mean):
    """(r, g, b, a) 8-bit sRGB with straight alpha, from the visor texture's mean sRGB colour."""
    h, s, v = colorsys.rgb_to_hsv(*(c / 255.0 for c in mean))
    if s < 0.25 or v < 0.15:
        # Grey or near black: clear to dark smoke by how dark the visor is
        return (0, 0, 0, round((0.08 + 0.50 * (1.0 - v)) * 255))
    # Coloured (iridium): its colour at full saturation, so the tint reads as that colour
    r, g, b = colorsys.hsv_to_rgb(h, min(1.0, s * 1.2), 0.85)
    return (round(r * 255), round(g * 255), round(b * 255), round(0.35 * 255))


def write_png(path, rgb_rows):
    import zlib
    h, w = len(rgb_rows), len(rgb_rows[0])
    raw = b"".join(b"\0" + bytes(c for p in row for c in p) for row in rgb_rows)
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def main(tools, game, out_path, previews=None):
    erp = load_erp(tools)
    root = os.path.join(game, "2025_asset_groups", "character_package", "drivers")
    rows = {}  # helmet name -> (rgba, mean)
    generic = 0
    for gender in sorted(os.listdir(root)):
        gdir = os.path.join(root, gender)
        if not os.path.isdir(gdir):
            continue
        for key in sorted(os.listdir(gdir)):
            kdir = os.path.join(gdir, key)
            if not os.path.isdir(kdir) or key in rows:
                continue
            helmet = sorted(f for f in os.listdir(kdir) if f.endswith("_helmet.erp"))
            if not helmet:
                continue
            try:
                name, frags = resource(erp, os.path.join(kdir, helmet[0]), "visor_d.tif.image")
            except Exception as e:  # an archive this reader doesn't know
                print(f"skipped {key}: {e}")
                continue
            if not frags or len(frags) < 2:
                generic += 1  # the game's generic visor: the plugin treats it as clear
                continue
            head = struct.unpack("<10I", frags[0][:40])
            w, h, fmt = head[3], head[4], head[2]
            if fmt != 54:
                print(f"skipped {key}: texture format {fmt}, not BC1")
                continue
            px = bc1_pixels(frags[1], w, h)
            if not px:
                continue
            mean = tuple(round(sum(p[i] for p in px) / len(px)) for i in range(3))
            rows[key] = (tint(mean), mean)
            if previews:
                os.makedirs(previews, exist_ok=True)
                write_png(os.path.join(previews, key + ".png"), [[mean] * 64] * 16)

    with open(out_path, "w", newline="\n") as f:
        f.write("/* Generated by make_driver_visors.py from the game's helmet visor textures. Do not edit. */\n")
        f.write("/* Sorted by helmet name (strcmp), for bsearch. */\n")
        f.write("typedef struct { const char* helmet; unsigned char rgba[4]; } DriverVisor;\n\n")
        f.write("static const DriverVisor driver_visors[] = {\n")
        for key in sorted(rows, key=lambda k: k.encode()):
            rgba, mean = rows[key]
            f.write(f"\t{{ \"{key}\", {{ {rgba[0]}, {rgba[1]}, {rgba[2]}, {rgba[3]} }} }}, /* visor {mean} */\n")
        f.write("};\n")
    print("wrote", out_path, len(rows), "visors;", generic, "helmets with the generic visor")


if __name__ == "__main__":
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    main(*sys.argv[1:])