#!/bin/sh
# Streams a public Nasdaq TotalView-ITCH 5.0 sample file and converts N order
# messages, starting at START, into data/itch.bin. The orders already resting
# at START go to data/itch.seed.bin. Only the part of the multi-GB file up to
# the end of the slice is downloaded (about 1 GB for the defaults).
#
#   scripts/fetch_itch.sh [N=5000000] [START=09:35:00]
cd "$(dirname "$0")/.." || exit 1
URL="${ITCH_URL:-https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz}"
N="${1:-5000000}"
START="${2:-09:35:00}"

[ -x build/itch_convert ] || { echo "build first: cmake -B build && cmake --build build" >&2; exit 1; }
mkdir -p data
# curl is cut off once N messages are written, so its exit status is ignored.
curl -fsSL "$URL" 2>/dev/null | gunzip -c 2>/dev/null |
    build/itch_convert - data/itch.bin "$N" "$START" data/itch.seed.bin
[ -s data/itch.bin ] || { echo "no data written; check the URL or your connection" >&2; exit 1; }
