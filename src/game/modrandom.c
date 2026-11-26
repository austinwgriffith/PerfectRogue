#include <float.h>
#include <ultra64.h>
#include <string.h>
#include <stdlib.h>
#ifndef PLATFORM_N64
#include "gelevels.h"
#endif
#include "constants.h"
#include "prcompat.h"
#include "game/bg.h"
#include "game/bondgun.h"
#include "game/chraction.h"
#include "game/chr.h"
#include "game/game_0b0fd0.h"
#include "game/hudmsg.h"
#include "game/inv.h"
#include "game/lang.h"
#include "game/atan2f.h"
#include "game/lv.h"
#include "game/modoptions.h"
#include "game/modheads.h"
#include "game/modalarm.h"
#include "game/modrules.h"
#include "game/explosions.h"
#include "game/modrandom.h"
#include "game/game_1531a0.h"
#include "lib/vi.h"
#include "gbiex.h"
#include <stdarg.h>
#include <stdio.h>
#include "game/modcareer.h"
#include "game/modrun.h"
#include "game/modspawnedit.h"
#include "game/mplayer/mplayer.h"
#include "game/mplayer/setup.h"
#include "mod.h"
#include "game/objectives.h"
#include "game/pad.h"
#include "game/padhalllv.h"
#include "game/player.h"
#include "game/prop.h"
#include "game/playermgr.h"
#include "game/propobj.h"
#include "game/propsnd.h"
#include "game/setup.h"
#include "game/setuputils.h"
#include "lib/ailist.h"
#include "bss.h"
#include "lang.h"
#include "lib/collision.h"
#include "lib/main.h"
#include "lib/memp.h"
#include "lib/model.h"
#include "lib/rng.h"
#include "lib/snd.h"
#include "lib/str.h"
#include "data.h"
#include "types.h"
#include "platform.h"
#ifndef PLATFORM_N64
#include "geguns.h"
#include "modloader.h"
#include "gexplus.h"
#include "langpack.h"
#ifndef PLATFORM_N64
#include "geroom.h"
#include "gestan.h"
#include "system.h"
#include "levelgenstage.h"
#include "video.h"
#endif
#endif

#ifndef PLATFORM_N64
extern void sysLogPrintf(s32 level, const char *fmt, ...);
#endif

#define MODEVENT_TEAM_TERROR  0x40
enum { MODEVENT_NONE, MODEVENT_MAIANS, MODEVENT_RESERVED_SIMULANTS, MODEVENT_EXPLOSIVE, MODEVENT_SKEDARS, MODEVENT_PURGE, MODEVENT_ALIEN_WAR, MODEVENT_ASSASSINS, MODEVENT_WARZONE, MODEVENT_BLOND, MODEVENT_COUNT };
#define MODRANDOM_WARZONE_ALLIES 50
#define MODRANDOM_WARZONE_ENEMIES 50
#define MODRANDOM_WARZONE_WAVE_SIZE 5
#define MODRANDOM_WARZONE_WAVE_DELAY TICKS(10 * 60)
#define MODRANDOM_WARZONE_SPAWN_DELAY TICKS(6)
#define MODRANDOM_WARZONE_RETRY TICKS(30)
#define MODRANDOM_WARZONE_PADS 40
#define MODRANDOM_WARZONE_MEMFLOOR (512 * 1024)
#define MODRANDOM_WARZONE_RANGE_SQ (1800.0f * 1800.0f)
static struct {
	bool active;
	s32 wave;
	s32 allies;
	s32 enemies;
	s32 next60;
	s32 waveStart60;
	s32 numspawned;
	s16 followers[MODRANDOM_WARZONE_ALLIES / MODRANDOM_WARZONE_WAVE_SIZE];
	struct coord positions[2 * MODRANDOM_WARZONE_WAVE_SIZE];
} g_ModRandomWarzone;
static bool g_ModRandomAlienWarActive;
static s32 g_ModRandomAssassinsUntil;
static bool g_ModRandomEventFired;
static s32 g_ModRandomEventChoice;
#ifndef PLATFORM_N64
#define MODEVENT_ANNOUNCE_TICKS (TICKS(3 * 60) * 4)
#define MODEVENT_ANNOUNCE_DELAY (TICKS(60) * 4)
static s32 g_ModRandomEventNotice240;
static s32 g_ModRandomEventNoticeDelay240;
static bool g_ModRandomEventNoticeFramePending;
static bool g_ModRandomEventNoticeEnding;
static bool g_ModRandomEventEndQueued;
#endif
static s32 g_ModRandomAlienElvis;
static s32 g_ModRandomAlienKing;
static s32 g_ModRandomAlienNextBoss60;
static s32 g_ModRandomAlienFaction;
static s32 g_ModRandomAlienPlayerTeam;
static void modRandomAlienWarFinish(s32 defeated, s32 playernum);

bool modRandomAlienWarActive(void)
{
	return g_ModRandomAlienWarActive;
}

// The weapon rows a roll draws from: not a ROM hack's own pistols, added
// after GoldenEye's guns, so a seed deals what it dealt before them
#ifdef MPWEAPON_GE_EXTRA1
#define MODRANDOM_MPWEAPONS MPWEAPON_GE_EXTRA1
#else
#define MODRANDOM_MPWEAPONS NUM_MPWEAPONS
#endif

/**
 * Randomizer: a mission dealt again from its own pieces.
 *
 * A stage's setup file is a command stream - one command per weapon, guard,
 * key, door, crate and objective - and the port loads it into a writable copy
 * of its own before anything is built from it (setupLoadFiles(), and
 * setupCreateProps() walks the copy afterwards). Everything here happens in
 * the gap between those two: the stream is rewritten, and the game then
 * builds the level the rewrite describes, with no idea it was not the one on
 * the disc.
 *
 * What it does not do is move geometry. One bg file is resident at a time and
 * a portal's room numbers index that one file, so rooms cannot be dealt from
 * different stages - see the note in CLAUDE-notes. The rooms are the stage's
 * own; what changes is everything standing in them, and which of them you can
 * get to.
 *
 * **Shuffled within a class, not scattered.** A weapon moves to a spot that
 * held a weapon, a guard to a pad that held a guard. The level's designer put
 * those spots where a thing can stand - on the floor, out of a wall, inside
 * the room's collision - and a random point in a room is none of those. So
 * placement is a permutation of the stage's own slots, and it is the *kind*
 * of thing in each slot that is rolled freely: any of the game's weapons can
 * turn up in any weapon spot, any ammo in any crate. That is where the
 * variety comes from, and it cannot put a gun inside a wall.
 *
 * **The portal graph decides whether a seed is playable.** Rooms are a graph
 * already - g_Rooms[i] lists its portals and each portal names the two rooms
 * it joins - and a locked door is an edge that needs a key. So after the keys
 * are moved the generator walks that graph from the spawn room, taking the
 * keys it finds and opening what they open, until nothing new is reachable.
 * Objectives are then only ever placed in rooms that walk reached. A seed
 * that cannot be finished is the one thing a randomizer must never hand a
 * player, and the graph is the only way to know before they have spent an
 * hour finding out.
 *
 * The walk is honest about what it does not model: lifts, hatches, ladders
 * and anything a mission script opens are not portals with doors on them, so
 * the walk can call a room unreachable that a player can reach. It is used
 * only to *reject* placements, never to prove one impossible, and the fallback
 * when it rejects everything is the stage's own key placement.
 *
 * **The seed is the run.** Mod.RandomizerSeed in pd.ini, or the menu: zero
 * means a fresh roll every mission, anything else is a run that can be
 * written down, replayed and handed to somebody else. A seed and a stage
 * always deal the same mission, on any machine, because the roll is this
 * file's own xorshift and never the game's rngRandom(), whose state depends
 * on how many frames the title screen was left running.
 *
 * **And it keeps dealing it after this file changes**, which is the point of
 * the streams: every decision draws from its own, so working on one part of
 * the roll cannot move the others. What a change to the *meaning* of a draw
 * does is covered by MODRANDOM_VERSION, and the fold printed with the seed is
 * how a change is checked - see modRandomOpen() and modRandomFold().
 */

/**
 * The generator's version, and what it is for.
 *
 * A seed is only worth writing down if it deals the same mission tomorrow.
 * Most of that is the streams below, which keep a change to one decision from
 * moving every other; this number covers the rest - a change to what the
 * draws *mean*, which no amount of stream separation can absorb.
 *
 * The rule for changing this file: if a change would deal a different mission
 * from the same seed, put it behind `g_ModRandomVersion >= N`, leave the older
 * behaviour where it is, and raise this to N. A run then keeps the generator
 * it was dealt by (Mod.RandomizerVersion in pd.ini, written beside the seed),
 * and a shared seed carries its version with it. Adding a new stream id, or
 * changing anything a draw does not feed, needs no bump.
 *
 * Version 1 is the first versioned generator. The build before it drew every
 * decision from one walked stream, so seeds from it are not reproducible here
 * and are not claimed to be.
 *
 * Version 2 widened where a "reach this room" objective may send the player,
 * from the deepest rooms the walk found to the deep half of them - Endless
 * Mode asks for a room over and over and four of them is not enough to ask
 * about. v1 runs keep the narrow rule.
 *
 * Version 3 asks whether a pad is somewhere a player can be stood up alive
 * before a start or a landing is put on it - modRandomPadSpawnPos() - which
 * moves the start on any stage whose pads include one that is not. v1 and v2
 * runs keep taking the pad on trust, out of bounds and all.
 *
 * Version 4 is two things a run's landing learnt. A landing in a room with no
 * light at all - Deep Sea's rooms 86, 87 and 89 have black vertex colours and
 * no lights - is dealt again on another pad (modrun.c, modRunRoomIsDark()),
 * and a map with no waypoints of its own lowers a pad that floats high over
 * its floor onto it before the pad graph is built (modRandomPadSettle()),
 * which changes which of its pads a landing may be dealt on.
 *
 * Version 5 hands a stage's guards only weapons whose primary function
 * shoots: a grenade, a mine or a knife in a guard's hand is held and aimed
 * like a gun and never thrown (user, 2026-09-28). Stages whose guards drew a
 * throwable under v4 draw again, in the same stream.
 *
 * Version 6 never asks for a weapon the player cannot pick up. Villa keeps
 * four tagged CMP150s on pad 0x19a for its script, uncollectable and
 * invisible, and a "Recover the ..." dealt on one of them was a mission that
 * could not be finished (F3 20261002-224232). And a run's landing on the Duel
 * keeps out of, and a door away from, the three rooms whose doorways show the
 * sky there (modrun.c, modRunRoomIsBare(); F3 20261002-082816).
 *
 * Version 7 never lands a run in a room whose sealed zone no chr could walk
 * out of: a waypoint link has to leave it (modrun.c, modRunZoneWalksOut()).
 * Carrington Institute's firing range lane leaves only through the booth's
 * glass, and a landing there could not be left once won (F3 20261011-004012).
 *
 * Version 8 replaces the old fetch/room objectives with one named hostile
 * character to kill. Endless mode draws another named target from another
 * eligible guard row until it has used them all.
 *
 * Version 9 turns one to three other guard rows into friendly buddy AI. Older
 * versioned seeds retain their version 8 guard and target rolls.
 *
 * Version 10 keeps every guard at its rolled level post, changes human guards
 * to female models, and spawns the buddies together around the player's start.
 *
 * Version 11 gives every hostile guard a Joanna outfit, uses Cassandra De Vries
 * as the kill target, and makes every spawned buddy Velvet Dark.
 *
 * Version 12 removes the hostile guard on the randomized start pad and gives
 * the Velvet Dark buddies ten times their normal health with the stock co-op
 * buddy initialization AI.
 *
 * Version 13 starts the player with one rolled gun and 100 total rounds, up to
 * that gun's ammo capacity.
 *
 * Version 14 gives a regular Cassandra mission three distinct targets and
 * keeps the mission open until all three have been killed.
 */
#define MODRANDOM_VERSION MODRANDOM_VERSION_DEFAULT

// One stream per kind of decision. Ids are permanent: changing one is
// changing every seed. New kinds take the next number.
#define MODRANDOM_STREAM_WEAPON    1 // index: which weapon spot
#define MODRANDOM_STREAM_CRATE     2 // index: which crate
#define MODRANDOM_STREAM_CHRPADS   3 // the guards' shuffle
#define MODRANDOM_STREAM_INTROGUN  4 // index: which intro weapon command
#define MODRANDOM_STREAM_SPAWN     5 // index: which try
#define MODRANDOM_STREAM_KEYS      6 // index: which try
#define MODRANDOM_STREAM_OBJCOUNT  7 // how many objectives
#define MODRANDOM_STREAM_OBJPOOL   8 // the order the fetchable things are taken in
#define MODRANDOM_STREAM_OBJROOM   9 // index: which objective's room
#define MODRANDOM_STREAM_TARGET   10 // which guard becomes which named target
#define MODRANDOM_STREAM_ALLY     11 // how many guards become buddy allies
#define MODRANDOM_STREAM_FEMALE   12 // female model assigned to each guard
#define MODRANDOM_STREAM_STARTGUN 13 // the player's one starting gun
#define MODRANDOM_MAX_ALLIES       3
#define MODRANDOM_BASE_CIVILIANS   3
#define MODRANDOM_ARM_CIVS_EXTRA   10
#define MODRANDOM_MAX_CIVILIANS    (MODRANDOM_BASE_CIVILIANS + MODRANDOM_ARM_CIVS_EXTRA)

struct modrandomstream {
	u32 state;
};

struct modrandomtarget {
	s32 chrnum;
	s32 bodynum;
	s32 headnum;
	const char *name;
	s32 padnum;
	struct coord spawnpos;
	struct coord finishpos;
	u8 team;
	bool spawned;
	bool complete;
	bool pacify; // knock her out rather than kill her
	bool armed;  // she has been seen holding a gun (she surrenders once it is gone)
	bool killedbyplayer; // a pacify target the player killed rather than knocked out
	bool relocated;
	bool relocateRequested;
	bool needsRescue;
	s32 relocateAt60;
	s32 blockedSince60;
	s32 rescueCheckAt60;
	s32 nextFacingAt60;
	f32 facingAngle;
};

struct modrandomprofile {
	s32 bodynum;
	s32 headnum;
	const char *name;
};

static const struct modrandomprofile g_ModRandomTargetProfiles[] = {
	{ BODY_TRENT,      HEAD_TRENT,      "Trent" },
	{ BODY_MRBLONDE,   HEAD_MRBLONDE,   "Mr Blonde" },
	{ BODY_CARRINGTON, HEAD_CARRINGTON, "Carrington" },
	{ BODY_SECRETARY,  HEAD_SECRETARY,  "the Secretary" },
	{ BODY_PRESIDENT,  HEAD_PRESIDENT,  "the President" },
};

static const struct modrandomprofile g_ModRandomFemaleProfiles[] = {
	{ BODY_STEWARDESS,  HEAD_RANDOM,      "the Stewardess" },
	{ BODY_SECRETARY,  HEAD_SECRETARY,   "the Secretary" },
	{ BODY_FEM_GUARD,  HEAD_FEM_GUARD,   "the Female Guard" },
	{ BODY_FEM_GUARD,  HEAD_FEM_GUARD2,  "the Female Guard" },
};

static const struct modrandomprofile g_ModRandomJoannaProfiles[] = {
	{ BODY_DARK_COMBAT,      HEAD_DARK_COMBAT, "Joanna" },
	{ BODY_DARK_TRENCH,      HEAD_DARK_COMBAT, "Joanna" },
	{ BODY_DARK_FROCK,       HEAD_DARK_FROCK,  "Joanna" },
	{ BODY_DARK_RIPPED,      HEAD_DARK_FROCK,  "Joanna" },
	{ BODY_DARK_AF1,         HEAD_DARK_COMBAT, "Joanna" },
	{ BODY_DARK_LEATHER,     HEAD_DARK_COMBAT, "Joanna" },
	{ BODY_DARK_NEGOTIATOR,  HEAD_DARK_COMBAT, "Joanna" },
	{ BODY_DARKWET,          HEAD_DARKAQUA,    "Joanna" },
	{ BODY_DARKAQUALUNG,     HEAD_DARKAQUA,    "Joanna" },
	{ BODY_DARKSNOW,         HEAD_DARK_SNOW,   "Joanna" },
	{ BODY_DARKLAB,          HEAD_DARK_COMBAT, "Joanna" },
};

static const struct modrandomprofile g_ModRandomCassandraProfile = {
	BODY_CASSANDRA, HEAD_CASSANDRA, "Cassandra De Vries"
};

static const struct modrandomprofile g_ModRandomVelvetProfile = {
	BODY_DARK_COMBAT, HEAD_VD, "Velvet Dark"
};

// The intro stream's command, as playerReset() reads it.
struct modrandomintrocmd {
	s32 type;
	s32 param1;
	s32 param2;
	s32 param3;
};

#define MODRANDOM_MAXOBJECTIVES 6  // generated per mission (a gauntlet's last mission asks for five)
#define MODRANDOM_OBJBLOCK      64 // bytes an objective's commands get, 16 aligned
#define MODRANDOM_MAXTRIES      16 // key placements tried before giving up on the roll
#define MODRANDOM_TEXTLEN       64

// What a pad has to be for a player to be put down on it. The lift is how far
// above the pad the ground is asked for, since the search takes the highest
// floor strictly *below* the position it is given and a pad standing exactly
// on its own floor would otherwise miss it; the drop is how far under the pad
// that floor may be before the pad is over a catwalk rather than on a floor;
// and -100000 is what the collision system answers when there is no floor at
// all, which is the game's own threshold for the same question
// (chrAdjustPosForSpawn()).
#define MODRANDOM_SPAWNLIFT      10.0f
#define MODRANDOM_SPAWNDROP      400.0f
#define MODRANDOM_SETTLEHEIGHT   50.0f
#define MODRANDOM_NOGROUND       (-100000.0f)

static u32 g_ModRandomSeed;    // the run's, as the player sees it
static s32 g_ModRandomVersion; // the generator this run is dealt by
static s32 g_ModRandomStartingWeapon = WEAPON_NONE;
// Arena missions seed this many persistent, map-wide guards. Keep it aligned
// with the extra character-slot reserve in modRandomGetAllyReserve(). Twenty-
// four covers the arenas while leaving stage memory for head models, ragdolls
// and civilian/punishment systems.
#define MODRANDOM_ARENA_GUARDS 24
#define MODRANDOM_LARGE_ARENA_GUARDS 36

static s32 modRandomArenaGuardLimit(void)
{
#ifndef PLATFORM_N64
	const char *name = gelevelsName(g_Vars.stagenum);
	if (name && (strcmp(name, "Stack") == 0 || strcmp(name, "Basement") == 0
			|| strcmp(name, "Library") == 0)) {
		return MODRANDOM_LARGE_ARENA_GUARDS;
	}
#endif
	return MODRANDOM_ARENA_GUARDS;
}
static u64 g_ModRandomArenaArmed;
static s32 g_ModRandomUtilityWeapon = WEAPON_NONE;
static s32 g_ModRandomLastGun = WEAPON_NONE;     // dealt to the mission before: never dealt twice running
static s32 g_ModRandomLastUtility = WEAPON_NONE;
s32 modRandomGetUtilityWeapon(void) { return modRandomIsOn() ? g_ModRandomUtilityWeapon : WEAPON_NONE; }

// The generated objectives: their command stream, which objectiveCheck()
// walks exactly as it walks the setup file's own, and their text, which the
// briefing and the HUD ask for by objective index.
// What the roll found in the stream, gathered once and used by every step.
struct modrandomlists {
	struct weaponobj **weapons;   // floor pickups only - see modRandomGather()
	struct defaultobj **crates;
	struct packedchr **chrs;
	u16 *vanillachrpads;         // captured before any guard placement randomization
	struct doorobj **doors;
	struct keyobj **keys;
	struct tag **tags;
	struct defaultobj **tagobjs; // what each tag names, resolved at roll time
	u32 *portalkeys;             // what each portal wants before it can be walked through
	s32 numportals;
	s32 numweapons;
	s32 numcrates;
	s32 numchrs;
	s32 numdoors;
	s32 numkeys;
	s32 numtags;
};

// Endless Mode's run: what the level's roll found, kept for the whole level so
// the next objective can be dealt during play, and the score, which is how much
// of the level the player got through before dying.
static struct modrandomlists g_ModRandomLists;
static u8 *g_ModRandomReached;   // the walk, from the start the roll chose
static u8 *g_ModRandomVisited;   // rooms the player has stood in
static s32 g_ModRandomRooms;     // how many of them
static s32 g_ModRandomCleared;   // objectives finished this run
static s32 g_ModRandomDealt;     // objectives dealt, which is the draw's index
static bool g_ModRandomRunOver;

// Where the roll wants the mission to start, and whether the player has been
// put there yet: 0 nothing to move, 1 waiting for the mission to begin,
// 2 asked for.
static s32 g_ModRandomSpawnPad = -1;
static bool g_ModRandomSpawnPending;

static u32 *g_ModRandomObjCmds;
static struct weaponobj **g_ModRandomPool; // things left to be asked for, in the order they will be
static s32 g_ModRandomPoolSize;
static s32 g_ModRandomLastRoom = -1;       // so the next room objective is not the one just reached
static char g_ModRandomObjText[MODRANDOM_MAXOBJECTIVES][MODRANDOM_TEXTLEN];
static s32 g_ModRandomNumObjectives;
static u32 g_ModRandomShieldPaid; // objectives already paid their quarter shield
static s32 g_ModRandomAnnouncedObjective;
static struct modrandomtarget *g_ModRandomTargets;
static s32 g_ModRandomNumTargets;
static s32 g_ModRandomAllyProfiles[MODRANDOM_MAX_ALLIES];
static s32 *g_ModRandomGuardIds;
static u8 *g_ModRandomGuardPending;
static bool g_ModRandomGuardsCaptured;
static s32 g_ModRandomGuardRespawn60;
static s32 g_ModRandomPacifyFailAt60; // when a killed pacify target ends the mission
static struct weaponobj *g_ModRandomCassandraGuns[8];
static s32 g_ModRandomNumCassandraGuns;
static s32 g_ModRandomTargetRetry60;
static s32 g_ModRandomTargetActive60;
static s32 g_ModRandomEndDelay60;
// The climax: a zone to reach and hold once the last target has fallen
#define MODRANDOM_CLIMAX_WAVE2_60 (60 * 30) // the second wave, this long after the computer appears
static s32 g_ModRandomClimaxSlot = -1;   // the objective's slot, -1 when this mission has none
static bool g_ModRandomClimaxStarted;
static bool g_ModRandomClimaxDone;
static struct coord g_ModRandomClimaxPos;
static s32 g_ModRandomLocatorTarget = -1;
static s32 g_ModRandomLocatorUntil60;
static struct prop *g_ModRandomClimaxTerminal; // the computer to activate
static s32 g_ModRandomClimaxPad = -1;
static s32 g_ModRandomClimaxWave;        // waves sent so far
static s32 g_ModRandomClimaxWaveAt60;    // when the first wave arrives
static s32 g_ModRandomClimaxWave2At60;
static s32 g_ModRandomCutsceneMuteUntil60; // 0 = not muted

void modRandomMuteCutscene(void)
{
	g_ModRandomCutsceneMuteUntil60 = g_Vars.lvframe60 + TICKS(60 * 45);
}

void modRandomUnmuteCutscene(void)
{
	g_ModRandomCutsceneMuteUntil60 = 0;
}

bool modRandomCutsceneMuted(void)
{
	return (modRandomIsOn() || modSpawnEditIsOn()) && g_ModRandomCutsceneMuteUntil60 != 0
		&& g_Vars.lvframe60 < g_ModRandomCutsceneMuteUntil60;
}

struct modrandomcivilian {
	s32 chrnum;
	s32 destinationPad;
	s32 nextthink60;
	bool dead;
	bool armed; // taken up arms: no longer wandering or surrendering
};
static struct modrandomcivilian g_ModRandomCivilians[MODRANDOM_MAX_CIVILIANS];
static s32 g_ModRandomCivilianSpawn60;
static s32 g_ModRandomArmCivsNext;
static s32 g_ModRandomArmCivsRetry60;
static s32 g_ModRandomArmCivsUntil60;
#ifndef PLATFORM_N64
static bool g_ModRandomGuaranteedPlaced[2048]; // Spawn Customizer Guaranteed Guards already stood up
static s32 g_ModRandomGuaranteedTicks;
#endif
static s32 g_ModRandomCivilianWaypoints;
extern struct prop *chrGiveWeaponWithAutoModel(struct chrdata *chr, s32 weaponnum, u32 flags);
static void modRandomEventReset(void);
static u8 g_ModRandomSurrendered[1024]; // by chrnum: gave up, and stays so
static bool g_ModRandomPurgeOn;   // Purge the innocent is running
static bool g_ModRandomExplosiveOn; // Explosive exit is running
static s32 g_ModRandomExplosiveStart60;
static s32 g_ModRandomExplosiveEnd60;
#define MODRANDOM_MAX_BLASTS 32
// the blasts to come: where, which rooms, and when (lvframe60)
static struct { struct coord pos; RoomNum rooms[8]; s32 at60; bool used; s32 chrnum; } g_ModRandomBlasts[MODRANDOM_MAX_BLASTS];
static s32 g_ModRandomPurgeEnd60;

static bool modRandomSequential(void)
{
	return modRandomHasReinforcements() && !modRandomIsEndless() && !modRunIsOn()
		&& modRandomGetVersion() >= 14;
}

struct modrandompatrol {
	s32 chrnum;
	struct path path;
	bool pending;
};
static struct modrandompatrol *g_ModRandomPatrols;
static s32 g_ModRandomNumPatrols;

struct path *modRandomGetPatrolPath(struct chrdata *chr)
{
	s32 i;
	if (!chr || !modRandomIsOn() || modRandomGetVersion() < 15) return NULL;
	for (i = 0; i < g_ModRandomNumPatrols; i++) {
		if (g_ModRandomPatrols[i].chrnum == chr->chrnum) return &g_ModRandomPatrols[i].path;
	}
	return NULL;
}


/**
 * Whether this stage is one to deal again.
 *
 * The Carrington Institute is the backdrop the Perfect Menu is drawn over, and
 * it is a level like any other as far as the code is concerned - it loads a
 * setup file with weapons, guards and pads in it, and the first version of
 * this rolled it. That put a random gun in the firing range, moved where the
 * player stands behind the menu and dealt an objective in a building with no
 * mission, all before the title screen had finished drawing.
 */
static bool modRandomStageIsMission(s32 stagenum)
{
	return STAGE_IS_LEVEL(stagenum) && stagenum != modDataBgStage(STAGE_CITRAINING);
}

/**
 * Whether a mission is being dealt again.
 *
 * Solo only. The Combat Simulator deals its own arena from mpsetup, and
 * co-operative and counter-operative have a second player whose start this
 * would move out from under them.
 */
/**
 * A mission armed as a random one from the Randomizer page, or by
 * --random-mission for a headless run that cannot press the page's button.
 *
 * The arming is a flag and not a setting, the way Ghost Trials arms a trial:
 * a setting is saved to pd.ini, and coming in through that door for one
 * mission should not leave every later mission dealt again. There was such a
 * setting once - Mod.Randomizer, the checkbox that the Randomizer page
 * replaced - and it outlived its checkbox in every pd.ini written while it
 * was ticked, dealing every Solo Mission again with nothing left in the menu
 * to switch it off. So nothing in pd.ini turns the roll on: the two doors
 * on the Randomizer page do, and the stock Solo Missions item disarms it on
 * the way past.
 */
static bool g_ModRandomArmed;

void modRandomArmMission(void)
{
	g_ModRandomArmed = true;
}

void modRandomDisarmMission(void)
{
	g_ModRandomArmed = false;
#ifndef PLATFORM_N64
	modSpawnEditClear(); // a mission started from a menu is not the Spawn Customizer
#endif
}

bool modRandomIsArmed(void)
{
	return g_ModRandomArmed;
}

bool modRandomIsOn(void)
{
	// A run deals every map it lands in, whatever the setting says: the roll
	// is what makes a landing worth making twice. See modrun.c.
	return (g_ModRandomArmed || modRunIsOn())
		&& !modSpawnEditIsOn() // the Spawn Customizer loads a map with nothing dealt
		&& !g_Vars.normmplayerisrunning
		&& !g_Vars.mplayerisrunning
		&& modRandomStageIsMission(g_Vars.stagenum);
}

/**
 * Endless Mode: the mission never ends, it deals another objective.
 *
 * A mission's objectives run out, which is the point of a mission and the
 * opposite of the point of a randomizer - the interesting part is the next
 * unknown thing, and stock runs out of those after three. So this deals one
 * objective at a time and deals another the moment it is finished, and the run
 * ends where a run should end, at the first death.
 *
 * The score is rooms: how much of the level the player got through before
 * dying, counted as rooms stood in and never counted twice. Kills would score
 * standing still in a doorway and objectives would score the same run twice,
 * but a room is ground covered, and covering ground is what the objectives are
 * for.
 */
bool modRandomHasReinforcements(void)
{
	return modRandomIsOn() && (!modRunIsOn() || modRunIsArenaMission());
}

bool modRandomIsEndless(void)
{
	// Not during a run: a run deals its own objective for the room it landed
	// in and scores that, and Endless Mode dealing a second one across the
	// whole map would be two modes asking for two different things at once.
	return modRandomIsOn() && !modRunIsOn() && g_ModOptions.randomendless != 0;
}

/**
 * The run so far: rooms covered and objectives finished. For the menu, and for
 * anything that wants to show a run's score while it is still being made.
 */
s32 modRandomGetRooms(void)
{
	return g_ModRandomRooms;
}

s32 modRandomGetCleared(void)
{
	return g_ModRandomCleared;
}

/**
 * The generator the current mission was dealt by, for the menu to show
 * beside the seed: the two together are the run.
 */
s32 modRandomGetVersion(void)
{
	return g_ModRandomVersion ? g_ModRandomVersion : MODRANDOM_VERSION;
}

s32 modRandomGetWarzoneReserve(void)
{
	return modRandomHasReinforcements() && !modRandomIsEndless() && !modRunIsOn()
		? MODRANDOM_WARZONE_ALLIES + MODRANDOM_WARZONE_ENEMIES : 0;
}

s32 modRandomGetAllyReserve(void)
{
	// setup.c's model manager is sized before modRandomRoll() chooses the
	// actual count, so reserve the version 10 maximum at both sizing points.
#ifndef PLATFORM_N64
	if (modSpawnEditIsOn()) return modSpawnEditChrSlots(); // the Spawn Customizer's characters
#endif
	if (!modRandomIsOn()) return 0;
	// Two replacements per original guard, two deferred targets, and up to
	// two bonus buddies per kill. Corpses retain their own model slots.
	return modRandomHasReinforcements() && !modRandomIsEndless() && !modRunIsOn() ? MODRANDOM_MAX_ALLIES + 8 + 16 + 24 + MODRANDOM_MAX_CIVILIANS
		+ modRandomGetWarzoneReserve()
		+ 2 * setupCountCommandType(OBJTYPE_CHR) + (modRunIsArenaMission() ? modRandomArenaGuardLimit() : 0) : MODRANDOM_MAX_ALLIES;
}

static bool modRandomChrKnockedOut(struct chrdata *chr)
{
	// a pacify target is pacified by surrendering as well as by being knocked out
	return chr->actiontype == ACT_SURRENDER
		|| chr->actiontype == ACT_DRUGGEDCOMINGUP
		|| chr->actiontype == ACT_DRUGGEDDROP
		|| chr->actiontype == ACT_DRUGGEDKO;
}

// A target that was in play and is no longer anywhere has gone into a kill
// floor (or been removed by the level): that counts as a death.
static bool modRandomTargetGone(const struct modrandomtarget *target, const struct chrdata *chr)
{
	return target->spawned && (!chr || !chr->prop || !chr->model);
}

s32 modRandomTargetStatus(s32 chrnum)
{
	s32 i;
	if (!modRandomSequential()) return -1;
	for (i = 0; i < g_ModRandomNumTargets; i++) {
		struct modrandomtarget *target = &g_ModRandomTargets[i];
		struct chrdata *chr;
		if (target->chrnum != chrnum) continue;
		if (target->pacify && target->killedbyplayer) return OBJECTIVE_FAILED;
		if (target->complete) return OBJECTIVE_COMPLETE;
		if (i != g_ModRandomCleared || !target->spawned) return OBJECTIVE_INCOMPLETE;
		chr = chrFindByLiteralId(chrnum);
		if (target->pacify) {
			// out cold completes it, and so does her dying by anyone's hand but
			// the player's; the player killing her fails it
			if (target->killedbyplayer) return OBJECTIVE_FAILED;
			return modRandomTargetGone(target, chr) || (chr && (modRandomChrKnockedOut(chr) || chrIsDead(chr)))
				? OBJECTIVE_COMPLETE : OBJECTIVE_INCOMPLETE;
		}
		return modRandomTargetGone(target, chr) || (chr && chrIsDead(chr)) ? OBJECTIVE_COMPLETE : OBJECTIVE_INCOMPLETE;
	}
	return -1;
}

s32 modRandomGetStartingWeapon(void)
{
	return modRandomIsOn() && modRandomGetVersion() >= 13
		? g_ModRandomStartingWeapon : WEAPON_NONE;
}

/** True while the current target is to be knocked out rather than killed. */
bool modRandomObjectiveTargetIsPacify(void)
{
	return modRandomSequential() && !g_ModRandomRunOver && g_ModRandomTargets
		&& g_ModRandomCleared < g_ModRandomNumTargets
		&& g_ModRandomTargets[g_ModRandomCleared].spawned
		&& g_ModRandomTargets[g_ModRandomCleared].pacify;
}

void modRandomUseTargetLocator(void)
{
	s32 chrnum = modRandomGetObjectiveTarget();

	if (modRandomIsOn() && chrnum >= 0) {
		g_ModRandomLocatorTarget = chrnum;
		g_ModRandomLocatorUntil60 = g_Vars.lvframe60 + TICKS(3 * 60);
	}
}

bool modRandomGetTargetLocatorPos(struct coord *pos, bool *pacify)
{
	struct chrdata *target;

	if (!modRandomIsOn() || g_ModRandomLocatorTarget < 0
			|| g_Vars.lvframe60 >= g_ModRandomLocatorUntil60
			|| modRandomGetObjectiveTarget() != g_ModRandomLocatorTarget) {
		return false;
	}

	target = chrFindByLiteralId(g_ModRandomLocatorTarget);
	if (!target || !target->prop || chrIsDead(target)) {
		return false;
	}

	*pos = target->prop->pos;
	*pacify = modRandomObjectiveTargetIsPacify();
	return true;
}

s32 modRandomGetObjectiveTarget(void)
{
	s32 index = modRandomIsEndless() ? g_ModRandomDealt : 0;
	s32 i;
	if (modRandomSequential()) {
		return !g_ModRandomRunOver && g_ModRandomTargets
			&& g_ModRandomCleared < g_ModRandomNumTargets
			&& g_ModRandomTargets[g_ModRandomCleared].spawned
			? g_ModRandomTargets[g_ModRandomCleared].chrnum : -1;
	}

	if (!modRandomIsOn() || modRandomGetVersion() < 8 || g_ModRandomRunOver
			|| g_ModRandomTargets == NULL) {
		return -1;
	}

	if (!modRandomIsEndless() && modRandomGetVersion() >= 14) {
		index = -1;
		for (i = 0; i < g_ModRandomNumObjectives; i++) {
			if (objectiveCheck(i) != OBJECTIVE_COMPLETE) {
				index = i;
				break;
			}
		}
	}

	if (index < 0 || index >= g_ModRandomNumTargets) {
		return -1;
	}

	return g_ModRandomTargets[index].chrnum;
}

/**
 * The seed the current mission was dealt from, for the menu to show.
 */
u32 modRandomGetSeed(void)
{
	return g_ModRandomSeed;
}

/**
 * xorshift32, and a mix on the way in so that seed 1 and seed 2 are different
 * missions rather than the same one a step apart.
 */
/**
 * The run's own number, mixed once: the seed, the stage and the difficulty.
 * Every stream below hangs off it, so one run's Villa and its Chicago are
 * different missions and Perfect Agent is its own deal rather than the Agent
 * one with more guards.
 */
static u32 g_ModRandomRun;

/**
 * A 32 bit finalizer - the mix from splitmix64's tail, in 32 bits. Every bit
 * of the output depends on every bit of the input, which is what makes seed 1
 * and seed 2 different missions rather than the same one a step apart.
 */
static u32 modRandomMix(u32 x)
{
	x ^= x >> 16;
	x *= 0x7feb352d;
	x ^= x >> 15;
	x *= 0x846ca68b;
	x ^= x >> 16;

	return x;
}

/**
 * Open the stream for one decision.
 *
 * **This is what makes a seed survive a change to the generator.** A single
 * PRNG walked from the first decision to the last means every draw's value
 * depends on how many draws came before it: adding one retry to the weapon
 * roll moves the guards, the keys and the objectives, and a seed written down
 * last week deals a different mission today. That is exactly what happened
 * between the first two builds of this file.
 *
 * So there is no single stream. Each decision opens its own, seeded from the
 * run, a stream id naming what the decision is *about*, and an index naming
 * which one of them it is - the fourth weapon spot, the second objective. A
 * draw's value then depends on nothing but those three, so:
 *
 * - changing how the weapon roll works moves weapons, and nothing else;
 * - adding a whole new kind of thing to randomize takes a new stream id and
 *   disturbs no existing one, which is the common case and needs no version
 *   bump at all;
 * - the order the roll runs its steps in stops mattering.
 *
 * What it cannot save is a change to what a draw *means* - a weapon dropped
 * from the pool, a different rule for which pads can be a start. Those change
 * the mission a seed deals however the numbers are drawn, and those are what
 * MODRANDOM_VERSION and the guards on it are for.
 */
static void modRandomOpen(struct modrandomstream *rng, s32 streamid, s32 index)
{
	u32 state = modRandomMix(g_ModRandomRun + modRandomMix(streamid * 0x9e3779b9u + index));

	rng->state = state ? state : 0x9e3779b9;
}

static u32 modRandomNext(struct modrandomstream *rng)
{
	rng->state ^= rng->state << 13;
	rng->state ^= rng->state >> 17;
	rng->state ^= rng->state << 5;

	return rng->state;
}

static u32 modRandomBelow(struct modrandomstream *rng, u32 limit)
{
	return limit ? modRandomNext(rng) % limit : 0;
}

/**
 * Fisher-Yates over an array of pointers, out of one stream: a shuffle is one
 * decision however many swaps it takes.
 */
static void modRandomShuffle(struct modrandomstream *rng, void **array, s32 count)
{
	s32 i;

	for (i = count - 1; i > 0; i--) {
		s32 j = modRandomBelow(rng, i + 1);
		void *tmp = array[i];

		array[i] = array[j];
		array[j] = tmp;
	}
}

static void *modRandomAlloc(s32 count, s32 size)
{
	if (count <= 0) {
		return NULL;
	}

	return mempAlloc(ALIGN16(count * size), MEMPOOL_STAGE);
}

/**
 * A pad's room, or -1. Valid only once setupPreparePads() has filled in the
 * rooms the file left at -1, which is why the roll runs after it.
 */
static s32 modRandomPadRoom(s32 padnum)
{
	struct pad pad;

	if (padnum < 0 || g_PadsFile == NULL || padnum >= g_PadsFile->numpads) {
		return -1;
	}

	// Not chrGetPadRoom(): that one is for AI script parameters, where a value
	// under 10000 is already a room number and is handed straight back, so
	// asking it about a pad gets the pad number returned as a room.
	padUnpack(padnum, PADFIELD_ROOM, &pad);

	return pad.room;
}

/**
 * Where a player put down on this pad would actually stand, or nothing when
 * the answer is "nowhere they would survive".
 *
 * A spawn hands a position to playerStartNewLife(), which asks
 * cdFindGroundInfoAtCyl() for the floor under it and stands the player on what
 * comes back. That search takes the highest floor **strictly below** the y it
 * is given, and it has one answer for finding nothing: -4294967296. A pad with
 * no floor beneath it therefore does not fail, it succeeds with the player
 * four billion units under the level, falling, and that is the whole of
 * "sometimes it spawns you out of bounds and you die". It is not rare - 2% to
 * 8% of a stage's waypoints are like it, 31 of one stage's 371 - and every one
 * of them is a landing the run was willing to deal.
 *
 * The other way a spawn kills is a GEOFLAG_DIE tile, which bondwalk.c kills
 * whoever stands on at the first walk tick.
 *
 * So the question is asked here instead, from MODRANDOM_SPAWNLIFT above the
 * pad so that the floor the pad is standing on is inside the search rather
 * than exactly level with its edge, and what is handed back is a position on
 * **the floor that was found**, not the pad's own. The game's query then
 * repeats this one and comes to the same floor.
 *
 * This runs at the roll, inside setupCreateProps(), which is late enough:
 * lvReset() loads the bg and builds its tables before it reads the setup file
 * at all, and bodyAllocateChr() does its own cdTestVolume() from the same
 * walk.
 *
 * The position and the verdict are two calls because they are asked at
 * different moments and want different answers - see modRandomPadCanSpawn().
 */
static bool modRandomPadGround(s32 padnum, struct pad *pad, f32 *ground, u16 *floorflags)
{
	struct coord query;
	RoomNum rooms[2];
	RoomNum floorroom = -1;

	*floorflags = 0;

	if (padnum < 0 || g_PadsFile == NULL || padnum >= g_PadsFile->numpads) {
		return false;
	}

	padUnpack(padnum, PADFIELD_POS | PADFIELD_ROOM, pad);

	if (pad->room <= 0 || pad->room >= g_Vars.roomcount) {
		return false;
	}

	rooms[0] = pad->room;
	rooms[1] = -1;

	query = pad->pos;
	query.y += MODRANDOM_SPAWNLIFT;

	*ground = cdFindGroundInfoAtCyl(&query, 30, rooms, NULL, NULL, floorflags, &floorroom, NULL, NULL);

	// Nothing under the pad at all, which is the whole of the fault: there is
	// no position to hand back that is not four billion units under the level.
	return floorroom >= 0 && *ground >= MODRANDOM_NOGROUND;
}

bool modRandomPadSpawnPos(s32 padnum, struct coord *pos, RoomNum *room)
{
	struct pad pad;
	u16 floorflags;
	f32 ground;

	if (!modRandomPadGround(padnum, &pad, &ground, &floorflags)) {
		return false;
	}

	if (pos) {
		pos->x = pad.pos.x;
		pos->y = ground + MODRANDOM_SPAWNLIFT;
		pos->z = pad.pos.z;
	}

	if (room) {
		*room = pad.room;
	}

	return true;
}

/**
 * And whether it is a pad worth putting them on at all.
 *
 * The verdict rather than the position: a floor under the pad, not one that
 * kills whoever stands on it, not so far below that the pad is over a drop
 * rather than on a floor, and room for someone to stand. This is what a
 * chooser asks before it deals a pad; modRandomPadSpawnPos() above is what
 * the spawn itself asks once one has been dealt, and answers for a pad this
 * would refuse, since a start already committed to is better placed on its
 * floor than on the pad's own y.
 *
 * Asked at the roll, where the level's own props do not exist yet: the volume
 * test therefore sees the bg alone, which is the same bg the ground came from.
 */
bool modRandomPadCanSpawn(s32 padnum)
{
	struct pad pad;
	RoomNum rooms[2];
	u16 floorflags;
	f32 ground;

	if (!modRandomPadGround(padnum, &pad, &ground, &floorflags)) {
		return false;
	}

	// A generated level's pads with a box are its doors', not spots on the
	// floor: nobody is put down in a doorway
	if (levelgenIsStage(mainGetStageNum()) && (padHasBboxData(padnum) || levelgenPadIsProp(padnum))) {
		return false;
	}

	// A tile that kills whoever stands on it, on the first walk tick.
	if (floorflags & GEOFLAG_DIE) {
		return false;
	}

	// A pad over a drop rather than on a floor: the player would be put down
	// at the bottom of it, which is not where the pad is.
	if (pad.pos.y - ground > MODRANDOM_SPAWNDROP) {
		return false;
	}

	rooms[0] = pad.room;
	rooms[1] = -1;

	// And room to stand, the test bodyAllocateChr() puts every one of the
	// stage's own guards through.
	if (cdTestVolume(&pad.pos, 20, rooms, CDTYPE_ALL, CHECKVERTICAL_YES, 200, -200) == CDRESULT_COLLISION) {
		return false;
	}

	return true;
}

/**
 * Lowers a pad that floats high over its own room's floor onto that floor.
 *
 * GoldenEye Arenas' Complex puts all 49 of its pads between 622 and 841 units
 * up, over floors at 0 and 281 - a GoldenEye multiplayer pad is a spot on the
 * map rather than a height - so modRandomPadCanSpawn() read 33 of them as
 * pads over a drop and the run's pad graph (modAlarmBuildPadWaypoints()) was
 * sixteen waypoints in eleven pieces: guards appeared twenty metres away and
 * never came (F3 20260928-041926). The floor must be the pad's own room's,
 * found straight below it, and it must not kill - a pad over a pit in another
 * room is left where it is.
 *
 * Writes the stage's copy of the pads file, so only the run's builder calls
 * it, and only on a map that came with no waypoints of its own.
 */
bool modRandomPadSettle(s32 padnum)
{
	struct pad pad;
	struct coord query;
	RoomNum rooms[2];
	RoomNum floorroom = -1;
	u16 floorflags = 0;
	f32 ground;
	const f32 drop = modRunIsArenaMission() ? 100.0f : MODRANDOM_SPAWNDROP;

	if (padnum < 0 || g_PadsFile == NULL || padnum >= g_PadsFile->numpads) {
		return false;
	}

	// a generated level's crates and doors stand where they were put
	if (levelgenIsStage(mainGetStageNum()) && levelgenPadIsProp(padnum)) {
		return false;
	}

	padUnpack(padnum, PADFIELD_POS | PADFIELD_ROOM, &pad);

	if (pad.room <= 0 || pad.room >= g_Vars.roomcount) {
		return false;
	}

	rooms[0] = pad.room;
	rooms[1] = -1;

	query = pad.pos;
	query.y += MODRANDOM_SPAWNLIFT;

	ground = cdFindGroundInfoAtCyl(&query, 30, rooms, NULL, NULL, &floorflags, &floorroom, NULL, NULL);

	if (floorroom != pad.room || ground < MODRANDOM_NOGROUND || (floorflags & GEOFLAG_DIE)
			|| pad.pos.y - ground <= drop) {
		return false;
	}

	padSetPosY(padnum, ground + MODRANDOM_SETTLEHEIGHT);

	return true;
}

/**
 * Whether an object stands somewhere of its own, so that moving it means
 * moving a thing in the world.
 *
 * A weapon in a guard's hands or inside a crate has a chr id or a container
 * in the field a free-standing one keeps its pad in (setupPlaceWeapon() reads
 * it as a chr id), so those are left where they are. Their *type* is still
 * rolled - a guard carrying something unexpected is the point - but nothing
 * touches where they are.
 */
static bool modRandomObjIsFreeStanding(struct defaultobj *obj)
{
	return (obj->flags & (OBJFLAG_ASSIGNEDTOCHR | OBJFLAG_INSIDEANOTHEROBJ)) == 0;
}

/**
 * Walk the stream once to count and once to fill.
 */
static void modRandomGather(struct modrandomlists *lists)
{
	struct defaultobj *obj;
	s32 *tagcmdindexes = NULL;
	s32 pass;
	s32 i;

	for (pass = 0; pass < 2; pass++) {
		s32 cmdindex = 0;

		lists->numweapons = 0;
		lists->numcrates = 0;
		lists->numchrs = 0;
		lists->numdoors = 0;
		lists->numkeys = 0;
		lists->numtags = 0;

		obj = (struct defaultobj *)g_StageSetup.props;

		while (obj->type != OBJTYPE_END) {
			switch (obj->type) {
			case OBJTYPE_WEAPON:
				if (pass) {
					lists->weapons[lists->numweapons] = (struct weaponobj *)obj;
				}
				lists->numweapons++;
				break;
			case OBJTYPE_AMMOCRATE:
				if (pass) {
					lists->crates[lists->numcrates] = obj;
				}
				lists->numcrates++;
				break;
			case OBJTYPE_CHR:
				if (((struct packedchr *)obj)->chrnum < 0) {
					break;
				}
				if (pass) {
					lists->chrs[lists->numchrs] = (struct packedchr *)obj;
					lists->vanillachrpads[lists->numchrs] = ((struct packedchr *)obj)->padnum;
				}
				lists->numchrs++;
				break;
			case OBJTYPE_DOOR:
				if (pass) {
					lists->doors[lists->numdoors] = (struct doorobj *)obj;
				}
				lists->numdoors++;
				break;
			case OBJTYPE_KEY:
				if (pass) {
					lists->keys[lists->numkeys] = (struct keyobj *)obj;
				}
				lists->numkeys++;
				break;
			case OBJTYPE_TAG:
				if (pass) {
					lists->tags[lists->numtags] = (struct tag *)obj;
					tagcmdindexes[lists->numtags] = cmdindex;
				}
				lists->numtags++;
				break;
			}

			obj = (struct defaultobj *)((u32 *)obj + setupGetCmdLength((u32 *)obj));
			cmdindex++;
		}

		if (pass == 0) {
			lists->weapons = modRandomAlloc(lists->numweapons, sizeof(void *));
			lists->crates = modRandomAlloc(lists->numcrates, sizeof(void *));
			lists->chrs = modRandomAlloc(lists->numchrs, sizeof(void *));
			lists->vanillachrpads = modRandomAlloc(lists->numchrs, sizeof(u16));
			lists->doors = modRandomAlloc(lists->numdoors, sizeof(void *));
			lists->keys = modRandomAlloc(lists->numkeys, sizeof(void *));
			lists->tags = modRandomAlloc(lists->numtags, sizeof(void *));
			lists->tagobjs = modRandomAlloc(lists->numtags, sizeof(void *));
			tagcmdindexes = modRandomAlloc(lists->numtags, sizeof(s32));

			if ((lists->numtags && (!lists->tagobjs || !tagcmdindexes))
					|| (lists->numweapons && !lists->weapons)
					|| (lists->numcrates && !lists->crates)
					|| (lists->numchrs && (!lists->chrs || !lists->vanillachrpads))
					|| (lists->numdoors && !lists->doors)
					|| (lists->numkeys && !lists->keys)
					|| (lists->numtags && !lists->tags)) {
				// The stage pool is out. Nothing has been rewritten yet, so
				// the mission the disc describes is still intact.
				lists->numweapons = lists->numcrates = lists->numchrs = 0;
				lists->numdoors = lists->numkeys = lists->numtags = 0;
				return;
			}
		}
	}

	// A tag names the object a fixed number of commands along from itself, and
	// nothing has resolved that yet: the walk that sets OBJHFLAG_TAGGED and
	// fills tag->obj is what runs after the roll. objGetTagNum() is therefore
	// useless here and this is the same sum, done early.
	for (i = 0; i < lists->numtags; i++) {
		lists->tagobjs[i] = setupGetObjByCmdIndex(tagcmdindexes[i] + lists->tags[i]->cmdoffset);
	}
}

/**
 * The tag a generated objective can name this object by, or -1.
 */
static s32 modRandomTagNumOf(struct modrandomlists *lists, struct defaultobj *obj)
{
	s32 i;

	for (i = 0; i < lists->numtags; i++) {
		if (lists->tagobjs[i] == obj) {
			return lists->tags[i]->tagnum;
		}
	}

	return -1;
}

/**
 * Roll the kind of thing standing in each slot.
 *
 * g_MpWeapons is the game's own list of what can be a pickup - a weapon
 * number, the model that stands for it on the floor and the scale it is drawn
 * at, which have to agree or the pickup is the wrong shape - so the roll picks
 * a row of that table rather than a weapon number on its own. Index 0 is
 * "Nothing" and is skipped: an empty weapon spot is a spot the player walks
 * past, and a mission where half of them are empty reads as a bug.
 */
static bool modRandomChrCanFire(s32 weaponnum)
{
	struct weapon *weapon = weaponFindById(weaponnum);
	struct weaponfunc *func = weapon ? weapon->functions[FUNC_PRIMARY] : NULL;

	return func != NULL && (func->type & 0xff) == INVENTORYFUNCTYPE_SHOOT;
}

// Set while a special event hands its members their guns: the pool filters
// (a Callisto only for one guard in ten, the assault-rifle swap) must not
// touch those, or a Maian's Callisto came out as something else.
static bool g_ModRandomEventGrant;
static s32 g_ModRandomSkedarSlayer = -1; // which arrival of the swarm carries the Slayer

bool modRandomIsObjectiveChr(s32 chrnum)
{
	if (!modRandomHasReinforcements()) return false;
	for (s32 i = 0; i < g_ModRandomNumTargets; i++) if (chrnum >= 0 && g_ModRandomTargets[i].chrnum == chrnum) return true;
	return false;
}

/**
 * One guard in five carries an assault rifle (AR34, K7 Avenger, Dragon or Shotgun),
 * whatever the roll dealt it. A guard is one of them, or not, by its number.
 */
bool modRandomChrCarriesAssaultRifle(s32 chrnum)
{
	return chrnum >= 0 && ((u32)chrnum * 2654435761u % 5) == 0;
}

bool modRandomNpcAssaultRifle(s32 chrnum, s32 *weaponnum, s32 *modelnum)
{
	static const struct { s32 weaponnum; s32 modelnum; } rifles[] = {
		{ WEAPON_AR34, MODEL_CHRAR34 }, { WEAPON_K7AVENGER, MODEL_CHRAVENGER }, { WEAPON_DRAGON, MODEL_CHRDRAGON },
		{ WEAPON_SHOTGUN, MODEL_CHRSHOTGUN },
	};
	u32 pick;

	if (g_ModRandomEventGrant || !modRandomHasReinforcements() || modRandomIsObjectiveChr(chrnum)
			|| !modRandomChrCarriesAssaultRifle(chrnum)) {
		return false;
	}

	pick = ((u32)chrnum * 2654435761u / 5) % ARRAYCOUNT(rifles);
	*weaponnum = rifles[pick].weaponnum;
	*modelnum = rifles[pick].modelnum;

	return true;
}

bool modRandomNpcWeaponAllowed(s32 chrnum, s32 weaponnum)
{
	if (g_ModRandomEventGrant) return true;
	if (g_ModRandomDebugShotguns && weaponHost(weaponnum) == WEAPON_SHOTGUN) return true; // debug: All Shotguns
	// GoldenEye's own guns are not part of Perfect Random's pool (generator 18 on)
	if (modRandomIsOn() && modRandomGetVersion() >= 18 && WEAPON_IS_GE(weaponnum)) return false;
	if (!modRandomHasReinforcements()) return true;
	// objective targets may carry anything but the Callisto NTG
	if (modRandomIsObjectiveChr(chrnum)) return weaponHost(weaponnum) != WEAPON_CALLISTO;
	switch (weaponHost(weaponnum)) {
	case WEAPON_NONE: case WEAPON_UNARMED: case WEAPON_FALCON2: case WEAPON_FALCON2_SILENCER:
	case WEAPON_FALCON2_SCOPE: case WEAPON_MAGSEC4: case WEAPON_DY357MAGNUM: case WEAPON_DY357LX:
	case WEAPON_CMP150: case WEAPON_CYCLONE: case WEAPON_RCP120:
		return weaponHost(weaponnum) != WEAPON_RCP120;
	case WEAPON_CALLISTO:
		return chrnum >= 0 && ((u32)chrnum * 2654435761u % 10) == 0;
	// the classic guns are as common as the CMP150 (generator 19 on)
	case WEAPON_PP9I: case WEAPON_CC13: case WEAPON_KL01313: case WEAPON_KF7SPECIAL:
	case WEAPON_ZZT: case WEAPON_DMC: case WEAPON_AR53: // (not the RC-P45: an objective gun)
		return modRandomGetVersion() >= 19;
	case WEAPON_AR34: case WEAPON_K7AVENGER: case WEAPON_DRAGON: case WEAPON_SHOTGUN:
		return modRandomChrCarriesAssaultRifle(chrnum); // one guard in five
	case WEAPON_LAPTOPGUN: // one guard in twenty (generator 20 on)
		return modRandomGetVersion() >= 20;
	default: return false;
	}
}

/**
 * The gun a guard gets when what it was rolled is not one a guard may carry.
 * This used to be the CMP150 every time, which made it by far the commonest
 * gun; now it is spread over the side arms and SMGs (generator 19 on).
 */
void modRandomFallbackGun(s32 chrnum, s32 *weaponnum, s32 *modelnum)
{
	static const u8 pool[] = {
		WEAPON_FALCON2, WEAPON_MAGSEC4, WEAPON_CYCLONE, WEAPON_PP9I, WEAPON_CC13, WEAPON_KL01313,
		WEAPON_KF7SPECIAL, WEAPON_ZZT, WEAPON_DMC, WEAPON_AR53, WEAPON_CMP150,
	};
	s32 gun = WEAPON_CMP150;

	if (modRandomIsOn() && modRandomGetVersion() >= 19) {
		gun = pool[((u32)(chrnum < 0 ? 0 : chrnum) * 2654435761u / 7) % ARRAYCOUNT(pool)];
	}

	*weaponnum = gun;
	*modelnum = gun == WEAPON_CMP150 ? MODEL_CHRCMP150 : playermgrGetModelOfWeapon(gun);
}

/**
 * The gun a coop ally (Friend Phone, Warzone) is given: drawn from the same
 * pool an enemy NPC's gun comes from - the weapon table, the same filters
 * (a gun a guard can fire, the classics, the Callisto and rifles only where
 * a guard could have them) - but one ally in three gets an AR34, against about
 * one guard in twenty.
 */
static struct prop *modRandomGiveAllyGun(struct chrdata *chr)
{
	struct modrandomstream rng;
	struct prop *prop = NULL;
	const bool eventgrant = g_ModRandomEventGrant;
	s32 tries;

	modRandomOpen(&rng, 0x414c4c59, chr->chrnum * 131 + (s32)g_Vars.lvframenum);

	if (modRandomBelow(&rng, 3) == 0) { // one ally in three
		g_ModRandomEventGrant = true; // the AR34 is not a gun a guard may carry on its own
		prop = chrGiveWeapon(chr, MODEL_CHRAR34, WEAPON_AR34, 0);
		g_ModRandomEventGrant = eventgrant;

		if (prop) {
			return prop;
		}
	}

	for (tries = 0; tries < 24 && !prop; tries++) {
		struct mpweapon *mpweapon = &g_MpWeapons[1 + modRandomBelow(&rng, MODRANDOM_MPWEAPONS - 1)];

		g_ModRandomEventGrant = false; // so the guard filter below really runs

		if (mpweapon->weaponnum == WEAPON_NONE || mpweapon->model < 0
				|| mpweapon->unlockfeature == MPFEATURE_NEVER
				|| !mpWeaponRowSuitsStage(mpweapon)
				|| mpweapon->priammotype <= 0
				|| !modRandomChrCanFire(mpweapon->weaponnum)
				|| (modRandomGetVersion() >= 18 && WEAPON_IS_GE(mpweapon->weaponnum))
				|| !modRandomNpcWeaponAllowed(chr->chrnum, mpweapon->weaponnum)) {
			continue;
		}

		g_ModRandomEventGrant = true; // already checked above; the hook must not swap it
		prop = chrGiveWeapon(chr, mpweapon->model, mpweapon->weaponnum, 0);
		g_ModRandomEventGrant = eventgrant;
	}

	if (!prop) {
		g_ModRandomEventGrant = true;
		prop = chrGiveWeapon(chr, MODEL_CHRFALCON2, WEAPON_FALCON2, 0);
		g_ModRandomEventGrant = eventgrant;
	}

	return prop;
}

static void modRandomRollWeapons(struct modrandomlists *lists)
{
	s32 i;

	for (i = 0; i < lists->numweapons; i++) {
		struct weaponobj *weapon = lists->weapons[i];
		struct modrandomstream rng;
		// A weapon in a guard's hands has to be something a guard can fire:
		// the table holds the specs, the cloaking device and the boost pill
		// as well, and a guard issued a pair of X-Ray specs is a guard that
		// stands there. Ammo is what tells the two apart.
		bool forchr = (weapon->base.flags & OBJFLAG_ASSIGNEDTOCHR) != 0;
		s32 tries;

		modRandomOpen(&rng, MODRANDOM_STREAM_WEAPON, i);

		// generator 20 on: one guard in twenty carries a Laptop Gun
		if (forchr && modRandomGetVersion() >= 20 && modRandomBelow(&rng, 20) == 0) {
			s32 row;

			for (row = 1; row < MODRANDOM_MPWEAPONS; row++) {
				struct mpweapon *laptop = &g_MpWeapons[row];

				if (laptop->weaponnum == WEAPON_LAPTOPGUN && laptop->model >= 0 && laptop->unlockfeature != MPFEATURE_NEVER
						&& mpWeaponRowSuitsStage(laptop) && modRandomChrCanFire(laptop->weaponnum)) {
					weapon->weaponnum = laptop->weaponnum;
					weapon->base.modelnum = laptop->model;
					weapon->base.extrascale = laptop->extrascale;
					break;
				}
			}

			if (row < MODRANDOM_MPWEAPONS) {
				continue;
			}
		}

		for (tries = 0; tries < 16; tries++) {
			struct mpweapon *mpweapon = &g_MpWeapons[1 + modRandomBelow(&rng, MODRANDOM_MPWEAPONS - 1)];

			// MPFEATURE_NEVER is a row switched off rather than a locked one:
			// GoldenEye's 25 guns sit in the table whether or not they are
			// switched on, and a mod that brings its own weapon list switches
			// them off. Rolling one scattered GoldenEye's guns through a mod's
			// own mission.
			if (mpweapon->weaponnum == WEAPON_NONE || mpweapon->model < 0
					|| (!forchr && mpweapon->weaponnum == WEAPON_DY357MAGNUM)
					|| mpweapon->unlockfeature == MPFEATURE_NEVER
					|| !mpWeaponRowSuitsStage(mpweapon)) {
				continue;
			}

			// GoldenEye's guns are out of the pool; Perfect Dark's classic
			// ones (PP9i to RCP45) stay (generator 18 on)
			if (modRandomGetVersion() >= 18 && WEAPON_IS_GE(mpweapon->weaponnum)) {
				continue;
			}

			if (forchr && mpweapon->priammotype <= 0) {
				continue;
			}

			// Nor one it would only hold like a gun: a grenade, a mine, a
			// knife. The AI fires a guard's weapon and never throws it, so a
			// throwable in a guard's hand was a guard pointing a grenade at
			// the player (F3 20260928-172330). Only weapons whose primary
			// function shoots, from generator version 5.
			if (forchr && modRandomGetVersion() >= 5 && !modRandomChrCanFire(mpweapon->weaponnum)) {
				continue;
			}

			weapon->weaponnum = mpweapon->weaponnum;
			weapon->base.modelnum = mpweapon->model;
#ifndef PLATFORM_N64
			weapon->base.modelnum = gegunsFloorModel(weapon->weaponnum, weapon->base.modelnum);
#endif
			weapon->base.extrascale = mpweapon->extrascale;
			break;
		}
	}
}

/**
 * ... and what is in the crates. The ammo types a crate can hold are the ones
 * a weapon in the table above takes, so the roll draws from the same table
 * and asks it for its ammo rather than picking a number: an ammo type nothing
 * in the mission fires is a crate that does nothing.
 */
static void modRandomRollCrates(struct modrandomlists *lists)
{
	s32 i;

	for (i = 0; i < lists->numcrates; i++) {
		struct ammocrateobj *crate = (struct ammocrateobj *)lists->crates[i];
		struct modrandomstream rng;
		s32 tries;

		modRandomOpen(&rng, MODRANDOM_STREAM_CRATE, i);

		for (tries = 0; tries < 8; tries++) {
			struct mpweapon *mpweapon = &g_MpWeapons[1 + modRandomBelow(&rng, MODRANDOM_MPWEAPONS - 1)];

			if (mpweapon->priammotype > 0) {
				crate->ammotype = mpweapon->priammotype;
				break;
			}
		}
	}
}

/**
 * Move the guards.
 *
 * A permutation of the pads the stage's own guards stand on, so every guard
 * is somewhere a guard was meant to be. bodyAllocateChr() collision-tests the
 * pad before it spawns anyone, so a pad that has since been filled by
 * something else costs that guard rather than crashing.
 *
 * Their patrol lists come with them and will walk them somewhere else
 * entirely, which is fine - a guard patrolling a route that starts across the
 * level is a guard on the move, and a randomizer wants them on the move.
 */
static void modRandomRollNavGuards(struct modrandomlists *lists)
{
	struct waypoint *waypoints = g_StageSetup.waypoints;
	struct modrandomstream rng;
	s32 *guards;
	u8 *valid;
	s32 numwaypoints = 0;
	s32 numguards = 0;
	s32 i;
	if (!waypoints || lists->numchrs < 2) return;
	while (waypoints[numwaypoints].padnum >= 0) numwaypoints++;
	if (numwaypoints < 2) return;
	guards = modRandomAlloc(lists->numchrs, sizeof(s32));
	valid = modRandomAlloc(numwaypoints, sizeof(u8));
	g_ModRandomPatrols = modRandomAlloc(lists->numchrs, sizeof(struct modrandompatrol));
	if (!guards || !valid || !g_ModRandomPatrols) return;
	for (i = 0; i < lists->numchrs; i++) {
		struct packedchr *chr = lists->chrs[i];
		if (chr->chrnum >= 0 && chr->team != TEAM_00 && chr->team != TEAM_NONCOMBAT
				&& !(chr->team & (TEAM_01 | TEAM_ALLY))
				&& !(chr->spawnflags & (SPAWNFLAG_INVINCIBLE | SPAWNFLAG_HIDDEN))) {
			guards[numguards++] = i;
		}
	}
	for (i = 0; i < numwaypoints; i++) {
		valid[i] = waypoints[i].neighbours && waypoints[i].neighbours[0] >= 0
			&& modRandomPadCanSpawn(waypoints[i].padnum);
	}
	modRandomOpen(&rng, MODRANDOM_STREAM_CHRPADS, 1);
	for (i = numguards - 1; i > 0; i--) {
		s32 j = modRandomBelow(&rng, i + 1);
		s32 tmp = guards[i]; guards[i] = guards[j]; guards[j] = tmp;
	}
	// Arena maps have no authored guards or patrols. Give three quarters of
	// their seeded guard force a route; normal missions retain half vanilla,
	// half randomized placement and activity.
	for (i = 0; i < (modRunIsArenaMission() ? (numguards * 3 + 3) / 4 : numguards / 2); i++) {
		struct packedchr *chr = lists->chrs[guards[i]];
		s32 attempt;
		for (attempt = 0; attempt < 64; attempt++) {
			s32 start = modRandomBelow(&rng, numwaypoints);
			s32 current = start;
			s32 route[9];
			s32 count = 1;
			s32 j;
			bool occupied = false;
			if (!valid[start]) continue;
			for (j = 0; j < lists->numchrs; j++) {
				if (lists->chrs[j]->padnum == waypoints[start].padnum) occupied = true;
			}
			if (occupied) continue;
			route[0] = waypoints[start].padnum;
			while (count < 8) {
				s32 chosen = -1;
				s32 choices = 0;
				s32 *neighbours = waypoints[current].neighbours;
				if (!neighbours) break;
				for (j = 0; neighbours[j] >= 0; j++) {
					s32 next = neighbours[j] & 0x3fffffff;
					if ((neighbours[j] & 0xc0000000u) || next >= numwaypoints || next == current || !valid[next]) continue;
					if (count > 1 && waypoints[next].padnum == route[count - 2]) continue;
					if (!modAlarmNavPadsWalkable(waypoints[current].padnum, waypoints[next].padnum)) continue;
					if (modRandomBelow(&rng, ++choices) == 0) chosen = next;
				}
				if (chosen < 0) break;
				current = chosen;
				route[count++] = waypoints[current].padnum;
			}
			if (count >= 2) {
				struct modrandompatrol *patrol = &g_ModRandomPatrols[g_ModRandomNumPatrols];
				s32 *pads = modRandomAlloc(count + 1, sizeof(s32));
				if (!pads) return;
				for (j = 0; j < count; j++) pads[j] = route[j];
				pads[count] = -1;
				patrol->chrnum = chr->chrnum;
				patrol->path.pads = pads;
				patrol->path.id = 0;
				patrol->path.flags = 0; // Ping-pong: every edge was checked in both directions.
				patrol->path.len = count;
				patrol->pending = true;
				chr->padnum = pads[0];
				chr->chair = -1;
				chr->ailistnum = GAILIST_UNALERTED;
				g_ModRandomNumPatrols++;
				break;
			}
		}
	}
}

static void modRandomRollChrs(struct modrandomlists *lists)
{
	struct modrandomstream rng;
	u16 *pads;
	s32 i;

	if (modRandomGetVersion() >= 15 && modRandomHasReinforcements()) {
		modRandomRollNavGuards(lists);
		return;
	}

	if (lists->numchrs < 2) {
		return;
	}

	pads = modRandomAlloc(lists->numchrs, sizeof(u16));

	if (pads == NULL) {
		return;
	}

	for (i = 0; i < lists->numchrs; i++) {
		pads[i] = lists->chrs[i]->padnum;
	}

	modRandomOpen(&rng, MODRANDOM_STREAM_CHRPADS, 0);

	for (i = lists->numchrs - 1; i > 0; i--) {
		s32 j = modRandomBelow(&rng, i + 1);
		u16 tmp = pads[i];

		pads[i] = pads[j];
		pads[j] = tmp;
	}

	for (i = 0; i < lists->numchrs; i++) {
		lists->chrs[i]->padnum = pads[i];
	}
}

/** Remove a hostile guard whose pad is also the player's randomized start. */
static void modRandomRemoveSpawnGuard(struct modrandomlists *lists, s32 spawnpad)
{
	s32 i;

	if (modRandomGetVersion() < 12 || modRunIsOn() || spawnpad < 0) {
		return;
	}

	for (i = 0; i < lists->numchrs; i++) {
		struct packedchr *chr = lists->chrs[i];
	u8 team = chr->team;

		if (chr->padnum != spawnpad || chr->chrnum < 0
				|| team == TEAM_00 || team == TEAM_ALLY || team == TEAM_NONCOMBAT
				|| (team & (TEAM_01 | TEAM_ALLY)) != 0) {
			continue;
		}

		// The setup loader only creates this row on the named difficulty.
		// Clear any existing restriction, then name a different difficulty so
		// the guard is absent from the setup entirely.
		chr->spawnflags &= ~(SPAWNFLAG_ONLYONA | SPAWNFLAG_ONLYONSA | SPAWNFLAG_ONLYONPA);
		chr->spawnflags |= lvGetDifficulty() == DIFF_A ? SPAWNFLAG_ONLYONSA : SPAWNFLAG_ONLYONA;
		chr->chrnum = -1;
		return;
	}
}

/**
 * The room graph, and how far a set of keys gets you into it.
 *
 * portalkeys[p] is what portal p wants before it can be walked through: 0 for
 * a doorway or an unlocked door, otherwise the key flags of the door standing
 * in it. Everything else about a door - that it is closed, that it opens when
 * shot, that a script opens it - is treated as open, because it is.
 */
static u32 *modRandomBuildPortalKeys(struct modrandomlists *lists, s32 *outcount)
{
	u32 *portalkeys;
	s32 numportals = 0;
	s32 i;
	s32 j;

	// The portal numbers this stage actually uses, as its rooms list them.
	for (i = 0; i < g_Vars.roomcount; i++) {
		for (j = 0; j < g_Rooms[i].numportals; j++) {
			s32 portalnum = g_RoomPortals[g_Rooms[i].roomportallistoffset + j];

			if (portalnum >= numportals) {
				numportals = portalnum + 1;
			}
		}
	}

	*outcount = numportals;

	if (numportals <= 0) {
		return NULL;
	}

	portalkeys = modRandomAlloc(numportals, sizeof(u32));

	if (portalkeys == NULL) {
		return NULL;
	}

	for (i = 0; i < numportals; i++) {
		portalkeys[i] = 0;
	}

	for (i = 0; i < lists->numdoors; i++) {
		struct doorobj *door = lists->doors[i];

		if (door->keyflags && modRandomObjIsFreeStanding(&door->base)) {
			s32 portalnum = setupGetPortalByDoorPad(door->base.pad);

			if (portalnum >= 0 && portalnum < numportals) {
				portalkeys[portalnum] |= door->keyflags;
			}
		}
	}

	return portalkeys;
}

/**
 * Walk the graph from the spawn room, taking every key that is standing in a
 * room the walk has already reached, until a pass adds nothing.
 *
 * Keys that are in a guard's hands or inside something are counted as held
 * from the start: where they are is not a room this can read, and a walk that
 * called them unobtainable would reject every placement on a stage that has
 * one. That is the permissive direction, so the check catches the failure it
 * exists for - the key shut behind the door it opens - and not every one.
 */
static void modRandomWalk(struct modrandomlists *lists, s32 spawnroom, u8 *reached)
{
	u32 *portalkeys = lists->portalkeys;
	s32 numportals = lists->numportals;
	u32 held = 0;
	s32 i;
	s32 j;
	bool grew;

	for (i = 0; i < g_Vars.roomcount; i++) {
		reached[i] = 0;
	}

	if (spawnroom < 0 || spawnroom >= g_Vars.roomcount || portalkeys == NULL) {
		return;
	}

	for (i = 0; i < lists->numkeys; i++) {
		struct keyobj *key = lists->keys[i];

		if (!modRandomObjIsFreeStanding(&key->base)) {
			held |= key->keyflags;
		}
	}

	// reached[r] is 0 for a room the walk never got to, and otherwise one more
	// than the number of rooms crossed to get there - so it is both the flag
	// and the distance, which is what puts an objective at the far end of a
	// level rather than in the room next door.
	reached[spawnroom] = 1;

	do {
		grew = false;

		for (i = 0; i < g_Vars.roomcount; i++) {
			if (!reached[i]) {
				continue;
			}

			for (j = 0; j < g_Rooms[i].numportals; j++) {
				s32 portalnum = g_RoomPortals[g_Rooms[i].roomportallistoffset + j];
				s32 neighbour;
				u32 wants;

				if (portalnum < 0 || portalnum >= numportals) {
					continue;
				}

				wants = portalkeys[portalnum];

				if (wants && (held & wants) != wants) {
					continue;
				}

				neighbour = g_BgPortals[portalnum].roomnum1 == i
					? g_BgPortals[portalnum].roomnum2
					: g_BgPortals[portalnum].roomnum1;

				if (neighbour > 0 && neighbour < g_Vars.roomcount && !reached[neighbour]) {
					reached[neighbour] = reached[i] < 254 ? reached[i] + 1 : 254;
					grew = true;
				}
			}
		}

		// Anything picked up in what the walk has reached opens more of it.
		for (i = 0; i < lists->numkeys; i++) {
			struct keyobj *key = lists->keys[i];
			s32 room;

			if (!modRandomObjIsFreeStanding(&key->base) || (held & key->keyflags) == key->keyflags) {
				continue;
			}

			room = modRandomPadRoom(key->base.pad);

			if (room > 0 && room < g_Vars.roomcount && reached[room]) {
				held |= key->keyflags;
				grew = true;
			}
		}
	} while (grew);
}

/**
 * How much of the stage a walk reached.
 */
static s32 modRandomCountReached(u8 *reached)
{
	s32 count = 0;
	s32 i;

	for (i = 0; i < g_Vars.roomcount; i++) {
		if (reached[i]) {
			count++;
		}
	}

	return count;
}

/**
 * Move the keys, and keep the first placement the walk says is playable.
 *
 * The keys move and the locks stay: permuting both would deal a different
 * mission that is the same shape, and it is a key being somewhere new that
 * makes a stage feel re-cut. Free-standing keys only, and only between spots
 * that already held one.
 *
 * A placement is kept when the walk reaches at least as much of the stage as
 * the stage's own does. Measuring against the stage rather than against "all
 * rooms" is deliberate: plenty of rooms are behind a lift or a script the
 * walk cannot model, and a stage whose own layout the walk scores at forty
 * rooms is telling us that forty is what a playable one looks like here.
 */
static void modRandomRollKeys(struct modrandomlists *lists, s32 spawnroom, u8 *reached)
{
	s16 *pads;
	s16 *best;
	s32 numfree = 0;
	s32 stockreach;
	s32 bestreach = -1;
	s32 tries;
	s32 slot;
	s32 i;

	for (i = 0; i < lists->numkeys; i++) {
		if (modRandomObjIsFreeStanding(&lists->keys[i]->base)) {
			numfree++;
		}
	}

	// What the stage's own key placement is worth, from this start.
	modRandomWalk(lists, spawnroom, reached);
	stockreach = modRandomCountReached(reached);

	if (numfree < 2) {
		return;
	}

	pads = modRandomAlloc(numfree, sizeof(s16));
	best = modRandomAlloc(numfree, sizeof(s16));

	if (pads == NULL || best == NULL) {
		return;
	}

	for (i = 0, numfree = 0; i < lists->numkeys; i++) {
		if (modRandomObjIsFreeStanding(&lists->keys[i]->base)) {
			pads[numfree] = lists->keys[i]->base.pad;
			best[numfree] = pads[numfree];
			numfree++;
		}
	}

	for (tries = 0; tries < MODRANDOM_MAXTRIES; tries++) {
		struct modrandomstream rng;
		s32 reach;

		modRandomOpen(&rng, MODRANDOM_STREAM_KEYS, tries);

		for (i = numfree - 1; i > 0; i--) {
			s32 j = modRandomBelow(&rng, i + 1);
			s16 tmp = pads[i];

			pads[i] = pads[j];
			pads[j] = tmp;
		}

		for (i = 0, slot = 0; i < lists->numkeys; i++) {
			if (modRandomObjIsFreeStanding(&lists->keys[i]->base)) {
				lists->keys[i]->base.pad = pads[slot++];
			}
		}

		modRandomWalk(lists, spawnroom, reached);
		reach = modRandomCountReached(reached);

		if (reach > bestreach) {
			bestreach = reach;

			for (i = 0; i < numfree; i++) {
				best[i] = pads[i];
			}
		}

		if (reach >= stockreach) {
			return;
		}
	}

	// Nothing this seed tried opened as much of the stage as the mission's own
	// keys do, so the best of them goes back in and is walked once more -
	// everything after this reads that walk.
	for (i = 0, slot = 0; i < lists->numkeys; i++) {
		if (modRandomObjIsFreeStanding(&lists->keys[i]->base)) {
			lists->keys[i]->base.pad = best[slot++];
		}
	}

	modRandomWalk(lists, spawnroom, reached);
}

/**
 * How long an intro command is, in bytes.
 *
 * The intro stream is not the setup stream and has no setupGetCmdLength() of
 * its own: the lengths live inside the switch statements that read it, in
 * playerReset() and playerStartNewLife(), where they are 12 for some commands
 * and 40 for one. Stepping by a flat 12 walks into the middle of a command and
 * reads a parameter as the next type, which is a rewrite of whatever happens
 * to be there. 0 means "not one this knows", and every walk here stops on it.
 */
static s32 modRandomIntroCmdLen(s32 type)
{
	switch (type) {
	case INTROCMD_SPAWN:        return 12;
	case INTROCMD_WEAPON:       return 16;
	case INTROCMD_AMMO:         return 16;
	case INTROCMD_3:            return 32;
	case INTROCMD_4:            return 8;
	case INTROCMD_OUTFIT:       return 8;
	case INTROCMD_6:            return 40;
	case INTROCMD_WATCHTIME:    return 12;
	case INTROCMD_CREDITOFFSET: return 8;
	case INTROCMD_CASE:         return 12;
	case INTROCMD_CASERESPAWN:  return 12;
	case INTROCMD_HILL:         return 8;
	}

	return 0;
}

static s32 modRandomWriteSpawn(s32 padnum)
{
	struct modrandomintrocmd *cmd = (struct modrandomintrocmd *)g_StageSetup.intro;
	s32 first = -1;

	while (cmd && cmd->type != INTROCMD_END) {
		s32 len = modRandomIntroCmdLen(cmd->type);

		if (len == 0) {
			break;
		}

		if (cmd->type == INTROCMD_SPAWN && cmd->param2 == 0) {
			if (first < 0) {
				first = cmd->param1;
			}

			if (padnum >= 0) {
				cmd->param1 = padnum;
			}
		}

		cmd = (struct modrandomintrocmd *)((uintptr_t)cmd + len);
	}

	return first;
}

#ifndef PLATFORM_N64
/** The Spawn Customizer: the pad the stage's own intro starts the player on. */
s32 modRandomEditStartPad(void)
{
	return modRandomWriteSpawn(-1);
}
#endif

/**
 * Where the mission starts.
 *
 * A solo mission's intro lists one spawn pad - the several a stage has are
 * co-operative's, and they are gated off - so picking between them is picking
 * between one thing, which is how the first version of this managed to roll
 * the same start on every seed. The pads worth starting on are the ones the
 * stage stands a *guard* on: a spot in a room, on the floor, that something
 * man-sized was meant to occupy, and there are dozens of them spread over the
 * level rather than one at the front door.
 *
 * A start is only taken if the walk from it reaches most of what the mission's
 * own start reaches. Otherwise it is a lift interior, a sealed vault or the
 * wrong side of a locked door - all of them somewhere a guard legitimately
 * stands and none of them a mission. The stage's own pad is the fallback and
 * is always tried first, so the worst case is the mission starting where it
 * always did.
 */
static s32 modRandomRollSpawn(struct modrandomlists *lists, u8 *reached)
{
	s32 stockpad = modRandomWriteSpawn(-1);
	s32 stockreach;
	s32 best = stockpad;
	s32 bestreach;
	s32 tries;

	modRandomWalk(lists, modRandomPadRoom(stockpad), reached);
	stockreach = modRandomCountReached(reached);
	bestreach = stockreach;

	for (tries = 0; tries < MODRANDOM_MAXTRIES && lists->numchrs > 0; tries++) {
		struct modrandomstream rng;
		s32 padnum;

		modRandomOpen(&rng, MODRANDOM_STREAM_SPAWN, tries);
		padnum = lists->chrs[modRandomBelow(&rng, lists->numchrs)]->padnum;
		s32 room = modRandomPadRoom(padnum);
		s32 reach;

		if (room <= 0 || room >= g_Vars.roomcount) {
			continue;
		}

		// And somewhere a player can be stood up alive. A guard's pad is a
		// place for a guard, which is not always a place for anybody: a ledge
		// with no floor under it starts the mission four billion units below
		// the level. See modRandomPadSpawnPos().
		if (g_ModRandomVersion >= 3 && !modRandomPadCanSpawn(padnum)) {
			continue;
		}

		modRandomWalk(lists, room, reached);
		reach = modRandomCountReached(reached);

		// Most of the mission, not all of it: a start deeper in the level is
		// past doors it will not have to come back through, and a start that
		// had to reach everything would only ever be the front door again.
		if (reach * 4 >= stockreach * 3) {
			best = padnum;
			bestreach = reach;
			break;
		}
	}

	modRandomWriteSpawn(best);
	modRandomWalk(lists, modRandomPadRoom(best), reached);

	return best;
}

/**
 * What the player starts holding.
 *
 * The intro's weapon commands, rolled from the same table the floor pickups
 * come from. A mission that starts you unarmed still does - the command is
 * rewritten, not added - so a stage that means you to walk in with nothing
 * keeps that, and one that arms you arms you with something else.
 */
static bool modRandomWeaponRowExists(s32 weaponnum)
{
	s32 i;

	for (i = 1; i < MODRANDOM_MPWEAPONS; i++) {
		if (g_MpWeapons[i].weaponnum == weaponnum && g_MpWeapons[i].model >= 0
				&& g_MpWeapons[i].unlockfeature != MPFEATURE_NEVER) {
			return true;
		}
	}

	return false;
}

static void modRandomRollIntroWeapons(void)
{
	struct modrandomintrocmd *cmd = (struct modrandomintrocmd *)g_StageSetup.intro;
	s32 index = 0;
	bool gaveweapon = false;

	g_ModRandomStartingWeapon = WEAPON_NONE;
	if (modRandomGetVersion() >= 13) {
		struct modrandomstream rng;
		// No Phoenix. GoldenEye's pistols are in, for the ones with a Perfect
		// Dark model, alongside the Perfect Dark pistols including the DY357-LX.
		static const s32 pistols[] = {
			WEAPON_FALCON2, WEAPON_FALCON2_SILENCER, WEAPON_FALCON2_SCOPE, WEAPON_MAGSEC4, WEAPON_MAULER,
			WEAPON_DY357LX,
#if PR_GOLDENEYE
			WEAPON_GE_PP7, WEAPON_GE_PP7SILENCED, WEAPON_GE_DD44, WEAPON_GE_COUGARMAGNUM, WEAPON_GE_GOLDENGUN,
#endif
		};
		static const s32 utilities[] = { WEAPON_CROSSBOW, WEAPON_TRANQUILIZER, WEAPON_GRENADE };

		modRandomOpen(&rng, MODRANDOM_STREAM_STARTGUN, 0);
		// generator 18 on: only Perfect Dark pistols, no GoldenEye pistols
		if (modRandomGetVersion() >= 19) {
			// generator 19 on: the PP9i and CC13 are also available
			static const s32 pdpistols[] = {
				WEAPON_FALCON2, WEAPON_FALCON2_SILENCER, WEAPON_FALCON2_SCOPE, WEAPON_MAGSEC4, WEAPON_MAULER,
				WEAPON_DY357LX, WEAPON_PP9I, WEAPON_CC13,
			};

			s32 tries = 0;

			do {
				g_ModRandomStartingWeapon = pdpistols[modRandomBelow(&rng, ARRAYCOUNT(pdpistols))];
			} while (g_ModRandomStartingWeapon == g_ModRandomLastGun && ++tries < 8);
		} else {
			g_ModRandomStartingWeapon = pistols[modRandomBelow(&rng, modRandomGetVersion() >= 18 ? 6 : ARRAYCOUNT(pistols))];
		}
		if (!modRandomWeaponRowExists(g_ModRandomStartingWeapon)) {
			g_ModRandomStartingWeapon = WEAPON_FALCON2;
		}
		if (modRandomGetVersion() >= 19) {
			// generator 19 on: Tranquilizer, Crossbow or N-Bomb
			static const s32 utilities19[] = { WEAPON_TRANQUILIZER, WEAPON_CROSSBOW, WEAPON_NBOMB };

			s32 tries = 0;

			do {
				g_ModRandomUtilityWeapon = utilities19[modRandomBelow(&rng, ARRAYCOUNT(utilities19))];
			} while (g_ModRandomUtilityWeapon == g_ModRandomLastUtility && ++tries < 8);
		} else {
			g_ModRandomUtilityWeapon = utilities[modRandomBelow(&rng, ARRAYCOUNT(utilities))];
		}
		if (g_Vars.stagenum == STAGE_RESCUE) {
			// Area 51: Rescue is played with a remote mine, whatever the roll
			g_ModRandomUtilityWeapon = WEAPON_REMOTEMINE;
		}
		if (g_ModRandomStartingWeapon == WEAPON_NONE) {
			g_ModRandomStartingWeapon = WEAPON_FALCON2;
		}
		// the next mission will not deal the same pair again
		g_ModRandomLastGun = g_ModRandomStartingWeapon;
		g_ModRandomLastUtility = g_ModRandomUtilityWeapon;
	}

	while (cmd && cmd->type != INTROCMD_END) {
		s32 len = modRandomIntroCmdLen(cmd->type);

		if (len == 0) {
			break;
		}

		if (cmd->type == INTROCMD_WEAPON) {
			if (modRandomGetVersion() >= 13) {
				if (!gaveweapon && cmd->param3 == 0) {
					cmd->param1 = g_ModRandomStartingWeapon;
					cmd->param2 = -1;
					gaveweapon = true;
				} else if (cmd->param3 == 0) {
					cmd->param3 = 1;
				}
			} else {
				struct modrandomstream rng;
				struct mpweapon *mpweapon;

				modRandomOpen(&rng, MODRANDOM_STREAM_INTROGUN, index++);
				mpweapon = &g_MpWeapons[1 + modRandomBelow(&rng, MODRANDOM_MPWEAPONS - 1)];

				// A switched-off row is not a gun this game has (see above)
				if (mpweapon->weaponnum != WEAPON_NONE && mpweapon->weaponnum != WEAPON_DY357MAGNUM
						&& mpweapon->unlockfeature != MPFEATURE_NEVER
						&& mpWeaponRowSuitsStage(mpweapon)) {
					cmd->param1 = mpweapon->weaponnum;

					// The second hand is a weapon number too, and -1 for a mission
					// that arms one hand: rolling that into a gun would hand out a
					// pair where the mission meant one.
					if (cmd->param2 >= 0) {
						cmd->param2 = mpweapon->weaponnum;
					}
				}
			}
		} else if (cmd->type == INTROCMD_AMMO && modRandomGetVersion() >= 13 && cmd->param3 == 0) {
			cmd->param3 = 1;
		}

		cmd = (struct modrandomintrocmd *)((uintptr_t)cmd + len);
	}
}

/**
 * Write one generated objective into the command buffer.
 *
 * An objective is a run of commands - BEGINOBJECTIVE, its requirements, then
 * ENDOBJECTIVE - and objectiveCheck() walks that run from the pointer it was
 * given, so a generated one only has to look like the file's. The type is the
 * top byte of the first word wherever a command keeps it, which is what
 * PD_BE32() of a bare type value writes on either endianness.
 */
static struct objective *modRandomObjectiveAt(s32 slot);

static u32 *modRandomWriteObjective(u32 *cmd, s32 index, u32 type, u32 param)
{
	struct objective *objective = (struct objective *)cmd;

	objective->unk00 = PD_BE32(OBJTYPE_BEGINOBJECTIVE);
	objective->index = index;
	objective->text = L_MISC_042; // never read: modRandomGetObjectiveText() answers first
	objective->unk0c = 0;
	objective->flags = 0;
	objective->difficulties = DIFFBIT_A | DIFFBIT_SA | DIFFBIT_PA | DIFFBIT_PD;

	cmd += setupGetCmdLength(cmd);

	if (type == OBJECTIVETYPE_ENTERROOM) {
		struct criteria_roomentered *criteria = (struct criteria_roomentered *)cmd;

		// The field is named pad and is read by chrGetPadRoom(), which takes a
		// value under 10000 as a room number already. A room is what this
		// knows, so a room is what goes in.
		criteria->unk00 = PD_BE32(OBJECTIVETYPE_ENTERROOM);
		criteria->pad = param;
		criteria->status = OBJECTIVE_INCOMPLETE;
		criteria->next = NULL;

		objectiveAddRoomEnteredCriteria(criteria);
	} else {
		cmd[0] = PD_BE32(type);
		cmd[1] = param;
	}

	cmd += setupGetCmdLength(cmd);

	cmd[0] = PD_BE32(OBJTYPE_ENDOBJECTIVE);

	// Nothing to hand back: an objective lives in the block its slot names,
	// which is also what keeps a criteria struct's trailing pointer aligned -
	// three commands do not come to a multiple of eight, and packing them end
	// to end leaves every second one misaligned, which x86 forgives and the
	// arm64 build does not.
	return cmd;
}

/**
 * Deal the mission's objectives from what the roll actually placed.
 *
 * Every objective names something the generator has just seen standing in a
 * room the walk reached: a weapon it put down, a thing the stage already had
 * a tag on, a room a pad leads to. That is the whole discipline here - an
 * objective that names a tag the stage does not have, or a room the player
 * cannot get to, is a mission that cannot be finished, and the player has no
 * way to tell which of the two it is.
 *
 * Three at most, because the briefing draws them and six is where it stops,
 * and because a generated objective is a sentence with no story behind it -
 * three reads as a mission, seven reads as a chore list.
 */
static struct objective *modRandomObjectiveAt(s32 slot)
{
	if (g_ModRandomObjCmds == NULL || slot < 0 || slot >= MODRANDOM_MAXOBJECTIVES) {
		return NULL;
	}

	return (struct objective *)((uintptr_t)g_ModRandomObjCmds + slot * MODRANDOM_OBJBLOCK);
}

/**
 * Choose hostile guard rows for the named kill targets and buddy allies.
 * Target rows keep their id, AI, team, weapons and pad. Version 10 also
 * replaces human hostile guard bodies and spawns buddy allies around the player.
 */
void modRandomPrepareArenaSetup(void)
{
	struct waypoint *waypoints = g_StageSetup.waypoints;
	u32 *stream = g_StageSetup.props, *end = stream, *copy;
	struct packedchr guards[MODRANDOM_LARGE_ARENA_GUARDS];
	struct modrandomstream rng;
	s32 *candidates;
	s32 count = 0, numcandidates = 0, w, words;
	if (!modRunIsArenaMission() || !stream) return;
#ifndef PLATFORM_N64
	if (modSpawnEditIsOn()) return; // no arena guards in the editor
#endif
	for (w = 0; waypoints && waypoints[w].padnum >= 0; w++);
	const s32 numpads = g_PadsFile ? g_PadsFile->numpads : 0;
	if (w == 0 && numpads == 0) return;
	candidates = modRandomAlloc(w > numpads ? w : numpads, sizeof(*candidates));
	if (!candidates) return;
	for (s32 i = 0; i < w; i++) {
		s32 padnum = waypoints[i].padnum;
		if (waypoints[i].neighbours && waypoints[i].neighbours[0] >= 0
				&& modRandomPadCanSpawn(padnum)) candidates[numcandidates++] = padnum;
	}
	if (!numcandidates) {
		// Initial guards can stand on a safe pad even if it lacks a waypoint.
		for (s32 i = 0; i < numpads; i++) {
			modRandomPadSettle(i);
			if (modRandomPadCanSpawn(i)) candidates[numcandidates++] = i;
		}
	}
	if (!numcandidates) return;
	// Shuffle candidates so the default force is distributed through the
	// whole navigable arena, rather than clustering at its first waypoints.
	modRandomOpen(&rng, MODRANDOM_STREAM_CHRPADS, 3);
	for (s32 i = numcandidates - 1; i > 0; i--) {
		s32 j = modRandomBelow(&rng, i + 1);
		s32 tmp = candidates[i]; candidates[i] = candidates[j]; candidates[j] = tmp;
	}
	memset(guards, 0, sizeof(guards));
	const s32 guardlimit = modRandomArenaGuardLimit();
	for (w = 0; w < numcandidates && count < guardlimit; w++) {
		s32 padnum = candidates[w];
		guards[count].typenum = OBJTYPE_CHR;
		guards[count].chrindex = -1;
		guards[count].chrnum = 1000 + count;
		guards[count].padnum = padnum;
		guards[count].bodynum = modRandomGuardBody(0);
		guards[count].headnum = modRandomGuardPackedHead(0);
		guards[count].ailistnum = GAILIST_UNALERTED;
		guards[count].hearscale = 1000;
		guards[count].viewdist = 100;
		guards[count].chair = -1;
		guards[count].padpreset = 0xffff;
		guards[count].chrpreset = 0xffff;
		guards[count].team = TEAM_ENEMY;
		count++;
	}
	if (!count) return;
	while (((struct defaultobj *)end)->type != OBJTYPE_END) end += setupGetCmdLength(end);
	words = end - stream;
	copy = modRandomAlloc(1, words * sizeof(u32) + count * sizeof(struct packedchr) + sizeof(u32));
	if (!copy) return;
	memcpy(copy, stream, words * sizeof(u32));
	memcpy(copy + words, guards, count * sizeof(struct packedchr));
	memcpy((u8 *)(copy + words) + count * sizeof(struct packedchr), end, sizeof(u32));
	g_StageSetup.props = copy;
}

/**
 * DEBUG OVERLAY. Everything between here and modRandomDebugRender() is the
 * overlay: remove it, its calls (grep modRandomDebug) and the player.c hook to
 * take the feature out. Mod.RandomDebug = 0 (or the menu checkbox) turns it off.
 */
s32 g_ModRandomDebug = 0;
s32 g_ModRandomDebugTint = 0;
s32 g_ModRandomDebugRadar = 0;
s32 g_ModRandomDebugDefendersRadar = 0;
s32 g_ModRandomDebugStateCount = 0;
s32 g_ModRandomDebugCloak = 0; // debug: the player is under a constant cloaking device effect
s32 g_ModRandomDebugShotguns = 0; // debug: every gun a chr carries is a shotgun, and the player has one
s32 g_ModRandomDebugMaxGrenades = 0;

/** True when either usable held weapon fires automatically in primary mode. */
bool modRandomChrHasAutomaticWeapon(struct chrdata *chr)
{
	for (s32 hand = 0; hand < 2; hand++) {
		struct prop *held = chrGetHeldUsableProp(chr, hand);

		if (held && held->type == PROPTYPE_WEAPON && held->weapon) {
			struct weaponfunc *func = weaponGetFunctionById(weaponHost(held->weapon->weaponnum), FUNC_PRIMARY);

			if (func && func->type == INVENTORYFUNCTYPE_SHOOT_AUTOMATIC) {
				return true;
			}
		}
	}

	return false;
}

/** Debug: turn a gun a chr is being given into a shotgun (grenades, mines, knives and items stay). */
bool modRandomDebugShotgun(s32 *weaponnum, s32 *modelnum)
{
	const s32 w = *weaponnum;
	const bool gun = (w >= WEAPON_FALCON2 && w <= WEAPON_COMBATKNIFE - 1)
		|| (w >= WEAPON_CROSSBOW && w <= WEAPON_LASER)
		|| (w >= WEAPON_PP9I && w <= WEAPON_PSYCHOSISGUN);

	if (!g_ModRandomDebugShotguns || !gun || w == WEAPON_SHOTGUN) {
		return false;
	}

	*weaponnum = WEAPON_SHOTGUN;
	*modelnum = MODEL_CHRSHOTGUN;
	return true;
}

/** Debug: once per level, put a shotgun (20 rounds) in the player's hands' reach. */
void modRandomDebugGivePlayerShotgun(void)
{
	static bool given = false;

	if (!g_ModRandomDebugShotguns || g_Vars.lvframenum < 5) {
		given = false;
		return;
	}

	if (!given && g_Vars.currentplayer && g_Vars.currentplayer->prop && !g_Vars.currentplayer->isdead) {
		invGiveSingleWeapon(WEAPON_SHOTGUN);
		bgunSetAmmoQuantity(AMMOTYPE_SHOTGUN, 20);
		given = true;
	}
}

void modRandomDebugGiveGrenades(void)
{
	s32 playernum = g_Vars.currentplayernum;
	static const s32 weapons[] = {
		WEAPON_GRENADE, WEAPON_NBOMB, WEAPON_TIMEDMINE, WEAPON_PROXIMITYMINE, WEAPON_REMOTEMINE, WEAPON_COMBATKNIFE
	};

	if (g_ModRandomDebugMaxGrenades && g_Vars.currentplayer) {
		for (s32 i = 0; i < ARRAYCOUNT(weapons); i++) {
			s32 ammo = bgunGetAmmoTypeForWeapon(weapons[i], FUNC_PRIMARY);
			invGiveSingleWeapon(weapons[i]);
			bgunSetAmmoQuantity(ammo, bgunGetCapacityByAmmotype(ammo));
		}
		sysLogPrintf(LOG_NOTE, "debug: gave player %d max grenades", playernum);
	}
}
#define MODDBG_COUNT 6
#define MODDBG_LINES 9
#define MODDBG_LINE_LIFE60 (60 * 9)
static struct model *g_ModDbgModel[1024];
static u8 g_ModDbgCat[1024];
static s32 g_ModDbgTotal[MODDBG_COUNT];
static bool g_ModDbgStageMarked;
static char g_ModDbgText[MODDBG_LINES][72];
static s32 g_ModDbgWhen[MODDBG_LINES];
static s32 g_ModDbgNext;
static const char *const g_ModDbgNames[MODDBG_COUNT] = { "Stage guards", "Reinforcements", "Punishment", "Event", "Respawned guards", "Civilians" };

void modRandomDebugMsg(const char *fmt, ...)
{
	va_list ap;

	if (!g_ModRandomDebug) {
		return;
	}

	va_start(ap, fmt);
	vsnprintf(g_ModDbgText[g_ModDbgNext % MODDBG_LINES], sizeof(g_ModDbgText[0]), fmt, ap);
	va_end(ap);
	g_ModDbgWhen[g_ModDbgNext % MODDBG_LINES] = g_Vars.lvframe60;
	g_ModDbgNext++;
}

void modRandomDebugSpawn(struct chrdata *chr, s32 category)
{
	if (!chr || chr->chrnum < 0 || category < 0 || category >= MODDBG_COUNT) {
		return;
	}

	g_ModDbgModel[chr->chrnum & 1023] = chr->model;
	g_ModDbgCat[chr->chrnum & 1023] = category;
	g_ModDbgTotal[category]++;
	if (category != MODDBG_CIV) {
		modRandomDebugMsg("+ %s (chr %d)", g_ModDbgNames[category], chr->chrnum);
	}
}

static void modRandomDebugReset(void)
{
	memset(g_ModDbgModel, 0, sizeof(g_ModDbgModel));
	memset(g_ModDbgCat, 0, sizeof(g_ModDbgCat));
	memset(g_ModDbgTotal, 0, sizeof(g_ModDbgTotal));
	memset(g_ModDbgText, 0, sizeof(g_ModDbgText));
	g_ModDbgNext = 0;
	g_ModDbgStageMarked = false;
}

/** The guards the stage itself placed, counted once the level is up. */
static void modRandomDebugMarkStage(void)
{
	s32 i;

	if (g_ModDbgStageMarked || g_Vars.lvframenum < 90 || !modRandomIsOn()) {
		return;
	}

	g_ModDbgStageMarked = true;

	for (i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr->chrnum < 0 || !chr->prop || !chr->model || chr->prop->type != PROPTYPE_CHR || chr->race != RACE_HUMAN
				|| chrIsDead(chr) || (chr->team & (TEAM_ALLY | TEAM_NONCOMBAT | TEAM_00 | 0x6c))
				|| g_ModDbgModel[chr->chrnum & 1023] == chr->model || modRandomIsCivilianChr(chr)) {
			continue;
		}

		g_ModDbgModel[chr->chrnum & 1023] = chr->model;
		g_ModDbgCat[chr->chrnum & 1023] = MODDBG_STAGE;
		g_ModDbgTotal[MODDBG_STAGE]++;
	}
}

/** One line of the overlay, in the game's own HUD font and drawn the way a HUD message is. */
static Gfx *modRandomDebugLine(Gfx *gdl, s32 x, s32 y, char *text, u32 colour)
{
	s32 textheight;
	s32 textwidth;

	textMeasure(&textheight, &textwidth, text, g_CharsHandelGothicXs, g_FontHandelGothicXs, 0);
	gdl = text0f153a34(gdl, x, y, x + textwidth, y + textheight, 0);

	return textRender(gdl, &x, &y, text, g_CharsHandelGothicXs, g_FontHandelGothicXs, colour, 0x000000a0,
			viGetWidth(), viGetHeight(), 0, 0);
}

static bool modRandomTacticsOn(void);
static Gfx *modRandomDebugStateCounts(Gfx *gdl, s32 left, s32 *y);

Gfx *modRandomDebugRender(Gfx *gdl)
{
	s32 living[MODDBG_COUNT] = {0};
	char text[96];
	s32 left;
	s32 y = viGetViewTop() + 24;
	s32 i, first;
	bool showstates = g_ModRandomDebugStateCount && modRandomTacticsOn();

	if ((!g_ModRandomDebug && !showstates) || !modRandomIsOn() || lvIsPaused()) {
		return gdl;
	}

	// hudmsgsRender() sets the HUD's horizontal scale (2 in hi-res) for its
	// text and puts it back to 1 when it is done; text is laid out with it
	g_ScaleX = g_ViRes == VIRES_HI ? 2 : 1;
	left = viGetViewLeft() / g_ScaleX + 8;

	// HUD text is drawn in the HUD's aspect mode (left-aligned); without it the
	// glyphs are stretched across the whole screen width and come out as blobs
	gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, g_HudAlignModeL);

	// and bracketed by the text begin/end calls every HUD text routine makes:
	// they set up the render state that makes glyphs crisp
	gdl = text0f153628(gdl);

	if (g_ModRandomDebug) {
		for (i = 0; i < g_NumChrSlots; i++) {
			struct chrdata *chr = &g_ChrSlots[i];

			if (chr->chrnum >= 0 && chr->prop && chr->model && g_ModDbgModel[chr->chrnum & 1023] == chr->model
					&& !chrIsDead(chr)) {
				living[g_ModDbgCat[chr->chrnum & 1023]]++;
			}
		}

		for (i = 0; i < MODDBG_COUNT; i++) {
			if (g_ModDbgTotal[i] == 0 && i != MODDBG_STAGE && i != MODDBG_CIV) {
				continue;
			}

			snprintf(text, sizeof(text), "%s  %d/%d", g_ModDbgNames[i], living[i], g_ModDbgTotal[i]);
			gdl = modRandomDebugLine(gdl, left, y, text, 0xffff00ff);
			y += 7;
		}
		y += 4;
	}

	if (showstates) {
		gdl = modRandomDebugStateCounts(gdl, left, &y);
		y += 4;
	}

	if (g_ModRandomDebug) {
		first = g_ModDbgNext > MODDBG_LINES ? g_ModDbgNext - MODDBG_LINES : 0;

		for (i = first; i < g_ModDbgNext; i++) {
			const s32 slot = i % MODDBG_LINES;
			const s32 age = g_Vars.lvframe60 - g_ModDbgWhen[slot];

			if (age > MODDBG_LINE_LIFE60 || age < 0) {
				continue;
			}

			snprintf(text, sizeof(text), "%s", g_ModDbgText[slot]);
			gdl = modRandomDebugLine(gdl, left, y, text, age > MODDBG_LINE_LIFE60 - 120 ? 0xffffff80 : 0xffffffff);
			y += 7;
		}
	}

	gdl = text0f153780(gdl);
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
	g_ScaleX = 1;

	return gdl;
}

/**
 * G5 Building and Carrington Institute's Defense run their own scripts: wave
 * spawns, timers, alarms and fail conditions that fight the generated mission.
 * On those two maps Perfect Random supplies everything itself, so the stage's
 * background lists and its guards' own lists are cut.
 */
bool modRandomStripsStageScripts(void)
{
	return modRandomIsOn() && (g_Vars.stagenum == STAGE_G5BUILDING || g_Vars.stagenum == STAGE_DEFENSE || g_Vars.stagenum == STAGE_AIRFORCEONE);
}

/*
 * Character IDs are local to each mission. These rows are cast, corpses,
 * robots or encounter templates that use the same team as ordinary guards.
 * See PERFECT_RANDOM_NPC_PURGE.md for the audited setup rows and AI spawns.
 */
static bool modRandomIsScriptActor(s32 stage, const struct packedchr *chr)
{
	const s32 id = chr->chrnum;
	const u16 list = chr->ailistnum;

	if ((chr->spawnflags & (SPAWNFLAG_HIDDEN | SPAWNFLAG_INVINCIBLE))
			|| list == GAILIST_INVINCIBLE_AND_IDLE || chr->bodynum == BODY_CHICROB) {
		return true;
	}

	switch (stage) {
	case STAGE_DEFECTION:
	case STAGE_INVESTIGATION:
		return (chr->spawnflags & SPAWNFLAG_BLUESIGHT) != 0
			|| (stage == STAGE_INVESTIGATION && id == 0x3a);
	case STAGE_EXTRACTION:
		return id == 0x0e;
	case STAGE_VILLA:
		return id >= 0x4e && id <= 0x54;
	case STAGE_CHICAGO:
		return id == 0x11 || id == 0x12 || (id >= 0x17 && id <= 0x1f);
	case STAGE_G5BUILDING:
		return list == 0x0410 || list == 0x0411; // meeting and intro
	case STAGE_INFILTRATION:
		return list == 0x041c || list == 0x0419 // guard templates
			|| (id >= 0x1f && id <= 0x24) || (id >= 0x29 && id <= 0x2b)
			|| (id >= 0x30 && id <= 0x35);
	case STAGE_RESCUE:
		return (id >= 0x2c && id <= 0x34) || id == 0x37
			|| (id >= 0x38 && id <= 0x44) || (id >= 0x47 && id <= 0x49);
	case STAGE_ESCAPE:
		return (id >= 0x09 && id <= 0x28) || id == 0x2b;
	case STAGE_AIRBASE:
		return id <= 0x01 || (id >= 0x38 && id <= 0x3d) || (id >= 0x44 && id <= 0x4c)
			|| list == 0x042a;
	case STAGE_AIRFORCEONE:
		return chr->bodynum == BODY_OVERALL || chr->bodynum == BODY_STEWARD
			|| chr->bodynum == BODY_STEWARDESS || chr->bodynum == BODY_PILOTAF1
			|| id == 0x00 || (id >= 0x16 && id <= 0x1a) || (id >= 0x30 && id <= 0x37);
	case STAGE_CRASHSITE:
		return list == 0x0422 || list == 0x0414 || list == 0x041a || list == 0x041e;
	case STAGE_PELAGIC:
		return chr->bodynum == BODY_DD_LABTECH || chr->bodynum == BODY_PILOTAF1;
	case STAGE_DEEPSEA:
		return list == 0x0411 || list == 0x0432 || id == 0x3f
			|| (id >= 0x30 && id <= 0x35) || (id >= 0x38 && id <= 0x3b)
			|| id == 0x41 || id == 0x42;
	case STAGE_DEFENSE:
		return (id >= 0x24 && id <= 0x2d) || (id >= 0x30 && id <= 0x34)
			|| list == 0x0401 || list == 0x0404 || list == 0x0407
			|| list == 0x040a || list == 0x040d || list == 0x0410;
	case STAGE_ATTACKSHIP:
		return list == 0x0412 || list == 0x0415 || list == 0x040b
			|| id == 0x02 || id == 0x30 || id == 0x34;
	case STAGE_SKEDARRUINS:
		return list == 0x0405 || list == 0x0406 || (id >= 0x33 && id <= 0x3b);
	case STAGE_MBR:
		return list == 0x0412 || list == 0x0414;
	case STAGE_MAIANSOS:
		return list == 0x0410 || list == 0x0413 || list == 0x0419 || list == 0x041c;
	case STAGE_WAR:
		return list == 0x040c;
	default:
		return false;
	}
}

static bool modRandomIsDormantGuard(s32 stage, const struct packedchr *chr)
{
	switch (stage) {
	case STAGE_G5BUILDING: return chr->bodynum == BODY_G5_GUARD;
	case STAGE_EXTRACTION: return chr->bodynum == BODY_FEM_GUARD;
	case STAGE_PELAGIC: return chr->bodynum == BODY_PELAGIC_GUARD;
	case STAGE_ATTACKSHIP: return chr->bodynum == BODY_SKEDAR && chr->ailistnum == GAILIST_IDLE;
	default: return false;
	}
}

static void modRandomDropPurgedActorKeys(const struct packedchr *chr)
{
	struct defaultobj *obj = (struct defaultobj *)g_StageSetup.props;

	while (obj->type != OBJTYPE_END) {
		if (obj->type == OBJTYPE_KEY && (obj->flags & OBJFLAG_ASSIGNEDTOCHR)
				&& obj->pad == chr->chrnum) {
			// Feed the key into the normal placement roll from its owner's pad.
			obj->flags &= ~OBJFLAG_ASSIGNEDTOCHR;
			obj->pad = chr->padnum;
		}

		obj = (struct defaultobj *)((u32 *)obj + setupGetCmdLength((u32 *)obj));
	}
}

static void modRandomPrepareStageGuards(s32 stage)
{
	struct defaultobj *obj = (struct defaultobj *)g_StageSetup.props;
	const bool converted = modloaderStageIsMission(stage);

	while (obj->type != OBJTYPE_END) {
		if (obj->type == OBJTYPE_CHR) {
			struct packedchr *chr = (struct packedchr *)obj;
			const u8 team = chr->team;
			const bool hostile = team != TEAM_00 && (team & (TEAM_01 | TEAM_ALLY | TEAM_NONCOMBAT)) == 0;
			const bool guard = converted
				? team != TEAM_ALLY && team != TEAM_NONCOMBAT && !gexPlusRomRowIsAlly(chr->bodynum)
				: hostile || (team == TEAM_NONCOMBAT && modRandomIsDormantGuard(stage, chr));

			if (chr->chrnum >= 0 && (!guard || modRandomIsScriptActor(stage, chr))) {
#ifndef PLATFORM_N64
				sysLogPrintf(LOG_NOTE, "randomizer: purged stock NPC stage %02x chr %04x body %u list %04x",
						stage, chr->chrnum, chr->bodynum, chr->ailistnum);
#endif
				modRandomDropPurgedActorKeys(chr);
				chr->chrnum = -1;
			} else if (chr->chrnum >= 0) {
				chr->team = TEAM_ENEMY;
				chr->ailistnum = GAILIST_UNALERTED;
				chr->chair = -1;
				chr->flags2 &= ~(CHRFLAG1_DOINGIDLEANIMATION | CHRFLAG1_WARFIRSTKING);
			}
		}

		obj = (struct defaultobj *)((u32 *)obj + setupGetCmdLength((u32 *)obj));
	}
}

#ifndef PLATFORM_N64
/**
 * Spawn Customizer configs. When the player has chosen a saved config for
 * this map (modspawnedit.c, "Configs in a run"), place hostile guards on its
 * shuffled Guard spawns. Sequential objectives reserve their own spawns for
 * later rounds. Other modes put guards on Objective spawns first so those
 * guards can be chosen as targets.
 */
static void modRandomApplyConfigGuards(struct modrandomlists *lists)
{
	struct modrandomstream rng;
	s32 *guards;
	s32 *slots;
	s32 numguards = 0;
	s32 numslots = 0;
	s32 nobjective;
	s32 nguard;
	s32 placed = 0;
	s32 removed = 0;
	s32 i;

	if (lists->numchrs <= 0) {
		return;
	}

	nobjective = modRandomSequential() ? 0 : modSpawnConfigCount(MODSPAWN_OBJECTIVE);
	nguard = modSpawnConfigCount(MODSPAWN_GUARD);

	if (nobjective + nguard == 0) {
		return;
	}

	guards = modRandomAlloc(lists->numchrs, sizeof(s32));
	slots = modRandomAlloc(nobjective + nguard, sizeof(s32));

	if (!guards || !slots) {
		return;
	}

	for (i = 0; i < lists->numchrs; i++) {
		struct packedchr *chr = lists->chrs[i];

		if (chr->chrnum >= 0 && chr->team != TEAM_00 && chr->team != TEAM_NONCOMBAT
				&& !(chr->team & (TEAM_01 | TEAM_ALLY))) {
			guards[numguards++] = i;
		}
	}

	modRandomOpen(&rng, MODRANDOM_STREAM_CHRPADS, 7);

	for (i = 0; i < nobjective; i++) {
		slots[numslots++] = modSpawnConfigPad(MODSPAWN_OBJECTIVE, i);
	}

	for (i = 0; i < nguard; i++) {
		slots[numslots++] = modSpawnConfigPad(MODSPAWN_GUARD, i);
	}

	// which spawn each guard takes: the Guard spawns are shuffled, the
	// Objective ones stay first so the first guards stand on them
	for (i = numslots - 1; i > nobjective; i--) {
		s32 j = nobjective + modRandomBelow(&rng, i - nobjective + 1);
		s32 tmp = slots[i];

		slots[i] = slots[j];
		slots[j] = tmp;
	}

	for (i = numguards - 1; i > 0; i--) {
		s32 j = modRandomBelow(&rng, i + 1);
		s32 tmp = guards[i];

		guards[i] = guards[j];
		guards[j] = tmp;
	}

	for (i = 0; i < numguards; i++) {
		struct packedchr *chr = lists->chrs[guards[i]];
		s32 j;

		if (i < numslots) {
			chr->padnum = slots[i];
			lists->vanillachrpads[guards[i]] = slots[i];
			chr->chair = -1;
			chr->ailistnum = GAILIST_UNALERTED;
			placed++;

			for (j = 0; j < g_ModRandomNumPatrols; j++) {
				if (g_ModRandomPatrols[j].chrnum == chr->chrnum) {
					g_ModRandomPatrols[j].pending = false;
					g_ModRandomPatrols[j].chrnum = -1;
				}
			}
		} else if (nguard > 0) {
			// more guards than the config has places for
			chr->spawnflags &= ~(SPAWNFLAG_ONLYONA | SPAWNFLAG_ONLYONSA | SPAWNFLAG_ONLYONPA);
			chr->spawnflags |= lvGetDifficulty() == DIFF_A ? SPAWNFLAG_ONLYONSA : SPAWNFLAG_ONLYONA;
			chr->chrnum = -1;
			removed++;
		}
	}

	sysLogPrintf(LOG_NOTE, "spawnconfig: %d guards placed on the config's spawns, %d left out", placed, removed);
}

/** The player starts on one of the config's Escape/Spawn spawns, if it has any. */
static s32 modRandomApplyConfigStart(struct modrandomlists *lists, u8 *reached, s32 spawnpad)
{
	struct modrandomstream rng;
	const s32 count = modSpawnConfigCount(MODSPAWN_ESCAPE);
	s32 pad;

	if (count <= 0) {
		return spawnpad;
	}

	modRandomOpen(&rng, MODRANDOM_STREAM_SPAWN, 77);
	pad = modSpawnConfigPad(MODSPAWN_ESCAPE, modRandomBelow(&rng, count));
	modRandomWriteSpawn(pad);

	if (lists->portalkeys) {
		modRandomWalk(lists, modRandomPadRoom(pad), reached);
	}

	sysLogPrintf(LOG_NOTE, "spawnconfig: start on pad %d (room %d)", pad, modRandomPadRoom(pad));
	return pad;
}

/** Guards standing on an Objective spawn come first, so the kill target is one of them. */
static void modRandomApplyConfigTargetOrder(struct packedchr **candidates, s32 numcandidates)
{
	s32 first = 0;
	s32 i;

	if (modSpawnConfigCount(MODSPAWN_OBJECTIVE) <= 0) {
		return;
	}

	for (i = 0; i < numcandidates; i++) {
		if (modSpawnConfigIsPadOfKind(MODSPAWN_OBJECTIVE, candidates[i]->padnum)) {
			struct packedchr *tmp = candidates[first];

			candidates[first++] = candidates[i];
			candidates[i] = tmp;
		}
	}
}
#endif

static void modRandomPrepareKillTargets(struct modrandomlists *lists, s32 spawnpad)
{
	struct packedchr **candidates;
	struct modrandomstream rng;
	s32 numcandidates = 0;
	s32 wanted;
	s32 i;

	if (lists->numchrs <= 0) {
		return;
	}

	candidates = modRandomAlloc(lists->numchrs, sizeof(*candidates));
	if (candidates == NULL) {
		return;
	}

	for (i = 0; i < lists->numchrs; i++) {
		struct packedchr *chr = lists->chrs[i];
		u8 team = chr->team;
		bool hostile = chr->chrnum >= 0
			&& team != TEAM_00 && team != TEAM_ALLY && team != TEAM_NONCOMBAT
			&& (team & (TEAM_01 | TEAM_ALLY)) == 0;

		if (modRandomGetVersion() >= 11 && hostile) {
			s32 profile = 2; // Joanna's full dress for the level's original guards
			if (!modRandomHasReinforcements()) {
				struct modrandomstream joannrng;
				modRandomOpen(&joannrng, MODRANDOM_STREAM_FEMALE, i);
				profile = modRandomBelow(&joannrng, ARRAYCOUNT(g_ModRandomJoannaProfiles));
			}
			chr->bodynum = g_ModRandomJoannaProfiles[profile].bodynum;
			chr->headnum = g_ModRandomJoannaProfiles[profile].headnum;
			if (!modRandomHasReinforcements() && modHeadsCount() > 0) {
				// the bank's faces on the level's own female guards too
				struct modrandomstream facerng;
				modRandomOpen(&facerng, MODRANDOM_STREAM_FEMALE + 100, i);
				chr->headnum = modHeadsPack(modRandomFemaleHead(modRandomBelow(&facerng, modRandomFemaleHeadCount())));
			}
			if (modRandomHasReinforcements()) {
				chr->bodynum = modRandomGuardBody(0);
				chr->headnum = modRandomGuardPackedHead(0);
			}
		} else if (modRandomGetVersion() >= 10 && hostile) {
			struct modrandomstream femalerng;
			s32 profile;

			modRandomOpen(&femalerng, MODRANDOM_STREAM_FEMALE, i);
			profile = modRandomBelow(&femalerng, ARRAYCOUNT(g_ModRandomFemaleProfiles));
			chr->bodynum = g_ModRandomFemaleProfiles[profile].bodynum;
			chr->headnum = g_ModRandomFemaleProfiles[profile].headnum;
			if (modHeadsCount() > 0) {
				// the bank's faces on the level's own female guards too
				struct modrandomstream facerng;
				modRandomOpen(&facerng, MODRANDOM_STREAM_FEMALE + 100, i);
				chr->headnum = modHeadsPack(modRandomFemaleHead(modRandomBelow(&facerng, modRandomFemaleHeadCount())));
			}
		}

		if (chr->chrnum < 0
				|| team == TEAM_00 || team == TEAM_ALLY || team == TEAM_NONCOMBAT
				|| (team & (TEAM_01 | TEAM_ALLY)) != 0) {
			continue;
		}

		// Objectives must stay on original, in-bounds guard sites. In
		// particular, never inherit the navigation roll's new pad or patrol.
		if (modRandomHasReinforcements()) {
			struct pad pad;
			u16 floorflags;
			f32 ground;
			s32 original = lists->vanillachrpads[i];
			u32 difficulties = chr->spawnflags & (SPAWNFLAG_ONLYONA | SPAWNFLAG_ONLYONSA | SPAWNFLAG_ONLYONPA);
			s32 j;
			if (difficulties && !((difficulties & SPAWNFLAG_ONLYONA && lvGetDifficulty() == DIFF_A)
					|| (difficulties & SPAWNFLAG_ONLYONSA && lvGetDifficulty() == DIFF_SA)
					|| (difficulties & SPAWNFLAG_ONLYONPA && lvGetDifficulty() == DIFF_PA))) continue;
			if (original == spawnpad || !modRandomPadCanSpawn(original)
					|| !modRandomPadGround(original, &pad, &ground, &floorflags)
					|| pad.pos.y < ground - 5.0f || pad.pos.y > ground + 120.0f) continue;
			// Keep distinct targets at distinct original locations.
			for (j = 0; j < i; j++) {
				if (lists->vanillachrpads[j] == original) break;
			}
			if (j < i) continue;
		}
		candidates[numcandidates++] = chr;
	}

	if (numcandidates == 0) {
		return;
	}
	if (modRandomHasReinforcements() && spawnpad >= 0) {
		// An objective guard stands at least 20 metres (2000 units) from where
		// the player starts. Farthest first; the near ones are dropped, except
		// that a map with fewer than two far guards keeps its two farthest.
		struct pad spawn;
		f32 *dist = modRandomAlloc(numcandidates, sizeof(*dist));
		s32 farcount = 0;
		padUnpack(spawnpad, PADFIELD_POS, &spawn);
		for (i = 0; dist && i < numcandidates; i++) {
			struct pad location;
			s32 j;
			dist[i] = 0;
			for (j = 0; j < lists->numchrs && lists->chrs[j] != candidates[i]; j++);
			if (j >= lists->numchrs || lists->vanillachrpads[j] < 0) continue;
			padUnpack(lists->vanillachrpads[j], PADFIELD_POS, &location);
			dist[i] = (location.pos.x - spawn.pos.x) * (location.pos.x - spawn.pos.x)
					+ (location.pos.z - spawn.pos.z) * (location.pos.z - spawn.pos.z);
		}
		for (i = 1; dist && i < numcandidates; i++) {
			s32 j = i;
			while (j > 0 && dist[j] > dist[j - 1]) {
				struct packedchr *tmp = candidates[j];
				f32 d = dist[j];
				candidates[j] = candidates[j - 1];
				dist[j] = dist[j - 1];
				candidates[j - 1] = tmp;
				dist[j - 1] = d;
				j--;
			}
		}
		for (i = 0; dist && i < numcandidates && dist[i] >= 2000.0f * 2000.0f; i++) farcount++;
		if (dist) numcandidates = farcount >= 2 ? farcount : (numcandidates < 2 ? numcandidates : 2);
	}

	modRandomOpen(&rng, MODRANDOM_STREAM_TARGET, 0);

	// A shuffle makes the first target a uniform choice and gives Endless mode
	// a stable, nonrepeating order through the rest of the eligible guards.
	for (i = numcandidates - 1; i > 0; i--) {
		s32 j = modRandomBelow(&rng, i + 1);
		struct packedchr *tmp = candidates[i];

		candidates[i] = candidates[j];
		candidates[j] = tmp;
	}
#ifndef PLATFORM_N64
	modRandomApplyConfigTargetOrder(candidates, numcandidates);
#endif
	if (modRandomHasReinforcements() && spawnpad >= 0) {
		struct pad start;
		padUnpack(spawnpad, PADFIELD_POS, &start);
		for (i = 0; i < numcandidates; i++) {
			struct pad site;
			s32 j;
			for (j = 0; j < lists->numchrs && lists->chrs[j] != candidates[i]; j++);
			if (j >= lists->numchrs) continue;
			padUnpack(lists->vanillachrpads[j], PADFIELD_POS, &site);
			if ((site.pos.x - start.pos.x) * (site.pos.x - start.pos.x)
					+ (site.pos.z - start.pos.z) * (site.pos.z - start.pos.z) >= 800.0f * 800.0f) {
				struct packedchr *tmp = candidates[0];
				candidates[0] = candidates[i];
				candidates[i] = tmp;
				break;
			}
		}
	}

	// Keep at least one hostile for the kill objective. The selected rows keep
	// their pads but become buddies that follow and fight beside the player.
	if (!modRandomHasReinforcements() && modRandomGetVersion() == 9 && numcandidates > 1) {
		struct modrandomstream allyrng;
		s32 numallies;

		modRandomOpen(&allyrng, MODRANDOM_STREAM_ALLY, 0);
		numallies = 1 + modRandomBelow(&allyrng, 3);
		if (numallies >= numcandidates) {
			numallies = numcandidates - 1;
		}

		for (i = 0; i < numallies; i++) {
			struct packedchr *ally = candidates[i];
			s32 profile = modRandomBelow(&allyrng, 2);

			ally->team = TEAM_ALLY;
			ally->ailistnum = GAILIST_BUDDY_MAIN;
			if (profile == 0) {
				ally->bodynum = BODY_STEWARDESS;
				ally->headnum = HEAD_RANDOM;
			} else {
				ally->bodynum = BODY_SECRETARY;
				ally->headnum = HEAD_SECRETARY;
			}
		}

		candidates += numallies;
		numcandidates -= numallies;
	}

	if (modRandomIsEndless()) {
		wanted = numcandidates;
	} else if (modRandomGetVersion() >= 14) {
		// Cassandra missions name three distinct guards, or all candidates on
		// a stage with fewer than three eligible hostiles.
		// A gauntlet asks for two on its first mission and five on its last
		// (modcareer.c); a mission with fewer eligible guards gets what it has.
		const s32 asked = modCareerIsOn() ? modCareerKillObjectives() : 3;

		wanted = numcandidates < asked ? numcandidates : asked;
	} else {
		wanted = 1;
	}
	// Spread objectives across the map, relaxing the desired gap on crowded stages.
	if (wanted > 1) {
		static const f32 spacing[] = { 1800.0f, 1200.0f, 600.0f, 0.0f };
		for (i = 0; i < wanted; i++) {
			s32 pass;
			for (pass = 0; pass < ARRAYCOUNT(spacing); pass++) {
				s32 eligible = 0, choice = -1, j;
				for (j = i; j < numcandidates; j++) {
					s32 row;
					struct pad site;
					bool spaced = true;
					for (row = 0; row < lists->numchrs && lists->chrs[row] != candidates[j]; row++);
					if (row >= lists->numchrs || lists->vanillachrpads[row] < 0) continue;
					padUnpack(lists->vanillachrpads[row], PADFIELD_POS, &site);
					for (s32 prior = 0; prior < i; prior++) {
						s32 priorrow;
						struct pad priorpos;
						for (priorrow = 0; priorrow < lists->numchrs && lists->chrs[priorrow] != candidates[prior]; priorrow++);
						if (priorrow >= lists->numchrs || lists->vanillachrpads[priorrow] < 0) continue;
						padUnpack(lists->vanillachrpads[priorrow], PADFIELD_POS, &priorpos);
						if ((site.pos.x - priorpos.pos.x) * (site.pos.x - priorpos.pos.x)
								+ (site.pos.z - priorpos.pos.z) * (site.pos.z - priorpos.pos.z)
								< spacing[pass] * spacing[pass]) { spaced = false; break; }
					}
					if (spaced && modRandomBelow(&rng, ++eligible) == 0) choice = j;
				}
				if (choice >= 0) {
					struct packedchr *tmp = candidates[i];
					candidates[i] = candidates[choice];
					candidates[choice] = tmp;
					break;
				}
			}
		}
	}
	g_ModRandomTargets = modRandomAlloc(wanted, sizeof(*g_ModRandomTargets));
	if (g_ModRandomTargets == NULL) {
		return;
	}

	for (i = 0; i < wanted; i++) {
		struct packedchr *chr = candidates[i];
		s32 profile = modRandomGetVersion() >= 11 ? 0
			: modRandomBelow(&rng, modRandomGetVersion() >= 10
					? ARRAYCOUNT(g_ModRandomFemaleProfiles)
					: ARRAYCOUNT(g_ModRandomTargetProfiles));

		g_ModRandomTargets[i].chrnum = chr->chrnum;
		g_ModRandomTargets[i].spawned = false;
		g_ModRandomTargets[i].complete = false;
		g_ModRandomTargets[i].armed = false;
		g_ModRandomTargets[i].killedbyplayer = false;
		g_ModRandomTargets[i].pacify = false;
		g_ModRandomTargets[i].relocated = false;
		g_ModRandomTargets[i].relocateRequested = false;
		g_ModRandomTargets[i].needsRescue = false;
		g_ModRandomTargets[i].relocateAt60 = 0;
		g_ModRandomTargets[i].blockedSince60 = 0;
		g_ModRandomTargets[i].rescueCheckAt60 = 0;
		if (modRandomHasReinforcements() && !modRandomIsEndless() && !modRunIsOn()
				&& modRandomGetVersion() >= 14) {
			// Each eligible target has a 20% chance to be Pacify.
			struct modrandomstream pacrng;
			modRandomOpen(&pacrng, 0x50414346, i);
			g_ModRandomTargets[i].pacify = modRandomBelow(&pacrng, 10) < 2;
		}
		g_ModRandomTargets[i].team = chr->team;
		if (modRandomHasReinforcements()) {
			s32 j;
			for (j = 0; j < lists->numchrs; j++) {
				if (lists->chrs[j] == chr) {
					chr->padnum = lists->vanillachrpads[j];
					break;
				}
			}
			chr->ailistnum = GAILIST_UNALERTED;
			chr->chair = -1;
			for (j = 0; j < g_ModRandomNumPatrols; j++) {
				if (g_ModRandomPatrols[j].chrnum == chr->chrnum) {
					g_ModRandomPatrols[j].pending = false;
					g_ModRandomPatrols[j].chrnum = -1;
				}
			}
		}
		g_ModRandomTargets[i].padnum = chr->padnum;
		chr->spawnflags &= ~(SPAWNFLAG_INVINCIBLE | SPAWNFLAG_HIDDEN);
		if (modRandomSequential()) {
			// All targets use the same checked runtime placement, including the first.
			chr->spawnflags &= ~(SPAWNFLAG_ONLYONA | SPAWNFLAG_ONLYONSA | SPAWNFLAG_ONLYONPA);
			chr->spawnflags |= lvGetDifficulty() == DIFF_A ? SPAWNFLAG_ONLYONSA : SPAWNFLAG_ONLYONA;
		}
		if (modRandomGetVersion() >= 11) {
			// The Pacify target defaults to the Carrington lab assistant.
			chr->bodynum = g_ModRandomTargets[i].pacify ? BODY_CIFEMTECH : g_ModRandomCassandraProfile.bodynum;
			chr->headnum = g_ModRandomTargets[i].pacify ? HEAD_SECRETARY : g_ModRandomCassandraProfile.headnum;
			g_ModRandomTargets[i].name = g_ModRandomTargets[i].pacify ? "the Lab Woman" : g_ModRandomCassandraProfile.name;
		} else if (modRandomGetVersion() >= 10) {
			chr->bodynum = g_ModRandomFemaleProfiles[profile].bodynum;
			chr->headnum = g_ModRandomFemaleProfiles[profile].headnum;
			g_ModRandomTargets[i].name = g_ModRandomFemaleProfiles[profile].name;
		} else {
			chr->bodynum = g_ModRandomTargetProfiles[profile].bodynum;
			chr->headnum = g_ModRandomTargetProfiles[profile].headnum;
			g_ModRandomTargets[i].name = g_ModRandomTargetProfiles[profile].name;
		}
		// the player's own pick for Kill target / KO target, if they made one
		const s32 modelslot = g_ModRandomTargets[i].pacify ? 2 : 1;
		const s32 model = g_RandomExtraModels[modelslot];
		g_ModRandomTargets[i].bodynum = chr->bodynum = modRandomExtraBody(modelslot, chr->bodynum);
		{
			// (the setup record's headnum is an s8: a bank head, 512 and up, goes
			// in packed - stored whole it wrapped round to a stock number, head
			// 514 to 2, a body model, and the chr went headless and crashed the
			// renderer, 2026-11-16)
			const s32 head = modRandomExtraHead(modelslot, modHeadsUnpack(chr->headnum));
			g_ModRandomTargets[i].headnum = head;
			chr->headnum = modHeadsPack(head);
		}
		if (modRandomModelIndex(model - 1) >= 0) {
			g_ModRandomTargets[i].name = modRandomExtraModelName(modRandomModelIndex(model - 1) + 1);
		}
	}

	g_ModRandomNumTargets = wanted;
}

static bool modRandomDealObjective(struct modrandomlists *lists, u8 *reached, s32 slot, s32 ordinal)
{
	u32 *cmd = (u32 *)modRandomObjectiveAt(slot);

	if (cmd == NULL) {
		return false;
	}

	if (modRandomGetVersion() >= 8) {
		const struct modrandomtarget *target;
		s32 targetindex = modRandomIsEndless() ? ordinal
			: (modRandomGetVersion() >= 14 ? slot : 0);

		if (targetindex < 0 || targetindex >= g_ModRandomNumTargets) {
			return false;
		}

		target = &g_ModRandomTargets[targetindex];
		modRandomWriteObjective(cmd, slot, OBJECTIVETYPE_COMPFLAGS,
				MODRANDOM_KILLCHR_TAG | (target->chrnum & 0xffff));
		if (!modRandomIsEndless() && modRandomGetVersion() >= 14 && target->pacify) {
			snprintf(g_ModRandomObjText[slot], MODRANDOM_TEXTLEN,
					langTr("Pacify %s (%d of %d)"), target->name, slot + 1, g_ModRandomNumTargets);
		} else if (!modRandomIsEndless() && modRandomGetVersion() >= 14) {
			snprintf(g_ModRandomObjText[slot], MODRANDOM_TEXTLEN,
					langTr("Kill %s (%d of %d)"), target->name, slot + 1, g_ModRandomNumTargets);
		} else {
			snprintf(g_ModRandomObjText[slot], MODRANDOM_TEXTLEN, langTr("Kill %s"), target->name);
		}
		return true;
	}

	// A thing to fetch while there are things left to fetch, and somewhere to
	// be after that. Fetching is the better objective - it names something the
	// player can see and carry - but each one can only be asked for once: the
	// item stays in the inventory, so asking again would complete the moment
	// it was dealt.
	if (ordinal < g_ModRandomPoolSize) {
		struct weaponobj *weapon = g_ModRandomPool[ordinal];
		struct weapon *weapondef = weaponFindById(weapon->weaponnum);
		char *name = weapondef ? langGet(weapondef->name) : NULL;

		// A weapon row with no name of its own reads "Recover the " - say
		// what it is instead of nothing.
		if (name && (name[0] == '\0' || name[0] == '\n')) {
			name = NULL;
		}

		modRandomWriteObjective(cmd, slot, OBJECTIVETYPE_COLLECTOBJ,
				modRandomTagNumOf(lists, &weapon->base));

		if (name) {
			snprintf(g_ModRandomObjText[slot], MODRANDOM_TEXTLEN, langTr("Recover the %s"), name);
		} else {
			snprintf(g_ModRandomObjText[slot], MODRANDOM_TEXTLEN, "%s", langTr("Recover the stolen hardware"));
		}

		return true;
	}

	// Somewhere to be: a room out at the far end of what the walk reached.
	// reached[] holds how many rooms were crossed to get there, so the deep
	// half of the level is everything at more than half the greatest depth -
	// which is a walk across the level rather than into the next room, and
	// still leaves enough rooms to choose between that an endless run does not
	// send the player to the same corner twice running.
	{
		struct modrandomstream rng;
		s32 deepest = 0;
		s32 chosen = -1;
		s32 count = 0;
		s32 i;

		for (i = 1; i < g_Vars.roomcount; i++) {
			if (reached[i] > deepest) {
				deepest = reached[i];
			}
		}

		if (deepest <= 1) {
			return false;
		}

		modRandomOpen(&rng, MODRANDOM_STREAM_OBJROOM, ordinal);

		for (i = 1; i < g_Vars.roomcount; i++) {
			// v1 took only the deepest rooms, which is the far end of the
			// level and about four rooms to choose from; from v2 the deep
			// half of it, so that an endless run asking again and again has
			// somewhere else to send the player each time. A change to what
			// the draw means, so the older runs keep the older rule.
			bool wanted = g_ModRandomVersion >= 2
				? (reached[i] * 2 >= deepest && i != g_ModRandomLastRoom)
				: (reached[i] == deepest);

			if (wanted) {
				count++;

				if (modRandomBelow(&rng, count) == 0) {
					chosen = i;
				}
			}
		}

		if (chosen < 0) {
			return false;
		}

		g_ModRandomLastRoom = chosen;

		modRandomWriteObjective(cmd, slot, OBJECTIVETYPE_ENTERROOM, chosen);
		strcpy(g_ModRandomObjText[slot], "Reach the extraction point");

		return true;
	}
}

/**
 * Deal the mission's objectives from what the roll actually placed.
 *
 * Every objective names something the generator has just seen standing in a
 * room the walk reached: a weapon it put down that the stage already had a tag
 * on, or a room a walk can get to. That is the whole discipline here - an
 * objective that names a tag the stage does not have, or a room the player
 * cannot reach, is a mission that cannot be finished, and the player has no
 * way to tell which of the two it is.
 *
 * Three at most, because the briefing draws them and six is where it stops,
 * and because a generated objective is a sentence with no story behind it -
 * three reads as a mission, seven reads as a chore list. Endless Mode deals
 * one at a time instead, and keeps dealing.
 */
static void modRandomGenerateObjectives(struct modrandomlists *lists, u8 *reached)
{
	struct modrandomstream rng;
	s32 numwanted;
	s32 i;

	g_ModRandomNumObjectives = 0;
	g_ModRandomPoolSize = 0;
	g_ModRandomLastRoom = -1;

	// The weapons the roll left in reachable rooms, which are the things it
	// can ask for by name. Shuffled once and taken in order, so an endless run
	// works through them without asking for the same one twice.
	g_ModRandomPool = modRandomAlloc(lists->numweapons + 1, sizeof(void *));

	if (g_ModRandomPool == NULL) {
		return;
	}

	for (i = 0; i < lists->numweapons; i++) {
		struct weaponobj *weapon = lists->weapons[i];
		s32 room;

		if (!modRandomObjIsFreeStanding(&weapon->base)) {
			continue;
		}

		// A weapon the stage keeps out of reach for its script is not one to
		// send the player for: objTestForPickup() refuses an uncollectable
		// one, and an invisible one is not there to find.
		if (g_ModRandomVersion >= 6
				&& ((weapon->base.flags & OBJFLAG_UNCOLLECTABLE) || (weapon->base.flags2 & OBJFLAG2_INVISIBLE))) {
			continue;
		}

		if (modRandomTagNumOf(lists, &weapon->base) < 0) {
			continue;
		}

		room = modRandomPadRoom(weapon->base.pad);

		if (room > 0 && room < g_Vars.roomcount && reached[room]) {
			g_ModRandomPool[g_ModRandomPoolSize++] = weapon;
		}
	}

	modRandomOpen(&rng, MODRANDOM_STREAM_OBJPOOL, 0);
	modRandomShuffle(&rng, (void **)g_ModRandomPool, g_ModRandomPoolSize);

	if (modRandomGetVersion() >= 14 && !modRandomIsEndless()) {
		// Cassandra missions ask for one objective per distinct target.
		numwanted = g_ModRandomNumTargets;
	} else if (modRandomGetVersion() >= 8) {
		// Endless deals its next named target after the current one dies.
		numwanted = 1;
	} else if (modRandomIsEndless()) {
		// One at a time: the run is the next objective, not a list of them.
		numwanted = 1;
	} else {
		modRandomOpen(&rng, MODRANDOM_STREAM_OBJCOUNT, 0);
		numwanted = 1 + modRandomBelow(&rng, MODRANDOM_MAXOBJECTIVES);
	}

	g_ModRandomObjCmds = modRandomAlloc(MODRANDOM_MAXOBJECTIVES, MODRANDOM_OBJBLOCK);

	if (g_ModRandomObjCmds == NULL) {
		return;
	}

	for (i = 0; i < numwanted; i++) {
		if (!modRandomDealObjective(lists, reached, i, g_ModRandomDealt)) {
			break;
		}

		g_ModRandomDealt++;
		g_ModRandomNumObjectives++;
	}

	// The last objective of a mission with rounds: hold the marked zone
	if (modRandomSequential() && modRandomGetVersion() >= 17
			&& g_ModRandomNumObjectives == g_ModRandomNumTargets
			&& g_ModRandomNumObjectives < MODRANDOM_MAXOBJECTIVES) {
		s32 slot = g_ModRandomNumObjectives;
		u32 *cmd = (u32 *)modRandomObjectiveAt(slot);

		if (cmd) {
			modRandomWriteObjective(cmd, slot, OBJECTIVETYPE_COMPFLAGS, MODRANDOM_HOLD_TAG);
			snprintf(g_ModRandomObjText[slot], MODRANDOM_TEXTLEN,
					"Mission complete, Get to the laptop!");
			g_ModRandomClimaxSlot = slot;
			g_ModRandomNumObjectives++;
		}
	}
}

/**
 * The text a generated objective shows, or NULL when the objective at that
 * index is the stage's own.
 */
char *modRandomGetObjectiveText(s32 index)
{
	// A run's room asks for its own thing, and everything that draws an
	// objective's text comes through here.
	char *runtext = modRunGetObjectiveText(index);

	if (runtext) {
		return runtext;
	}

	if (modRandomIsOn()
			&& index >= 0
			&& index < g_ModRandomNumObjectives) {
		return g_ModRandomObjText[index];
	}

	return NULL;
}

/**
 * A number naming what the roll produced, for checking that a change to this
 * file left old seeds alone.
 *
 * The claim the streams make - that changing one decision moves nothing else -
 * is only worth as much as it can be checked, and reading a mission to see
 * whether it is the same mission is not checking. So each part of the roll
 * gets its own fold of what it decided, printed with the seed: change the
 * weapon roll and the weapon number moves while the others stand still. If one
 * of the others moves too, the change leaked, and every seed written down for
 * this build has quietly become a different mission.
 */
static u32 modRandomFold(u32 hash, u32 value)
{
	return modRandomMix(hash * 0x9e3779b9u + value);
}

// Gauntlet: special events already dealt this run (bit per kind). An event is
// not dealt again until every enabled one has been, then the cycle restarts.
static u32 g_ModRandomEventsUsed;

void modRandomEventHistoryReset(void)
{
	g_ModRandomEventsUsed = 0;
}

static void modRandomChooseEventAtRoll(void)
{
	s32 allowed[MODEVENT_COUNT];
	s32 count = 0, kind;
	g_ModRandomEventChoice = 0;
	if (!modRandomSequential() || modRandomGetVersion() < 20) return;
	for (kind = 1; kind < MODEVENT_COUNT; kind++) {
		if (modCareerEventAllowed(kind)) allowed[count++] = kind;
	}
	if (count == 0) {
		g_ModRandomEventFired = true;
		return;
	}
	if (modCareerIsOn()) {
		s32 fresh[MODEVENT_COUNT];
		s32 nfresh = 0, i;
		for (i = 0; i < count; i++) {
			if (!(g_ModRandomEventsUsed & (1u << allowed[i]))) fresh[nfresh++] = allowed[i];
		}
		if (nfresh == 0) {
			// all played: start a new cycle
			g_ModRandomEventsUsed = 0;
			for (i = 0; i < count; i++) fresh[nfresh++] = allowed[i];
		}
		g_ModRandomEventChoice = fresh[rngRandom() % nfresh];
		g_ModRandomEventsUsed |= 1u << g_ModRandomEventChoice;
		return;
	}
	g_ModRandomEventChoice = allowed[rngRandom() % count];
}

/**
 * Deal the mission.
 *
 * Called from setupCreateProps(), after the pads have been prepared - the
 * walk needs their rooms - and before the stream is built from, which is what
 * makes every rewrite here free.
 */
void modRandomRoll(s32 stagenum)
{
	struct modrandomlists lists;
	u8 *reached;
	s32 spawnpad;
	s32 spawnroom;
	g_ModRandomArenaArmed = 0;

	g_ModRandomNumObjectives = 0;
	g_ModRandomTargets = NULL;
	g_ModRandomNumTargets = 0;
	g_ModRandomGuardIds = NULL;
	g_ModRandomGuardPending = NULL;
	g_ModRandomGuardsCaptured = false;
	g_ModRandomGuardRespawn60 = 0;
	g_ModRandomPacifyFailAt60 = 0;
	g_ModRandomNumCassandraGuns = 0;
	g_ModRandomTargetRetry60 = 0;
	g_ModRandomTargetActive60 = 0;
	g_ModRandomEndDelay60 = 0;
	g_ModRandomClimaxSlot = -1;
	g_ModRandomLocatorTarget = -1;
	g_ModRandomClimaxStarted = false;
	g_ModRandomShieldPaid = 0;
	g_ModRandomAnnouncedObjective = -1;
	modRandomEventReset();
	g_ModRandomClimaxDone = false;
	g_ModRandomClimaxTerminal = NULL;
	g_ModRandomClimaxPad = -1;
	g_ModRandomClimaxWave2At60 = 0;
	g_ModRandomClimaxWave = 0;
	g_ModRandomClimaxWaveAt60 = 0;
	g_ModRandomCutsceneMuteUntil60 = 0;
	memset(g_ModRandomCivilians, 0, sizeof(g_ModRandomCivilians));
	{
		s32 i;
		for (i = 0; i < MODRANDOM_MAX_CIVILIANS; i++) g_ModRandomCivilians[i].chrnum = -1;
	}
	g_ModRandomCivilianSpawn60 = 0;
	g_ModRandomArmCivsUntil60 = 0;
	g_ModRandomCivilianWaypoints = -1;
#ifndef PLATFORM_N64
	memset(g_ModRandomGuaranteedPlaced, 0, sizeof(g_ModRandomGuaranteedPlaced));
	g_ModRandomGuaranteedTicks = 0;
#endif
	g_ModRandomPatrols = NULL;
	g_ModRandomNumPatrols = 0;
	g_ModRandomObjCmds = NULL;
	g_ModRandomSpawnPad = -1;
	g_ModRandomSpawnPending = false;
	g_ModRandomVisited = NULL;
	g_ModRandomReached = NULL;
	g_ModRandomRooms = 0;
	g_ModRandomCleared = 0;
	g_ModRandomDealt = 0;
	g_ModRandomRunOver = false;

#ifndef PLATFORM_N64
	// The Spawn Customizer takes the stage as it is: nothing is dealt.
	if (modSpawnEditOnStageLoad(stagenum)) {
		return;
	}
#endif

	// modRandomIsOn() asks about g_Vars.stagenum, which is this stage by the
	// time the roll runs, but the stage being loaded is the one that matters.
	if (!modRandomIsOn() || !modRandomStageIsMission(stagenum)) {
		return;
	}

	if (g_StageSetup.props == NULL) {
		return;
	}

	modRandomPrepareStageGuards(stagenum);

	if (g_StageSetup.padfiledata == NULL || g_PadsFile == NULL) {
		return;
	}

	if (g_Vars.roomcount <= 0 || g_Rooms == NULL || g_BgPortals == NULL || g_RoomPortals == NULL) {
		return;
	}

	// A run's hop is dealt from the run's seed rather than the menu's, so that
	// a run written down deals the same rooms with the same guns in them.
	g_ModRandomSeed = modRunIsOn()
		? modRunGetSeed()
		: (g_ModOptions.randomseed ? (u32)g_ModOptions.randomseed : rngRandom());

	// A kept seed keeps the generator it was dealt by; a fresh mission every
	// time has nothing to keep and takes the newest. Otherwise a config
	// written by an older build would pin every future run to that build's
	// generator, which is the opposite of what the setting is for.
	if (g_ModOptions.randomseed) {
		g_ModRandomVersion = g_ModOptions.randomversion;
	} else {
		g_ModRandomVersion = MODRANDOM_VERSION;
		g_ModOptions.randomversion = MODRANDOM_VERSION;
	}

	// A run dealt by a generator this build does not have is dealt by the
	// newest one it does, and says so: a mission that quietly differs from the
	// one the seed was written down for is worse than being told it will.
	if (g_ModRandomVersion < 1 || g_ModRandomVersion > MODRANDOM_VERSION) {
#ifndef PLATFORM_N64
		if (g_ModRandomVersion > MODRANDOM_VERSION) {
			sysLogPrintf(1, "randomizer: seed %u wants generator v%d and this build has v%d; "
					"dealing it with v%d", g_ModRandomSeed, g_ModRandomVersion,
					MODRANDOM_VERSION, MODRANDOM_VERSION);
		}
#endif
		g_ModRandomVersion = MODRANDOM_VERSION;
	}

	// The run's number: the seed, the stage so that one run's Villa and its
	// Chicago are different missions, and the difficulty so that Perfect Agent
	// is its own deal rather than the Agent one with more guards. Every stream
	// hangs off this and nothing else.
	g_ModRandomRun = modRandomMix(g_ModRandomSeed)
		+ modRandomMix(stagenum * 0x9e3779b9u + lvGetDifficulty());

	// And the hop's number on top of it during a run, so that a run landing on
	// the same map twice is not the same map twice.
	if (modRunIsOn()) {
		g_ModRandomRun += modRandomMix(modRunGetRooms() * 0x85ebca6bu);
	}

	modRandomGather(&lists);
#ifndef PLATFORM_N64
	if (modRunIsArenaMission()) {
		// A Combat Simulator map is a mission here, not a match: its weapon
		// spawns and ammo boxes are left out (by excluding them on every
		// difficulty, as the setup walk does), so what the player has is what
		// the gauntlet gave them and what the guards drop.
		struct defaultobj *obj = (struct defaultobj *)g_StageSetup.props;

		while (obj->type != OBJTYPE_END) {
			if ((obj->type == OBJTYPE_WEAPON || obj->type == OBJTYPE_AMMOCRATE || obj->type == OBJTYPE_MULTIAMMOCRATE)
					&& (obj->flags & OBJFLAG_ASSIGNEDTOCHR) == 0) {
				obj->flags2 |= OBJFLAG2_EXCLUDE_A | OBJFLAG2_EXCLUDE_SA | OBJFLAG2_EXCLUDE_PA | OBJFLAG2_EXCLUDE_PD;
			}

			obj = (struct defaultobj *)((u32 *)obj + setupGetCmdLength((u32 *)obj));
		}
	}
#endif
	bool hasconfig = false;
#ifndef PLATFORM_N64
	hasconfig = modSpawnConfigPrepare(stagenum);
#endif
	if (lists.numweapons == 0 && lists.numchrs == 0 && !hasconfig) {
		return;
	}

	reached = modRandomAlloc(g_Vars.roomcount, sizeof(u8));

	if (reached == NULL) {
		return;
	}

	// The locked edges are a property of the stage's doors, not of the roll,
	// so the graph is built once and every walk below borrows it.
	lists.portalkeys = modRandomBuildPortalKeys(&lists, &lists.numportals);

	if (lists.portalkeys == NULL) {
		// A stage with no portals, or the pool is out. There is then nothing
		// to say a room cannot be got to, and the walk exists only to say
		// that, so everything counts as reachable and the roll goes on.
		s32 i;

		for (i = 0; i < g_Vars.roomcount; i++) {
			reached[i] = 1;
		}
	}

	// What is standing about, and what is in the player's hands, first:
	// neither depends on where anything is.
	modRandomRollWeapons(&lists);
	modRandomRollCrates(&lists);
	modRandomRollIntroWeapons();

	// Then where the guards are, because the start is picked from their pads;
	// then the start, because the walk is measured from it; then the keys,
	// because what they open decides what the walk reaches; and only then the
	// objectives, which may name nothing the walk did not reach.
	modRandomRollChrs(&lists);
#ifndef PLATFORM_N64
	modRandomApplyConfigGuards(&lists);
#endif

	if (lists.portalkeys) {
		spawnpad = modRandomRollSpawn(&lists, reached);
		spawnroom = modRandomPadRoom(spawnpad);
	} else {
		spawnpad = modRandomWriteSpawn(-1);
		spawnroom = modRandomPadRoom(spawnpad);
	}
#ifndef PLATFORM_N64
	spawnpad = modRandomApplyConfigStart(&lists, reached, spawnpad);
	spawnroom = modRandomPadRoom(spawnpad);
#endif

	modRandomRemoveSpawnGuard(&lists, spawnpad);
	if (modRandomGetVersion() >= 8) {
		modRandomPrepareKillTargets(&lists, spawnpad);
	}

	if (lists.portalkeys) {
		modRandomRollKeys(&lists, spawnroom, reached);
	}

	// A run's objectives are its room's and its start is its landing, both
	// dealt by modrun.c: the roll above still moves the guns, the crates, the
	// guards and the keys, which is everything a landing wants from it.
	if (!modRunIsOn()) {
		modRandomGenerateObjectives(&lists, reached);
	}
	modRandomChooseEventAtRoll();

	// Nothing to move to if the roll kept the mission's own start.
	g_ModRandomSpawnPad = spawnpad;
	g_ModRandomSpawnPending = spawnpad >= 0 && !modRunIsOn();

	// What an endless run needs for the rest of the level: the lists, the
	// walk, and a room per bit of score.
	g_ModRandomLists = lists;
	if (modRandomSequential()) {
		g_ModRandomGuardIds = modRandomAlloc(lists.numchrs, sizeof(s32));
		g_ModRandomGuardPending = modRandomAlloc(lists.numchrs, sizeof(u8));
		if (g_ModRandomGuardPending) memset(g_ModRandomGuardPending, 0, lists.numchrs);
	}
	g_ModRandomReached = reached;
	g_ModRandomVisited = modRandomAlloc(g_Vars.roomcount, sizeof(u8));

	if (g_ModRandomVisited) {
		s32 i;

		for (i = 0; i < g_Vars.roomcount; i++) {
			g_ModRandomVisited[i] = 0;
		}
	}

#ifndef PLATFORM_N64
	{
		u32 wf = 0;
		u32 cf = 0;
		u32 kf = 0;
		u32 of = 0;
		s32 i;

		for (i = 0; i < lists.numweapons; i++) {
			wf = modRandomFold(wf, lists.weapons[i]->weaponnum);
		}

		for (i = 0; i < lists.numchrs; i++) {
			cf = modRandomFold(cf, lists.chrs[i]->padnum);
		}

		for (i = 0; i < lists.numkeys; i++) {
			kf = modRandomFold(kf, lists.keys[i]->base.pad);
		}

		{
			// What each objective asks for: the requirement's type and the tag
			// or room it names, which is the objective. Folding the objective
			// struct instead would fold the same four words every time.
			for (i = 0; i < g_ModRandomNumObjectives; i++) {
				u32 *cmd = (u32 *)modRandomObjectiveAt(i);

				if (cmd == NULL) {
					break;
				}

				cmd += setupGetCmdLength(cmd); // past the begin

				while ((u8)PD_BE32(cmd[0]) != OBJTYPE_ENDOBJECTIVE) {
					of = modRandomFold(of, PD_BE32(cmd[0]));
					of = modRandomFold(of, cmd[1]);
					cmd += setupGetCmdLength(cmd);
				}
			}
		}

		sysLogPrintf(0, "randomizer: fold weapons %08x guards %08x keys %08x spawn %08x objectives %08x",
				wf, cf, kf, (u32)spawnpad, of);
	}

	{
		s32 i;

		for (i = 0; i < g_ModRandomNumObjectives; i++) {
			sysLogPrintf(0, "randomizer: objective %d - %s", i, g_ModRandomObjText[i]);
		}
	}

	sysLogPrintf(0, "randomizer: stage 0x%02x seed %u v%d - %d weapons, %d guards, %d keys, "
			"spawn pad %d room %d, %d/%d rooms reachable, %d objectives",
			stagenum, g_ModRandomSeed, g_ModRandomVersion,
			lists.numweapons, lists.numchrs, lists.numkeys,
			spawnpad, spawnroom, modRandomCountReached(reached), g_Vars.roomcount,
			g_ModRandomNumObjectives);
#endif
}

static void modRandomTickEndless(void);

#define MODRANDOM_ALLY_HEALTH_MULTIPLIER 5.0f // Allies take 20% of normal damage relative to their health.

/** Spawn the rolled buddies around the player after the randomized start. */
static s32 modRandomSpawnAlliesAt(s32 count, bool atplayer, struct coord *where, RoomNum room)
{
	struct player *player = g_Vars.bond;
	struct coord pos;
	RoomNum rooms[8];
	f32 angle;
	s32 spawned = 0;
	s32 i;

	if (player == NULL || player->prop == NULL || player->prop->chr == NULL) {
		return 0;
	}

	pos = atplayer ? player->prop->pos : *where;
	rooms[0] = atplayer ? player->prop->rooms[0] : room;
	rooms[1] = -1;
	angle = atan2f(-player->bond2.unk00.x, player->bond2.unk00.z);

	for (i = 0; i < count; i++) {
		const struct modrandomprofile *profile = modRandomGetVersion() >= 11
			? &g_ModRandomVelvetProfile
			: &g_ModRandomFemaleProfiles[g_ModRandomAllyProfiles[i]];
		s32 ailistnum = modRandomGetVersion() >= 12
			? GAILIST_INIT_DEFAULT_BUDDY
			: GAILIST_BUDDY_MAIN;
		s32 allybody = modRandomGetVersion() >= 17 ? modRandomAllyBody() : profile->bodynum;
		s32 allyhead = modRandomGetVersion() >= 17 ? modRandomAllyHead() : profile->headnum;
		struct prop *prop = chrSpawnAtCoord(allybody, allyhead, &pos, rooms, angle,
				ailistFindById(ailistnum), SPAWNFLAG_ALLOWONSCREEN);

		if (prop == NULL || prop->chr == NULL) {
			continue;
		}

		prop->chr->team = TEAM_ALLY;
		prop->chr->voicebox = modRandomVoiceIsFemale(allybody, allyhead)
			? VOICEBOX_FEMALE : prop->chr->chrnum % 3;
		if (modRandomGetVersion() >= 12) {
			prop->chr->maxdamage *= MODRANDOM_ALLY_HEALTH_MULTIPLIER;
		}
		modRandomGiveAllyGun(prop->chr);
		modRandomSpawnFadeIn(prop->chr);
		spawned++;
	}

	if (spawned > 0) {
		rebuildTeams();
		rebuildSquadrons();
	}
	return spawned;
}

static s32 modRandomSpawnAllies(s32 count)
{
	return modRandomSpawnAlliesAt(count, true, NULL, 0);
}

// Round NPCs need a walkable floor. Keep the player's start clear.
static struct prop *modRandomSpawnRoundNpc(s32 padnum, s32 body, s32 head, u8 team)
{
	struct pad pad;
	struct coord pos;
	RoomNum rooms[2];
	u16 flags;
	f32 ground, dx, dz;
	struct prop *prop;
	s32 i;
	if (padnum == g_ModRandomSpawnPad || !modRandomPadGround(padnum, &pad, &ground, &flags)
			|| (flags & GEOFLAG_DIE) || pad.pos.y < ground - 5 || pad.pos.y > ground + 120) return NULL;
	pos = pad.pos;
	pos.y = ground + MODRANDOM_SETTLEHEIGHT;
	// Do not materialize inside the player or another living character.
	for (i = 0; i < chrsGetNumSlots(); i++) {
		struct chrdata *chr = &g_ChrSlots[i];
		if (chr->chrnum < 0 || !chr->prop || !chr->model || chrIsDead(chr)
				|| (chr->chrflags & CHRCFLAG_HIDDEN)) continue;
		dx = chr->prop->pos.x - pos.x;
		dz = chr->prop->pos.z - pos.z;
		if (fabsf(chr->prop->pos.y - pos.y) < 160
				&& dx * dx + dz * dz < (chr->prop->type == PROPTYPE_PLAYER ? 40000 : 6400)) return NULL;
	}
	rooms[0] = pad.room;
	rooms[1] = -1;
	prop = chrSpawnAtCoord(body, head, &pos, rooms, atan2f(pad.look.x, pad.look.z),
			ailistFindById(GAILIST_UNALERTED), SPAWNFLAG_ALLOWONSCREEN);
	if (prop && prop->chr) {
		prop->chr->team = team;
		prop->chr->voicebox = modRandomVoiceIsFemale(body, head)
			? VOICEBOX_FEMALE : prop->chr->chrnum % 3;
		prop->chr->flags |= CHRFLAG0_AIVSAI;
		modRandomSpawnFadeIn(prop->chr);
	}
	return prop;
}

#define MODRANDOM_TARGET_SOLIDS (CDTYPE_BG | CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_PATHBLOCKER)

static bool modRandomTargetHasClearance(struct prop *prop, f32 ground)
{
	const struct chrdata *chr = prop->chr;
	return cdTestVolume(&prop->pos, chr->radius, prop->rooms, MODRANDOM_TARGET_SOLIDS,
			CHECKVERTICAL_YES, ground + chr->height - prop->pos.y, ground + 20.0f - prop->pos.y)
		!= CDRESULT_COLLISION;
}

// Spawn adjustment may move a chr away from the checked pad, including over an
// edge. Validate the actual position and floor before announcing the target.
static bool modRandomTargetSpawnIsSafe(struct prop *prop, s32 padnum)
{
	struct pad pad;
	struct coord query;
	RoomNum floorroom = -1;
	u16 flags = 0;
	f32 floor, dx, dz;
	if (!prop || !prop->chr || !prop->chr->model || !g_ModRandomReached) return false;
	if (padnum < 0 || !g_PadsFile || padnum >= g_PadsFile->numpads) return false;
	padUnpack(padnum, PADFIELD_POS | PADFIELD_ROOM, &pad);
	dx = prop->pos.x - pad.pos.x;
	dz = prop->pos.z - pad.pos.z;
	if (dx * dx + dz * dz > 25.0f * 25.0f) return false;
	query = prop->pos;
	query.y += 20.0f;
	floor = cdFindGroundInfoAtCyl(&query, 20, prop->rooms, NULL, NULL, &flags, &floorroom, NULL, NULL);
	if (floorroom <= 0 || floorroom >= g_Vars.roomcount || !g_ModRandomReached[floorroom]
			|| floor < MODRANDOM_NOGROUND || (flags & GEOFLAG_DIE)
			|| query.y - floor > 140.0f || floor > query.y + 120.0f) return false;
	if (!modRandomTargetHasClearance(prop, floor)) return false;
	// chrSpawnAtCoord can leave the model slightly below a nearby floor when
	// a stage's collision seam is corrected. Put it on the validated floor
	// before the target is announced, instead of rejecting the Cassandra.
	if (floor > prop->pos.y) {
		struct coord settled = prop->pos;
		settled.y = floor + MODRANDOM_SETTLEHEIGHT;
		chrMoveToCheckedPos(prop->chr, &settled, prop->rooms, chrGetInverseTheta(prop->chr));
	}
#ifndef PLATFORM_N64
	if (geRoomActive()) {
		struct coord checked;
		s32 legal = geStanTeleportSpot(&prop->pos, floorroom, 0.0f, 20.0f, prop, &checked);
		// The spawn helper can legally nudge a character a few units around a
		// GoldenEye collision seam. The old exact-coordinate requirement treated
		// that correction as an invalid/out-of-bounds spawn and discarded every
		// Cassandra candidate on affected maps. The actual position has already
		// passed the floor/room/reachability checks above, so only reject when
		// the nav query says there is no legal position at all.
		if (legal == 0) return false;
	}
#endif
	return true;
}

static struct modrandomtarget *modRandomGetRescueTarget(void)
{
	s32 chrnum;
	s32 i;

	if (!modRandomIsOn() || g_ModRandomRunOver || g_ModRandomAlienWarActive) {
		return NULL;
	}

	chrnum = modRandomGetObjectiveTarget();
	for (i = 0; chrnum >= 0 && i < g_ModRandomNumTargets; i++) {
		struct modrandomtarget *target = &g_ModRandomTargets[i];

		if (target->chrnum == chrnum && !target->complete && !target->killedbyplayer) {
			return target;
		}
	}

	return NULL;
}

bool modRandomCanRelocateTarget(void)
{
	struct modrandomtarget *target = modRandomGetRescueTarget();

	return target && target->relocateAt60 > 0
		&& g_Vars.lvframe60 >= target->relocateAt60
		&& !target->relocated && !target->relocateRequested;
}

void modRandomRequestTargetRelocation(void)
{
	if (modRandomCanRelocateTarget()) {
		modRandomGetRescueTarget()->relocateRequested = true;
	}
}

// Keep a live objective from being deleted by a pit or a kill floor.
bool modRandomDeferTargetFall(struct chrdata *chr)
{
	struct modrandomtarget *target = modRandomGetRescueTarget();

	if (!target || target->chrnum != chr->chrnum || chrIsDead(chr)
			|| (target->pacify && modRandomChrKnockedOut(chr))) {
		return false;
	}

	target->needsRescue = true;
	return true;
}

static bool modRandomRelocateTarget(struct chrdata *chr, struct player *player)
{
	const u32 solid = MODRANDOM_TARGET_SOLIDS;
	const f32 radius = chr->radius > 25.0f ? chr->radius : 25.0f;
	const f32 height = chr->height > 160.0f ? chr->height : 160.0f;
	const f32 behind = atan2f(-player->bond2.unk00.x, -player->bond2.unk00.z);
	s32 pass;
	s32 direction;

	// Follow short walkable paths from the player. Prefer spots behind their
	// view, then try closer spots or other directions if the area is cramped.
	for (pass = 0; pass < 3; pass++) {
		for (direction = 0; direction < 12; direction++) {
			s32 offset = (direction + 1) / 2 * (direction % 2 ? 1 : -1);
			f32 angle = behind + offset * M_BADTAU / 12;
			f32 dx = sinf(angle) * 30.0f;
			f32 dz = cosf(angle) * 30.0f;
			s32 steps = pass == 0 ? 10 : pass == 1 ? 6 : 3;
			struct coord from = player->prop->pos;
			RoomNum rooms[8];
			s32 step;

			if (pass == 0 && abs(offset) > 2) continue;

			from.y = player->vv_manground + 10.0f;
			roomsCopy(player->prop->rooms, rooms);

			for (step = 0; step < steps; step++) {
				struct coord to = from;
				RoomNum nextrooms[8] = { -1 };
				RoomNum floorroom = -1;
				u16 flags = 0;
				f32 ground;

				to.x += dx;
				to.y += 30.0f;
				to.z += dz;
				if (!cdTestLos11(&from, rooms, &to, nextrooms, solid) || nextrooms[0] <= 0) break;

				ground = cdFindGroundInfoAtCyl(&to, radius, nextrooms, NULL, NULL,
						&flags, &floorroom, NULL, NULL);
				if (floorroom <= 0 || (flags & GEOFLAG_DIE)
						|| fabsf(ground + 10.0f - from.y) > 25.0f) break;

				to.y = ground + 10.0f;
				if (cdExamCylMove01(&from, &to, radius, nextrooms, solid,
							CHECKVERTICAL_YES, height, 10.0f) != CDRESULT_NOCOLLISION
						|| cdTestVolume(&to, radius, nextrooms, solid,
							CHECKVERTICAL_YES, height, 10.0f) == CDRESULT_COLLISION) break;

				from = to;
				roomsCopy(nextrooms, rooms);
			}

			if (step != steps || cdTestVolume(&from, radius, rooms, CDTYPE_ALL,
						CHECKVERTICAL_YES, height, 10.0f) == CDRESULT_COLLISION) continue;

			chrMoveToCheckedPos(chr, &from, rooms, chrGetInverseTheta(chr));
			chr->fallspeed.x = chr->fallspeed.y = chr->fallspeed.z = 0;
			chr->fallpeakset = 0;
			return true;
		}
	}

	return false;
}

static void modRandomRestoreTargetVisibility(struct chrdata *chr)
{
	// An active objective must stay visible and damageable, even if a stage
	// script changes the state of the original guard's character ID.
	chr->chrflags &= ~(CHRCFLAG_HIDDEN | CHRCFLAG_INVINCIBLE);
	chr->hidden &= ~(CHRHFLAG_UNTARGETABLE | CHRHFLAG_CLOAKED);
	chr->hidden2 &= ~CHRH2FLAG_ASSASSINCLOAK;
	chr->cloakfadefrac = 0;
	chr->cloakfadefinished = false;
	if (!(chr->prop->flags & PROPFLAG_ENABLED)) propEnable(chr->prop);
}

static void modRandomTickTargetRescue(void)
{
	struct player *player = g_Vars.bond;
	struct modrandomtarget *target;
	struct chrdata *chr;
	s32 now = g_Vars.lvframe60;

	if (!player || !player->prop || g_Vars.currentplayer != player || player->isdead
			|| player->mpmenuon || player->dostartnewlife || g_ModRandomSpawnPending
			|| g_InCutscene || lvIsPaused()) return;

	target = modRandomGetRescueTarget();
	if (!target) return;
	chr = chrFindByLiteralId(target->chrnum);
	if (!chr || !chr->prop || chr->prop->type != PROPTYPE_CHR || !chr->model || chrIsDead(chr)
			|| (target->pacify && modRandomChrKnockedOut(chr))) return;

	modRandomRestoreTargetVisibility(chr);
	if (!target->relocateAt60) target->relocateAt60 = now + TICKS(30 * 60);
	if (!target->relocateRequested && now < target->rescueCheckAt60) return;
	target->rescueCheckAt60 = now + TICKS(60);

	if (!target->needsRescue) {
		struct prop *prop = chr->prop;
		if (prop->rooms[0] <= 0 || (chr->fallspeed.y == 0.0f
				&& !modRandomTargetHasClearance(prop, chr->manground))) {
			if (!target->blockedSince60) target->blockedSince60 = now;
			target->needsRescue = now - target->blockedSince60 >= TICKS(120);
		} else {
			target->blockedSince60 = 0;
		}
	}

	if (!target->needsRescue && !target->relocateRequested) return;

	if (modRandomRelocateTarget(chr, player)) {
		target->relocated |= target->relocateRequested;
		target->needsRescue = false;
		target->blockedSince60 = 0;
		modRandomUseTargetLocator();
		hudmsgCreateWithFlags("Objective target relocated.\n", HUDMSGTYPE_EVENT,
				HUDMSGFLAG_OBJECTIVE | HUDMSGFLAG_ALLOWDUPES);
	} else if (target->relocateRequested) {
		hudmsgCreateWithFlags("No safe spot nearby. Move and try again.\n", HUDMSGTYPE_EVENT,
				HUDMSGFLAG_OBJECTIVE | HUDMSGFLAG_ALLOWDUPES);
	}

	target->relocateRequested = false;
}

bool modRandomAssassinCloakActive(void)
{
	return g_ModRandomAssassinsUntil > 0 && g_Vars.lvframe60 < g_ModRandomAssassinsUntil;
}

/**
 * Every NPC that spawns in after the level has started arrives the way
 * reinforcements do: fully cloaked, then the game's own decloak transition
 * (with its sound). Skipped for the level's first two seconds so the
 * opening population does not all shimmer at once, and for assassin-cloaked
 * hunters, who stay cloaked on purpose.
 */
void modRandomSpawnFadeIn(struct chrdata *chr)
{
	if (!chr || !chr->prop || !chr->model || g_Vars.lvframe60 < TICKS(120)
			|| (chr->hidden2 & CHRH2FLAG_ASSASSINCLOAK)) return;

	chrCloak(chr, false);
	chr->cloakfadefinished = true;
	chr->cloakfadefrac = 0;
	chrUncloak(chr, true);
}

void modRandomCloakHunter(struct chrdata *chr)
{
	if (!modRandomAssassinCloakActive()) return;

	chr->hidden2 |= CHRH2FLAG_ASSASSINCLOAK;
	chrCloak(chr, false);
	chr->ailist = ailistFindById(GAILIST_SEARCH_FOR_PLAYER);
	chr->aioffset = 0;
	chr->alertness = 100;
}

// (defined with the event state further down)
static bool g_ModRandomSwarmOn;
static s32 g_ModRandomInvaderKills;
static s32 g_ModRandomSwarmKills;

// Which chrs are Maian invaders. Not a chr->hidden2 bit: its top three bits
// (13-15) are the flinch type the game rolls for every chr, so a flag there
// was set on about half of all guards.
#define MODRANDOM_MAXINVADERS 128
static s16 g_ModRandomInvaders[MODRANDOM_MAXINVADERS];
static s32 g_ModRandomNumInvaders;

static bool modRandomIsInvader(struct chrdata *chr)
{
	s32 i;
	for (i = 0; i < g_ModRandomNumInvaders && i < MODRANDOM_MAXINVADERS; i++) {
		if (g_ModRandomInvaders[i] == chr->chrnum) return true;
	}
	return false;
}

void modRandomOnChrDeath(struct chrdata *chr, s32 playernum)
{
	s32 i;
	if (!modRandomSequential() || g_ModRandomRunOver || !chr) return;
	if (playernum == g_Vars.bondplayernum && (chr->hidden2 & CHRH2FLAG_ASSASSINCLOAK)
			&& (chr->hidden & CHRHFLAG_CLOAKED)) {
		modCareerCloakReward();
	}
	if (playernum == g_Vars.bondplayernum) {
		if (chr->hidden2 & CHRH2FLAG_BLONDREVENGE) {
			modCareerBlondReward(); // the Devastator, next map
		} else if (modRandomIsInvader(chr)) {
			if (++g_ModRandomInvaderKills == 6) modCareerAlienWarReward(false); // 6 Maians: the Skedar swarm
		} else if (!g_ModRandomAlienWarActive && chr->bodynum == BODY_SKEDAR && chr->team == MODEVENT_TEAM_MAIAN
				&& g_ModRandomSwarmOn) {
			if (++g_ModRandomSwarmKills == 6) modCareerAlienWarReward(true); // 6 Skedar: Maian backup
		}
	}
	if (g_ModRandomAlienWarActive && g_ModRandomAlienFaction == 0
			&& chr->team == MODEVENT_TEAM_MAIAN && playernum == g_Vars.bondplayernum) {
		s32 j;
		g_ModRandomAlienFaction = 2; // killing Maians sides the player with the Skedar
		g_ModRandomAlienNextBoss60 = g_Vars.lvframe60 + TICKS(15 * 60);
		hudmsgCreateWithFlags("You join the Skedar!\n", HUDMSGTYPE_EVENT,
				HUDMSGFLAG_DELAY | HUDMSGFLAG_ALLOWDUPES);
		if (g_Vars.bond && g_Vars.bond->prop && g_Vars.bond->prop->chr) {
			g_Vars.bond->prop->chr->team = (g_ModRandomAlienPlayerTeam & ~(MODEVENT_TEAM_MAIAN | MODEVENT_TEAM_TERROR)) | TEAM_ALLY | MODEVENT_TEAM_TERROR;
		}
		for (j = 0; j < chrsGetNumSlots(); j++) if (g_ChrSlots[j].chrnum >= 0 && g_ChrSlots[j].team == MODEVENT_TEAM_MAIAN && !chrIsDead(&g_ChrSlots[j])) {
			g_ChrSlots[j].target = g_Vars.bond->prop - g_Vars.props; // (an index into g_Vars.props, not a chrnum) g_ChrSlots[j].alertness = 100;
			g_ChrSlots[j].ailist = ailistFindById(GAILIST_SEARCH_FOR_PLAYER); g_ChrSlots[j].aioffset = 0;
		}
	} else if (g_ModRandomAlienWarActive && g_ModRandomAlienFaction == 0
			&& chr->team == MODEVENT_TEAM_TERROR && playernum == g_Vars.bondplayernum) {
		s32 j;
		g_ModRandomAlienFaction = 1; // killing Skedar sides the player with the Maians
		g_ModRandomAlienNextBoss60 = g_Vars.lvframe60 + TICKS(15 * 60);
		hudmsgCreateWithFlags("You join the Maians!\n", HUDMSGTYPE_EVENT,
				HUDMSGFLAG_DELAY | HUDMSGFLAG_ALLOWDUPES);
		if (g_Vars.bond && g_Vars.bond->prop && g_Vars.bond->prop->chr) {
			g_Vars.bond->prop->chr->team = (g_ModRandomAlienPlayerTeam & ~(MODEVENT_TEAM_MAIAN | MODEVENT_TEAM_TERROR)) | TEAM_ALLY | MODEVENT_TEAM_MAIAN;
		}
		for (j = 0; j < chrsGetNumSlots(); j++) if (g_ChrSlots[j].chrnum >= 0 && g_ChrSlots[j].team == MODEVENT_TEAM_TERROR && !chrIsDead(&g_ChrSlots[j])) {
			g_ChrSlots[j].target = g_Vars.bond->prop - g_Vars.props; // (an index into g_Vars.props, not a chrnum) g_ChrSlots[j].alertness = 100;
			g_ChrSlots[j].ailist = ailistFindById(GAILIST_SEARCH_FOR_PLAYER); g_ChrSlots[j].aioffset = 0;
		}
	}
	if (g_ModRandomAlienWarActive && (chr->chrnum == g_ModRandomAlienKing || chr->chrnum == g_ModRandomAlienElvis)) {
		if (playernum == g_Vars.bondplayernum) {
			const f32 frac = playerGetShieldFrac() + 0.5f;
			playerSetShieldFrac(frac > 1.0f ? 1.0f : frac);
			hudmsgCreateWithFlags("Alien boss killed: +50% shield\n", HUDMSGTYPE_DEFAULT,
					HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
		}
		modRandomAlienWarFinish(chr->chrnum == g_ModRandomAlienKing ? 2 : 1, playernum);
	}
	if (g_ModRandomExplosiveOn && g_Vars.lvframe60 >= (u32)g_ModRandomExplosiveStart60
			&& chr->prop && chr->race == RACE_HUMAN && chr->prop->type == PROPTYPE_CHR
			&& !(chr->team & (TEAM_ALLY | TEAM_NONCOMBAT)) && !modRandomIsCivilianChr(chr)) {
		// Explosive exit: a second after it dies, it goes up
		for (i = 0; i < MODRANDOM_MAX_BLASTS; i++) {
			if (!g_ModRandomBlasts[i].used) {
				s32 r;

				g_ModRandomBlasts[i].used = true;
				g_ModRandomBlasts[i].pos = chr->prop->pos;
				g_ModRandomBlasts[i].pos.y += 40.0f;

				for (r = 0; r < 7 && chr->prop->rooms[r] != -1; r++) {
					g_ModRandomBlasts[i].rooms[r] = chr->prop->rooms[r];
				}

				g_ModRandomBlasts[i].rooms[r] = -1;
				g_ModRandomBlasts[i].at60 = g_Vars.lvframe60 + TICKS(60);
				g_ModRandomBlasts[i].chrnum = chr->chrnum;
				break;
			}
		}
	}
	if (playernum >= 0) {
		// a real kill by the player fails a pacify target even after she has
		// been disarmed and has surrendered (the objective was already done)
		for (i = 0; i < g_ModRandomNumTargets; i++) {
			if (g_ModRandomTargets[i].pacify && g_ModRandomTargets[i].chrnum == chr->chrnum) {
				g_ModRandomTargets[i].killedbyplayer = true;
			}
		}
	}
	for (i = 0; i < MODRANDOM_MAX_CIVILIANS; i++) {
		struct modrandomcivilian *civilian = &g_ModRandomCivilians[i];
		if (civilian->chrnum < 0 || civilian->chrnum != chr->chrnum || civilian->dead) continue;
		civilian->dead = true; // latch before any further damage/death callbacks

		if (!g_ModRandomPurgeOn && playernum >= 0 && playernum < PLAYERCOUNT()
				&& !chrGetHeldProp(chr, HAND_RIGHT) && !chrGetHeldProp(chr, HAND_LEFT)) {
			struct player *killer = g_Vars.players[playernum];
			if (killer && killer->prop && killer->prop->chr) {
				const s32 previousPlayer = g_Vars.currentplayernum;
				setCurrentPlayerNum(playernum);
				playerSetShieldFrac(playerGetShieldFrac() + 0.20f * modCareerCivShieldScale()); // +50% with the permanent boon
				setCurrentPlayerNum(previousPlayer);
			}
		}

		if (g_ModRandomPurgeOn) {
			// the purge: no punishment guards while it lasts; a kill of the
			// player's own is worth half a shield
			if (playernum == g_Vars.bondplayernum) {
				const f32 gain = 0.5f * modCareerCivShieldScale(); // +50% with the permanent boon
				const f32 frac = playerGetShieldFrac() + gain;
				char text[40];

				playerSetShieldFrac(frac > 1.0f ? 1.0f : frac);
				snprintf(text, sizeof(text), "Purge: +%d%% shield\n", (s32)(gain * 100.0f + 0.5f));
				hudmsgCreateWithFlags(text, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
			}
		} else if (playernum == g_Vars.bondplayernum) {
			// Give the player's civilian kill an in-world voice reaction as well
			// as the punishment wave.
			if (g_Vars.bond && g_Vars.bond->prop) {
				psCreate(NULL, g_Vars.bond->prop, MP3_M1_BE_CAREFUL, -1, -1,
					0, 0, PSTYPE_CHRTALK, NULL, -1, NULL, -1, -1, -1, -1);
			}
			// the big banner, in the event colour
			hudmsgCreateWithFlags("Punishment inbound\n", HUDMSGTYPE_EVENT,
					HUDMSGFLAG_DELAY | HUDMSGFLAG_ALLOWDUPES);
			modAlarmStartCivilianPunishment();
		}
		return;
	}
}

/** Send a civilian to a distant escape pad, preferring the user's configured
 * Escape spawns and falling back to distant, reachable map waypoints. */
static bool modRandomCivilianTryEscapePads(struct chrdata *chr,
		struct modrandomcivilian *civilian, bool configured, f32 minimum)
{
	struct waypoint *waypoints = g_StageSetup.waypoints;
	const s32 count = configured ? modSpawnConfigCount(MODSPAWN_ESCAPE) : g_ModRandomCivilianWaypoints;
	s32 attempt;

	if (count <= 0) {
		return false;
	}

	for (attempt = 0; attempt < 48; attempt++) {
		s32 padnum;
		struct pad pad;
		s32 room;
		f32 dx, dz;

		if (configured) {
			padnum = modSpawnConfigPad(MODSPAWN_ESCAPE, rngRandom() % count);
		} else {
			s32 w = rngRandom() % count;

			if (!waypoints[w].neighbours || waypoints[w].neighbours[0] < 0) {
				continue;
			}

			padnum = waypoints[w].padnum;
		}

		if (padnum < 0 || padnum == civilian->destinationPad || !modRandomPadCanSpawn(padnum)) {
			continue;
		}

		room = modRandomPadRoom(padnum);
		if (room <= 0 || room >= g_Vars.roomcount || (g_ModRandomReached && !g_ModRandomReached[room])) {
			continue;
		}

		padUnpack(padnum, PADFIELD_POS, &pad);
		dx = pad.pos.x - chr->prop->pos.x;
		dz = pad.pos.z - chr->prop->pos.z;
		if (dx * dx + dz * dz < minimum * minimum) {
			continue;
		}

		// The first two passes (minimum > 0) keep civilians from running at the
		// player: the pad must not be nearer the player than the civilian is,
		// nor lie in the player's direction (within about 70 degrees)
		if (minimum > 0.0f && g_Vars.bond && g_Vars.bond->prop) {
			const f32 px = g_Vars.bond->prop->pos.x - chr->prop->pos.x;
			const f32 pz = g_Vars.bond->prop->pos.z - chr->prop->pos.z;
			const f32 qx = pad.pos.x - g_Vars.bond->prop->pos.x;
			const f32 qz = pad.pos.z - g_Vars.bond->prop->pos.z;
			const f32 pl = sqrtf(px * px + pz * pz);
			const f32 dl = sqrtf(dx * dx + dz * dz);

			if (qx * qx + qz * qz < px * px + pz * pz) {
				continue;
			}

			if (pl > 1.0f && dl > 1.0f && (dx * px + dz * pz) / (pl * dl) > 0.35f) {
				continue;
			}
		}

		if (chrGoToPad(chr, padnum, GOPOSFLAG_RUN)) {
			civilian->destinationPad = padnum;
			return true;
		}
	}

	return false;
}

static void modRandomCivilianRunToEscape(struct chrdata *chr, struct modrandomcivilian *civilian)
{
	static const f32 distances[] = { 2500.0f, 1500.0f, 0.0f };
	s32 pass;

	for (pass = 0; pass < ARRAYCOUNT(distances); pass++) {
		if (modSpawnConfigCount(MODSPAWN_ESCAPE) > 0
				&& modRandomCivilianTryEscapePads(chr, civilian, true, distances[pass])) {
			return;
		}

		if (modRandomCivilianTryEscapePads(chr, civilian, false, distances[pass])) {
			return;
		}
	}
}

static void modRandomTickCivilians(void)
{
	struct waypoint *waypoints = g_StageSetup.waypoints;
	struct player *player = g_Vars.currentplayer;
	s32 i;
	if (!modRandomSequential() || g_ModRandomRunOver || g_InCutscene || lvIsPaused()
			|| player != g_Vars.bond || !player || !player->prop || player->isdead
			|| player->dostartnewlife || g_ModRandomSpawnPending || !waypoints) return;
	if (g_ModRandomCivilianWaypoints < 0) {
		g_ModRandomCivilianWaypoints = 0;
		while (waypoints[g_ModRandomCivilianWaypoints].padnum >= 0) g_ModRandomCivilianWaypoints++;
	}
	if (!g_ModRandomCivilianWaypoints) return;
#ifndef PLATFORM_N64
	if (sysArgCheck("--random-civilian-test")) player->prop->chr->chrflags |= CHRCFLAG_INVINCIBLE;
#endif
	for (i = 0; i < MODRANDOM_BASE_CIVILIANS; i++) {
		struct modrandomcivilian *civilian = &g_ModRandomCivilians[i];
		struct chrdata *chr;
		f32 dx, dy, dz;
		if (civilian->dead) continue;
		if (g_ModRandomClimaxStarted || civilian->armed) continue; // armed, and theirs to fight with
		if (civilian->chrnum < 0) {
			s32 attempt;
			if (g_Vars.lvframe60 < g_ModRandomCivilianSpawn60) continue;
			for (attempt = 0; attempt < 32; attempt++) {
				s32 w = rngRandom() % g_ModRandomCivilianWaypoints;
				s32 padnum = waypoints[w].padnum;
				bool onconfig = false;
				s32 room;
				bool secretary = (i & 1) != 0;
				struct prop *prop;
#ifndef PLATFORM_N64
				// a Spawn Customizer config: civilians stand where its Guard spawns are
				if (modSpawnConfigCount(MODSPAWN_GUARD) > 0) {
					padnum = modSpawnConfigPad(MODSPAWN_GUARD, rngRandom() % modSpawnConfigCount(MODSPAWN_GUARD));
					onconfig = true;
					w = -1;
				}
#endif
				room = modRandomPadRoom(padnum);
				if ((!onconfig && (!waypoints[w].neighbours || waypoints[w].neighbours[0] < 0)) || room <= 0
						|| room >= g_Vars.roomcount || (g_ModRandomReached && !g_ModRandomReached[room])) continue;
				prop = modRandomSpawnRoundNpc(padnum,
					modRandomExtraBody(0, secretary ? BODY_SECRETARY : BODY_STEWARDESS),
					modRandomExtraHead(0, secretary ? HEAD_SECRETARY : HEAD_RANDOM), TEAM_NONCOMBAT);
				if (!prop || !prop->chr) continue;
				chr = prop->chr;
				chr->ailist = ailistFindById(GAILIST_IDLE);
				chr->aioffset = 0;
				chr->flags &= ~CHRFLAG0_AIVSAI;
				chr->chrflags |= CHRCFLAG_NEVERSLEEP;
				chr->target = -1;
				civilian->chrnum = chr->chrnum;
				civilian->destinationPad = padnum;
				modRandomDebugSpawn(chr, MODDBG_CIV);
#ifndef PLATFORM_N64
				sysLogPrintf(0, "civilian: spawned chr %d body %d at pad %d room %d", chr->chrnum, chr->bodynum, padnum, room);
#endif
				break;
			}
			// Spread initial arrivals and retry safely when a map is crowded.
			g_ModRandomCivilianSpawn60 = g_Vars.lvframe60 + TICKS(30);
		}
		chr = civilian->chrnum >= 0 ? chrFindByLiteralId(civilian->chrnum) : NULL;
#ifndef PLATFORM_N64
		// One opt-in smoke replay exercises a player kill and duplicate callbacks;
		// the normal damage/death hook must still enqueue exactly one full wave.
		if (i == 0 && sysArgCheck("--random-civilian-test") && chr && chr->prop && chr->model
				&& !chrIsDead(chr) && g_Vars.lvframe60 >= TICKS(360)) {
			struct coord dir = {0, 0, 1};
			struct gset gset = {0};
			gset.weaponnum = WEAPON_FALCON2;
			playerSetShieldFrac(0.4f);
			chrBeginDeath(chr, &dir, 0, HITPART_TORSO, &gset, false, g_Vars.bondplayernum);
			modRandomOnChrDeath(chr, g_Vars.bondplayernum);
			const f32 shield = playerGetShieldFrac();
			sysLogPrintf(0, "civilian-test: %s one unarmed civilian shield reward (%.3f)",
					shield > 0.599f && shield < 0.601f ? "PASS" : "FAIL", shield);
		}
#endif
		if (!chr || !chr->prop || !chr->model || chrIsDead(chr) || !chrIsReadyForOrders(chr)) continue;
		if (g_Vars.lvframe60 < civilian->nextthink60) continue;
		civilian->nextthink60 = g_Vars.lvframe60 + TICKS(30);
		dx = chr->prop->pos.x - player->prop->pos.x;
		dy = chr->prop->pos.y - player->prop->pos.y;
		dz = chr->prop->pos.z - player->prop->pos.z;
		if (dx * dx + dy * dy + dz * dz < 500.0f * 500.0f && chrHasLosToProp(chr, player->prop)) {
			if (chr->actiontype != ACT_SURRENDER) {
				chrSurrender(chr);
				psCreate(NULL, chr->prop, rngRandom() & 1 ? SFX_F_PLEASE_DONT : SFX_F_DONT_SHOOT,
					-1, -1, 0, 0, PSTYPE_CHRTALK, NULL, -1, NULL, -1, -1, -1, -1);
#ifndef PLATFORM_N64
				sysLogPrintf(0, "civilian: chr %d surrendered", chr->chrnum);
#endif
			}
			continue;
		}
		// Keep hands raised until the player is well clear, avoiding animation flicker.
		if (chr->actiontype == ACT_SURRENDER) {
			if (dx * dx + dy * dy + dz * dz < 800.0f * 800.0f) continue;
			chrStand(chr);
		}
		if (chr->actiontype != ACT_GOPOS) {
			modRandomCivilianRunToEscape(chr, civilian);
#ifndef PLATFORM_N64
			if (sysArgCheck("--random-civilian-test")) sysLogPrintf(0,
				"civilian: chr %d running to escape pad %d", chr->chrnum, civilian->destinationPad);
#endif
		}
	}
}

static void modRandomCaptureRoundGuards(void)
{
	s32 i, j;
	if (g_ModRandomGuardsCaptured || !g_ModRandomGuardIds || !g_ModRandomGuardPending) return;
	for (i = 0; i < g_ModRandomLists.numchrs; i++) {
		struct packedchr *row = g_ModRandomLists.chrs[i];
		struct chrdata *chr = chrFindByLiteralId(row->chrnum);
		g_ModRandomGuardIds[i] = -1;
		if (!chr || !chr->prop || !chr->model || row->chrnum < 0
				|| row->team == TEAM_00 || (row->team & (TEAM_01 | TEAM_ALLY | TEAM_NONCOMBAT))
				|| (row->spawnflags & (SPAWNFLAG_HIDDEN | SPAWNFLAG_INVINCIBLE))) continue;
		for (j = 0; j < g_ModRandomNumTargets; j++) {
			if (row->chrnum == g_ModRandomTargets[j].chrnum) break;
		}
		if (j == g_ModRandomNumTargets) g_ModRandomGuardIds[i] = row->chrnum;
	}
	g_ModRandomGuardsCaptured = true;
}

static void modRandomQueueGuardRespawns(void)
{
	s32 i;
	if (!g_ModRandomGuardsCaptured) return;
	// Give the player breathing room after the target kill before guards return.
	g_ModRandomGuardRespawn60 = g_Vars.lvframe60 + TICKS(300);
	for (i = 0; i < g_ModRandomLists.numchrs; i++) {
		struct chrdata *chr;
		if (g_ModRandomGuardIds[i] < 0) continue;
		chr = chrFindByLiteralId(g_ModRandomGuardIds[i]);
		if ((!chr || !chr->prop || chrIsDead(chr)) && !g_ModRandomGuardPending[i]
				&& (s32)(rngRandom() % 100) < (modCareerIsOn()
					? modCareerRespawnChance() : modGetRandomRespawnPercent())) {
			g_ModRandomGuardPending[i] = true; // each dead guard has its own chance (Guard Respawn Chance)
		}
	}
}

/** The pad of the [index]th place a returning guard may use with a config: its Guaranteed Guard spawns, then its Guard spawns. */
#ifndef PLATFORM_N64
static s32 modRandomReturnPad(s32 index)
{
	const s32 ng = modSpawnConfigCount(MODSPAWN_GUARANTEED);

	return index < ng ? modSpawnConfigPad(MODSPAWN_GUARANTEED, index) : modSpawnConfigPad(MODSPAWN_GUARD, index - ng);
}
#endif

/**
 * A returning guard: at its own site, or - with a Spawn Customizer config that
 * has Guaranteed Guard or Guard spawns - at the first free one of those, from
 * a start that rotates.
 */
static struct prop *modRandomSpawnReturningGuard(s32 sitepad, u8 team)
{
#ifndef PLATFORM_N64
	const s32 total = modSpawnConfigCount(MODSPAWN_GUARANTEED) + modSpawnConfigCount(MODSPAWN_GUARD);

	if (total > 0) {
		static s32 nextreturn;
		struct prop *prop = NULL;
		s32 k;

		for (k = 0; k < total && !prop; k++) {
			prop = modRandomSpawnRoundNpc(modRandomReturnPad((nextreturn + k) % total),
					modRandomGuardBody(3), modRandomGuardHead(3), team);
		}

		nextreturn = (nextreturn + k) % total;
		return prop;
	}
#endif

	return modRandomSpawnRoundNpc(sitepad, modRandomGuardBody(3), modRandomGuardHead(3), team);
}

#ifndef PLATFORM_N64
/** For the Spawn Customizer's self-test: bring one returning guard in; the config pad it stands on, -2 if every place was taken, -1 if it failed. */
s32 modRandomTestReturningGuard(void)
{
	struct prop *prop = modRandomSpawnReturningGuard(-1, TEAM_ENEMY);
	s32 best = -1;
	s32 i;

	if (!prop || !prop->chr) {
		// refused: fine if every place was taken (a guard stands on it, or the
		// player is near), which a small config on a crowded map can be
		bool alltaken = true;

		for (i = 0; i < modSpawnConfigCount(MODSPAWN_GUARANTEED) + modSpawnConfigCount(MODSPAWN_GUARD); i++) {
			struct pad pad;
			bool taken = false;
			s32 j;

			padUnpack(modRandomReturnPad(i), PADFIELD_POS, &pad);

			for (j = 0; j < chrsGetNumSlots(); j++) {
				struct chrdata *chr = &g_ChrSlots[j];
				f32 dx, dz;

				if (chr->chrnum < 0 || !chr->prop || !chr->model || chrIsDead(chr) || (chr->chrflags & CHRCFLAG_HIDDEN)) continue;
				dx = chr->prop->pos.x - pad.pos.x;
				dz = chr->prop->pos.z - pad.pos.z;
				if (dx * dx + dz * dz < (chr->prop->type == PROPTYPE_PLAYER ? 40000 : 6400) * 1.5f) taken = true;
			}

			if (!taken) alltaken = false;
		}

		return alltaken ? -2 : -1;
	}

	for (i = 0; i < modSpawnConfigCount(MODSPAWN_GUARANTEED) + modSpawnConfigCount(MODSPAWN_GUARD); i++) {
		struct pad pad;
		f32 dx, dz;

		padUnpack(modRandomReturnPad(i), PADFIELD_POS, &pad);
		dx = pad.pos.x - prop->pos.x;
		dz = pad.pos.z - prop->pos.z;

		if (dx * dx + dz * dz < 150.0f * 150.0f) {
			best = modRandomReturnPad(i);
		}
	}

	return best;
}
#endif

static void modRandomRespawnRoundGuards(void)
{
	s32 i, spawned = 0;
	if (!g_ModRandomGuardsCaptured || g_Vars.lvframe60 < g_ModRandomGuardRespawn60) return;
	// One arrival per two seconds, with a short retry if all pending sites are blocked.
	g_ModRandomGuardRespawn60 = g_Vars.lvframe60 + TICKS(30);
	for (i = 0; i < g_ModRandomLists.numchrs; i++) {
		struct packedchr *row = g_ModRandomLists.chrs[i];
		struct prop *prop;
		if (!g_ModRandomGuardPending[i]) continue;
		if (chrsGetNumFree() < 4) break; // keep space for target and buddies
		prop = modRandomSpawnReturningGuard(g_ModRandomLists.vanillachrpads[i], row->team);
		if (!prop || !prop->chr) continue;
		// A replacement must start as a fresh, unengaged guard. Do not restore
		// the old row's combat AI or any target/hearing state left by the guard
		// whose corpse occupied this slot; those made respawns immediately chase
		// the player. The patrol hook below will attach the randomized route.
		prop->chr->alertness = 0;
		prop->chr->target = -1;
		prop->chr->lastseetarget60 = 0;
		prop->chr->lastvisibletarget60 = 0;
		prop->chr->lastheartarget60 = 0;
		prop->chr->lastshooter = -1;
		prop->chr->timeshooter = 0;
		prop->chr->hidden &= ~(CHRHFLAG_IS_HEARING_TARGET | CHRHFLAG_FIRINGLEFT | CHRHFLAG_FIRINGRIGHT);
		prop->chr->ailist = ailistFindById(GAILIST_UNALERTED);
		prop->chr->aioffset = 0;
		prop->chr->aireturnlist = -1;
		prop->chr->aishotlist = -1;
		chrStand(prop->chr);
		prop->chr->naturalanim = row->naturalanim;
		prop->chr->padpreset1 = row->padpreset;
		prop->chr->chrpreset1 = row->chrpreset;
		// Reattach the original randomized patrol to the replacement's new ID.
		for (s32 p = 0; p < g_ModRandomNumPatrols; p++) {
			struct modrandompatrol *patrol = &g_ModRandomPatrols[p];
			if (patrol->chrnum == g_ModRandomGuardIds[i]) {
				patrol->chrnum = prop->chr->chrnum;
				patrol->pending = true;
				break;
			}
		}
		g_ModRandomGuardIds[i] = prop->chr->chrnum;
		g_ModRandomGuardPending[i] = false;
		modRandomDebugSpawn(prop->chr, MODDBG_ROUND);
		chrGiveWeapon(prop->chr, MODEL_CHRCMP150, WEAPON_CMP150, 0);
		// modRandomSpawnRoundNpc() already ran modRandomSpawnFadeIn().
		g_ModRandomGuardRespawn60 = g_Vars.lvframe60 + TICKS(120);
		spawned++;
		break;
	}
	if (spawned) {
		rebuildTeams();
		rebuildSquadrons();
#ifndef PLATFORM_N64
		sysLogPrintf(0, "randomizer: round guards respawned %d", spawned);
#endif
	}
}

/**
 * What Cassandra can carry: seven guns a guard never has. One is dealt to her
 * from the target's own stream. The gun she drops is marked, and picking it
 * up gives 100 rounds (modRandomIsCassandraGun(), propobj.c).
 */
static void modRandomGiveCassandraGun(struct chrdata *chr)
{
	static const s32 guns[] = {
		WEAPON_REAPER, WEAPON_SUPERDRAGON, WEAPON_RCP45,
		WEAPON_RCP120, WEAPON_CROSSBOW, WEAPON_FARSIGHT,
	};
	struct modrandomstream rng;
	struct prop *prop = NULL;
	s32 start;
	s32 k;
	s32 i;

	modRandomOpen(&rng, 0x43415347, g_ModRandomCleared);
	start = modRandomBelow(&rng, ARRAYCOUNT(guns));

	for (k = 0; k < ARRAYCOUNT(guns) && !prop; k++) {
		const s32 weaponnum = guns[(start + k) % ARRAYCOUNT(guns)];

		for (i = 1; i < MODRANDOM_MPWEAPONS; i++) {
			if (g_MpWeapons[i].weaponnum == weaponnum && g_MpWeapons[i].model >= 0
					&& g_MpWeapons[i].unlockfeature != MPFEATURE_NEVER) {
				prop = chrGiveWeapon(chr, g_MpWeapons[i].model, weaponnum, 0);
				break;
			}
		}
	}

	if (!prop) {
		prop = chrGiveWeapon(chr, MODEL_CHRSUPERDRAGON, WEAPON_SUPERDRAGON, 0);
	}

	if (prop && prop->obj && g_ModRandomNumCassandraGuns < ARRAYCOUNT(g_ModRandomCassandraGuns)) {
		g_ModRandomCassandraGuns[g_ModRandomNumCassandraGuns++] = (struct weaponobj *)prop->obj;
	}
}

bool modRandomIsCassandraGun(struct weaponobj *weapon)
{
	s32 i;

	for (i = 0; weapon && i < g_ModRandomNumCassandraGuns; i++) {
		if (g_ModRandomCassandraGuns[i] == weapon) {
			return true;
		}
	}

	return false;
}

s32 modRandomGiveAllies(s32 count)
{
	return modRandomSpawnAllies(count);
}

static void modRandomShowPacifyFailure(const struct modrandomtarget *target)
{
	char text[112];
	snprintf(text, sizeof(text), "You killed %s. Mission failed!\n", target->name);
	hudmsgCreateWithFlags(text, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
}

void modRandomObjectiveReactToPlayer(struct chrdata *chr, struct prop *player)
{
	if (!player || player->type != PROPTYPE_PLAYER || chrIsDead(chr)
			|| modRandomChrKnockedOut(chr) || (chr->hidden & CHRHFLAG_PSYCHOSISED)) return;
	if (chr->ailist != ailistFindById(GAILIST_ROGUE_OBJECTIVE_WAIT)
			&& chr->aireturnlist != GAILIST_ROGUE_OBJECTIVE_WAIT) return;

	chr->target = player - g_Vars.props;
	chr->p1p2 = playermgrGetPlayerNumByProp(player);
	chr->alertness = 255;
	chr->aireturnlist = GAILIST_ROGUE_OBJECTIVE_PURSUE;
	chr->aishotlist = GAILIST_ROGUE_OBJECTIVE_PURSUE;
	chr->ailist = ailistFindById(GAILIST_ROGUE_OBJECTIVE_PURSUE);
	chr->aioffset = 0;
}

// Use the actual previous objective's positions. It may have used a fallback
// pad or moved before it was completed.
static f32 modRandomPreviousTargetDistanceSquared(const struct coord *pos)
{
	if (g_ModRandomCleared == 0 || g_ModRandomCleared > g_ModRandomNumTargets) return 1e30f;

	const struct modrandomtarget *previous = &g_ModRandomTargets[g_ModRandomCleared - 1];
	if (!previous->complete) return 1e30f;
	const f32 spawnx = pos->x - previous->spawnpos.x;
	const f32 spawnz = pos->z - previous->spawnpos.z;
	const f32 finishx = pos->x - previous->finishpos.x;
	const f32 finishz = pos->z - previous->finishpos.z;
	const f32 spawnDist = spawnx * spawnx + spawnz * spawnz;
	const f32 finishDist = finishx * finishx + finishz * finishz;
	return spawnDist < finishDist ? spawnDist : finishDist;
}

static const f32 g_ModRandomTargetSpacing[] = { 1800.0f, 1200.0f, 800.0f, 400.0f, 0.0f };

static void modRandomFacePlayerWhileWaiting(struct modrandomtarget *target, struct chrdata *chr)
{
	struct prop *player = g_Vars.bond->prop;
	if (!player || !chr->prop || !chr->model || chr->actiontype != ACT_STAND
			|| chr->act_stand.prestand || chr->act_stand.flags
			|| chr->ailist != ailistFindById(GAILIST_ROGUE_OBJECTIVE_WAIT)
			|| (chr->hidden & CHRHFLAG_PSYCHOSISED)) {
		return;
	}

	const s32 now = g_Vars.lvframe60;
	const bool visible = player->chr && !(player->chr->hidden & (CHRHFLAG_CLOAKED | CHRHFLAG_UNTARGETABLE))
		&& chrHasLosToProp(chr, player);
	const f32 current = chrGetInverseTheta(chr);
	if (visible || now >= target->nextFacingAt60) {
		const f32 dx = player->pos.x - chr->prop->pos.x;
		const f32 dz = player->pos.z - chr->prop->pos.z;
		target->facingAngle = dx * dx + dz * dz > 1.0f ? atan2f(dx, dz) : current;
		target->nextFacingAt60 = now + TICKS(5 * 60);
	}

	f32 delta = target->facingAngle - current;
	if (delta > M_PI) delta -= M_BADTAU;
	if (delta < -M_PI) delta += M_BADTAU;
	const f32 step = M_PI * g_Vars.lvupdate60freal / 60.0f;
	f32 angle = current + MAX(-step, MIN(step, delta));
	if (angle < 0.0f) angle += M_BADTAU;
	if (angle >= M_BADTAU) angle -= M_BADTAU;
	chrSetLookAngle(chr, angle);
}

static struct prop *modRandomSpawnRoundTarget(struct modrandomtarget *target, f32 minGapSquared, f32 maxGapSquared, bool authored)
{
	const struct coord *playerpos = &g_Vars.bond->prop->pos;
	s32 count = g_ModRandomNumTargets + g_ModRandomLists.numchrs;
#ifndef PLATFORM_N64
	s32 start = 0;
	if (authored) {
		struct modrandomstream rng;
		count = modSpawnConfigCount(MODSPAWN_OBJECTIVE);
		if (!count) return NULL;
		modRandomOpen(&rng, MODRANDOM_STREAM_TARGET, g_ModRandomCleared + 1);
		start = modRandomBelow(&rng, count);
	}
#else
	if (authored) return NULL;
#endif
	for (s32 i = 0; i < count; i++) {
		s32 padnum;
#ifndef PLATFORM_N64
		if (authored) {
			padnum = modSpawnConfigPad(MODSPAWN_OBJECTIVE, (start + i) % count);
		} else
#endif
		if (i < g_ModRandomNumTargets) {
			padnum = g_ModRandomTargets[(g_ModRandomCleared + i) % g_ModRandomNumTargets].padnum;
		} else {
			const s32 j = (i - g_ModRandomNumTargets + g_ModRandomCleared * 7) % g_ModRandomLists.numchrs;
			const struct packedchr *row = g_ModRandomLists.chrs[j];
			if (row->chrnum < 0 || row->team == TEAM_00 || row->team == TEAM_NONCOMBAT
					|| (row->team & (TEAM_01 | TEAM_ALLY))
					|| (row->spawnflags & (SPAWNFLAG_INVINCIBLE | SPAWNFLAG_HIDDEN))) continue;
			padnum = g_ModRandomLists.vanillachrpads[j];
		}
		if (padnum < 0 || padnum >= g_PadsFile->numpads) continue;
#ifndef PLATFORM_N64
		if (!authored && modSpawnConfigIsPadOfKind(MODSPAWN_OBJECTIVE, padnum)) continue;
#endif
		struct pad candidate;
		padUnpack(padnum, PADFIELD_POS | PADFIELD_ROOM, &candidate);
		if (candidate.room <= 0 || candidate.room >= g_Vars.roomcount || !g_ModRandomReached[candidate.room]) continue;
		const f32 dx = candidate.pos.x - playerpos->x;
		const f32 dz = candidate.pos.z - playerpos->z;
		const f32 gapSquared = modRandomPreviousTargetDistanceSquared(&candidate.pos);
		if (dx * dx + dz * dz < 800.0f * 800.0f
				|| gapSquared < minGapSquared || gapSquared >= maxGapSquared) continue;

		struct prop *prop = modRandomSpawnRoundNpc(padnum, target->bodynum, target->headnum, target->team);
		if (!prop) continue;
		if (modRandomTargetSpawnIsSafe(prop, padnum)) {
			target->padnum = padnum;
			return prop;
		}

		// chrRemove releases the model/chr but leaves the prop on the active list.
		chrRemove(prop, true);
		propDelist(prop);
		propDisable(prop);
		propFree(prop);
	}

	return NULL;
}

static void modRandomTickRounds(void)
{
	struct modrandomtarget *target;
	struct chrdata *chr;
	if (!modRandomSequential() || !g_ModRandomNumTargets || g_ModRandomRunOver || g_ModRandomAlienWarActive
			|| g_InCutscene || lvIsPaused() || g_Vars.currentplayer != g_Vars.bond
			|| g_Vars.currentplayer->isdead || g_Vars.currentplayer->dostartnewlife
			|| g_ModRandomSpawnPending || g_Vars.lvframenum < 2) return;
	modRandomCaptureRoundGuards();
	{
		s32 k;

		// killing a pacify target after she was disarmed still fails the mission
		for (k = 0; k < g_ModRandomNumTargets; k++) {
			if (g_ModRandomTargets[k].pacify && g_ModRandomTargets[k].complete && g_ModRandomTargets[k].killedbyplayer) {
				if (g_ModRandomPacifyFailAt60 == 0) {
					g_ModRandomPacifyFailAt60 = g_Vars.lvframe60 + TICKS(180);
					modRandomShowPacifyFailure(&g_ModRandomTargets[k]);
				} else if (g_Vars.lvframe60 >= g_ModRandomPacifyFailAt60) {
					g_ModRandomRunOver = true;
					mainEndStage();
				}

				return;
			}
		}
	}
	if (g_ModRandomCleared >= g_ModRandomNumTargets) return;
	target = &g_ModRandomTargets[g_ModRandomCleared];
	chr = chrFindByLiteralId(target->chrnum);
	if (!target->spawned) {
		if (!chr || !chr->prop || !chr->model) {
			struct prop *prop = NULL;
			if (!g_ModRandomReached || !g_PadsFile || g_Vars.lvframe60 < g_ModRandomTargetRetry60) return;
			g_ModRandomTargetRetry60 = g_Vars.lvframe60 + TICKS(60);
			// At each distance, try the marked objective sites before guard sites.
			f32 maxGapSquared = 1e31f;
			for (s32 i = 0; !prop && i < ARRAYCOUNT(g_ModRandomTargetSpacing); i++) {
				const f32 gap = g_ModRandomTargetSpacing[i];
				prop = modRandomSpawnRoundTarget(target, gap * gap, maxGapSquared, true);
				if (!prop) prop = modRandomSpawnRoundTarget(target, gap * gap, maxGapSquared, false);
				if (g_ModRandomCleared == 0) break;
				maxGapSquared = gap * gap;
			}
			if (!prop || !prop->chr) return; // retry blocked locations, never fail a future objective
			chr = prop->chr;
			chrSetChrnum(chr, target->chrnum);
			modRandomGiveCassandraGun(chr);
			rebuildTeams();
			rebuildSquadrons();
		}
		modRandomRestoreTargetVisibility(chr);
		chr->ailist = ailistFindById(GAILIST_ROGUE_OBJECTIVE_WAIT);
		chr->aioffset = 0;
		chr->aishotlist = GAILIST_ROGUE_OBJECTIVE_WAIT;
		target->spawned = true;
		target->spawnpos = chr->prop->pos;
		target->nextFacingAt60 = g_Vars.lvframe60;
		target->facingAngle = chrGetInverseTheta(chr);
		if (!chr->weapons_held[HAND_RIGHT]) modRandomGiveCassandraGun(chr);
		if (!target->pacify && (modRandomGetVersion() >= 19 ? modCareerObjectiveShield() : g_ModRandomCleared == 2)) {
			chrSetShield(chr, 8.0f);
		}
		g_ModRandomTargetActive60 = g_Vars.lvframe60;
#ifndef PLATFORM_N64
		if (g_ModRandomCleared > 0) {
			sysLogPrintf(0, "randomizer: target %d distance from previous objective: %.0f", g_ModRandomCleared + 1,
					sqrtf(modRandomPreviousTargetDistanceSquared(&target->spawnpos)));
		}
		sysLogPrintf(0, "randomizer: target %d active: chr %d pad %d room %d (%s, %.1f %.1f %.1f)",
				g_ModRandomCleared + 1, target->chrnum, target->padnum, chr->prop->rooms[0],
				modSpawnConfigIsPadOfKind(MODSPAWN_OBJECTIVE, target->padnum) ? "marked objective" : "fallback",
				target->spawnpos.x, target->spawnpos.y, target->spawnpos.z);
#endif
	}
#ifndef PLATFORM_N64
	// Hidden, separate-save smoke runs use the normal death and round machinery.
	if (sysArgCheck("--random-round-test")) {
		s32 i, living = 0;
		g_Vars.currentplayer->prop->chr->chrflags |= CHRCFLAG_INVINCIBLE;
		for (i = 0; i < g_ModRandomNumTargets; i++) {
			struct chrdata *other = chrFindByLiteralId(g_ModRandomTargets[i].chrnum);
			if (other && other->prop && other->model && !chrIsDead(other)) living++;
		}
		if (living > 1) sysLogPrintf(2, "random-round-test: FAIL multiple living targets: %d", living);
		if (chr && !chrIsDead(chr) && g_Vars.lvframe60 - g_ModRandomTargetActive60 > TICKS(180)) {
			struct coord dir = {0, 0, 1};
			struct gset gset = {0};
			gset.weaponnum = WEAPON_FALCON2;
			for (i = 0; g_ModRandomGuardsCaptured && i < g_ModRandomLists.numchrs; i++) {
				struct chrdata *guard = chrFindByLiteralId(g_ModRandomGuardIds[i]);
				if (guard && guard->prop && guard->model && !chrIsDead(guard)) {
					chrBeginDeath(guard, &dir, 0, HITPART_TORSO, &gset, false, 0);
					break;
				}
			}
			chrBeginDeath(chr, &dir, 0, HITPART_TORSO, &gset, false, 0);
			sysLogPrintf(0, "random-round-test: eliminated target %d; future status %d", g_ModRandomCleared + 1,
				g_ModRandomCleared + 1 < g_ModRandomNumTargets ? objectiveCheck(g_ModRandomCleared + 1) : -1);
		}
	}
#endif
	if (chr && target->spawned && !chrIsDead(chr) && !modRandomChrKnockedOut(chr)) {
		// Any objective target can lose their weapon; only a KO target gives up.
		chr->flags |= CHRFLAG0_CANLOSEGUN;
		if (target->pacify) {
			if (chrGetHeldProp(chr, HAND_RIGHT) || chrGetHeldProp(chr, HAND_LEFT)) {
				target->armed = true;
			} else if (target->armed && chr->actiontype != ACT_SURRENDER) {
				chr->gunprop = NULL; // no running back for it
				chrSurrender(chr);
			}
		}
		modRandomFacePlayerWhileWaiting(target, chr);
	}
	if (target->pacify && target->killedbyplayer) {
		// The player killed a Cassandra who was to be pacified: the mission is lost
		if (g_ModRandomPacifyFailAt60 == 0) {
			g_ModRandomPacifyFailAt60 = g_Vars.lvframe60 + TICKS(180);
			modRandomShowPacifyFailure(target);
		} else if (g_Vars.lvframe60 >= g_ModRandomPacifyFailAt60) {
			g_ModRandomRunOver = true;
			mainEndStage();
		}
		return;
	}
	if (modRandomTargetGone(target, chr) || (chr && (chrIsDead(chr) || (target->pacify && modRandomChrKnockedOut(chr))))) {
		if (!chr) {
			sysLogPrintf(0, "randomizer: target %d left the level (kill floor) - counted as killed", g_ModRandomCleared + 1);
		}
		target->finishpos = chr && chr->prop ? chr->prop->pos : target->spawnpos;
		target->complete = true;
		g_ModRandomCleared++;
		modRandomDebugMsg("OBJECTIVE %d/%d complete", g_ModRandomCleared, g_ModRandomNumTargets);
#ifndef PLATFORM_N64
		sysLogPrintf(0, "randomizer: completed target %d of %d", g_ModRandomCleared, g_ModRandomNumTargets);
#endif
		if (g_ModRandomCleared < g_ModRandomNumTargets) {
			modRandomQueueGuardRespawns();
			g_ModRandomTargetRetry60 = 0;
		}
		return;
	}
	modRandomRespawnRoundGuards();
}

static bool modRandomAllObjectivesComplete(void)
{
	s32 i;

	for (i = 0; i < g_ModRandomNumObjectives; i++) {
		if (objectiveCheck(i) != OBJECTIVE_COMPLETE) {
			return false;
		}
	}

	return g_ModRandomNumObjectives > 0;
}

s32 modRandomHoldStatus(void)
{
	return g_ModRandomClimaxDone ? OBJECTIVE_COMPLETE : OBJECTIVE_INCOMPLETE;
}

bool modRandomGetClimaxZone(struct coord *pos)
{
	if (!modRandomIsOn() || g_ModRandomClimaxSlot < 0 || !g_ModRandomClimaxStarted
			|| g_ModRandomClimaxDone || g_ModRandomRunOver) {
		return false;
	}

	*pos = g_ModRandomClimaxPos;
	return true;
}

/** Choose a reachable escape pad, or a regular map pad when no config is selected. */
static bool modRandomClimaxPickZone(s32 attempt, const s32 *failedPads)
{
	struct modrandomstream rng;
	struct coord from;
	f32 bestscore = 1e30f;
	s32 bestTier = ARRAYCOUNT(g_ModRandomTargetSpacing);
	s32 best = -1;
	s32 tries;

	if (!g_PadsFile || g_PadsFile->numpads <= 0 || !g_ModRandomReached || !g_Vars.bond || !g_Vars.bond->prop) {
		return false;
	}

	from = g_Vars.bond->prop->pos;
	modRandomOpen(&rng, 0x434c4d58, attempt);

	s32 candidates = 200;
#ifndef PLATFORM_N64
	const bool configured = modSpawnConfigChosenName(g_Vars.stagenum) != NULL;
	if (configured) candidates = modSpawnConfigCount(MODSPAWN_ESCAPE);
#endif
	for (tries = 0; tries < candidates; tries++) {
#ifndef PLATFORM_N64
		const s32 padnum = configured ? modSpawnConfigPad(MODSPAWN_ESCAPE, tries)
			: modRandomBelow(&rng, g_PadsFile->numpads);
#else
		const s32 padnum = modRandomBelow(&rng, g_PadsFile->numpads);
#endif
		struct pad pad;
		f32 dx, dz, dist, score;
		s32 room, j;

		for (j = 0; j < attempt && failedPads[j] != padnum; j++) {}
		// Bunker 1 (stage 0x54): pad 152 sits where the computer cannot be reached
		if (g_Vars.stagenum == 0x54 && padnum == 152) {
			continue;
		}
		if (j < attempt || !modRandomPadCanSpawn(padnum)) {
			continue;
		}

		padUnpack(padnum, PADFIELD_POS | PADFIELD_ROOM, &pad);
		room = modRandomPadRoom(padnum);

		if (room <= 0 || room >= g_Vars.roomcount || !g_ModRandomReached[room]) {
			continue;
		}

		dx = pad.pos.x - from.x;
		dz = pad.pos.z - from.z;
		dist = sqrtf(dx * dx + dz * dz);

		// about 3000 units off, and on the player's own floor if there is one
		score = fabsf(dist - 3000.0f) + fabsf(pad.pos.y - from.y) * 2.0f;

		if (dist < 1200.0f) {
			score += 5000.0f;
		}

		const f32 gapSquared = modRandomPreviousTargetDistanceSquared(&pad.pos);
		s32 tier = 0;
		while (gapSquared < g_ModRandomTargetSpacing[tier] * g_ModRandomTargetSpacing[tier]) tier++;

		if (tier < bestTier || (tier == bestTier && score < bestscore)) {
			bestTier = tier;
			bestscore = score;
			best = padnum;
			g_ModRandomClimaxPad = padnum;
			g_ModRandomClimaxPos = pad.pos;
		}
	}

	return best >= 0;
}

/** One wave of the climax: hostile guards into the level, and Velvet-style friends at the zone. */
/**
 * Where a climax guard stands: the map's guard site [fallback], or, with a
 * Spawn Customizer config that has Hunter spawns, one of those (the waves
 * are reinforcements, so they arrive where the reinforcements do).
 */
static s32 modRandomClimaxPad(struct modrandomstream *rng, s32 fallback)
{
#ifndef PLATFORM_N64
	s32 hunters = modSpawnConfigCount(MODSPAWN_HUNTER);

	if (hunters > 0) {
		return modSpawnConfigPad(MODSPAWN_HUNTER, modRandomBelow(rng, hunters));
	}
#endif
	return fallback;
}

#ifndef PLATFORM_N64
/** For the Spawn Customizer's self-test: a climax guard's pad, a config Hunter spawn when there is one. */
s32 modRandomTestClimaxPad(void)
{
	struct modrandomstream rng;

	modRandomOpen(&rng, 0x434c5750, 0);
	return modRandomClimaxPad(&rng, -1);
}
#endif

static void modRandomClimaxWave(s32 wave)
{
	struct modrandomstream rng;
	s32 wanted = 4 + wave * 2;
	s32 sent = 0;
	s32 tries;
	s32 allies = 0;

	if (g_ModRandomLists.numchrs > 0) {
		modRandomOpen(&rng, 0x434c5750, wave);

		for (tries = 0; tries < 80 && sent < wanted; tries++) {
			s32 j = modRandomBelow(&rng, g_ModRandomLists.numchrs);
			struct packedchr *row = g_ModRandomLists.chrs[j];
			s32 padnum = modRandomClimaxPad(&rng, g_ModRandomLists.vanillachrpads[j]);
			struct pad pad;
			struct prop *prop;
			f32 dx, dz, d2;
			s32 room;

			if (chrsGetNumFree() < 6) {
				break;
			}

			if (row->chrnum < 0 || row->team == TEAM_00 || row->team == TEAM_NONCOMBAT
					|| (row->team & (TEAM_01 | TEAM_ALLY))
					|| (row->spawnflags & (SPAWNFLAG_INVINCIBLE | SPAWNFLAG_HIDDEN))) {
				continue;
			}

			if (padnum < 0 || !g_PadsFile || padnum >= g_PadsFile->numpads) {
				continue;
			}

			room = modRandomPadRoom(padnum);

			if (room <= 0 || room >= g_Vars.roomcount || !g_ModRandomReached[room]) {
				continue;
			}

			padUnpack(padnum, PADFIELD_POS, &pad);
			dx = pad.pos.x - g_ModRandomClimaxPos.x;
			dz = pad.pos.z - g_ModRandomClimaxPos.z;
			d2 = dx * dx + dz * dz;

			if (d2 < 900.0f * 900.0f || d2 > 4500.0f * 4500.0f) {
				continue;
			}

			prop = modRandomSpawnRoundNpc(padnum, modRandomGuardBody(1), modRandomGuardHead(1), row->team);

			if (!prop || !prop->chr) {
				continue;
			}

			// straight into the fight: alerted, and coming for the player
			prop->chr->alertness = 100;
			prop->chr->target = -1;
			prop->chr->ailist = ailistFindById(GAILIST_ALERTED);
			prop->chr->aioffset = 0;
			prop->chr->aireturnlist = -1;
			prop->chr->aishotlist = -1;
			chrGiveWeapon(prop->chr, MODEL_CHRCMP150, WEAPON_CMP150, 0);
			sent++;
		}
	}

	// friends arrive at the zone itself
	if (chrsGetNumFree() >= 4) {
		RoomNum room = g_Vars.bond && g_Vars.bond->prop ? g_Vars.bond->prop->rooms[0] : 0;
		s32 zoneroom = -1;
		s32 k;

		// the room the zone stands in: the pad nearest to it
		if (g_PadsFile) {
			f32 best = 1e30f;

			for (k = 0; k < g_PadsFile->numpads; k++) {
				struct pad pad;
				f32 dx, dy, dz, d;

				padUnpack(k, PADFIELD_POS | PADFIELD_ROOM, &pad);
				dx = pad.pos.x - g_ModRandomClimaxPos.x;
				dy = pad.pos.y - g_ModRandomClimaxPos.y;
				dz = pad.pos.z - g_ModRandomClimaxPos.z;
				d = dx * dx + dy * dy + dz * dz;

				if (d < best && pad.room > 0) {
					best = d;
					zoneroom = pad.room;
				}
			}
		}

		if (zoneroom > 0) {
			room = zoneroom;
		}

		allies = modRandomSpawnAlliesAt(2, false, &g_ModRandomClimaxPos, room);
	}

	if (sent || allies) {
		rebuildTeams();
		rebuildSquadrons();
	}

#ifndef PLATFORM_N64
	sysLogPrintf(0, "randomizer: climax wave %d: %d hostile, %d friendly", wave + 1, sent, allies);
#endif
}

/**
 * The climax turns the level's civilians into fighters: they draw guns, change
 * to the player's side and take on the guards. A guard killing one of them
 * costs nothing - only the player's kill sends reinforcements
 * (modRandomOnChrDeath()).
 */
static void modRandomArmCivilian(struct modrandomcivilian *civilian, struct chrdata *chr)
{
	civilian->armed = true;
	chr->team = TEAM_ALLY;
	chr->flags |= CHRFLAG0_AIVSAI;
	chr->chrflags &= ~CHRCFLAG_NEVERSLEEP;
	chr->alertness = 90;
	chr->target = -1;
	chr->ailist = ailistFindById(GAILIST_INIT_DEFAULT_BUDDY);
	chr->aioffset = 0;
	chr->aireturnlist = -1;
	chr->aishotlist = -1;
	chrStand(chr);
	if (!chrGetHeldProp(chr, HAND_RIGHT)) {
		g_ModRandomEventGrant = true;
		chrGiveWeapon(chr, MODEL_CHRFALCON2, WEAPON_FALCON2, 0);
		g_ModRandomEventGrant = false;
	}
}

static s32 modRandomClimaxArmCivilians(void)
{
	s32 i;
	s32 armed = 0;

	for (i = 0; i < MODRANDOM_MAX_CIVILIANS; i++) {
		struct modrandomcivilian *civilian = &g_ModRandomCivilians[i];
		struct chrdata *chr;

		if (civilian->chrnum < 0 || civilian->dead || civilian->armed) {
			continue;
		}

		chr = chrFindByLiteralId(civilian->chrnum);

		if (!chr || !chr->prop || !chr->model || chrIsDead(chr)) {
			continue;
		}

		modRandomArmCivilian(civilian, chr);
		armed++;
	}

	// and every guard looks for enemies among them
	for (i = 0; i < chrsGetNumSlots(); i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr->chrnum < 0 || !chr->prop || !chr->model || chr->prop->type != PROPTYPE_CHR
				|| chrIsDead(chr) || chr->team == TEAM_ALLY || chr->team == TEAM_NONCOMBAT
				|| chr->team == TEAM_00) {
			continue;
		}

		chr->flags |= CHRFLAG0_AIVSAI;
	}

	if (armed) {
		rebuildTeams();
		rebuildSquadrons();
		hudmsgCreateWithFlags("The civilians have taken up arms!\n", HUDMSGTYPE_DEFAULT,
				HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
	}
#ifndef PLATFORM_N64
	sysLogPrintf(0, "randomizer: climax armed %d civilians", armed);
#endif
	return armed;
}

/**
 * The two items a gauntlet boon can bring. They are Data Uplinks in all but
 * name (modRandomItemsInit()), and are spent when used: the item is taken
 * from the player a moment after the blow lands, once the hand has put it down.
 */

// Partners still to arrive. The level's character slots are mostly spent on
// guards and bodies by the time the phone is used, so a spawn can fail; the
// request is kept and retried each tick (a failed spawn also marks a corpse to
// be reaped, which frees a slot a moment later).
static s32 g_ModRandomPhoneWanted;
static s32 g_ModRandomPhoneGot;
static s32 g_ModRandomPhoneTries;
static s32 g_ModRandomAlienAllyWanted;
static s32 g_ModRandomAlienAllyGot;
static s32 g_ModRandomAlienAllyFaction;
static s32 g_ModRandomAlienAllyTries;

void modRandomUseFriendPhone(void)
{
	if (g_ModRandomPhoneWanted > 0) {
		hudmsgCreateWithFlags("Friend Phone: already calling\n", HUDMSGTYPE_DEFAULT,
				HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
		return;
	}

	// used up, unless a Gauntlet boon left another one
	if (!modCareerSpendCharge(WEAPON_TESTER)) {
		invRemoveItemByNum(WEAPON_TESTER);
	}

	g_ModRandomPhoneWanted = 1 + rngRandom() % 3;
	g_ModRandomPhoneGot = 0;
	g_ModRandomPhoneTries = 0;
	sysLogPrintf(0, "randomizer: friend phone used, %d wanted", g_ModRandomPhoneWanted);
}

static void modRandomTickFriendPhone(void)
{
	char text[64];

	if (g_ModRandomPhoneWanted <= 0 || !g_Vars.bond || g_Vars.currentplayer != g_Vars.bond || lvIsPaused()) {
		return;
	}

	g_ModRandomPhoneGot += modRandomSpawnAllies(1);
	g_ModRandomPhoneTries++;

	if (g_ModRandomPhoneGot >= g_ModRandomPhoneWanted || g_ModRandomPhoneTries > 60 * 8) {
		snprintf(text, sizeof(text), g_ModRandomPhoneGot ? "Friend Phone: %d partner%s on the way\n" : "Friend Phone: no one picked up\n",
				g_ModRandomPhoneGot, g_ModRandomPhoneGot == 1 ? "" : "s");
		hudmsgCreateWithFlags(text, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
		sysLogPrintf(0, "randomizer: friend phone: %d of %d after %d tries", g_ModRandomPhoneGot,
				g_ModRandomPhoneWanted, g_ModRandomPhoneTries);
		g_ModRandomPhoneWanted = 0;
	}
}

static s32 modRandomSpawnAlienAllies(s32 count, s32 faction)
{
	struct player *player = g_Vars.bond;
	struct coord pos;
	RoomNum rooms[8];
	s32 spawned = 0, i;
	if (!player || !player->prop) return 0;
	pos = player->prop->pos;
	memcpy(rooms, player->prop->rooms, sizeof(rooms));
	for (i = 0; i < count; i++) {
		s32 body = faction == 1 ? BODY_MAIAN_SOLDIER : BODY_SKEDAR;
		s32 head = faction == 1 ? MOD_HEADNUM(HEAD_MAIAN_S) : HEAD_DARK_COMBAT;
		struct prop *prop = chrSpawnAtCoord(body, head, &pos, rooms,
				atan2f(-player->bond2.unk00.x, player->bond2.unk00.z),
				ailistFindById(GAILIST_INIT_DEFAULT_BUDDY), SPAWNFLAG_ALLOWONSCREEN);
		if (!prop || !prop->chr) continue;
		prop->chr->team = TEAM_ALLY;
		prop->chr->voicebox = VOICEBOX_MALE0;
		prop->chr->maxdamage *= MODRANDOM_ALLY_HEALTH_MULTIPLIER;
		chrGiveWeaponWithAutoModel(prop->chr, faction == 1 ? WEAPON_CALLISTO : WEAPON_REAPER, 0);
		modRandomSpawnFadeIn(prop->chr);
		spawned++;
	}
	if (spawned) { rebuildTeams(); rebuildSquadrons(); }
	return spawned;
}

static void modRandomTickAlienAllies(void)
{
	if (g_ModRandomAlienAllyWanted <= 0 || !g_Vars.bond || g_Vars.currentplayer != g_Vars.bond || lvIsPaused()) return;
	g_ModRandomAlienAllyGot += modRandomSpawnAlienAllies(1, g_ModRandomAlienAllyFaction);
	g_ModRandomAlienAllyTries++;
	if (g_ModRandomAlienAllyGot >= g_ModRandomAlienAllyWanted || g_ModRandomAlienAllyTries > TICKS(8 * 60)) {
		char text[80];
		snprintf(text, sizeof(text), g_ModRandomAlienAllyFaction == 1 ? "Maian Backup: %d of %d joined you\n" : "Skedar Swarm: %d of %d joined you\n",
				g_ModRandomAlienAllyGot, g_ModRandomAlienAllyWanted);
		hudmsgCreateWithFlags(text, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
		g_ModRandomAlienAllyWanted = 0;
	}
}

void modRandomUseArmCivs(void)
{
	modRandomClimaxArmCivilians();
	g_ModRandomArmCivsNext = MODRANDOM_BASE_CIVILIANS;
	g_ModRandomArmCivsRetry60 = g_Vars.lvframe60;
	g_ModRandomArmCivsUntil60 = g_Vars.lvframe60 + TICKS(15 * 60);

	// used up, unless a Gauntlet boon left another one
	if (!modCareerSpendCharge(WEAPON_PRESIDENTSCANNER)) {
		invRemoveItemByNum(WEAPON_PRESIDENTSCANNER);
	}
}

static s32 modRandomEventCountWaypoints(void);

static void modRandomTickArmedCivArrivals(void)
{
	if (!g_ModRandomArmCivsUntil60 || g_ModRandomRunOver || !g_Vars.bond
			|| g_Vars.currentplayer != g_Vars.bond || g_Vars.bond->isdead || lvIsPaused()
			|| g_InCutscene || g_Vars.lvframe60 < g_ModRandomArmCivsRetry60) return;

	if (g_ModRandomArmCivsNext >= MODRANDOM_MAX_CIVILIANS
			|| g_Vars.lvframe60 >= g_ModRandomArmCivsUntil60) {
		char text[64];
		snprintf(text, sizeof(text), "Arm the Civs: %d of %d armed civilians arrived\n",
			g_ModRandomArmCivsNext - MODRANDOM_BASE_CIVILIANS, MODRANDOM_ARM_CIVS_EXTRA);
		hudmsgCreateWithFlags(text, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
		g_ModRandomArmCivsUntil60 = 0;
		return;
	}

	const s32 waypoints = modRandomEventCountWaypoints();
	s32 configured = 0;
#ifndef PLATFORM_N64
	configured = modSpawnConfigCount(MODSPAWN_GUARD);
#endif
	g_ModRandomArmCivsRetry60 = g_Vars.lvframe60 + TICKS(12);
	for (s32 attempt = 0; attempt < 48 && configured + waypoints > 0; attempt++) {
		s32 padnum;
#ifndef PLATFORM_N64
		if (configured && (attempt < 16 || !waypoints)) {
			padnum = modSpawnConfigPad(MODSPAWN_GUARD, rngRandom() % configured);
		} else
#endif
		{
			struct waypoint *waypoint = &g_StageSetup.waypoints[rngRandom() % waypoints];
			if (!waypoint->neighbours || waypoint->neighbours[0] < 0) continue;
			padnum = waypoint->padnum;
		}

		const s32 room = modRandomPadRoom(padnum);
		if (room <= 0 || room >= g_Vars.roomcount || (g_ModRandomReached && !g_ModRandomReached[room])) continue;

		bool used = false;
		for (s32 i = MODRANDOM_BASE_CIVILIANS; i < g_ModRandomArmCivsNext; i++) {
			const s32 previous = g_ModRandomCivilians[i].destinationPad;
			if ((attempt < 44 && previous == padnum)
					|| (attempt < 32 && modRandomPadRoom(previous) == room)) {
				used = true;
				break;
			}
		}
		if (used) continue;

		const bool secretary = (g_ModRandomArmCivsNext & 1) != 0;
		struct prop *prop = modRandomSpawnRoundNpc(padnum,
			modRandomExtraBody(0, secretary ? BODY_SECRETARY : BODY_STEWARDESS),
			modRandomExtraHead(0, secretary ? HEAD_SECRETARY : HEAD_RANDOM), TEAM_ALLY);
		if (!prop || !prop->chr) continue;

		struct modrandomcivilian *civilian = &g_ModRandomCivilians[g_ModRandomArmCivsNext++];
		civilian->chrnum = prop->chr->chrnum;
		civilian->destinationPad = padnum;
		modRandomArmCivilian(civilian, prop->chr);
		modRandomDebugSpawn(prop->chr, MODDBG_CIV);
		rebuildTeams();
		rebuildSquadrons();
		return;
	}
}

/**
 * Mind Melt: every character in the level is blinded the way Extraction's
 * top-floor guards are when the lights come back on (func0411 in setupark.c:
 * the ANIM_BLINDED animation, half speed), then carries on.
 */
static void modRandomUseMindMelt(void)
{
	struct chrdata *own = g_Vars.bond && g_Vars.bond->prop ? g_Vars.bond->prop->chr : NULL;
	s32 melted = 0;
	s32 i;

	for (i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr->chrnum < 0 || chr == own || chr->race != RACE_HUMAN || !chr->prop || !chr->model || chrIsDead(chr)
				|| modRandomChrKnockedOut(chr) || chr->actiontype == ACT_DIE || chr->actiontype == ACT_DEAD
				|| chr->actiontype == ACT_ARGH || chr->actiontype == ACT_PREARGH) {
			continue;
		}

		chrStartAnim(chr, ANIM_BLINDED, 0, -1, CHRANIMFLAG_SLOWUPDATE, 16, 0.5f);
		melted++;
	}

	sysLogPrintf(0, "randomizer: mind melt blinded %d characters", melted);
	hudmsgCreateWithFlags("Mind Melt: everyone is blinded\n", HUDMSGTYPE_DEFAULT,
			HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
}

/**
 * The items are devices, like the cloaking device: picked from the quick
 * menu they switch on, which here means they go off once and are spent.
 */
// ---------------------------------------------------------------------------
// Special events: once a mission, when an objective is done, something happens
// to the level. Each is a handful of characters on a team of their own, so they
// are everybody's enemy and fight other characters (CHRFLAG0_AIVSAI).
// ---------------------------------------------------------------------------
#define DEVICE_MAIANBACKUP    0x800
#define DEVICE_SKEDARSWARM    0x1000

static s32 g_ModRandomEventKind;        // running now (0: none)
static s32 g_ModRandomInvaderKills;     // Maians killed by the player in the Maian invasion this mission
static s32 g_ModRandomSwarmKills;       // Skedar killed by the player in the Skedar swarm this mission
static s32 g_ModRandomEventLeft;        // still to arrive
static s32 g_ModRandomEventArrived;
static s32 g_ModRandomEventTries;
static s32 g_ModRandomEventNext60;
static s32 g_ModRandomEventBase = -1;   // the squad's waypoint
static bool g_ModRandomSwarmOn;      // a Skedar swarm is loose: its members are sent at targets
static bool g_ModRandomEventFired;      // this mission has had its event
static s32 g_ModRandomEventAfter;       // fires on this many objectives done (0: not chosen yet)
static s32 g_ModRandomEventDone;        // objectives done so far
static bool g_ModRandomAlienWarActive;
static bool g_ModRandomAlienWarOngoing;
static bool g_ModRandomAlienBossesSpawned;
static s32 g_ModRandomAlienElvis = -1;
static s32 g_ModRandomAlienKing = -1;
static s32 g_ModRandomAlienNextBoss60;
static s32 g_ModRandomAlienFaction;       // player's ally: 1 Maian, 2 Skedar
static s32 g_ModRandomAlienWinner;        // faction that keeps spawning after the boss dies
static s32 g_ModRandomAlienPlayerTeam;
static s32 g_ModRandomAlienNextBoss60;
static s32 g_ModRandomAlienInitialMaians;
static s32 g_ModRandomAlienInitialSkedars;
static s32 g_ModRandomAlienNextMaian60;
static s32 g_ModRandomAlienNextSkedar60;
static s32 g_ModRandomAlienNextWinner60;
static s32 g_ModRandomAlienAllyWanted;
static s32 g_ModRandomAlienAllyGot;
static s32 g_ModRandomAlienAllyFaction;
static s32 g_ModRandomAlienAllyTries;
static s32 g_ModRandomTerroristChrnum = -1;
static void *g_ModRandomTerroristModel;

// Guards that lost their gun to a shot and are to give up as soon as they can
#define MODRANDOM_MAX_SURRENDERS 16
static struct { s32 chrnum; s32 until60; } g_ModRandomSurrenders[MODRANDOM_MAX_SURRENDERS];

/**
 * Cassandra, the kill target, never surrenders: she is to be killed. The
 * pacify target (the lab woman) is the one who can, and surrendering pacifies her.
 */
bool modRandomChrMaySurrender(struct chrdata *chr)
{
	s32 i;

	if (!chr || chr->chrnum < 0 || !modRandomIsOn() || !g_ModRandomTargets) {
		return true;
	}

	for (i = 0; i < g_ModRandomNumTargets; i++) {
		if (g_ModRandomTargets[i].chrnum == chr->chrnum) {
			return g_ModRandomTargets[i].pacify;
		}
	}

	return true;
}

void modRandomOnGunShotOut(struct chrdata *chr)
{
	s32 i;

	if (!chr || !modRandomIsOn() || !modRandomChrMaySurrender(chr) || rngRandom() % 10 == 0) { // nine in ten
		return;
	}

	for (i = 0; i < MODRANDOM_MAX_SURRENDERS; i++) {
		if (g_ModRandomSurrenders[i].chrnum <= 0 || g_ModRandomSurrenders[i].until60 < (s32)g_Vars.lvframe60) {
			g_ModRandomSurrenders[i].chrnum = chr->chrnum + 1;
			g_ModRandomSurrenders[i].until60 = g_Vars.lvframe60 + TICKS(240);
			return;
		}
	}
}

void modRandomOnSurrender(struct chrdata *chr)
{
	if (chr && chr->chrnum >= 0 && modRandomIsOn() && !modRandomIsCivilianChr(chr)) {
		g_ModRandomSurrendered[chr->chrnum & 1023] = 1;
	}
}

/**
 * A guard that has surrendered stays surrendered: its list is taken from it (it
 * would only walk it back into the fight), and if a flinch or anything else
 * broke the pose, the hands go back up as soon as it can take an order.
 */
static void modRandomTickSurrendered(void)
{
	s32 i;

	if (lvIsPaused()) {
		return;
	}

	for (i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr->chrnum < 0 || !g_ModRandomSurrendered[chr->chrnum & 1023]) {
			continue;
		}

		if (!chr->prop || !chr->model || chrIsDead(chr)) {
			g_ModRandomSurrendered[chr->chrnum & 1023] = 0;
			continue;
		}

		if (modRandomChrKnockedOut(chr)) {
			continue;
		}

		chr->ailist = ailistFindById(GAILIST_IDLE);
		chr->aioffset = 0;

		if (chr->actiontype != ACT_SURRENDER) {
			chrTrySurrender(chr);
		}
	}
}

/**
 * Stage and punishment guards each have up to four attackers. The other
 * engaged guards hold cover.
 * Seekers move to a post on the approach to the objective, then defend it
 * independently of the attackers.
 */
#define MODRANDOM_TACTICS_DEBUG 1
#define MODRANDOM_MAX_ATTACKERS 4
#define MODRANDOM_POST_ROUTE_MAX 64
#define MODRANDOM_POST_STANDOFF 500.0f
#define MODRANDOM_POST_ARRIVAL 120.0f
#define MODRANDOM_SEEK_ENGAGE_DISTANCE 1000.0f
#define MODRANDOM_POST_TRAVEL60 (25 * 60)
#define MODRANDOM_POST_UNASSIGNED (-2) // -1 is a fallback while there is no active target

enum {
	TACTICS_NONE,
	TACTICS_PASSIVE,
	TACTICS_ATTACKER,
	TACTICS_DEFENDER,
	TACTICS_SEEK,
	TACTICS_PRONE,
	TACTICS_HOLD_POSITION,
	TACTICS_WAIT_FOR_SIGHT,
	TACTICS_COUNT,
};

static u8 g_ModRandomRole[1024];     // by chrnum
static u8 g_ModRandomDefCover[1024]; // a defender that has been sent to cover
static struct modrandompost {
	struct model *model; // distinguishes a seeker from the next occupant of its slot
	s32 objective;
	s32 pad;
	s32 facepad;
	RoomNum entryroom;
	RoomNum room;
	u32 deadline;
} g_ModRandomPosts[1024];
static s32 g_ModRandomProneUntil[1024]; // when a prone enemy gets up (lvframe60)
static u8 g_ModRandomProneMode[1024];   // 0 not yet down, 1 down and aiming, 2 down and firing
#define MODRANDOM_PRONE_CHANCE 5        // one defender in five goes prone
#define MODRANDOM_PRONE_HOLD60 (60 * 120)
static struct model *g_ModRandomTacticsModel[1024]; // whose slot this is: a new model means a new arrival
static s32 g_ModRandomBorn60[1024];                    // when the tactics first saw it
#define MODRANDOM_PASSIVE60 20                         // passive until the next tactics tick (every half second)
static u8 g_ModRandomHold[1024];     // a defender holding out of the attacker rotation
static u32 g_ModRandomHoldUntil60[1024];
#define MODRANDOM_HOLD_LOST_SIGHT60 TICKS(8 * 60)
#define MODRANDOM_HOLD_CHANCE 10         // one new defender in ten
static u8 g_ModRandomArrived[1024];  // entered the fight, whatever its current AI list says

static bool modRandomTacticsOn(void)
{
	return modRandomSequential() && modRandomGetVersion() >= 20 && !g_ModRandomRunOver;
}

/** The debug tint: true and the colour set when this chr is to be tinted. */
bool modRandomTacticsTint(struct chrdata *chr, s32 *colour)
{
#if MODRANDOM_TACTICS_DEBUG
	if (!g_ModRandomDebugTint || !chr || chr->chrnum < 0 || !modRandomTacticsOn()) {
		return false;
	}

	switch (g_ModRandomRole[chr->chrnum & 1023]) {
	case TACTICS_ATTACKER:
		colour[0] = 255; colour[1] = 0; colour[2] = 0; colour[3] = 120;
		return true;
	case TACTICS_PRONE:
		colour[0] = 255; colour[1] = 120; colour[2] = 0; colour[3] = 120;
		return true;
	case TACTICS_SEEK:
		colour[0] = 0; colour[1] = 220; colour[2] = 60; colour[3] = 120;
		return true;
	case TACTICS_HOLD_POSITION:
		colour[0] = 170; colour[1] = 0; colour[2] = 255; colour[3] = 120;
		return true;
	case TACTICS_PASSIVE:
	case TACTICS_WAIT_FOR_SIGHT:
		colour[0] = 0; colour[1] = 70; colour[2] = 255; colour[3] = 120;
		return true;
	case TACTICS_DEFENDER:
		colour[0] = 255; colour[1] = 220; colour[2] = 0; colour[3] = 120;
		return true;
	}
#endif

	return false;
}

/**
 * Explosive exit: a dead guard about to go up blinks red three times, the
 * third blink being the one it explodes on. The tint is the game's own shade
 * (chr.c asks while it draws a character, as for the tactics tint). The wait
 * is split in six: dark, blink, dark, blink, dark, blink - then the blast.
 */
bool modRandomExplosiveTint(struct chrdata *chr, s32 *colour)
{
	s32 i;

	if (!chr || chr->chrnum < 0) {
		return false;
	}

	for (i = 0; i < MODRANDOM_MAX_BLASTS; i++) {
		if (g_ModRandomBlasts[i].used && g_ModRandomBlasts[i].chrnum == chr->chrnum) {
			const s32 total = TICKS(60);
			const s32 elapsed = total - (g_ModRandomBlasts[i].at60 - (s32)g_Vars.lvframe60);
			const s32 sixth = elapsed * 6 / (total > 0 ? total : 1);

			if (elapsed >= 0 && (sixth == 1 || sixth == 3 || sixth >= 5)) {
				colour[0] = 255;
				colour[1] = 0;
				colour[2] = 0;
				colour[3] = 210;
				return true;
			}

			return false;
		}
	}

	return false;
}

bool modRandomTacticsDefenderRadarOn(void)
{
#if MODRANDOM_TACTICS_DEBUG
	return g_ModRandomDebugDefendersRadar && modRandomTacticsOn();
#else
	return false;
#endif
}

u32 modRandomTacticsRadarColour(struct chrdata *chr)
{
#if MODRANDOM_TACTICS_DEBUG
	if ((!g_ModRandomDebugRadar && !g_ModRandomDebugDefendersRadar)
			|| !modRandomTacticsOn() || chrIsDead(chr)) {
		return 0;
	}

	u8 role = g_ModRandomRole[chr->chrnum & 1023];

	if (g_ModRandomDebugRadar && role == TACTICS_ATTACKER) {
		return 0xff000000;
	}

	if (g_ModRandomDebugDefendersRadar && (role == TACTICS_DEFENDER || role == TACTICS_HOLD_POSITION)
			&& !modRandomChrKnockedOut(chr)) {
		return 0xff885500;
	}
#else
	(void)chr;
#endif
	return 0;
}

static bool modRandomTacticsEngaged(struct chrdata *chr)
{
	static const u16 lists[] = {
		GAILIST_ALERTED, GAILIST_COMBAT_WITH_TARGET, GAILIST_CHOOSE_TARGET,
		GAILIST_SEARCH_FOR_PLAYER, GAILIST_DODGE, GAILIST_HAND_COMBAT,
	};
	s32 i;

	for (i = 0; i < (s32)ARRAYCOUNT(lists); i++) {
		if (chr->ailist == ailistFindById(lists[i])) {
			return true;
		}
	}

	return false;
}

static bool modRandomTacticsWarzoneAlly(struct chrdata *chr)
{
	return !g_ModRandomWarzone.active && (chr->hidden2 & CHRH2FLAG_WARZONE) && chr->team == TEAM_ALLY;
}

static bool modRandomTacticsEligible(struct chrdata *chr, struct chrdata *own)
{
	bool punishment = (chr->hidden2 & CHRH2FLAG_PUNISHMENT) != 0;
	bool warzoneally = modRandomTacticsWarzoneAlly(chr);

	if (chr->chrnum < 0 || chr == own || !chr->prop || !chr->model || (chr->hidden2 & CHRH2FLAG_BLONDREVENGE)
			|| (!punishment && !warzoneally && (chr->race != RACE_HUMAN || (chr->team & (TEAM_ALLY | TEAM_NONCOMBAT | TEAM_00 | 0x6c))))
			|| chr->prop->type != PROPTYPE_CHR || chrIsDead(chr) || modRandomChrKnockedOut(chr)
			|| (chr->chrflags & CHRCFLAG_HIDDEN) || ((chr->hidden2 & CHRH2FLAG_WARZONE) && !warzoneally)
			|| chr->actiontype == ACT_SURRENDER || g_ModRandomSurrendered[chr->chrnum & 1023]
			|| modRandomIsObjectiveChr(chr->chrnum) || modRandomIsCivilianChr(chr)) {
		return false;
	}

	return true;
}

void modRandomTacticsQueuePunishment(struct chrdata *chr)
{
	if (!modRandomTacticsOn()) {
		return;
	}

	chr->hidden2 |= CHRH2FLAG_PUNISHMENT;
	chr->ailist = ailistFindById(GAILIST_IDLE);
	chr->aioffset = 0;
}

static Gfx *modRandomDebugStateCounts(Gfx *gdl, s32 left, s32 *y)
{
	static const struct {
		s32 role;
		const char *label;
	} rows[] = {
		{ TACTICS_PASSIVE, "Passive" },
		{ TACTICS_HOLD_POSITION, "Hold position" },
		{ TACTICS_SEEK, "Seeker" },
		{ TACTICS_ATTACKER, "Attacker" },
		{ TACTICS_DEFENDER, "Defender" },
		{ TACTICS_PRONE, "Prone" },
	};
	s32 counts[TACTICS_COUNT] = {0};

	for (s32 i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];
		s32 id = chr->chrnum & 1023;

		if (modRandomTacticsEligible(chr, NULL)
				&& (g_ModRandomTacticsModel[id] == chr->model || g_ModRandomPosts[id].model == chr->model)) {
			counts[g_ModRandomRole[id]]++;
		}
	}

	counts[TACTICS_PASSIVE] += counts[TACTICS_WAIT_FOR_SIGHT];

	for (s32 i = 0; i < ARRAYCOUNT(rows); i++) {
		char text[32];
		snprintf(text, sizeof(text), "%s  %d", rows[i].label, counts[rows[i].role]);
		gdl = modRandomDebugLine(gdl, left, *y, text, 0x9deaffff);
		*y += 7;
	}

	return gdl;
}

bool modRandomTacticsSeek(struct chrdata *chr)
{
	if (!chr || !modRandomTacticsOn() || !modRandomTacticsEligible(chr, NULL)) {
		return false;
	}

	s32 id = chr->chrnum & 1023;

	g_ModRandomPosts[id] = (struct modrandompost){
		.model = chr->model, .objective = MODRANDOM_POST_UNASSIGNED, .pad = -1, .facepad = -1,
	};
	g_ModRandomRole[id] = TACTICS_SEEK;
	g_ModRandomHoldUntil60[id] = 0;
	chr->ailist = ailistFindById(GAILIST_IDLE);
	chr->aioffset = 0;
	chr->aishotlist = -1;
	chr->aipunchdodgelist = -1;
	chr->aishootingatmelist = -1;
	chr->aidarkroomlist = -1;
	return true;
}

/**
 * How far (radians, 0..PI) the player is off the way a chr is facing. A guard
 * lying down cannot turn its body: the pose only lets the gun swing about 30
 * degrees either side, so it has to be facing the player to hit anything.
 */
#define MODRANDOM_PRONE_LIE_CONE 0.35f // may lie down when the player is this close to dead ahead (~20 degrees)
#define MODRANDOM_PRONE_UP_CONE  0.45f // gets up to turn once the player is further round than this (~26 degrees)

static f32 modRandomProneAngleOff(struct chrdata *chr, struct prop *playerprop)
{
	f32 d = atan2f(playerprop->pos.x - chr->prop->pos.x, playerprop->pos.z - chr->prop->pos.z) - chrGetInverseTheta(chr);

	while (d > M_PI) {
		d -= M_BADTAU;
	}

	while (d < -M_PI) {
		d += M_BADTAU;
	}

	return d < 0.0f ? -d : d;
}

// Perf/bug log for the floating prone guard: one line per event, at most 40 a run
static void modRandomProneLog(const char *what, struct chrdata *chr, f32 delta)
{
	static s32 count;

	if (count < 40) {
		count++;
		sysLogPrintf(LOG_NOTE, "prone: %s: chr %d at (%.0f %.0f %.0f) room %d, height over floor %.1f, action %d",
				what, chr->chrnum, chr->prop->pos.x, chr->prop->pos.y, chr->prop->pos.z,
				chr->prop->rooms[0], delta, chr->actiontype);
	}
}

/**
 * The lying-down pose is drawn for a chr whose feet are on the floor, so a
 * guard that stopped a little above it (mid-step on stairs or a slope, or
 * spawned on a lifted pad) hangs in the air once it lies down. Put it back on
 * the floor under it.
 */
static bool modRandomProneSettle(struct chrdata *chr)
{
	static s32 lastputdown[256];
	static u8 putdowns[256];
	struct prop *prop = chr->prop;
	struct coord query;
	RoomNum floorroom = -1;
	f32 ground;

	if (!prop || prop->rooms[0] < 0) return false;

	// mid-fall: the fall code is moving it; do not move it by hand
	if (chr->fallspeed.y != 0.0f || chr->fallpeakset) return true;

	query = prop->pos;
	query.y += MODRANDOM_SPAWNLIFT;
	ground = cdFindGroundInfoAtCyl(&query, chr->radius, prop->rooms, NULL, NULL, NULL, &floorroom, NULL, NULL);

	if (floorroom < 0 || ground < MODRANDOM_NOGROUND) {
		modRandomProneLog("no floor under prone chr", chr, 0.0f);
		return false;
	}

	if (prop->pos.y - ground > 3.0f && prop->pos.y - ground < 12.0f) {
		const s32 slot = chr->chrnum & 255;

		// A guard put down again and again is on a floor this probe does not
		// see (the game keeps lifting it back, as the log showed: a steady
		// 16 units over the probed floor). Its pose floats there: do not lie it
		// down on such a spot.
		if ((s32)g_Vars.lvframe60 - lastputdown[slot] < 120) {
			if (putdowns[slot] < 255) putdowns[slot]++;
		} else {
			putdowns[slot] = 1;
		}

		lastputdown[slot] = g_Vars.lvframe60;

		if (putdowns[slot] >= 3) {
			modRandomProneLog("floor keeps lifting the prone chr, stays standing", chr, prop->pos.y - ground);
			return false;
		}

		modRandomProneLog("prone chr above floor, put down", chr, prop->pos.y - ground);
		prop->pos.y = ground;
		chr->ground = ground;
		chr->manground = ground;
		chr->sumground = ground * (PAL ? 8.4175090789795f : 9.999998f);
	} else if (prop->pos.y - ground >= 12.0f || ground - prop->pos.y > 3.0f) {
		// (a guard more than a step off the probed floor is on a slope or stair
		// this probe misreads: putting it down would teleport it)
		// the floor found is nowhere near the guard (it stands on something this
		// probe does not see, or the floor under it moved): not a pose to hold
		modRandomProneLog("prone chr far from the floor found", chr, prop->pos.y - ground);
		return false;
	}

	return true;
}

/**
 * The lying-down pose is about two metres long, so the floor has to be level
 * along it: on stairs, a kerb or a ledge one end of the body hangs in the air
 * (or sinks into the floor) however well the middle is put on the ground.
 * Probes the floor along the way the guard faces, from behind its feet to past
 * its gun. A spot that is missing (no floor, a doorway into another room)
 * counts as not level.
 */
#define MODRANDOM_PRONE_FLAT_TOL 8.0f

static bool modRandomProneFlat(struct chrdata *chr)
{
	static const f32 offsets[] = { -100.0f, -65.0f, -30.0f, 30.0f, 65.0f, 100.0f, 135.0f };
	struct prop *prop = chr->prop;
	const f32 theta = chrGetInverseTheta(chr);
	const f32 fx = sinf(theta);
	const f32 fz = cosf(theta);
	struct coord query;
	RoomNum floorroom = -1;
	f32 base;
	u32 i;

	if (!prop || prop->rooms[0] < 0) return false;

	query = prop->pos;
	query.y += MODRANDOM_SPAWNLIFT;
	base = cdFindGroundInfoAtCyl(&query, 15.0f, prop->rooms, NULL, NULL, NULL, &floorroom, NULL, NULL);

	if (floorroom < 0 || base < MODRANDOM_NOGROUND) return false;

	// not on the floor this probe finds (a slope, stair or ramp it is walking down)
	if (prop->pos.y - base > 6.0f || base - prop->pos.y > 6.0f) return false;

	for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
		f32 ground;

		query.x = prop->pos.x + fx * offsets[i];
		query.z = prop->pos.z + fz * offsets[i];
		query.y = base + MODRANDOM_SPAWNLIFT;
		floorroom = -1;
		ground = cdFindGroundInfoAtCyl(&query, 15.0f, prop->rooms, NULL, NULL, NULL, &floorroom, NULL, NULL);

		if (floorroom < 0 || ground < MODRANDOM_NOGROUND
				|| ground - base > MODRANDOM_PRONE_FLAT_TOL || base - ground > MODRANDOM_PRONE_FLAT_TOL) {
			return false;
		}
	}

	return true;
}

/** Replace movement or idle poses between actions, without cutting off a burst. */
static bool modRandomTacticsCanGiveOrder(struct chrdata *chr)
{
	if (!chrIsReadyForOrders(chr) || (chr->hidden & (CHRHFLAG_FIRINGLEFT | CHRHFLAG_FIRINGRIGHT))) {
		return false;
	}

	s32 anim = modelGetAnimNum(chr->model);

	switch (chr->actiontype) {
	case ACT_STAND:
		return !chr->act_stand.prestand && anim != ANIM_SNIPING_GETUP;
	case ACT_INIT:
	case ACT_KNEEL:
	case ACT_GOPOS:
	case ACT_RUNPOS:
	case ACT_PATROL:
		return true;
	case ACT_ANIM:
	case ACT_ATTACKAMOUNT:
	case ACT_ROBOTATTACK:
		return chrIsStopped(chr);
	case ACT_ATTACK:
		if ((chr->hidden & CHRHFLAG_NEEDANIM) || anim == ANIM_SNIPING_GETDOWN) {
			return false;
		}
		// The prone pose persists after a burst. Its shot count marks the
		// boundary where a new order can take over.
		if (anim == ANIM_SNIPING_ONGROUND && !(chr->act_attack.flags & ATTACKFLAG_AIMONLY)) {
			return chr->act_attack.numshots >= chr->act_attack.maxshots;
		}
		return (chr->act_attack.flags & ATTACKFLAG_AIMONLY)
			&& modelGetCurAnimFrame(chr->model) >= modelGetAnimEndFrame(chr->model);
	default:
		return false;
	}
}

static void modRandomTacticsIdle(struct chrdata *chr)
{
	if (chr->ailist != ailistFindById(GAILIST_IDLE)) {
		chr->ailist = ailistFindById(GAILIST_IDLE);
		chr->aioffset = 0;
	}
}

/**
 * A prone enemy turns to face the player first, then lies down (the sniper's
 * pose), aims at the player and fires only when it can see them. It keeps this
 * for two minutes, but if the player gets round to its side (past what the
 * pose can aim at) it gets up, turns, and lies down again facing them.
 */
static void modRandomTacticsProneHold(struct chrdata *chr, struct prop *playerprop)
{
	const s32 id = chr->chrnum & 1023;
	const bool seen = chrHasLosToProp(chr, playerprop);
	const u8 mode = seen ? 2 : 1;
	const f32 off = modRandomProneAngleOff(chr, playerprop);

	// its own list would stand it up and send it at the player
	modRandomTacticsIdle(chr);

	if (!modRandomTacticsCanGiveOrder(chr)) {
		return;
	}

	// Still falling in (a reinforcement drops about 400 units, over 50 frames) or
	// not yet down on its floor: lying down now would pose it in mid-air, and
	// putting it down by hand fights the fall. Wait for the landing.
	if (g_ModRandomProneMode[id] == 0
			&& (chr->fallspeed.y != 0.0f || chr->fallpeakset || chr->manground - chr->ground > 3.0f)) {
		static s32 waitsince[1024];

		if (waitsince[id] == 0) {
			waitsince[id] = g_Vars.lvframe60 ? g_Vars.lvframe60 : 1;
		}

		// A landing takes about a second. A chr that is still "not down" after
		// two seconds is not falling: it fights standing, as a defender, and
		// does not stand idle for the rest of the mission.
		if (g_Vars.lvframe60 - waitsince[id] < TICKS(120)) {
			return;
		}

		waitsince[id] = 0;
		modRandomProneLog("never landed, stays standing", chr, chr->manground - chr->ground);
		g_ModRandomRole[id] = g_ModRandomHold[id] ? TACTICS_HOLD_POSITION : TACTICS_DEFENDER;
		g_ModRandomDefCover[id] = 1;
		return;
	}

	if (g_ModRandomProneMode[id] != 0) {
		// down already: stay while the player is within what the pose can aim at
		if (off > MODRANDOM_PRONE_UP_CONE) {
			chrTryStop(chr);
			g_ModRandomProneMode[id] = 0;
			return;
		}
	} else if (off > MODRANDOM_PRONE_LIE_CONE) {
		// standing: turn to face the player before going down
		chrFaceEntity(chr, ATTACKFLAG_AIMATBOND, 0);
		return;
	}

	if (g_ModRandomProneMode[id] == 0 && off > MODRANDOM_PRONE_LIE_CONE) {
		return;
	}

	if (g_ModRandomProneMode[id] == 0
			&& (chr->actiontype == ACT_GOPOS || chr->actiontype == ACT_RUNPOS || chr->actiontype == ACT_PATROL)) {
		// lying down while still walking slides it down a slope mid-pose: halt first
		chrTryStop(chr);
		return;
	}

	if (g_ModRandomProneMode[id] == 0 && !modRandomProneFlat(chr)) {
		// the floor here is not level (stairs, a ledge, a kerb): lying down
		// would leave part of it floating. Fights standing, as a defender.
		modRandomProneLog("floor not level, stays standing", chr, 0.0f);
		g_ModRandomRole[id] = g_ModRandomHold[id] ? TACTICS_HOLD_POSITION : TACTICS_DEFENDER;
		g_ModRandomDefCover[id] = 1;
		g_ModRandomProneMode[id] = 0;
		chrStopFiring(chr);
		chrStop(chr);
		return;
	}

	{
		static s32 lastlog[1024];
		static s32 logged;

		if (logged < 60 && g_Vars.lvframe60 - lastlog[id] > TICKS(600)) {
			lastlog[id] = g_Vars.lvframe60;
			logged++;
			sysLogPrintf(LOG_NOTE, "prone: state: chr %d mode %d seen %d action %d anim %d shots %d/%d flags 0x%x firing 0x%x",
					chr->chrnum, g_ModRandomProneMode[id], seen, chr->actiontype, (s32)modelGetAnimNum(chr->model),
					chr->act_attack.numshots, chr->act_attack.maxshots, chr->act_attack.flags,
					chr->hidden & (CHRHFLAG_FIRINGLEFT | CHRHFLAG_FIRINGRIGHT));
		}
	}

	if (g_ModRandomProneMode[id] != mode || chr->actiontype != ACT_ATTACK
			|| (seen && modelGetAnimNum(chr->model) == ANIM_SNIPING_ONGROUND
				&& chr->act_attack.numshots >= chr->act_attack.maxshots)) {
		// The target attack path accounts for the gun's sideways offset in
		// the prone pose and uses the normal player hit calculation.
		chr->target = playerprop - g_Vars.props;
		const u32 flags = ATTACKFLAG_AIMATTARGET | (seen ? 0 : ATTACKFLAG_AIMONLY);
		if (chrTryAttackLie(chr, flags, 0)) {
			g_ModRandomProneMode[id] = mode;
		}
	}

	if (g_ModRandomProneMode[id] != 0 && !modRandomProneSettle(chr)) {
		// it cannot be put on a floor: get up rather than hang in the air
		g_ModRandomRole[id] = g_ModRandomHold[id] ? TACTICS_HOLD_POSITION : TACTICS_DEFENDER;
		g_ModRandomDefCover[id] = 1;
		g_ModRandomProneMode[id] = 0;
		chrStopFiring(chr);
		chrStop(chr);
		chrTryStop(chr);
	}
}

/** Out of the prone pose and back to being an ordinary fighting guard. */
static void modRandomTacticsGetUp(struct chrdata *chr)
{
	chrTryStop(chr);
	chr->ailist = ailistFindById(GAILIST_ALERTED);
	chr->aioffset = 0;
	chr->sleep = 0;
}

/** Use the same cover orders for defenders and guards waiting to engage. */
static bool modRandomTacticsTakeCover(struct chrdata *chr)
{
	const s32 id = chr->chrnum & 1023;

	modRandomTacticsIdle(chr);

	if (g_ModRandomDefCover[id]) {
		return false;
	}

	if (!modRandomTacticsCanGiveOrder(chr)) {
		return true;
	}

	g_ModRandomDefCover[id] = 1;

	if (chrAssignCoverByCriteria(chr, COVERCRITERIA_0001 | COVERCRITERIA_DISTTOME | COVERCRITERIA_FORCENEWCOVER
			| COVERCRITERIA_0020 | COVERCRITERIA_1000 | COVERCRITERIA_ALLOWSOFT, 0) != -1) {
		chrGoToCover(chr, GOPOSFLAG_RUN);
	} else if (chr->actiontype == ACT_GOPOS || chr->actiontype == ACT_RUNPOS || chr->actiontype == ACT_PATROL) {
		chrTryStop(chr);
	}

	return true;
}

static void modRandomTacticsDefend(struct chrdata *chr, struct prop *target, s32 facepad, bool tracktarget)
{
	if ((chr->actiontype != ACT_STAND && chr->actiontype != ACT_KNEEL && chr->actiontype != ACT_ATTACK)
			|| !modRandomTacticsCanGiveOrder(chr)) {
		return;
	}

	u32 attackflags = target->type == PROPTYPE_PLAYER ? ATTACKFLAG_AIMATBOND : ATTACKFLAG_AIMATTARGET;

	if (chrHasLosToProp(chr, target)) {
		if (chr->actiontype != ACT_ATTACK || (chr->act_attack.flags & ATTACKFLAG_AIMONLY)) {
			if (chr->chrnum & 1) {
				chrTryAttackKneel(chr, attackflags, 0);
			} else {
				chrTryAttackStand(chr, attackflags, 0);
			}
		}
	} else if (chr->actiontype == ACT_ATTACK) {
		chrTryStop(chr);
	} else if (chr->actiontype == ACT_STAND) {
		if (facepad >= 0) {
			chrFaceEntity(chr, ATTACKFLAG_AIMATPAD, facepad);
		} else if (tracktarget) {
			chrFaceEntity(chr, attackflags, 0);
		}
	}
}

static bool modRandomPostIsOccupied(struct modrandompost *post, RoomNum entryroom, RoomNum room)
{
	for (s32 i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr->chrnum < 0) {
			continue;
		}

		struct modrandompost *other = &g_ModRandomPosts[chr->chrnum & 1023];

		if (other->model != post->model && other->model == chr->model && other->pad >= 0
				&& other->objective == post->objective && other->entryroom == entryroom
				&& other->room == room && modRandomTacticsEligible(chr, NULL)) {
			return true;
		}
	}

	return false;
}

static void modRandomFindPost(struct chrdata *chr, struct prop *player, struct prop *target,
		struct modrandompost *post, s32 oldpad)
{
	if (target == NULL || arrayIntersects(chr->prop->rooms, player->rooms)
			|| arrayIntersects(target->rooms, player->rooms)) {
		return;
	}

	struct waypoint *route[MODRANDOM_POST_ROUTE_MAX];
	struct waypoint *from = waypointFindClosestToPos(&player->pos, player->rooms);
	struct waypoint *to = waypointFindClosestToPos(&target->pos, target->rooms);
	RoomNum room = player->rooms[0];
	RoomNum entryroom = room;
	s32 entrypad = -1;

	// Stable tie-breaking makes guards choose the same approach for the same route.
	navSetSeed(1, 1);
	s32 count = navFindRoute(from, to, route, ARRAYCOUNT(route)) - 1;
	navSetSeed(0, 0);

	struct waypoint *destination = NULL;

	for (s32 i = 0; i < count && route[i] != NULL; i++) {
		struct pad pad;
		RoomNum padrooms[2] = { -1, -1 };
		padUnpack(route[i]->padnum, PADFIELD_POS | PADFIELD_ROOM, &pad);

		if (pad.room <= 0) {
			continue;
		}

		if (pad.room != room) {
			entryroom = room;
			room = pad.room;
			entrypad = i > 0 ? route[i - 1]->padnum : -1;
		}

		padrooms[0] = room;
		f32 dx = pad.pos.x - player->pos.x;
		f32 dz = pad.pos.z - player->pos.z;

		if (entrypad >= 0 && !arrayIntersects(padrooms, player->rooms)
				&& route[i]->padnum != oldpad
				&& dx * dx + dz * dz >= MODRANDOM_POST_STANDOFF * MODRANDOM_POST_STANDOFF) {
			destination = route[i];
			break;
		}
	}

	if (destination == NULL || modRandomPostIsOccupied(post, entryroom, room)) {
		return;
	}

	// A post is only useful if the guard can get there without crossing the player's room.
	from = waypointFindClosestToPos(&chr->prop->pos, chr->prop->rooms);
	navSetSeed(CHRNAVSEED(chr), CHRNAVSEED(chr));
	count = navFindRoute(from, destination, route, ARRAYCOUNT(route)) - 1;
	navSetSeed(0, 0);

	if (count <= 0 || route[count - 1] != destination) {
		return;
	}

	for (s32 i = 0; i < count; i++) {
		struct pad pad;
		RoomNum padrooms[2] = { -1, -1 };

		if (route[i] == NULL) {
			return;
		}

		padUnpack(route[i]->padnum, PADFIELD_ROOM, &pad);
		padrooms[0] = pad.room;

		if (pad.room <= 0 || arrayIntersects(padrooms, player->rooms)) {
			return;
		}
	}

	post->pad = destination->padnum;
	post->facepad = entrypad;
	post->entryroom = entryroom;
	post->room = room;
}

static bool modRandomRelocateHolder(struct chrdata *chr, struct prop *player, struct prop *objective)
{
	s32 id = chr->chrnum & 1023;
	u32 now = g_Vars.lvframe60;
	u32 *until = &g_ModRandomHoldUntil60[id];

	if (*until == 0 || (player->chr && !(player->chr->hidden & (CHRHFLAG_CLOAKED | CHRHFLAG_UNTARGETABLE))
			&& chrHasLosToProp(chr, player))) {
		*until = now + MODRANDOM_HOLD_LOST_SIGHT60;
		return false;
	}

	if (now < *until || !modRandomTacticsCanGiveOrder(chr)) {
		return false;
	}

	s32 oldpad = g_ModRandomPosts[id].model == chr->model ? g_ModRandomPosts[id].pad : -1;
	struct modrandompost post = {
		.model = chr->model, .objective = objective ? g_ModRandomCleared : -1, .pad = -1, .facepad = -1,
		.deadline = now + TICKS(MODRANDOM_POST_TRAVEL60),
	};
	modRandomFindPost(chr, player, objective, &post, oldpad);

	if (post.pad < 0 || chrGetDistanceToPad(chr, post.pad) <= MODRANDOM_POST_ARRIVAL) {
		// If the objective approach has no free post, advance to a waypoint
		// along the approach to the player, stopping before the player's room.
		post.pad = -1;
		post.facepad = -1;
		struct waypoint *route[MODRANDOM_POST_ROUTE_MAX];
		struct waypoint *from = waypointFindClosestToPos(&chr->prop->pos, chr->prop->rooms);
		struct waypoint *to = waypointFindClosestToPos(&player->pos, player->rooms);
		s32 count = navFindRoute(from, to, route, ARRAYCOUNT(route)) - 1;

		for (s32 i = 0; i < count && route[i]; i++) {
			s32 padnum = route[i]->padnum;
			struct pad pad;
			padUnpack(padnum, PADFIELD_ROOM, &pad);
			RoomNum rooms[2] = { pad.room, -1 };

			if (pad.room <= 0 || arrayIntersects(rooms, player->rooms)) {
				break;
			}

			if (padnum != oldpad && chrGetDistanceToPad(chr, padnum) > MODRANDOM_POST_ARRIVAL) {
				post.pad = padnum;
				post.room = pad.room;
				break;
			}
		}
	}

	if (post.pad < 0 || !modRandomTacticsSeek(chr)) {
		*until = now + MODRANDOM_HOLD_LOST_SIGHT60;
		return false;
	}

	// Drop the cover order before the seeker starts toward the new post.
	chrTryStop(chr);
	g_ModRandomPosts[id] = post;
	g_ModRandomHold[id] = 0;

	if (sysArgCheck("--chr-trace")) {
		sysLogPrintf(LOG_NOTE, "seeker: chr %d lost sight for 8 seconds and moved to seek pad %d", chr->chrnum, post.pad);
	}

	return true;
}

static void modRandomTickPost(struct chrdata *chr, struct prop *player, struct prop *target)
{
	s32 id = chr->chrnum & 1023;
	s32 objective = target ? g_ModRandomCleared : -1;
	struct modrandompost *post = &g_ModRandomPosts[id];
	u8 *role = &g_ModRandomRole[id];

	modRandomTacticsIdle(chr);

	if (post->objective != objective) {
		if (!modRandomTacticsCanGiveOrder(chr) || !chrTryStop(chr)) {
			return;
		}

		post->objective = objective;
		post->pad = -1;
		post->facepad = -1;
		modRandomFindPost(chr, player, target, post, -1);
		*role = post->pad >= 0 ? TACTICS_SEEK : TACTICS_HOLD_POSITION;
		g_ModRandomHoldUntil60[id] = 0;
		post->deadline = g_Vars.lvframe60 + TICKS(MODRANDOM_POST_TRAVEL60);

		if (sysArgCheck("--chr-trace")) {
			if (post->pad >= 0) {
				sysLogPrintf(LOG_NOTE, "seeker: chr %d objective %d assigned pad %d, approach %d -> %d",
						chr->chrnum, objective, post->pad, post->entryroom, post->room);
			} else {
				sysLogPrintf(LOG_NOTE, "seeker: chr %d objective %d holding locally in room %d",
						chr->chrnum, objective, chr->prop->rooms[0]);
			}
		}
	}

	if (*role == TACTICS_SEEK) {
		if (!modRandomTacticsCanGiveOrder(chr)) {
			return;
		}

		RoomNum rooms[2] = { post->room, -1 };
		bool arrived = arrayIntersects(rooms, chr->prop->rooms)
			&& chrGetDistanceToPad(chr, post->pad) <= MODRANDOM_POST_ARRIVAL;
		bool engage = chrGetSquaredDistanceToCoord(chr, &player->pos)
			<= MODRANDOM_SEEK_ENGAGE_DISTANCE * MODRANDOM_SEEK_ENGAGE_DISTANCE
			&& chrHasLosToProp(chr, player);

		if (!arrived && !engage && g_Vars.lvframe60 < post->deadline) {
			if (chr->actiontype == ACT_GOPOS || chrGoToPad(chr, post->pad, GOPOSFLAG_JOG)) {
				return;
			}
		}

		if (!chrTryStop(chr)) {
			return;
		}

		if (engage) {
			if (sysArgCheck("--chr-trace")) {
				sysLogPrintf(LOG_NOTE, "seeker: chr %d engaged a nearby player and joined the attacker queue", chr->chrnum);
			}

			// Leave the post assignment and join the normal combat rotation.
			post->model = NULL;
			post->pad = -1;
			*role = TACTICS_DEFENDER;
			g_ModRandomTacticsModel[id] = chr->model;
			g_ModRandomBorn60[id] = (s32)g_Vars.lvframe60 - MODRANDOM_PASSIVE60;
			g_ModRandomArrived[id] = 1;
			g_ModRandomHold[id] = 0;
			g_ModRandomDefCover[id] = 1;
			modRandomTacticsDefend(chr, player, -1, true);
			return;
		}

		if (sysArgCheck("--chr-trace")) {
			sysLogPrintf(LOG_NOTE, "seeker: chr %d %s post pad %d from room %d (%.0fcm away)",
					chr->chrnum, arrived ? "reached" : "could not reach", post->pad,
					chr->prop->rooms[0], chrGetDistanceToPad(chr, post->pad));
		}

		if (!arrived) {
			post->pad = -1;
			post->facepad = -1;
		}

		*role = TACTICS_HOLD_POSITION;
	}

	if (!modRandomRelocateHolder(chr, player, target)) {
		modRandomTacticsDefend(chr, player, post->facepad, false);
	}
}

static void modRandomDefendWarzoneAlly(struct chrdata *chr);

static void modRandomTickTactics(void)
{
	struct player *player = g_Vars.bond;
	struct chrdata *own = player && player->prop ? player->prop->chr : NULL;
	s32 count = 0;
	struct chrdata *seen[8];
	s32 numseen = 0;
	s32 i;

	if (!own || g_Vars.currentplayer != player || lvIsPaused() || g_InCutscene || player->isdead
			|| g_Vars.lvframenum < 30 || (g_Vars.lvframe60 + 3) % 30 >= g_Vars.lvupdate60) {
		return;
	}

	if (!modRandomTacticsOn()) {
		return;
	}

	struct chrdata *pool[g_NumChrSlots];
	f32 dist[g_NumChrSlots];

	s32 targetnum = modRandomGetObjectiveTarget();
	struct chrdata *target = targetnum >= 0 ? chrFindByLiteralId(targetnum) : NULL;
	struct prop *objective = target && !chrIsDead(target) ? target->prop : NULL;

	if (objective == NULL && g_ModRandomClimaxStarted && !g_ModRandomClimaxDone) {
		objective = g_ModRandomClimaxTerminal;
	}

	// the guards currently attacking: a passive guard that sees one is alerted
	for (i = 0; i < g_NumChrSlots && numseen < 8; i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr->chrnum >= 0 && chr->prop && g_ModRandomRole[chr->chrnum & 1023] == TACTICS_ATTACKER
				&& !chrIsDead(chr)) {
			seen[numseen++] = chr;
		}
	}

	for (i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr->chrnum < 0) {
			continue;
		}

		if (!modRandomTacticsEligible(chr, own)) {
			g_ModRandomRole[chr->chrnum & 1023] = TACTICS_NONE;
			g_ModRandomDefCover[chr->chrnum & 1023] = 0;
			g_ModRandomPosts[chr->chrnum & 1023].objective = MODRANDOM_POST_UNASSIGNED;
			g_ModRandomPosts[chr->chrnum & 1023].pad = -1;
			g_ModRandomArrived[chr->chrnum & 1023] = 0;
			g_ModRandomHold[chr->chrnum & 1023] = 0;
			g_ModRandomHoldUntil60[chr->chrnum & 1023] = 0;
			continue;
		}

		bool punishment = (chr->hidden2 & CHRH2FLAG_PUNISHMENT) != 0;

		if (modRandomTacticsWarzoneAlly(chr)) {
			g_ModRandomTacticsModel[chr->chrnum & 1023] = chr->model;
			g_ModRandomRole[chr->chrnum & 1023] = TACTICS_DEFENDER;
			modRandomDefendWarzoneAlly(chr);
			continue;
		}

		if (!punishment && g_ModRandomPosts[chr->chrnum & 1023].model == chr->model) {
			modRandomTickPost(chr, player->prop, objective);
			continue;
		}

		// a new arrival stays passive for half a second before its routine starts
		if (g_ModRandomTacticsModel[chr->chrnum & 1023] != chr->model) {
			g_ModRandomTacticsModel[chr->chrnum & 1023] = chr->model;
			g_ModRandomRole[chr->chrnum & 1023] = TACTICS_NONE;
			g_ModRandomBorn60[chr->chrnum & 1023] = g_Vars.lvframe60;
			g_ModRandomDefCover[chr->chrnum & 1023] = 0;
			g_ModRandomArrived[chr->chrnum & 1023] = 0;
			g_ModRandomHold[chr->chrnum & 1023] = 0;
			g_ModRandomHoldUntil60[chr->chrnum & 1023] = 0;
		}

		if (!punishment && g_Vars.lvframe60 - g_ModRandomBorn60[chr->chrnum & 1023] < MODRANDOM_PASSIVE60) {
			s32 id = chr->chrnum & 1023;

			if (g_ModRandomRole[id] == TACTICS_DEFENDER) {
				modRandomTacticsTakeCover(chr);
			} else {
				g_ModRandomRole[id] = TACTICS_PASSIVE;
			}
			continue;
		}

		if (g_ModRandomRole[chr->chrnum & 1023] == TACTICS_PRONE) {
			const s32 id = chr->chrnum & 1023;
			const bool hold = g_ModRandomHold[id] != 0;
			const f32 pdx = chr->prop->pos.x - player->prop->pos.x;
			const f32 pdz = chr->prop->pos.z - player->prop->pos.z;

			if (!modRandomTacticsCanGiveOrder(chr)) {
				continue;
			}

			if (!modRandomChrHasAutomaticWeapon(chr) || pdx * pdx + pdz * pdz < 400.0f * 400.0f) {
				// Get up for close combat, keeping the post if this guard has one.
				g_ModRandomRole[id] = hold ? TACTICS_HOLD_POSITION : TACTICS_NONE;
				g_ModRandomDefCover[id] = hold;
				g_ModRandomArrived[id] = !hold;
				g_ModRandomProneMode[id] = 0;
				modRandomTacticsGetUp(chr);
			} else if (g_Vars.lvframe60 >= (u32)g_ModRandomProneUntil[chr->chrnum & 1023]) {
				// up again, and a defender like any other
				g_ModRandomRole[id] = hold ? TACTICS_HOLD_POSITION : TACTICS_DEFENDER;
				g_ModRandomDefCover[id] = 1;
				g_ModRandomProneMode[id] = 0;
				chrTryStop(chr);
			} else {
				modRandomTacticsProneHold(chr, player->prop);
				continue;
			}
		}

		if (g_ModRandomRole[chr->chrnum & 1023] == TACTICS_HOLD_POSITION) {
			if (!modRandomRelocateHolder(chr, player->prop, objective) && !modRandomTacticsTakeCover(chr)) {
				modRandomTacticsDefend(chr, player->prop, -1, false);
			}
			continue;
		}

		{
			const f32 dx = chr->prop->pos.x - player->prop->pos.x;
			const f32 dz = chr->prop->pos.z - player->prop->pos.z;

			if (g_ModRandomRole[chr->chrnum & 1023] == TACTICS_WAIT_FOR_SIGHT) {
				if (!chrHasLosToProp(chr, player->prop)) {
					modRandomTacticsTakeCover(chr);
					continue;
				}

				// Keep the guard's cover orders if it is picked to defend.
				g_ModRandomRole[chr->chrnum & 1023] = TACTICS_DEFENDER;
				g_ModRandomArrived[chr->chrnum & 1023] = 1;
			}

			u8 role = g_ModRandomRole[chr->chrnum & 1023];
			if (punishment || role == TACTICS_ATTACKER || role == TACTICS_DEFENDER
					|| g_ModRandomArrived[chr->chrnum & 1023] || modRandomTacticsEngaged(chr)) {
				pool[count] = chr;
				dist[count] = sqrtf(dx * dx + dz * dz);
				count++;
			} else {
				s32 k;

				g_ModRandomRole[chr->chrnum & 1023] = TACTICS_PASSIVE;
				g_ModRandomDefCover[chr->chrnum & 1023] = 0;

				for (k = 0; k < numseen; k++) {
					const f32 ax = seen[k]->prop->pos.x - chr->prop->pos.x;
					const f32 az = seen[k]->prop->pos.z - chr->prop->pos.z;

					if (ax * ax + az * az < 2500.0f * 2500.0f && chrHasLosToProp(chr, seen[k]->prop)) {
						g_ModRandomRole[chr->chrnum & 1023] = TACTICS_WAIT_FOR_SIGHT;
						modRandomTacticsTakeCover(chr);
						break;
					}
				}
			}
		}
	}

	// the nearest enemies attack: whoever is already attacking is let off a
	// little distance, so two at the same range do not trade places every time
	for (i = 0; i < count; i++) {
		if (g_ModRandomRole[pool[i]->chrnum & 1023] == TACTICS_ATTACKER) {
			dist[i] -= 150.0f;
		}
	}

	{
		bool chosen[g_NumChrSlots];

		memset(chosen, 0, sizeof(chosen));

		for (s32 queue = 0; queue < 2; queue++) {
			for (s32 attackers = 0; attackers < MODRANDOM_MAX_ATTACKERS; attackers++) {
				s32 best = -1;

				for (i = 0; i < count; i++) {
					bool punishment = (pool[i]->hidden2 & CHRH2FLAG_PUNISHMENT) != 0;

					if (!chosen[i] && punishment == (queue == 1) && (best < 0 || dist[i] < dist[best])) {
						best = i;
					}
				}

				if (best < 0) {
					break;
				}

				chosen[best] = true;
			}
		}

		for (i = 0; i < count; i++) {
			struct chrdata *chr = pool[i];
			s32 id = chr->chrnum & 1023;
			u8 role = g_ModRandomRole[id];

			if (chosen[i]) {
				g_ModRandomRole[id] = TACTICS_ATTACKER;
				g_ModRandomDefCover[id] = 0;

				if (!modRandomTacticsCanGiveOrder(chr)) {
					continue;
				}

				bool waiting = chr->ailist == ailistFindById(GAILIST_IDLE);

				if ((chr->hidden2 & CHRH2FLAG_PUNISHMENT) && (role != TACTICS_ATTACKER || waiting)) {
					modRandomTacticsGetUp(chr);
					chr->alertness = 100;
					chr->target = player->prop - g_Vars.props;
					if (!chrHasLosToProp(chr, player->prop)) {
						chrGoToProp(chr, player->prop, GOPOSFLAG_RUN);
					}
					if (sysArgCheck("--chr-trace")) {
						sysLogPrintf(LOG_NOTE, "tactics: punishment guard %d took an attacker slot", chr->chrnum);
					}
				} else if (role == TACTICS_DEFENDER || waiting) {
					// back into the fight
					chr->ailist = ailistFindById(GAILIST_ALERTED);
					chr->aioffset = 0;
					chr->sleep = 0;
				}
			} else if (g_ModRandomRole[chr->chrnum & 1023] != TACTICS_DEFENDER) {
				g_ModRandomRole[chr->chrnum & 1023] = TACTICS_NONE; // set below
			}
		}
	}

	// the others hold
	for (i = 0; i < count; i++) {
		struct chrdata *chr = pool[i];

		if (g_ModRandomRole[chr->chrnum & 1023] == TACTICS_ATTACKER) {
			continue;
		}

		if (g_ModRandomRole[chr->chrnum & 1023] != TACTICS_DEFENDER) {
			s32 id = chr->chrnum & 1023;
			bool punishment = (chr->hidden2 & CHRH2FLAG_PUNISHMENT) != 0;
			g_ModRandomHold[id] = !punishment && rngRandom() % MODRANDOM_HOLD_CHANCE == 0;
			g_ModRandomHoldUntil60[id] = 0;
			g_ModRandomRole[id] = g_ModRandomHold[id] ? TACTICS_HOLD_POSITION : TACTICS_DEFENDER;
			g_ModRandomDefCover[id] = 0;

			if (!punishment && rngRandom() % MODRANDOM_PRONE_CHANCE == 0 && modRandomChrHasAutomaticWeapon(chr)) {
				// this one lies down instead, for two minutes
				g_ModRandomRole[chr->chrnum & 1023] = TACTICS_PRONE;
				modRandomTacticsIdle(chr);
				g_ModRandomProneUntil[chr->chrnum & 1023] = g_Vars.lvframe60 + MODRANDOM_PRONE_HOLD60;
				g_ModRandomProneMode[chr->chrnum & 1023] = 0;
				continue;
			}
		}

		if (!modRandomTacticsTakeCover(chr)) {
			modRandomTacticsDefend(chr, player->prop, -1, g_ModRandomRole[chr->chrnum & 1023] == TACTICS_DEFENDER);
		}
	}
}

/** The hit flinch comes first; once the guard can take an order it puts its hands up. */
static void modRandomTickSurrenders(void)
{
	s32 i, j;

	for (i = 0; i < MODRANDOM_MAX_SURRENDERS; i++) {
		if (g_ModRandomSurrenders[i].chrnum <= 0) {
			continue;
		}

		if (g_ModRandomSurrenders[i].until60 < (s32)g_Vars.lvframe60) {
			g_ModRandomSurrenders[i].chrnum = 0;
			continue;
		}

		for (j = 0; j < g_NumChrSlots; j++) {
			struct chrdata *chr = &g_ChrSlots[j];

			if (chr->chrnum != g_ModRandomSurrenders[i].chrnum - 1 || !chr->prop || !chr->model) {
				continue;
			}

			if (chrIsDead(chr)) {
				g_ModRandomSurrenders[i].chrnum = 0;
			} else if (chrTrySurrender(chr)) {
				g_ModRandomSurrenders[i].chrnum = 0;
			}

			break;
		}
	}
}

bool modRandomChrExplosionImmune(struct chrdata *chr)
{
	return chr && g_ModRandomTerroristChrnum >= 0 && chr->chrnum == g_ModRandomTerroristChrnum
		&& chr->model == g_ModRandomTerroristModel;
}

static void modRandomEventReset(void)
{
	memset(g_ModRandomSurrenders, 0, sizeof(g_ModRandomSurrenders));
	memset(g_ModRandomSurrendered, 0, sizeof(g_ModRandomSurrendered));
	memset(g_ModRandomRole, 0, sizeof(g_ModRandomRole));
	memset(g_ModRandomDefCover, 0, sizeof(g_ModRandomDefCover));
	memset(g_ModRandomPosts, 0, sizeof(g_ModRandomPosts));
	memset(g_ModRandomProneUntil, 0, sizeof(g_ModRandomProneUntil));
	memset(g_ModRandomProneMode, 0, sizeof(g_ModRandomProneMode));
	memset(g_ModRandomArrived, 0, sizeof(g_ModRandomArrived));
	memset(g_ModRandomHold, 0, sizeof(g_ModRandomHold));
	memset(g_ModRandomHoldUntil60, 0, sizeof(g_ModRandomHoldUntil60));
	memset(g_ModRandomTacticsModel, 0, sizeof(g_ModRandomTacticsModel));
	memset(g_ModRandomBorn60, 0, sizeof(g_ModRandomBorn60));
	modRandomDebugReset();
	g_ModRandomAssassinsUntil = 0;
	memset(&g_ModRandomWarzone, 0, sizeof(g_ModRandomWarzone));
	g_ModRandomEventKind = MODEVENT_NONE;
	g_ModRandomInvaderKills = 0;
	g_ModRandomSwarmKills = 0;
	g_ModRandomNumInvaders = 0;
	g_ModRandomEventChoice = 0;
#ifndef PLATFORM_N64
	g_ModRandomEventNotice240 = 0;
	g_ModRandomEventNoticeDelay240 = 0;
	g_ModRandomEventNoticeFramePending = false;
	g_ModRandomEventNoticeEnding = false;
	g_ModRandomEventEndQueued = false;
#endif
	g_ModRandomAlienWarActive = false;
	g_ModRandomAlienWarOngoing = false;
	g_ModRandomAlienBossesSpawned = false;
	g_ModRandomAlienElvis = g_ModRandomAlienKing = -1;
	g_ModRandomAlienFaction = g_ModRandomAlienWinner = 0;
	g_ModRandomAlienPlayerTeam = 0;
	g_ModRandomAlienNextMaian60 = g_ModRandomAlienNextSkedar60 = g_ModRandomAlienNextWinner60 = 0;
	g_ModRandomAlienAllyWanted = g_ModRandomAlienAllyGot = g_ModRandomAlienAllyTries = 0;
	g_ModRandomAlienAllyFaction = 0;
	g_ModRandomEventLeft = g_ModRandomEventArrived = g_ModRandomEventTries = 0;
	g_ModRandomEventNext60 = 0;
	g_ModRandomEventBase = -1;
	g_ModRandomEventFired = false;
	g_ModRandomPurgeOn = false;
	g_ModRandomExplosiveOn = false;
	memset(g_ModRandomBlasts, 0, sizeof(g_ModRandomBlasts));
	g_ModRandomSwarmOn = false;
	g_ModRandomEventAfter = 0;
	g_ModRandomEventDone = 0;
	g_ModRandomTerroristChrnum = -1;
	g_ModRandomTerroristModel = NULL;
}

static s32 modRandomEventCountWaypoints(void)
{
	struct waypoint *waypoints = g_StageSetup.waypoints;

	if (!waypoints) {
		return 0;
	}

	if (g_ModRandomCivilianWaypoints < 0) {
		g_ModRandomCivilianWaypoints = 0;
		while (waypoints[g_ModRandomCivilianWaypoints].padnum >= 0) g_ModRandomCivilianWaypoints++;
	}

	return g_ModRandomCivilianWaypoints;
}

/** A waypoint in a room the player has reached the way to, a good way from them. */
static bool modRandomEventWaypointOk(s32 w)
{
	struct waypoint *waypoints = g_StageSetup.waypoints;
	struct pad pad;
	s32 room;
	f32 dx, dz;

	if (w < 0 || w >= g_ModRandomCivilianWaypoints || !waypoints[w].neighbours || waypoints[w].neighbours[0] == -1) {
		return false;
	}

	room = modRandomPadRoom(waypoints[w].padnum);

	if (room <= 0 || room >= g_Vars.roomcount || (g_ModRandomReached && !g_ModRandomReached[room])) {
		return false;
	}

	padUnpack(waypoints[w].padnum, PADFIELD_POS, &pad);
	dx = pad.pos.x - g_Vars.bond->prop->pos.x;
	dz = pad.pos.z - g_Vars.bond->prop->pos.z;

	return dx * dx + dz * dz > 1200.0f * 1200.0f;
}

/** A waypoint to arrive at: the squad's base, or one of its neighbours' */
static s32 modRandomEventPickWaypoint(bool squad)
{
	struct waypoint *waypoints = g_StageSetup.waypoints;
	s32 attempt;

	for (attempt = 0; attempt < 48; attempt++) {
		s32 w;

		if (squad && g_ModRandomEventBase >= 0) {
			// the base, or a step or two out from it
			s32 *nb = waypoints[g_ModRandomEventBase].neighbours;
			s32 count = 0;

			w = g_ModRandomEventBase;

			while (nb && nb[count] != -1 && count < 8) count++;

			if (count > 0 && (rngRandom() & 3) != 0) {
				w = nb[rngRandom() % count] & 0x3fffffff;

				if (w >= g_ModRandomCivilianWaypoints || !waypoints[w].neighbours || waypoints[w].neighbours[0] == -1) {
					continue;
				}

				if ((rngRandom() & 1) == 0) {
					s32 *nb2 = waypoints[w].neighbours;
					s32 count2 = 0;

					while (nb2[count2] != -1 && count2 < 8) count2++;

					if (count2 > 0) {
						s32 w2 = nb2[rngRandom() % count2] & 0x3fffffff;

						if (w2 < g_ModRandomCivilianWaypoints && waypoints[w2].neighbours && waypoints[w2].neighbours[0] != -1) {
							w = w2;
						}
					}
				}
			}

			return w;
		}

		w = rngRandom() % g_ModRandomCivilianWaypoints;

		if (modRandomEventWaypointOk(w)) {
			if (squad) {
				g_ModRandomEventBase = w;
			}

			return w;
		}
	}

	return -1;
}

#ifndef PLATFORM_N64
/**
 * A Spawn Customizer config's Hunter spawn to arrive at: a room the player has
 * reached the way to, a good way from them. A squad shares the one it first
 * took (g_ModRandomEventBase holds its index here), a step out from it now
 * and then. -1 when none qualifies.
 */
static s32 modRandomEventConfigPad(bool squad)
{
	const s32 n = modSpawnConfigCount(MODSPAWN_HUNTER);
	s32 attempt;

	// (A mission whose doors are shut at the start - the Perfect Dark ones - can
	// have few rooms the walk reaches, and none of them hold a Hunter spawn: the
	// second half of the attempts then do without that test rather than send
	// no wave at all.)
	for (attempt = 0; attempt < 48 && n > 0; attempt++) {
		s32 idx = rngRandom() % n;
		s32 padnum;
		struct pad pad;
		s32 room;
		f32 dx, dz;

		if (squad && g_ModRandomEventBase >= 0 && g_ModRandomEventBase < n && (rngRandom() & 3) != 0) {
			idx = g_ModRandomEventBase;
		}

		padnum = modSpawnConfigPad(MODSPAWN_HUNTER, idx);
		room = modRandomPadRoom(padnum);

		if (room <= 0 || room >= g_Vars.roomcount
				|| (attempt < 24 && g_ModRandomReached && !g_ModRandomReached[room])) {
			continue;
		}

		padUnpack(padnum, PADFIELD_POS, &pad);
		dx = pad.pos.x - g_Vars.bond->prop->pos.x;
		dz = pad.pos.z - g_Vars.bond->prop->pos.z;

		// (A small mission - Defection - has every Hunter spawn within 1200 of
		// the player: the later attempts then settle for a shorter distance
		// rather than send no wave at all.)
		{
			f32 mindist = attempt < 24 ? 1200.0f : (attempt < 36 ? 500.0f : 250.0f);

			if (dx * dx + dz * dz <= mindist * mindist) {
				continue;
			}
		}

		if (squad && g_ModRandomEventBase < 0) {
			g_ModRandomEventBase = idx;
		}

		return padnum;
	}

	return -1;
}
#endif

#ifndef PLATFORM_N64
/** For the Spawn Customizer's self-test: where an event would arrive, which must be a config Hunter spawn (or -1). */
s32 modRandomTestEventSpot(bool squad)
{
	static bool told = false;
	s32 pad = modRandomEventConfigPad(squad);

	if (pad < 0 && !told) {
		const s32 n = modSpawnConfigCount(MODSPAWN_HUNTER);
		s32 i;

		told = true;
		sysLogPrintf(0, "spawnconfig: no event spot among %d Hunter spawns; the player is at %d,%d,%d", n,
				(s32)g_Vars.bond->prop->pos.x, (s32)g_Vars.bond->prop->pos.y, (s32)g_Vars.bond->prop->pos.z);

		for (i = 0; i < n && i < 12; i++) {
			struct pad p;
			s32 padnum = modSpawnConfigPad(MODSPAWN_HUNTER, i);

			padUnpack(padnum, PADFIELD_POS, &p);
			sysLogPrintf(0, "spawnconfig:   hunter %d: pad %d room %d at %d,%d,%d reached %d", i, padnum,
					modRandomPadRoom(padnum), (s32)p.pos.x, (s32)p.pos.y, (s32)p.pos.z,
					g_ModRandomReached && modRandomPadRoom(padnum) > 0 && modRandomPadRoom(padnum) < g_Vars.roomcount
						? g_ModRandomReached[modRandomPadRoom(padnum)] : -1);
		}
	}

	return pad;
}
#endif

static void modRandomMakeMaianInvader(struct chrdata *chr)
{
	chr->team = MODEVENT_TEAM_MAIAN;
	if (!modRandomIsInvader(chr)) g_ModRandomInvaders[g_ModRandomNumInvaders++ % MODRANDOM_MAXINVADERS] = chr->chrnum;
	chr->hidden2 &= ~CHRH2FLAG_BLUESIGHT;
}

static bool modRandomEventSpawnOne(s32 kind, s32 index)
{
	struct waypoint *waypoints = g_StageSetup.waypoints;
	const bool squad = kind == MODEVENT_MAIANS;
	struct prop *prop = NULL;
	struct chrdata *chr;
	s32 body, head, team, tries;

	switch (kind) {
	case MODEVENT_MAIANS:
		body = BODY_MAIAN_SOLDIER;
		head = MOD_HEADNUM(HEAD_MAIAN_S);
		team = MODEVENT_TEAM_MAIAN;
		break;
	case MODEVENT_SKEDARS:
		body = BODY_SKEDAR;
		head = HEAD_DARK_COMBAT;
		team = MODEVENT_TEAM_MAIAN; // one swarm, one team: they leave each other alone
		break;
	case MODEVENT_ALIEN_WAR:
		if (index == 1) {
			body = BODY_MAIAN_SOLDIER;
			head = MOD_HEADNUM(HEAD_MAIAN_S);
			team = MODEVENT_TEAM_MAIAN;
		} else {
			body = BODY_SKEDAR;
			head = HEAD_DARK_COMBAT;
			team = MODEVENT_TEAM_TERROR;
		}
		break;
	default:
		body = BODY_ALASKAN_GUARD;
		head = HEAD_RANDOM;
		team = MODEVENT_TEAM_TERROR;
		break;
	}

	for (tries = 0; tries < 8 && !prop; tries++) {
		s32 padnum;
#ifndef PLATFORM_N64
		if (modSpawnConfigCount(MODSPAWN_HUNTER) > 0) {
			padnum = modRandomEventConfigPad(squad);
		} else
#endif
		{
			const s32 count = modRandomEventCountWaypoints();
			s32 w = count > 0 ? modRandomEventPickWaypoint(squad) : -1;

			// Small maps may have no waypoint that meets the usual distance cutoff.
			if (w < 0 && count > 0 && kind == MODEVENT_ALIEN_WAR) {
				w = rngRandom() % count;
			}

			padnum = w < 0 ? -1 : waypoints[w].padnum;
		}

		if (padnum < 0) {
			continue;
		}

		prop = modRandomSpawnRoundNpc(padnum, body, head, team);

		if (prop && !prop->chr) {
			prop = NULL;
		}
	}

	if (!prop) {
		return false;
	}

	chr = prop->chr;
	chr->team = team;
	if (kind == MODEVENT_ALIEN_WAR && !g_ModRandomAlienWarActive) {
		const s32 faction = index == 1 ? 1 : 2;
		chr->team = faction == g_ModRandomAlienWinner && faction == g_ModRandomAlienFaction
			? TEAM_ALLY : faction == 1 ? MODEVENT_TEAM_MAIAN : MODEVENT_TEAM_TERROR;
	}
	chr->flags |= CHRFLAG0_CAN_HEAR_ALARMS | CHRFLAG0_AIVSAI;
	chr->chrflags &= ~(CHRCFLAG_INVINCIBLE | CHRCFLAG_HIDDEN);
	chr->alertness = 90;
	chr->target = -1;
	// roams from pad to pad and takes on whoever it sees (the search list)
	chr->ailist = ailistFindById(kind == MODEVENT_ALIEN_WAR ? GAILIST_UNALERTED : GAILIST_SEARCH_FOR_PLAYER);
	chr->aioffset = 0;
	chr->aireturnlist = -1;
	chr->aishotlist = -1;

	g_ModRandomEventGrant = true;

	switch (kind) {
	case MODEVENT_SKEDARS:
		{
			// the swarm's weapons: one of them has the Slayer, the rest a
			// Mauler, a Reaper or nothing but claws
			static const s32 skedarguns[] = { WEAPON_MAULER, WEAPON_REAPER, WEAPON_NONE };
			const s32 gun = index == g_ModRandomSkedarSlayer ? WEAPON_SLAYER : skedarguns[rngRandom() % ARRAYCOUNT(skedarguns)];

			if (gun != WEAPON_NONE) {
				chrGiveWeaponWithAutoModel(chr, gun, 0);
			}
		}
		break;
	case MODEVENT_MAIANS:
		modRandomMakeMaianInvader(chr);
		// every one of them carries a Callisto NTG
		chrGiveWeaponWithAutoModel(chr, WEAPON_CALLISTO, 0);
		break;
	case MODEVENT_ALIEN_WAR:
		if (index == 1) chrGiveWeaponWithAutoModel(chr, WEAPON_CALLISTO, 0);
		else chrGiveWeaponWithAutoModel(chr, WEAPON_REAPER, 0);
		break;
	default:
		chrGiveWeapon(chr, MODEL_CHRDEVASTATOR, WEAPON_DEVASTATOR, 0);
		g_ModRandomTerroristChrnum = chr->chrnum;
		g_ModRandomTerroristModel = chr->model;
		break;
	}

	g_ModRandomEventGrant = false;
	modRandomDebugSpawn(chr, MODDBG_EVENT);

	return true;
}

static s32 modRandomAlienSpawnBoss(s32 padnum, bool elvis)
{
	struct prop *prop;
	s32 i;
	const s32 body = elvis ? BODY_ELVIS1 : BODY_SKEDARKING;
	const s32 head = elvis ? HEAD_ELVIS : HEAD_DARK_COMBAT;
	const u8 team = elvis ? MODEVENT_TEAM_MAIAN : MODEVENT_TEAM_TERROR;
	struct chrdata *hidden[MODRANDOM_MAXOBJECTIVES];
	u32 oldflags[MODRANDOM_MAXOBJECTIVES];
	struct pad pad;
	s32 n = 0;
	padUnpack(padnum, PADFIELD_POS, &pad);
	for (i = 0; i < g_ModRandomNumTargets && i < MODRANDOM_MAXOBJECTIVES; i++) {
		struct chrdata *occupant = chrFindByLiteralId(g_ModRandomTargets[i].chrnum);
		if (occupant && occupant->prop && fabsf(occupant->prop->pos.y - pad.pos.y) < 160.0f
				&& (occupant->prop->pos.x - pad.pos.x) * (occupant->prop->pos.x - pad.pos.x)
				+ (occupant->prop->pos.z - pad.pos.z) * (occupant->prop->pos.z - pad.pos.z) < 10000.0f) {
			hidden[n] = occupant; oldflags[n++] = occupant->chrflags;
			occupant->chrflags |= CHRCFLAG_HIDDEN;
		}
	}
	prop = modRandomSpawnRoundNpc(padnum, body, head, team);
	while (n > 0) { n--; hidden[n]->chrflags = oldflags[n]; }
	if (!prop || !prop->chr) return -1;

	struct chrdata *chr = prop->chr;
	chr->flags |= CHRFLAG0_AIVSAI | CHRFLAG0_CAN_HEAR_ALARMS;
	chr->alertness = 90;
	chr->target = -1;
	chrSetShield(chr, 8.0f);
	if (!elvis) chrSetMaxDamage(chr, chrGetMaxDamage(chr) * 4.0f);
	g_ModRandomEventGrant = true;
	chrGiveWeaponWithAutoModel(chr, elvis ? WEAPON_CALLISTO : WEAPON_REAPER, 0);
	g_ModRandomEventGrant = false;
	return chr->chrnum;
}

static s32 modRandomAlienLeader(void)
{
	s32 pads[64];
	struct coord positions[ARRAYCOUNT(pads)];
	s32 count = 0, i, j, tier;
	s32 limit = 0;
	struct waypoint *waypoints = g_StageSetup.waypoints;
	bool elvis = g_ModRandomAlienFaction == 2;

	// Prefer objective pads, then use any other valid spawner.
	for (tier = 0; tier < 4; tier++) {
		switch (tier) {
#ifndef PLATFORM_N64
		case 0: limit = modSpawnConfigCount(MODSPAWN_OBJECTIVE); break;
		case 2: limit = modSpawnConfigCount(MODSPAWN_HUNTER); break;
#endif
		case 1: limit = g_ModRandomNumTargets; break;
		case 3: limit = modRandomEventCountWaypoints(); break;
		default: limit = 0; break;
		}
		for (i = 0; i < limit && count < ARRAYCOUNT(pads); i++) {
			struct pad pad;
			s32 padnum;
#ifndef PLATFORM_N64
			if (tier == 0 || tier == 2) {
				padnum = modSpawnConfigPad(tier == 0 ? MODSPAWN_OBJECTIVE : MODSPAWN_HUNTER, i);
			} else
#endif
			{
				padnum = tier == 1 ? g_ModRandomTargets[i].padnum : waypoints[i].padnum;
			}
			if (padnum < 0 || padnum == g_ModRandomSpawnPad || !modRandomPadCanSpawn(padnum)) continue;
			for (j = 0; j < count && pads[j] != padnum; j++) {}
			if (j < count) continue;
			padUnpack(padnum, PADFIELD_POS, &pad);
			positions[count] = pad.pos;
			pads[count++] = padnum;
		}

		// Large alien waves can occupy the first several pads. Try candidates in
		// a changing order so a blocked pad cannot starve the leader indefinitely.
		for (i = 0; i < count; i++) {
			s32 pick = (i + (g_Vars.lvframe60 / TICKS(30))) % count;
			s32 chrnum = modRandomAlienSpawnBoss(pads[pick], elvis);
			if (chrnum < 0) continue;
			if (elvis) g_ModRandomAlienElvis = chrnum;
			else g_ModRandomAlienKing = chrnum;
			hudmsgCreateWithFlags(elvis ? "Kill Elvis!\n" : "Kill the Skedar King!\n",
				HUDMSGTYPE_EVENT, HUDMSGFLAG_DELAY | HUDMSGFLAG_ALLOWDUPES | HUDMSGFLAG_OBJECTIVE);
			rebuildTeams();
			rebuildSquadrons();
			return chrnum;
		}
		// Keep untried candidates and make room for the next fallback pool.
		for (i = 0, j = 0; i < count; i++) {
			if (pads[i] < 0) continue;
			pads[j] = pads[i];
			positions[j++] = positions[i];
		}
		count = j;
	}
	return -1;
}

// Level time is measured in 60 Hz ticks. Try again soon if a pad is busy.
#define MODRANDOM_ALIEN_SPAWN_INTERVAL TICKS(3 * 60)
#define MODRANDOM_ALIEN_SPAWN_RETRY TICKS(30)

static void modRandomAlienWarFinish(s32 defeated, s32 playernum)
{
	s32 i;
	if (!g_ModRandomAlienWarActive) return;
	g_ModRandomAlienWarActive = false;
	g_ModRandomAlienWinner = defeated == 1 ? 2 : 1;
	g_ModRandomEventKind = MODEVENT_NONE;
	for (i = 0; i < chrsGetNumSlots(); i++) {
		struct chrdata *chr = &g_ChrSlots[i];
		if (chr->chrnum < 0 || !chr->prop || !chr->model || chrIsDead(chr)) continue;
		if (chr->team == MODEVENT_TEAM_MAIAN || chr->bodynum == BODY_MAIAN_SOLDIER || chr->bodynum == BODY_ELVIS1) {
			chr->team = g_ModRandomAlienWinner == 1 && g_ModRandomAlienFaction == 1 ? TEAM_ALLY
				: g_ModRandomAlienWinner == 1 ? MODEVENT_TEAM_MAIAN : TEAM_NONCOMBAT;
		} else if (chr->team == MODEVENT_TEAM_TERROR || chr->bodynum == BODY_SKEDAR || chr->bodynum == BODY_SKEDARKING) {
			chr->team = g_ModRandomAlienWinner == 2 && g_ModRandomAlienFaction == 2 ? TEAM_ALLY
				: g_ModRandomAlienWinner == 2 ? MODEVENT_TEAM_TERROR : TEAM_NONCOMBAT;
		}
	}
	if (g_Vars.bond && g_Vars.bond->prop && g_Vars.bond->prop->chr) {
		g_Vars.bond->prop->chr->team = g_ModRandomAlienPlayerTeam;
		if (g_ModRandomAlienFaction == 0 || g_ModRandomAlienFaction == g_ModRandomAlienWinner) {
			g_Vars.bond->prop->chr->team |= g_ModRandomAlienWinner == 1 ? MODEVENT_TEAM_MAIAN : MODEVENT_TEAM_TERROR;
		}
	}
	if (playernum == g_Vars.bondplayernum && g_ModRandomAlienFaction == g_ModRandomAlienWinner) {
		modCareerAlienWarReward(g_ModRandomAlienWinner == 1);
	}
	rebuildTeams(); rebuildSquadrons();
	// Anyone who is no longer the player's enemy stops attacking them: the war
	// left Maians on the "search for the player" list with the player as target
	// (and mid-burst), which kept them firing at a player they cannot hurt.
	if (g_Vars.bond && g_Vars.bond->prop && g_Vars.bond->prop->chr) {
		struct chrdata *pchr = g_Vars.bond->prop->chr;
		for (i = 0; i < chrsGetNumSlots(); i++) {
			struct chrdata *chr = &g_ChrSlots[i];
			if (chr->chrnum < 0 || !chr->prop || !chr->model || chr == pchr || chrIsDead(chr)) continue;
			if (chr->team == TEAM_NONCOMBAT || chr->team == TEAM_ALLY || chrCompareTeams(chr, pchr, COMPARE_FRIENDS)) {
				if (chr->target == pchr->chrnum || chr->actiontype == ACT_ATTACK) {
					chrStopFiring(chr);
					chrStop(chr);
				}
				chr->target = -1;
				chr->alertness = 0;
				chr->ailist = ailistFindById(GAILIST_UNALERTED);
				chr->aioffset = 0;
				chr->aireturnlist = -1;
				chr->aishotlist = -1;
			}
		}
	}
	g_ModRandomAlienNextWinner60 = g_Vars.lvframe60 + MODRANDOM_ALIEN_SPAWN_INTERVAL;
	hudmsgCreateWithFlags("Alien War over! Objective and guard reinforcements resumed.\n",
		HUDMSGTYPE_EVENT, HUDMSGFLAG_DELAY | HUDMSGFLAG_ALLOWDUPES);
}

static void modRandomTickAlienWave(s32 faction, s32 *next60)
{
	if (g_Vars.lvframe60 < *next60) return;

	if (modRandomEventSpawnOne(MODEVENT_ALIEN_WAR, faction)) {
		*next60 = g_Vars.lvframe60 + MODRANDOM_ALIEN_SPAWN_INTERVAL;
		rebuildTeams();
		rebuildSquadrons();
	} else {
		*next60 = g_Vars.lvframe60 + MODRANDOM_ALIEN_SPAWN_RETRY;
	}
}

static void modRandomTickAlienWar(void)
{
	if (!g_ModRandomAlienWarOngoing || g_ModRandomRunOver || !g_Vars.bond || !g_Vars.bond->prop
			|| g_Vars.bond->isdead || lvIsPaused() || g_InCutscene || g_Vars.currentplayer != g_Vars.bond) return;

	if (g_ModRandomAlienWarActive) {
		if (g_ModRandomAlienFaction != 0 && !g_ModRandomAlienBossesSpawned && g_Vars.lvframe60 >= g_ModRandomAlienNextBoss60) {
			{
				s32 leader = modRandomAlienLeader();
				g_ModRandomAlienBossesSpawned = leader >= 0;
			if (!g_ModRandomAlienBossesSpawned) {
				sysLogPrintf(0, "randomizer: Alien War leader spawn failed, faction %d", g_ModRandomAlienFaction);
				g_ModRandomAlienNextBoss60 = g_Vars.lvframe60 + MODRANDOM_ALIEN_SPAWN_RETRY;
			}
			}
		}
		if (g_ModRandomAlienInitialMaians > 0 && modRandomEventSpawnOne(MODEVENT_ALIEN_WAR, 1)) g_ModRandomAlienInitialMaians--;
		if (g_ModRandomAlienInitialSkedars > 0 && modRandomEventSpawnOne(MODEVENT_ALIEN_WAR, 2)) g_ModRandomAlienInitialSkedars--;
		modRandomTickAlienWave(1, &g_ModRandomAlienNextMaian60);
		modRandomTickAlienWave(2, &g_ModRandomAlienNextSkedar60);
	} else if (g_ModRandomAlienWinner) {
		modRandomTickAlienWave(g_ModRandomAlienWinner, &g_ModRandomAlienNextWinner60);
	}
}

s32 modRandomGetAlienLeader(void)
{
	if (!g_ModRandomAlienWarActive || !g_ModRandomAlienBossesSpawned) return -1;
	return g_ModRandomAlienFaction == 1 ? g_ModRandomAlienKing : g_ModRandomAlienElvis;
}

const char *modRandomEventName(s32 kind)
{
	static const char *const names[MODEVENT_COUNT] = {
		[MODEVENT_MAIANS] = "Maian invasion",
		[MODEVENT_EXPLOSIVE] = "Explosive exit",
		[MODEVENT_SKEDARS] = "Skedar swarm",
		[MODEVENT_PURGE] = "Purge the innocent",
		[MODEVENT_ALIEN_WAR] = "Alien War!",
		[MODEVENT_ASSASSINS] = "Invisible assassins",
		[MODEVENT_WARZONE] = "Warzone!",
		[MODEVENT_BLOND] = "Mr Blonde's Revenge",
	};

	return kind > MODEVENT_NONE && kind < MODEVENT_COUNT ? names[kind] : NULL;
}

const char *modRandomGetActiveEventName(void)
{
	if (!g_ModRandomEventFired || g_ModRandomRunOver || !modRandomIsOn()) return NULL;

	const s32 kind = g_ModRandomEventChoice;

#ifndef PLATFORM_N64
	// Keep the label visible through the announcement even if the event ends early.
	if (g_ModRandomEventNotice240 > 0 && !g_ModRandomEventNoticeEnding) return modRandomEventName(kind);
#endif

	switch (kind) {
	case MODEVENT_EXPLOSIVE:
		return g_ModRandomExplosiveOn ? modRandomEventName(kind) : NULL;
	case MODEVENT_PURGE:
		return g_ModRandomPurgeOn ? modRandomEventName(kind) : NULL;
	case MODEVENT_ALIEN_WAR:
		return g_ModRandomAlienWarActive ? modRandomEventName(kind) : NULL;
	case MODEVENT_ASSASSINS:
		return modRandomAssassinCloakActive() ? modRandomEventName(kind) : NULL;
	case MODEVENT_WARZONE:
		return g_ModRandomEventKind == MODEVENT_WARZONE ? modRandomEventName(kind) : NULL;
	case MODEVENT_MAIANS:
	case MODEVENT_SKEDARS:
	case MODEVENT_BLOND:
		// A wave stays active after the last NPC arrives, until its survivors are gone.
		if (g_ModRandomEventKind == kind) return modRandomEventName(kind);
		for (s32 i = 0; i < g_NumChrSlots; i++) {
			struct chrdata *chr = &g_ChrSlots[i];
			if (chr->chrnum < 0 || !chr->prop || !chr->model || chrIsDead(chr)) continue;

			if (kind == MODEVENT_BLOND ? (chr->hidden2 & CHRH2FLAG_BLONDREVENGE)
					: chr->team == MODEVENT_TEAM_MAIAN && (kind == MODEVENT_MAIANS
						? modRandomIsInvader(chr) : chr->bodynum == BODY_SKEDAR)) {
				return modRandomEventName(kind);
			}
		}
		break;
	}

	return NULL;
}

#ifndef PLATFORM_N64
static void modRandomQueueEventEndNotice(void)
{
	g_ModRandomEventEndQueued = true;
}

void modRandomQueueEventOverlay(void)
{
	static Gfx overlay[2048];
	static const char *const descriptions[MODEVENT_COUNT] = {
		[MODEVENT_MAIANS] = "Fifteen Maians are moving in.",
		[MODEVENT_EXPLOSIVE] = "Enemies explode a second after death.",
		[MODEVENT_SKEDARS] = "A dozen Skedar are closing in.",
		[MODEVENT_PURGE] = "Everyone is hunting the civilians.",
		[MODEVENT_ALIEN_WAR] = "Choose a side!",
		[MODEVENT_ASSASSINS] = "Cloaked hunters are after you.",
		[MODEVENT_WARZONE] = "The Calvary has arrived!",
		[MODEVENT_BLOND] = "He's carrying a rocket launcher.",
	};

	struct player *player = g_Vars.currentplayer;
	if ((g_ModRandomEventNotice240 <= 0 && !g_ModRandomEventEndQueued)
			|| !player || player != g_Vars.bond
			|| player->isdead || player->dostartnewlife || player->mpmenuon || player->menuisactive
			|| lvIsPaused() || g_InCutscene || g_Vars.in_cutscene || g_MainIsEndscreen
			|| g_ModRandomRunOver || !modRandomIsOn() || !videoIsWindowVisible()) {
		return;
	}

	if (g_ModRandomEventNotice240 <= 0) {
		g_ModRandomEventEndQueued = false;
		g_ModRandomEventNoticeEnding = true;
		g_ModRandomEventNotice240 = MODEVENT_ANNOUNCE_TICKS;
	}

	const s32 kind = g_ModRandomEventChoice;
	const char *title = modRandomEventName(kind);
	if (!title) return;

	g_ModRandomEventNoticeFramePending = true;
	if (g_ModRandomEventNoticeDelay240 > 0) return;

	char endtitle[48];
	const char *description = descriptions[kind];
	if (g_ModRandomEventNoticeEnding) {
		snprintf(endtitle, sizeof(endtitle), "%s ended", title);
		title = endtitle;
		description = NULL;
	}

	const s32 oldscale = g_ScaleX;
	g_ScaleX = g_ViRes == VIRES_HI ? 2 : 1;
	const s32 center = (viGetViewLeft() + viGetViewWidth() / 2) / g_ScaleX;
	const s32 top = viGetViewTop() + viGetViewHeight() / 4;
	s32 y = top;
	s32 width, height, descwidth = 0;

	textMeasure(&height, &width, (char *)title, g_CharsHandelGothicMd, g_FontHandelGothicMd, 0);
	if (description) {
		textMeasure(&height, &descwidth, (char *)description, g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);
	}

	Gfx *gdl = overlay;
	gDPSetSubpixelOffsetEXT(gdl++, 0, 0);
	gDPGrayscaleEXT(gdl++, false);
	gDPSetDepthSource(gdl++, G_ZS_PIXEL);
	gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);
	gDPSetTextureDetail(gdl++, G_TD_CLAMP);
	// The overlay pass starts from whatever state the scene ended in. The
	// scissor is widened or shifted by the aspect mode in force when it is set,
	// so set the banner's own (centred) mode first: a left or right HUD mode
	// left over from the scene put the scissor off to one side and cut the
	// banner off (all of it on a wide enough screen).
	gSPExtraGeometryModeEXT(gdl++, 0x00ffffff, G_ASPECT_CENTER_EXT);
	gdl = bgScissorToViewport(gdl);
	textBackupAndResetBlends();
	gdl = text0f153628(gdl);
	s32 x = center - width / 2;
	gdl = textRender(gdl, &x, &y, (char *)title, g_CharsHandelGothicMd, g_FontHandelGothicMd,
			0x80caffff, 0x000000d0, viGetWidth(), viGetHeight(), 0, 0);
	if (description) {
		y = top + 16;
		x = center - descwidth / 2;
		gdl = textRender(gdl, &x, &y, (char *)description, g_CharsHandelGothicSm, g_FontHandelGothicSm,
				0xffffffff, 0x000000d0, viGetWidth(), viGetHeight(), 0, 0);
	}
	gSPEndDisplayList(gdl++);
	textRestoreBlends();
	g_ScaleX = oldscale;
	videoQueueOverlay(overlay);
}

void modRandomEventAnnouncementFramePresented(bool presented, bool overlayqueued)
{
	const bool pending = g_ModRandomEventNoticeFramePending;
	g_ModRandomEventNoticeFramePending = false;
	if (!pending || !presented || g_Vars.diffframe240 <= 0) return;

	const s32 elapsed = MIN(g_Vars.diffframe240, TICKS(4) * 4);
	if (g_ModRandomEventNoticeDelay240 > 0) {
		g_ModRandomEventNoticeDelay240 -= elapsed;
		return;
	}
	if (!overlayqueued) return;

	const char *title = modRandomEventName(g_ModRandomEventChoice);
	const char *suffix = g_ModRandomEventNoticeEnding ? " ended" : "";
	static bool bannershot;
	static s32 bannershots;
	if (g_ModRandomEventNotice240 == MODEVENT_ANNOUNCE_TICKS) {
		bannershot = false;
		extern void gfx_overlay_get_info(u32 *aspectin, u32 *winw, u32 *winh, s32 *scissor);
		u32 aspectin, winw, winh;
		s32 scissor[4];

		gfx_overlay_get_info(&aspectin, &winw, &winh, scissor);
		sndStart(var80095200, SFX_HUDMSG, NULL, -1, -1, -1, -1, -1);
		sysLogPrintf(LOG_NOTE, "randomizer: special event banner presented: %s%s (frame %d; window %ux%u, "
				"scene left aspect mode %x, banner scissor x %d w %d y %d h %d, view %d,%d %dx%d)",
				title, suffix, g_Vars.lvframenum, winw, winh, aspectin,
				scissor[0], scissor[2], scissor[1], scissor[3],
				viGetViewLeft(), viGetViewTop(), viGetViewWidth(), viGetViewHeight());
	}
	// DIAGNOSTIC (off unless the PD_BANNER_SHOTS environment variable is set: saving
	// a 1440p PNG stalls the frame): a second and a half into each banner, save the frame as it is
	// drawn (the overlay included) so a banner the log says was presented but the
	// player did not see can be looked at. At most eight a run, beside pd.log.
	if (!bannershot && bannershots < 8 && getenv("PD_BANNER_SHOTS") != NULL && g_ModRandomEventNotice240 <= MODEVENT_ANNOUNCE_TICKS - 360) {
		extern void gfx_request_screenshot(const char *path);
		char path[48];
		bannershot = true;
		snprintf(path, sizeof(path), "pd-banner-%d.png", ++bannershots);
		gfx_request_screenshot(path);
		sysLogPrintf(LOG_NOTE, "randomizer: banner screenshot requested: %s (%s%s, frame %d)",
				path, title, suffix, g_Vars.lvframenum);
	}
	g_ModRandomEventNotice240 -= elapsed;
	if (g_ModRandomEventNotice240 <= 0) {
		sysLogPrintf(LOG_NOTE, "randomizer: special event banner finished: %s%s (frame %d)",
				title, suffix, g_Vars.lvframenum);
	}
}
#endif

static void modRandomEventStart(s32 kind)
{
	g_ModRandomEventFired = true;
	g_ModRandomEventKind = kind;
#ifndef PLATFORM_N64
	g_ModRandomEventNotice240 = MODEVENT_ANNOUNCE_TICKS;
	// Give the objective completion that triggered the event a moment on screen.
	g_ModRandomEventNoticeDelay240 = MODEVENT_ANNOUNCE_DELAY;
	g_ModRandomEventNoticeEnding = false;
	g_ModRandomEventEndQueued = false;
#endif
	modRandomDebugMsg("EVENT: %s", modRandomEventName(kind));
	g_ModRandomSwarmOn = kind == MODEVENT_SKEDARS;
	g_ModRandomSkedarSlayer = kind == MODEVENT_SKEDARS ? (s32)(rngRandom() % 12) : -1;
	g_ModRandomEventLeft = kind == MODEVENT_MAIANS ? 15 : (kind == MODEVENT_SKEDARS ? 12 : 0);
	g_ModRandomEventArrived = 0;
	g_ModRandomEventTries = 0;
	g_ModRandomEventNext60 = 0;
	g_ModRandomEventBase = -1;
	if (kind == MODEVENT_MAIANS) {
		for (s32 i = 0; i < g_NumChrSlots; i++) {
			struct chrdata *chr = &g_ChrSlots[i];
			if (chr->chrnum < 0 || !chr->prop || chr->prop->type != PROPTYPE_CHR
					|| !chr->model || chrIsDead(chr)) {
				continue;
			}
			if (chr->bodynum == BODY_MAIAN_SOLDIER || chr->bodynum == BODY_ELVIS1
					|| chr->bodynum == BODY_ELVISWAISTCOAT || chr->bodynum == BODY_THEKING) {
				modRandomMakeMaianInvader(chr);
				chr->flags |= CHRFLAG0_AIVSAI;
				chr->alertness = 90;
				chr->target = -1;
				chr->ailist = ailistFindById(GAILIST_SEARCH_FOR_PLAYER);
				chr->aioffset = 0;
				chr->aireturnlist = -1;
				chr->aishotlist = -1;
			}
		}
		rebuildTeams();
		rebuildSquadrons();
	}
	if (kind == MODEVENT_ASSASSINS) {
		g_ModRandomAssassinsUntil = g_Vars.lvframe60 + TICKS(60 * 60);
	} else if (kind == MODEVENT_WARZONE) {
		g_ModRandomWarzone.active = true;
		g_ModRandomWarzone.next60 = g_Vars.lvframe60;
	}

	if (kind == MODEVENT_ALIEN_WAR) {
		g_ModRandomAlienWarActive = true;
		g_ModRandomAlienWarOngoing = true;
		g_ModRandomAlienBossesSpawned = false;
		g_ModRandomAlienElvis = g_ModRandomAlienKing = -1;
		g_ModRandomAlienFaction = g_ModRandomAlienWinner = 0;
		g_ModRandomAlienPlayerTeam = g_Vars.bond && g_Vars.bond->prop && g_Vars.bond->prop->chr
			? g_Vars.bond->prop->chr->team : TEAM_ALLY;
		if (g_Vars.bond && g_Vars.bond->prop && g_Vars.bond->prop->chr) {
			g_Vars.bond->prop->chr->team |= MODEVENT_TEAM_MAIAN | MODEVENT_TEAM_TERROR;
		}
		g_ModRandomAlienNextBoss60 = g_Vars.lvframe60;
		g_ModRandomAlienInitialMaians = 20;
		g_ModRandomAlienInitialSkedars = 20;
		g_ModRandomAlienNextMaian60 = g_Vars.lvframe60 + MODRANDOM_ALIEN_SPAWN_INTERVAL;
		g_ModRandomAlienNextSkedar60 = g_Vars.lvframe60 + MODRANDOM_ALIEN_SPAWN_INTERVAL;
		g_ModRandomAlienNextWinner60 = 0;
	}
	if (kind == MODEVENT_EXPLOSIVE) {
		g_ModRandomExplosiveOn = true;
		g_ModRandomExplosiveStart60 = g_Vars.lvframe60 + TICKS(10 * 60);
		g_ModRandomExplosiveEnd60 = g_ModRandomExplosiveStart60 + TICKS(60 * 60);
		g_ModRandomEventKind = MODEVENT_NONE; // no one arrives: the dead do the work
	}

	if (kind == MODEVENT_PURGE) {
		g_ModRandomPurgeOn = true;
		g_ModRandomPurgeEnd60 = g_Vars.lvframe60 + TICKS(60 * 90);
		g_ModRandomEventKind = MODEVENT_NONE; // no one arrives: the level itself turns
	}

	sysLogPrintf(0, "randomizer: special event %d", kind);
}

/** An objective was completed: the mission's event comes after one of them. */
static void modRandomEventObjectiveDone(void)
{
	if (!modRandomSequential() || modRandomGetVersion() < 20 || g_ModRandomEventFired) {
		return;
	}

	g_ModRandomEventDone++;

	if (g_ModRandomEventAfter == 0) {
		// the last objective ends the mission: the event is for one before it
		const s32 span = g_ModRandomNumObjectives > 1 ? g_ModRandomNumObjectives - 1 : 1;

		g_ModRandomEventAfter = 1 + rngRandom() % span;
	}

	if (g_ModRandomEventDone >= g_ModRandomEventAfter) {
		if (g_ModRandomEventChoice > 0) modRandomEventStart(g_ModRandomEventChoice);
		else g_ModRandomEventFired = true;
	}
}

/** Skedar have no list that finds a target out of sight: every half second a
 * standing one is sent running at a random living character that is not of
 * the swarm, and once it can see something its own AI takes over. */
static void modRandomTickSwarm(void)
{
	s32 i, j;

	if (!g_ModRandomSwarmOn || lvIsPaused() || g_InCutscene) {
		return;
	}

	for (i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];
		struct chrdata *victim = NULL;
		s32 seen = 0;

		if (chr->chrnum < 0 || !chr->prop || !chr->model || chr->bodynum != BODY_SKEDAR
				|| chr->team != MODEVENT_TEAM_MAIAN || chrIsDead(chr) || chr->actiontype != ACT_STAND
				|| (g_Vars.lvframe60 + chr->chrnum) % 30 >= g_Vars.lvupdate60) {
			continue;
		}

		for (j = 0; j < g_NumChrSlots; j++) {
			struct chrdata *other = &g_ChrSlots[j];

			if (other->chrnum < 0 || other == chr || !other->prop || !other->model || other->race != RACE_HUMAN
					|| other->team == MODEVENT_TEAM_MAIAN || chrIsDead(other)
					|| (other->chrflags & CHRCFLAG_HIDDEN) || other->prop->type != PROPTYPE_CHR) {
				continue;
			}

			// reservoir pick: each candidate has an equal chance
			if (rngRandom() % ++seen == 0) {
				victim = other;
			}
		}

		if (victim) {
			chrGoToProp(chr, victim->prop, GOPOSFLAG_RUN);
		}
	}
}

bool modRandomIsCivilianChr(struct chrdata *chr)
{
	s32 i;

	for (i = 0; chr && i < MODRANDOM_MAX_CIVILIANS; i++) {
		if (g_ModRandomCivilians[i].chrnum >= 0 && g_ModRandomCivilians[i].chrnum == chr->chrnum) {
			return true;
		}
	}

	return false;
}

/**
 * The purge: for ninety seconds the civilians are on a team of their own, which
 * every other team takes for an enemy, and every character fights other AI, so
 * guards, allies and everyone else go for them as they would for an enemy. When
 * it ends the civilians are noncombatants again.
 */
static void modRandomTickPurge(void)
{
	const bool over = g_Vars.lvframe60 >= (u32)g_ModRandomPurgeEnd60;
	s32 i;

	if (!g_ModRandomPurgeOn || lvIsPaused() || g_InCutscene
			|| (g_Vars.lvframe60 + 7) % 30 >= g_Vars.lvupdate60) {
		return;
	}

	for (i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr->chrnum < 0 || !chr->prop || !chr->model || chr->race != RACE_HUMAN || chr->prop->type != PROPTYPE_CHR
				|| chrIsDead(chr)) {
			continue;
		}

		if (modRandomIsCivilianChr(chr)) {
			if (!over && chr->team == TEAM_NONCOMBAT) {
				chr->team = MODEVENT_TEAM_TERROR;
			} else if (over && chr->team == MODEVENT_TEAM_TERROR) {
				chr->team = TEAM_NONCOMBAT;
			}
		} else if (!over) {
			chr->flags |= CHRFLAG0_AIVSAI;
		}
	}

	rebuildTeams();

	if (over) {
		g_ModRandomPurgeOn = false;
#ifndef PLATFORM_N64
		modRandomQueueEventEndNotice();
#endif
	}
}

/**
 * Explosive exit: for ninety seconds every enemy that dies goes off like a
 * grenade a second later. The blast has no owner, so what it kills is nobody's
 * kill: not a civilian the player is punished for, not an objective the player
 * has made. Blasts already due still go off after the ninety seconds are up.
 */
static void modRandomTickBlasts(void)
{
	s32 i;

	if (g_ModRandomExplosiveOn && g_Vars.lvframe60 >= (u32)g_ModRandomExplosiveEnd60) {
		g_ModRandomExplosiveOn = false;
#ifndef PLATFORM_N64
		modRandomQueueEventEndNotice();
#endif
	}

	if (lvIsPaused()) {
		return;
	}

	for (i = 0; i < MODRANDOM_MAX_BLASTS; i++) {
		if (g_ModRandomBlasts[i].used && g_Vars.lvframe60 >= (u32)g_ModRandomBlasts[i].at60) {
			g_ModRandomBlasts[i].used = false;
			explosionCreateExplosiveExit(&g_ModRandomBlasts[i].pos, g_ModRandomBlasts[i].rooms);
		}
	}
}

/**
 * Door sweep. Guards that are walking somewhere open every closed door they
 * come within ~2.5 m of, whatever the nav code thinks is in the way. Locked
 * (keyed) doors, lasers and the special GE door types are left alone.
 */
static void modRandomTickDoors(void)
{
	s16 propnums[MAX_ROOMPROPS];
	s32 i;

	if (!modRandomIsOn() || lvIsPaused() || g_InCutscene || g_Vars.lvframenum < 30
			|| (g_Vars.lvframe60 + 11) % 12 >= g_Vars.lvupdate60) {
		return;
	}

	for (i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];
		s16 *pn;

		if (chr->chrnum < 0 || !chr->prop || chr->prop->type != PROPTYPE_CHR || chr->aibot) {
			continue;
		}

		if (chr->actiontype != ACT_GOPOS && chr->actiontype != ACT_PATROL && chr->actiontype != ACT_RUNPOS
				&& chr->actiontype != ACT_ATTACKWALK) {
			continue;
		}

		roomGetProps(chr->prop->rooms, propnums, MAX_ROOMPROPS);

		for (pn = propnums; *pn >= 0; pn++) {
			struct prop *prop = &g_Vars.props[*pn];
			struct doorobj *door;
			f32 dx;
			f32 dy;
			f32 dz;

			if (prop->type != PROPTYPE_DOOR) {
				continue;
			}

			door = prop->door;

			if (!doorIsClosed(door) || door->keyflags != 0
					|| door->doortype == DOORTYPE_LASER || door->doortype == DOORTYPE_EYE
					|| door->doortype == DOORTYPE_IRIS || door->doortype == DOORTYPE_FALLAWAY
					|| door->doortype == DOORTYPE_AZTECCHAIR || door->doortype == DOORTYPE_HULL
					|| (door->doorflags & DOORFLAG_DAMAGEONCONTACT)) {
				continue;
			}

			dx = prop->pos.x - chr->prop->pos.x;
			dy = prop->pos.y - chr->prop->pos.y;
			dz = prop->pos.z - chr->prop->pos.z;

			if (dx * dx + dz * dz < 250.0f * 250.0f && dy > -300.0f && dy < 300.0f) {
				modRandomOpenDoorFor(chr, prop);
			}
		}
	}
}

/**
 * Safety net: a guard that is in the level at the start with nothing in its
 * hands gets a gun (and the reason is in pd.log from propobj.c). Only in the
 * first ten seconds, so a gun shot out of a hand stays out.
 */
static void modRandomTickArmGuards(void)
{
	s32 i;

	if (!modRandomIsOn() || lvIsPaused() || g_InCutscene || g_Vars.lvframenum < 20 || g_Vars.lvframe60 > TICKS(60 * 10)
			|| (g_Vars.lvframe60 + 5) % 30 >= g_Vars.lvupdate60 || !g_Vars.bond || !g_Vars.bond->prop) {
		return;
	}

	for (i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];
		s32 wn, mn;

		if (chr->chrnum < 0 || !chr->prop || !chr->model || chr->prop->type != PROPTYPE_CHR || chr->aibot
				|| chr->race != RACE_HUMAN || chrIsDead(chr) || chr == g_Vars.bond->prop->chr
				|| (chr->team & (TEAM_ALLY | TEAM_NONCOMBAT | TEAM_00 | 0x6c))
				|| chr->weapons_held[HAND_RIGHT] || chr->weapons_held[HAND_LEFT]
				|| modRandomIsCivilianChr(chr) || g_ModRandomSurrendered[chr->chrnum & 1023]) {
			continue;
		}

		modRandomFallbackGun(chr->chrnum, &wn, &mn);
		sysLogPrintf(0, "randomizer: guard %d spawned unarmed (body %d), giving weapon %d", chr->chrnum, chr->bodynum, wn);
		chrGiveWeapon(chr, mn, wn, 0);
	}
}

static bool modRandomSpawnMrBlond(void)
{
	s32 i;
	s32 count = 0;
#ifndef PLATFORM_N64
	count = modSpawnConfigCount(MODSPAWN_ESCAPE);
#endif
	const bool configured = count > 0;
	if (!configured) count = modRandomEventCountWaypoints();

	for (i = 0; i < count; i++) {
		s32 padnum;
#ifndef PLATFORM_N64
		if (configured) padnum = modSpawnConfigPad(MODSPAWN_ESCAPE, i);
		else
#endif
		padnum = g_StageSetup.waypoints[i].padnum;

		struct prop *prop = modRandomSpawnRoundNpc(padnum, BODY_MRBLONDE, HEAD_MRBLONDE, TEAM_ENEMY);
		if (!prop || !prop->chr) continue;

		struct chrdata *chr = prop->chr;
		chr->hidden2 |= CHRH2FLAG_BLONDREVENGE;
		chr->chrflags |= CHRCFLAG_RUNFASTER;
		chr->flags &= ~CHRFLAG0_AIVSAI;
		chrSetMaxDamage(chr, 24.0f);
		chr->alertness = 100;
		chr->ailist = ailistFindById(GAILIST_SEARCH_FOR_PLAYER);
		chr->aishotlist = GAILIST_ALERTED;
		chr->target = g_Vars.bond->prop - g_Vars.props;
		g_ModRandomEventGrant = true;
		chrGiveWeaponWithAutoModel(chr, WEAPON_ROCKETLAUNCHER, 0);
		g_ModRandomEventGrant = false;
		chrGoToProp(chr, g_Vars.bond->prop, GOPOSFLAG_RUN);
		rebuildTeams();
		rebuildSquadrons();
		return true;
	}
	return false;
}

/**
 * Mr Blonde's Revenge: he fires his rocket launcher from far away. He stands
 * and shoots while he can see the player from beyond rocket arm's length, and
 * runs at the player otherwise.
 */
static void modRandomTickBlond(void)
{
	s32 i;

	if (!g_Vars.bond || !g_Vars.bond->prop || g_Vars.lvframe60 % TICKS(15) != 0) {
		return;
	}

	for (i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];
		f32 dist;
		bool inrange;

		if (chr->chrnum < 0 || !chr->prop || !chr->model || !(chr->hidden2 & CHRH2FLAG_BLONDREVENGE)
				|| chrIsDead(chr) || chr->actiontype == ACT_DEAD || chr->actiontype == ACT_DIE) {
			continue;
		}

		chr->target = g_Vars.bond->prop - g_Vars.props;
		dist = chrGetDistanceToTarget(chr);
		inrange = dist > 450.0f && dist < 9000.0f && chrHasLosToTarget(chr);

		if (inrange) {
			if (chr->actiontype != ACT_ATTACK) {
				chrTryAttackStand(chr, ATTACKFLAG_AIMATBOND, 0);
			}
		} else if (chr->actiontype == ACT_ATTACK) {
			chrStop(chr);
			chrGoToProp(chr, g_Vars.bond->prop, GOPOSFLAG_RUN);
		}
	}
}

static bool modRandomWarzoneWaypointOk(const struct waypoint *waypoint)
{
	s32 padnum = waypoint->padnum;

	if (!waypoint->neighbours || waypoint->neighbours[0] == -1
			|| padnum < 0 || !g_PadsFile || padnum >= g_PadsFile->numpads || padnum == g_ModRandomSpawnPad) {
		return false;
	}

	s32 room = modRandomPadRoom(padnum);

	return room > 0 && room < g_Vars.roomcount && (!g_ModRandomReached || g_ModRandomReached[room]);
}

struct modrandomwarzonepad {
	s32 padnum;
	f32 separation;
};

static s32 modRandomWarzonePads(struct modrandomwarzonepad *pads)
{
	s32 count = 0;
	s32 waypoints = modRandomEventCountWaypoints();
	s32 start = waypoints > 0 ? rngRandom() % waypoints : 0;

	for (s32 i = 0; i < waypoints; i++) {
		const struct waypoint *waypoint = &g_StageSetup.waypoints[(i + start) % waypoints];

		if (!modRandomWarzoneWaypointOk(waypoint)) {
			continue;
		}

		struct pad pad;
		padUnpack(waypoint->padnum, PADFIELD_POS, &pad);

		// Spread out from the nearest arrival in this wave, including retries.
		f32 separation = FLT_MAX;

		for (s32 k = 0; k < g_ModRandomWarzone.numspawned; k++) {
			const struct coord *pos = &g_ModRandomWarzone.positions[k];
			f32 dx = pad.pos.x - pos->x;
			f32 dy = pad.pos.y - pos->y;
			f32 dz = pad.pos.z - pos->z;
			separation = MIN(separation, dx * dx + dy * dy + dz * dz);
		}

		s32 j = MIN(count, MODRANDOM_WARZONE_PADS - 1);

		if (count == MODRANDOM_WARZONE_PADS && separation <= pads[j].separation) {
			continue;
		}

		while (j > 0 && separation > pads[j - 1].separation) {
			pads[j] = pads[j - 1];
			j--;
		}

		pads[j] = (struct modrandomwarzonepad){ waypoint->padnum, separation };
		count = MIN(count + 1, MODRANDOM_WARZONE_PADS);
	}

	return count;
}

enum { WARZONE_PAD_BLOCKED, WARZONE_SPAWNED, WARZONE_OUT_OF_RESOURCES };

static s32 modRandomSpawnWarzoneNpc(s32 padnum, bool ally)
{
	if (mempGetStageFreeTotal() < MODRANDOM_WARZONE_MEMFLOOR || chrsGetNumFree() <= 1) {
		return WARZONE_OUT_OF_RESOURCES;
	}

	s32 body = ally ? modRandomAllyBody() : modRandomGuardBody(1);
	s32 head = ally ? modRandomAllyHead() : modRandomGuardHead(1);
	struct prop *prop = modRandomSpawnRoundNpc(padnum, body, head, ally ? TEAM_ALLY : TEAM_ENEMY);

	if (!prop || !prop->chr) {
		return WARZONE_PAD_BLOCKED;
	}

	struct chrdata *chr = prop->chr;

	g_ModRandomEventGrant = true;
	struct prop *weapon = ally ? modRandomGiveAllyGun(chr) : chrGiveWeaponWithAutoModel(chr, WEAPON_CMP150, 0);
	g_ModRandomEventGrant = false;

	if (!weapon) {
		chrRemove(prop, true);
		propDelist(prop);
		propDisable(prop);
		propFree(prop);
		return WARZONE_OUT_OF_RESOURCES;
	}

	chr->hidden2 |= CHRH2FLAG_WARZONE;
	chr->ailist = ailistFindById(GAILIST_IDLE);
	chr->aioffset = 0;
	chr->chrflags |= CHRCFLAG_NOFRIENDLYFIRE; // Solo guards need this flag to hit enemy NPCs.
	bool follower = ally && g_ModRandomWarzone.allies % MODRANDOM_WARZONE_WAVE_SIZE == 0;

	if (follower) {
		// Start the Friend Phone buddy loop ready to fight. The default buddy
		// intro sets mission flags and starts the buddy in passive mode.
		chr->ailist = ailistFindById(GAILIST_BUDDY_MAIN);
		chr->chrflags |= CHRCFLAG_PUSHABLE;
		chr->alertness = 100;
		chrSetChrPreset(chr, CHR_BOND);
		g_ModRandomWarzone.followers[g_ModRandomWarzone.wave] = chr->chrnum;
	} else {
		chr->flags &= ~CHRFLAG0_AIVSAI; // The event supplies these actors' targets.
	}
	g_ModRandomWarzone.positions[g_ModRandomWarzone.numspawned++] = prop->pos;
	modRandomDebugSpawn(chr, MODDBG_EVENT);
	sysLogPrintf(0, "warzone: wave %d spawned %s %d at pad %d, room %d, tick %d%s",
			g_ModRandomWarzone.wave + 1, ally ? "partner" : "enemy", chr->chrnum, padnum, prop->rooms[0],
			g_Vars.lvframe60, follower ? " (follows player)" : "");

	return WARZONE_SPAWNED;
}

static s32 modRandomTryWarzoneSpawn(bool ally)
{
	struct modrandomwarzonepad pads[MODRANDOM_WARZONE_PADS];
	s32 count = modRandomWarzonePads(pads);

	for (s32 i = 0; i < count; i++) {
		s32 result = modRandomSpawnWarzoneNpc(pads[i].padnum, ally);

		if (result != WARZONE_PAD_BLOCKED) {
			return result;
		}
	}

	// The most widely spaced pads can all be blocked on a crowded map.
	if (count == ARRAYCOUNT(pads)) {
		s32 waypoints = modRandomEventCountWaypoints();
		s32 start = rngRandom() % waypoints;

		for (s32 i = 0; i < waypoints; i++) {
			const struct waypoint *waypoint = &g_StageSetup.waypoints[(i + start) % waypoints];

			if (modRandomWarzoneWaypointOk(waypoint)) {
				s32 result = modRandomSpawnWarzoneNpc(waypoint->padnum, ally);

				if (result != WARZONE_PAD_BLOCKED) {
					return result;
				}
			}
		}
	}

	return WARZONE_PAD_BLOCKED;
}

static void modRandomAdvanceWarzoneWave(void)
{
	s32 due = (g_ModRandomWarzone.wave + 1) * MODRANDOM_WARZONE_WAVE_SIZE;
	s32 allies = MIN(due, MODRANDOM_WARZONE_ALLIES);
	s32 enemies = MIN(due, MODRANDOM_WARZONE_ENEMIES);

	bool ally = g_ModRandomWarzone.allies < allies
		&& (g_ModRandomWarzone.allies <= g_ModRandomWarzone.enemies || g_ModRandomWarzone.enemies == enemies);

	if (modRandomTryWarzoneSpawn(ally) != WARZONE_SPAWNED) {
		g_ModRandomWarzone.next60 = g_Vars.lvframe60 + MODRANDOM_WARZONE_RETRY;
		return;
	}

	if (g_ModRandomWarzone.numspawned == 1) {
		g_ModRandomWarzone.waveStart60 = g_Vars.lvframe60;
	}
	if (ally) {
		g_ModRandomWarzone.allies++;
	} else {
		g_ModRandomWarzone.enemies++;
	}
	rebuildTeams();
	rebuildSquadrons();
	g_ModRandomWarzone.next60 = g_Vars.lvframe60 + MODRANDOM_WARZONE_SPAWN_DELAY;

	if (g_ModRandomWarzone.allies == allies && g_ModRandomWarzone.enemies == enemies) {
		g_ModRandomWarzone.wave++;
		g_ModRandomWarzone.numspawned = 0;
		g_ModRandomWarzone.next60 = MAX(g_ModRandomWarzone.next60, g_ModRandomWarzone.waveStart60 + MODRANDOM_WARZONE_WAVE_DELAY);
		sysLogPrintf(0, "warzone: wave %d at tick %d, %d partners and %d enemies arrived in total (started at tick %d)",
				g_ModRandomWarzone.wave, g_Vars.lvframe60, allies, enemies, g_ModRandomWarzone.waveStart60);
	}
}

static bool modRandomWarzoneCombatant(struct chrdata *chr)
{
	return chr->chrnum >= 0 && chr->prop && chr->model && chr->prop->type == PROPTYPE_CHR
		&& !(chr->chrflags & CHRCFLAG_HIDDEN) && !chrIsDead(chr) && !modRandomChrKnockedOut(chr);
}

static bool modRandomWarzoneTargetVisible(struct chrdata *chr, struct prop *target)
{
	return target->chr && !(target->chr->hidden & (CHRHFLAG_CLOAKED | CHRHFLAG_UNTARGETABLE | CHRHFLAG_DONTSHOOTME))
		&& chrHasLosToProp(chr, target);
}

static struct prop *modRandomWarzoneVisibleEnemy(struct chrdata *chr, f32 maxdistance)
{
	struct prop *target = NULL;

	for (s32 i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *other = &g_ChrSlots[i];

		if (!(other->team & TEAM_ENEMY) || (other->team & (TEAM_ALLY | TEAM_NONCOMBAT))
				|| !modRandomWarzoneCombatant(other)) {
			continue;
		}

		f32 distance = chrGetSquaredDistanceToCoord(chr, &other->prop->pos);

		if (distance < maxdistance && modRandomWarzoneTargetVisible(chr, other->prop)) {
			target = other->prop;
			maxdistance = distance;
		}
	}

	return target;
}

static void modRandomDefendWarzoneAlly(struct chrdata *chr)
{
	if (modRandomTacticsTakeCover(chr) || !modRandomTacticsCanGiveOrder(chr)
			|| (chr->actiontype != ACT_STAND && chr->actiontype != ACT_KNEEL && chr->actiontype != ACT_ATTACK)) {
		return;
	}

	struct prop *target = modRandomWarzoneVisibleEnemy(chr, MODRANDOM_WARZONE_RANGE_SQ);
	s32 targetnum = target ? target - g_Vars.props : -1;

	if (chr->actiontype == ACT_ATTACK && chr->target != targetnum) {
		chrTryStop(chr);
		if (!modRandomTacticsCanGiveOrder(chr)) {
			return;
		}
	}

	chr->target = targetnum;

	if (target) {
		modRandomTacticsDefend(chr, target, -1, false);
	}
}

static void modRandomWarzoneFight(struct chrdata *chr, struct chrdata **opponents, s32 count)
{
	if (!modRandomTacticsCanGiveOrder(chr)) {
		return;
	}

	struct prop *target = NULL;
	f32 nearest = FLT_MAX;

	for (s32 i = 0; i < count; i++) {
		struct chrdata *other = opponents[i];
		f32 distance = chrGetSquaredDistanceToCoord(chr, &other->prop->pos);

		if (distance < nearest) {
			target = other->prop;
			nearest = distance;
		}
	}

	bool canAttack = target && nearest <= MODRANDOM_WARZONE_RANGE_SQ && modRandomWarzoneTargetVisible(chr, target);

	if (chr->team == TEAM_ENEMY) {
		struct prop *player = g_Vars.bond->prop;

		if (chrGetSquaredDistanceToCoord(chr, &player->pos) <= MODRANDOM_WARZONE_RANGE_SQ
				&& chrIsLookingAtPos(chr, &player->pos, 64) && modRandomWarzoneTargetVisible(chr, player)) {
			target = player;
			canAttack = true;
		}
	} else {
		// A visible guard nearby can take priority over a more distant battle.
		struct prop *enemy = modRandomWarzoneVisibleEnemy(chr, canAttack ? nearest : MODRANDOM_WARZONE_RANGE_SQ);

		if (enemy) {
			target = enemy;
			canAttack = true;
		}
	}

	if (target == NULL) {
		chr->target = -1;
		if (chr->actiontype == ACT_ATTACK || chr->actiontype == ACT_GOPOS) {
			chrTryStop(chr);
		}
		return;
	}

	s32 targetnum = target - g_Vars.props;
	bool changed = chr->target != targetnum;
	chr->target = targetnum;

	if (canAttack) {
		chrTryAttackStand(chr, ATTACKFLAG_AIMATTARGET, 0);
	} else if (chr->actiontype != ACT_GOPOS || changed
			|| chrGetSquaredDistanceToCoord(target->chr, &chr->act_gopos.endpos) > 250.0f * 250.0f) {
		chrGoToProp(chr, target, GOPOSFLAG_RUN);
	}
}

static bool modRandomWarzoneIsFollower(struct chrdata *chr)
{
	for (s32 i = 0; i < ARRAYCOUNT(g_ModRandomWarzone.followers); i++) {
		if (g_ModRandomWarzone.followers[i] == chr->chrnum) {
			return true;
		}
	}

	return false;
}

static void modRandomEndWarzone(void)
{
	g_ModRandomWarzone.active = false;
	g_ModRandomEventKind = MODEVENT_NONE;

	for (s32 i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr->chrnum < 0 || !(chr->hidden2 & CHRH2FLAG_WARZONE) || !chr->prop || !chr->model || chrIsDead(chr)) {
			continue;
		}

		if (chr->team != TEAM_ALLY) {
			chr->hidden2 &= ~CHRH2FLAG_WARZONE;
		}

		if (modRandomTacticsCanGiveOrder(chr)) {
			chrTryStop(chr);
			chr->target = -1;
		}
		chr->ailist = ailistFindById(GAILIST_IDLE);
		chr->aioffset = 0;
		chr->aireturnlist = -1;
		chr->aishotlist = -1;

		s32 id = chr->chrnum & 1023;
		g_ModRandomPosts[id].model = NULL;
		g_ModRandomTacticsModel[id] = chr->model;
		g_ModRandomRole[id] = TACTICS_DEFENDER;
		g_ModRandomBorn60[id] = g_Vars.lvframe60;
		g_ModRandomDefCover[id] = 0;
		g_ModRandomArrived[id] = 1;
		g_ModRandomHold[id] = 0;
	}

#ifndef PLATFORM_N64
	modRandomQueueEventEndNotice();
#endif
}

static void modRandomTickWarzone(void)
{
	if (g_ModRandomWarzone.enemies < MODRANDOM_WARZONE_ENEMIES || g_ModRandomWarzone.allies < MODRANDOM_WARZONE_ALLIES) {
		if (g_Vars.lvframe60 >= g_ModRandomWarzone.next60) {
			modRandomAdvanceWarzoneWave();
		}
	}

	struct chrdata *allies[MODRANDOM_WARZONE_ALLIES];
	struct chrdata *enemies[MODRANDOM_WARZONE_ENEMIES];
	s32 allycount = 0;
	s32 enemycount = 0;

	for (s32 i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (!(chr->hidden2 & CHRH2FLAG_WARZONE) || !modRandomWarzoneCombatant(chr)) {
			continue;
		}

		if (chr->team == TEAM_ALLY && allycount < ARRAYCOUNT(allies)) {
			allies[allycount++] = chr;
		} else if (chr->team == TEAM_ENEMY && enemycount < ARRAYCOUNT(enemies)) {
			enemies[enemycount++] = chr;
		}
	}

	if (g_ModRandomWarzone.enemies == MODRANDOM_WARZONE_ENEMIES && g_ModRandomWarzone.allies == MODRANDOM_WARZONE_ALLIES
			&& (allycount == 0 || enemycount == 0)) {
		modRandomEndWarzone();
		return;
	}

	for (s32 i = 0; i < allycount; i++) {
		if ((g_Vars.lvframe60 + allies[i]->chrnum) % 30 < g_Vars.lvupdate60 && !modRandomWarzoneIsFollower(allies[i])) {
			modRandomWarzoneFight(allies[i], enemies, enemycount);
		}
	}

	for (s32 i = 0; i < enemycount; i++) {
		if ((g_Vars.lvframe60 + enemies[i]->chrnum) % 30 < g_Vars.lvupdate60) {
			modRandomWarzoneFight(enemies[i], allies, allycount);
		}
	}
}

static void modRandomTickNewEvents(void)
{
	const bool cloakExpired = g_ModRandomAssassinsUntil > 0
		&& g_Vars.lvframe60 >= g_ModRandomAssassinsUntil;

	if (cloakExpired) {
		for (s32 i = 0; i < g_NumChrSlots; i++) {
			struct chrdata *chr = &g_ChrSlots[i];
			if (chr->chrnum < 0 || !chr->prop || !chr->model || chr->prop->type != PROPTYPE_CHR
					|| chrIsDead(chr)) continue;

			if (chr->hidden2 & CHRH2FLAG_ASSASSINCLOAK) {
				chr->hidden2 &= ~CHRH2FLAG_ASSASSINCLOAK;
				chrUncloak(chr, true);
			}
		}
#ifndef PLATFORM_N64
		modRandomQueueEventEndNotice();
#endif
		g_ModRandomAssassinsUntil = 0;
	}

	if (g_ModRandomWarzone.active && g_Vars.lvupdate60 > 0) {
		modRandomTickWarzone();
	}

	if (g_Vars.lvupdate60 > 0) {
		modRandomTickBlond();
	}

	if (g_ModRandomEventKind == MODEVENT_BLOND && g_Vars.lvframe60 >= g_ModRandomEventNext60) {
		if (modRandomSpawnMrBlond()) g_ModRandomEventKind = MODEVENT_NONE;
		else g_ModRandomEventNext60 = g_Vars.lvframe60 + TICKS(30);
	}
}

static void modRandomTickEvent(void)
{
	if (g_Vars.bond && g_Vars.bond->prop && g_Vars.currentplayer == g_Vars.bond
			&& !g_Vars.bond->isdead && !g_ModRandomRunOver && !g_InCutscene && !lvIsPaused()) {
		modRandomTickNewEvents();
	}
	modRandomTickAlienWar();
	modRandomDebugMarkStage();
	modRandomTickArmGuards();
	modRandomTickDoors();
	modRandomTickBlasts();
	modRandomTickTactics();
	modRandomTickSurrendered();
	modRandomTickPurge();
	modRandomTickSurrenders();
	modRandomTickSwarm();

	if (g_ModRandomEventKind == MODEVENT_ALIEN_WAR || g_ModRandomEventKind == MODEVENT_NONE
			|| g_ModRandomEventLeft <= 0 || g_ModRandomRunOver
			|| !g_Vars.bond || !g_Vars.bond->prop || g_Vars.currentplayer != g_Vars.bond
			|| g_Vars.bond->isdead || lvIsPaused() || g_InCutscene || g_Vars.lvframe60 < g_ModRandomEventNext60) {
		return;
	}

	if (modRandomEventCountWaypoints() <= 0) {
		g_ModRandomEventKind = MODEVENT_NONE;
		return;
	}

	// one at a time: the level's character slots are mostly spent by now, and
	// a failed spawn marks a corpse to be cleared, so a later try can succeed
	if (modRandomEventSpawnOne(g_ModRandomEventKind, g_ModRandomEventArrived)) {
		g_ModRandomEventLeft--;
		g_ModRandomEventArrived++;
		g_ModRandomEventNext60 = g_Vars.lvframe60 + TICKS(6);
		rebuildTeams();
		rebuildSquadrons();
	} else {
		g_ModRandomEventNext60 = g_Vars.lvframe60 + TICKS(20);
	}

	if (++g_ModRandomEventTries > 400 && g_ModRandomEventLeft > 0) {
		sysLogPrintf(0, "randomizer: special event %d gave up with %d still to come", g_ModRandomEventKind, g_ModRandomEventLeft);
		g_ModRandomEventLeft = 0;
	}

	if (g_ModRandomEventLeft <= 0) {
		sysLogPrintf(0, "randomizer: special event %d: %d arrived", g_ModRandomEventKind, g_ModRandomEventArrived);
		g_ModRandomEventKind = MODEVENT_NONE;
	}
}

// Every objective done is worth a quarter of a shield, once.
static void modRandomTickObjectiveShield(void)
{
	struct player *player = g_Vars.bond;
	s32 i;

	if (!player || g_Vars.currentplayer != player || player->isdead || g_InCutscene || lvIsPaused()) {
		return;
	}

	for (i = 0; i < g_ModRandomNumObjectives && i < 32; i++) {
		if (!(g_ModRandomShieldPaid & (1u << i)) && objectiveCheck(i) == OBJECTIVE_COMPLETE) {
			f32 frac = playerGetShieldFrac() + 0.25f;

			g_ModRandomShieldPaid |= 1u << i;
			modRandomEventObjectiveDone();
			playerSetShieldFrac(frac > 1.0f ? 1.0f : frac);
		}
	}
}

static void modRandomTickObjectiveBanner(void)
{
	struct player *player = g_Vars.bond;
	s32 i;

	if (!modRandomIsOn() || modRunIsOn() || modRandomIsEndless() || g_ModRandomRunOver
			|| g_ModRandomAlienWarActive
#ifndef PLATFORM_N64
			|| g_ModRandomEventNotice240 > 0
#endif
			|| g_InCutscene || g_Vars.in_cutscene || lvIsPaused() || !player
			|| g_Vars.currentplayer != player || player->mpmenuon || player->isdead
			|| player->dostartnewlife || g_ModRandomSpawnPending || g_Vars.lvframenum < 10) {
		return;
	}

	for (i = 0; i < g_ModRandomNumObjectives; i++) {
		if (objectiveCheck(i) == OBJECTIVE_INCOMPLETE) {
			if (i == g_ModRandomAnnouncedObjective
					|| (i < g_ModRandomNumTargets && !g_ModRandomTargets[i].spawned)
					|| (i == g_ModRandomClimaxSlot && !g_ModRandomClimaxStarted)) return;

			char text[MODRANDOM_TEXTLEN + 2];
			snprintf(text, sizeof(text), "%s\n", g_ModRandomObjText[i]);
			hudmsgCreateWithFlags(text, HUDMSGTYPE_EVENT,
				HUDMSGFLAG_DELAY | HUDMSGFLAG_ALLOWDUPES | HUDMSGFLAG_OBJECTIVE);
			g_ModRandomAnnouncedObjective = i;
			return;
		}
	}
}

// A mission opens with nobody within three metres of the player: any guard the
// level (or the roll) put that close is taken out of it, for the first second.
#define MODRANDOM_CLEAR_RADIUS 300.0f

static void modRandomTickSpawnClear(void)
{
	struct player *player = g_Vars.bond;
	struct chrdata *own = player && player->prop ? player->prop->chr : NULL;
	s32 i;

	if (!own || g_Vars.currentplayer != player || !modRandomIsOn() || g_Vars.lvframenum < 2
			|| g_Vars.lvframenum > 90 || g_ModRandomSpawnPending) {
		return;
	}

	for (i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];
		struct prop *prop = chr->prop;
		f32 dx, dy, dz;

		if (chr->chrnum < 0 || chr == own || !prop || !chr->model || chr->race != RACE_HUMAN
				|| (chr->team & (TEAM_ALLY | TEAM_NONCOMBAT)) || modRandomIsObjectiveChr(chr->chrnum)
				|| chrIsDead(chr)) {
			continue;
		}

		dx = prop->pos.x - player->prop->pos.x;
		dy = prop->pos.y - player->prop->pos.y;
		dz = prop->pos.z - player->prop->pos.z;

		if (dx * dx + dz * dz > MODRANDOM_CLEAR_RADIUS * MODRANDOM_CLEAR_RADIUS || dy > 200.0f || dy < -200.0f) {
			continue;
		}

		sysLogPrintf(0, "randomizer: cleared a guard %d from the player's spawn", (s32)sqrtf(dx * dx + dz * dz));
		chrRemove(prop, true);
		propDelist(prop);
		propDisable(prop);
		propFree(prop);
	}
}

/**
 * A mission opens with nobody able to shoot at the player: for the first few
 * seconds any enemy that is in range of the player and has a line to them is
 * stopped, has its gun lowered, and is turned to face away from the player.
 */
#define MODRANDOM_SAFESTART_FRAMES 240
#define MODRANDOM_SAFESTART_RANGE  3500.0f

static void modRandomTickSafeStart(void)
{
	struct player *player = g_Vars.bond;
	struct chrdata *own = player && player->prop ? player->prop->chr : NULL;
	static s32 turned;
	s32 i;

	if (!own || g_Vars.currentplayer != player || !modRandomIsOn() || g_Vars.lvframenum < 2
			|| g_ModRandomSpawnPending || g_InCutscene || g_Vars.in_cutscene) {
		return;
	}

	if (g_Vars.lvframenum > MODRANDOM_SAFESTART_FRAMES) {
		return;
	}

	if (g_Vars.lvframenum <= 3) {
		turned = 0;
	}

	for (i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];
		struct prop *prop = chr->prop;
		f32 dx, dy, dz;

		if (chr->chrnum < 0 || chr == own || !prop || !chr->model || chr->race != RACE_HUMAN
				|| (chr->team & (TEAM_ALLY | TEAM_NONCOMBAT)) || chrIsDead(chr)
				|| chr->actiontype == ACT_DIE || chr->actiontype == ACT_DEAD || chr->actiontype == ACT_DRUGGEDKO) {
			continue;
		}

		dx = player->prop->pos.x - prop->pos.x;
		dy = player->prop->pos.y - prop->pos.y;
		dz = player->prop->pos.z - prop->pos.z;

		if (dx * dx + dz * dz > MODRANDOM_SAFESTART_RANGE * MODRANDOM_SAFESTART_RANGE
				|| dy > 400.0f || dy < -400.0f || !chrHasLosToProp(chr, player->prop)) {
			continue;
		}

		if (chr->actiontype == ACT_ATTACK || chr->actiontype == ACT_RUNPOS || chr->actiontype == ACT_GOPOS) {
			chrStopFiring(chr);
			chrTryStop(chr);
		}

		chrSetLookAngle(chr, atan2f(-dx, -dz) < 0.0f ? atan2f(-dx, -dz) + M_BADTAU : atan2f(-dx, -dz));

		if (turned < 40) {
			turned++;
			sysLogPrintf(0, "randomizer: safe start: turned chr %d away (%d from the player)",
					chr->chrnum, (s32)sqrtf(dx * dx + dz * dz));
		}
	}
}

static void modRandomTickSpentItem(void)
{
	modRandomTickEvent();
	modRandomTickSpawnClear();
	modRandomTickSafeStart();

	struct player *player = g_Vars.bond;

	modRandomTickFriendPhone();
	modRandomTickAlienAllies();
	modRandomTickArmedCivArrivals();

	if (!player || g_Vars.currentplayer != player) {
		return;
	}

	if (player->devicesactive & DEVICE_FRIENDPHONE) {
		player->devicesactive &= ~DEVICE_FRIENDPHONE;
		modRandomUseFriendPhone(); // takes the item, or one of its charges
	}

	if (player->devicesactive & DEVICE_ARMCIVS) {
		player->devicesactive &= ~DEVICE_ARMCIVS;
		modRandomUseArmCivs();
	}

	if (player->devicesactive & DEVICE_MINDMELT) {
		player->devicesactive &= ~DEVICE_MINDMELT;
		invRemoveItemByNum(WEAPON_COMMSRIDER);
		modRandomUseMindMelt();
	}
	if (player->devicesactive & DEVICE_MAIANBACKUP) {
		player->devicesactive &= ~DEVICE_MAIANBACKUP;
		invRemoveItemByNum(WEAPON_DISGUISE40);
		g_ModRandomAlienAllyFaction = 1;
		g_ModRandomAlienAllyWanted = 6;
		g_ModRandomAlienAllyGot = g_ModRandomAlienAllyTries = 0;
	}
	if (player->devicesactive & DEVICE_SKEDARSWARM) {
		player->devicesactive &= ~DEVICE_SKEDARSWARM;
		invRemoveItemByNum(WEAPON_DISGUISE41);
		g_ModRandomAlienAllyFaction = 2;
		g_ModRandomAlienAllyWanted = 12;
		g_ModRandomAlienAllyGot = g_ModRandomAlienAllyTries = 0;
	}
}

#ifndef PLATFORM_N64
extern struct weapon invitem_tester;
extern struct weapon invitem_presidentscanner;
extern struct weapon invitem_commsrider;
extern struct weapon invitem_cloakingdevice;
extern struct weaponfunc_device invfunc_cloak_primary;

static struct weaponfunc_device g_ModRandomFriendPhoneFunc;
static struct weaponfunc_device g_ModRandomArmCivsFunc;
static struct weaponfunc_device g_ModRandomMindMeltFunc;
static struct weaponfunc_device g_ModRandomMaianBackupFunc;
static struct weaponfunc_device g_ModRandomSkedarSwarmFunc;
static struct weapon g_ModRandomMaianBackupItem;
static struct weapon g_ModRandomSkedarSwarmItem;
static struct weapon *g_ModRandomAlienOriginalItems[2];

/**
 * Two items a gauntlet boon can bring, which the game never hands out (the
 * test gun, and the President scanner a mission's script keeps to itself).
 * They are cloaking devices in all but name: a device of their own that the
 * quick menu switches on.
 */
PD_CONSTRUCTOR static void modRandomItemsInit(void)
{
	const u16 phone = langAddPortText("Friend Phone");
	const u16 civs = langAddPortText("Arm The Civs");
	const u16 melt = langAddPortText("Mind Melt");
	const u16 maian = langAddPortText("Maian Backup");
	const u16 skedar = langAddPortText("Skedar Swarm");

	g_ModRandomFriendPhoneFunc = invfunc_cloak_primary;
	g_ModRandomFriendPhoneFunc.base.name = phone;
	g_ModRandomFriendPhoneFunc.base.ammoindex = -1;
	g_ModRandomFriendPhoneFunc.device = DEVICE_FRIENDPHONE;
	invitem_tester = invitem_cloakingdevice;
	invitem_tester.functions[0] = (struct weaponfunc *) &g_ModRandomFriendPhoneFunc;
	invitem_tester.ammos[0] = NULL;
	invitem_tester.shortname = phone;
	invitem_tester.name = phone;

	g_ModRandomArmCivsFunc = invfunc_cloak_primary;
	g_ModRandomArmCivsFunc.base.name = civs;
	g_ModRandomArmCivsFunc.base.ammoindex = -1;
	g_ModRandomArmCivsFunc.device = DEVICE_ARMCIVS;
	invitem_presidentscanner = invitem_cloakingdevice;
	invitem_presidentscanner.functions[0] = (struct weaponfunc *) &g_ModRandomArmCivsFunc;
	invitem_presidentscanner.ammos[0] = NULL;
	invitem_presidentscanner.shortname = civs;
	invitem_presidentscanner.name = civs;

	g_ModRandomMindMeltFunc = invfunc_cloak_primary;
	g_ModRandomMindMeltFunc.base.name = melt;
	g_ModRandomMindMeltFunc.base.ammoindex = -1;
	g_ModRandomMindMeltFunc.device = DEVICE_MINDMELT;
	invitem_commsrider = invitem_cloakingdevice;
	invitem_commsrider.functions[0] = (struct weaponfunc *) &g_ModRandomMindMeltFunc;
	invitem_commsrider.ammos[0] = NULL;
	invitem_commsrider.shortname = melt;
	invitem_commsrider.name = melt;

	g_ModRandomAlienOriginalItems[0] = g_Weapons[WEAPON_DISGUISE40];
	g_ModRandomAlienOriginalItems[1] = g_Weapons[WEAPON_DISGUISE41];
	g_ModRandomMaianBackupFunc = invfunc_cloak_primary;
	g_ModRandomMaianBackupFunc.base.name = maian;
	g_ModRandomMaianBackupFunc.base.ammoindex = -1;
	g_ModRandomMaianBackupFunc.device = DEVICE_MAIANBACKUP;
	g_ModRandomMaianBackupItem = invitem_cloakingdevice;
	g_ModRandomMaianBackupItem.functions[0] = (struct weaponfunc *)&g_ModRandomMaianBackupFunc;
	g_ModRandomMaianBackupItem.ammos[0] = NULL;
	g_ModRandomMaianBackupItem.shortname = maian;
	g_ModRandomMaianBackupItem.name = maian;
	g_ModRandomSkedarSwarmFunc = invfunc_cloak_primary;
	g_ModRandomSkedarSwarmFunc.base.name = skedar;
	g_ModRandomSkedarSwarmFunc.base.ammoindex = -1;
	g_ModRandomSkedarSwarmFunc.device = DEVICE_SKEDARSWARM;
	g_ModRandomSkedarSwarmItem = invitem_cloakingdevice;
	g_ModRandomSkedarSwarmItem.functions[0] = (struct weaponfunc *)&g_ModRandomSkedarSwarmFunc;
	g_ModRandomSkedarSwarmItem.ammos[0] = NULL;
	g_ModRandomSkedarSwarmItem.shortname = skedar;
	g_ModRandomSkedarSwarmItem.name = skedar;
}

void modRandomAlienItemsSet(bool enabled)
{
	if (enabled) {
		g_Weapons[WEAPON_DISGUISE40] = &g_ModRandomMaianBackupItem;
		g_Weapons[WEAPON_DISGUISE41] = &g_ModRandomSkedarSwarmItem;
	} else if (g_ModRandomAlienOriginalItems[0] && g_ModRandomAlienOriginalItems[1]) {
		g_Weapons[WEAPON_DISGUISE40] = g_ModRandomAlienOriginalItems[0];
		g_Weapons[WEAPON_DISGUISE41] = g_ModRandomAlienOriginalItems[1];
	}
}
#else
void modRandomAlienItemsSet(bool enabled)
{
	(void)enabled;
}
#endif

/** The climax: once the last target is down, find the computer and activate it. */
static void modRandomTickClimax(void)
{
	struct player *player = g_Vars.bond;

	if (g_ModRandomClimaxSlot < 0 || g_ModRandomClimaxDone || g_ModRandomRunOver || g_ModRandomAlienWarActive
			|| g_InCutscene || lvIsPaused() || g_Vars.currentplayer != g_Vars.bond
			|| !player || !player->prop || player->isdead || player->dostartnewlife
			|| g_ModRandomSpawnPending || g_Vars.lvframenum < 2) {
		return;
	}

	if (!g_ModRandomClimaxStarted) {
		s32 failedPads[6];

		if (g_ModRandomCleared < g_ModRandomNumTargets) {
			return;
		}

		// the computer stands on the zone's pad; if that cannot take it, the next best pad does
		for (s32 attempt = 0; attempt < ARRAYCOUNT(failedPads) && !g_ModRandomClimaxTerminal; attempt++) {
			if (!modRandomClimaxPickZone(attempt, failedPads)) {
				break;
			}

			g_ModRandomClimaxTerminal = scenarioCreateObj(MODEL_GOODPC, g_ModRandomClimaxPad, 0.2f,
					OBJFLAG_FALL | OBJFLAG_INVINCIBLE | OBJFLAG_FORCENOBOUNCE,
					OBJFLAG2_IMMUNETOGUNFIRE | OBJFLAG2_IMMUNETOEXPLOSIONS,
					OBJFLAG3_INTERACTABLE);
			failedPads[attempt] = g_ModRandomClimaxPad;
		}

		if (!g_ModRandomClimaxTerminal || !g_ModRandomClimaxTerminal->obj) {
			// no computer could be placed: the targets were the mission
			sysLogPrintf(0, "randomizer: no valid climax pad; finishing after the targets");
			g_ModRandomClimaxDone = true;
			return;
		}

		g_ModRandomClimaxStarted = true;
		modRandomClimaxArmCivilians();
		g_ModRandomClimaxWaveAt60 = g_Vars.lvframe60 + TICKS(120);
		g_ModRandomClimaxWave2At60 = g_Vars.lvframe60 + TICKS(MODRANDOM_CLIMAX_WAVE2_60);
#ifndef PLATFORM_N64
		sysLogPrintf(0, "randomizer: climax computer at pad %d: %.0f %.0f %.0f", g_ModRandomClimaxPad,
				g_ModRandomClimaxPos.x, g_ModRandomClimaxPos.y, g_ModRandomClimaxPos.z);
#endif
		return;
	}

	if (g_ModRandomClimaxWave == 0 && g_Vars.lvframe60 >= g_ModRandomClimaxWaveAt60) {
		modRandomClimaxWave(0);
		g_ModRandomClimaxWave = 1;
	}

	if (g_ModRandomClimaxWave == 1 && g_Vars.lvframe60 >= g_ModRandomClimaxWave2At60) {
		modRandomClimaxWave(1);
		g_ModRandomClimaxWave = 2;
	}

	// the computer follows the zone's marking; pressing the button on it is all it takes
	if (g_ModRandomClimaxTerminal && g_ModRandomClimaxTerminal->obj) {
		struct defaultobj *obj = g_ModRandomClimaxTerminal->obj;

		// it may have settled onto the floor a little way from its pad
		g_ModRandomClimaxPos = g_ModRandomClimaxTerminal->pos;

		if (obj->hidden & (OBJHFLAG_ACTIVATED_BY_BOND | OBJHFLAG_ACTIVATED_BY_COOP)) {
			obj->hidden &= ~(OBJHFLAG_ACTIVATED_BY_BOND | OBJHFLAG_ACTIVATED_BY_COOP);
			g_ModRandomClimaxDone = true;
			return;
		}
	}

}

extern s32 g_DefaultWeapons[2];

bool randomMenuGauntletPageShow(void); // port/src/randommenu.c

#ifndef PLATFORM_N64
/**
 * Spawn Customizer Guaranteed Guards: one guard at each such spawn of the
 * chosen config as soon as the map is running, whatever the roll dealt (they
 * are made here, not from the setup's guards, so there is no limit but the
 * game's character slots). A spawn something is standing on is retried for a
 * few seconds.
 */
static void modRandomTickGuaranteed(void)
{
	const s32 n = modSpawnConfigCount(MODSPAWN_GUARANTEED);
	bool pending = false;
	s32 made = 0;
	s32 i;

	if (n <= 0 || g_ModRandomGuaranteedTicks < 0 || g_InCutscene || g_Vars.lvframenum < 3
			|| g_Vars.currentplayer != g_Vars.bond || !g_Vars.bond || !g_Vars.bond->prop) {
		return;
	}

	if (++g_ModRandomGuaranteedTicks > 600) {
		g_ModRandomGuaranteedTicks = -1; // gave up on any that are left
		return;
	}

	for (i = 0; i < n && i < 2048; i++) {
		struct prop *prop;

		if (g_ModRandomGuaranteedPlaced[i]) {
			continue;
		}

		if (chrsGetNumFree() < 4) {
			pending = true;
			continue;
		}

		prop = modRandomSpawnRoundNpc(modSpawnConfigPad(MODSPAWN_GUARANTEED, i), modRandomGuardBody(0), modRandomGuardHead(0), TEAM_ENEMY);

		if (!prop || !prop->chr) {
			pending = true;
			continue;
		}

		prop->chr->alertness = 0;
		prop->chr->target = -1;
		prop->chr->ailist = ailistFindById(GAILIST_UNALERTED);
		prop->chr->aioffset = 0;
		prop->chr->aireturnlist = -1;
		prop->chr->aishotlist = -1;
		chrStand(prop->chr);
		chrGiveWeapon(prop->chr, MODEL_CHRCMP150, WEAPON_CMP150, 0);
		modRandomDebugSpawn(prop->chr, MODDBG_ROUND);
		g_ModRandomGuaranteedPlaced[i] = true;
		made++;
	}

	if (made) {
		rebuildTeams();
		rebuildSquadrons();
		sysLogPrintf(LOG_NOTE, "spawnconfig: %d Guaranteed Guards stood up", made);
	}

	if (!pending) {
		g_ModRandomGuaranteedTicks = -1;
	}
}
#endif

#ifndef PLATFORM_N64
u64 g_ModPerfUs[28];
static u64 g_ModPrevPerf[28]; // g_ModPerfUs at the last tick (for the slow frame lines)
#endif

void modRandomTick(void)
{
#ifndef PLATFORM_N64
	// Frame-time watch for a mission: a frame that took long is logged with what
	// was being animated, so a stutter report comes with numbers (40 lines at most).
	{
		static u64 lastframe = 0;
		static s32 logged = 0;
		const u64 now = sysGetMicroseconds();

		// What the frame that just ended drew and how long the graphics handoff
		// took: the change in g_ModPerfUs since the last tick (it is zeroed once a
		// second below, where g_ModPrevPerf is zeroed with it)
		u64 fd[28];
		{
			s32 k;

			for (k = 0; k < 28; k++) {
				fd[k] = g_ModPerfUs[k] >= g_ModPrevPerf[k] ? g_ModPerfUs[k] - g_ModPrevPerf[k] : g_ModPerfUs[k];
				g_ModPrevPerf[k] = g_ModPerfUs[k];
			}
		}

		if (modRandomIsOn() && lastframe != 0 && g_Vars.lvframenum > 90 && !lvIsPaused() && logged < 150
				&& now - lastframe > 20000u && now - lastframe < 2000000u) {
			logged++;
			sysLogPrintf(0, "perf: slow frame %d ms (stage 0x%02x, frame %d): %d live and %d dead chrs animated, "
					"cap %d, %d free chr slots, %u decals stored; that frame drew %d live and %d dead chrs, %d objects, "
					"%d other props; chr draw build %.2f ms, graphics handoff %.2f ms, schedEndFrame %.2f ms, "
					"ragdoll step %.2f ms (%d stepping, %d solid)",
					(s32)((now - lastframe) / 1000u), g_Vars.stagenum, g_Vars.lvframenum, var8009cdb0, var8009cdac,
					modChrOnscreenCap(), chrsGetNumFree(), g_WallhitsNumUsed,
					(s32)fd[20], (s32)fd[21], (s32)fd[22], (s32)fd[24], fd[23] / 1000.0f, fd[10] / 1000.0f, fd[11] / 1000.0f,
					(fd[12] + fd[13] + fd[14] + fd[15] + fd[16]) / 1000.0f, (s32)fd[17], (s32)fd[18]);
		}

		// and every second how the frames are going, so a slowdown that comes
		// on as a level goes on shows as a trend (not only as the worst frames)
		{
			static u64 sampleat = 0;
			static u64 samplesum = 0;
			static s32 sampleframes = 0;
			static u64 sampleworst = 0;
			static s32 samplelong = 0; // frames over 20 ms

			if (modRandomIsOn() && lastframe != 0 && now - lastframe < 2000000u) {
				samplesum += now - lastframe;
				sampleframes++;

				if (now - lastframe > sampleworst) {
					sampleworst = now - lastframe;
				}

				if (now - lastframe > 20000u) {
					samplelong++;
				}
			}

			if (sampleat == 0 || g_Vars.lvframenum < 5) {
				sampleat = now;
				samplesum = 0;
				sampleframes = 0;
			} else if (now - sampleat >= 1000000u && sampleframes > 0) {
				s32 live = 0;
				s32 dead = 0;
				s32 i;

				for (i = 0; i < g_NumChrSlots; i++) {
					if (g_ChrSlots[i].chrnum >= 0 && g_ChrSlots[i].model && g_ChrSlots[i].prop) {
						if (chrIsDead(&g_ChrSlots[i])) {
							dead++;
						} else {
							live++;
						}
					}
				}

				if (modRandomIsOn()) {
					sysLogPrintf(0, "perf: stage 0x%02x frame %d: %.1f ms/frame avg (%.0f fps), %d live and %d dead chrs, "
							"%d free chr slots, %d animated last frame, worst frame %d ms, %d frames over 20 ms; "
							"per frame: tick %.2f (ragdolls %.2f: ceilings %.2f, grab %.2f, bodies %.2f, cuts %.2f, examine %.2f), "
							"player tick %.2f, render %.2f, swap %.2f ms",
							g_Vars.stagenum, g_Vars.lvframenum, (f32)samplesum / sampleframes / 1000.0f,
							1000000.0f * sampleframes / samplesum, live, dead, chrsGetNumFree(),
							var8009cdb0 + var8009cdac, (s32)(sampleworst / 1000u), samplelong,
							g_ModPerfUs[0] / 1000.0f / sampleframes, g_ModPerfUs[1] / 1000.0f / sampleframes,
							g_ModPerfUs[5] / 1000.0f / sampleframes, g_ModPerfUs[6] / 1000.0f / sampleframes,
							g_ModPerfUs[7] / 1000.0f / sampleframes, g_ModPerfUs[8] / 1000.0f / sampleframes,
							g_ModPerfUs[9] / 1000.0f / sampleframes,
							g_ModPerfUs[2] / 1000.0f / sampleframes, g_ModPerfUs[3] / 1000.0f / sampleframes,
							g_ModPerfUs[4] / 1000.0f / sampleframes);
				}

				if (modRandomIsOn()) {
					sysLogPrintf(0, "perf3: per frame: %.1f live chr draws, %.1f dead chr draws, %.1f object draws, "
							"%.1f other prop draws; chr draw build %.2f ms",
							(f32)g_ModPerfUs[20] / sampleframes, (f32)g_ModPerfUs[21] / sampleframes,
							(f32)g_ModPerfUs[22] / sampleframes, (f32)g_ModPerfUs[24] / sampleframes,
							g_ModPerfUs[23] / 1000.0f / sampleframes);
				}

				if (modRandomIsOn()) {
					sysLogPrintf(0, "perf2: wall %.2f ms/frame; the ragdoll step: pre loop %.2f, props %.2f, drive loop %.2f, "
							"physics step %.2f, post loop %.2f ms; bodies per frame: %.1f stepping, %.1f solid, %.1f asleep; "
							"display list handed over %.2f ms, schedEndFrame %.2f ms",
							(f32)samplesum / sampleframes / 1000.0f,
							g_ModPerfUs[12] / 1000.0f / sampleframes, g_ModPerfUs[13] / 1000.0f / sampleframes,
							g_ModPerfUs[14] / 1000.0f / sampleframes, g_ModPerfUs[15] / 1000.0f / sampleframes,
							g_ModPerfUs[16] / 1000.0f / sampleframes,
							(f32)g_ModPerfUs[17] / sampleframes, (f32)g_ModPerfUs[18] / sampleframes,
							(f32)g_ModPerfUs[19] / sampleframes,
							g_ModPerfUs[10] / 1000.0f / sampleframes, g_ModPerfUs[11] / 1000.0f / sampleframes);
				}

				sampleat = now;
				samplesum = 0;
				sampleframes = 0;
				sampleworst = 0;
				samplelong = 0;
				memset(g_ModPerfUs, 0, sizeof(g_ModPerfUs));
				memset(g_ModPrevPerf, 0, sizeof(g_ModPrevPerf));
			}
		}

		lastframe = now;
	}

	// A Gauntlet's closing screen sends the player to the Institute: open the
	// Gauntlet page there once they are in control
	if (modCareerGauntletPageDue()) {
		if (g_Vars.stagenum != STAGE_CITRAINING) {
			if (g_Vars.lvframenum > 5) {
				modCareerGauntletPageDone(); // somewhere else: not this time
			}
		} else if (g_Vars.lvframenum > 40 && g_FileState == FILESTATE_SELECTED && randomMenuGauntletPageShow()) {
			modCareerGauntletPageDone();
		}
	}
#endif

#ifndef PLATFORM_N64
	// Diagnostic (run with --log): the gun's state in an arena mission
	if (modRunIsArenaMission() && g_Vars.currentplayer && g_Vars.lvframenum % 90 == 5 && g_Vars.lvframenum < 900) {
		struct player *pl = g_Vars.currentplayer;

		sysLogPrintf(LOG_NOTE, "armdiag f%d: weapon=%d switchto=%d prev=%d dual=%d defaults=%d/%d "
				"owner=%d memnew=%d memtype=%d lock=%d memload=%d gunload=%d loadedammo=%d inuse=%d haschrbody=%d lockscreen=%d paused=%d",
				g_Vars.lvframenum, pl->gunctrl.weaponnum, pl->gunctrl.switchtoweaponnum,
				pl->gunctrl.prevweaponnum, pl->gunctrl.dualwielding,
				g_DefaultWeapons[HAND_LEFT], g_DefaultWeapons[HAND_RIGHT],
				pl->gunctrl.gunmemowner, pl->gunctrl.gunmemnew, pl->gunctrl.gunmemtype, pl->gunctrl.gunlocktimer,
				pl->gunctrl.masterloadstate, pl->gunctrl.gunloadstate,
				pl->hands[HAND_RIGHT].loadedammo[0], pl->hands[HAND_RIGHT].inuse,
				pl->haschrbody, g_Vars.lockscreen, lvIsPaused());
	}
#endif

	if (modCareerIsOn()) {
		modCareerTick();
	}

	if (modRunIsArenaMission() && !g_InCutscene && g_Vars.currentplayer == g_Vars.bond) {
		const s32 guardlimit = modRandomArenaGuardLimit();
		for (s32 id = 1000; id < 1000 + guardlimit; id++) {
			struct chrdata *chr = chrFindByLiteralId(id);
			if (g_ModRandomArenaArmed & (1ull << (id - 1000))) continue;
			if (chr && chr->model && chr->prop && !chrIsDead(chr)) {
				if (!chr->weapons_held[HAND_RIGHT]) chrGiveWeapon(chr, MODEL_CHRCMP150, WEAPON_CMP150, 0);
				g_ModRandomArenaArmed |= 1ull << (id - 1000);
			}
		}
	}
	if (modRandomIsOn() && !g_InCutscene && g_Vars.lvframenum >= 2
			&& g_Vars.currentplayer == g_Vars.bond) {
		s32 i;
		for (i = 0; i < g_ModRandomNumPatrols; i++) {
			struct modrandompatrol *patrol = &g_ModRandomPatrols[i];
			struct chrdata *chr;
			if (!patrol->pending) continue;
			patrol->pending = false;
			chr = chrFindByLiteralId(patrol->chrnum);
			if (chr && chr->prop && chr->model && chr->alertness < 100
					&& chr->team != TEAM_ALLY && chr->actiontype == ACT_STAND
					&& !(chr->hidden & CHRHFLAG_DELETING)) {
				chrStartPatrol(chr, &patrol->path);
			}
		}
	}
	modRandomTickSpentItem();
	modRandomTickTargetRescue();
	modRandomTickEndless();
	modRandomTickCivilians();
	modRandomTickRounds();
	modRandomTickObjectiveShield();
#ifndef PLATFORM_N64
	if (modRandomIsOn()) modRandomTickGuaranteed();
#endif
	modRandomTickClimax();
	modRandomTickObjectiveBanner();
	modCareerTickMods();

	// A regular Random Mission is won as soon as its generated kill objective
	// is complete. Endless and Randomizer Run have their own completion rules.
	if (modRandomIsOn() && !modRandomIsEndless() && !modRunIsOn()
			&& modRandomGetVersion() >= 8 && !g_ModRandomRunOver && !g_ModRandomAlienWarActive
			&& g_ModRandomNumObjectives > 0 && g_Vars.currentplayer == g_Vars.bond
			&& !g_InCutscene && !g_Vars.currentplayer->isdead
			&& modRandomAllObjectivesComplete()) {
		if (g_ModRandomEndDelay60 == 0) {
			// Leave the completed level playable briefly so the final kill,
			// boon and guard reaction are visible before the results screen.
			g_ModRandomEndDelay60 = g_Vars.lvframe60 + TICKS(300);
			hudmsgCreateWithFlags("Mission complete. Extracting in 5 seconds...\n",
				HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
		} else if (g_Vars.lvframe60 >= g_ModRandomEndDelay60
#ifndef PLATFORM_N64
				&& (g_ModRandomEventNotice240 <= 0 || g_ModRandomEventNoticeEnding)
#endif
				) {
			g_ModRandomRunOver = true;
			// Perfect Random Gauntlet: carry straight on into the next mission
			// instead of ending at the results screen. False on the last one.
			if (modCareerOnMissionComplete()) {
				return;
			}
			mainEndStage();
#ifndef PLATFORM_N64
			if (sysArgCheck("--random-round-test")) {
				sysLogPrintf(0, "random-round-test: PASS %d targets complete; endscreen %d", g_ModRandomCleared, g_MainIsEndscreen);
				exit(g_MainIsEndscreen ? 0 : 1);
			}
#endif
			return;
		}
	}

}

/**
 * The run: count the ground covered, deal the next objective when the live one
 * is finished, and end at the first death.
 *
 * Everything the deal needs was kept from the level's roll - the lists point
 * into the setup stream and the walk into the stage pool, both of which live
 * as long as the level does - so dealing another objective mid-mission is the
 * same work the roll did, minus the rewriting.
 */
static void modRandomTickEndless(void)
{
	struct player *player = g_Vars.currentplayer;
	s32 room;

	if (!modRandomIsEndless() || g_ModRandomRunOver || g_ModRandomVisited == NULL) {
		return;
	}

	if (player != g_Vars.bond || g_InCutscene) {
		return;
	}

	if (player->isdead) {
		static char text[64];

		g_ModRandomRunOver = true;

		if (g_ModRandomRooms > g_ModOptions.endlessbest) {
			g_ModOptions.endlessbest = g_ModRandomRooms;
			snprintf(text, sizeof(text), langTr("%d rooms, %d objectives - best yet\n"), g_ModRandomRooms, g_ModRandomCleared);
		} else {
			snprintf(text, sizeof(text), langTr("%d rooms, %d objectives (best %d)\n"),
					g_ModRandomRooms, g_ModRandomCleared, g_ModOptions.endlessbest);
		}

		// Four seconds: the death fade is a second and a bit, and a score the
		// player cannot read is not a score.
		{
			extern struct hudmsgtype g_HudmsgTypes[];
			hudmsgCreateWithDuration(text, HUDMSGTYPE_DEFAULT, &g_HudmsgTypes[HUDMSGTYPE_DEFAULT], TICKS(240));
		}

		return;
	}

	// Ground covered. rooms[0] is where the player is standing; a room already
	// stood in is not covered again, so pacing back and forth scores nothing.
	room = player->prop->rooms[0];

	if (room > 0 && room < g_Vars.roomcount && !g_ModRandomVisited[room]) {
		g_ModRandomVisited[room] = 1;
		g_ModRandomRooms++;
	}

	// The live objective, and the next one. Dealt in the same tick it is
	// finished, so objectiveIsAllComplete() is never true for a whole frame -
	// which is what the stage's own exit trigger asks, and an endless run has
	// no business ending at the exit.
	if (g_ModRandomNumObjectives > 0 && objectiveCheck(0) == OBJECTIVE_COMPLETE) {
		static char text[96];

		g_ModRandomCleared++;

		if (modRandomDealObjective(&g_ModRandomLists, g_ModRandomReached, 0, g_ModRandomDealt)) {
			g_ModRandomDealt++;
			g_ObjectiveStatuses[0] = OBJECTIVE_INCOMPLETE;

			// The score rides along with the new objective: a run's number is
			// no use to the player on the death screen, where the fade and
			// the failure dialog are what they are looking at.
			snprintf(text, sizeof(text), langTr("%s - %d rooms\n"), g_ModRandomObjText[0], g_ModRandomRooms);
			hudmsgCreateWithFlags(text, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
		} else {
			// Nothing left to ask for on this stage. The run stands on what it
			// scored rather than looping an objective the player has done -
			// and says so, because a run that quietly stops dealing looks
			// like a bug from the inside.
			snprintf(text, sizeof(text), langTr("Nothing left to find - %d rooms, %d objectives\n"),
					g_ModRandomRooms, g_ModRandomCleared);
			hudmsgCreateWithFlags(text, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);

			if (g_ModRandomRooms > g_ModOptions.endlessbest) {
				g_ModOptions.endlessbest = g_ModRandomRooms;
			}

			g_ModRandomRunOver = true;
		}
	}
}

/**
 * The spot playerReset() should use at the start of a dealt mission.
 *
 * Answers once: the same call is what marks the move done, so a later death
 * spawns where the game would have spawned it.
 */
/**
 * A start or a landing on a level converted from GoldenEye, moved off what
 * stands on its pad.
 *
 * The pad is dealt at the roll, where the level's props do not exist yet
 * (modRandomPadCanSpawn()), and a GoldenEye arena stands doors on pads it also
 * names as spots: Goldfinger 64's China has one on pad 45, and a run landing
 * there put the player inside the door, where every step was a collision and
 * he could not move at all (F3 20261005-095105). So once the props stand, a
 * place a body of the player's width (thirty) is not clear at - a wall of the
 * tile graph or a prop's edge within reach - moves where GoldenEye puts a chr
 * teleported to a pad: the first of eight points sixty units out, from the
 * pad's own facing, that the floor reaches and that is clear
 * (geStanTeleportSpot()). Which pad the seed dealt does not change, only where
 * on it the player stands, as with the floor's own y above.
 */
/**
 * The same on Perfect Dark's own levels, where what stands on a pad is
 * furniture: Carrington Institute's sofa covers a waypoint the run landed on,
 * and the player started inside it, every step a collision (F3
 * 20261011-012426). The pad is dealt at the roll, before the props stand, so
 * this asks once they do: the body's own volume clear of everything but
 * players (the player's own prop is still wherever the last life left it),
 * and if not, the nearest of eight directions at 60, 120, 180 and 240 units
 * that the bg lets a line reach from the pad, that has a floor within a step
 * of the pad's and is clear. chrAdjustPosForSpawn()'s search, wider, since a
 * sofa is longer than 60 units. Nothing clear leaves the spot as it was.
 */
#ifndef PLATFORM_N64
static void modRandomSpawnClearProps(struct coord *pos, RoomNum *rooms, f32 angle)
{
	const u32 types = CDTYPE_ALL & ~CDTYPE_PLAYERS;
	s32 ring;
	s32 i;

	if (cdTestVolume(pos, 30, rooms, types, CHECKVERTICAL_YES, 200, -200) != CDRESULT_COLLISION) {
		return;
	}

	for (ring = 1; ring <= 4; ring++) {
		f32 curangle = angle;

		for (i = 0; i < 8; i++) {
			struct coord testpos;
			RoomNum testrooms[8];
			f32 ground;

			testpos.x = pos->x + sinf(curangle) * (60.0f * ring);
			testpos.y = pos->y;
			testpos.z = pos->z + cosf(curangle) * (60.0f * ring);

			curangle += M_BADTAU / 8;

			if (curangle >= M_BADTAU) {
				curangle -= M_BADTAU;
			}

			testrooms[0] = -1;

			if (!cdTestLos11(pos, rooms, &testpos, testrooms, CDTYPE_BG) || testrooms[0] <= 0) {
				continue;
			}

			chr0f021fa8(NULL, &testpos, testrooms);

			if (testrooms[0] <= 0) {
				continue;
			}

			ground = cdFindGroundAtCyl(&testpos, 30, testrooms, NULL, NULL);

			if (ground < -100000 || ground + MODRANDOM_SPAWNLIFT - pos->y > 40
					|| pos->y - (ground + MODRANDOM_SPAWNLIFT) > 40) {
				continue;
			}

			testpos.y = ground + MODRANDOM_SPAWNLIFT;

			if (cdTestVolume(&testpos, 30, testrooms, types, CHECKVERTICAL_YES, 200, -200) == CDRESULT_COLLISION) {
				continue;
			}

			sysLogPrintf(LOG_NOTE, "randomizer: the spot (%.0f %.0f %.0f) is inside a prop; standing at (%.0f %.0f %.0f) in room %d",
					pos->x, pos->y, pos->z, testpos.x, testpos.y, testpos.z, testrooms[0]);

			*pos = testpos;
			rooms[0] = testrooms[0];
			rooms[1] = -1;
			return;
		}
	}

	sysLogPrintf(LOG_NOTE, "randomizer: the spot (%.0f %.0f %.0f) is inside a prop and nothing near it is clear",
			pos->x, pos->y, pos->z);
}
#endif

void modRandomSpawnClear(struct coord *pos, RoomNum *rooms, f32 angle)
{
#ifndef PLATFORM_N64
	struct prop *self = g_Vars.currentplayer ? g_Vars.currentplayer->prop : NULL;
	struct coord spot;
	s32 room;

	if (rooms[0] <= 0) {
		return;
	}

	if (!geRoomActive()) {
		modRandomSpawnClearProps(pos, rooms, angle);
		return;
	}

	if (geStanTeleportSpot(pos, rooms[0], angle, 30, self, &spot) <= 0
			|| (spot.x == pos->x && spot.z == pos->z)) {
		return;
	}

	room = geStanTileRoomAt(&spot, rooms[0]);

	sysLogPrintf(LOG_NOTE, "randomizer: the spot (%.0f %.0f %.0f) is not clear; standing at (%.0f %.0f %.0f) in room %d",
			pos->x, pos->y, pos->z, spot.x, pos->y, spot.z, room > 0 ? room : rooms[0]);

	pos->x = spot.x;
	pos->z = spot.z;

	if (room > 0) {
		rooms[0] = room;
		rooms[1] = -1;
	}
#endif
}

bool modRandomTakeSpawn(struct coord *pos, RoomNum *rooms, f32 *angle)
{
	struct pad pad;

	if (!g_ModRandomSpawnPending || !modRandomIsOn() || g_ModRandomSpawnPad < 0
			|| g_Vars.currentplayer != g_Vars.bond) {
		return false;
	}

	g_ModRandomSpawnPending = false;

	padUnpack(g_ModRandomSpawnPad, PADFIELD_POS | PADFIELD_ROOM | PADFIELD_LOOK, &pad);

	// The floor's own y, not the pad's: the game's ground query takes the
	// highest floor strictly below the position it is handed, and a pad level
	// with its own floor misses it. Every version gets this - it does not
	// change which pad the seed dealt, only where on it the player stands.
	if (!modRandomPadSpawnPos(g_ModRandomSpawnPad, pos, &rooms[0])) {
		pos->x = pad.pos.x;
		pos->y = pad.pos.y;
		pos->z = pad.pos.z;

		rooms[0] = pad.room;
	}

	rooms[1] = -1;

	*angle = atan2f(pad.look.x, pad.look.z);

	modRandomSpawnClear(pos, rooms, *angle);

#ifndef PLATFORM_N64
	if (modSpawnConfigIsPadOfKind(MODSPAWN_ESCAPE, g_ModRandomSpawnPad)) {
		sysLogPrintf(LOG_NOTE, "spawnconfig: placed player at Escape/Spawn pad %d in room %d (%.1f %.1f %.1f)",
				g_ModRandomSpawnPad, rooms[0], pos->x, pos->y, pos->z);
	}
#endif

	return true;
}

/**
 * Replace the stage's objectives with the generated ones.
 *
 * Called at the end of setupCreateProps(), once the stream's own objectives
 * have been inserted: they are cleared here rather than skipped during the
 * walk so that everything else the walk does with them - the briefing text,
 * the tags they name - happens exactly as it always did, and only the list
 * the game checks is ours.
 */
void modRandomInsertObjectives(void)
{
	s32 i;
#ifndef PLATFORM_N64
	// The Spawn Customizer has no objectives: nothing to complete, nothing to fail
	if (modSpawnEditIsOn()) {
		for (i = 0; i < MAX_OBJECTIVES; i++) {
			g_Objectives[i] = NULL;
			g_ObjectiveStatuses[i] = OBJECTIVE_INCOMPLETE;
		}

		for (i = 0; i < ARRAYCOUNT(g_Briefing.objectivenames); i++) {
			g_Briefing.objectivenames[i] = 0;
			g_Briefing.objectivedifficulties[i] = 0;
		}

		g_ObjectiveLastIndex = -1;
		return;
	}
#endif

	if (!modRandomIsOn() || g_ModRandomNumObjectives == 0) {
		return;
	}

	for (i = 0; i < MAX_OBJECTIVES; i++) {
		g_Objectives[i] = NULL;
		g_ObjectiveStatuses[i] = OBJECTIVE_INCOMPLETE;
	}

	for (i = 0; i < ARRAYCOUNT(g_Briefing.objectivenames); i++) {
		g_Briefing.objectivenames[i] = 0;
		g_Briefing.objectivedifficulties[i] = 0;
	}

	g_ObjectiveLastIndex = -1;

	// Re-insert from the buffer the roll wrote, which is where the objective
	// structs and their requirements live. One fixed block each, because
	// Endless Mode rewrites a block in place while the others stand: packing
	// them end to end would mean an objective's length deciding where the next
	// one lives, and a re-deal moving every objective after it.
	for (i = 0; i < g_ModRandomNumObjectives; i++) {
		struct objective *objective = modRandomObjectiveAt(i);

		if (objective == NULL) {
			break;
		}

		objectiveInsert(objective);

		g_Briefing.objectivenames[objective->index] = L_MISC_042;
		g_Briefing.objectivedifficulties[objective->index] = objective->difficulties;
	}
}
