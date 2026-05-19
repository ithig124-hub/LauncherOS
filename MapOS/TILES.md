# How to get tiles for MapOS

MapOS reads its map straight from the SD card. There is no GPU and no
internet on the device, so all the work happens on a PC beforehand. This
guide covers every realistic way to obtain tiles and feed them to MapOS.

> **SD-card layout (every file MapOS uses)**
> ```
> /WATCH/MapOS/tiles/z<Z>/x<X>/y<Y>.raw    256x256 RGB565 little-endian (~128 KB each)
> /WATCH/MapOS/me.txt                       "lat,lon" line for the red "you are here" dot
> /WATCH/MapOS/breadcrumbs.csv              auto-created by MapOS on long-press
> ```
> Everything lives under `WATCH/MapOS/` so it does not collide with BookOS,
> WiFiOS, or any other LauncherOS app.

---

## 1. Fastest path: bbox download from OpenStreetMap

```bash
python3 -m pip install pillow requests

cd MapOS_v1.1/tools

python3 prep_tiles.py download \
    --lat1 48.80 --lon1 2.25 \
    --lat2 48.91 --lon2 2.42 \
    --zmin 13 --zmax 15 \
    --out ./SD
```

What the script does, end-to-end:

1. Walks every tile `(z, x, y)` inside the bbox at every zoom in `[zmin, zmax]`.
2. Skips tiles that already exist on disk (re-runs are incremental).
3. Downloads each tile as a 256x256 PNG with a polite `User-Agent`.
4. Converts to RGB565 little-endian (132 144 bytes per tile, exactly).
5. Saves to `./SD/WATCH/MapOS/tiles/zZ/xX/yY.raw`.
6. Sleeps 100 ms between requests (max 10 req/s, OSM-friendly).
7. Writes a default `me.txt` at the centre of the bbox.

Then copy `./SD/WATCH` to the root of your SD card so the watch sees
`/WATCH/`.

### Picking a sensible bbox + zoom range

Each tile is 256x256 px x 2 bytes = **132 144 bytes** (~128 KB).
Tile count grows by 4x per extra zoom level.

| What you cover                          | Zooms   | Tiles   | Disk      |
|-----------------------------------------|---------|---------|-----------|
| One neighbourhood (~3 km box)           | 14-16   | ~250    | ~32 MB    |
| Paris bbox above                        | 13-15   | ~900    | ~110 MB   |
| Whole country, low detail only          | 6-9     | ~1500   | ~190 MB   |
| Country wide + city detail in 1-2 cities| 6-15    | ~6000   | ~750 MB   |

A 16 GB card holds far more than you need. The bottleneck is download time,
not space: at 10 tiles/s, 1000 tiles = 100 s, 10000 tiles = ~17 minutes.

### Other useful flags

| Flag         | What it does                                                     |
|--------------|------------------------------------------------------------------|
| `--url`      | Slippy URL template, default OSM. Use any `{z}/{x}/{y}` provider |
| `--sleep`    | Seconds between requests (default 0.1 = 10 req/s)                |
| `--force`    | Re-download tiles that already exist                             |
| `-y`         | Skip the "more than 2000 tiles, continue?" prompt                |

---

## 2. Bring your own PNG tiles

If you already have tiles (Maperitive export, JOSM, QGIS Atlas, your own
tile server, a Mapbox/MapTiler download, your friend's hard drive):

```bash
python3 prep_tiles.py convert --in ./my_png_tiles --out ./SD
```

Layout `my_png_tiles` so each file is at `z<Z>/x<X>/y<Y>.png`. The script
walks recursively, converts every PNG (resizing to 256x256 if needed) and
writes the matching `.raw` to `./SD/WATCH/MapOS/tiles/`.

---

## 3. Use a different tile provider

The script accepts any slippy `{z}/{x}/{y}` URL through `--url`. Some
real-world examples:

**Stadia Maps** (free tier, sign up at stadiamaps.com):
```
--url "https://tiles.stadiamaps.com/tiles/alidade_smooth/{z}/{x}/{y}.png?api_key=YOUR_KEY"
```

**MapTiler** (free tier, sign up at maptiler.com):
```
--url "https://api.maptiler.com/maps/streets-v2/256/{z}/{x}/{y}.png?key=YOUR_KEY"
```

**Thunderforest** (free dev tier):
```
--url "https://tile.thunderforest.com/transport/{z}/{x}/{y}.png?apikey=YOUR_KEY"
```

**Your own tegola / tileserver-gl / openmaptiles instance**:
```
--url "http://192.168.1.10:8080/styles/osm-bright/256/{z}/{x}/{y}.png"
```

For anything bigger than a city, use a commercial provider or self-host.
Public OSM tile servers are **not** a free CDN.

---

## 4. "You are here" - `me.txt`

The watch has no GPS, so the red dot is placed by reading
`/WATCH/MapOS/me.txt`:

```
48.8566,2.3522
```

Two decimal places of lat/lon is roughly a kilometre, six is roughly 10 cm.
Edit it by hand any time; MapOS reloads it on boot.

To write/overwrite it with the CLI:

```bash
python3 prep_tiles.py me --lat 48.8566 --lon 2.3522 --out ./SD
```

If `me.txt` is missing, MapOS opens at `(0, 0)` and skips drawing the red
dot. The breadcrumb action also requires `me.txt` (it records the *me*
lat/lon, since there's no real fix to record).

---

## 5. Breadcrumbs - `breadcrumbs.csv`

When you long-press anywhere on the screen for ~1 second (without dragging),
MapOS appends a line like:

```
1715869123,48.856600,2.352200
```

to `/WATCH/MapOS/breadcrumbs.csv`. The format is `unix_ts,lat,lon`. Pull
the SD card out and open it in any spreadsheet, or convert to GPX with
`gpsbabel`:

```bash
gpsbabel -i unicsv -f breadcrumbs.csv -o gpx -F route.gpx
```

---

## 6. Pre-canned examples

See `examples/paris.sh` in the zip for a complete one-liner that downloads
central Paris at zoom 13-15 and writes the right `me.txt`. Copy it, change
the bbox, you have a new city.

---

## 7. Troubleshooting tile rendering

If you boot MapOS and see solid grey squares with thin outlines, **MapOS
loaded fine but couldn't find a tile** for that `(z, x, y)`. Either:

- the zoom you're on isn't represented (try a different one with BOOT tap),
- the bbox you downloaded didn't include the lat/lon you're centred on
  (check `me.txt`), or
- the folder ended up at `/WATCH/map/` (old layout) instead of
  `/WATCH/MapOS/tiles/` - move it.

If the screen renders coloured noise instead of map, you have the wrong
pixel format. Re-run the CLI; it always writes RGB565 little-endian.

If a single tile fails to download (HTTP 404, 503), the script prints a
line and continues. Re-run later; existing tiles are skipped, only the
missing ones are fetched.
