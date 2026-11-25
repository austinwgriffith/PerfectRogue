#ifndef IN_GAME_MODCAREER_H
#define IN_GAME_MODCAREER_H

#include <ultra64.h>
#include <stddef.h>
#include "types.h"

/**
 * Perfect Random Gauntlet: a chain of complete randomized missions, each harder
 * than the last. See modcareer.c.
 *
 * (The "career" names are from the first stub and are kept so the hooks that
 * already call them - modoptions.c's wave size - did not have to move.)
 */
#define MODCAREER_UNLIMITED 0
#define MODCAREER_MINMISSIONS 4
#define MODCAREER_DEFAULTMISSIONS 6
#define MODCAREER_MAXMISSIONS 20

// The kinds of map a gauntlet can be dealt, in the order the pool lists them.
#define MODCAREER_KIND_SOLO  0 // a Perfect Dark mission
#define MODCAREER_KIND_GE    1 // a GoldenEye mission (GE Plus)
#define MODCAREER_KIND_ARENA 2 // a Combat Simulator map
#define MODCAREER_KIND_GEN   3 // a generated level (levelgenstage.c)
#define MODCAREER_NUMKINDS   4
#define MODCAREER_MAXPOOL    256

// The gauntlet's settings, saved in pd.ini (port/src/main.c) and edited from
// the Perfect Random > Gauntlet page.
extern s32 g_ModCareerLength;
extern s32 g_ModCareerBest;         // most missions cleared in one run
extern s32 g_ModCareerHideUnfunMaps;
#define MODCAREER_NUMEVENTS 7
#define MODCAREER_BASE_WAVE_SIZE 2
#define MODCAREER_BASE_RESPAWN_CHANCE 25
#define MODCAREER_EVENT_SETTINGS_MASK 0x1ff // bit 1 stays reserved for removed Simulant Deathmatch
extern s32 g_ModCareerDebugAlienWar; // Debug: add Alien War to the special events
extern s32 g_ModCareerEventsOff;       // special events left out: bit n is event n + 1
const char *modCareerEventName(s32 index);
s32 modCareerEventKind(s32 index);    // compact menu index to saved/internal event number
bool modCareerEventAllowed(s32 kind);  // saved/internal event number 1..6
extern char g_ModCareerOff[1536];    // the maps left out: stage numbers, comma separated
extern s32 g_ModCareerStepGuards;   // extra guards per wave, per mission
extern s32 g_ModCareerStepSpawn;    // % faster waves, per mission
extern s32 g_ModCareerStepAccuracy; // % enemy accuracy, per mission
extern s32 g_ModCareerStepAkimbo;   // % more guards dual wielding, per mission
extern s32 g_ModCareerStepRespawn;
extern s32 g_ModCareerStepDamage;   // % damage to the player, per mission
// Starting difficulty preset and the saved Custom starting points.
extern s32 g_ModCareerStartPreset;
extern s32 g_ModCareerStartGuards;
extern s32 g_ModCareerStartAccuracy;
extern s32 g_ModCareerStartAkimbo;
extern s32 g_ModCareerStartRespawn;

bool modCareerIsOn(void);
void modCareerStart(void);
void modCareerStop(void);

// A gauntlet mission was completed. True when the next one has been started,
// false when that was the last (the caller then ends the stage as usual).
bool modCareerOnMissionComplete(void);

// The stage is ending for any other reason (death, abort): the gauntlet is over.
void modCareerOnStageEnded(void);

s32 modCareerMission(void);           // 0-based index of the mission being played
s32 modCareerLength(void);            // mission limit, or MODCAREER_UNLIMITED
const char *modCareerGetStatus(void); // one line for the menu

// The pool: every map a gauntlet could be dealt, built from what is installed.
s32 modCareerPoolCount(void);
s32 modCareerPoolKind(s32 index);
s32 modCareerPoolStage(s32 index);
const char *modCareerPoolName(s32 index);
bool modCareerPoolMapVisible(s32 index);
bool modCareerPoolPicked(s32 index);
void modCareerPoolSetPicked(s32 index, bool picked);
s32 modCareerPoolNumPicked(void);
s32 modCareerKindNumPicked(s32 kind);
s32 modCareerKindCount(s32 kind);
void modCareerKindPick(s32 kind, bool picked); // all of a kind on or off

// Difficulty ramp, by mission. All are neutral (1.0 / 0 / 100) when no
// gauntlet is on.
s32 modCareerExtraWaveGuards(void);  // added to Enemies Per Wave
s32 modCareerWaveSeconds(void);
s32 modCareerSpawnPercent(void);     // % of the Spawn Interval (lower = faster)
f32 modCareerAccuracyScale(void);    // enemy accuracy
s32 modCareerShieldChance(void);       // % of guards with a shield
s32 modCareerGrenadierChance(void);   // % of guards that throw grenades
s32 modCareerAkimboChance(void);       // % of guards that dual wield
s32 modCareerRespawnChance(void);      // % chance a dead guard returns
f32 modCareerPlayerDamageScale(void);// damage enemies do to the player

// Kill objectives a mission asks for: 2 on the first, 5 on the last, between
// on the ones in the middle. 0 when no gauntlet is on (the roll's own count).
s32 modCareerKillObjectives(void);

// Boons: after each mission the player picks one for the missions that follow;
// it applies to that one mission only.
#define MODCAREER_NUM_OFFERS 8
s32 modCareerBoonCount(void);          // how many are on offer this mission
bool modCareerAchievementsShown(void); // true when there is a previous mission to list
void modCareerAchievementText(s32 index, char *buf, size_t size); // line of the achievement list (3 of them)
struct prop;
void modCareerNoteHit(struct chrdata *chr, struct prop *aprop, bool gunfire); // the player hit a chr (gunfire: not a punch, whip or dart)
const char *modCareerBoonLabel(s32 slot); // the text of offer slot
bool modCareerBoonPending(void);      // a pick is owed before the mission is played
void modCareerBoonTake(s32 slot);     // the player's pick (an offer slot)

// Permanent boons: offered after every third mission cleared, kept for the run.
enum {
	MODCAREER_PERM_THROWABLES, // Double Starting Throwables
	MODCAREER_PERM_PHONE,      // Free Friend Phone (every mission)
	MODCAREER_PERM_CIVS,       // Free Arm the Civs (every mission)
	MODCAREER_PERM_CIVSHIELD,  // 50% more shield on a civilian kill
	MODCAREER_PERM_KNIFE,      // Knife Expert: max knives, and they one-shot anything, shield or not
	MODCAREER_PERM_COUNT
};
bool modCareerPermPending(void);       // a permanent pick is owed before the mission is played
s32 modCareerPermCount(void);          // how many are on offer
const char *modCareerPermLabel(s32 slot);
void modCareerPermTake(s32 slot);
bool modCareerHasPerm(s32 perm);
s32 modCareerThrowableScale(void);     // 2 with Double Starting Throwables
f32 modCareerCivShieldScale(void);     // 1.5 with 50% More Shield on Civ Kill
// Mine Layer: 4 enemies killed by one of the player's grenades earns it as an offer next mission.
void modCareerGrenadeExploding(bool on); // a player grenade is exploding: explosions made now are its
s16 modCareerGrenadeTag(void);           // the id to give an explosion being made
void modCareerGrenadeSource(s16 id);     // damage from this grenade's explosion is being dealt (0: not)
bool modCareerSpendCharge(s32 weaponnum); // a Friend Phone / Arm The Civs was used: true if another is left (keep the item)
void modCareerCloakReward(void);
void modCareerAlienWarReward(bool maian); // offer a faction reward on the next map
void modCareerBlondReward(void); // offer the Devastator boon on the next map
struct chrdata;
void modCareerRecordDeath(struct chrdata *chr, s32 aplayernum); // a chr died (aplayernum >= 0: by the player)
void modCareerRecordKnockout(struct chrdata *chr);
bool modCareerLeaveToGauntletPage(void); // the closing screen's button: back to the Gauntlet page, not the title
bool modCareerGauntletPageDue(void);
void modCareerGauntletPageDone(void);
bool modCareerSummaryTake(bool *won); // true once after a gauntlet ends: show its closing screen
void modCareerSummaryText(char lines[][64], char *title, size_t titlesize);
bool modCareerObjectiveShield(void);   // the modifier gives kill targets shields
void modCareerModsText(char *buf, size_t size); // "Modifiers: ..." for the briefing
void modCareerTickMods(void);          // once a frame: the enemy modifiers
void modCareerBriefAck(void);         // the enemy briefing was confirmed
const char *modCareerRampNew(void);
const char *modCareerModNew(void);
const char *modCareerRampNote(void);  // what the last mission made worse
void modCareerTick(void);             // once a frame while a mission is played
#ifndef PLATFORM_N64
Gfx *modCareerRenderHudNames(Gfx *gdl);
#endif


#endif
