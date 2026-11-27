#include <ultra64.h>
#include "constants.h"
#include "prcompat.h"
#include "bss.h"
#include "game/game_0b0fd0.h"
#include "game/modcareer.h"
#include "game/chraction.h"
#include "game/hudmsg.h"
#include "game/modghost.h"
#include "game/modoptions.h"
#include "game/modrandom.h"
#include "game/modrun.h"
#include "game/lv.h"
#include "game/pdmode.h"
#include "game/bondgun.h"
#include "game/inv.h"
#include "game/player.h"
#include "game/title.h"
#include "lib/main.h"
#include "lib/rng.h"
#include "lib/vi.h"
#include "data.h"
#include "types.h"
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include "system.h"
#include "game/lang.h"
#include "game/stagetable.h"
#ifndef PLATFORM_N64
#include "game/mplayer/setup.h"
#include "game/game_1531a0.h"
#include "modloader.h"
#include "arenatabs.h"
#include "config.h"
#include "levelgenstage.h"
#include "game/modlevelgen.h"
#endif

/**
 * Perfect Random Gauntlet.
 *
 * Perfect Random missions back to back. Each is an ordinary Perfect
 * Random mission (the roll, the objectives, the reinforcement waves) - what the
 * gauntlet adds is the order and a difficulty ramp that is read from here by
 * the code that already owns each thing:
 *
 *   - wave size: modGetRandomSpawnCount() adds modCareerExtraWaveGuards()
 *   - wave rate: modalarm.c's modAlarmGetInterval60() uses modCareerSpawnPercent()
 *   - accuracy:  chraction.c's chrs shooting use modCareerAccuracyScale()
 *   - damage:    chraction.c's enemy-hits-player uses modCareerPlayerDamageScale()
 *
 * The mission is completed by modrandom.c's usual objective check, which asks
 * modCareerOnMissionComplete() before it ends the stage. Any other way the
 * stage can end (death, Abort Mission) goes through mainEndStage(), which calls
 * modCareerOnStageEnded() - the gauntlet is lost there.
 */

s32 g_ModCareerLength = MODCAREER_DEFAULTMISSIONS;
s32 g_ModCareerBest;
s32 g_ModCareerHideUnfunMaps = 1;
char g_ModCareerOff[1536] = "";
s32 g_ModCareerEventsOff = 0;

// Alien War (6) is not in the list: it is a performance test, off unless the
// Debug menu's "Add Alien War (performance)" is on.
static const s32 g_ModCareerEventKinds[MODCAREER_NUMEVENTS] = { 1, 3, 4, 5, 7, 8, 9 };
s32 g_ModCareerDebugAlienWar = 0;

s32 modCareerEventKind(s32 index)
{
	return index >= 0 && index < MODCAREER_NUMEVENTS ? g_ModCareerEventKinds[index] : 0;
}

const char *modCareerEventName(s32 index)
{
	const char *name = modRandomEventName(modCareerEventKind(index));
	return name ? name : "Event";
}

bool modCareerEventAllowed(s32 kind)
{
	// Event 2 was Simulant Deathmatch. Keep its saved-settings bit reserved so
	// existing pd.ini event selections retain their meaning.
	if (kind == 6) {
		return g_ModCareerDebugAlienWar != 0; // Alien War: debug option only
	}

	return kind >= 1 && kind <= 9 && kind != 2 && (g_ModCareerEventsOff & (1 << (kind - 1))) == 0;
}
s32 g_ModCareerStepGuards = 2;
s32 g_ModCareerStepSpawn = 25;
s32 g_ModCareerStepAccuracy = 30;
s32 g_ModCareerStepDamage = 20; // unused: damage is no longer ramped
s32 g_ModCareerStepAkimbo = 10;  // % more guards dual wielding, per mission
s32 g_ModCareerStepRespawn = 25;
s32 g_ModCareerStartPreset = 2; // Perfect Agent preserves today's opening
s32 g_ModCareerStartGuards = 0;
s32 g_ModCareerStartAccuracy = 1;
s32 g_ModCareerStartAkimbo = 0;
s32 g_ModCareerStartRespawn = 0;

static bool g_ModCareerOn;
static s32 g_ModCareerMission;

// Each mission after the first makes the enemy one step worse in exactly one
// way, chosen at random; the levels add up over the gauntlet.
enum { RAMP_GUARDS, RAMP_ACCURACY, RAMP_AKIMBO, RAMP_RESPAWN, RAMP_COUNT };
static s32 g_ModCareerLevel[RAMP_COUNT];
static s32 g_ModCareerInitialRespawnChance;
static const char *g_ModCareerRampText[RAMP_COUNT] = {
	"Bigger reinforcement waves",
	"Enemies shoot more accurately",
	"More enemies dual wield",
	"Higher enemy respawn chance",
};
static const char *g_ModCareerAnnounce = NULL;
static s32 g_ModCareerLastRamp = -1;

static void modCareerRollRamp(void)
{
	if (g_ModCareerMission <= 0) {
		// the first mission always carries an accuracy buff
		g_ModCareerLastRamp = RAMP_ACCURACY;
		g_ModCareerLevel[RAMP_ACCURACY]++;
		g_ModCareerAnnounce = g_ModCareerRampText[RAMP_ACCURACY];
		return;
	}

	{
		// never the same stat two missions running
		s32 which = rngRandom() % RAMP_COUNT;

		if (which == g_ModCareerLastRamp) {
			which = (which + 1 + rngRandom() % (RAMP_COUNT - 1)) % RAMP_COUNT;
		}

		g_ModCareerLastRamp = which;

		g_ModCareerLevel[which]++;
		g_ModCareerAnnounce = g_ModCareerRampText[which];
		sysLogPrintf(LOG_NOTE, "gauntlet: mission %d ramps stat %d to level %d", g_ModCareerMission + 1, which, g_ModCareerLevel[which]);
	}
}
static s32 g_ModCareerUsed[MODCAREER_MAXPOOL];
static s32 g_ModCareerNumUsed;
static char g_ModCareerStatus[96] = "";

// Boons picked so far, whether one is still owed for the mission just loaded,
// and whether this mission's have been handed out yet.
static s32 g_ModCareerBoonPick = -1;   // pool entry taken for this mission, -1 for none
static s32 g_ModCareerOffers[MODCAREER_NUM_OFFERS]; // the pool entries on offer
static char g_ModCareerOfferLabels[MODCAREER_NUM_OFFERS][48];
static bool g_ModCareerBoonPending;
static bool g_ModCareerBoonApplied;
#ifndef PLATFORM_N64
static char g_ModCareerMapName[48];
static u32 g_ModCareerMapNameStart60;
#endif
static bool g_ModCareerBriefDone; // the enemy briefing has been confirmed for this mission
static bool g_ModCareerCloakReward;
static bool g_ModCareerMineLayerHit;   // 4 kills with one grenade this mission
static bool g_ModCareerMineLayerOffer; // ...earned last mission: offered now
static s16 g_ModCareerGrenadePending;  // the id an explosion made now gets
static s16 g_ModCareerGrenadeCurrent;  // whose explosion is dealing damage now
static s32 g_ModCareerGrenadeNext;
static struct { s16 id; s16 kills; } g_ModCareerGrenadeTally[8];
static s32 g_ModCareerGrenadeTallyPos;

void modCareerGrenadeExploding(bool on)
{
	if (on && g_ModCareerOn) {
		g_ModCareerGrenadeNext = g_ModCareerGrenadeNext >= 30000 ? 1 : g_ModCareerGrenadeNext + 1;
		g_ModCareerGrenadePending = g_ModCareerGrenadeNext;
	} else {
		g_ModCareerGrenadePending = 0;
	}
}

s16 modCareerGrenadeTag(void)
{
	return g_ModCareerGrenadePending;
}

void modCareerGrenadeSource(s16 id)
{
	g_ModCareerGrenadeCurrent = id;
}
static s32 g_ModCareerAlienReward; // bits: 1 Maian backup, 2 Skedar swarm, offered on next map
static bool g_ModCareerBlondReward; // Mr Blonde killed in his revenge: a Devastator is offered on next map

void modCareerBriefAck(void)
{
	g_ModCareerBriefDone = true;
}

// Every third mission the gauntlet adds a permanent modifier, drawn from this
// list without repeats; they stay for the rest of the run.
enum { MOD_OBJECTIVE_SHIELD, MOD_SHIELDED, MOD_GRENADIERS, MOD_COUNT };
static const char *g_ModCareerModNames[MOD_COUNT] = {
	"Objective shields", "10% shielded", "20% grenadiers",
};
static const char *g_ModCareerModAnnounce[MOD_COUNT] = {
	"Objective gets shields", "10% of enemies have shields",
	"20% of enemies throw grenades",
};
static bool g_ModCareerModOn[MOD_COUNT];
static const char *g_ModCareerModNew;

static void modCareerResetMods(void)
{
	memset(g_ModCareerModOn, 0, sizeof(g_ModCareerModOn));
	g_ModCareerModNew = NULL;
}

static void modCareerRollMod(void)
{
	s32 left[MOD_COUNT];
	s32 n = 0;
	s32 i;

	for (i = 0; i < MOD_COUNT; i++) {
		if (!g_ModCareerModOn[i]) {
			left[n++] = i;
		}
	}

	if (n > 0) {
		const s32 pick = left[rngRandom() % n];

		g_ModCareerModOn[pick] = true;
		g_ModCareerModNew = g_ModCareerModAnnounce[pick];
		sysLogPrintf(LOG_NOTE, "gauntlet: mission %d adds modifier %d", g_ModCareerMission + 1, pick);
	}
}

bool modCareerObjectiveShield(void)
{
	return g_ModCareerOn && g_ModCareerModOn[MOD_OBJECTIVE_SHIELD];
}

void modCareerModsText(char *buf, size_t size)
{
	size_t used;
	s32 count = 0;
	s32 i;

	snprintf(buf, size, "Modifiers:");

	for (i = 0; i < MOD_COUNT; i++) {
		if (g_ModCareerModOn[i]) {
			used = strlen(buf);
			snprintf(buf + used, size - used, "%s %s", count ? "," : "", g_ModCareerModNames[i]);
			count++;
		}
	}

	if (count == 0) {
		snprintf(buf, size, "Modifiers: none yet");
	}
}

extern struct prop *chrGiveWeaponWithAutoModel(struct chrdata *chr, s32 weaponnum, u32 flags);

#define MODCAREER_MAXMEMO 512
static struct { s32 chrnum; void *model; } g_ModCareerMemo[MODCAREER_MAXMEMO];

static bool modCareerPercent(s32 chrnum, u32 salt, u32 pct)
{
	u32 h = ((u32)chrnum + salt) * 2654435761u;

	h ^= h >> 15;
	h *= 2246822519u;

	return ((h >> 8) % 100) < pct;
}

/**
 * The modifiers on the enemies already in the level. Each enemy is looked at
 * once (by chr number and model, as a slot is reused), and which of them are
 * picked is by number, so a given enemy is the same in every run of a seed.
 */
void modCareerTickMods(void)
{
	struct player *player = g_Vars.bond;
	s32 i;

	if (!g_ModCareerOn || !player || !player->prop || g_Vars.currentplayer != player || lvIsPaused()
			|| (!g_ModCareerModOn[MOD_SHIELDED] && !modCareerAkimboChance() && !g_ModCareerModOn[MOD_GRENADIERS])) {
		return;
	}

	for (i = 0; i < g_NumChrSlots && i < MODCAREER_MAXMEMO; i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr->chrnum < 0 || !chr->prop || !chr->model || chr->prop->type != PROPTYPE_CHR
				|| chr == player->prop->chr || chr->race != RACE_HUMAN || chrIsDead(chr)
				|| (chr->team & (TEAM_ALLY | TEAM_NONCOMBAT | TEAM_00 | 0x6c)) // 0x6c: the special events' teams
				|| modRandomIsObjectiveChr(chr->chrnum)) {
			continue;
		}

		if (g_ModCareerMemo[i].chrnum == chr->chrnum && g_ModCareerMemo[i].model == chr->model) {
			continue;
		}

		// dual wielding needs the gun in the hand first
		if (modCareerAkimboChance() && !chr->weapons_held[HAND_RIGHT]) {
			continue;
		}

		g_ModCareerMemo[i].chrnum = chr->chrnum;
		g_ModCareerMemo[i].model = chr->model;

		if (g_ModCareerModOn[MOD_SHIELDED] && modCareerPercent(chr->chrnum, 0x1111, 10)) {
			chrSetShield(chr, 4.0f);
		}

		if (g_ModCareerModOn[MOD_GRENADIERS] && modCareerPercent(chr->chrnum, 0x2222, 20)) {
			chr->flags |= CHRFLAG0_CAN_THROW_GRENADES;
		}

		if (modCareerAkimboChance() && modCareerPercent(chr->chrnum, 0x3333, modCareerAkimboChance())
				&& !chr->weapons_held[HAND_LEFT]) {
			const s32 gun = chr->weapons_held[HAND_RIGHT]->weapon->weaponnum;

			if (modCanAkimbo(gun)) {
				chrGiveWeaponWithAutoModel(chr, gun, OBJFLAG_WEAPON_LEFTHANDED);
			}
		}
	}
}

// What the whole gauntlet adds up to, for the closing screen. Kept to what the
// player did: kills, civilians, knock outs and friendlies are the player's own.
static s32 g_ModCareerStatKills, g_ModCareerStatCivs, g_ModCareerStatKnockouts, g_ModCareerStatFriendlies;
static s32 g_ModCareerStatShots, g_ModCareerStatHits;
static s32 g_ModCareerStatMissions;       // missions cleared
static bool g_ModCareerSummaryPending;
static bool g_ModCareerSummaryWon;

static void modCareerResetStats(void)
{
	g_ModCareerStatKills = g_ModCareerStatCivs = g_ModCareerStatKnockouts = g_ModCareerStatFriendlies = 0;
	g_ModCareerStatShots = g_ModCareerStatHits = 0;
	g_ModCareerStatMissions = 0;
	g_ModCareerSummaryPending = false;
	g_ModCareerSummaryWon = false;
}

/** Achievements of the mission just played: each one unlocks a boon for the next. */
enum { ACH_CIVS, ACH_BUDDY, ACH_SURRENDER, ACH_COUNT };
static const char *g_ModCareerAchName[ACH_COUNT] = {
	"No harm to civilians", "No harm to your partners", "No surrendering guard killed",
};
static bool g_ModCareerHurt[ACH_COUNT];   // broken during the mission being played
static bool g_ModCareerEarned[ACH_COUNT]; // earned in the mission before: what the boon pick is built from
static u8 g_ModCareerShotSurrender[1024]; // by chrnum: hit by the player while surrendering

void modCareerRecordDeath(struct chrdata *chr, s32 aplayernum)
{
	if (!g_ModCareerOn || !chr || aplayernum < 0) {
		return;
	}

	if (chr->chrnum >= 0 && g_ModCareerShotSurrender[chr->chrnum & 1023]
			&& chr->team != TEAM_NONCOMBAT && chr->team != TEAM_ALLY && !modRandomIsCivilianChr(chr)) {
		g_ModCareerHurt[ACH_SURRENDER] = true;
	}

	if (chr->team == TEAM_NONCOMBAT || modRandomIsCivilianChr(chr)) {
		g_ModCareerHurt[ACH_CIVS] = true; // killed, however it was done
		g_ModCareerStatCivs++;

		// Every civilian the player kills gives a grenade (up to the most they can hold)
		{
			const s32 ammo = bgunGetAmmoTypeForWeapon(WEAPON_GRENADE, FUNC_PRIMARY);
			const s32 held = bgunGetAmmoCount(ammo);

			if (held < bgunGetCapacityByAmmotype(ammo)) {
				invGiveSingleWeapon(WEAPON_GRENADE);
				bgunSetAmmoQuantity(ammo, held + 1);
			}
		}
	} else if (chr->team == TEAM_ALLY) {
		g_ModCareerStatFriendlies++;
	} else {
		g_ModCareerStatKills++;

		if (g_ModCareerGrenadeCurrent > 0) {
			s32 slot = -1;

			for (s32 i = 0; i < 8; i++) {
				if (g_ModCareerGrenadeTally[i].id == g_ModCareerGrenadeCurrent) {
					slot = i;
				}
			}

			if (slot < 0) {
				slot = g_ModCareerGrenadeTallyPos++ % 8;
				g_ModCareerGrenadeTally[slot].id = g_ModCareerGrenadeCurrent;
				g_ModCareerGrenadeTally[slot].kills = 0;
			}

			if (++g_ModCareerGrenadeTally[slot].kills >= 4 && !g_ModCareerMineLayerHit) {
				g_ModCareerMineLayerHit = true;
				hudmsgCreateWithFlags("4 kills with one grenade: Mine Layer next mission\n", HUDMSGTYPE_DEFAULT,
						HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
			}
		}
	}
}

void modCareerRecordKnockout(struct chrdata *chr)
{
	if (g_ModCareerOn && chr && chr->team != TEAM_ALLY && chr->team != TEAM_NONCOMBAT) {
		g_ModCareerStatKnockouts++;
	}
}

/** The mission just played, added to the totals (its own counters reset with the next stage). */
static void modCareerBankShots(void)
{
	const struct playerstats *stats = &g_Vars.playerstats[g_Vars.bondplayernum < 0 ? 0 : g_Vars.bondplayernum];
	s32 hits = 0;
	s32 i;

	for (i = SHOTREGION_HEAD; i <= SHOTREGION_OBJECT; i++) {
		hits += stats->shotcount[i];
	}

	g_ModCareerStatShots += stats->shotcount[SHOTREGION_TOTAL];
	g_ModCareerStatHits += hits;
}

static bool g_ModCareerLeaveToMenu;   // the closing screen is up: its button goes back to the Gauntlet page
static bool g_ModCareerOpenOnArrival; // and the page is opened once the Institute is loaded

/** The closing screen was dismissed: true once, if it should go to the Gauntlet page and not the title. */
bool modCareerLeaveToGauntletPage(void)
{
	const bool leave = g_ModCareerLeaveToMenu;

	g_ModCareerLeaveToMenu = false;
	g_ModCareerOpenOnArrival = leave;

	return leave;
}

/** True once, in the stage the closing screen went to, when the Gauntlet page should open. */
bool modCareerGauntletPageDue(void)
{
	return g_ModCareerOpenOnArrival;
}

void modCareerGauntletPageDone(void)
{
	g_ModCareerOpenOnArrival = false;
}

bool modCareerSummaryTake(bool *won)
{
	const bool pending = g_ModCareerSummaryPending;

	g_ModCareerSummaryPending = false;
	g_ModCareerLeaveToMenu = pending;
	*won = g_ModCareerSummaryWon;

	return pending;
}

void modCareerSummaryText(char lines[][64], char *title, size_t titlesize)
{
	const s32 acc = g_ModCareerStatShots > 0
		? (s32)((g_ModCareerStatHits > g_ModCareerStatShots ? g_ModCareerStatShots : g_ModCareerStatHits) * 1000 / g_ModCareerStatShots)
		: 0;

	snprintf(title, titlesize, g_ModCareerSummaryWon ? "Gauntlet Complete" : "Gauntlet Over");
	if (modCareerLength() == MODCAREER_UNLIMITED) {
		snprintf(lines[0], 64, "Missions cleared: %d\n", g_ModCareerStatMissions);
	} else {
		snprintf(lines[0], 64, "Missions cleared: %d of %d\n", g_ModCareerStatMissions, modCareerLength());
	}
	snprintf(lines[1], 64, "Total kills: %d\n", g_ModCareerStatKills);
	snprintf(lines[2], 64, "Civs killed: %d\n", g_ModCareerStatCivs);
	snprintf(lines[3], 64, "Knock outs: %d\n", g_ModCareerStatKnockouts);
	snprintf(lines[4], 64, "Friendlies killed: %d\n", g_ModCareerStatFriendlies);
	snprintf(lines[5], 64, "Total accuracy: %d.%d%%\n", acc / 10, acc % 10);
}

/** The stat this mission made harder, or NULL (the first mission, or already announced). */
const char *modCareerRampNew(void)
{
	return g_ModCareerAnnounce;
}

/** The modifier this mission added, or NULL. */
const char *modCareerModNew(void)
{
	return g_ModCareerModNew;
}

const char *modCareerRampNote(void)
{
	return g_ModCareerAnnounce ? g_ModCareerAnnounce : (g_ModCareerMission > 0 ? "Harder: more of the same" : "No buffs yet");
}

#ifndef PLATFORM_N64
bool randomMenuBriefShow(void); // port/src/randommenu.c: true while the briefing is up
bool randomMenuBoonShow(void); // port/src/randommenu.c: true while the pick is up
bool randomMenuPermShow(void); // port/src/randommenu.c: true while the permanent pick is up
#endif

static bool modCareerBoonsLeft(void)
{
	return true; // every mission offers all of them again
}

// Boon pool "achievement" codes of the three always-offered starting boons
// (negative: not unlocked by an achievement; -1 is the old "always").
#define MODCAREER_BOON_GRENADES  -5
#define MODCAREER_BOON_RIFLEAMMO -6
#define MODCAREER_BOON_AKIMBO    -7
#define MODCAREER_BOON_SHOTGUN   -9

struct modcareerboon {
	s32 weaponnum; // 0 for the shield, which is not an item
	s32 ammotype;  // 0 for an item with no ammunition
	s32 quantity;  // rounds; for the shield, the percentage added
	const char *name;
	s32 achievement; // the achievement that unlocks it, -1 for always
};

static const struct modcareerboon g_ModCareerBoonPool[] = {
	{ WEAPON_GRENADE,          0,                0,   "Extra Grenades",       MODCAREER_BOON_GRENADES },  // +2 grenades
	{ WEAPON_AR34,             AMMOTYPE_RIFLE,   150, "Rifle Ammo Bag (150)", MODCAREER_BOON_RIFLEAMMO }, // +150 rifle rounds
	{ WEAPON_FALCON2,          0,                0,   "Akimbo Pistol",        MODCAREER_BOON_AKIMBO },    // the starting pistol, in both hands
	{ WEAPON_SHOTGUN,           0,                0,   "Super Shotgun",       MODCAREER_BOON_SHOTGUN },   // with its ammunition full
	{ WEAPON_PRESIDENTSCANNER, 0,                0,   "Arm The Civs", ACH_CIVS },      // see modrandom.c
	{ WEAPON_TESTER,           0,                0,   "Friend Phone", ACH_BUDDY },     // see modrandom.c
	{ WEAPON_PSYCHOSISGUN,     AMMOTYPE_PSYCHOSIS, 24, "Psychosis Gun", ACH_SURRENDER }, // 24 rounds
	{ WEAPON_DISGUISE40,       0,                0,   "Maian Backup", -2 },
	{ WEAPON_DISGUISE41,       0,                0,   "Skedar Swarm", -2 },
	{ WEAPON_CLOAKINGDEVICE, AMMOTYPE_CLOAK, 30, "Cloaking Device", -3 },
	{ WEAPON_TIMEDMINE,      0,                0,   "Mine Layer",  -4 }, // 3 of each mine; earned by 4 kills with one grenade
	{ WEAPON_DEVASTATOR,     AMMOTYPE_DEVASTATOR, 16, "Devastator", -8 }, // 16 rounds; earned by killing Mr Blonde in his revenge
};

static s32 g_ModCareerNumOffers;

/*
 * Permanent boons: every third mission cleared (before missions 4, 7, 10...)
 * the player picks one more, from a window of its own (green, not the blue
 * mission pick), and keeps it for the rest of the run. Each can be taken once.
 */
// (the window is as wide as the screen allows: about 20 letters of its font)
static const char *const g_ModCareerPermName[MODCAREER_PERM_COUNT] = {
	"Double Throwables",    // Double Starting Throwables
	"Free Friend Phone",
	"Free Arm the Civs",
	"+50% Civ Kill Shield", // 50% more shield on a civilian kill
	"Knife Expert",         // max knives; a knife kills in one hit, shield or not
};
static bool g_ModCareerPerm[MODCAREER_PERM_COUNT];
static bool g_ModCareerPermNew[MODCAREER_PERM_COUNT]; // taken at the start of this mission
static s32 g_ModCareerPermOffers[MODCAREER_PERM_COUNT];
static s32 g_ModCareerNumPermOffers;
static bool g_ModCareerPermPending;

// Friend Phones and Arm The Civs held this mission: a boon and a permanent
// boon stack, and using one keeps the item while another is left.
static s32 g_ModCareerCharges[2];

static s32 modCareerChargeIndex(s32 weaponnum)
{
	return weaponnum == WEAPON_TESTER ? 0 : weaponnum == WEAPON_PRESIDENTSCANNER ? 1 : -1;
}

static void modCareerAddCharge(s32 weaponnum)
{
	const s32 i = modCareerChargeIndex(weaponnum);

	invGiveSingleWeapon(weaponnum);

	if (i >= 0) {
		g_ModCareerCharges[i]++;
	}
}

bool modCareerSpendCharge(s32 weaponnum)
{
	const s32 i = modCareerChargeIndex(weaponnum);

	if (!g_ModCareerOn || i < 0 || g_ModCareerCharges[i] <= 1) {
		if (i >= 0) {
			g_ModCareerCharges[i] = 0;
		}
		return false;
	}

	g_ModCareerCharges[i]--;
	{
		char text[64];

		snprintf(text, sizeof(text), "%s: %d left\n", i == 0 ? "Friend Phone" : "Arm The Civs", g_ModCareerCharges[i]);
		hudmsgCreateWithFlags(text, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
	}
	return true;
}

bool modCareerHasPerm(s32 perm)
{
	return g_ModCareerOn && perm >= 0 && perm < MODCAREER_PERM_COUNT && g_ModCareerPerm[perm];
}

s32 modCareerThrowableScale(void)
{
	return modCareerHasPerm(MODCAREER_PERM_THROWABLES) ? 2 : 1;
}

f32 modCareerCivShieldScale(void)
{
	return modCareerHasPerm(MODCAREER_PERM_CIVSHIELD) ? 1.5f : 1.0f;
}

static void modCareerRollPermOffers(void)
{
	g_ModCareerNumPermOffers = 0;

	for (s32 i = 0; i < MODCAREER_PERM_COUNT; i++) {
		if (!g_ModCareerPerm[i]) {
			g_ModCareerPermOffers[g_ModCareerNumPermOffers++] = i;
		}
	}

	g_ModCareerPermPending = g_ModCareerNumPermOffers > 0;
}

bool modCareerPermPending(void)
{
	return g_ModCareerOn && g_ModCareerPermPending;
}

s32 modCareerPermCount(void)
{
	return g_ModCareerNumPermOffers;
}

const char *modCareerPermLabel(s32 slot)
{
	return slot >= 0 && slot < g_ModCareerNumPermOffers ? g_ModCareerPermName[g_ModCareerPermOffers[slot]] : "";
}

void modCareerPermTake(s32 slot)
{
	if (slot >= 0 && slot < g_ModCareerNumPermOffers) {
		const s32 perm = g_ModCareerPermOffers[slot];

		g_ModCareerPerm[perm] = true;
		g_ModCareerPermNew[perm] = true;
		sysLogPrintf(LOG_NOTE, "gauntlet: permanent boon %d (%s) taken", perm, g_ModCareerPermName[perm]);
	}

	g_ModCareerPermPending = false;
}

/** Deal this mission's offers: the defaults, and a boon for each achievement earned last mission. */
static f32 g_ModCareerCarryShield; // shield kept from the last mission

static void modCareerRollOffers(void)
{
	s32 i;

	g_ModCareerNumOffers = 0;

	for (i = 0; i < (s32)ARRAYCOUNT(g_ModCareerBoonPool) && g_ModCareerNumOffers < MODCAREER_NUM_OFFERS; i++) {
		const struct modcareerboon *boon = &g_ModCareerBoonPool[i];

		if (boon->achievement >= 0 && !g_ModCareerEarned[boon->achievement]) {
			continue;
		}
		if (boon->achievement == -2 && ((boon->weaponnum == WEAPON_DISGUISE40 && !(g_ModCareerAlienReward & 1))
				|| (boon->weaponnum == WEAPON_DISGUISE41 && !(g_ModCareerAlienReward & 2)))) continue;

		if (boon->achievement == -8 && !g_ModCareerBlondReward) continue;

		if (boon->achievement == -3 && !g_ModCareerCloakReward) continue;

		if (boon->achievement == -4 && !g_ModCareerMineLayerOffer) continue;

		g_ModCareerOffers[g_ModCareerNumOffers] = i;

		if (boon->ammotype && boon->quantity < 100) {
			snprintf(g_ModCareerOfferLabels[g_ModCareerNumOffers], sizeof(g_ModCareerOfferLabels[0]), "%s (%d)",
					boon->name, boon->quantity);
		} else {
			snprintf(g_ModCareerOfferLabels[g_ModCareerNumOffers], sizeof(g_ModCareerOfferLabels[0]), "%s", boon->name);
		}

		g_ModCareerNumOffers++;
	}
	g_ModCareerAlienReward = 0;
	g_ModCareerBlondReward = false;
	g_ModCareerCloakReward = false;
	g_ModCareerMineLayerOffer = false;
}

void modCareerCloakReward(void)
{
	if (g_ModCareerOn) g_ModCareerCloakReward = true;
}

void modCareerAlienWarReward(bool maian)
{
	if (g_ModCareerOn) g_ModCareerAlienReward |= maian ? 1 : 2;
}

void modCareerBlondReward(void)
{
	if (g_ModCareerOn) g_ModCareerBlondReward = true;
}

s32 modCareerBoonCount(void)
{
	return g_ModCareerNumOffers;
}

/** The player did something to a chr: note any achievement it breaks. */
void modCareerNoteHit(struct chrdata *chr, struct prop *aprop, bool gunfire)
{
	if (!g_ModCareerOn || !chr || !aprop || aprop->type != PROPTYPE_PLAYER || chr->prop == aprop) {
		return;
	}

	if (chr->team == TEAM_NONCOMBAT || modRandomIsCivilianChr(chr)) {
		// shooting one fails it even if they live; a punch, a pistol whip or a
		// dart that leaves them standing or knocked out does not (a kill does:
		// see modCareerRecordDeath)
		if (gunfire) {
			g_ModCareerHurt[ACH_CIVS] = true;
		}
	} else if (chr->team == TEAM_ALLY) {
		g_ModCareerHurt[ACH_BUDDY] = true;
	} else if (chr->actiontype == ACT_SURRENDER && chr->chrnum >= 0) {
		// only killing a surrendering guard breaks the achievement (see
		// modCareerRecordDeath); a knock out is fine
		g_ModCareerShotSurrender[chr->chrnum & 1023] = 1;
	}
}

/** The mission just ended well: what was earned in it decides the next pick. */
static void modCareerBankAchievements(void)
{
	s32 i;

	for (i = 0; i < ACH_COUNT; i++) {
		g_ModCareerEarned[i] = !g_ModCareerHurt[i];
		g_ModCareerHurt[i] = false;
	}

	memset(g_ModCareerShotSurrender, 0, sizeof(g_ModCareerShotSurrender));
}

static void modCareerClearAchievements(void)
{
	memset(g_ModCareerHurt, 0, sizeof(g_ModCareerHurt));
	memset(g_ModCareerShotSurrender, 0, sizeof(g_ModCareerShotSurrender));
	memset(g_ModCareerEarned, 0, sizeof(g_ModCareerEarned));
}

/** True once there is a mission behind the pick: the list below is only meaningful then. */
bool modCareerAchievementsShown(void)
{
	return g_ModCareerMission > 0;
}

/** One line of the list shown with the pick: what was earned (or not) and the boon it brings. */
void modCareerAchievementText(s32 index, char *buf, size_t size)
{
	static const char *const reward[ACH_COUNT] = { "Arm The Civs", "Friend Phone", "Psychosis Gun" };

	if (index < 0 || index >= ACH_COUNT) {
		snprintf(buf, size, "%s", "");
		return;
	}

	snprintf(buf, size, "%s: %s (%s)\n", g_ModCareerEarned[index] ? "Earned" : "Missed",
			g_ModCareerAchName[index], reward[index]);
}

const char *modCareerBoonLabel(s32 slot)
{
	return slot >= 0 && slot < g_ModCareerNumOffers ? g_ModCareerOfferLabels[slot] : "";
}

bool modCareerBoonPending(void)
{
	return g_ModCareerOn && g_ModCareerBoonPending;
}

void modCareerBoonTake(s32 slot)
{
	if (slot >= 0 && slot < g_ModCareerNumOffers) {
		g_ModCareerBoonPick = g_ModCareerOffers[slot];
	}

	g_ModCareerBoonPending = false;
	sysLogPrintf(LOG_NOTE, "gauntlet: boon slot %d taken (pool %d)", slot, g_ModCareerBoonPick);
}

/**
 * Called every frame of a gauntlet mission. Once the player is in control the
 * pick owed from the last mission is put to them (the game stops while they
 * choose), and then the boons they hold are handed out, once per mission.
 */
void modCareerTick(void)
{
	struct player *pl = g_Vars.currentplayer;

	if (!g_ModCareerOn || g_ModCareerBoonApplied || !pl || !pl->prop || pl->isdead
			|| g_InCutscene || g_Vars.in_cutscene || g_Vars.lvframenum < 10) {
		return;
	}

#ifndef PLATFORM_N64
	if (!g_ModCareerBriefDone) {
		randomMenuBriefShow();
		return;
	}

	if (g_ModCareerPermPending) {
		randomMenuPermShow();
		return;
	}

	if (g_ModCareerBoonPending && modCareerBoonsLeft()) {
		randomMenuBoonShow();
		return;
	}
#endif

	g_ModCareerBoonPending = false;
	g_ModCareerBoonApplied = true;
#ifndef PLATFORM_N64
	g_ModCareerMapNameStart60 = g_Vars.lvframe60;
#endif

	if (g_ModCareerCarryShield > 0.0f) {
		playerSetShieldFrac(g_ModCareerCarryShield);
		g_ModCareerCarryShield = 0.0f;
	}

	if (g_ModCareerModNew) {
		char note[80];

		snprintf(note, sizeof(note), "New modifier: %s\n", g_ModCareerModNew);
		hudmsgCreateWithFlags(note, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
		g_ModCareerModNew = NULL;
	}

	if (g_ModCareerAnnounce) {
		char note[80];

		snprintf(note, sizeof(note), "Harder: %s\n", g_ModCareerAnnounce);
		hudmsgCreateWithFlags(note, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
		g_ModCareerAnnounce = NULL;
	}

	// the alien items are good for the one mission after the event: an unused one is taken back
	invRemoveItemByNum(WEAPON_DISGUISE40);
	invRemoveItemByNum(WEAPON_DISGUISE41);

	if (g_ModCareerBoonPick >= 0 && g_ModCareerBoonPick < (s32)ARRAYCOUNT(g_ModCareerBoonPool)) {
		const struct modcareerboon *boon = &g_ModCareerBoonPool[g_ModCareerBoonPick];
		char text[64];

		if (boon->weaponnum == 0) {
			const f32 frac = playerGetShieldFrac() + boon->quantity * 0.01f;

			playerSetShieldFrac(frac > 1.0f ? 1.0f : frac);
		} else if (boon->achievement == MODCAREER_BOON_GRENADES) {
			// Extra Grenades: 2 more than the starting 3 (up to the most that can be held)
			const s32 ammo = bgunGetAmmoTypeForWeapon(WEAPON_GRENADE, FUNC_PRIMARY);
			const s32 cap = bgunGetCapacityByAmmotype(ammo);
			const s32 held = bgunGetAmmoCount(ammo) + 2;

			invGiveSingleWeapon(WEAPON_GRENADE);
			bgunSetAmmoQuantity(ammo, held > cap ? cap : held);
		} else if (boon->achievement == MODCAREER_BOON_RIFLEAMMO) {
			// Rifle Ammo Bag: 150 rounds of rifle ammunition (AR34, K7 Avenger, Dragon...)
			const s32 cap = bgunGetCapacityByAmmotype(AMMOTYPE_RIFLE);
			const s32 held = bgunGetAmmoCount(AMMOTYPE_RIFLE) + boon->quantity;

			bgunSetAmmoQuantity(AMMOTYPE_RIFLE, held > cap ? cap : held);
		} else if (boon->achievement == MODCAREER_BOON_SHOTGUN) {
			// Super Shotgun: the gun, and as much ammunition as can be held
			const s32 ammo = bgunGetAmmoTypeForWeapon(WEAPON_SHOTGUN, FUNC_PRIMARY);

			invGiveSingleWeapon(WEAPON_SHOTGUN);
			bgunSetAmmoQuantity(ammo, bgunGetCapacityByAmmotype(ammo));
		} else if (boon->achievement == MODCAREER_BOON_AKIMBO) {
			// Akimbo Pistol: the pistol this mission started with, now one in each hand
			const s32 pistol = g_DefaultWeapons[HAND_RIGHT];

			if (pistol > WEAPON_UNARMED && weaponHasFlag(pistol, WEAPONFLAG_DUALWIELD) && invGiveDoubleWeapon(pistol, pistol)) {
				bgunEquipWeapon2(HAND_RIGHT, pistol);
				bgunEquipWeapon2(HAND_LEFT, pistol);
			} else {
				hudmsgCreateWithFlags("This pistol cannot be dual wielded\n", HUDMSGTYPE_DEFAULT,
						HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
			}
		} else if (boon->achievement == -4) {
			// Mine Layer: 3 of each mine (they are thrown from the quick throw)
			static const s32 mines[] = { WEAPON_TIMEDMINE, WEAPON_PROXIMITYMINE, WEAPON_REMOTEMINE };

			for (s32 m = 0; m < 3; m++) {
				const s32 ammo = bgunGetAmmoTypeForWeapon(mines[m], FUNC_PRIMARY);

				invGiveSingleWeapon(mines[m]);
				bgunSetAmmoQuantity(ammo, bgunGetReservedAmmoCount(ammo) + 3);
			}
		} else {
			modCareerAddCharge(boon->weaponnum);

			if (boon->ammotype) {
				bgunSetAmmoQuantity(boon->ammotype, boon->ammotype == AMMOTYPE_CLOAK ? boon->quantity * TICKS(60) : boon->quantity);
			}
		}

		snprintf(text, sizeof(text), "Boon: %s\n", boon->name);
		hudmsgCreateWithFlags(text, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
	}

	// The permanent boons: their items every mission, on top of the pick above
	// (Free Friend Phone + a Friend Phone pick = two of them).
	if (g_ModCareerPerm[MODCAREER_PERM_PHONE]) {
		modCareerAddCharge(WEAPON_TESTER);
	}

	if (g_ModCareerPerm[MODCAREER_PERM_CIVS]) {
		modCareerAddCharge(WEAPON_PRESIDENTSCANNER);
	}

	// Double Starting Throwables taken just now: the kit was dealt at the
	// spawn, before the pick, so the second half comes now.
	if (g_ModCareerPermNew[MODCAREER_PERM_THROWABLES]) {
		modRunTopUpStartingThrowables();
	}

	if (g_ModCareerPermNew[MODCAREER_PERM_KNIFE]) {
		modRunFillKnives();
	}

	memset(g_ModCareerPermNew, 0, sizeof(g_ModCareerPermNew));

	for (s32 i = 0; i < 2; i++) {
		if (g_ModCareerCharges[i] > 1) {
			char text[64];

			snprintf(text, sizeof(text), "%s x%d\n", i == 0 ? "Friend Phone" : "Arm The Civs", g_ModCareerCharges[i]);
			hudmsgCreateWithFlags(text, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
		}
	}
}

#ifndef PLATFORM_N64
Gfx *modCareerRenderHudNames(Gfx *gdl)
{
	if (!g_Vars.currentplayer || g_Vars.currentplayer != g_Vars.bond || g_Vars.currentplayer->isdead
			|| lvIsPaused() || g_InCutscene || g_Vars.in_cutscene) {
		return gdl;
	}

	const bool showMap = g_ModCareerOn && g_ModCareerBoonApplied && g_ModCareerMapName[0]
		&& g_Vars.lvframe60 - g_ModCareerMapNameStart60 < TICKS(5 * 60);
	const char *event = modRandomGetActiveEventName();
	if (!showMap && !event) {
		return gdl;
	}

	const s32 oldscale = g_ScaleX;
	g_ScaleX = g_ViRes == VIRES_HI ? 2 : 1;
	const s32 left = viGetViewLeft() / g_ScaleX + 27;
	/* Leave the first line clear for the optional FPS counter. */
	const s32 top = viGetViewTop() + 28;

	textBackupAndResetBlends();
	gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, g_HudAlignModeL);
	gdl = text0f153628(gdl);
	if (showMap) {
		s32 x = left;
		s32 y = top;
		gdl = textRender(gdl, &x, &y, g_ModCareerMapName, g_CharsHandelGothicSm, g_FontHandelGothicSm,
				0xffffffff, 0x000000b0, viGetWidth(), viGetHeight(), 0, 0);
	}
	if (event) {
		s32 x = left;
		s32 y = top + (showMap ? 14 : 0);
		gdl = textRender(gdl, &x, &y, (char *)event, g_CharsHandelGothicSm, g_FontHandelGothicSm,
				0x80caffff, 0x000000b0, viGetWidth(), viGetHeight(), 0, 0);
	}
	gdl = text0f153780(gdl);
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
	textRestoreBlends();
	g_ScaleX = oldscale;
	return gdl;
}
#endif

/**
 * The pool: Perfect Dark's missions, GoldenEye's (GE Plus), and the Combat
 * Simulator's maps, whichever of them this install has. Generated (PR) maps are
 * left out on purpose.
 *
 * Built from the game's own tables, so a new mod map joins it without anything
 * here changing. What is left out is remembered, not what is taken: a map that
 * arrives later is in until the player says otherwise.
 */
struct modcareerentry {
	s32 kind;
	s32 stage;
	char name[48];
};

static struct modcareerentry g_ModCareerPool[MODCAREER_MAXPOOL];
static s32 g_ModCareerPoolCount;
static s32 g_ModCareerPoolSig = -1;

static void modCareerPoolAdd(s32 kind, s32 stage, const char *name)
{
	struct modcareerentry *entry;
	s32 len;

	if (g_ModCareerPoolCount >= MODCAREER_MAXPOOL || stage <= 0) {
		return;
	}

	entry = &g_ModCareerPool[g_ModCareerPoolCount++];
	entry->kind = kind;
	entry->stage = stage;
	snprintf(entry->name, sizeof(entry->name), "%s", name && name[0] ? name : "Map");

	// Perfect Dark's names end in a line break
	len = strlen(entry->name);
	while (len > 0 && (entry->name[len - 1] == '\n' || entry->name[len - 1] == ' ')) {
		entry->name[--len] = '\0';
	}
}

static char *modCareerOffFind(s32 stage);

static bool modCareerPoolHasStage(s32 stage)
{
	s32 i;

	for (i = 0; i < g_ModCareerPoolCount; i++) {
		if (g_ModCareerPool[i].stage == stage) {
			return true;
		}
	}

	return false;
}

static void modCareerPoolBuild(void)
{
	s32 i;

	g_ModCareerPoolCount = 0;

	// Perfect Dark's missions, up to Skedar Ruins; the four special stages
	// after it (Mr. Blonde's Revenge, Maian SOS, War!, The Duel) are not missions
	for (i = 0; i < NUM_SOLOSTAGES; i++) {
		if (g_SoloStages[i].stagenum == STAGE_MBR) {
			break;
		}

		modCareerPoolAdd(MODCAREER_KIND_SOLO, g_SoloStages[i].stagenum, langGet(g_SoloStages[i].name3));
	}

#ifndef PLATFORM_N64
	// GoldenEye's missions, when GE Plus has them: its own order first, then
	// any other converted mission the loader knows (a ROM hack's)
	for (i = 0; i < 32; i++) {
		const s32 stage = modloaderMissionStageOf(NULL, i);

		if (stage && !modCareerPoolHasStage(stage)) {
			const char *name = modloaderGetStageMapName(stage);

			modCareerPoolAdd(MODCAREER_KIND_GE, stage, name ? name : "GoldenEye mission");
		}
	}

	for (i = 1; i <= STAGE_MAX_ID; i++) {
		if (modloaderStageIsMission(i) && !modCareerPoolHasStage(i)) {
			const char *name = modloaderGetStageMapName(i);

			modCareerPoolAdd(MODCAREER_KIND_GE, i, name ? name : "GoldenEye mission");
		}
	}

	// The Combat Simulator's maps, the ones a Perfect Random mission can be
	// started on from the Combat Maps page
	arenaTabsSync();

	for (i = 0; i < mpGetNumStages() && i < mpGetNumStages(); i++) {
		const s32 stage = g_MpArenas[i].stagenum;
		const char *name = modloaderGetStageMapName(stage);

		if (arenaTabsIsMissionCopyRow(i) || modCareerPoolHasStage(stage)) {
			continue; // the mission copies (arenatabs.c) are not arenas, nor is a mission twice
		}

		if (stage == STAGE_MP_RANDOM || !STAGE_IS_LEVEL(stage) || stageGetIndex(stage) < 0) {
			continue;
		}

		if (name && strncmp(name, "PR ", 3) == 0) {
			continue;
		}

		modCareerPoolAdd(MODCAREER_KIND_ARENA, stage, name ? name : mpGetArenaName(i));
	}

	// The level generator's levels, one entry per layout ("(GEN) Lab"): a new
	// level of that layout for every mission it is dealt. They start out left
	// off (the first time the pool is built; the marker -2 in the off list
	// says that has been done), so a gauntlet only has them once the player
	// ticks them on the Gauntlet page. (2026-11-15: before, one "Generated
	// Level" entry under the stage number, marker -1; whoever had ticked that
	// one on gets every layout on.)
	if (levelgenIsStage(LEVELGEN_STAGENUM) && !modCareerPoolHasStage(LEVELGEN_POOLID(0))) {
		static const char *names[LG_LAYOUT_COUNT] = { "(GEN) Hub", "(GEN) Lab", "(GEN) Streets", "(GEN) Ravine", "(GEN) Rooftops", "(GEN) Villa", "(GEN) Docks", "(GEN) Backwoods", "(GEN) Penthouse", "(GEN) Building Site", "(GEN) Market Town", "(GEN) Mall", "(GEN) Pipeworks" };
		const bool firsttime = strstr(g_ModCareerOff, ",-2,") == NULL;
		const bool firstStation = strstr(g_ModCareerOff, ",-4,") == NULL;
		const bool wason = strstr(g_ModCareerOff, ",-1,") != NULL && modCareerOffFind(LEVELGEN_STAGENUM) == NULL;
		s32 l;

		for (l = 0; l < LG_LAYOUT_COUNT; l++) {
			if (l == LG_LAYOUT_PLAZA || l == LG_LAYOUT_PIPEWORKS) {
				continue;   // (the market town and the pipeworks were removed, 2026-11-19)
			}
			modCareerPoolAdd(MODCAREER_KIND_GEN, LEVELGEN_POOLID(l), names[l]);

			if (firsttime && !wason) {
				modCareerPoolSetPicked(g_ModCareerPoolCount - 1, false);
			}
		}

		if (firsttime && strlen(g_ModCareerOff) + 4 < sizeof(g_ModCareerOff)) {
			strcat(g_ModCareerOff, strlen(g_ModCareerOff) == 0 ? ",-2," : "-2,");
		}

		if (g_ModCareerPoolCount < MODCAREER_MAXPOOL) {
			modCareerPoolAdd(MODCAREER_KIND_GEN, LEVELGEN_POOLID(LG_LAYOUT_STATION), "(GEN) Switchback Station");
			if (firstStation && !wason) {
				modCareerPoolSetPicked(g_ModCareerPoolCount - 1, false);
			}
			if (firstStation && strlen(g_ModCareerOff) + 4 < sizeof(g_ModCareerOff)) {
				strcat(g_ModCareerOff, strlen(g_ModCareerOff) == 0 ? ",-4," : "-4,");
			}
		}

		// Bunker 2 (2026-11-26): a designed level, off until ticked on the Gauntlet page
		if (g_ModCareerPoolCount < MODCAREER_MAXPOOL) {
			const bool firstBunker = strstr(g_ModCareerOff, ",-5,") == NULL;

			modCareerPoolAdd(MODCAREER_KIND_GEN, LEVELGEN_POOLID(LG_LAYOUT_BUNKER2), "(GEN) Bunker 3");
			if (firstBunker && !wason) {
				modCareerPoolSetPicked(g_ModCareerPoolCount - 1, false);
			}
			if (firstBunker && strlen(g_ModCareerOff) + 4 < sizeof(g_ModCareerOff)) {
				strcat(g_ModCareerOff, strlen(g_ModCareerOff) == 0 ? ",-5," : "-5,");
			}
		}
	}
#endif
}

// Rebuilt when the number of maps the game knows changes (a mod loaded).
static void modCareerPoolEnsure(void)
{
	s32 sig = 0;

#ifndef PLATFORM_N64
	sig = mpGetNumStages() + 1000;
#endif

	if (sig != g_ModCareerPoolSig || g_ModCareerPoolCount == 0) {
		g_ModCareerPoolSig = sig;
		modCareerPoolBuild();
	}
}

s32 modCareerPoolCount(void)
{
	modCareerPoolEnsure();
	return g_ModCareerPoolCount;
}

s32 modCareerPoolKind(s32 index)
{
	modCareerPoolEnsure();
	return index >= 0 && index < g_ModCareerPoolCount ? g_ModCareerPool[index].kind : 0;
}

s32 modCareerPoolStage(s32 index)
{
	modCareerPoolEnsure();
	return index >= 0 && index < g_ModCareerPoolCount ? g_ModCareerPool[index].stage : 0;
}

const char *modCareerPoolName(s32 index)
{
	modCareerPoolEnsure();
	return index >= 0 && index < g_ModCareerPoolCount ? g_ModCareerPool[index].name : "";
}

bool modCareerPoolMapVisible(s32 index)
{
	static const s32 funStages[] = {
		STAGE_DEFECTION, STAGE_INVESTIGATION, STAGE_VILLA, STAGE_CHICAGO,
		STAGE_G5BUILDING, STAGE_AIRFORCEONE, STAGE_DEFENSE,
		STAGE_MP_SKEDAR, STAGE_MP_PIPES, STAGE_MP_RAVINE, STAGE_MP_G5BUILDING,
		STAGE_MP_SEWERS, STAGE_MP_WAREHOUSE, STAGE_MP_GRID, STAGE_MP_RUINS,
		STAGE_MP_AREA52, STAGE_MP_BASE, STAGE_MP_FORTRESS, STAGE_MP_VILLA,
		STAGE_MP_CARPARK, STAGE_MP_TEMPLE, STAGE_MP_COMPLEX, STAGE_MP_FELICITY,
	};
	static const char *const funGeNames[] = {
		"Facility", "Bunker 1", "Bunker 2", "Control", "Aztec", "Egyptian",
	};

	modCareerPoolEnsure();
	if (index < 0 || index >= g_ModCareerPoolCount) return false;
	if (!g_ModCareerHideUnfunMaps) return true;

	const s32 kind = g_ModCareerPool[index].kind;
	const s32 stage = g_ModCareerPool[index].stage;

	if (kind == MODCAREER_KIND_GE) {
		// Converted missions are assigned stage numbers as they are loaded.
		const char *name = g_ModCareerPool[index].name;
		for (s32 i = 0; i < ARRAYCOUNT(funGeNames); i++) {
			if (strcmp(name, funGeNames[i]) == 0) return true;
		}
	} else if (kind == MODCAREER_KIND_SOLO || kind == MODCAREER_KIND_ARENA) {
		for (s32 i = 0; i < ARRAYCOUNT(funStages); i++) {
			if (stage == funStages[i]) return true;
		}
#ifndef PLATFORM_N64
	} else if (kind == MODCAREER_KIND_GEN) {
		return stage == LEVELGEN_POOLID(LG_LAYOUT_STREET);
#endif
	}

	return false;
}

// g_ModCareerOff is ",12,40," style: find ",<stage>," in it
static char *modCareerOffFind(s32 stage)
{
	char key[16];

	snprintf(key, sizeof(key), ",%d,", stage);

	return strstr(g_ModCareerOff, key);
}

bool modCareerPoolPicked(s32 index)
{
	return modCareerPoolStage(index) > 0 && modCareerOffFind(modCareerPoolStage(index)) == NULL;
}

static bool modCareerPoolEligible(s32 index)
{
	return modCareerPoolMapVisible(index) && modCareerPoolPicked(index);
}

void modCareerPoolSetPicked(s32 index, bool picked)
{
	const s32 stage = modCareerPoolStage(index);
	char *found = stage > 0 ? modCareerOffFind(stage) : NULL;
	char key[16];

	if (stage <= 0) {
		return;
	}

	snprintf(key, sizeof(key), ",%d,", stage);

	if (picked && found) {
		// ",12,40," without ",12" is ",40,"; keep the comma that follows
		char *rest = found + strlen(key) - 1;

		memmove(found, rest, strlen(rest) + 1);

		if (strcmp(g_ModCareerOff, ",") == 0) {
			g_ModCareerOff[0] = '\0';
		}
	} else if (!picked && !found) {
		const size_t len = strlen(g_ModCareerOff);

		if (len + strlen(key) < sizeof(g_ModCareerOff)) {
			// "" becomes ",12,", and ",12," becomes ",12,40,"
			strcat(g_ModCareerOff, len == 0 ? key : key + 1);
		}
	}
}

s32 modCareerPoolNumPicked(void)
{
	s32 i;
	s32 count = 0;

	for (i = 0; i < modCareerPoolCount(); i++) {
		if (modCareerPoolEligible(i)) {
			count++;
		}
	}

	return count;
}

s32 modCareerKindCount(s32 kind)
{
	s32 i;
	s32 count = 0;

	for (i = 0; i < modCareerPoolCount(); i++) {
		if (modCareerPoolKind(i) == kind) {
			count++;
		}
	}

	return count;
}

s32 modCareerKindNumPicked(s32 kind)
{
	s32 i;
	s32 count = 0;

	for (i = 0; i < modCareerPoolCount(); i++) {
		if (modCareerPoolKind(i) == kind && modCareerPoolPicked(i)) {
			count++;
		}
	}

	return count;
}

void modCareerKindPick(s32 kind, bool picked)
{
	s32 i;

	for (i = 0; i < modCareerPoolCount(); i++) {
		if (modCareerPoolKind(i) == kind) {
			modCareerPoolSetPicked(i, picked);
		}
	}
}

s32 modCareerLength(void)
{
	if (g_ModCareerLength == MODCAREER_UNLIMITED) {
		return MODCAREER_UNLIMITED;
	}
	if (g_ModCareerLength < MODCAREER_MINMISSIONS) {
		return MODCAREER_DEFAULTMISSIONS;
	}
	const s32 length = MIN(g_ModCareerLength, MODCAREER_MAXMISSIONS);
	return length + (length % 2);
}

bool modCareerIsOn(void)
{
	return g_ModCareerOn;
}

s32 modCareerMission(void)
{
	return g_ModCareerMission;
}

const char *modCareerGetStatus(void)
{
	if (g_ModCareerOn) {
		if (modCareerLength() == MODCAREER_UNLIMITED) {
			snprintf(g_ModCareerStatus, sizeof(g_ModCareerStatus), "Unlimited Gauntlet: mission %d\n", g_ModCareerMission + 1);
		} else {
			snprintf(g_ModCareerStatus, sizeof(g_ModCareerStatus), "Gauntlet: mission %d of %d\n",
					g_ModCareerMission + 1, modCareerLength());
		}
	} else if (g_ModCareerStatus[0] == '\0') {
		snprintf(g_ModCareerStatus, sizeof(g_ModCareerStatus), "Missions get harder as you go\n");
	}

	return g_ModCareerStatus;
}

s32 modCareerExtraWaveGuards(void)
{
	return g_ModCareerOn ? g_ModCareerLevel[RAMP_GUARDS] * g_ModCareerStepGuards : 0;
}

s32 modCareerSpawnPercent(void)
{
	return 100; // the wave interval is no longer ramped
}

/** Seconds between reinforcement waves: a fixed 30 in the gauntlet. */
s32 modCareerWaveSeconds(void)
{
	return 30;
}

f32 modCareerAccuracyScale(void)
{
	return g_ModCareerOn ? 1.0f + g_ModCareerLevel[RAMP_ACCURACY] * g_ModCareerStepAccuracy * 0.01f : 1.0f;
}

f32 modCareerPlayerDamageScale(void)
{
	return 1.0f; // the ramp no longer touches damage
}

s32 modCareerShieldChance(void)
{
	return g_ModCareerOn && g_ModCareerModOn[MOD_SHIELDED] ? 10 : 0;
}

s32 modCareerGrenadierChance(void)
{
	return g_ModCareerOn && g_ModCareerModOn[MOD_GRENADIERS] ? 20 : 0;
}

/** Percent of guards that dual wield: the ramp's steps (Difficulty Increase). */
s32 modCareerAkimboChance(void)
{
	s32 pct;

	if (!g_ModCareerOn) {
		return 0;
	}

	pct = g_ModCareerLevel[RAMP_AKIMBO] * g_ModCareerStepAkimbo;

	return pct > 100 ? 100 : pct;
}

/** Guard respawn chance after applying the starting preset and difficulty increases. */
s32 modCareerRespawnChance(void)
{
	s32 pct = g_ModCareerOn ? g_ModCareerInitialRespawnChance
		+ g_ModCareerLevel[RAMP_RESPAWN] * g_ModCareerStepRespawn : 0;
	return pct > 100 ? 100 : pct;
}

s32 modCareerKillObjectives(void)
{
	if (!g_ModCareerOn) {
		return 0;
	}

	const s32 length = modCareerLength();
	// Unlimited: two objectives, one more every two missions, up to five.
	if (length == MODCAREER_UNLIMITED) {
		return 2 + MIN(g_ModCareerMission / 2, 3);
	}
	const s32 last = length - 1;
	return 2 + (3 * MIN(g_ModCareerMission, last) + last / 2) / last;
}

static bool modCareerWasUsed(s32 stage)
{
	s32 i;

	for (i = 0; i < g_ModCareerNumUsed; i++) {
		if (g_ModCareerUsed[i] == stage) {
			return true;
		}
	}

	return false;
}

/**
 * The next mission is one of the visible, picked maps. Prefer maps that
 * haven't appeared in this cycle. Returns -1 if none are eligible.
 */
static s32 modCareerChooseMap(void)
{
	s32 pick[MODCAREER_MAXPOOL];
	s32 fresh[MODCAREER_MAXPOOL];
	s32 num = 0;
	s32 numfresh = 0;
	const s32 count = modCareerPoolCount();
	s32 i;

	for (i = 0; i < count; i++) {
		if (modCareerPoolEligible(i)) {
			pick[num++] = i;
		}
	}

	if (num == 0) {
		return -1;
	}

	for (i = 0; i < num; i++) {
		if (!modCareerWasUsed(modCareerPoolStage(pick[i]))) {
			fresh[numfresh++] = pick[i];
		}
	}

	if (numfresh == 0 || g_ModCareerNumUsed == ARRAYCOUNT(g_ModCareerUsed)) {
		const s32 previous = g_ModCareerUsed[g_ModCareerNumUsed - 1];

		g_ModCareerNumUsed = 0;
		numfresh = 0;
		for (i = 0; i < num; i++) {
			if (modCareerPoolStage(pick[i]) != previous) {
				fresh[numfresh++] = pick[i];
			}
		}
		if (numfresh == 0) {
			fresh[numfresh++] = pick[0];
		}
	}

	{
		s32 combat[MODCAREER_MAXPOOL];
		s32 other[MODCAREER_MAXPOOL];
		s32 numcombat = 0;
		s32 numother = 0;

		for (i = 0; i < numfresh; i++) {
			if (modCareerPoolKind(fresh[i]) == MODCAREER_KIND_ARENA) {
				combat[numcombat++] = fresh[i];
			} else {
				other[numother++] = fresh[i];
			}
		}

		if (numcombat && (!numother || rngRandom() % 2 == 0)) {
			return combat[rngRandom() % numcombat];
		}
		return other[rngRandom() % numother];
	}
}

static bool modCareerEnter(void)
{
	const s32 index = modCareerChooseMap();
	if (index < 0) {
		sysLogPrintf(LOG_WARNING, "gauntlet: no eligible maps");
		return false;
	}

	const s32 stage = modCareerPoolStage(index);
	const s32 kind = modCareerPoolKind(index);

	g_ModCareerUsed[g_ModCareerNumUsed++] = stage;
	g_ModCareerBoonApplied = false;
	g_ModCareerBriefDone = false;
	g_ModCareerBoonPick = -1; // a boon is for the one mission it was picked for
	memset(g_ModCareerCharges, 0, sizeof(g_ModCareerCharges));
	g_ModCareerMineLayerHit = false;
	memset(g_ModCareerGrenadeTally, 0, sizeof(g_ModCareerGrenadeTally));
	modCareerRollOffers();
	if (modCareerLength() == MODCAREER_UNLIMITED) {
		sysLogPrintf(LOG_NOTE, "gauntlet: mission %d on stage 0x%02x (%s)", g_ModCareerMission + 1, stage,
				modCareerPoolName(index));
	} else {
		sysLogPrintf(LOG_NOTE, "gauntlet: mission %d of %d on stage 0x%02x (%s)",
				g_ModCareerMission + 1, modCareerLength(), stage, modCareerPoolName(index));
	}

	g_ModOptions.randomendless = 0;
	g_ModOptions.randomversion = MODRANDOM_VERSION_DEFAULT;

#ifndef PLATFORM_N64
	const char *mapName = modCareerPoolName(index);
	snprintf(g_ModCareerMapName, sizeof(g_ModCareerMapName), "%s", mapName);
	g_ModCareerMapName[strcspn(g_ModCareerMapName, "\r\n")] = '\0';

	if (kind == MODCAREER_KIND_GEN) {
		// a fresh level from a fresh seed, run as an arena mission
		u32 genseed = (u32)rngRandom();

		if (genseed == 0) {
			genseed = 1;
		}

		const s32 layout = LEVELGEN_POOLID_LAYOUT(stage);
		bool started;

		sysLogPrintf(LOG_NOTE, "gauntlet: generated level, layout %d, seed %u", layout, genseed);

		// the entry's layout, for this level only (-1, an old entry: the seed's)
		modLevelGenForceTheme((layout >= 0 && layout < LG_LAYOUT_COUNT) || layout == LG_LAYOUT_STATION || layout == LG_LAYOUT_BUNKER2 ? layout : -1, -1);
		started = levelgenStart(genseed, g_LevelGenSizeSetting);
		modLevelGenForceTheme(-1, -1);

		if (started) {
			const LgLayout *generated = levelgenGetLayout();
			if (generated) {
				snprintf(g_ModCareerMapName, sizeof(g_ModCareerMapName), "%s", modLevelGenLayoutName(generated->theme.layout));
			}
			return true;
		}

		sysLogPrintf(LOG_WARNING, "gauntlet: the generated level did not start");
		return false;
	}

	if (kind == MODCAREER_KIND_ARENA || kind == MODCAREER_KIND_GE) {
		// A Combat Simulator map is a Perfect Random mission by way of the
		// arena start the Combat Maps page uses (it arms the roll itself).
		// So is a GoldenEye level: its setup is an arena's (gelevels.c).
		modRunStartArenaMission(stage);
		return true;
	}
#endif

	// A Perfect Dark or GoldenEye mission. An arena mission just before it
	// left its flag on.
	modRunStop();
	modRandomArmMission();
	g_MissionConfig.iscoop = false;
	g_MissionConfig.isanti = false;
	g_MissionConfig.stagenum = stage;
	g_MissionConfig.difficulty = modGetRunDifficulty();
	g_Vars.bondplayernum = 0;
	g_Vars.coopplayernum = -1;
	g_Vars.antiplayernum = -1;
	setNumPlayers(1);
	titleSetNextStage(stage);
	lvSetDifficulty(g_MissionConfig.difficulty);
	titleSetNextMode(TITLEMODE_SKIP);
	mainChangeToStage(stage);
	viBlack(true);
	return true;
}

void modCareerStart(void)
{
	s32 i;
	g_ModCareerOn = true;
	modRandomEventHistoryReset();
	modRandomAlienItemsSet(true);
	g_ModCareerCloakReward = false;
	g_ModCareerMission = 0;
	g_ModCareerNumUsed = 0;
	g_ModCareerAlienReward = 0;
	g_ModCareerBlondReward = false;
	memset(g_ModCareerLevel, 0, sizeof(g_ModCareerLevel));
	if (g_ModCareerStartPreset == 1) {
		for (i = 0; i < RAMP_COUNT; i++) g_ModCareerLevel[i] = 1;
	} else if (g_ModCareerStartPreset == 2) {
		g_ModCareerLevel[RAMP_ACCURACY] = 1;
	} else if (g_ModCareerStartPreset >= 3) {
		g_ModCareerLevel[RAMP_GUARDS] = g_ModCareerStartGuards < 0 ? 0 : g_ModCareerStartGuards > 10 ? 10 : g_ModCareerStartGuards;
		g_ModCareerLevel[RAMP_ACCURACY] = g_ModCareerStartAccuracy < 0 ? 0 : g_ModCareerStartAccuracy > 10 ? 10 : g_ModCareerStartAccuracy;
		g_ModCareerLevel[RAMP_AKIMBO] = g_ModCareerStartAkimbo < 0 ? 0 : g_ModCareerStartAkimbo > 10 ? 10 : g_ModCareerStartAkimbo;
		g_ModCareerLevel[RAMP_RESPAWN] = g_ModCareerStartRespawn < 0 ? 0 : g_ModCareerStartRespawn > 4 ? 4 : g_ModCareerStartRespawn;
	}
	g_ModCareerInitialRespawnChance = MODCAREER_BASE_RESPAWN_CHANCE + g_ModCareerLevel[RAMP_RESPAWN] * 25;
	g_ModCareerLevel[RAMP_RESPAWN] = 0;
	modCareerResetStats();
	modCareerClearAchievements();
	g_ModCareerCarryShield = 0.0f;
	modCareerResetMods();
	g_ModCareerLastRamp = -1;
	g_ModCareerAnnounce = NULL;
	// Perfect Agent preserves the old opening accuracy step; other presets set
	// the opening point directly and the random ramp starts after mission one.
	g_ModCareerLastRamp = RAMP_ACCURACY;
	g_ModCareerAnnounce = NULL;
	memset(g_ModCareerPerm, 0, sizeof(g_ModCareerPerm));
	g_ModCareerMineLayerOffer = false;
	memset(g_ModCareerPermNew, 0, sizeof(g_ModCareerPermNew));
	g_ModCareerNumPermOffers = 0;
	g_ModCareerPermPending = false;
	g_ModCareerBoonPick = -1;
	g_ModCareerBoonPending = false;
	g_ModCareerStatus[0] = '\0';
	modGhostDisarmTrial();
	g_ModOptions.randomendless = 0;
	g_ModOptions.randomversion = MODRANDOM_VERSION_DEFAULT;
	if (!modCareerEnter()) {
		modCareerStop();
		return;
	}
	g_ModCareerBoonPending = modCareerBoonsLeft(); // the first mission offers a boon too
}

void modCareerStop(void)
{
	g_ModCareerOn = false;
	modRandomAlienItemsSet(false);
	g_ModCareerMission = 0;
}

bool modCareerOnMissionComplete(void)
{
	sysLogPrintf(LOG_NOTE, "gauntlet: mission %d complete (gauntlet %s)", g_ModCareerMission + 1, g_ModCareerOn ? "on" : "off");

	if (!g_ModCareerOn) {
		return false;
	}

	g_ModCareerCarryShield = g_Vars.currentplayer && !g_Vars.currentplayer->isdead ? playerGetShieldFrac() : 0.0f;
	modCareerBankShots();
	modCareerBankAchievements();
	g_ModCareerMineLayerOffer = g_ModCareerMineLayerHit;
	g_ModCareerStatMissions = g_ModCareerMission + 1;
	if (g_ModCareerStatMissions > g_ModCareerBest) {
		g_ModCareerBest = g_ModCareerStatMissions;
#ifndef PLATFORM_N64
		configSave(CONFIG_PATH);
#endif
	}

	const s32 length = modCareerLength();
	if (length == MODCAREER_UNLIMITED || g_ModCareerStatMissions < length) {
		if (g_ModCareerMission < INT_MAX - 1) {
			g_ModCareerMission++;
		}
		modCareerRollRamp();

		if (g_ModCareerMission % 3 == 2) {
			modCareerRollMod(); // the third map, the sixth...
		}
		if (g_ModCareerMission % 3 == 0) {
			modCareerRollPermOffers(); // three cleared, six cleared...: a permanent boon
		}
		g_ModCareerBoonPending = modCareerBoonsLeft();
		if (modCareerEnter()) {
			return true;
		}
		snprintf(g_ModCareerStatus, sizeof(g_ModCareerStatus),
				"Last gauntlet: stopped after %d missions\n", g_ModCareerStatMissions);
		g_ModCareerSummaryPending = true;
		g_ModCareerSummaryWon = false;
		modCareerStop();
		return false;
	}

	// The last one: the stage ends as an ordinary completed mission.
	snprintf(g_ModCareerStatus, sizeof(g_ModCareerStatus), "Last gauntlet: cleared all %d missions\n", modCareerLength());
	g_ModCareerSummaryPending = true;
	g_ModCareerSummaryWon = true;
	modCareerStop();
	return false;
}

void modCareerOnStageEnded(void)
{
	if (!g_ModCareerOn) {
		return;
	}

	sysLogPrintf(LOG_NOTE, "gauntlet: stage ended on mission %d - gauntlet over", g_ModCareerMission + 1);
	modCareerBankShots();
	g_ModCareerStatMissions = g_ModCareerMission; // the one that ended it was not cleared
	g_ModCareerSummaryPending = true;
	g_ModCareerSummaryWon = false;
	if (modCareerLength() == MODCAREER_UNLIMITED) {
		snprintf(g_ModCareerStatus, sizeof(g_ModCareerStatus), "Last gauntlet: cleared %d missions\n", g_ModCareerStatMissions);
	} else {
		snprintf(g_ModCareerStatus, sizeof(g_ModCareerStatus), "Last gauntlet: failed on mission %d of %d\n",
				g_ModCareerMission + 1, modCareerLength());
	}
	modCareerStop();

#ifndef PLATFORM_N64
	// The Gauntlet is over, and so is whatever it armed: left on, the next
	// stage this game loads (Retry, or a mission picked from the menu) was
	// dealt as a Gauntlet arena - the Combat Simulator's setup file, no
	// weapons - and the player died in it again and again.
	modRunStop();
	modRandomDisarmMission();
#endif
}
