#!/bin/bash
# Converts the GoldenEye 007 (US) ROM into Perfect Dark files for Perfect Random's
# GoldenEye maps (port/src/gelevels.c). Run from MSYS2 MINGW64 (or any Linux shell):
#
#   tools/geconvert/convert.sh "path/to/GoldenEye 007 (USA).z64"
#
# Writes data/mods/goldeneye/ (the game reads mods/ from its base folder, data/).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
GAME="$(cd "$HERE/../../.." && pwd)"
ROM="${1:-$GAME/../added-content/GoldenEye 007 (USA).z64}"
OUT="$GAME/data/mods/goldeneye"   # the game reads mods/ from its base folder, data/
TMP="$GAME/data/mods/.geconvert-tmp"

SRC="$HERE/../.."
gcc -O2 -DGECONVERT_MAIN -I"$SRC/port/include" -o "$HERE/geconvert" "$SRC/port/src/geconvert.c" -lz -lm
rm -rf "$TMP"
"$HERE/geconvert" "$ROM" "$TMP"

# only what the levels use: the level files, the prop models, the textures
rm -rf "$OUT"
mkdir -p "$OUT/files/bgdata" "$OUT/textures"
cp "$TMP/modconfig.txt" "$OUT/"
grep -o "GECONVERT_VERSION_STR \"[0-9]*\"" "$HERE/../../port/include/geconvert.h" | grep -o "[0-9]*" | tr -d "\n" > "$OUT/geconvert.version"
cp "$TMP"/files/bgdata/* "$OUT/files/bgdata/"
cp "$TMP"/files/Pgx* "$TMP"/files/Ump_setupgx* "$OUT/files/"
cp "$TMP"/textures/* "$OUT/textures/"
rm -rf "$TMP"
echo "GoldenEye levels written to $OUT"
