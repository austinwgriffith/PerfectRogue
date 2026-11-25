#ifndef IN_GAME_MODSPAWNEDIT_H
#define IN_GAME_MODSPAWNEDIT_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

/**
 * The Spawn Customizer (Perfect Random). Phase 1: a read-only viewer.
 *
 * A map loads with no guards, objectives, waves or events, the player flies
 * through it with no gravity or collision, and the places the game would use
 * are drawn as markers: guard, hunter, objective and escape (start) spawns.
 * See modspawnedit.c, and claude/spawn-customizer-plan.md in the project.
 */

#define MODSPAWN_GUARD     0 // guards and civilians
#define MODSPAWN_HUNTER    1 // where reinforcement waves may appear
#define MODSPAWN_OBJECTIVE 2 // kill targets and pickups
#define MODSPAWN_ESCAPE    3 // the player's start and the way out
#define MODSPAWN_GUARANTEED 4 // a guard that is always there when the stage loads
#define MODSPAWN_NUMKINDS  5

// Character slots the editor asks the stage for, which is how many spawns can
// stand as characters at once (setup.c sizes the model manager from this)
#define MODSPAWN_EDIT_CHRSLOTS_MAX 804 // 800 guards standing at once, and the placement previews

#ifndef PLATFORM_N64
extern s32 g_SpawnEditTestStage; // --selftest-spawnedit
s32 modSpawnEditChrSlots(void);
void modSpawnEditTestTick(void);
bool modSpawnEditIsOn(void);
bool modSpawnEditPlayEnded(void);
s32 modChrOnscreenCap(void);
void modSpawnEditClear(void);
void modSpawnEditStart(s32 stagenum);
bool modSpawnEditOnStageLoad(s32 stagenum);
void modSpawnEditFlyTick(void);
void modSpawnEditWalkTick(void);
bool modSpawnEditFlying(void);
Gfx *modSpawnEditRender(Gfx *gdl);
bool modSpawnEditIsGhost(struct prop *prop);
bool modSpawnEditEatsEscape(void);
bool modSpawnEditMenuOpen(void);
bool modSpawnEditChrTint(struct chrdata *chr, s32 *colour);

// Configs in a run (modspawnedit.c, "Configs in a run")
void modSpawnConfigUse(s32 stagenum, const char *name);
void modSpawnConfigOff(s32 stagenum);
bool modSpawnConfigIsChosen(s32 stagenum);
const char *modSpawnConfigName(void);
const char *modSpawnConfigChosenName(s32 stagenum); // NULL: the map's default
#define MODSPAWN_CONFIG_NAMELEN 24
s32 modSpawnConfigList(s32 stagenum, char names[][MODSPAWN_CONFIG_NAMELEN + 1], s32 max);
bool modSpawnConfigPrepare(s32 stagenum);
s32 modSpawnConfigCount(s32 kind);
s32 modSpawnConfigPad(s32 kind, s32 index);
bool modSpawnConfigIsPadOfKind(s32 kind, s32 padnum);
struct waypoint *modSpawnConfigHunters(s32 *count);
#else
#define modSpawnEditIsOn() 0
#define modSpawnEditPlayEnded() 0
#define modSpawnEditClear()
#define modSpawnEditIsGhost(prop) 0
#define modSpawnEditEatsEscape() 0
#define modSpawnEditMenuOpen() 0
#define modSpawnEditChrTint(chr, colour) 0
#define modSpawnConfigHunters(count) ((struct waypoint *)0)
#define modSpawnConfigCount(kind) 0
#endif

#endif
