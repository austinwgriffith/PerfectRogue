#ifndef _IN_LEVELGENSTAGE_H
#define _IN_LEVELGENSTAGE_H

#include <PR/ultratypes.h>
#include "types.h"

/**
 * The level generator's stage (port/src/levelgenstage.c).
 *
 * One stage number, one row of g_Stages and four files, rewritten for every
 * level: the generator (src/game/modlevelgen*.c) makes the bg, tiles, pads
 * and setup files for a seed, this module writes them under mods/levelgen/,
 * makes the file slots reload them, and starts the stage the way Perfect
 * Random starts a Combat Simulator arena. Loading a generated level twice in
 * a row works because nothing about the stage is cached between loads.
 */

#define LEVELGEN_STAGENUM 0x15 // free in stock Perfect Dark (nothing else uses it)

#define LEVELGEN_SIZE_SMALL  0
#define LEVELGEN_SIZE_MEDIUM 1
#define LEVELGEN_SIZE_LARGE  2

// Gives the stage its number, file slots and g_Stages row. Writes a default
// level so a --boot-stage 0x15 works. Call after gelevelsInit().
void levelgenStageInit(void);

s32 levelgenIsStage(s32 stagenum);
// whether the generated level's lifts go up and down on their own (the pipeworks') rather than wait to be used
bool levelgenLiftsAuto(void);
// whether a pad of the generated level is a prop's (a door's or a crate's), not a spot on a floor
bool levelgenPadIsProp(s32 padnum);

// the stage pool allocation string, or NULL when stagenum is not ours
const char *levelgenAllocation(s32 stagenum);

// Makes the files for a seed and size and resets the file slots. Returns
// false (and leaves the previous files) when the generator reports an error.
bool levelgenPrepare(u32 seed, s32 size);

// levelgenPrepare(), then starts the level as an arena mission.
bool levelgenStart(u32 seed, s32 size);
bool levelgenStartDesigned(s32 layout);   // Claude Missions: a designed level (LG_LAYOUT_PIER)

// The size the menus choose (LEVELGEN_SIZE_*), saved in pd.ini as Mod.LevelGenSize.
extern s32 g_LevelGenSizeSetting;
// The layout the Generated Levels menu picks (LG_LAYOUT_*, -1 random), saved as Mod.LevelGenLayout.
extern s32 g_LevelGenLayoutSetting;

// The Gauntlet's map pool has one entry per generated layout, each under an
// id of its own (not a real stage number: they all play LEVELGEN_STAGENUM),
// so each can be ticked on and off and remembered on its own.
#define LEVELGEN_POOLID_BASE 0x150000
#define LEVELGEN_POOLID(layout) (LEVELGEN_POOLID_BASE + (layout))
#define LEVELGEN_POOLID_LAYOUT(id) ((id) >= LEVELGEN_POOLID_BASE && (id) < LEVELGEN_POOLID_BASE + 64 ? (id) - LEVELGEN_POOLID_BASE : -1)
bool levelgenRoomLoadsHeld(void);   // the next level's files are on disk, the old level is still drawn
void levelgenRoomLoadsResume(void);
const char *levelgenSizeName(s32 size);
const char *levelgenLevelName(char *buf, s32 buflen); // "(PR) <Layout>, <Aesthetic>" of the level made last
// A level seed: the Seed option's, or a new one (1..0x7fffffff, the range the
// Seed option holds, so that "Keep This Run" brings the same level back).
u32 levelgenNewSeed(void);
// From the game's tick: tells the player the level's seed once it is under way.
void levelgenTick(void);

// the seed and size of the level made last (0 before the first)
u32 levelgenGetSeed(void);
s32 levelgenGetSize(void);
s32 levelgenGetHalfX(void); // half the layout's size, x and z (it is centred on 0,0)
s32 levelgenGetHalfZ(void);
s32 levelgenPadsFileNum(void);
// the layout of the level made last, or NULL (modlevelgen.h)
struct LgLayoutTag;
const struct LgLayoutTag *levelgenGetLayout(void); // the pads file's number (for the load test)
bool levelgenNpcPathHasFloor(const struct coord *from, const struct coord *to);

// Runs the port's file preprocessors over generated files, as the loaders
// do (levelgentest.c). Prints SELFTEST lines; returns the number of failures.
int levelgenLoadersSelfTest(void (*print)(const char *line));

// --selftest-levelgen-load: starts generated levels from the game's tick and
// checks them (levelgenloadtest.c); run from modRunTick().
extern bool g_LevelGenLoadTest;
void levelgenLoadTestTick(void);

// --levelgen-tour SEED: the same hook walks the player through every room taking pictures
extern bool g_LevelGenTour;

// --levelgen-map: pictures of generated levels from above, then exit (main.c)
s32 levelgenMapCommand(u32 seed, s32 size, s32 count);

// Skies: every level gets one dealt from its seed (or forced, for the tests:
// --levelgen-sky N). levelgenGetEnv() is env.c's hook.
void levelgenForceSky(s32 sky);
s32 levelgenSkyCount(void);
s32 levelgenGetSky(void);
const char *levelgenSkyName(s32 sky);
bool levelgenGetEnv(s32 stagenum, struct nofogenvironment **nofog);

#endif
