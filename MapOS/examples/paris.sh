#!/usr/bin/env bash
# Example: download two maps -- central Paris and central London -- into
# separate folders so MapOS shows them as a 2-entry library carousel.
set -euo pipefail

python3 -m pip install --quiet pillow requests

python3 tools/prep_tiles.py download \
    --map "Paris" \
    --title "Paris" \
    --desc "Inside the boulevard peripherique" \
    --lat1 48.80 --lon1 2.25 \
    --lat2 48.91 --lon2 2.42 \
    --zmin 13 --zmax 15 \
    --out ./SD

python3 tools/prep_tiles.py download \
    --map "London" \
    --title "London" \
    --desc "Zone 1 + 2" \
    --lat1 51.46 --lon1 -0.22 \
    --lat2 51.55 --lon2 -0.02 \
    --zmin 13 --zmax 15 \
    --out ./SD

# Global "you are here" (shared between maps)
python3 tools/prep_tiles.py me --lat 48.8566 --lon 2.3522 --out ./SD

echo
echo "Done. Copy ./SD/WATCH/ to the SD card root."
echo "MapOS will show a 2-map carousel: Paris and London."
