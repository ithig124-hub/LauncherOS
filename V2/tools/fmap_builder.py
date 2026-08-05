#!/usr/bin/env python3
"""
fmap_builder.py — reference writer for the WaypointOS `.fmap` v1 binary format.

Little-endian, matches the decoder in WaypointOS.ino exactly.

Usage:
    from fmap_builder import FMap
    m = FMap(title="Byford", desc="...", bounds=(-32.24,116.05,-32.19,116.10))
    m.add_zoom(1, 5.0); m.add_zoom(2, 2.0); m.add_zoom(3, 0.8); m.add_zoom(4, 0.25)
    m.tile_size(1000)
    m.add_line(layer=LAY_ROADS, pts=[(0,0),(500,120),(900,-80)], zmin=1, zmax=4, width=3)
    m.add_poly(layer=LAY_PARKS, pts=[(-400,-200),(-400,200),(-100,200),(-100,-200)])
    m.add_poi(kind=POI_LANDMARK, x=200, y=200, name="Town Hall")
    m.set_style(LAY_ROADS, colour=0xC618, width=3)
    m.write("Byford.fmap")

The world is a local metric grid (units per metre = 1.0 by default). Bounds in
lat/lon are stored as metadata so a future GPS layer can align. All object
coords are int32 world units.
"""

from __future__ import annotations
import struct, io, time
from dataclasses import dataclass, field
from typing import List, Tuple, Dict, Iterable

# Layer IDs — must match WaypointOS.ino
LAY_ROADS, LAY_BUILDINGS, LAY_PARKS, LAY_WATER, LAY_PATHS, \
LAY_SCHOOLS, LAY_SHOPS, LAY_TRANSIT, LAY_BEACONS, LAY_SAVED, LAY_BREADCRUMBS = range(11)

# POI type IDs
POI_GENERIC, POI_SHOP, POI_FOOD, POI_TRANSIT, POI_SCHOOL, \
POI_PARK, POI_LANDMARK, POI_SERVICE = range(8)

# Object types
OBJ_LINE, OBJ_POLY, OBJ_POINT = 1, 2, 3

# Section IDs
SEC_META, SEC_ZOOM, SEC_TILE_IDX, SEC_VEC, SEC_POI, SEC_SEARCH, SEC_STYLE = 1,2,3,4,5,6,7


def _zz(n: int) -> int:
    """signed 32-bit -> unsigned 32-bit zig-zag encode"""
    return ((n << 1) ^ (n >> 31)) & 0xFFFFFFFF

def _varint(v: int) -> bytes:
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)

def _lstr(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("<H", len(b)) + b


@dataclass
class _Obj:
    kind: int
    layer: int
    zmin: int
    zmax: int
    flags: int
    pts: List[Tuple[int, int]]

@dataclass
class _POI:
    kind: int
    x: int
    y: int
    icon: int
    name: str

@dataclass
class _Style:
    layer: int
    colour: int
    width: int = 1
    dash: int = 0
    z_min: int = 1


class FMap:
    def __init__(self, title: str, desc: str,
                 bounds: Tuple[float, float, float, float]=(0,0,0,0),
                 origin: Tuple[float, float]=(0.0, 0.0),
                 units_per_metre: float = 1.0):
        self.title = title
        self.desc = desc
        self.min_lat, self.min_lon, self.max_lat, self.max_lon = bounds
        self.origin_lat, self.origin_lon = origin
        self.units_per_metre = units_per_metre
        self.projection = 0
        self.created = int(time.time())
        self.zooms: List[Tuple[int, float]] = []
        self._tile_size = 1000
        self._objects: List[_Obj] = []
        self._pois: List[_POI] = []
        self._styles: Dict[int, _Style] = {}

    # ---- geometry helpers ----------------------------------------------------
    def tile_size(self, size: int) -> "FMap":
        self._tile_size = int(size); return self

    def add_zoom(self, zoom_id: int, min_px_scale: float) -> "FMap":
        self.zooms.append((int(zoom_id), float(min_px_scale)))
        return self

    def add_line(self, *, layer: int, pts: Iterable[Tuple[int,int]],
                 zmin: int = 1, zmax: int = 4, flags: int = 0) -> None:
        self._objects.append(_Obj(OBJ_LINE, layer, zmin, zmax, flags, [(int(x), int(y)) for x, y in pts]))

    def add_poly(self, *, layer: int, pts: Iterable[Tuple[int,int]],
                 zmin: int = 1, zmax: int = 4, filled: bool = True) -> None:
        flags = 0x01 if filled else 0
        self._objects.append(_Obj(OBJ_POLY, layer, zmin, zmax, flags, [(int(x), int(y)) for x, y in pts]))

    def add_point(self, *, layer: int, x: int, y: int,
                  zmin: int = 1, zmax: int = 4) -> None:
        self._objects.append(_Obj(OBJ_POINT, layer, zmin, zmax, 0, [(int(x), int(y))]))

    def add_poi(self, *, kind: int, x: int, y: int, name: str, icon: int = 0) -> None:
        self._pois.append(_POI(kind, int(x), int(y), icon, name))

    def set_style(self, layer: int, *, colour: int, width: int = 1, dash: int = 0, z_min: int = 1) -> None:
        self._styles[layer] = _Style(layer, colour, width, dash, z_min)

    # ---- write ---------------------------------------------------------------
    def _encode_meta(self) -> bytes:
        b = io.BytesIO()
        b.write(_lstr(self.title))
        b.write(_lstr(self.desc))
        b.write(struct.pack("<I", self.created))
        b.write(struct.pack("<f", self.min_lat))
        b.write(struct.pack("<f", self.min_lon))
        b.write(struct.pack("<f", self.max_lat))
        b.write(struct.pack("<f", self.max_lon))
        b.write(struct.pack("<f", self.origin_lat))
        b.write(struct.pack("<f", self.origin_lon))
        b.write(struct.pack("<f", self.units_per_metre))
        b.write(struct.pack("<B", self.projection))
        return b.getvalue()

    def _encode_zoom(self) -> bytes:
        b = io.BytesIO()
        b.write(struct.pack("<B", len(self.zooms)))
        # count objects visible at each zoom
        for zid, sc in self.zooms:
            cnt = sum(1 for o in self._objects if o.zmin <= zid <= o.zmax)
            b.write(struct.pack("<BfI", zid, sc, cnt))
        return b.getvalue()

    def _encode_object(self, o: _Obj) -> bytes:
        b = io.BytesIO()
        b.write(struct.pack("<BBBBBH", o.kind, o.layer, o.zmin, o.zmax, o.flags, len(o.pts)))
        if not o.pts:
            return b.getvalue()
        x0, y0 = o.pts[0]
        b.write(struct.pack("<ii", x0, y0))
        px, py = x0, y0
        for x, y in o.pts[1:]:
            b.write(_varint(_zz(x - px)))
            b.write(_varint(_zz(y - py)))
            px, py = x, y
        return b.getvalue()

    def _bucket_tiles(self) -> Dict[Tuple[int,int], List[_Obj]]:
        """Assign each object to the tile containing its first point."""
        buckets: Dict[Tuple[int,int], List[_Obj]] = {}
        for o in self._objects:
            if not o.pts: continue
            x, y = o.pts[0]
            tx = x // self._tile_size
            ty = y // self._tile_size
            buckets.setdefault((tx, ty), []).append(o)
        return buckets

    def _encode_tile_idx_and_vec(self):
        buckets = self._bucket_tiles()
        tile_coords = sorted(buckets.keys())
        # Compute grid extents
        if tile_coords:
            xs = [tx for tx,_ in tile_coords]
            ys = [ty for _,ty in tile_coords]
            gmin_tx, gmax_tx = min(xs), max(xs)
            gmin_ty, gmax_ty = min(ys), max(ys)
        else:
            gmin_tx = gmin_ty = gmax_tx = gmax_ty = 0

        # Build VEC section with per-tile offsets (relative to VEC start)
        vec_buf = io.BytesIO()
        tile_records = []
        for (tx, ty) in tile_coords:
            objs = buckets[(tx, ty)]
            offset = vec_buf.tell()
            vec_buf.write(struct.pack("<H", len(objs)))
            zmask = 0
            for o in objs:
                for z in range(o.zmin, o.zmax + 1):
                    if 1 <= z <= 8:
                        zmask |= (1 << (z - 1))
                vec_buf.write(self._encode_object(o))
            length = vec_buf.tell() - offset
            tile_records.append((tx, ty, zmask, offset, length))

        # Build TILE_IDX section
        idx_buf = io.BytesIO()
        idx_buf.write(struct.pack("<i", self._tile_size))
        idx_buf.write(struct.pack("<hhhh", gmin_tx, gmin_ty, gmax_tx, gmax_ty))
        idx_buf.write(struct.pack("<I", len(tile_records)))
        for tx, ty, zmask, offset, length in tile_records:
            idx_buf.write(struct.pack("<hhBII", tx, ty, zmask, offset, length))

        return idx_buf.getvalue(), vec_buf.getvalue()

    def _encode_poi(self) -> bytes:
        b = io.BytesIO()
        b.write(struct.pack("<I", len(self._pois)))
        for p in self._pois:
            b.write(struct.pack("<BiiB", p.kind, p.x, p.y, p.icon))
            b.write(_lstr(p.name))
        return b.getvalue()

    def _encode_style(self) -> bytes:
        b = io.BytesIO()
        b.write(struct.pack("<B", len(self._styles)))
        for layer, s in self._styles.items():
            b.write(struct.pack("<BHBBB", s.layer, s.colour, s.width, s.dash, s.z_min))
        return b.getvalue()

    def write(self, path: str) -> None:
        # Encode each section body first
        meta = self._encode_meta()
        zoom = self._encode_zoom() if self.zooms else b""
        tidx, vec = self._encode_tile_idx_and_vec()
        poi = self._encode_poi() if self._pois else b""
        style = self._encode_style() if self._styles else b""

        sections: List[Tuple[int, bytes]] = []
        sections.append((SEC_META, meta))
        if zoom: sections.append((SEC_ZOOM, zoom))
        sections.append((SEC_TILE_IDX, tidx))
        sections.append((SEC_VEC, vec))
        if poi: sections.append((SEC_POI, poi))
        if style: sections.append((SEC_STYLE, style))

        # Header (12 bytes) + section table entries (10 bytes each) + payloads
        header_size = 4 + 2 + 2 + 2 + 2         # magic, ver, flags, sc, reserved
        entry_size = 2 + 4 + 4                  # id, offset, length
        table_size = entry_size * len(sections)
        payload_offset = header_size + table_size

        # Assign offsets sequentially
        offsets: List[Tuple[int, int, int]] = []
        cursor = payload_offset
        for sid, body in sections:
            offsets.append((sid, cursor, len(body)))
            cursor += len(body)

        flags = 0
        if poi: flags |= 0x02
        if style: flags |= 0x08

        with open(path, "wb") as f:
            f.write(b"FMAP")
            f.write(struct.pack("<H", 1))            # version
            f.write(struct.pack("<H", flags))
            f.write(struct.pack("<H", len(sections)))
            f.write(struct.pack("<H", 0))            # reserved
            for sid, off, length in offsets:
                f.write(struct.pack("<H", sid))
                f.write(struct.pack("<I", off))
                f.write(struct.pack("<I", length))
            for _, body in sections:
                f.write(body)
        print(f"wrote {path}: {cursor} bytes, {len(self._objects)} objects, "
              f"{len(self._pois)} POIs, {len(sections)} sections")
