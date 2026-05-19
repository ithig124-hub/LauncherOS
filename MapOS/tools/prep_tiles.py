#!/usr/bin/env python3
"""
prep_tiles.py  --  MapOS tile preparation CLI

Turns a lat/lon bounding box + zoom range into the exact folder structure that
MapOS.ino expects on the SD card:

    out/WATCH/MapOS/tiles/z{Z}/x{X}/y{Y}.raw     (256x256, RGB565 little-endian)
    out/WATCH/MapOS/me.txt                       ("lat,lon" -- you-are-here)
    out/WATCH/MapOS/breadcrumbs.csv              (created on watch by long-press)

It can either:
  (A) Download tiles from a slippy-map tile server (OSM by default), OR
  (B) Convert a folder of existing 256x256 PNGs to .raw in place.

USAGE
-----
  # 1. Download an OSM region for Paris from zoom 12 to 15, write to ./SD
  python3 prep_tiles.py download \
      --lat1 48.80 --lon1 2.25 --lat2 48.91 --lon2 2.42 \
      --zmin 12 --zmax 15 --out ./SD

  # 2. Convert a folder of PNG tiles you already have
  python3 prep_tiles.py convert --in ./png_tiles --out ./SD

  # 3. Set the "you are here" marker (writes /WATCH/map/me.txt)
  python3 prep_tiles.py me --lat 48.8566 --lon 2.3522 --out ./SD

After running, eject the SD card from the PC, plug it into the watch, boot
MapOS, and you'll see the tiles you just prepared.

DEPENDENCIES
------------
  pip install pillow requests

LICENCE NOTE
------------
OpenStreetMap tiles fall under the OSM tile usage policy. Be polite -- use a
real User-Agent (this script already sets one), rate-limit (this script
sleeps 100 ms between tiles by default), and don't download huge regions from
the public OSM servers. For anything bigger than a city, host your own tile
server (tegola / openmaptiles) or use a commercial provider.
"""
import argparse
import math
import os
import sys
import time
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("ERROR: Pillow is required.  pip install pillow")

try:
    import requests
except ImportError:
    requests = None  # only needed for the "download" subcommand


TILE_SZ = 256
DEFAULT_TILE_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
USER_AGENT = "MapOS-tile-prep/1.1 (offline ESP32 watch tool)"


# ----- maths ----------------------------------------------------------------
def lonlat_to_tile(lat: float, lon: float, z: int):
    n = 1 << z
    x = int((lon + 180.0) / 360.0 * n)
    lat_r = math.radians(lat)
    y = int((1.0 - math.log(math.tan(lat_r) + 1.0 / math.cos(lat_r)) / math.pi) / 2.0 * n)
    return x, y


def png_to_rgb565_raw(png_path: Path, raw_path: Path) -> bool:
    """Convert one 256x256 PNG to little-endian RGB565."""
    try:
        img = Image.open(png_path).convert("RGB")
    except Exception as e:
        print(f"  skip {png_path}: {e}")
        return False
    if img.size != (TILE_SZ, TILE_SZ):
        img = img.resize((TILE_SZ, TILE_SZ), Image.LANCZOS)
    raw = bytearray(TILE_SZ * TILE_SZ * 2)
    px = img.load()
    i = 0
    for y in range(TILE_SZ):
        for x in range(TILE_SZ):
            r, g, b = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            raw[i] = v & 0xFF
            raw[i + 1] = (v >> 8) & 0xFF
            i += 2
    raw_path.parent.mkdir(parents=True, exist_ok=True)
    raw_path.write_bytes(raw)
    return True


# ----- subcommands ----------------------------------------------------------
def cmd_download(args):
    if requests is None:
        sys.exit("ERROR: requests is required for download.  pip install requests")
    if not args.map:
        sys.exit("ERROR: --map <Name> is required (each map lives in its own folder).")
    map_root = Path(args.out) / "WATCH" / "MapOS" / "tiles" / args.map
    tiles_root = map_root
    tiles_root.mkdir(parents=True, exist_ok=True)
    # Write/refresh meta.txt with centre + bbox info
    clat = (args.lat1 + args.lat2) / 2
    clon = (args.lon1 + args.lon2) / 2
    default_zoom = (args.zmin + args.zmax) // 2
    meta_file = map_root / "meta.txt"
    if not meta_file.exists() or args.force:
        meta_file.write_text(
            f"TITLE={args.title or args.map}\n"
            f"DESC={args.desc or ''}\n"
            f"CENTER_LAT={clat:.6f}\n"
            f"CENTER_LON={clon:.6f}\n"
            f"DEFAULT_ZOOM={default_zoom}\n"
            f"MIN_ZOOM={args.zmin}\n"
            f"MAX_ZOOM={args.zmax}\n"
        )

    lat_top = max(args.lat1, args.lat2)
    lat_bot = min(args.lat1, args.lat2)
    lon_lft = min(args.lon1, args.lon2)
    lon_rgt = max(args.lon1, args.lon2)

    total_tiles = 0
    for z in range(args.zmin, args.zmax + 1):
        x1, y1 = lonlat_to_tile(lat_top, lon_lft, z)
        x2, y2 = lonlat_to_tile(lat_bot, lon_rgt, z)
        x_lo, x_hi = sorted((x1, x2))
        y_lo, y_hi = sorted((y1, y2))
        count = (x_hi - x_lo + 1) * (y_hi - y_lo + 1)
        total_tiles += count
        print(f"z={z}: {count} tiles  ({x_lo}-{x_hi} x  {y_lo}-{y_hi} y)")

    if total_tiles > 2000 and not args.yes:
        ans = input(f"\nThis will fetch {total_tiles} tiles. Continue? [y/N] ")
        if ans.strip().lower() != "y":
            return

    session = requests.Session()
    session.headers.update({"User-Agent": USER_AGENT})

    done = 0
    skipped = 0
    failed = 0
    for z in range(args.zmin, args.zmax + 1):
        x1, y1 = lonlat_to_tile(lat_top, lon_lft, z)
        x2, y2 = lonlat_to_tile(lat_bot, lon_rgt, z)
        x_lo, x_hi = sorted((x1, x2))
        y_lo, y_hi = sorted((y1, y2))
        for x in range(x_lo, x_hi + 1):
            for y in range(y_lo, y_hi + 1):
                raw_path = tiles_root / f"z{z}" / f"x{x}" / f"y{y}.raw"
                if raw_path.exists() and not args.force:
                    skipped += 1
                    continue
                url = args.url.format(z=z, x=x, y=y)
                try:
                    r = session.get(url, timeout=15)
                    if r.status_code != 200:
                        print(f"  HTTP {r.status_code} on {url}")
                        failed += 1
                        continue
                    # Save to a temp file then convert
                    tmp = raw_path.with_suffix(".png")
                    tmp.parent.mkdir(parents=True, exist_ok=True)
                    tmp.write_bytes(r.content)
                    if png_to_rgb565_raw(tmp, raw_path):
                        done += 1
                    tmp.unlink(missing_ok=True)
                except Exception as e:
                    print(f"  fail {url}: {e}")
                    failed += 1
                time.sleep(args.sleep)
                if done and done % 25 == 0:
                    print(f"  ... {done} tiles converted")

    print(f"\nDone. converted={done} skipped={skipped} failed={failed}")
    print(f"Tiles written under: {out_root}")
    print("Copy the SD folder contents to the root of your SD card and boot MapOS.")


def cmd_convert(args):
    if not args.map:
        sys.exit("ERROR: --map <Name> is required.")
    in_root = Path(args.input)
    out_root = Path(args.out) / "WATCH" / "MapOS" / "tiles" / args.map
    out_root.mkdir(parents=True, exist_ok=True)
    done = 0
    for png in in_root.rglob("*.png"):
        # Expect a layout of z/x/y.png; fall back to flat -> z=13
        parts = png.relative_to(in_root).parts
        if len(parts) == 3 and parts[0].startswith("z"):
            z_dir, x_dir, y_file = parts
            z = int(z_dir.lstrip("z"))
            x = int(x_dir.lstrip("x"))
            y = int(Path(y_file).stem.lstrip("y"))
        else:
            # last 3 path elements interpreted as z/x/y.png
            try:
                z = int(parts[-3].lstrip("z"))
                x = int(parts[-2].lstrip("x"))
                y = int(Path(parts[-1]).stem.lstrip("y"))
            except Exception:
                print(f"  unrecognised path: {png}  -- expected z<Z>/x<X>/y<Y>.png")
                continue
        raw_path = out_root / f"z{z}" / f"x{x}" / f"y{y}.raw"
        if png_to_rgb565_raw(png, raw_path):
            done += 1
    print(f"Converted {done} tiles into {out_root}")


def cmd_me(args):
    out_root = Path(args.out) / "WATCH" / "MapOS"
    out_root.mkdir(parents=True, exist_ok=True)
    (out_root / "me.txt").write_text(f"{args.lat:.6f},{args.lon:.6f}\n")
    print(f"me.txt written to {out_root/'me.txt'}")


# ----- argparse -------------------------------------------------------------
def build_parser():
    p = argparse.ArgumentParser(description="MapOS tile-prep CLI")
    sub = p.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("download", help="Download a bbox of slippy-map tiles into one map folder")
    d.add_argument("--map", required=True, help='Map folder name, eg "Paris" or "Yosemite"')
    d.add_argument("--title", default="", help="Display title (default = map name)")
    d.add_argument("--desc", default="", help="One-line description shown on the library card")
    d.add_argument("--lat1", type=float, required=True, help="bbox corner 1 lat")
    d.add_argument("--lon1", type=float, required=True, help="bbox corner 1 lon")
    d.add_argument("--lat2", type=float, required=True, help="bbox corner 2 lat")
    d.add_argument("--lon2", type=float, required=True, help="bbox corner 2 lon")
    d.add_argument("--zmin", type=int, default=12)
    d.add_argument("--zmax", type=int, default=15)
    d.add_argument("--out", default="./SD", help="Output folder; tiles land in <out>/WATCH/MapOS/tiles/<map>/")
    d.add_argument("--url", default=DEFAULT_TILE_URL,
                   help='Tile URL template (default: OSM). Use "{z}", "{x}", "{y}"')
    d.add_argument("--sleep", type=float, default=0.1, help="Seconds between requests")
    d.add_argument("--force", action="store_true", help="Re-download tiles + overwrite meta.txt")
    d.add_argument("-y", "--yes", action="store_true", help="Skip the >2000-tiles confirmation")
    d.set_defaults(func=cmd_download)

    c = sub.add_parser("convert", help="Convert a folder of PNG tiles into one map folder")
    c.add_argument("--map", required=True, help="Map folder name")
    c.add_argument("--in", dest="input", required=True, help="Folder containing z/x/y.png")
    c.add_argument("--out", default="./SD")
    c.set_defaults(func=cmd_convert)

    m = sub.add_parser("me", help="Write the 'you are here' file (me.txt)")
    m.add_argument("--lat", type=float, required=True)
    m.add_argument("--lon", type=float, required=True)
    m.add_argument("--out", default="./SD")
    m.set_defaults(func=cmd_me)

    return p


if __name__ == "__main__":
    args = build_parser().parse_args()
    args.func(args)
