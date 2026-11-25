/**
 * Perfect Random on Carrington Evolved: stand-ins for the Dabs fork's modules
 * that this build leaves out.
 *
 * Perfect Random was written on the Dabs fork, and the mode and the game code
 * it touches still ask the fork's GoldenEye, netplay, Ghost Trials, mod
 * loader and Mission Respawn modules questions. None of those is here, so
 * each question gets the answer it gets when that module is idle: no
 * GoldenEye level, no online game, no trial, no mod stage, no respawn.
 *
 * To bring GoldenEye back, port the module and delete its stubs from this
 * file; the linker then names every place that still needs it.
 */

#include <ultra64.h>
#include "gelevels.h"
#include "constants.h"
#include "types.h"
#include "prcompat.h"

// ---- netplay (Dabs: port/src/net/) ---------------------------------------

s32 g_NetMode = NETMODE_NONE;

void netEvChrDamage(struct chrdata *chr, struct prop *aprop, s32 hitpart, s32 damageshield, s32 explosion) {}
s32 netEvRecordDeath(s32 aplayernum, s32 vplayernum) { return 0; }
void netHostMatchEnded(void) {}
void netMigrateRestoreHealth(void) {}
void netMigrateRestoreInventory(void) {}
s32 netMigrateTakeSpawn(struct coord *pos, s16 *rooms, f32 *angle) { return false; }
s32 netSlotOwnS32(s32 playernum, const s32 *var, s32 mine) { return mine; }
void netCoopHostCameraSwitch(s32 cmdindex, struct warpparams *params) {}
s32 netCoopJoinSpawn(struct coord *pos, s16 *rooms, f32 *turnanglerad) { return false; }
f32 netCoopSpawnPick(struct coord *pos, s16 *rooms) { return 0; }

// ---- online co-op / counter-op (Dabs: src/game/coop.c) --------------------

u32 g_CoopAntiMask = 0;

bool coopIsAgentProp(struct prop *prop)
{
	return prop && prop->type == PROPTYPE_PLAYER;
}

// ---- kill feed (Dabs: port/src/killfeed.c) --------------------------------

void killfeedNoteHit(struct chrdata *chr, struct gset *gset, struct prop *aprop, s32 explosion) {}

// ---- Ghost Trials (Dabs: src/game/modghost.c) -----------------------------

void modGhostDisarmTrial(void) {}
bool modGhostTrialRulesApply(void) { return false; }

// ---- Mission Respawn and kept bodies (Dabs: modrespawn.c, modbodies.c) ----

void modRespawnBegin(void) {}
bool modRespawnCanRespawn(void) { return false; }
void modRespawnEnd(void) {}
bool modRespawnIsRespawning(void) { return false; }
void modRespawnReset(void) {}
bool modRespawnTakeSpot(struct coord *pos, RoomNum *rooms, f32 *angle) { return false; }
s32 modBodiesGetReserve(void) { return 0; }
s32 modBodiesSetReserve(void) { return 0; }

// ---- the mod loader and mod data (Dabs: modloader.c, moddata.c) -----------

s32 modloaderDirIndexIsConversion(s32 moddir) { return false; }
const char *modloaderGetStageMapName(s32 stagenum) { return gelevelsName(stagenum); } // GoldenEye levels
s32 modloaderGetStageModDirIndex(s32 stagenum) { return -1; }
s32 modloaderMissionStageOf(const char *variant, s32 mission) { return gelevelsStageAt(mission); } // 0 past the last
s32 modloaderStageIsMission(s32 stagenum) { return false; }
s32 modloaderStageIsRemake(s32 stagenum) { return false; }
s32 modDataBgStage(s32 def) { return def; }
s32 modDataHeadNum(s32 def) { return def; }
s32 arenaTabsIsMissionCopyRow(s32 i) { return false; }
void arenaTabsSync(void) {}

// ---- languages (Dabs: langpack.c) -----------------------------------------

const char *langTr(const char *en) { return en; }

// ---- GoldenEye (Dabs: ge*.c, gex*.c, gebean*.c, xbla*.c) -------------------

s32 g_GeChrAnims = 0;

s32 geRoomActive(void) { return false; }
s32 geChrAnimsPd(s32 animnum) { return animnum; }
s32 geChrAnimsPdDeaths(void) { return true; }
void geChrAnimsSetPdDeaths(s32 on) {}
struct animtablerow *geChrAnimsStagger(s32 index) { return NULL; }
s32 geDeathCamHolds(void) { return false; }
s32 geSfxNum(s32 id) { return id; }
s32 geSfxStage(void) { return false; }
f32 geStanFloorAt(struct coord *pos) { return -4294967296.0f; }
s32 geStanLinks(struct coord *from, f32 fromground, struct coord *to, f32 toground, bool climbs) { return true; }
s32 geStanSpawnLegal(struct coord *pos, s32 padroom, f32 radius) { return true; }
s32 geStanTeleportSpot(struct coord *pos, s32 padroom, f32 facing, f32 radius, struct prop *self, struct coord *out) { return false; }
s32 geStanTileRoomAt(struct coord *pos, s32 prefer) { return -1; }
void geTankLeaveForCutscene(void) {}
s32 gebeanGuardBodies(s32 *rows, s32 max) { return 0; }
u16 gebeanPoolLoadCheck(struct modeldef *modeldef, u16 fileid) { return 0; } /* 0 = no ROM replacement */
s32 gebeanRandomHeadForBody(s32 bodynum) { return -1; }
s32 gecinemaEndingOver(void) { return false; }
void gecinemaSetMissionTimerHeld(s32 held) {}
s32 gegunsFloorModel(s32 weaponnum, s32 fallback) { return fallback; }
s32 gegunsNeverPairs(s32 weaponnum) { return false; }
s32 gegunsPdEquivalent(s32 weaponnum) { return weaponnum; }
s32 gexFrontMissionReport(void) { return false; }
s32 gexPlusDkModeSpares(s32 bodynum, s32 headnum) { return 0; }
void gexPlusExitOnButtonPress(void) {}
s32 gexPlusGetPdGuns(void) { return true; }
s32 gexPlusRomOwnHead(s32 bodynum) { return -1; }
s32 gexPlusRomRowIsAlly(s32 row) { return false; }
void gexPlusSetPdEnemies(s32 on) {}
void gexPlusSetPdGuns(s32 on) {}
void xblaMeshRegisterModel(struct modeldef *modeldef, u16 fileid) {}

// ---- Dabs game code this build does not carry -----------------------------

// A simulant's flinch animation on a hit that does not kill (Dabs chraction.c)
void chrPlayArghAnimation(struct chrdata *chr, f32 angle, s32 hitpart) {}

// ---- small helpers Perfect Random uses from the Dabs fork's shared files ---

#include <string.h>
#include "fs.h"
#include "system.h"
#include "data.h"
#include "game/challenge.h"
#include "game/lang.h"

/** fsFileLoad(), with pad zeroed bytes after the data (Dabs fs.c). */
void *fsFileLoadPadded(const char *name, u32 *outSize, u32 pad)
{
	u32 size = 0;
	u8 *data = fsFileLoad(name, &size);
	u8 *padded;

	if (!data) {
		return NULL;
	}

	padded = sysMemZeroAlloc(size + pad + 1);

	if (padded) {
		memcpy(padded, data, size);
	}

	sysMemFree(data);

	if (outSize) {
		*outSize = size;
	}

	return padded;
}

/** Whether a Combat Simulator weapon row can be handed out (Dabs mplayer.c). */
bool mpCanSpawnWithWeapon(const struct mpweapon *mpweapon)
{
	if (mpweapon->weaponnum == WEAPON_NONE
			|| mpweapon->weaponnum == WEAPON_MPSHIELD
			|| mpweapon->weaponnum == WEAPON_DISABLED) {
		return false;
	}

	return challengeIsFeatureUnlocked(mpweapon->unlockfeature);
}

/** Every weapon row suits every stage here: there are no GoldenEye rows. */
bool mpWeaponRowSuitsStage(const struct mpweapon *mpweapon)
{
	return true;
}

/** The simulants a match can have; setup reserves chrs for this many. */
s32 mpGetNumSimSlotsOn(void)
{
	return MAX_BOTS;
}

/** An arena's name, as its Combat Simulator row has it. */
char *mpGetArenaName(s32 index)
{
	return langGet(g_MpArenas[index].name);
}
