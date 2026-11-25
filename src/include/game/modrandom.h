#ifndef IN_GAME_MODRANDOM_H
#define IN_GAME_MODRANDOM_H
#include "prcompat.h"
#include <ultra64.h>
#include "data.h"
#include "types.h"

// The generator this build deals with; a run's own version lives in
// g_ModOptions.randomversion and may be older. See modrandom.c.
#define MODRANDOM_VERSION_DEFAULT 21
#define GAILIST_ROGUE_OBJECTIVE_WAIT 0x0031
#define GAILIST_ROGUE_OBJECTIVE_PURSUE 0x0032
#define MODEVENT_TEAM_MAIAN 0x08

// Reserved encoding in an objective completion-flags criterion for a
// Randomizer kill-target character id.
#define MODRANDOM_KILLCHR_TAG_MASK  0xffff0000u
#define MODRANDOM_KILLCHR_TAG       0x4b490000u

// The climax objective: reach the marked zone and hold it
#define MODRANDOM_HOLD_TAG          0x484f0000u
s32 modRandomHoldStatus(void);
void modRandomUseFriendPhone(void); // the Friend Phone item: 1-3 partners next to the player
void modRandomAlienItemsSet(bool enabled); // Gauntlet-only Maian/Skedar devices
bool modRandomAlienWarActive(void);
const char *modRandomEventName(s32 kind);
const char *modRandomGetActiveEventName(void);
#ifndef PLATFORM_N64
void modRandomQueueEventOverlay(void);
void modRandomEventAnnouncementFramePresented(bool presented, bool overlayqueued);
#endif
void modRandomCloakHunter(struct chrdata *chr);
void modRandomSpawnFadeIn(struct chrdata *chr);
bool modRandomVoiceIsFemale(s32 bodynum, s32 headnum);
bool modRandomAssassinCloakActive(void);
s32 modRandomGetAlienLeader(void);
void modRandomUseArmCivs(void);     // the Arm The Civs item
void modRandomUseTargetLocator(void); // mark the current objective target in the world
bool modRandomGetTargetLocatorPos(struct coord *pos, bool *pacify);
bool modRandomCanRelocateTarget(void);
void modRandomRequestTargetRelocation(void);
bool modRandomDeferTargetFall(struct chrdata *chr);
bool modRandomGetClimaxZone(struct coord *pos); // the zone to mark on the radar, while it is live

void modRandomArmMission(void);
void modRandomDisarmMission(void);
bool modRandomIsArmed(void);
struct weaponobj;
bool modRandomIsOn(void);
void modRandomEventHistoryReset(void);
s32 modRandomGiveAllies(s32 count); // buddies around the player; how many came
bool modRandomHasReinforcements(void);
bool modRandomNpcWeaponAllowed(s32 chrnum, s32 weaponnum);
bool modRandomStripsStageScripts(void);
bool modRandomChrMaySurrender(struct chrdata *chr);

// Debug overlay (Mod.RandomDebug / Randomizer Options > Debug > Debug Text): a scrolling
// log of spawns, objectives and events at the top left, and living/total tallies.
#define MODDBG_STAGE 0
#define MODDBG_REINF 1
#define MODDBG_PUNISH 2
#define MODDBG_EVENT 3
#define MODDBG_ROUND 4
#define MODDBG_CIV 5
extern s32 g_ModRandomDebug;
extern s32 g_ModRandomDebugTint;
extern s32 g_ModRandomDebugRadar;
extern s32 g_ModRandomDebugDefendersRadar;
extern s32 g_ModRandomDebugStateCount;
extern s32 g_ModRandomDebugCloak;
extern s32 g_ModRandomDebugShotguns;
extern s32 g_ModRandomDebugMaxGrenades;
void modRandomDebugGiveGrenades(void);
bool modRandomChrHasAutomaticWeapon(struct chrdata *chr);
bool modRandomDebugShotgun(s32 *weaponnum, s32 *modelnum);
void modRandomDebugGivePlayerShotgun(void);
void modRandomDebugMsg(const char *fmt, ...);
void modRandomDebugSpawn(struct chrdata *chr, s32 category);
Gfx *modRandomDebugRender(Gfx *gdl);
void modRandomFallbackGun(s32 chrnum, s32 *weaponnum, s32 *modelnum);
bool modRandomIsObjectiveChr(s32 chrnum);
void modRandomObjectiveReactToPlayer(struct chrdata *chr, struct prop *player);
bool modRandomIsCivilianChr(struct chrdata *chr);
bool modRandomTacticsDefenderRadarOn(void);
u32 modRandomTacticsRadarColour(struct chrdata *chr);
bool modRandomTacticsSeek(struct chrdata *chr); // route a reinforcement to an objective post
void modRandomTacticsQueuePunishment(struct chrdata *chr);
bool modRandomTacticsTint(struct chrdata *chr, s32 *colour); // the tactics debug tint
bool modRandomExplosiveTint(struct chrdata *chr, s32 *colour); // Explosive Exit: the red blinks before a blast
void modRandomOnSurrender(struct chrdata *chr); // a guard that gives up stays given up
void modRandomOnGunShotOut(struct chrdata *chr); // may make the guard surrender
bool modRandomChrExplosionImmune(struct chrdata *chr); // the terrorist event's
bool modRandomChrCarriesAssaultRifle(s32 chrnum);
bool modRandomNpcAssaultRifle(s32 chrnum, s32 *weaponnum, s32 *modelnum); // one guard in five: swap in an assault rifle
bool modRandomIsCassandraGun(struct weaponobj *weapon); // her gun: 100 rounds when picked up
struct path *modRandomGetPatrolPath(struct chrdata *chr);
bool modRandomIsEndless(void);

// A Random mission skips the stage's intro cutscene (aiSetCameraAnimation) but
// the scripts still speak and play its music. Muted from the moment the skipped
// cutscene would have begun until its end command, or a time limit.
void modRandomMuteCutscene(void);
void modRandomUnmuteCutscene(void);
bool modRandomCutsceneMuted(void);
s32 modRandomGetRooms(void);
s32 modRandomGetCleared(void);
s32 modRandomGetVersion(void);
s32 modRandomGetAllyReserve(void);
s32 modRandomGetWarzoneReserve(void);
s32 modRandomGetStartingWeapon(void);
s32 modRandomGetUtilityWeapon(void);
s32 modRandomGetObjectiveTarget(void);
bool modRandomObjectiveTargetIsPacify(void); // the target is to be knocked out: blue on the radar
s32 modRandomTargetStatus(s32 chrnum);
u32 modRandomGetSeed(void);
void modRandomRoll(s32 stagenum);
void modRandomPrepareArenaSetup(void);
void modRandomTick(void);
void modRandomOnChrDeath(struct chrdata *chr, s32 playernum);
bool modRandomTakeSpawn(struct coord *pos, RoomNum *rooms, f32 *angle);
void modRandomSpawnClear(struct coord *pos, RoomNum *rooms, f32 angle);

// Where a player put down on this pad would stand (false only when there is no
// floor under it at all), and whether it is a pad worth dealing in the first
// place. The Randomizer's start and the run's landing both ask; see
// modrandom.c.
bool modRandomPadSpawnPos(s32 padnum, struct coord *pos, RoomNum *room);
s32 modRandomEditStartPad(void);
bool modRandomPadCanSpawn(s32 padnum);
bool modRandomPadSettle(s32 padnum);
void modRandomInsertObjectives(void);
char *modRandomGetObjectiveText(s32 index);

#ifndef PLATFORM_N64
s32 modRandomTestEventSpot(bool squad);
s32 modRandomTestReturningGuard(void);
s32 modRandomTestClimaxPad(void);
/** Microseconds spent this sample in: 0 lvTick, 1 ragdollTick (inside it), 2 lvTickPlayer, 3 lvRender, 4 buffer swap. For the frame-time line. */
extern u64 g_ModPerfUs[28]; // 20/21 chr draw calls live/dead, 22 object draw calls, 23 chr draw build time (us), 24 other prop draws; // 10 rdpCreateTask, 11 schedEndFrame, 12-16 inside the body tick: pre loop, props, drive loop, physics step, post loop; 17-19 counts: stepped, solid, asleep bodies // and inside the ragdoll tick: 5 ceilings, 6 grab, 7 bodies, 8 shotgun cuts, 9 examine
#endif

#endif
