"""Builds driver_visors.h for the OpenXR VR addon's plugin: the visor tint of each stock F1 driver's helmet, by the
game's driver id (stat_id, the id telemetry reports) and season.

Each driver's helmet has its own visor material whose colour is a texture, eg
character_package/drivers/male/2025-ferrari_lewis_hamilton/textures/lewis_hamilton_visor_d.tif (BC1). This reads
every <season>-<team>_<driver> helmet archive of the F1 teams for 2024-2026, averages the visor texture's top mip and
turns it into a tint: a coloured visor tints the view its colour, a grey one darkens it by how dark it is.

Usage: python make_driver_visors.py <Swofty tools dir with erp.py> <F1 25 folder> <swofty.exe> <output .h> [previews dir]
Needs the zstandard module (the archives are zstd-compressed).
"""

import colorsys
import importlib.util
import os
import re
import struct
import subprocess
import sys

import zstandard

SEASONS = ("2024", "2025", "2026")
# Base team liveries; special ones (eg ferrarimonza, williamsgulf) are left out
TEAMS = {"ferrari", "mercedes", "redbull", "mclaren", "williams", "forceindia", "renault", "haas", "sauber", "tororosso",
         "cadillac"}


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


def to_linear(c):
    c /= 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def tint(mean):
    """(r, g, b, a) 8-bit sRGB with straight alpha, from the visor texture's mean sRGB colour."""
    h, s, v = colorsys.rgb_to_hsv(*(c / 255.0 for c in mean))
    if s < 0.25 or v < 0.15:
        # Grey or near black: clear to dark smoke by how dark the visor is
        return (0, 0, 0, round((0.08 + 0.50 * (1.0 - v)) * 255))
    # Coloured (iridium): its colour at full saturation, so the tint reads as that colour
    r, g, b = colorsys.hsv_to_rgb(h, min(1.0, s * 1.2), 0.85)
    return (round(r * 255), round(g * 255), round(b * 255), round(0.35 * 255))


def stat_id(swofty, label):
    out = subprocess.run([swofty, "db", "get", f"entrants.drivers.driver[_metadata_={label}]"], capture_output=True, text=True)
    m = re.search(r"^\s*stat_id\s+(\d+)\s*$", out.stdout, re.M)
    return int(m.group(1)) if m else None


def write_png(path, rgb_rows):
    import zlib
    h, w = len(rgb_rows), len(rgb_rows[0])
    raw = b"".join(b"\0" + bytes(c for p in row for c in p) for row in rgb_rows)
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def main(tools, game, swofty, out_path, previews=None):
    erp = load_erp(tools)
    root = os.path.join(game, "2025_asset_groups", "character_package", "drivers")
    rows = []  # (stat_id, season, rgba, key, mean)
    ids = {}
    for gender in sorted(os.listdir(root)):
        gdir = os.path.join(root, gender)
        if not os.path.isdir(gdir):
            continue
        for key in sorted(os.listdir(gdir)):
            m = re.fullmatch(r"(\d{4})-([a-z]+)_(.+)", key)
            if not m or m.group(1) not in SEASONS or m.group(2) not in TEAMS:
                continue
            season, team, label = m.groups()
            helmet = [f for f in os.listdir(os.path.join(gdir, key)) if f.endswith("_helmet.erp")]
            if not helmet:
                continue
            name, frags = resource(erp, os.path.join(gdir, key, helmet[0]), "visor_d.tif.image")
            if not frags:
                # No visor material of its own: the game's generic visor, which the plugin treats as clear
                print("no visor texture (generic visor):", key)
                continue
            head = struct.unpack("<10I", frags[0][:40])
            w, h, fmt = head[3], head[4], head[2]
            if fmt != 54:
                print(f"skipped {key}: texture format {fmt}, not BC1")
                continue
            px = bc1_pixels(frags[1], w, h)
            mean = tuple(round(sum(p[i] for p in px) / len(px)) for i in range(3))
            if label not in ids:
                ids[label] = stat_id(swofty, label)
            if ids[label] is None:
                print("no driver row:", label)
                continue
            rgba = tint(mean)
            rows.append((ids[label], int(season), rgba, key, mean))
            print(f"{key:45} id {ids[label]:4} mean {mean} -> tint {rgba}")
            if previews:
                os.makedirs(previews, exist_ok=True)
                write_png(os.path.join(previews, key + ".png"), [[mean] * 64] * 16)

    rows.sort()
    with open(out_path, "w", newline="\n") as f:
        f.write("/* Generated by make_driver_visors.py from the game's helmet visor textures. Do not edit. */\n")
        f.write("typedef struct { unsigned short driver; unsigned short season; unsigned char rgba[4]; } DriverVisor;\n\n")
        f.write("static const DriverVisor driver_visors[] = {\n")
        for sid, season, rgba, key, mean in rows:
            f.write(f"\t{{ {sid}, {season}, {{ {rgba[0]}, {rgba[1]}, {rgba[2]}, {rgba[3]} }} }}, /* {key}, visor {mean} */\n")
        f.write("};\n")
    print("wrote", out_path, len(rows), "visors")


if __name__ == "__main__":
    if len(sys.argv) not in (5, 6):
        sys.exit(__doc__)
    main(*sys.argv[1:])
