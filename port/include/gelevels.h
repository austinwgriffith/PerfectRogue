#ifndef _IN_GELEVELS_H
#define _IN_GELEVELS_H

#include <PR/ultratypes.h>
#include "types.h"

/**
 * GoldenEye's levels as Perfect Random maps (port/src/gelevels.c).
 *
 * tools/geconvert turns the player's own GoldenEye ROM into Perfect Dark's
 * file formats under mods/goldeneye/ (bg, tiles, pads, an arena setup per
 * level, GoldenEye's prop models and textures, and a modconfig.txt that lists
 * them). At boot this module gives those files numbers, gives each level a
 * stage number and a row in g_Stages, and fills the prop models' slots. From
 * then on a GoldenEye level is an ordinary stage: the engine has no
 * GoldenEye-specific rules. Perfect Random plays them the way it plays a
 * Combat Simulator map (modRunStartArenaMission()).
 *
 * The one thing that cannot be a plain file is the textures: the display lists
 * name a texture by a 12-bit number and GoldenEye's 2,290 overlap Perfect
 * Dark's. So they are a second set under the same numbers, in force while a
 * GoldenEye level's rooms or one of GoldenEye's prop models load
 * (gelevelsTexSet()), and cached apart from Perfect Dark's (GELEVELS_SRCMOD).
 */

#define GELEVELS_MAX_STAGES 32 // rows after the stock 61 in g_Stages
#define GELEVELS_SRCMOD 0xb0   // struct tex's srcmod for GoldenEye's textures (heads use 0xc0+)

void gelevelsInit(void);

// If the GoldenEye ROM sits beside the game and mods/goldeneye is missing or
// out of date, makes it (port/src/gelevelsconv.c). gelevelsInit() calls it.
void gelevelsAutoConvert(void);

s32 gelevelsCount(void);
s32 gelevelsStageAt(s32 index);
s32 gelevelsIsStage(s32 stagenum);
const char *gelevelsName(s32 stagenum);
s32 gelevelsFileIsGe(s32 filenum);
const char *gelevelsAllocation(s32 stagenum);

// the level's GoldenEye music: sequence numbers for main theme, background, X theme
bool gelevelsGetMusic(s32 stagenum, s32 tracks[3]);

bool gelevelsGetEnv(s32 stagenum, struct fogenvironment **fog, struct nofogenvironment **nofog);

s32 gelevelsTexSet(s32 on);
s32 gelevelsTexActive(void);
s32 gelevelsTextureLoad(u16 num, void *dst, u32 dstSize);

#endif
