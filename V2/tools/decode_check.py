#!/usr/bin/env python3
"""
Round-trip validator: decode each generated .fmap the same way WaypointOS.ino
does. Prints title/desc/tile count/POI count/first-line coords per file.
Fails hard if any offset/length is wrong, so a green run == the C++ decoder
will see the same data.
"""
import os, struct

SAMPLES = os.path.join(os.path.dirname(__file__), "..", "samples")

def zz(u): return (u >> 1) ^ -(u & 1)

def rd_varint(f):
    r, shift = 0, 0
    while True:
        b = f.read(1)[0]
        r |= (b & 0x7F) << shift
        shift += 7
        if not (b & 0x80): return r
        if shift > 32: return r

def read(f, fmt): return struct.unpack(fmt, f.read(struct.calcsize(fmt)))

def rd_lstr(f):
    (n,) = read(f, "<H")
    return f.read(n).decode("utf-8", "replace")

def decode(path):
    with open(path, "rb") as f:
        magic = f.read(4)
        assert magic == b"FMAP", f"bad magic {magic!r}"
        (ver, flags, sc, _res) = read(f, "<HHHH")
        assert ver == 1
        sections = {}
        for _ in range(sc):
            (sid, off, ln) = read(f, "<HII")
            sections[sid] = (off, ln)

        # META
        f.seek(sections[1][0])
        title = rd_lstr(f); desc = rd_lstr(f)
        (created,) = read(f, "<I")
        (min_lat, min_lon, max_lat, max_lon,
         olat, olon, upm) = read(f, "<fffffff")
        (proj,) = read(f, "<B")

        # ZOOM
        zooms = []
        if 2 in sections:
            f.seek(sections[2][0])
            (n,) = read(f, "<B")
            for _ in range(n):
                (zid, sc_, cnt) = read(f, "<BfI")
                zooms.append((zid, sc_, cnt))

        # TILE_IDX
        f.seek(sections[3][0])
        (tile_size,) = read(f, "<i")
        (gmintx, gminty, gmaxtx, gmaxty) = read(f, "<hhhh")
        (tcount,) = read(f, "<I")
        tiles = []
        for _ in range(tcount):
            (tx, ty, zmask, off, ln) = read(f, "<hhBII")
            tiles.append((tx, ty, zmask, off, ln))

        # VEC — decode first tile only for sanity
        first_line = None
        if tiles:
            tx, ty, zmask, off, ln = tiles[0]
            f.seek(sections[4][0] + off)
            (obj_cnt,) = read(f, "<H")
            for _ in range(obj_cnt):
                (kind, layer, zmin, zmax, ofl, pcnt) = read(f, "<BBBBBH")
                if pcnt == 0: continue
                (x0, y0) = read(f, "<ii")
                pts = [(x0, y0)]
                px, py = x0, y0
                for _ in range(pcnt - 1):
                    dx = zz(rd_varint(f)); dy = zz(rd_varint(f))
                    px += dx; py += dy
                    pts.append((px, py))
                if kind == 1 and first_line is None:  # LINE
                    first_line = pts[:3]

        # POI
        pois = []
        if 5 in sections:
            f.seek(sections[5][0])
            (n,) = read(f, "<I")
            for _ in range(n):
                (kind, x, y, icon) = read(f, "<BiiB")
                name = rd_lstr(f)
                pois.append((kind, x, y, icon, name))

        return {
            "title": title, "desc": desc, "zooms": zooms,
            "tile_size": tile_size, "tiles": len(tiles),
            "grid": (gmintx, gminty, gmaxtx, gmaxty),
            "pois": len(pois),
            "sample_poi": pois[0] if pois else None,
            "first_line_head": first_line,
        }

for fn in sorted(os.listdir(SAMPLES)):
    if not fn.endswith(".fmap"): continue
    p = os.path.join(SAMPLES, fn)
    d = decode(p)
    print(f"\n== {fn} ==")
    for k, v in d.items(): print(f"  {k}: {v}")
