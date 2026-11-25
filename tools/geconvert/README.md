# geconvert: GoldenEye levels for Perfect Random

`geconvert.c` is the Dabs fork's converter (version 126, `port/src/geconvert.c`
there), copied as is. It reads the player's own GoldenEye 007 (US) ROM and
writes GoldenEye's levels in Perfect Dark's own file formats: per level a bg
file, a tiles file (GoldenEye's floor tiles, with walls raised on every open
edge), a pads file and an arena setup; GoldenEye's prop models (`PgxNNNZ`);
GoldenEye's textures (`textures/XXXX.bin`, under Perfect Dark numbers); and a
`modconfig.txt` listing them (`maps` and `models` blocks).

`convert.sh` builds it and keeps only what the levels need in
`mods/goldeneye/` beside the exe. At boot `port/src/gelevels.c` reads that
folder: each level gets a free stage number and a row in `g_Stages` (cloned
from Skedar's arena), the files get numbers after the ROM's, the prop models
fill model slots 0x200+, and the textures load as a second set while a
GoldenEye room or prop is loading. Perfect Random lists them on its
"GoldenEye Maps" page and in the Gauntlet pool, and plays each the way it
plays a Combat Simulator map.

Rule for changes: fix a level in the converter's output, not with a
GoldenEye branch in the engine.

The converter itself is `port/src/geconvert.c` (headers in `port/include/`),
linked into the exe.

**Players need no tool.** Put the GoldenEye 007 (USA) ROM (any file name, .z64/.n64/.v64)
in the game's base folder (`data/`, beside `pd.ntsc-final.z64`) or next to the exe, and start the game. On first
launch `port/src/gelevelsconv.c` converts it (about a second) into `data/mods/goldeneye/` (the base folder's `mods/goldeneye/`),
with a `geconvert.version` stamp so a newer converter redoes it. With no ROM there are
no GoldenEye levels and nothing else changes.

`convert.sh` is the command-line route for developers; `mods/goldeneye/` is generated
game data from the player's own ROM and must not be committed.
