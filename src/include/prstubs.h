#ifndef _IN_PRSTUBS_H
#define _IN_PRSTUBS_H

/**
 * Declarations for port/src/prstubs.c (the Dabs modules this build leaves
 * out, answered as if idle) and the few game functions Perfect Random calls
 * that have no header of their own here. Included through prcompat.h.
 */

#include <ultra64.h>
#include "types.h"

struct warpparams;
struct animtablerow;
struct mpweapon;

// netplay, co-op, kill feed
void netEvChrDamage(struct chrdata *chr, struct prop *aprop, s32 hitpart, s32 damageshield, s32 explosion);
s32 netEvRecordDeath(s32 aplayernum, s32 vplayernum);
void netHostMatchEnded(void);
void netMigrateRestoreHealth(void);
void netMigrateRestoreInventory(void);
s32 netMigrateTakeSpawn(struct coord *pos, s16 *rooms, f32 *angle);
s32 netSlotOwnS32(s32 playernum, const s32 *var, s32 mine);
void netCoopHostCameraSwitch(s32 cmdindex, struct warpparams *params);
s32 netCoopJoinSpawn(struct coord *pos, s16 *rooms, f32 *turnanglerad);
f32 netCoopSpawnPick(struct coord *pos, s16 *rooms);
extern u32 g_CoopAntiMask;
bool coopIsAgentProp(struct prop *prop);
void killfeedNoteHit(struct chrdata *chr, struct gset *gset, struct prop *aprop, s32 explosion);

// Ghost Trials, Mission Respawn, kept bodies
void modGhostDisarmTrial(void);
bool modGhostTrialRulesApply(void);
void modRespawnBegin(void);
bool modRespawnCanRespawn(void);
void modRespawnEnd(void);
bool modRespawnIsRespawning(void);
void modRespawnReset(void);
bool modRespawnTakeSpot(struct coord *pos, RoomNum *rooms, f32 *angle);
s32 modBodiesGetReserve(void);
s32 modBodiesSetReserve(void);

// the mod loader
s32 modloaderDirIndexIsConversion(s32 moddir);
const char *modloaderGetStageMapName(s32 stagenum);
s32 modloaderGetStageModDirIndex(s32 stagenum);
s32 modloaderMissionStageOf(const char *variant, s32 mission);
s32 modloaderStageIsMission(s32 stagenum);
s32 modloaderStageIsRemake(s32 stagenum);
s32 modDataBgStage(s32 def);
s32 modDataHeadNum(s32 def);
s32 arenaTabsIsMissionCopyRow(s32 i);
void arenaTabsSync(void);
const char *langTr(const char *en);

// GoldenEye
extern s32 g_GeChrAnims;
s32 geRoomActive(void);
s32 geChrAnimsPd(s32 animnum);
s32 geChrAnimsPdDeaths(void);
void geChrAnimsSetPdDeaths(s32 on);
struct animtablerow *geChrAnimsStagger(s32 index);
s32 geDeathCamHolds(void);
s32 geSfxNum(s32 id);
s32 geSfxStage(void);
f32 geStanFloorAt(struct coord *pos);
s32 geStanLinks(struct coord *from, f32 fromground, struct coord *to, f32 toground, bool climbs);
s32 geStanSpawnLegal(struct coord *pos, s32 padroom, f32 radius);
s32 geStanTeleportSpot(struct coord *pos, s32 padroom, f32 facing, f32 radius, struct prop *self, struct coord *out);
s32 geStanTileRoomAt(struct coord *pos, s32 prefer);
void geTankLeaveForCutscene(void);
s32 gebeanGuardBodies(s32 *rows, s32 max);
u16 gebeanPoolLoadCheck(struct modeldef *modeldef, u16 fileid);
s32 gebeanRandomHeadForBody(s32 bodynum);
s32 gecinemaEndingOver(void);
void gecinemaSetMissionTimerHeld(s32 held);
s32 gegunsFloorModel(s32 weaponnum, s32 fallback);
s32 gegunsNeverPairs(s32 weaponnum);
s32 gegunsPdEquivalent(s32 weaponnum);
s32 gexFrontMissionReport(void);
s32 gexPlusDkModeSpares(s32 bodynum, s32 headnum);
void gexPlusExitOnButtonPress(void);
s32 gexPlusGetPdGuns(void);
s32 gexPlusRomOwnHead(s32 bodynum);
s32 gexPlusRomRowIsAlly(s32 row);
void gexPlusSetPdEnemies(s32 on);
void gexPlusSetPdGuns(s32 on);
void xblaMeshRegisterModel(struct modeldef *modeldef, u16 fileid);

// game code carried from the Dabs fork, or left out of it
void chrPlayArghAnimation(struct chrdata *chr, f32 angle, s32 hitpart);
void *fsFileLoadPadded(const char *name, u32 *outSize, u32 pad);
bool mpCanSpawnWithWeapon(const struct mpweapon *mpweapon);
bool mpWeaponRowSuitsStage(const struct mpweapon *mpweapon);
s32 mpGetNumSimSlotsOn(void);
char *mpGetArenaName(s32 index);
u16 langAddPortText(const char *text);
u32 mempGetStageFreeTotal(void);
void padSetPosY(s32 padnum, f32 y);
struct modeldef *modeldefLoadHead(s32 headnum);
void chrShotBlood(struct chrdata *chr, struct prop *prop, struct coord *gunpos, struct coord *hitpos, struct coord *dir, struct chrdata *shooter);
void bgSetShotPassGlass(bool enabled);
void randomMenuSummaryPush(void);
const char *romdataFileGetName(s32 fileNum);
struct chrdata *chrFindByLiteralId(s32 chrnum);
bool bg3dPosTo2dPos(struct coord *cornerpos, struct coord *screenpos);
void cdSetObstacleVtxProp(struct coord *vtx1, struct coord *vtx2, struct prop *prop);

#endif
