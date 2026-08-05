#!/usr/bin/env python3
"""
Generate the three sample .fmap files (Byford, Perth, School).

Run:
    cd /app/WaypointOS/tools
    python3 make_samples.py
Output goes to /app/WaypointOS/samples/ .
Copy those to the SD card at /WATCH/WayPoint/ .
"""

import os, math, random
from fmap_builder import (
    FMap,
    LAY_ROADS, LAY_BUILDINGS, LAY_PARKS, LAY_WATER, LAY_PATHS,
    LAY_SCHOOLS, LAY_SHOPS, LAY_TRANSIT, LAY_BEACONS,
    POI_GENERIC, POI_SHOP, POI_FOOD, POI_TRANSIT, POI_SCHOOL,
    POI_PARK, POI_LANDMARK, POI_SERVICE,
)

OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "samples")
os.makedirs(OUT_DIR, exist_ok=True)


def default_styles(m: FMap):
    # RGB565 colours matching Fusion Dark defaults in WaypointOS.ino
    m.set_style(LAY_ROADS,     colour=0xC618, width=3)
    m.set_style(LAY_BUILDINGS, colour=0x39C7, width=1)
    m.set_style(LAY_PARKS,     colour=0x0500, width=1)
    m.set_style(LAY_WATER,     colour=0x0293, width=1)
    m.set_style(LAY_PATHS,     colour=0xBDF7, width=1, dash=4)
    m.set_style(LAY_SCHOOLS,   colour=0xFA20, width=1)
    m.set_style(LAY_SHOPS,     colour=0xF813, width=1)
    m.set_style(LAY_TRANSIT,   colour=0x03FF, width=2)


def default_zooms(m: FMap):
    # units-per-pixel thresholds: higher = zoomed out
    m.add_zoom(1, 8.0)   # overview
    m.add_zoom(2, 3.0)
    m.add_zoom(3, 1.0)
    m.add_zoom(4, 0.3)   # detailed


# -----------------------------------------------------------------------------
# School — tiny campus (~200x200 units)
# -----------------------------------------------------------------------------
def build_school():
    m = FMap(
        title="School",
        desc="Byford Secondary — main campus, paths and buildings.",
        bounds=(-32.220, 116.070, -32.215, 116.078),
        origin=(-32.2175, 116.074),
        units_per_metre=1.0,
    )
    m.tile_size(200)
    default_zooms(m)

    # School boundary polygon
    m.add_poly(layer=LAY_SCHOOLS, pts=[(-180,-180),(180,-180),(180,180),(-180,180)],
               zmin=1, zmax=4)

    # Main building (H shape via two rects)
    m.add_poly(layer=LAY_BUILDINGS, pts=[(-100,-40),(-40,-40),(-40,40),(-100,40)], zmin=2, zmax=4)
    m.add_poly(layer=LAY_BUILDINGS, pts=[(40,-40),(100,-40),(100,40),(40,40)], zmin=2, zmax=4)
    m.add_poly(layer=LAY_BUILDINGS, pts=[(-40,-10),(40,-10),(40,10),(-40,10)], zmin=2, zmax=4)

    # Sports building
    m.add_poly(layer=LAY_BUILDINGS, pts=[(-160,80),(-100,80),(-100,140),(-160,140)], zmin=2, zmax=4)

    # Oval (park layer as green)
    m.add_poly(layer=LAY_PARKS, pts=[(70,80),(160,80),(160,160),(70,160)], zmin=1, zmax=4)

    # Paths
    m.add_line(layer=LAY_PATHS, pts=[(-180,0),(-100,0)], zmin=2, zmax=4)
    m.add_line(layer=LAY_PATHS, pts=[(100,0),(180,0)], zmin=2, zmax=4)
    m.add_line(layer=LAY_PATHS, pts=[(0,50),(0,180)], zmin=2, zmax=4)
    m.add_line(layer=LAY_PATHS, pts=[(-130,80),(-130,140),(0,140)], zmin=2, zmax=4)

    # POIs
    m.add_poi(kind=POI_SCHOOL,   x=0, y=0, name="Main Office", icon=1)
    m.add_poi(kind=POI_LANDMARK, x=-130, y=110, name="Gymnasium", icon=2)
    m.add_poi(kind=POI_PARK,     x=115, y=120, name="Oval", icon=3)
    m.add_poi(kind=POI_SERVICE,  x=0, y=-160, name="Bike Racks", icon=4)

    default_styles(m)
    m.write(os.path.join(OUT_DIR, "School.fmap"))


# -----------------------------------------------------------------------------
# Byford — suburb style (2 km across, tile_size=500, ~4x4 tiles)
# -----------------------------------------------------------------------------
def build_byford():
    m = FMap(
        title="Byford",
        desc="Small town south of Perth — arterial roads, parks and shops.",
        bounds=(-32.240, 116.050, -32.190, 116.110),
        origin=(-32.215, 116.080),
        units_per_metre=1.0,
    )
    m.tile_size(500)
    default_zooms(m)

    # South Western Highway — long north-south arterial with jitter
    hwy = [(x, int(30 * math.sin(x/300))) for x in range(-1000, 1001, 50)]
    m.add_line(layer=LAY_ROADS, pts=hwy, zmin=1, zmax=4)

    # Abernethy Rd — east-west
    m.add_line(layer=LAY_ROADS,
               pts=[(-1000, 300),(-500, 320),(0, 340),(500, 330),(1000, 320)],
               zmin=1, zmax=4)

    # Residential road grid on the east side
    for gy in range(-800, 801, 200):
        m.add_line(layer=LAY_ROADS, pts=[(100, gy),(900, gy)], zmin=2, zmax=4)
    for gx in range(100, 901, 200):
        m.add_line(layer=LAY_ROADS, pts=[(gx, -800),(gx, 800)], zmin=2, zmax=4)

    # Suburban houses (small squares)
    rng = random.Random(42)
    for _ in range(60):
        cx = rng.randint(150, 850)
        cy = rng.randint(-750, 750)
        sz = rng.randint(18, 32)
        m.add_poly(layer=LAY_BUILDINGS,
                   pts=[(cx-sz,cy-sz),(cx+sz,cy-sz),(cx+sz,cy+sz),(cx-sz,cy+sz)],
                   zmin=3, zmax=4)

    # Shopping centre near town square
    m.add_poly(layer=LAY_BUILDINGS, pts=[(-60,-60),(90,-60),(90,80),(-60,80)],
               zmin=2, zmax=4)

    # Byford oval / park
    m.add_poly(layer=LAY_PARKS,
               pts=[(-800,-400),(-400,-400),(-400,-100),(-800,-100)],
               zmin=1, zmax=4)
    # Second park
    m.add_poly(layer=LAY_PARKS,
               pts=[(400, 500),(700, 500),(700, 700),(400, 700)],
               zmin=1, zmax=4)

    # Serpentine River — winding water polygon (thin)
    river = [(-1000,-600),(-800,-540),(-500,-580),(-200,-520),(100,-600),(500,-560),(1000,-620),
             (1000,-660),(500,-600),(100,-640),(-200,-560),(-500,-620),(-800,-580),(-1000,-640)]
    m.add_poly(layer=LAY_WATER, pts=river, zmin=1, zmax=4)

    # Rail line (transit) north-south
    m.add_line(layer=LAY_TRANSIT, pts=[(-950,-800),(-950,800)], zmin=1, zmax=4)

    # Walking paths through parks
    m.add_line(layer=LAY_PATHS, pts=[(-800,-250),(-400,-250)], zmin=3, zmax=4)
    m.add_line(layer=LAY_PATHS, pts=[(-600,-400),(-600,-100)], zmin=3, zmax=4)

    # POIs
    pois = [
        (POI_LANDMARK,  0,    0,   "Byford Town Square"),
        (POI_SHOP,     30,  -20,   "IGA Supermarket"),
        (POI_FOOD,     60,   40,   "Willow Cafe"),
        (POI_FOOD,    -30,   60,   "The Country Kitchen"),
        (POI_SCHOOL, -300, -600,   "Byford Primary"),
        (POI_SCHOOL,  400, -600,   "Byford Secondary"),
        (POI_TRANSIT,-950,  200,   "Byford Rail Halt"),
        (POI_PARK,   -600, -250,   "Doley Park"),
        (POI_PARK,    550,  600,   "Menzies Reserve"),
        (POI_SERVICE, 100,  100,   "Post Office"),
        (POI_SERVICE, 150,  120,   "Byford Medical"),
        (POI_SHOP,    120,   80,   "Hardware Store"),
        (POI_FOOD,    150,   40,   "Byford Bakery"),
        (POI_SHOP,     90,   60,   "Newsagent"),
        (POI_LANDMARK,-700, -300,  "Rotary Rotunda"),
    ]
    for kind, x, y, name in pois:
        m.add_poi(kind=kind, x=x, y=y, name=name)

    default_styles(m)
    m.write(os.path.join(OUT_DIR, "Byford.fmap"))


# -----------------------------------------------------------------------------
# Perth — city style (denser grid, river, transit, more POIs)
# -----------------------------------------------------------------------------
def build_perth():
    m = FMap(
        title="Perth",
        desc="Perth CBD — CBD grid, Swan River, key landmarks and stations.",
        bounds=(-31.970, 115.840, -31.940, 115.880),
        origin=(-31.955, 115.860),
        units_per_metre=1.0,
    )
    m.tile_size(600)
    default_zooms(m)

    # CBD grid (fine mesh)
    for gy in range(-600, 601, 120):
        m.add_line(layer=LAY_ROADS, pts=[(-1200, gy),(1200, gy)], zmin=2, zmax=4)
    for gx in range(-1200, 1201, 120):
        m.add_line(layer=LAY_ROADS, pts=[(gx, -600),(gx, 600)], zmin=2, zmax=4)

    # Freeway / Mitchell — heavy north-south west side
    m.add_line(layer=LAY_ROADS, pts=[(-1400,-1400),(-1400,1400)], zmin=1, zmax=4)
    # Great Eastern Highway — east
    m.add_line(layer=LAY_ROADS, pts=[( 1400,-1400),( 1400,1400)], zmin=1, zmax=4)
    # Riverside Drive — arcs along river
    riverside = [(int(1200*math.cos(a)), int(-900 + 200*math.sin(a)))
                 for a in [i*0.3 for i in range(-11, 12)]]
    m.add_line(layer=LAY_ROADS, pts=riverside, zmin=1, zmax=4)

    # Swan River — big polygon along the south
    river = []
    for i in range(-15, 16):
        x = i * 100
        y = -1100 + int(70 * math.sin(i * 0.5))
        river.append((x, y))
    for i in range(15, -16, -1):
        x = i * 100
        y = -1400 + int(40 * math.sin(i * 0.3))
        river.append((x, y))
    m.add_poly(layer=LAY_WATER, pts=river, zmin=1, zmax=4)

    # Kings Park — big green area to the west
    m.add_poly(layer=LAY_PARKS,
               pts=[(-1400, 100),(-800, 100),(-800, 900),(-1400, 900)], zmin=1, zmax=4)
    # Wellington Square
    m.add_poly(layer=LAY_PARKS, pts=[(300,300),(500,300),(500,500),(300,500)], zmin=2, zmax=4)
    # Supreme Court Gardens
    m.add_poly(layer=LAY_PARKS, pts=[(-200,-700),(200,-700),(200,-500),(-200,-500)], zmin=2, zmax=4)

    # Building blocks — dense grid of small buildings within the CBD
    rng = random.Random(7)
    for gx in range(-1150, 1150, 120):
        for gy in range(-580, 580, 120):
            if abs(gx) < 800 and abs(gy) < 600 and (gy < 100 or gy > 900 or gx > -800):
                cx = gx + 60 + rng.randint(-10, 10)
                cy = gy + 60 + rng.randint(-10, 10)
                w  = rng.randint(30, 50)
                h  = rng.randint(30, 50)
                m.add_poly(layer=LAY_BUILDINGS,
                           pts=[(cx-w,cy-h),(cx+w,cy-h),(cx+w,cy+h),(cx-w,cy+h)],
                           zmin=3, zmax=4)

    # Train line (Perth-Fremantle style, curves south then west)
    train = [(-1400, 300),(-1200, 320),(-800, 340),(-400, 360),(0, 380),(400, 380),(800, 360),(1200, 320),(1400, 300)]
    m.add_line(layer=LAY_TRANSIT, pts=train, zmin=1, zmax=4)
    # Bus corridor
    m.add_line(layer=LAY_TRANSIT, pts=[(-1200, -100),(1200, -100)], zmin=2, zmax=4)

    # POIs — landmarks + transit + shops + food
    pois = [
        (POI_LANDMARK,    0,    0, "Perth Town Hall"),
        (POI_LANDMARK, -300,  200, "St George's Cathedral"),
        (POI_LANDMARK,  200, -300, "Bell Tower"),
        (POI_LANDMARK, -900,  500, "Kings Park DNA Tower"),
        (POI_LANDMARK,  600,  400, "WACA Ground"),
        (POI_TRANSIT,   -200, 380, "Perth Station"),
        (POI_TRANSIT,    500, 380, "McIver Station"),
        (POI_TRANSIT,   -600, 380, "City West Station"),
        (POI_TRANSIT,    900, 360, "Claisebrook Station"),
        (POI_SHOP,       120,  60, "Myer"),
        (POI_SHOP,      -120,  60, "David Jones"),
        (POI_SHOP,       240,  60, "Carillon City"),
        (POI_SHOP,      -240, -60, "Wesley Quarter"),
        (POI_FOOD,        60, 120, "Northbridge Piazza"),
        (POI_FOOD,        60,-120, "Elizabeth Quay Food"),
        (POI_FOOD,       360, 120, "Beaufort St Cafes"),
        (POI_SCHOOL,   -800,  700, "Perth Modern"),
        (POI_PARK,    -1100,  500, "Kings Park"),
        (POI_PARK,        0, -600, "Supreme Court Gardens"),
        (POI_PARK,      400,  400, "Wellington Square"),
        (POI_SERVICE,   -60,  240, "Public Library"),
        (POI_SERVICE,   240, -240, "Perth Post"),
        (POI_LANDMARK,  120, -600, "Elizabeth Quay"),
        (POI_LANDMARK, -600, -800, "South Perth Ferry"),
        (POI_TRANSIT,  -300,-1050,"Barrack St Jetty"),
    ]
    for kind, x, y, name in pois:
        m.add_poi(kind=kind, x=x, y=y, name=name)

    default_styles(m)
    m.write(os.path.join(OUT_DIR, "Perth.fmap"))


if __name__ == "__main__":
    build_school()
    build_byford()
    build_perth()
    print("done")
