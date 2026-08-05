# `.fmap` Binary Format v1

Little-endian. All integers are unsigned unless stated. Strings are UTF-8, prefixed
with a `uint16` length. Coordinates are int32 in a local metric projection whose
origin + scale is stored in the metadata header.

```
+------------------------------------------------------------+
|                       [0] HEADER                           |
+------------------------------------------------------------+
| magic[4]      = "FMAP"                                     |
| version       : uint16   (=1)                              |
| flags         : uint16   (bit0=compressed, bit1=has_poi,   |
|                           bit2=has_search, bit3=has_style) |
| section_count : uint16                                     |
| reserved      : uint16                                     |
+------------------------------------------------------------+
|                    [1] SECTION TABLE                       |
+------------------------------------------------------------+
| repeat section_count times:                                |
|   id     : uint16   (1=meta, 2=zoom, 3=tile_idx,           |
|                      4=vec, 5=poi, 6=search, 7=style)      |
|   offset : uint32   (absolute file offset)                 |
|   length : uint32                                          |
+------------------------------------------------------------+
|                   [2] SECTION 1 : META                     |
+------------------------------------------------------------+
| title    : lstr                                            |
| desc     : lstr                                            |
| created  : uint32   (unix seconds)                         |
| min_lat  : float32                                         |
| min_lon  : float32                                         |
| max_lat  : float32                                         |
| max_lon  : float32                                         |
| origin_lat : float32                                       |
| origin_lon : float32                                       |
| units_per_metre : float32  (world unit scale)              |
| projection : uint8  (0=equirect local)                     |
+------------------------------------------------------------+
|                   [3] SECTION 2 : ZOOM                     |
+------------------------------------------------------------+
| count : uint8                                              |
| repeat count:                                              |
|   zoom_id      : uint8   (1..N)                            |
|   min_px_scale : float32 (world units per screen pixel)    |
|   object_count : uint32                                    |
+------------------------------------------------------------+
|                 [4] SECTION 3 : TILE INDEX                 |
+------------------------------------------------------------+
| tile_size_units : int32   (side of a tile in world units)  |
| grid_min_tx  : int16                                       |
| grid_min_ty  : int16                                       |
| grid_max_tx  : int16                                       |
| grid_max_ty  : int16                                       |
| tile_count   : uint32                                      |
| repeat tile_count:                                         |
|   tx      : int16                                          |
|   ty      : int16                                          |
|   zmask   : uint8   (bit i set => tile has objects at Zi)  |
|   offset  : uint32  (offset into VEC section)              |
|   length  : uint32                                         |
+------------------------------------------------------------+
|                 [5] SECTION 4 : VECTOR DATA                |
+------------------------------------------------------------+
| per tile, packed back-to-back:                             |
|   object_count : uint16                                    |
|   repeat object_count:                                     |
|     type      : uint8   (1=LINE 2=POLY 3=POINT)            |
|     layer     : uint8   (see LAYER_* constants)            |
|     zmin      : uint8   (min zoom this object appears)     |
|     zmax      : uint8                                      |
|     flags     : uint8   (bit0=closed poly, bit1=one-way)   |
|     point_cnt : uint16                                     |
|     dx0,dy0   : int32   (absolute first coord)             |
|     repeat point_cnt-1:                                    |
|       dx,dy   : varint  (zig-zag encoded delta)            |
+------------------------------------------------------------+
|                   [6] SECTION 5 : POI DB                   |
+------------------------------------------------------------+
| count : uint32                                             |
| repeat count:                                              |
|   type    : uint8   (POI_* constant)                       |
|   x       : int32   (world units)                          |
|   y       : int32                                          |
|   icon_id : uint8                                          |
|   name    : lstr                                           |
+------------------------------------------------------------+
|                 [7] SECTION 6 : SEARCH INDEX               |
+------------------------------------------------------------+
| count : uint32                                             |
| entries sorted lexicographically by lowercase name:        |
|   name_offset : uint16  (offset into a tiny string pool)   |
|   poi_index   : uint32                                     |
| string_pool_len : uint32                                   |
| string_pool bytes                                          |
+------------------------------------------------------------+
|                   [8] SECTION 7 : STYLE                    |
+------------------------------------------------------------+
| layer_count : uint8                                        |
| repeat layer_count:                                        |
|   layer     : uint8                                        |
|   colour    : uint16   (RGB565)                            |
|   width_px  : uint8                                        |
|   dash_len  : uint8    (0 = solid)                         |
|   z_min     : uint8                                        |
+------------------------------------------------------------+
```

### Varint (zig-zag)

Delta coordinates are encoded with zig-zag varints:

```
zz(n) = (n << 1) ^ (n >> 31)          # int32 -> uint32
```

then LEB128: 7 bits per byte, MSB set = "more bytes follow".

### Layer IDs

| ID | Name          |
|----|---------------|
| 0  | ROADS         |
| 1  | BUILDINGS     |
| 2  | PARKS         |
| 3  | WATER         |
| 4  | PATHS         |
| 5  | SCHOOLS       |
| 6  | SHOPS         |
| 7  | PUBLIC_TRANSIT|
| 8  | BEACONS       |
| 9  | SAVED         |
| 10 | BREADCRUMBS   |

### POI Types

| ID | Name       |
|----|------------|
| 0  | GENERIC    |
| 1  | SHOP       |
| 2  | FOOD       |
| 3  | TRANSIT    |
| 4  | SCHOOL     |
| 5  | PARK       |
| 6  | LANDMARK   |
| 7  | SERVICE    |
