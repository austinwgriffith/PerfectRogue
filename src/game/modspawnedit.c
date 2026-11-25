#include <ultra64.h>
#include "constants.h"
#include "prcompat.h"
#include "math.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <dirent.h>
#include "game/atan2f.h"
#include "game/bg.h"
#include "game/bondmove.h"
#include "game/bondwalk.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/modoptions.h"
#include "lib/ailist.h"
#include "game/game_152fa0.h"
#include "game/game_1531a0.h"
#include "game/lv.h"
#include "game/modrandom.h"
#include "game/modrun.h"
#include "game/modspawnedit.h"
#include "game/pad.h"
#include "game/player.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "game/setup.h"
#include "game/setuputils.h"
#include "bss.h"
#include "gbiex.h"
#include "lib/collision.h"
#include "lib/main.h"
#include "lib/vars.h"
#include "lib/vi.h"
#include "data.h"
#include "types.h"
#include "system.h"
#include "input.h"
#include "fs.h"
#include "lib/memp.h"
#include "game/pad.h"

#ifndef PLATFORM_N64

/**
 * THE SPAWN CUSTOMIZER
 *
 * Entered from Perfect Random > Spawn Customizer. The map loads as a Combat
 * Map mission does, except that the roll is not dealt: no guards, no
 * objectives, no reinforcement waves, no events. The player flies (no gravity,
 * no collision) and every place the game would use is drawn as a marker.
 *
 * What the markers are, for the map's default arrangement:
 *
 *   GUARD      the pads a Combat Map mission stands its guards on: waypoint
 *              pads with a route out that a player can stand on, plus any pad
 *              a guard of the stage's own stands on
 *   HUNTER     every waypoint, which is the pool reinforcement waves draw
 *              from (modalarm.c, modAlarmSpawnOne)
 *   OBJECTIVE  the pads the stage's loose weapons and ammo lie on, which is
 *              where a pickup objective can ask for something
 *   ESCAPE     the pad the stage's intro starts the player on
 *
 * Positions are stored as positions rather than pad numbers, because a spawn
 * a player places has no pad. See claude/spawn-customizer-plan.md.
 *
 * Phase 2, editing. The mouse and the keys are read here directly, not through
 * the player's binds, and bondmove.c keeps the gun from firing, aiming or
 * cycling while the editor is open - so a saved bind for Fire on Space (or
 * anything else) cannot shoot, and the buttons are free for editing:
 *
 *   left mouse     place a spawn of the chosen kind where the crosshair hits
 *   right mouse    delete the marker under the crosshair
 *   1 2 3 4        choose the kind: Guard, Hunter, Objective, Escape/Spawn
 *   mouse wheel    turn the spawn about to be placed (15 degrees a notch)
 *   , and .        turn the marker under the crosshair (hold)
 *   Backspace, U   undo the last place, delete or turn (one step)
 *   Space, Ctrl    fly up, down; Shift flies fast
 *
 * Everything here is gated on g_ModSpawnEditOn, which is only set by
 * modSpawnEditStart() and cleared by every other way of starting a stage.
 */

#define MODSPAWN_MAX      2048
#define MODSPAWN_DRAWDIST 6000.0f // markers further than this are not drawn
#define MODSPAWN_DELETERANGE 800.0f // markers further than this cannot be deleted or turned
#define MODSPAWN_PICKPX   28      // how close to the crosshair counts as aimed at
#define MODSPAWN_RAYLEN   5000.0f // how far the crosshair reaches
#define MODSPAWN_LIFT     10.0f   // a spawn stands this far above its floor
#define MODSPAWN_NOGROUND (-100000.0f)
#define MODSPAWN_MAXDROP  400.0f  // a ceiling or ledge this far above a floor is no spawn
#define MODSPAWN_TURNRATE 2.0f    // radians a second while , or . is held

#define MODSPAWN_NAMELEN  24
#define MODSPAWN_UNDO_NONE   0
#define MODSPAWN_UNDO_ADDED  1 // undo removes spawn [index]
#define MODSPAWN_UNDO_DELETED 2 // undo puts the saved spawn back at [index]
#define MODSPAWN_UNDO_TURNED 3 // undo puts the saved angle back on [index]
#define MODSPAWN_UNDO_CLEARED 4

struct modspawn {
	u32 id; // never reused, so a model can follow its spawn while the array shifts
	u8 kind;
	s16 pad; // -1 for a spawn that was placed, not found
	f32 angle;
	RoomNum room;
	struct coord pos;
};

s32 g_SpawnEditTestStage = -1; // --selftest-spawnedit: the stage to test, or -1
static bool g_ModSpawnEditOn;
static bool g_ModSpawnEditLoaded;
static s32 g_ModSpawnEditStage;
static struct modspawn g_ModSpawns[MODSPAWN_MAX];
static s32 g_ModNumSpawns;
static s32 g_ModSpawnCounts[MODSPAWN_NUMKINDS];
static u32 g_ModSpawnNextId;

// The characters standing at the spawns nearest the player
#define MODSPAWN_MODELS_MAX (MODSPAWN_EDIT_CHRSLOTS_MAX - 4)
#define MODSPAWN_MODELS    (modSpawnEditChrSlots() - 4) // characters that may stand at once; 4 slots are left for the placement preview
#define MODSPAWN_GHOSTALPHA 255 // 255 draws in the solid pass; anything less goes through the see-through pass
#define MODSPAWN_LAPTOPSCALE 0.2f // the laptop's size: the same as a mission's final computer
#define MODSPAWN_MODELDIST 6000.0f // a spawn further than this (or past the pool) is a small square
#define MODSPAWN_OVERLAP   70.0f   // spawns this close share a spot
#define MODSPAWN_SIDESTEP  55.0f   // and stand this far apart
#define MODSPAWN_PERNEW    6       // characters made per pass: each loads a model

struct modspawnmodel {
	u32 id; // 0 when the slot is free
	struct prop *prop;
	s32 chrnum;
	struct defaultobj *obj; // a laptop, for Escape/Spawn; NULL for a character
	u8 kind;
	struct coord pos; // where it stands, which is not the spawn's when spawns overlap
};

/** Character slots the editor asks the stage for: always the most it can stand (800 guards and the previews). */
s32 modSpawnEditChrSlots(void)
{
	return MODSPAWN_EDIT_CHRSLOTS_MAX;
}

static struct modspawnmodel g_ModSpawnModels[MODSPAWN_MODELS_MAX];
#define MODSPAWN_LAPTOPS 32

static struct {
	u32 id;           // the spawn it stands for; 0 when unused
	struct coord pos; // where it stands
	f32 angle;
} g_ModSpawnLaptops[MODSPAWN_LAPTOPS];

static struct defaultobj g_ModSpawnLaptopObjs[MODSPAWN_LAPTOPS];

static struct defaultobj g_ModSpawnObjs[MODSPAWN_MODELS_MAX + 1]; // the laptops, and the last is the preview's

// The placement preview: one see-through model where a click would put a spawn
static struct prop *g_ModSpawnGhostChr[MODSPAWN_NUMKINDS];
static s32 g_ModSpawnGhostChrnum[MODSPAWN_NUMKINDS];
static struct prop *g_ModSpawnGhostLaptop;
static struct coord g_ModSpawnGhostAt[MODSPAWN_NUMKINDS];
static f32 g_ModSpawnGhostAngle[MODSPAWN_NUMKINDS];
static bool g_ModSpawnGhostPlaced[MODSPAWN_NUMKINDS];
static s32 g_ModSpawnGhostFailUntil;
static struct coord g_ModSpawnLastGood; // the last place the camera had a room
static bool g_ModSpawnLastGoodSet;
static bool g_ModSpawnFlying; // Space takes off; landing on a floor, or F, walks again
static f32 g_ModSpawnPlaceTurn; // what the mouse wheel has added to the way a new spawn faces

/** The way a spawn placed now faces: the way the player looks, turned by the wheel. */
static f32 modSpawnEditPlaceAngle(void)
{
	f32 angle = atan2f(g_Vars.currentplayer->bond2.unk00.x, g_Vars.currentplayer->bond2.unk00.z) + g_ModSpawnPlaceTurn;

	while (angle > M_PI) {
		angle -= 2.0f * M_PI;
	}

	while (angle < -M_PI) {
		angle += 2.0f * M_PI;
	}

	return angle;
}

// Whether a marker can be seen from the eye, found with a line of sight and
// kept a few frames so that hundreds of markers cost a few tests a frame. The
// array is by index, so any change to the list forgets it.
#define MODSPAWN_LOS_BUDGET 40 // tests a frame
#define MODSPAWN_LOS_STALE  8  // frames before a result is asked again
static u8 g_ModSpawnSeen[MODSPAWN_MAX];
static s32 g_ModSpawnSeenFrame[MODSPAWN_MAX]; // 0: never asked
static s32 g_ModSpawnNextModelPass;
static s32 g_ModSpawnModelFailUntil;
static s32 g_ModSpawnLostCount;
static s32 g_ModSpawnLaptopFailUntil; // laptops have their own: a full character pool must not block them

static s32 g_ModSpawnPlaceKind;  // what the left mouse places
static s32 g_ModSpawnAimed = -1; // the marker under the crosshair, from the last frame drawn
static s32 g_ModSpawnTurning = -1;
static Gfx *modSpawnEditLine(Gfx *gdl, s32 x, s32 y, char *text, u32 colour);
static void modSpawnEditLoadConfigByChoice(void);
static bool modSpawnEditPlay(void);
static struct modspawn g_ModSpawnDefaults[MODSPAWN_MAX]; // what the map loaded with
static s32 g_ModNumDefaults;
static char g_ModSpawnConfigName[MODSPAWN_NAMELEN + 1] = "Default";
static s32 g_ModSpawnMenu;
static char g_ModCfgChoices[STAGE_MAX_ID + 1][MODSPAWN_NAMELEN + 1];
static char g_ModCfgName[MODSPAWN_NAMELEN + 1]; // the config loaded for the current mission
static bool modSpawnEditLoadBuiltinDefault(s32 stagenum, bool updateDefaults);
void modSpawnConfigUse(s32 stagenum, const char *name);
bool modSpawnConfigIsChosen(s32 stagenum);
static s32 g_ModSpawnPlayReturn = -1; // the map a Play is running on: its end goes back to the editor
static bool g_ModSpawnDirty;     // anything placed, deleted or turned since the map loaded

static struct {
	s32 type;
	s32 index;
	struct modspawn spawn;
} g_ModSpawnUndo;
static struct modspawn g_ModSpawnBeforeClear[MODSPAWN_MAX];
static s32 g_ModSpawnCountBeforeClear;

static char g_ModSpawnMsg[96];
static s32 g_ModSpawnMsgUntil60;

static const char *g_ModSpawnNames[MODSPAWN_NUMKINDS] = {
	"Guard", "Hunter", "Objective", "Escape/Spawn", "Guaranteed Guard",
};

static const u32 g_ModSpawnColours[MODSPAWN_NUMKINDS] = {
	0xff3030ff, // guard: red
	0xff8000ff, // hunter: orange
	0xffe000ff, // objective: yellow
	0x30ff30ff, // escape: green
	0x20ffc0ff, // guaranteed guard: green-teal
};

bool modSpawnEditIsOn(void)
{
	return g_ModSpawnEditOn;
}

void modSpawnEditClear(void)
{
	g_ModSpawnPlayReturn = -1; // any other mission starting forgets it
	g_ModSpawnEditOn = false;
	g_ModSpawnEditLoaded = false;
	g_ModNumSpawns = 0;
	g_ModSpawnAimed = -1;
	g_ModSpawnTurning = -1;
	g_ModSpawnDirty = false;
	g_ModSpawnMenu = 0;
	g_ModSpawnUndo.type = MODSPAWN_UNDO_NONE;
	memset(g_ModSpawnModels, 0, sizeof(g_ModSpawnModels)); // the stage and its characters are going
	memset(g_ModSpawnGhostChr, 0, sizeof(g_ModSpawnGhostChr));
	memset(g_ModSpawnLaptops, 0, sizeof(g_ModSpawnLaptops));
	memset(g_ModSpawnGhostPlaced, 0, sizeof(g_ModSpawnGhostPlaced));
	g_ModSpawnGhostLaptop = NULL;
	g_ModSpawnPlaceTurn = 0;
	g_ModSpawnLastGoodSet = false;
	g_ModSpawnFlying = false;
}

/**
 * Open the editor on a map. The map is started the way a Combat Map mission
 * is; the flag is set afterwards because starting a mission clears it.
 */
void modSpawnEditStart(s32 stagenum)
{
	bool started;
	bool solo = false;
	s32 i;

	modSpawnEditClear();

	for (i = 0; i < NUM_SOLOSTAGES; i++) {
		if ((s32)g_SoloStages[i].stagenum == stagenum) {
			solo = true;
		}
	}

	if (solo) {
		// a Perfect Dark mission: its own stage, nothing dealt
		started = modRunStartEditorMission(stagenum);
	} else {
		modRunStartArenaMission(stagenum);
		started = modRunIsArenaMission();
	}

	if (started) {
		g_ModSpawnEditOn = true;
		g_ModSpawnEditStage = stagenum;
		g_ModSpawnPlaceKind = MODSPAWN_GUARD;
		sysLogPrintf(LOG_NOTE, "spawnedit: opening stage 0x%02x", stagenum);
	}
}

static void modSpawnEditSay(const char *text)
{
	if (g_Vars.lvframe60 < g_ModSpawnMsgUntil60 && strcmp(g_ModSpawnMsg, text) == 0) {
		return; // already on screen: do not repeat it in the log every tick
	}

	snprintf(g_ModSpawnMsg, sizeof(g_ModSpawnMsg), "%s", text);
	sysLogPrintf(LOG_NOTE, "spawnedit: %s", text);
	g_ModSpawnMsgUntil60 = g_Vars.lvframe60 + 60 * 4;
}

static void modSpawnEditRecount(void)
{
	s32 i;

	memset(g_ModSpawnCounts, 0, sizeof(g_ModSpawnCounts));

	for (i = 0; i < g_ModNumSpawns; i++) {
		g_ModSpawnCounts[g_ModSpawns[i].kind]++;
	}
}

static bool modSpawnEditPadOk(s32 padnum)
{
	return padnum >= 0 && g_PadsFile != NULL && padnum < g_PadsFile->numpads;
}

static void modSpawnEditAdd(u8 kind, s32 padnum, bool snap)
{
	struct modspawn *spawn;
	struct pad pad;
	struct coord pos;
	RoomNum room;

	if (g_ModNumSpawns >= MODSPAWN_MAX || !modSpawnEditPadOk(padnum)) {
		return;
	}

	padUnpack(padnum, PADFIELD_POS | PADFIELD_LOOK | PADFIELD_ROOM, &pad);

	pos = pad.pos;
	room = pad.room;

	// The floor's own y, not the pad's, as a mission's start does
	if (snap && modRandomPadSpawnPos(padnum, &pos, &room)) {
		// pos and room are the floor's
	}

	spawn = &g_ModSpawns[g_ModNumSpawns++];
	spawn->id = ++g_ModSpawnNextId;
	spawn->kind = kind;
	spawn->pad = padnum;
	spawn->pos = pos;
	spawn->room = room;
	spawn->angle = atan2f(pad.look.x, pad.look.z);
	g_ModSpawnCounts[kind]++;
}

static bool modSpawnEditHasPad(u8 kind, s32 padnum)
{
	s32 i;

	for (i = 0; i < g_ModNumSpawns; i++) {
		if (g_ModSpawns[i].kind == kind && g_ModSpawns[i].pad == padnum) {
			return true;
		}
	}

	return false;
}

/**
 * Called from modRandomRoll() while the stage is being built, after the pads
 * and waypoints exist and before anything is made from the setup stream.
 * True when the editor has the stage, in which case the roll is not dealt.
 */
bool modSpawnEditOnStageLoad(s32 stagenum)
{
	struct waypoint *waypoints = g_StageSetup.waypoints;
	u32 *cmd = g_StageSetup.props;
	s32 i;

	if (!g_ModSpawnEditOn) {
		return false;
	}

	g_ModNumSpawns = 0;
	memset(g_ModSpawnCounts, 0, sizeof(g_ModSpawnCounts));
	g_ModSpawnEditStage = stagenum;
	g_ModSpawnEditLoaded = true;
	g_ModSpawnAimed = -1;
	g_ModSpawnTurning = -1;
	g_ModSpawnDirty = false;
	g_ModSpawnUndo.type = MODSPAWN_UNDO_NONE;
	memset(g_ModSpawnModels, 0, sizeof(g_ModSpawnModels));
	// The old stage is gone, and with it every laptop prop: forget them here
	// (not in modSpawnEditClear(), when the old stage may still tick them)
	memset(g_ModSpawnLaptops, 0, sizeof(g_ModSpawnLaptops));
	memset(g_ModSpawnLaptopObjs, 0, sizeof(g_ModSpawnLaptopObjs));
	memset(g_ModSpawnObjs, 0, sizeof(g_ModSpawnObjs));
	memset(g_ModSpawnGhostChr, 0, sizeof(g_ModSpawnGhostChr));
	memset(g_ModSpawnGhostPlaced, 0, sizeof(g_ModSpawnGhostPlaced));
	g_ModSpawnGhostLaptop = NULL;
	g_ModSpawnNextModelPass = 0;
	g_ModSpawnModelFailUntil = 0;

	// The stage's own start
	modSpawnEditAdd(MODSPAWN_ESCAPE, modRandomEditStartPad(), true);

	// Guards: the pads a Combat Map mission stands them on
	if (waypoints) {
		for (i = 0; waypoints[i].padnum >= 0; i++) {
			s32 padnum = waypoints[i].padnum;

			if (waypoints[i].neighbours && waypoints[i].neighbours[0] >= 0
					&& modSpawnEditPadOk(padnum) && modRandomPadCanSpawn(padnum)) {
				modSpawnEditAdd(MODSPAWN_GUARD, padnum, true);
			}
		}

		// Hunters: every waypoint with a route out
		for (i = 0; waypoints[i].padnum >= 0; i++) {
			if (waypoints[i].neighbours && waypoints[i].neighbours[0] >= 0) {
				modSpawnEditAdd(MODSPAWN_HUNTER, waypoints[i].padnum, false);
			}
		}
	}

	// The stage's own setup: its guards' pads, its loose pickups' pads. The
	// guards themselves are taken out, the way the roll takes out an idle
	// population, so that nothing in the editor shoots at the player.
	while (cmd && ((struct defaultobj *)cmd)->type != OBJTYPE_END) {
		struct defaultobj *obj = (struct defaultobj *)cmd;

		if (obj->type == OBJTYPE_CHR) {
			struct packedchr *chr = (struct packedchr *)cmd;

			if (chr->chrnum >= 0) {
				if (modSpawnEditPadOk(chr->padnum) && !modSpawnEditHasPad(MODSPAWN_GUARD, chr->padnum)) {
					modSpawnEditAdd(MODSPAWN_GUARD, chr->padnum, true);
				}

				chr->spawnflags &= ~(SPAWNFLAG_ONLYONA | SPAWNFLAG_ONLYONSA | SPAWNFLAG_ONLYONPA);
				chr->spawnflags |= lvGetDifficulty() == DIFF_A ? SPAWNFLAG_ONLYONSA : SPAWNFLAG_ONLYONA;
				chr->chrnum = -1;
			}
		} else if ((obj->type == OBJTYPE_WEAPON || obj->type == OBJTYPE_AMMOCRATE
					|| obj->type == OBJTYPE_MULTIAMMOCRATE)
				&& (obj->flags & OBJFLAG_ASSIGNEDTOCHR) == 0
				&& modSpawnEditPadOk(obj->pad)
				&& !modSpawnEditHasPad(MODSPAWN_OBJECTIVE, obj->pad)) {
			modSpawnEditAdd(MODSPAWN_OBJECTIVE, obj->pad, false);
		}

		cmd += setupGetCmdLength(cmd);
	}

	memcpy(g_ModSpawnDefaults, g_ModSpawns, g_ModNumSpawns * sizeof(struct modspawn));
	g_ModNumDefaults = g_ModNumSpawns;
	snprintf(g_ModSpawnConfigName, sizeof(g_ModSpawnConfigName), "Default");
	g_ModSpawnMenu = 0;

	sysLogPrintf(LOG_NOTE, "spawnedit: stage 0x%02x loaded: %d guard, %d hunter, %d objective, %d escape",
			stagenum, g_ModSpawnCounts[MODSPAWN_GUARD], g_ModSpawnCounts[MODSPAWN_HUNTER],
			g_ModSpawnCounts[MODSPAWN_OBJECTIVE], g_ModSpawnCounts[MODSPAWN_ESCAPE]);

	// the setting chosen in the menu opens in the editor
	if (modSpawnConfigIsChosen(stagenum)) {
		modSpawnEditLoadConfigByChoice();
	} else {
		modSpawnEditLoadBuiltinDefault(stagenum, true);
	}

	return true;
}

/* ------------------------------------------------------------------------
 * Characters at the spawns
 *
 * The squares were unreadable, so the spawns nearest the player are shown as
 * characters: the guard look for a Guard, the reinforcement look for a Hunter
 * (both the ones chosen in Perfect Random Options > Character Models), the
 * target's look for an Objective and Joanna for an Escape. They are real
 * characters on the idle AI, invincible, and exist only here. When two spawns
 * share a spot their characters stand a step apart.
 * --------------------------------------------------------------------- */

static s32 modSpawnEditFindId(u32 id)
{
	s32 i;

	for (i = 0; i < g_ModNumSpawns; i++) {
		if (g_ModSpawns[i].id == id) {
			return i;
		}
	}

	return -1;
}

static struct modspawnmodel *modSpawnEditModelFor(u32 id)
{
	s32 i;

	for (i = 0; i < MODSPAWN_MODELS; i++) {
		if (g_ModSpawnModels[i].id == id && id != 0) {
			return &g_ModSpawnModels[i];
		}
	}

	return NULL;
}

/** Where the character for spawn [index] stands: its spawn, or a step aside from the others on its spot. */
static void modSpawnEditDisplayPos(s32 index, struct coord *out)
{
	struct modspawn *spawn = &g_ModSpawns[index];
	s32 rank = 0;
	s32 count = 0;
	s32 j;

	for (j = 0; j < g_ModNumSpawns; j++) {
		struct modspawn *other = &g_ModSpawns[j];
		f32 dx = other->pos.x - spawn->pos.x;
		f32 dz = other->pos.z - spawn->pos.z;

		if (dx * dx + dz * dz < MODSPAWN_OVERLAP * MODSPAWN_OVERLAP
				&& fabsf(other->pos.y - spawn->pos.y) < 150.0f) {
			count++;

			if (other->kind < spawn->kind || (other->kind == spawn->kind && other->id < spawn->id)) {
				rank++;
			}
		}
	}

	*out = spawn->pos;
	out->x += ((f32)rank - (f32)(count - 1) * 0.5f) * MODSPAWN_SIDESTEP;
}

static void modSpawnEditModelLook(u8 kind, s32 *body, s32 *head)
{
	switch (kind) {
	case MODSPAWN_HUNTER:
		*body = modRandomGuardBody(1);
		*head = modRandomGuardHead(1);
		break;
	case MODSPAWN_OBJECTIVE:
		*body = modRandomExtraBody(1, BODY_CASSANDRA);
		*head = modRandomExtraHead(1, HEAD_CASSANDRA);
		break;
	case MODSPAWN_ESCAPE:
		*body = BODY_DARK_COMBAT;
		*head = HEAD_DARK_COMBAT;
		break;
	default:
		*body = modRandomGuardBody(0);
		*head = modRandomGuardHead(0);
		break;
	}
}

/** The markers are not solid: the player, and the player's shots, pass through them. */
static void modSpawnEditNoCollide(struct prop *prop)
{
	if (prop) {
		propSetPerimEnabled(prop, false);
	}
}

static void modSpawnEditModelKill(struct modspawnmodel *model)
{
	if (model->obj) {
		if (model->prop && model->prop->obj == model->obj) {
			objFreePermanently(model->obj, true);
		}
	} else
	// only if it is still the character it was: the game may have taken it
	if (model->prop && model->prop->chr && model->prop->chr->chrnum == model->chrnum) {
		chrRemove(model->prop, true);
		propDelist(model->prop);
		propDisable(model->prop);
		propFree(model->prop);
	}

	memset(model, 0, sizeof(*model));
}

/** Any pad inside a room: an object is made on a pad and then moved off it. */
static s32 modSpawnEditAnyPad(void)
{
	static s32 pad = -1;
	struct pad p;
	s32 i;

	if (pad >= 0) {
		return pad;
	}

	for (i = 0; g_PadsFile != NULL && i < g_PadsFile->numpads; i++) {
		padUnpack(i, PADFIELD_ROOM, &p);

		if (p.room > 0) {
			pad = i;
			break;
		}
	}

	return pad;
}

/** Put a laptop where [pos] is, turned to [angle]. */
static void modSpawnEditLaptopPlace(struct defaultobj *obj, struct coord *pos, RoomNum room, f32 angle)
{
	f32 c = cosf(angle);
	f32 s = sinf(angle);

	// realrot is the turn times the model's scale (setupCreateObject builds it
	// from a scaled matrix): a bare turn here drew the laptop at full size
	f32 k = obj->model ? obj->model->scale : 1.0f;

	obj->realrot[0][0] = c * k;  obj->realrot[0][1] = 0; obj->realrot[0][2] = -s * k;
	obj->realrot[1][0] = 0;      obj->realrot[1][1] = k; obj->realrot[1][2] = 0;
	obj->realrot[2][0] = s * k;  obj->realrot[2][1] = 0; obj->realrot[2][2] = c * k;

	// the model's origin is its middle: stand its underside on [pos], so that
	// the whole laptop shows above the floor
	{
		struct modelrodata_bbox *bbox = obj->model ? modelFindBboxRodata(obj->model) : NULL;

		obj->prop->pos = *pos;

		if (bbox) {
			obj->prop->pos.y += -objGetLocalYMin(bbox) * k + 1.0f;
		}
	}

	obj->prop->rooms[0] = room;
	obj->prop->rooms[1] = -1;
	func0f069c70(obj, false, true);
}

/** A laptop (MODEL_GOODPC) in the stage, in [slot]; NULL when it could not be made. */
static struct prop *modSpawnEditLaptopMake(struct defaultobj *slot, struct coord *pos, RoomNum room, f32 angle)
{
	struct defaultobj template = {
		256, 0, OBJTYPE_BASIC, MODEL_GOODPC, -1, 0, 0, 0, NULL, NULL,
		1, 0, 0, 0, 1, 0, 0, 0, 1,
		0, NULL, NULL, 0, 1000,
		0xff, 0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0x00, 0x0fff, 0,
	};
	s32 pad = modSpawnEditAnyPad();

	if (pad < 0) {
		return NULL;
	}

	*slot = template;
	slot->pad = pad;
	slot->flags = OBJFLAG_INVINCIBLE | OBJFLAG_FORCENOBOUNCE;
	slot->flags2 = OBJFLAG2_IMMUNETOGUNFIRE | OBJFLAG2_IMMUNETOEXPLOSIONS;
	slot->flags3 = 0;
	slot->extrascale = MODSPAWN_LAPTOPSCALE * 256;
	slot->hidden2 &= ~OBJHFLAG_DELETING;

	setupCreateObject(slot, 123);

	if (slot->prop == NULL) {
		return NULL;
	}

	// setupCreateObject has activated and enabled the prop
	modSpawnEditNoCollide(slot->prop);
	modSpawnEditLaptopPlace(slot, pos, room, angle);

	return slot->prop;
}

/*
 * The laptops. They are made once, kept for the stage and never freed: a
 * laptop that is not wanted is switched off (propDisable) and one that is is
 * moved to its spawn and switched on. Freeing props from inside the frame
 * left stale props in the game's lists and crashed it.
 */
static bool modSpawnEditLaptopPos(u32 id, struct coord *out)
{
	s32 i;

	for (i = 0; i < MODSPAWN_LAPTOPS; i++) {
		struct prop *prop = g_ModSpawnLaptopObjs[i].prop;

		if (g_ModSpawnLaptops[i].id == id && prop && prop->obj == &g_ModSpawnLaptopObjs[i]
				&& (prop->flags & PROPFLAG_ENABLED)) {
			*out = g_ModSpawnLaptops[i].pos;
			return true;
		}
	}

	return false;
}

static void modSpawnEditTickLaptops(void)
{
	bool wanted[MODSPAWN_LAPTOPS];
	s32 i;
	s32 j;

	memset(wanted, 0, sizeof(wanted));

	for (j = 0; j < g_ModNumSpawns; j++) {
		struct modspawn *spawn = &g_ModSpawns[j];
		struct coord pos;
		s32 slot = -1;

		if (spawn->kind != MODSPAWN_ESCAPE) {
			continue;
		}

		for (i = 0; i < MODSPAWN_LAPTOPS; i++) {
			if (g_ModSpawnLaptops[i].id == spawn->id) {
				slot = i;
				break;
			}
		}

		if (slot < 0) {
			for (i = 0; i < MODSPAWN_LAPTOPS; i++) {
				if (g_ModSpawnLaptops[i].id == 0 && !wanted[i]) {
					slot = i;
					break;
				}
			}
		}

		if (slot < 0) {
			continue; // more than 32 Escape/Spawns: the rest have no laptop
		}

		wanted[slot] = true;
		modSpawnEditDisplayPos(j, &pos);

		{
			struct defaultobj *obj = &g_ModSpawnLaptopObjs[slot];
			bool alive = obj->prop && obj->prop->obj == obj;
			bool moved = g_ModSpawnLaptops[slot].id != spawn->id
				|| fabsf(g_ModSpawnLaptops[slot].pos.x - pos.x) > 0.5f
				|| fabsf(g_ModSpawnLaptops[slot].pos.y - pos.y) > 0.5f
				|| fabsf(g_ModSpawnLaptops[slot].pos.z - pos.z) > 0.5f
				|| fabsf(g_ModSpawnLaptops[slot].angle - spawn->angle) > 0.001f;
			struct coord lpos = pos;

			lpos.y -= MODSPAWN_LIFT;

			if (!alive) {
				if (g_Vars.lvframe60 < g_ModSpawnLaptopFailUntil
						|| modSpawnEditLaptopMake(obj, &lpos, spawn->room, spawn->angle) == NULL) {
					g_ModSpawnLaptops[slot].id = 0;
					wanted[slot] = false;
					g_ModSpawnLaptopFailUntil = g_Vars.lvframe60 + 20;
					continue;
				}
			} else if (moved) {
				modSpawnEditLaptopPlace(obj, &lpos, spawn->room, spawn->angle);
			}

			if (!(obj->prop->flags & PROPFLAG_ENABLED)) {
				propEnable(obj->prop);
			}

			modSpawnEditNoCollide(obj->prop);

			g_ModSpawnLaptops[slot].id = spawn->id;
			g_ModSpawnLaptops[slot].pos = pos;
			g_ModSpawnLaptops[slot].angle = spawn->angle;
		}
	}

	for (i = 0; i < MODSPAWN_LAPTOPS; i++) {
		struct defaultobj *obj = &g_ModSpawnLaptopObjs[i];

		if (!wanted[i]) {
			g_ModSpawnLaptops[i].id = 0;

			if (obj->prop && obj->prop->obj == obj && (obj->prop->flags & PROPFLAG_ENABLED)) {
				propDisable(obj->prop);
			}
		}
	}
}

static void modSpawnEditModelMake(s32 index)
{
	struct modspawn *spawn = &g_ModSpawns[index];
	struct modspawnmodel *model = NULL;
	struct coord pos;
	RoomNum rooms[2];
	struct prop *prop;
	s32 body;
	s32 head;
	s32 i;

	for (i = 0; i < MODSPAWN_MODELS; i++) {
		if (g_ModSpawnModels[i].id == 0) {
			model = &g_ModSpawnModels[i];
			break;
		}
	}

	if (model == NULL || spawn->room <= 0 || spawn->room >= g_Vars.roomcount) {
		return;
	}

	modSpawnEditDisplayPos(index, &pos);

	if (spawn->kind == MODSPAWN_ESCAPE) {
		struct defaultobj *slot = &g_ModSpawnObjs[model - g_ModSpawnModels];

		pos.y -= MODSPAWN_LIFT;
		prop = modSpawnEditLaptopMake(slot, &pos, spawn->room, spawn->angle);

		if (prop == NULL) {
			g_ModSpawnModelFailUntil = g_Vars.lvframe60 + 20;
			return;
		}

		model->id = spawn->id;
		model->prop = prop;
		model->obj = slot;
		model->kind = spawn->kind;
		modSpawnEditDisplayPos(index, &model->pos);
		return;
	}

	pos.y += 50.0f - MODSPAWN_LIFT; // a character stands 50 above its floor, as a pad's does

	rooms[0] = spawn->room;
	rooms[1] = -1;

	modSpawnEditModelLook(spawn->kind, &body, &head);
	prop = chrSpawnAtCoord(body, head, &pos, rooms, spawn->angle, ailistFindById(GAILIST_IDLE),
			SPAWNFLAG_ALLOWONSCREEN);

	if (prop == NULL || prop->chr == NULL) {
		g_ModSpawnModelFailUntil = g_Vars.lvframe60 + 20; // no slot or no room: not every pass
		return;
	}

	prop->chr->chrflags |= CHRCFLAG_INVINCIBLE;
	modSpawnEditNoCollide(prop);

	model->id = spawn->id;
	model->prop = prop;
	model->chrnum = prop->chr->chrnum;
	model->kind = spawn->kind;
	modSpawnEditDisplayPos(index, &model->pos);
}

/**
 * Keep the characters in step with the spawns, from the flight tick. A
 * character whose spawn is gone, changed kind, moved or got another neighbour
 * is taken down; the nearest spawns without one get one, a few a pass because
 * each loads a model. Turning a spawn turns its character every tick.
 */
static void modSpawnEditTickModels(void)
{
	struct coord eye = g_Vars.currentplayer->prop->pos;
	u32 wantid[MODSPAWN_MODELS_MAX];
	f32 wantdist[MODSPAWN_MODELS_MAX];
	s32 wantindex[MODSPAWN_MODELS_MAX];
	s32 numwant = 0;
	s32 made = 0;
	s32 i;
	s32 j;

	for (i = 0; i < MODSPAWN_MODELS; i++) {
		struct modspawnmodel *model = &g_ModSpawnModels[i];
		s32 index;

		if (model->id == 0) {
			continue;
		}

		index = modSpawnEditFindId(model->id);

		if (!model->obj) {
			// A character the game took, killed, hid or turned into something else is
			// forgotten here so the next pass puts a new one up (it used to stay
			// "present" and never come back until the player walked out of range).
			struct prop *mp = model->prop;
			struct chrdata *mc = mp ? mp->chr : NULL;
			const char *why = NULL;

			if (!mp || !mc || mc->chrnum != model->chrnum || !mc->model) {
				why = "chr gone";
			} else if (!(mp->flags & PROPFLAG_ENABLED)) {
				why = "prop disabled";
			} else if (mc->actiontype == ACT_DEAD || mc->actiontype == ACT_DRUGGEDKO || mc->actiontype == ACT_DIE) {
				why = "chr dead";
			} else if (mc->hidden & CHRHFLAG_PERIMDISABLED) {
				why = NULL;
			}

			if (why) {
				sysLogPrintf(0, "spawnedit: marker %u lost (%s)", model->id, why);
				g_ModSpawnLostCount++;

				if (mp && mc && mc->chrnum == model->chrnum) {
					modSpawnEditModelKill(model);
				} else {
					memset(model, 0, sizeof(*model));
				}

				g_ModSpawnNextModelPass = 0;
				continue;
			}
		}

		if (model->prop && model->prop->chr && model->prop->chr->chrnum == model->chrnum) {
			modSpawnEditNoCollide(model->prop); // the game turns collision back on now and then
		}

		if (index >= 0 && g_ModSpawns[index].kind == model->kind && model->obj) {
			if (model->prop && model->prop->obj == model->obj) {
				f32 c = cosf(g_ModSpawns[index].angle);
				f32 s = sinf(g_ModSpawns[index].angle);

				if (fabsf(model->obj->realrot[0][0] - c) > 0.001f || fabsf(model->obj->realrot[2][0] - s) > 0.001f) {
					struct coord lpos = model->pos;

					lpos.y -= MODSPAWN_LIFT;
					modSpawnEditLaptopPlace(model->obj, &lpos, g_ModSpawns[index].room, g_ModSpawns[index].angle);
				}
			}
		} else if (index >= 0 && g_ModSpawns[index].kind == model->kind && model->prop && model->prop->chr
				&& model->prop->chr->chrnum == model->chrnum) {
			chrSetLookAngle(model->prop->chr, g_ModSpawns[index].angle);
		}
	}

	if (g_Vars.lvframenum < g_ModSpawnNextModelPass) {
		return;
	}

	g_ModSpawnNextModelPass = g_Vars.lvframenum + 4;

	// the nearest spawns inside the range, nearest first
	for (i = 0; i < g_ModNumSpawns; i++) {
		struct modspawn *spawn = &g_ModSpawns[i];
		f32 dx = spawn->pos.x - eye.x;
		f32 dy = spawn->pos.y - eye.y;
		f32 dz = spawn->pos.z - eye.z;
		f32 dist = sqrtf(dx * dx + dy * dy + dz * dz);

		if (dist > MODSPAWN_MODELDIST || spawn->kind == MODSPAWN_ESCAPE) {
			continue; // (Escape/Spawn is a laptop: see modSpawnEditTickLaptops)
		}

		for (j = numwant; j > 0 && wantdist[j - 1] > dist; j--) {
			if (j < MODSPAWN_MODELS) {
				wantid[j] = wantid[j - 1];
				wantdist[j] = wantdist[j - 1];
				wantindex[j] = wantindex[j - 1];
			}
		}

		if (j < MODSPAWN_MODELS) {
			wantid[j] = spawn->id;
			wantdist[j] = dist;
			wantindex[j] = i;

			if (numwant < MODSPAWN_MODELS) {
				numwant++;
			}
		}
	}

	// take down what is out of range, gone, changed or has moved
	for (i = 0; i < MODSPAWN_MODELS; i++) {
		struct modspawnmodel *model = &g_ModSpawnModels[i];
		bool keep = false;

		if (model->id == 0) {
			continue;
		}

		for (j = 0; j < numwant; j++) {
			if (wantid[j] == model->id) {
				struct coord pos;

				modSpawnEditDisplayPos(wantindex[j], &pos);
				keep = g_ModSpawns[wantindex[j]].kind == model->kind
					&& fabsf(pos.x - model->pos.x) < 1.0f && fabsf(pos.z - model->pos.z) < 1.0f
					&& fabsf(pos.y - model->pos.y) < 1.0f;
				break;
			}
		}

		if (!keep) {
			modSpawnEditModelKill(model);
		}
	}

	// and put up the nearest that have none
	if (g_Vars.lvframe60 < g_ModSpawnModelFailUntil) {
		return;
	}

	for (j = 0; j < numwant && made < MODSPAWN_PERNEW; j++) {
		if (modSpawnEditModelFor(wantid[j]) == NULL) {
			modSpawnEditModelMake(wantindex[j]);
			made++;
		}
	}
}

/* ------------------------------------------------------------------------
 * Editing
 * --------------------------------------------------------------------- */

static void modSpawnEditInsert(s32 index, const struct modspawn *spawn)
{
	if (g_ModNumSpawns >= MODSPAWN_MAX || index < 0 || index > g_ModNumSpawns) {
		return;
	}

	memmove(&g_ModSpawns[index + 1], &g_ModSpawns[index], (g_ModNumSpawns - index) * sizeof(struct modspawn));
	g_ModSpawns[index] = *spawn;
	g_ModNumSpawns++;
	memset(g_ModSpawnSeenFrame, 0, sizeof(g_ModSpawnSeenFrame));
	g_ModSpawnAimed = -1;
	g_ModSpawnTurning = -1;
	modSpawnEditRecount();
}

static void modSpawnEditRemove(s32 index)
{
	if (index < 0 || index >= g_ModNumSpawns) {
		return;
	}

	memmove(&g_ModSpawns[index], &g_ModSpawns[index + 1], (g_ModNumSpawns - index - 1) * sizeof(struct modspawn));
	g_ModNumSpawns--;
	memset(g_ModSpawnSeenFrame, 0, sizeof(g_ModSpawnSeenFrame));
	g_ModSpawnAimed = -1;
	g_ModSpawnTurning = -1;
	modSpawnEditRecount();
}

/**
 * The player's look, as a direction: the level look the walk keeps in
 * bond2.unk00 with the pitch put back.
 */
static void modSpawnEditLook(struct coord *look)
{
	struct player *player = g_Vars.currentplayer;

	look->x = player->bond2.unk00.x * player->vv_cosverta;
	look->y = player->vv_sinverta;
	look->z = player->bond2.unk00.z * player->vv_cosverta;
}

/**
 * Where the crosshair lands: the first wall, floor or closed door along the
 * look, the way a bullet is found. False when nothing is in reach or the
 * player is outside every room, where a ray cannot be walked.
 */
static bool modSpawnEditRay(struct coord *hit, struct coord *look)
{
	struct player *player = g_Vars.currentplayer;
	struct coord start = player->prop->pos;
	struct coord end;

	RoomNum eyerooms[21];
	RoomNum aboverooms[21];
	RoomNum bestroom = -1;
	RoomNum *rooms = player->prop->rooms;

	if (rooms[0] < 0) {
		// the player's own rooms went stale: ask the map where the eye is
		eyerooms[0] = -1;
		aboverooms[0] = -1;
		bgFindRoomsByPos(&start, eyerooms, aboverooms, 20, &bestroom);
		rooms = eyerooms[0] != -1 ? eyerooms : aboverooms;

		if (rooms[0] < 0) {
			modSpawnEditSay("The camera is outside the map: fly back in");
			return false;
		}
	}

	modSpawnEditLook(look);

	end.x = start.x + look->x * MODSPAWN_RAYLEN;
	end.y = start.y + look->y * MODSPAWN_RAYLEN;
	end.z = start.z + look->z * MODSPAWN_RAYLEN;

	if (cdExamLos08(&start, rooms, &end,
			CDTYPE_BG | CDTYPE_CLOSEDDOORS,
			GEOFLAG_FLOOR1 | GEOFLAG_FLOOR2 | GEOFLAG_WALL | GEOFLAG_BLOCK_SIGHT) == CDRESULT_COLLISION) {
		cdGetPos(hit, 0, "modspawnedit.c");
		return true;
	}

	return false;
}

/**
 * From a point the crosshair hit to a place somebody could stand: the point
 * is drawn back a little off the surface it hit, the floor below it is found,
 * and the spot is refused when there is no floor, the floor kills, a ledge or
 * ceiling is a long way above it, or there is no room to stand - the checks
 * the roll puts a pad through (modRandomPadCanSpawn).
 */
/* (Characters do not count as in the way: the markers and the preview are characters, and the preview
 * pushed its own aim spot away, then back, every other tick - two copies side by side.) */
static bool modSpawnEditSnapAt(struct coord *query, struct coord *pos, RoomNum *room, const char **why)
{
	RoomNum inrooms[21];
	RoomNum aboverooms[21];
	RoomNum bestroom = -1;
	RoomNum *rooms;
	RoomNum floorrooms[2];
	RoomNum floorroom = -1;
	u16 floorflags = 0;
	struct coord q = *query;
	f32 ground;

	inrooms[0] = -1;
	aboverooms[0] = -1;

	bgFindRoomsByPos(&q, inrooms, aboverooms, 20, &bestroom);
	rooms = inrooms[0] != -1 ? inrooms : aboverooms;

	if (rooms[0] == -1) {
		*why = "That spot is outside the map";
		return false;
	}

	q.y += MODSPAWN_LIFT;
	ground = cdFindGroundInfoAtCyl(&q, 30, rooms, NULL, NULL, &floorflags, &floorroom, NULL, NULL);

	if (floorroom < 0 || ground < MODSPAWN_NOGROUND) {
		*why = "No floor under that spot";
		return false;
	}

	if (floorflags & GEOFLAG_DIE) {
		*why = "That floor kills";
		return false;
	}

	if (q.y - ground > MODSPAWN_MAXDROP) {
		*why = "Too far above the floor";
		return false;
	}

	pos->x = q.x;
	pos->y = ground + MODSPAWN_LIFT;
	pos->z = q.z;
	*room = floorroom;

	floorrooms[0] = floorroom;
	floorrooms[1] = -1;

	if (cdTestVolume(pos, 20, floorrooms, CDTYPE_ALL & ~CDTYPE_CHRS, CHECKVERTICAL_YES, 200, -200) == CDRESULT_COLLISION) {
		*why = "No room to stand there";
		return false;
	}

	return true;
}

/**
 * Where a spawn aimed at [hit] goes: there if it fits, else the nearest spot
 * around it that does (rings out to 3 m).
 */
static bool modSpawnEditSnap(struct coord *hit, struct coord *look, struct coord *pos, RoomNum *room, const char **why)
{
	static const f32 radii[] = {25, 50, 75, 100, 150, 200, 300};
	struct coord query;
	const char *firstwhy = NULL;
	s32 r;
	s32 k;

	query.x = hit->x - look->x * 15.0f;
	query.y = hit->y - look->y * 15.0f;
	query.z = hit->z - look->z * 15.0f;

	if (modSpawnEditSnapAt(&query, pos, room, why)) {
		return true;
	}

	firstwhy = *why;

	for (r = 0; r < (s32)(sizeof(radii) / sizeof(radii[0])); r++) {
		for (k = 0; k < 12; k++) {
			struct coord q = query;
			f32 a = (f32)k * (M_PI / 6.0f);

			q.x += sinf(a) * radii[r];
			q.z += cosf(a) * radii[r];

			if (modSpawnEditSnapAt(&q, pos, room, why)) {
				return true;
			}
		}
	}

	*why = firstwhy;
	return false;
}

static void modSpawnEditPlace(void)
{
	struct coord hit;
	struct coord look;
	struct modspawn spawn;
	const char *why = NULL;
	char text[64];

	if (g_ModNumSpawns >= MODSPAWN_MAX) {
		modSpawnEditSay("Spawn limit reached");
		return;
	}

	if (!modSpawnEditRay(&hit, &look)) {
		modSpawnEditSay("Nothing in reach (fly inside the map)");
		return;
	}

	memset(&spawn, 0, sizeof(spawn));

	if (!modSpawnEditSnap(&hit, &look, &spawn.pos, &spawn.room, &why)) {
		modSpawnEditSay(why);
		return;
	}

	spawn.id = ++g_ModSpawnNextId;
	spawn.kind = g_ModSpawnPlaceKind;
	spawn.pad = -1;
	spawn.angle = modSpawnEditPlaceAngle();

	modSpawnEditInsert(g_ModNumSpawns, &spawn);

	g_ModSpawnUndo.type = MODSPAWN_UNDO_ADDED;
	g_ModSpawnUndo.index = g_ModNumSpawns - 1;
	g_ModSpawnDirty = true;

	snprintf(text, sizeof(text), "%s spawn placed (room %d)", g_ModSpawnNames[spawn.kind], (int)spawn.room);
	modSpawnEditSay(text);
}

static void modSpawnEditDelete(void)
{
	s32 index = g_ModSpawnAimed;
	char text[64];

	if (index < 0 || index >= g_ModNumSpawns) {
		modSpawnEditSay("Aim at a nearby marker to delete it");
		return;
	}

	g_ModSpawnUndo.type = MODSPAWN_UNDO_DELETED;
	g_ModSpawnUndo.index = index;
	g_ModSpawnUndo.spawn = g_ModSpawns[index];
	g_ModSpawnDirty = true;

	snprintf(text, sizeof(text), "%s spawn deleted", g_ModSpawnNames[g_ModSpawns[index].kind]);
	modSpawnEditRemove(index);
	modSpawnEditSay(text);
}

static void modSpawnEditClearKind(s32 kind)
{
	s32 removed = g_ModSpawnCounts[kind];
	s32 kept = 0;
	char text[64];

	if (removed == 0) {
		snprintf(text, sizeof(text), "No %s spawns to clear", g_ModSpawnNames[kind]);
		modSpawnEditSay(text);
		return;
	}

	g_ModSpawnCountBeforeClear = g_ModNumSpawns;
	memcpy(g_ModSpawnBeforeClear, g_ModSpawns, g_ModNumSpawns * sizeof(g_ModSpawns[0]));
	g_ModSpawnUndo.type = MODSPAWN_UNDO_CLEARED;

	for (s32 i = 0; i < g_ModNumSpawns; i++) {
		if (g_ModSpawns[i].kind != kind) {
			g_ModSpawns[kept++] = g_ModSpawns[i];
		}
	}

	g_ModNumSpawns = kept;
	memset(g_ModSpawnSeenFrame, 0, sizeof(g_ModSpawnSeenFrame));
	g_ModSpawnAimed = -1;
	g_ModSpawnTurning = -1;
	g_ModSpawnDirty = true;
	modSpawnEditRecount();
	snprintf(text, sizeof(text), "Cleared %d %s spawn%s", removed, g_ModSpawnNames[kind], removed == 1 ? "" : "s");
	modSpawnEditSay(text);
}

static void modSpawnEditUndo(void)
{
	switch (g_ModSpawnUndo.type) {
	case MODSPAWN_UNDO_ADDED:
		modSpawnEditRemove(g_ModSpawnUndo.index);
		modSpawnEditSay("Undid the placement");
		break;
	case MODSPAWN_UNDO_DELETED:
		modSpawnEditInsert(g_ModSpawnUndo.index, &g_ModSpawnUndo.spawn);
		modSpawnEditSay("Undid the delete");
		break;
	case MODSPAWN_UNDO_TURNED:
		if (g_ModSpawnUndo.index >= 0 && g_ModSpawnUndo.index < g_ModNumSpawns) {
			g_ModSpawns[g_ModSpawnUndo.index].angle = g_ModSpawnUndo.spawn.angle;
		}
		modSpawnEditSay("Undid the turn");
		break;
	case MODSPAWN_UNDO_CLEARED:
		g_ModNumSpawns = g_ModSpawnCountBeforeClear;
		memcpy(g_ModSpawns, g_ModSpawnBeforeClear, g_ModNumSpawns * sizeof(g_ModSpawns[0]));
		memset(g_ModSpawnSeenFrame, 0, sizeof(g_ModSpawnSeenFrame));
		g_ModSpawnAimed = -1;
		modSpawnEditRecount();
		g_ModSpawnDirty = true;
		modSpawnEditSay("Undid the clear");
		break;
	default:
		modSpawnEditSay("Nothing to undo");
		return;
	}

	g_ModSpawnUndo.type = MODSPAWN_UNDO_NONE;
	g_ModSpawnTurning = -1;
}

static void modSpawnEditSetKind(s32 kind)
{
	char text[48];

	g_ModSpawnPlaceKind = (kind + MODSPAWN_NUMKINDS) % MODSPAWN_NUMKINDS;
	snprintf(text, sizeof(text), "Placing: %s", g_ModSpawnNames[g_ModSpawnPlaceKind]);
	modSpawnEditSay(text);
}

/**
 * The editor's inputs, once a tick from the flight. Every key is polled every
 * tick, because inputKeyJustPressed() compares with the last time it was asked.
 */
static void modSpawnEditTickInput(void)
{
	const bool place = inputKeyJustPressed(VK_MOUSE_LEFT);
	const bool delete = inputKeyJustPressed(VK_MOUSE_RIGHT);
	const bool undo = inputKeyJustPressed(VK_BACKSPACE) | inputKeyJustPressed(VK_A + ('U' - 'A'));
	const bool wheelup = inputKeyJustPressed(VK_MOUSE_WHEEL_UP);
	const bool wheeldown = inputKeyJustPressed(VK_MOUSE_WHEEL_DN);
	const bool turnleft = inputKeyPressed(VK_COMMA);
	const bool turnright = inputKeyPressed(VK_PERIOD);
	s32 i;

	for (i = 0; i < MODSPAWN_NUMKINDS; i++) {
		if (inputKeyJustPressed(VK_1 + i)) {
			modSpawnEditSetKind(i);
		}
	}

	// the wheel turns the spawn about to be placed, 15 degrees a notch
	if (wheelup) {
		g_ModSpawnPlaceTurn += M_PI / 12.0f;
	}

	if (wheeldown) {
		g_ModSpawnPlaceTurn -= M_PI / 12.0f;
	}

	if (undo) {
		modSpawnEditUndo();
	}

	if (place) {
		modSpawnEditPlace();
	}

	if (delete) {
		modSpawnEditDelete();
	}

	// Turning the marker under the crosshair: one undo step per hold
	if ((turnleft || turnright) && !(turnleft && turnright)) {
		s32 index = g_ModSpawnTurning >= 0 ? g_ModSpawnTurning : g_ModSpawnAimed;

		if (index >= 0 && index < g_ModNumSpawns) {
			f32 angle = g_ModSpawns[index].angle;

			if (g_ModSpawnTurning != index) {
				g_ModSpawnUndo.type = MODSPAWN_UNDO_TURNED;
				g_ModSpawnUndo.index = index;
				g_ModSpawnUndo.spawn = g_ModSpawns[index];
				g_ModSpawnTurning = index;
			}

			angle += (turnleft ? -1.0f : 1.0f) * MODSPAWN_TURNRATE * g_Vars.lvupdate60freal * (1.0f / 60.0f);

			while (angle > M_PI) {
				angle -= 2.0f * M_PI;
			}

			while (angle < -M_PI) {
				angle += 2.0f * M_PI;
			}

			g_ModSpawns[index].angle = angle;
			g_ModSpawnDirty = true;
		}
	} else {
		g_ModSpawnTurning = -1;
	}
}

/**
 * Free flight, from bmoveTick() in place of bwalkTick().
 *
 * No gravity and no collision. The look comes from the same input the walk
 * uses (bmoveProcessInput has already run), the move from the walk's own
 * forward and sideways speeds, so the mouse and the move keys are the ones
 * the player has bound. Space goes up, Ctrl goes down, Shift is fast. (E and
 * Q are the game's own action and weapon keys, so they are left alone.)
 */
/**
 * Guaranteed Guard markers are the guard model shaded green (chr.c asks while
 * it draws a character). The colour is the game's own shade, as the
 * Randomizer's tactics tint does it.
 */
bool modSpawnEditChrTint(struct chrdata *chr, s32 *colour)
{
	s32 i;

	if (!g_ModSpawnEditOn || chr == NULL) {
		return false;
	}

	if (g_ModSpawnGhostChr[MODSPAWN_GUARANTEED] && g_ModSpawnGhostChrnum[MODSPAWN_GUARANTEED] == chr->chrnum) {
		i = -1;
	} else {
		for (i = 0; i < MODSPAWN_MODELS; i++) {
			if (g_ModSpawnModels[i].id && g_ModSpawnModels[i].kind == MODSPAWN_GUARANTEED
					&& g_ModSpawnModels[i].chrnum == chr->chrnum && g_ModSpawnModels[i].obj == NULL) {
				break;
			}
		}

		if (i == MODSPAWN_MODELS) {
			return false;
		}
	}

	colour[0] = 0;
	colour[1] = 230;
	colour[2] = 70;
	colour[3] = 140;
	return true;
}

bool modSpawnEditIsGhost(struct prop *prop)
{
	return g_ModSpawnEditOn && prop != NULL && prop == g_ModSpawnGhostLaptop;
}

static struct prop *modSpawnEditGhostChr(s32 kind)
{
	struct prop *prop = g_ModSpawnGhostChr[kind];

	if (prop && prop->chr && prop->chr->chrnum == g_ModSpawnGhostChrnum[kind]) {
		return prop;
	}

	return NULL;
}

/** True (and remembered) when the ghost of [kind] is not already at [pos], facing [angle]. */
static bool modSpawnEditGhostMoved(s32 kind, struct coord *pos, f32 angle)
{
	if (g_ModSpawnGhostPlaced[kind]
			&& fabsf(g_ModSpawnGhostAt[kind].x - pos->x) < 2.0f
			&& fabsf(g_ModSpawnGhostAt[kind].y - pos->y) < 2.0f
			&& fabsf(g_ModSpawnGhostAt[kind].z - pos->z) < 2.0f
			&& fabsf(g_ModSpawnGhostAngle[kind] - angle) < 0.02f) {
		return false;
	}

	g_ModSpawnGhostPlaced[kind] = true;
	g_ModSpawnGhostAt[kind] = *pos;
	g_ModSpawnGhostAngle[kind] = angle;
	return true;
}

static void modSpawnEditGhostHide(s32 exceptkind)
{
	s32 i;

	for (i = 0; i < MODSPAWN_NUMKINDS; i++) {
		struct prop *prop = modSpawnEditGhostChr(i);

		if (prop && i != exceptkind) {
			prop->chr->chrflags |= CHRCFLAG_HIDDEN;
		}
	}

	if (g_ModSpawnGhostLaptop && exceptkind != MODSPAWN_ESCAPE
			&& g_ModSpawnGhostLaptop->obj == &g_ModSpawnObjs[MODSPAWN_MODELS]
			&& (g_ModSpawnGhostLaptop->flags & PROPFLAG_ENABLED)) {
		propDisable(g_ModSpawnGhostLaptop);
	}
}

/**
 * The see-through model at the point a click would place a spawn, in the
 * selected kind's look: a character, or the laptop. One of each is made when
 * first wanted and kept for the stage (they are hidden, never freed, which
 * keeps the churn of props to nothing); gone while the crosshair is on
 * nothing a spawn can stand on.
 */
static void modSpawnEditTickGhost(void)
{
	struct coord hit;
	struct coord look;
	struct coord pos;
	RoomNum room;
	RoomNum rooms[2];
	const char *why;
	f32 angle = modSpawnEditPlaceAngle();
	s32 kind = g_ModSpawnPlaceKind;

	if (!modSpawnEditRay(&hit, &look) || !modSpawnEditSnap(&hit, &look, &pos, &room, &why)) {
		modSpawnEditGhostHide(-1);
		return;
	}

	modSpawnEditGhostHide(kind);

	rooms[0] = room;
	rooms[1] = -1;

	if (g_Vars.lvframe60 < g_ModSpawnGhostFailUntil) {
		return;
	}

	if (kind == MODSPAWN_ESCAPE) {
		pos.y -= MODSPAWN_LIFT;

		if (g_ModSpawnGhostLaptop == NULL) {
			g_ModSpawnGhostLaptop = modSpawnEditLaptopMake(&g_ModSpawnObjs[MODSPAWN_MODELS], &pos, room, angle);

			if (g_ModSpawnGhostLaptop == NULL) {
				g_ModSpawnGhostFailUntil = g_Vars.lvframe60 + 60;
			}

			return;
		}

		if (!(g_ModSpawnGhostLaptop->flags & PROPFLAG_ENABLED)) {
			propEnable(g_ModSpawnGhostLaptop);
		}

		modSpawnEditNoCollide(g_ModSpawnGhostLaptop);

		if (modSpawnEditGhostMoved(MODSPAWN_ESCAPE, &pos, angle)) {
			modSpawnEditLaptopPlace(g_ModSpawnGhostLaptop->obj, &pos, room, angle);
		}

		return;
	}

	pos.y += 50.0f - MODSPAWN_LIFT;

	{
		struct prop *prop = modSpawnEditGhostChr(kind);

		if (prop == NULL) {
			s32 body;
			s32 head;

			modSpawnEditModelLook(kind, &body, &head);
			prop = chrSpawnAtCoord(body, head, &pos, rooms, angle, ailistFindById(GAILIST_IDLE),
					SPAWNFLAG_ALLOWONSCREEN);

			if (prop == NULL || prop->chr == NULL) {
				g_ModSpawnGhostFailUntil = g_Vars.lvframe60 + 60;
				return;
			}

			g_ModSpawnGhostChr[kind] = prop;
			g_ModSpawnGhostChrnum[kind] = prop->chr->chrnum;
			prop->chr->chrflags |= CHRCFLAG_INVINCIBLE;
		}

		modSpawnEditNoCollide(prop);
		prop->chr->chrflags &= ~CHRCFLAG_HIDDEN;
		prop->chr->fadealpha = MODSPAWN_GHOSTALPHA;

		// only when the aim has really moved: re-placing a character every tick
		// leaves it never quite at rest
		if (modSpawnEditGhostMoved(kind, &pos, angle)) {
			chrMoveToPos(prop->chr, &pos, rooms, angle, true);
			chrSetLookAngle(prop->chr, angle);
		}

	}
}

/** The editing that goes on whether the player walks or flies. */
/* ------------------------------------------------------------------------
 * Configs: save, load, share
 *
 * A config is a text file, spawns/stage_XX/<name>.txt:
 *
 *   PerfectRandomSpawns 1
 *   stage 0x32
 *   name My Layout
 *   guard 1234.0 56.0 -789.0 1.5708
 *
 * one spawn a line: kind x y z angle (radians). Hand-editable, and a file
 * from a friend goes in the same folder. A file for another stage is refused.
 * "Default" is the arrangement the map loaded with; it is never a file.
 * --------------------------------------------------------------------- */

#define MODSPAWN_FORMAT   1
#define MODSPAWN_MAXFILES 40

static const char *g_ModSpawnKindWords[MODSPAWN_NUMKINDS] = {
	"guard", "hunter", "objective", "escape", "guaranteed",
};

static struct modspawn g_ModSpawnLoadBuf[MODSPAWN_MAX];

static bool modSpawnEditNameOk(const char *name)
{
	s32 i;

	if (name[0] == '\0' || strlen(name) > MODSPAWN_NAMELEN || strcasecmp(name, "default") == 0) {
		return false;
	}

	for (i = 0; name[i]; i++) {
		char c = name[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ')) {
			return false;
		}
	}

	return name[0] != ' ' && name[i - 1] != ' ';
}

static void modSpawnEditConfigDir(char *out, size_t size, s32 stagenum)
{
	snprintf(out, size, "spawns/stage_%02x", stagenum);
}

static void modSpawnEditConfigPath(char *out, size_t size, s32 stagenum, const char *name)
{
	snprintf(out, size, "spawns/stage_%02x/%s.txt", stagenum, name);
}

/** Replace the spawns with [list]. Every spawn gets a new id, so models are made afresh. */
static void modSpawnEditApply(const struct modspawn *list, s32 count)
{
	s32 i;

	g_ModNumSpawns = count;
	memcpy(g_ModSpawns, list, count * sizeof(struct modspawn));

	for (i = 0; i < count; i++) {
		g_ModSpawns[i].id = ++g_ModSpawnNextId;
	}

	memset(g_ModSpawnSeenFrame, 0, sizeof(g_ModSpawnSeenFrame));
	g_ModSpawnAimed = -1;
	g_ModSpawnTurning = -1;
	g_ModSpawnUndo.type = MODSPAWN_UNDO_NONE;
	modSpawnEditRecount();
}

static bool modSpawnEditSaveConfig(const char *name)
{
	char path[96];
	FILE *f;
	s32 i;

	if (!modSpawnEditNameOk(name)) {
		modSpawnEditSay("Names: letters, digits, space, - and _ (not \"Default\")");
		return false;
	}

	fsCreateDir("spawns");
	modSpawnEditConfigDir(path, sizeof(path), g_ModSpawnEditStage);
	fsCreateDir(path);
	modSpawnEditConfigPath(path, sizeof(path), g_ModSpawnEditStage, name);

	f = fsFileOpenWrite(path);

	if (f == NULL) {
		modSpawnEditSay("Could not write the file");
		return false;
	}

	fprintf(f, "PerfectRandomSpawns %d\nstage 0x%02x\nname %s\n", MODSPAWN_FORMAT, g_ModSpawnEditStage, name);

	for (i = 0; i < g_ModNumSpawns; i++) {
		const struct modspawn *s = &g_ModSpawns[i];

		fprintf(f, "%s %.1f %.1f %.1f %.4f %d\n", g_ModSpawnKindWords[s->kind], s->pos.x, s->pos.y, s->pos.z, s->angle, (int)s->room);
	}

	fclose(f);
	if (name != g_ModSpawnConfigName) {
		snprintf(g_ModSpawnConfigName, sizeof(g_ModSpawnConfigName), "%s", name);
	}
	g_ModSpawnDirty = false;
	modSpawnConfigUse(g_ModSpawnEditStage, name); // the Spawn Customizer menu now shows it, and runs use it
	sysLogPrintf(LOG_NOTE, "spawnedit: saved %d spawns to %s", g_ModNumSpawns, path);
	return true;
}

static void modSpawnEditRevert(void)
{
	if (modSpawnEditLoadBuiltinDefault(g_ModSpawnEditStage, false)) {
		modSpawnEditSay("Back to the map's default spawns");
		return;
	}

	modSpawnEditApply(g_ModSpawnDefaults, g_ModNumDefaults);
	snprintf(g_ModSpawnConfigName, sizeof(g_ModSpawnConfigName), "Default");
	g_ModSpawnDirty = false;
	modSpawnConfigOff(g_ModSpawnEditStage);
	modSpawnEditSay("Back to the map's default spawns");
}

/**
 * Read a config file for [stagenum] into [out] (room for MODSPAWN_MAX). Returns
 * the number of spawns, or -1 with [*err] saying why. Spawns outside the map
 * are left out and counted in [*rejected]. A line may end in the spawn's room
 * (files saved by this build do); without it the room is looked up, which
 * needs the map's rooms to be ready.
 */
static s32 modSpawnReadFile(s32 stagenum, const char *name, struct modspawn *out, s32 *rejected, const char **err)
{
	static char errbuf[96];
	char path[96];
	char line[160];
	FILE *f;
	s32 count = 0;
	s32 stage = -1;
	s32 format = 0;
	bool header = false;

	*rejected = 0;
	*err = "";
	modSpawnEditConfigPath(path, sizeof(path), stagenum, name);
	f = fsFileOpenRead(path);

	if (f == NULL) {
		*err = "That config is not there";
		return -1;
	}

	while (fgets(line, sizeof(line), f)) {
		char word[24];
		float x, y, z, angle;
		int room = -1;
		s32 fields;
		s32 kind;
		struct modspawn *s;

		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
			continue;
		}

		if (!header) {
			if (sscanf(line, "PerfectRandomSpawns %d", &format) != 1 || format < 1 || format > MODSPAWN_FORMAT) {
				fclose(f);
				*err = "Not a spawn config file";
				return -1;
			}

			header = true;
			continue;
		}

		if (strncmp(line, "stage ", 6) == 0) {
			stage = (s32)strtol(line + 6, NULL, 0);

			if (stage != stagenum) {
				fclose(f);
				snprintf(errbuf, sizeof(errbuf), "That config is for stage 0x%02x, not this map", stage);
				*err = errbuf;
				return -1;
			}

			continue;
		}

		if (strncmp(line, "name ", 5) == 0) {
			continue;
		}

		if (stage < 0) {
			fclose(f);
			*err = "The config has no stage line";
			return -1;
		}

		fields = sscanf(line, "%23s %f %f %f %f %d", word, &x, &y, &z, &angle, &room);

		if (fields < 5) {
			(*rejected)++;
			continue;
		}

		for (kind = 0; kind < MODSPAWN_NUMKINDS; kind++) {
			if (strcasecmp(word, g_ModSpawnKindWords[kind]) == 0) {
				break;
			}
		}

		if (kind == MODSPAWN_NUMKINDS || count >= MODSPAWN_MAX) {
			(*rejected)++;
			continue;
		}

		s = &out[count];
		memset(s, 0, sizeof(*s));
		s->kind = kind;
		s->pad = -1;
		s->pos.x = x;
		s->pos.y = y;
		s->pos.z = z;
		s->angle = angle;

		if (fields == 6 && room > 0 && room < g_Vars.roomcount) {
			s->room = room;
		} else {
			RoomNum inrooms[21];
			RoomNum aboverooms[21];
			RoomNum best = -1;

			inrooms[0] = -1;
			aboverooms[0] = -1;
			bgFindRoomsByPos(&s->pos, inrooms, aboverooms, 20, &best);

			if (best < 0) {
				best = inrooms[0] != -1 ? inrooms[0] : aboverooms[0];
			}

			if (best < 0) {
				(*rejected)++; // outside the map: the map has changed since
				continue;
			}

			s->room = best;
		}

		count++;
	}

	fclose(f);

	if (!header || stage < 0) {
		*err = "Not a spawn config file";
		return -1;
	}

	return count;
}

static bool modSpawnEditHasBuiltinDefault(s32 stagenum)
{
	return stagenum == STAGE_ESCAPE || stagenum == STAGE_CHICAGO
			|| stagenum == STAGE_G5BUILDING || stagenum == STAGE_PELAGIC
			|| stagenum == STAGE_AIRBASE || stagenum == STAGE_VILLA
			|| stagenum == STAGE_DEFENSE || stagenum == STAGE_INFILTRATION
			|| stagenum == STAGE_DEFECTION || stagenum == STAGE_INVESTIGATION
			|| stagenum == STAGE_ATTACKSHIP || stagenum == STAGE_RESCUE
			|| stagenum == STAGE_MP_AREA52;
}

static bool modSpawnEditLoadBuiltinDefault(s32 stagenum, bool updateDefaults)
{
	const char *err;
	s32 rejected;
	s32 count;

	if (!modSpawnEditHasBuiltinDefault(stagenum)) {
		return false;
	}

	count = modSpawnReadFile(stagenum, "Default", g_ModSpawnLoadBuf, &rejected, &err);
	if (count <= 0) {
		return false;
	}

	modSpawnEditApply(g_ModSpawnLoadBuf, count);
	if (updateDefaults) {
		memcpy(g_ModSpawnDefaults, g_ModSpawnLoadBuf, count * sizeof(struct modspawn));
		g_ModNumDefaults = count;
	}
	snprintf(g_ModSpawnConfigName, sizeof(g_ModSpawnConfigName), "Default");
	g_ModSpawnDirty = false;
	return true;
}

static bool modSpawnEditLoadConfig(const char *name)
{
	char msg[96];
	const char *err;
	s32 rejected;
	s32 count;

	if (strcasecmp(name, "default") == 0) {
		modSpawnEditRevert();
		return true;
	}

	count = modSpawnReadFile(g_ModSpawnEditStage, name, g_ModSpawnLoadBuf, &rejected, &err);

	if (count < 0) {
		modSpawnEditSay(err);
		return false;
	}

	modSpawnEditApply(g_ModSpawnLoadBuf, count);
	snprintf(g_ModSpawnConfigName, sizeof(g_ModSpawnConfigName), "%s", name);
	g_ModSpawnDirty = false;
	modSpawnConfigUse(g_ModSpawnEditStage, name);

	if (rejected > 0) {
		snprintf(msg, sizeof(msg), "Loaded %d spawns (%d did not fit this map)", count, rejected);
	} else {
		snprintf(msg, sizeof(msg), "Loaded %d spawns", count);
	}

	modSpawnEditSay(msg);
	return true;
}

static void modSpawnEditLoadConfigByChoice(void)
{
	char name[MODSPAWN_NAMELEN + 1];

	const char *chosen = modSpawnConfigChosenName(g_ModSpawnEditStage);

	if (chosen) {
		snprintf(name, sizeof(name), "%s", chosen);
		modSpawnEditLoadConfig(name);
	}
}

/**
 * Play: deal this map as a Perfect Random mission with the config as it is.
 * A named config with changes is saved first (the roll reads the file); the
 * unsaved Default has to be given a name. Leaving the mission is the game's
 * own (the end screen); the Spawn Customizer menu opens the editor again on
 * the same Setting.
 */
static bool modSpawnEditPlay(void)
{
	const s32 stage = g_ModSpawnEditStage;
	bool solo = false;
	s32 i;

	if (strcasecmp(g_ModSpawnConfigName, "default") == 0) {
		if (g_ModSpawnDirty) {
			modSpawnEditSay("Save the layout first (Save as...), then Play");
			return false;
		}

		modSpawnConfigOff(stage);
	} else if (g_ModSpawnDirty && !modSpawnEditSaveConfig(g_ModSpawnConfigName)) {
		return false;
	} else {
		modSpawnConfigUse(stage, g_ModSpawnConfigName);
	}

	for (i = 0; i < NUM_SOLOSTAGES; i++) {
		if ((s32)g_SoloStages[i].stagenum == stage) {
			solo = true;
		}
	}

	sysLogPrintf(LOG_NOTE, "spawnedit: play stage 0x%02x with \"%s\"", stage, g_ModSpawnConfigName);

	if (solo) {
		if (!modRunStartEditorMission(stage)) {
			return false;
		}
	} else {
		modRunStartArenaMission(stage);
	}

	g_ModSpawnPlayReturn = stage; // after the start: starting a mission clears it
	return true;
}

/**
 * The stage a Play ran on has ended (Abort Mission, a death, the objectives):
 * instead of the end screen the editor opens again on the same map, with the
 * same Setting. From mainEndStage().
 */
bool modSpawnEditPlayEnded(void)
{
	const s32 stage = g_ModSpawnPlayReturn;

	if (stage < 0 || g_ModSpawnEditOn) {
		return false;
	}

	g_ModSpawnPlayReturn = -1;
	sysLogPrintf(LOG_NOTE, "spawnedit: the Play ended; back to the editor on stage 0x%02x", stage);
	modSpawnEditStart(stage);
	return g_ModSpawnEditOn;
}

/* The menu: M opens it. Up and Down move, Enter chooses, Backspace or M closes. */

#define MODSPAWN_MENU_CLOSED 0
#define MODSPAWN_MENU_MAIN   1
#define MODSPAWN_MENU_LOAD   2
#define MODSPAWN_MENU_NAME   3
#define MODSPAWN_MENU_COMMANDS 4
#define MODSPAWN_MENU_COMMANDS_ITEM 5
#define MODSPAWN_VK_DOWN     81 // SDL scancodes: the arrow keys have no virtkey name
#define MODSPAWN_VK_UP       82

static s32 g_ModSpawnMenuSel;
static char g_ModSpawnMenuFiles[MODSPAWN_MAXFILES][MODSPAWN_NAMELEN + 1];
static s32 g_ModSpawnMenuNumFiles;
static char g_ModSpawnMenuName[MODSPAWN_NAMELEN + 1];

static const char *g_ModSpawnMenuItems[] = {"Play", "Save", "Save as...", "Load...", "Revert to Default", "Commands"};

static s32 modSpawnEditMenuCount(void)
{
	switch (g_ModSpawnMenu) {
	case MODSPAWN_MENU_MAIN: return ARRAYCOUNT(g_ModSpawnMenuItems);
	case MODSPAWN_MENU_COMMANDS: return MODSPAWN_NUMKINDS;
	default: return g_ModSpawnMenuNumFiles;
	}
}

static int modSpawnEditCmpNames(const void *a, const void *b)
{
	return strcasecmp((const char *)a, (const char *)b);
}

/** The saved configs for a map, "Default" first, into [names]; the count. */
s32 modSpawnConfigList(s32 stagenum, char names[][MODSPAWN_CONFIG_NAMELEN + 1], s32 max)
{
	char dir[96];
	DIR *d;
	struct dirent *e;
	s32 n = 0;

	if (max < 1) {
		return 0;
	}

	snprintf(names[n++], MODSPAWN_CONFIG_NAMELEN + 1, "Default");
	modSpawnEditConfigDir(dir, sizeof(dir), stagenum);
	d = opendir(fsFullPath(dir));

	if (d == NULL) {
		return n;
	}

	while ((e = readdir(d)) != NULL && n < max) {
		size_t len = strlen(e->d_name);

		if (len > 4 && len - 4 <= MODSPAWN_CONFIG_NAMELEN && strcasecmp(e->d_name + len - 4, ".txt") == 0) {
			if (strcasecmp(e->d_name, "Default.txt") == 0) {
				continue;
			}
			memcpy(names[n], e->d_name, len - 4);
			names[n][len - 4] = '\0';
			n++;
		}
	}

	closedir(d);
	qsort(names[1], n - 1, MODSPAWN_CONFIG_NAMELEN + 1, modSpawnEditCmpNames);
	return n;
}

static void modSpawnEditListConfigs(void)
{
	g_ModSpawnMenuNumFiles = modSpawnConfigList(g_ModSpawnEditStage, g_ModSpawnMenuFiles, MODSPAWN_MAXFILES);
}

static s32 g_ModSpawnEscEatenFrame = -10;

/**
 * True while the editor's menu is open, or closed itself on Esc a moment ago:
 * Esc then goes back a step in the menu and must not also pause the game
 * (bondmove.c and player.c ask before they treat Esc as Start).
 */
/** True while the editor's menu is open: the player holds still and the keys belong to the menu. */
bool modSpawnEditMenuOpen(void)
{
	return g_ModSpawnEditOn && g_ModSpawnMenu != MODSPAWN_MENU_CLOSED;
}

bool modSpawnEditEatsEscape(void)
{
	return g_ModSpawnEditOn && (g_ModSpawnMenu != MODSPAWN_MENU_CLOSED
			|| (s32)g_Vars.lvframenum - g_ModSpawnEscEatenFrame <= 2);
}

static void modSpawnEditMenuTick(bool escape)
{
	bool up = inputKeyJustPressed(MODSPAWN_VK_UP);
	bool down = inputKeyJustPressed(MODSPAWN_VK_DOWN);
	bool enter = inputKeyJustPressed(VK_RETURN);
	bool back = inputKeyJustPressed(VK_BACKSPACE);
	bool toggle = inputKeyJustPressed(VK_A + ('M' - 'A'));
	s32 count = modSpawnEditMenuCount();
	s32 i;

	if (g_ModSpawnMenu == MODSPAWN_MENU_CLOSED) {
		if (toggle) {
			g_ModSpawnMenu = MODSPAWN_MENU_MAIN;
			g_ModSpawnMenuSel = 0;
		}

		return;
	}

	if (escape) {
		g_ModSpawnEscEatenFrame = g_Vars.lvframenum; // a step back, not a pause
	}

	if (g_ModSpawnMenu == MODSPAWN_MENU_NAME) {
		size_t len = strlen(g_ModSpawnMenuName);
		bool shift = inputKeyPressed(VK_LSHIFT) || inputKeyPressed(VK_RSHIFT);

		if (back && len > 0) {
			g_ModSpawnMenuName[len - 1] = '\0';
		} else if (len < MODSPAWN_NAMELEN) {
			char c = 0;

			for (i = 0; i < 26; i++) {
				if (inputKeyJustPressed(VK_A + i)) {
					c = (shift ? 'A' : 'a') + i;
				}
			}

			for (i = 0; i < 10; i++) {
				if (inputKeyJustPressed(i == 9 ? VK_0 : VK_1 + i)) {
					c = i == 9 ? '0' : '1' + i;
				}
			}

			if (inputKeyJustPressed(VK_SPACE)) {
				c = ' ';
			}

			if (inputKeyJustPressed(VK_MINUS)) {
				c = shift ? '_' : '-';
			}

			if (c) {
				g_ModSpawnMenuName[len] = c;
				g_ModSpawnMenuName[len + 1] = '\0';
			}
		}

		if (enter) {
			if (modSpawnEditSaveConfig(g_ModSpawnMenuName)) {
				char msg[64];

				snprintf(msg, sizeof(msg), "Saved as %s", g_ModSpawnMenuName);
				modSpawnEditSay(msg);
				g_ModSpawnMenu = MODSPAWN_MENU_CLOSED;
			}
		} else if (escape) {
			g_ModSpawnMenu = MODSPAWN_MENU_MAIN;
		}

		return;
	}

	if (up) {
		g_ModSpawnMenuSel = (g_ModSpawnMenuSel + count - 1) % count;
	}

	if (down) {
		g_ModSpawnMenuSel = (g_ModSpawnMenuSel + 1) % count;
	}

	back |= escape;

	if (toggle || (back && g_ModSpawnMenu == MODSPAWN_MENU_MAIN)) {
		g_ModSpawnMenu = MODSPAWN_MENU_CLOSED;
		return;
	}

	if (back) {
		g_ModSpawnMenuSel = g_ModSpawnMenu == MODSPAWN_MENU_COMMANDS ? MODSPAWN_MENU_COMMANDS_ITEM : 3;
		g_ModSpawnMenu = MODSPAWN_MENU_MAIN;
		return;
	}

	if (!enter) {
		return;
	}

	if (g_ModSpawnMenu == MODSPAWN_MENU_COMMANDS) {
		modSpawnEditClearKind(g_ModSpawnMenuSel);
		return;
	}

	if (g_ModSpawnMenu == MODSPAWN_MENU_LOAD) {
		if (modSpawnEditLoadConfig(g_ModSpawnMenuFiles[g_ModSpawnMenuSel])) {
			g_ModSpawnMenu = MODSPAWN_MENU_CLOSED;
		}

		return;
	}

	switch (g_ModSpawnMenuSel) {
	case 0: // Play
		if (modSpawnEditPlay()) {
			g_ModSpawnMenu = MODSPAWN_MENU_CLOSED;
		}
		break;
	case 1: // Save: to the config in use; Default asks for a name
		if (strcasecmp(g_ModSpawnConfigName, "default") == 0) {
			g_ModSpawnMenuName[0] = '\0';
			g_ModSpawnMenu = MODSPAWN_MENU_NAME;
		} else if (modSpawnEditSaveConfig(g_ModSpawnConfigName)) {
			modSpawnEditSay("Saved");
			g_ModSpawnMenu = MODSPAWN_MENU_CLOSED;
		}
		break;
	case 2: // Save as
		g_ModSpawnMenuName[0] = '\0';
		g_ModSpawnMenu = MODSPAWN_MENU_NAME;
		break;
	case 3: // Load
		modSpawnEditListConfigs();
		g_ModSpawnMenu = MODSPAWN_MENU_LOAD;
		g_ModSpawnMenuSel = 0;
		break;
	case 4: // Revert
		modSpawnEditRevert();
		g_ModSpawnMenu = MODSPAWN_MENU_CLOSED;
		break;
	case MODSPAWN_MENU_COMMANDS_ITEM:
		g_ModSpawnMenu = MODSPAWN_MENU_COMMANDS;
		g_ModSpawnMenuSel = 0;
		break;
	}
}

/** The menu's lines, drawn with the HUD. Returns the next y. */
static Gfx *modSpawnEditMenuRender(Gfx *gdl, s32 left, s32 y)
{
	char text[96];
	s32 i;

	if (g_ModSpawnMenu == MODSPAWN_MENU_CLOSED) {
		return gdl;
	}

	y += 4;

	if (g_ModSpawnMenu == MODSPAWN_MENU_NAME) {
		snprintf(text, sizeof(text), "Save as:  %s_", g_ModSpawnMenuName);
		gdl = modSpawnEditLine(gdl, left, y, text, 0xffffffff);
		y += 7;
		gdl = modSpawnEditLine(gdl, left, y, "Type a name   Enter save   Esc back", 0xffffff80);
		return gdl;
	}

	{
		s32 count = modSpawnEditMenuCount();

		for (i = 0; i < count; i++) {
			const char *cursor = i == g_ModSpawnMenuSel ? ">" : " ";
			if (g_ModSpawnMenu == MODSPAWN_MENU_COMMANDS) {
				snprintf(text, sizeof(text), "%s Clear %s (%d)", cursor, g_ModSpawnNames[i], g_ModSpawnCounts[i]);
			} else {
				const char *label = g_ModSpawnMenu == MODSPAWN_MENU_MAIN ? g_ModSpawnMenuItems[i] : g_ModSpawnMenuFiles[i];
				snprintf(text, sizeof(text), "%s %s", cursor, label);
			}
			gdl = modSpawnEditLine(gdl, left, y, text, i == g_ModSpawnMenuSel ? 0xffff40ff : 0xffffffff);
			y += 7;
		}
	}

	gdl = modSpawnEditLine(gdl, left, y, "Up/Down choose   Enter select   Esc back   M close", 0xffffff80);
	return gdl;
}

static void modSpawnEditTickWork(void)
{
	if (!lvIsPaused() && !g_Vars.currentplayer->mpmenuon && g_ModSpawnEditLoaded) {
		bool menuWasOpen = modSpawnEditMenuOpen();
		modSpawnEditMenuTick(menuWasOpen && inputKeyJustPressed(VK_ESCAPE));

		if (!menuWasOpen && g_ModSpawnMenu == MODSPAWN_MENU_CLOSED) {
			modSpawnEditTickInput();
		}

		modSpawnEditTickModels();
		modSpawnEditTickLaptops();
		modSpawnEditTickGhost();
	}
}

/** Move the player to [newpos] if their body fits there; true if it did. */
static bool modSpawnEditFlyTry(struct coord *newpos)
{
	struct player *player = g_Vars.currentplayer;
	RoomNum rooms[8];
	f32 radius;
	f32 ymax;
	f32 ymin;
	s32 result;

	playerGetBbox(player->prop, &radius, &ymax, &ymin);
	func0f065e74(&player->prop->pos, player->prop->rooms, newpos, rooms);
	bmoveFindEnteredRoomsByPos(player, newpos, rooms);
	propSetPerimEnabled(player->prop, false);

	result = cdTestVolume(newpos, radius, rooms, CDTYPE_BG | CDTYPE_CLOSEDDOORS, CHECKVERTICAL_YES,
			ymax - player->prop->pos.y, ymin - 0.1f - player->prop->pos.y);

	propSetPerimEnabled(player->prop, true);

	if (result == CDRESULT_NOCOLLISION && rooms[0] != -1) {
		player->prop->pos = *newpos;
		propDeregisterRooms(player->prop);
		roomsCopy(rooms, player->prop->rooms);
		return true;
	}

	return false;
}

void modSpawnEditFlyTick(void)
{
	struct player *player = g_Vars.currentplayer;
	struct coord *pos = &player->prop->pos;
	f32 forward = player->speedforwards;
	f32 sideways = player->speedsideways;
	f32 vertical = 0;
	f32 mult = g_Vars.lvupdate60freal * 10.0f;
	f32 rotate;
	f32 theta;

	bwalkUpdatePrevPos();

	// Turn: what bwalkUpdateTheta() adds, without its collision test
	rotate = player->speedtheta * g_Vars.lvupdate60freal * 0.0174505133f * 3.5f;
	theta = player->vv_theta + (rotate * 360.0f) / M_BADTAU;

	while (theta < 0) {
		theta += 360;
	}

	while (theta >= 360) {
		theta -= 360;
	}

	player->vv_theta = theta;
	bmoveUpdateVerta();

	// The walk has no step of its own to start: the camera is where we say
	player->walkinitmove = false;

	// The camera's orientation is rebuilt from vv_theta and vv_verta here, and
	// in the walk only bwalk0f0c69b8() does it - which flight does not run, so
	// without this the view never turned (no mouselook)
	bmove0f0cc654(0.0f, 0.0f, 0.0f);

	if (g_ModSpawnMenu == MODSPAWN_MENU_CLOSED && inputKeyJustPressed(VK_A + ('F' - 'A'))) {
		g_ModSpawnFlying = false;
		modSpawnEditSay("Walking: Space to fly");
		modSpawnEditTickWork();
		return;
	}

	if (g_ModSpawnMenu == MODSPAWN_MENU_CLOSED) {
		if (inputKeyPressed(VK_SPACE)) {
			vertical += 1.0f;
		}

		if (inputKeyPressed(VK_LCTRL)) {
			vertical -= 1.0f;
		}

		if (inputKeyPressed(VK_LSHIFT)) {
			mult *= 3.0f;
		}
	}

	// Forward follows the look, pitch included; sideways stays level. The
	// level move is the walk's own (bond2.unk00 is the level look). Flight has
	// the player's own collision: it cannot pass a wall or a floor, so it
	// cannot leave the map (the camera outside it had no rooms, and the editor
	// misbehaved there).
	{
		struct coord goal;
		struct coord step;
		f32 dx = (player->bond2.unk00.f[0] * player->vv_cosverta * forward
				- player->bond2.unk00.f[2] * sideways) * mult;
		f32 dz = (player->bond2.unk00.f[2] * player->vv_cosverta * forward
				+ player->bond2.unk00.f[0] * sideways) * mult;
		f32 dy = (player->vv_sinverta * forward + vertical) * mult;
		bool movedy;

		goal = *pos;
		step = goal;
		step.x += dx;
		step.y += dy;
		step.z += dz;

		if (!modSpawnEditFlyTry(&step)) {
			// slide along what is in the way: one axis at a time
			step = *pos;
			step.x += dx;
			modSpawnEditFlyTry(&step);

			step = *pos;
			step.z += dz;
			modSpawnEditFlyTry(&step);

			step = *pos;
			step.y += dy;
			movedy = modSpawnEditFlyTry(&step);

			// pushing down onto the floor lands the player: walking again
			if (!movedy && dy < -0.01f) {
				g_ModSpawnFlying = false;
				modSpawnEditSay("Landed: Space to fly");
			}
		}
	}

	bmoveUpdateRooms(player);
	bmove0f0cc19c(&player->prop->pos);
	playerUpdatePerimInfo();

	modSpawnEditTickWork();
}

bool modSpawnEditFlying(void)
{
	return g_ModSpawnFlying;
}

/** Called each tick instead of nothing when the player is walking: Space takes off. */
void modSpawnEditWalkTick(void)
{
	if (g_ModSpawnMenu == MODSPAWN_MENU_CLOSED && inputKeyJustPressed(VK_SPACE)) {
		g_ModSpawnFlying = true;
		modSpawnEditSay("Flying: Space up, Ctrl down, F to land");
	}

	modSpawnEditTickWork();
}

/* ------------------------------------------------------------------------
 * Drawing
 * --------------------------------------------------------------------- */

static Gfx *modSpawnEditLine(Gfx *gdl, s32 x, s32 y, char *text, u32 colour)
{
	s32 textheight;
	s32 textwidth;

	textMeasure(&textheight, &textwidth, text, g_CharsHandelGothicXs, g_FontHandelGothicXs, 0);
	gdl = text0f153a34(gdl, x, y, x + textwidth, y + textheight, 0);

	return textRender(gdl, &x, &y, text, g_CharsHandelGothicXs, g_FontHandelGothicXs, colour, 0x000000a0,
			viGetWidth(), viGetHeight(), 0, 0);
}

/**
 * Whether the eye can see a point: nothing solid between them. Asked with a
 * budget (see g_ModSpawnSeen); a marker not yet asked about is not drawn.
 */
static bool modSpawnEditSees(s32 index, struct coord *eye, struct coord *point, s32 *budget)
{
	const s32 frame = g_Vars.lvframenum + 1; // never 0

	if (g_ModSpawnSeenFrame[index] == 0 || frame - g_ModSpawnSeenFrame[index] >= MODSPAWN_LOS_STALE) {
		if (*budget <= 0) {
			return g_ModSpawnSeenFrame[index] != 0 && g_ModSpawnSeen[index];
		}

		(*budget)--;
		g_ModSpawnSeenFrame[index] = frame;
		g_ModSpawnSeen[index] = g_Vars.currentplayer->prop->rooms[0] < 0
			|| cdExamLos08(eye, g_Vars.currentplayer->prop->rooms, point,
				CDTYPE_BG | CDTYPE_CLOSEDDOORS,
				GEOFLAG_FLOOR1 | GEOFLAG_FLOOR2 | GEOFLAG_WALL | GEOFLAG_BLOCK_SIGHT) != CDRESULT_COLLISION;
	}

	return g_ModSpawnSeen[index];
}

/**
 * The markers, and the HUD lines under them, from playerRender() after the
 * rest of the HUD. Each marker is a square at the spawn and a short row of
 * dots along its facing; the nearest to the crosshair is outlined in white
 * and named, and is the one the delete and turn keys act on.
 */
Gfx *modSpawnEditRender(Gfx *gdl)
{
	struct player *player = g_Vars.currentplayer;
	struct coord eye;
	s32 scale = 1;
	s32 i;
	s32 centrex;
	s32 centrey;
	s32 aimed = -1;
	s32 aimedx = 0;
	s32 aimedy = 0;
	s32 aimedhalf = 0;
	s32 aimeddist = MODSPAWN_PICKPX * MODSPAWN_PICKPX;
	s32 losbudget = MODSPAWN_LOS_BUDGET;
	char text[128];
	s32 left;
	s32 y;

	if (!g_ModSpawnEditOn || !g_ModSpawnEditLoaded || player == NULL || player->prop == NULL
			|| lvIsPaused() || player->mpmenuon) {
		return gdl;
	}

#if !PAL
	if (g_ViRes == VIRES_HI) {
		scale = 2;
	}
#endif

	eye = player->prop->pos;
	centrex = viGetViewLeft() + viGetViewWidth() / 2;
	centrey = viGetViewTop() + viGetViewHeight() / 2;

	g_ScaleX = scale;
	gdl = func0f153134(gdl);

	for (i = 0; i < g_ModNumSpawns; i++) {
		struct modspawn *spawn = &g_ModSpawns[i];
		struct coord world;
		struct coord scr;
		f32 dx = spawn->pos.x - eye.x;
		f32 dy = spawn->pos.y - eye.y;
		f32 dz = spawn->pos.z - eye.z;
		f32 dist = sqrtf(dx * dx + dy * dy + dz * dz);
		s32 x;
		s32 yy;
		s32 half;
		s32 d;
		struct modspawnmodel *model;

		if (dist > MODSPAWN_DRAWDIST) {
			continue;
		}

		// Each kind floats a little higher than the last, so a guard, a hunter
		// and an objective on one pad do not sit on one pixel
		model = modSpawnEditModelFor(spawn->id);

		if (model) {
			world = model->pos;
		} else if (spawn->kind != MODSPAWN_ESCAPE || !modSpawnEditLaptopPos(spawn->id, &world)) {
			continue; // no little squares: a spawn without its model yet is not shown
		}

		// aim at the torso
		world.y += 55.0f;

		if (!bg3dPosTo2dPos(&world, &scr)) {
			continue;
		}

		x = (s32)scr.x;
		yy = (s32)scr.y;

		if (x < viGetViewLeft() + 3 || x > viGetViewLeft() + viGetViewWidth() - 3
				|| yy < viGetViewTop() + 3 || yy > viGetViewTop() + viGetViewHeight() - 3) {
			continue;
		}

		// not through walls: a marker behind something solid is not drawn and
		// cannot be aimed at
		if (!modSpawnEditSees(i, &eye, &world, &losbudget)) {
			continue;
		}

		half = 7;

		d = (x - centrex) * (x - centrex) + (yy - centrey) * (yy - centrey);

		if (d < aimeddist && dist <= MODSPAWN_DELETERANGE) {
			aimeddist = d;
			aimed = i;
			aimedx = x;
			aimedy = yy;
			aimedhalf = half;
		}

		if (1) {
			continue; // the character is the marker
		}

		gdl = textSetPrimColour(gdl, g_ModSpawnColours[spawn->kind]);
		gDPFillRectangleScaled(gdl++, x / scale - half, yy - half, x / scale + half, yy + half);
		gdl = text0f153838(gdl);

		// the facing: three dots along it, if it is near enough to read
		if (dist < 2500) {
			s32 step;

			for (step = 1; step <= 3; step++) {
				struct coord ahead = world;
				struct coord aheadscr;

				ahead.x += sinf(spawn->angle) * 45.0f * step;
				ahead.z += cosf(spawn->angle) * 45.0f * step;

				if (bg3dPosTo2dPos(&ahead, &aheadscr)) {
					s32 ax = (s32)aheadscr.x;
					s32 ay = (s32)aheadscr.y;

					if (ax > viGetViewLeft() + 2 && ax < viGetViewLeft() + viGetViewWidth() - 2
							&& ay > viGetViewTop() + 2 && ay < viGetViewTop() + viGetViewHeight() - 2) {
						gdl = textSetPrimColour(gdl, g_ModSpawnColours[spawn->kind]);
						gDPFillRectangleScaled(gdl++, ax / scale - 1, ay - 1, ax / scale, ay);
						gdl = text0f153838(gdl);
					}
				}
			}
		}
	}

	g_ModSpawnAimed = aimed;

	// The aimed marker gets a white frame
	if (aimed >= 0) {
		const s32 outer = aimedhalf + 3;
		const s32 cx = aimedx / scale;

		gdl = textSetPrimColour(gdl, 0xffffffff);
		gDPFillRectangleScaled(gdl++, cx - outer, aimedy - outer, cx + outer + 1, aimedy - outer + 1);
		gDPFillRectangleScaled(gdl++, cx - outer, aimedy + outer, cx + outer + 1, aimedy + outer + 1);
		gDPFillRectangleScaled(gdl++, cx - outer, aimedy - outer, cx - outer + 1, aimedy + outer + 1);
		gDPFillRectangleScaled(gdl++, cx + outer, aimedy - outer, cx + outer + 1, aimedy + outer + 1);
		gdl = text0f153838(gdl);
	}

	g_ScaleX = 1;

	// The HUD lines
	g_ScaleX = g_ViRes == VIRES_HI ? 2 : 1;
	left = viGetViewLeft() / g_ScaleX + 8;
	y = viGetViewTop() + 24;

	gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, g_HudAlignModeL);
	gdl = text0f153628(gdl);

	snprintf(text, sizeof(text), "Config: %s%s   (M menu)", g_ModSpawnConfigName, g_ModSpawnDirty ? " *" : "");
	gdl = modSpawnEditLine(gdl, left, y, text, 0xc0e0ffff);
	y += 7;

	snprintf(text, sizeof(text), "Guard %d  Hunter %d  Objective %d  Escape/Spawn %d  Guaranteed %d",
			g_ModSpawnCounts[MODSPAWN_GUARD], g_ModSpawnCounts[MODSPAWN_HUNTER],
			g_ModSpawnCounts[MODSPAWN_OBJECTIVE], g_ModSpawnCounts[MODSPAWN_ESCAPE],
			g_ModSpawnCounts[MODSPAWN_GUARANTEED]);
	gdl = modSpawnEditLine(gdl, left, y, text, 0xffffffff);
	y += 7;

	snprintf(text, sizeof(text), "Placing: %s   (1-5; wheel turns)", g_ModSpawnNames[g_ModSpawnPlaceKind]);
	gdl = modSpawnEditLine(gdl, left, y, text, g_ModSpawnColours[g_ModSpawnPlaceKind]);
	y += 7;

	snprintf(text, sizeof(text), "Click place   Right-click delete   , . turn   Backspace undo");
	gdl = modSpawnEditLine(gdl, left, y, text, 0xffffff80);
	y += 7;

	if (g_ModSpawnFlying) {
		snprintf(text, sizeof(text), "FLYING:  F land   Space up   Ctrl down   Shift fast");
		gdl = modSpawnEditLine(gdl, left, y, text, 0x80ff80ff);
	} else {
		snprintf(text, sizeof(text), "WALKING:  Space take off and fly");
		gdl = modSpawnEditLine(gdl, left, y, text, 0xffffff80);
	}

	y += 7;

	snprintf(text, sizeof(text), "Room %d   x %d  y %d  z %d",
			(int)player->prop->rooms[0], (int)eye.x, (int)eye.y, (int)eye.z);
	gdl = modSpawnEditLine(gdl, left, y, text, 0xffffff60);
	y += 7;

	if (g_ModNumSpawns > 0 && g_ModSpawnCounts[MODSPAWN_HUNTER] < 6) {
		snprintf(text, sizeof(text), "Warning: fewer than 6 Hunter spawns");
		gdl = modSpawnEditLine(gdl, left, y, text, 0xff8000ff);
		y += 7;
	}

	if (g_Vars.lvframe60 < g_ModSpawnMsgUntil60 && g_ModSpawnMsg[0]) {
		gdl = modSpawnEditLine(gdl, left, y, g_ModSpawnMsg, 0x80ffffff);
		y += 7;
	}

	gdl = modSpawnEditMenuRender(gdl, left, y);

	if (aimed >= 0 && g_ModSpawnMenu == MODSPAWN_MENU_CLOSED) {
		struct modspawn *spawn = &g_ModSpawns[aimed];

		if (spawn->pad >= 0) {
			snprintf(text, sizeof(text), "%s  pad %d  room %d  x %d  y %d  z %d",
					g_ModSpawnNames[spawn->kind], (int)spawn->pad, (int)spawn->room,
					(int)spawn->pos.x, (int)spawn->pos.y, (int)spawn->pos.z);
		} else {
			snprintf(text, sizeof(text), "%s  placed  room %d  x %d  y %d  z %d",
					g_ModSpawnNames[spawn->kind], (int)spawn->room,
					(int)spawn->pos.x, (int)spawn->pos.y, (int)spawn->pos.z);
		}

		gdl = modSpawnEditLine(gdl, left, y + 4, text, g_ModSpawnColours[spawn->kind]);
	}

	gdl = text0f153780(gdl);
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
	g_ScaleX = 1;

	return gdl;
}

/* ------------------------------------------------------------------------
 * Configs in a run
 *
 * "Use in runs" in the editor's menu (or the Play button) names a config for
 * one map. When that map is dealt as a Perfect Random mission, the roll calls
 * modSpawnConfigPrepare(): the file is read, every spawn becomes a pad after
 * the stage's own (so everything that works in pads - guards, the start, the
 * respawns - works unchanged) and the pads are listed by kind. modrandom.c
 * puts the guards, the objective guards and the start on them; modalarm.c
 * draws reinforcement waves from the hunter pads. A kind with no spawns in
 * the config keeps the map's default. Nothing here runs unless a config has
 * been chosen, so every seed deals as it always did.
 * --------------------------------------------------------------------- */

static bool g_ModCfgLoaded;                    // loaded for the stage being played
static s32 g_ModCfgCount[MODSPAWN_NUMKINDS];
static s16 g_ModCfgPads[MODSPAWN_NUMKINDS][MODSPAWN_MAX];
static struct waypoint g_ModCfgHunters[MODSPAWN_MAX];
static bool g_ModCfgHuntersReady;

void modSpawnConfigUse(s32 stagenum, const char *name)
{
	if (stagenum < 0 || stagenum > STAGE_MAX_ID) {
		return;
	}

	snprintf(g_ModCfgChoices[stagenum], sizeof(g_ModCfgChoices[stagenum]), "%s", name);
	sysLogPrintf(LOG_NOTE, "spawnconfig: runs on stage 0x%02x use \"%s\"", stagenum, name);
}

void modSpawnConfigOff(s32 stagenum)
{
	if (stagenum >= 0 && stagenum <= STAGE_MAX_ID) {
		g_ModCfgChoices[stagenum][0] = '\0';
	}
}

bool modSpawnConfigIsChosen(s32 stagenum)
{
	return stagenum >= 0 && stagenum <= STAGE_MAX_ID && g_ModCfgChoices[stagenum][0] != '\0';
}

const char *modSpawnConfigChosenName(s32 stagenum)
{
	if (modSpawnConfigIsChosen(stagenum)) {
		return g_ModCfgChoices[stagenum];
	}

	return modSpawnEditHasBuiltinDefault(stagenum) ? "Default" : NULL;
}

const char *modSpawnConfigName(void)
{
	return g_ModCfgLoaded ? g_ModCfgName : NULL;
}

/** Put [count] spawns on the stage as pads after its own; the first pad's number, or -1. */
static s32 modSpawnConfigAppendPads(const struct modspawn *spawns, s32 count)
{
	const s32 oldnum = g_PadsFile->numpads;
	const u32 headflags = PADFLAG_UPALIGNTOY; // up is Y, the look is stored
	const size_t each = 32;                   // header, position, look, padded
	size_t end = (u8 *)&g_PadOffsets[oldnum] - (u8 *)g_StageSetup.padfiledata;
	size_t at;
	u8 *buf;
	u16 *offsets;
	s32 i;

	for (i = 0; i < oldnum; i++) {
		u32 header = *(u32 *)&g_StageSetup.padfiledata[g_PadOffsets[i]];
		u32 flags = header >> 14;
		size_t len = 4;

		len += (flags & PADFLAG_INTPOS) ? 8 : 12;
		len += (flags & (PADFLAG_UPALIGNTOX | PADFLAG_UPALIGNTOY | PADFLAG_UPALIGNTOZ)) ? 0 : 12;
		len += (flags & (PADFLAG_LOOKALIGNTOX | PADFLAG_LOOKALIGNTOY | PADFLAG_LOOKALIGNTOZ)) ? 0 : 12;
		len += (flags & PADFLAG_HASBBOXDATA) ? 24 : 0;

		if (g_PadOffsets[i] + len > end) {
			end = g_PadOffsets[i] + len;
		}
	}

	at = ALIGN16(end);

	if (count <= 0 || oldnum + count > 0xfff0 || at + (size_t)count * each > 0xffff) {
		return -1;
	}

	buf = mempAlloc(ALIGN16(at + count * each), MEMPOOL_STAGE);
	offsets = mempAlloc(ALIGN16((oldnum + count) * sizeof(u16)), MEMPOOL_STAGE);

	if (buf == NULL || offsets == NULL) {
		return -1;
	}

	memcpy(buf, g_StageSetup.padfiledata, end);
	memcpy(offsets, g_PadOffsets, oldnum * sizeof(u16));

	for (i = 0; i < count; i++) {
		u32 *header = (u32 *)&buf[at];
		f32 *fpos = (f32 *)&buf[at + 4];
		f32 *flook = (f32 *)&buf[at + 16];

		*header = (headflags << 14) | (((u32)spawns[i].room & 0x3ff) << 4);
		fpos[0] = spawns[i].pos.x;
		fpos[1] = spawns[i].pos.y;
		fpos[2] = spawns[i].pos.z;
		flook[0] = sinf(spawns[i].angle);
		flook[1] = 0.0f;
		flook[2] = cosf(spawns[i].angle);
		offsets[oldnum + i] = (u16)at;
		at += each;
	}

	g_StageSetup.padfiledata = (s8 *)buf;
	g_PadsFile = (struct padsfileheader *)buf;
	g_PadOffsets = offsets;
	g_PadsFile->numpads = oldnum + count;

	return oldnum;
}

/**
 * G5 Building: a guard or a hunter put on a dumpster stands on top of it (or
 * inside it). Drops the config's guard, hunter and guaranteed-guard spawns that
 * fall within a dumpster's footprint; the count left is returned. Each drop is
 * logged with the dumpster's position so the radius can be checked in a log.
 */
#define MODSPAWN_DUMPSTER_RADIUS 110.0f

static s32 modSpawnDropDumpsterSpawns(s32 stagenum, struct modspawn *buf, s32 count)
{
	u32 *cmd = g_StageSetup.props;
	s32 dumpsters = 0;
	s32 dropped = 0;

	if (stagenum != STAGE_G5BUILDING || buf == NULL) {
		return count;
	}

	while (cmd && ((struct defaultobj *)cmd)->type != OBJTYPE_END) {
		struct defaultobj *obj = (struct defaultobj *)cmd;

		if (obj->type == OBJTYPE_BASIC && obj->pad >= 0
				&& (obj->modelnum == MODEL_DUMPSTER || obj->modelnum == MODEL_G5DUMPSTER)) {
			struct pad pad;
			s32 i;
			s32 kept = 0;

			padUnpack(obj->pad, PADFIELD_POS, &pad);
			dumpsters++;

			for (i = 0; i < count; i++) {
				const f32 dx = buf[i].pos.x - pad.pos.x;
				const f32 dz = buf[i].pos.z - pad.pos.z;
				const f32 dy = buf[i].pos.y - pad.pos.y;
				const bool kind = buf[i].kind == MODSPAWN_GUARD || buf[i].kind == MODSPAWN_HUNTER
						|| buf[i].kind == MODSPAWN_GUARANTEED;

				if (kind && dx * dx + dz * dz < MODSPAWN_DUMPSTER_RADIUS * MODSPAWN_DUMPSTER_RADIUS
						&& dy > -100.0f && dy < 250.0f) {
					sysLogPrintf(LOG_NOTE, "spawnconfig: dropped a %s spawn at (%.0f %.0f %.0f): on the dumpster at (%.0f %.0f %.0f)",
							buf[i].kind == MODSPAWN_HUNTER ? "hunter" : "guard",
							buf[i].pos.x, buf[i].pos.y, buf[i].pos.z, pad.pos.x, pad.pos.y, pad.pos.z);
					dropped++;
					continue;
				}

				if (kept != i) {
					buf[kept] = buf[i];
				}

				kept++;
			}

			count = kept;
		}

		cmd += setupGetCmdLength(cmd);
	}

	sysLogPrintf(LOG_NOTE, "spawnconfig: %d dumpsters found, %d spawns dropped", dumpsters, dropped);
	return count;
}

/**
 * Called by the roll once the stage's pads exist. True when a config is in
 * force for this map and has at least one spawn.
 */
bool modSpawnConfigPrepare(s32 stagenum)
{
	const char *name = modSpawnConfigChosenName(stagenum);
	const char *err;
	s32 rejected;
	s32 count;
	s32 first;
	s32 i;

	g_ModCfgLoaded = false;
	g_ModCfgHuntersReady = false;
	memset(g_ModCfgCount, 0, sizeof(g_ModCfgCount));

	if (!name || g_ModSpawnEditOn || g_PadsFile == NULL) {
		return false;
	}

	count = modSpawnReadFile(stagenum, name, g_ModSpawnLoadBuf, &rejected, &err);

	if (count <= 0) {
		sysLogPrintf(LOG_WARNING, "spawnconfig: \"%s\" not used: %s", name, count < 0 ? err : "it has no spawns");
		return false;
	}

	count = modSpawnDropDumpsterSpawns(stagenum, g_ModSpawnLoadBuf, count);

	if (count <= 0) {
		sysLogPrintf(LOG_WARNING, "spawnconfig: \"%s\" not used: it has no spawns left", name);
		return false;
	}

	first = modSpawnConfigAppendPads(g_ModSpawnLoadBuf, count);

	if (first < 0) {
		sysLogPrintf(LOG_WARNING, "spawnconfig: \"%s\" not used: no room for %d pads", name, count);
		return false;
	}

	for (i = 0; i < count; i++) {
		u8 kind = g_ModSpawnLoadBuf[i].kind;

		g_ModCfgPads[kind][g_ModCfgCount[kind]++] = first + i;
	}

	snprintf(g_ModCfgName, sizeof(g_ModCfgName), "%s", name);
	g_ModCfgLoaded = true;
	sysLogPrintf(LOG_NOTE, "spawnconfig: \"%s\" in force: %d guard, %d hunter, %d objective, %d escape (%d did not fit)",
			g_ModCfgName, g_ModCfgCount[MODSPAWN_GUARD], g_ModCfgCount[MODSPAWN_HUNTER],
			g_ModCfgCount[MODSPAWN_OBJECTIVE], g_ModCfgCount[MODSPAWN_ESCAPE], rejected);
	return true;
}

s32 modSpawnConfigCount(s32 kind)
{
	return g_ModCfgLoaded && kind >= 0 && kind < MODSPAWN_NUMKINDS ? g_ModCfgCount[kind] : 0;
}

s32 modSpawnConfigPad(s32 kind, s32 index)
{
	return index >= 0 && index < modSpawnConfigCount(kind) ? g_ModCfgPads[kind][index] : -1;
}

bool modSpawnConfigIsPadOfKind(s32 kind, s32 padnum)
{
	s32 i;

	for (i = 0; i < modSpawnConfigCount(kind); i++) {
		if (g_ModCfgPads[kind][i] == padnum) {
			return true;
		}
	}

	return false;
}

/**
 * The hunter spawns as waypoints, for the code that draws reinforcements from
 * the stage's waypoints: the pad, and the group of the nearest real waypoint
 * (a run asks whether a waypoint's group has a route to the player). NULL
 * when the config has no hunters.
 */
struct waypoint *modSpawnConfigHunters(s32 *count)
{
	s32 n = modSpawnConfigCount(MODSPAWN_HUNTER);
	s32 i;

	if (n <= 0) {
		return NULL;
	}

	if (!g_ModCfgHuntersReady) {
		struct waypoint *real = g_StageSetup.waypoints;

		for (i = 0; i < n; i++) {
			struct pad pad;
			f32 best = 1e30f;
			s32 j;

			g_ModCfgHunters[i].padnum = g_ModCfgPads[MODSPAWN_HUNTER][i];
			g_ModCfgHunters[i].neighbours = NULL;
			g_ModCfgHunters[i].groupnum = 0;
			g_ModCfgHunters[i].step = 0;
			padUnpack(g_ModCfgHunters[i].padnum, PADFIELD_POS, &pad);

			for (j = 0; real && real[j].padnum >= 0; j++) {
				struct pad other;
				f32 dx, dy, dz, d;

				padUnpack(real[j].padnum, PADFIELD_POS, &other);
				dx = other.pos.x - pad.pos.x;
				dy = other.pos.y - pad.pos.y;
				dz = other.pos.z - pad.pos.z;
				d = dx * dx + dy * dy + dz * dz;

				if (d < best) {
					best = d;
					g_ModCfgHunters[i].groupnum = real[j].groupnum;
				}
			}
		}

		g_ModCfgHuntersReady = true;
	}

	*count = n;
	return g_ModCfgHunters;
}

/* ------------------------------------------------------------------------
 * Self-test (regression test), run with:  pd.x86_64.exe --selftest-spawnedit [--selftest-stage 0x32]
 *
 * Boots, opens the editor on the stage, then drives it with no input: places,
 * deletes and undoes spawns, flies and walks, teleports about the map so that
 * characters and laptops are made and taken down in bulk, and cycles the
 * placement preview's kind. Every tick it checks that the game's prop list
 * holds no prop whose character or object has gone (the crash we had twice)
 * and that the camera always has a room. Results go to pd.log as SELFTEST
 * lines; the process exits 0 if all passed, 1 if not. test/run_tests.sh runs
 * it for several maps and collects the result.
 * --------------------------------------------------------------------- */

#define TEST_SETTLE  200   // ticks after load before the churn starts
#define TEST_CHURN   1200  // ticks of churn
#define TEST_LOADMAX 2400  // ticks to wait for the editor to open

static struct {
	bool started;
	bool loaded;
	s32 waited;
	s32 tick;
	s32 passed;
	s32 failed;
	s32 baseline;
	s32 teleports;
	s32 teleportindex;
	s32 places;
	s32 placeok;
	bool ghostseen;
	bool lastteleported;
	bool staleReported;
	bool roomReported;
	u32 escapeid;
} g_Test;

static void modSpawnEditTestCheck(bool ok, const char *name, const char *detail)
{
	if (ok) {
		g_Test.passed++;
		sysLogPrintf(LOG_NOTE, "SELFTEST PASS %s", name);
	} else {
		g_Test.failed++;
		sysLogPrintf(LOG_NOTE, "SELFTEST FAIL %s: %s", name, detail ? detail : "");
	}
}

static void modSpawnEditTestFinish(void)
{
	sysLogPrintf(LOG_NOTE, "SELFTEST RESULT stage 0x%02x passed %d failed %d", g_SpawnEditTestStage,
			g_Test.passed, g_Test.failed);
	exit(g_Test.failed ? 1 : 0);
}

/** Props in the active list whose character or object is gone. */
static s32 modSpawnEditTestStaleProps(void)
{
	struct prop *prop;
	s32 bad = 0;
	s32 n = 0;

	for (prop = g_Vars.activeprops; prop && prop != g_Vars.pausedprops && n < 200000; prop = prop->next, n++) {
		if (prop->type == PROPTYPE_CHR) {
			if (prop->chr == NULL) {
				bad++;
			}
		} else if (prop->type == PROPTYPE_OBJ || prop->type == PROPTYPE_WEAPON || prop->type == PROPTYPE_DOOR) {
			if (prop->obj == NULL) {
				bad++;
			}
		}
	}

	return bad;
}

static bool modSpawnEditTestAddAt(s32 kind, struct coord at)
{
	struct coord down;
	struct coord pos;
	RoomNum room;
	const char *why;
	struct modspawn sp;

	down.x = 0;
	down.y = -1;
	down.z = 0;

	if (!modSpawnEditSnap(&at, &down, &pos, &room, &why)) {
		return false;
	}

	memset(&sp, 0, sizeof(sp));
	sp.id = ++g_ModSpawnNextId;
	sp.kind = kind;
	sp.pad = -1;
	sp.pos = pos;
	sp.room = room;
	sp.angle = 0;
	modSpawnEditInsert(g_ModNumSpawns, &sp);
	return true;
}

static void modSpawnEditTestTeleport(struct coord *to)
{
	struct player *player = g_Vars.currentplayer;
	struct coord q = *to;
	RoomNum in[21];
	RoomNum above[21];
	RoomNum best = -1;
	s32 i;

	q.y += 120.0f;
	in[0] = -1;
	above[0] = -1;
	bgFindRoomsByPos(&q, in, above, 20, &best);

	if (in[0] == -1) {
		return;
	}

	propDeregisterRooms(player->prop);
	player->prop->pos = q;

	for (i = 0; i < 7 && in[i] != -1; i++) {
		player->prop->rooms[i] = in[i];
	}

	player->prop->rooms[i] = -1;
	propRegisterRooms(player->prop);
	g_Test.teleports++;
	g_Test.lastteleported = true;
}

/* ------------------------------------------------------------------------
 * Self-test for configs in a run:  pd.x86_64.exe --selftest-spawnconfig [--selftest-stage 0x32]
 *
 * Opens the editor, keeps a handful of the map's default spawns as a saved
 * config, chooses it, leaves the editor and has the map dealt as a Perfect
 * Random mission. Then checks that the config was read and became pads, that
 * the player started on its Escape spawn, that a guard stands on one of its
 * Guard/Objective spawns, that the hunter pool is its Hunter spawns, and that
 * the game's prop list is sound.
 * --------------------------------------------------------------------- */

bool g_SpawnConfigTest;

#define CFGTEST_NAME "selftest run"

static struct {
	s32 phase;
	s32 waited;
	s32 tick;
	bool cutscenePlayed;
	s32 count[MODSPAWN_NUMKINDS];
	struct coord pos[MODSPAWN_NUMKINDS][4];
} g_CfgTest;

static f32 modSpawnConfigTestDist(const struct coord *a, const struct coord *b)
{
	f32 dx = a->x - b->x;
	f32 dz = a->z - b->z;

	return sqrtf(dx * dx + dz * dz);
}

static void modSpawnConfigTestTick(void)
{
	struct player *player = g_Vars.currentplayer;
	char detail[96];
	char path[96];
	s32 i;
	s32 k;

	if (player == NULL || player != g_Vars.bond) {
		return;
	}

	if (g_CfgTest.phase >= 1 && g_CfgTest.phase <= 3 && g_Vars.stagenum == g_SpawnEditTestStage
			&& (g_InCutscene || g_Vars.tickmode == TICKMODE_CUTSCENE || g_Vars.tickmode == TICKMODE_WARP)) {
		g_CfgTest.cutscenePlayed = true;
	}

	if (g_CfgTest.phase == 0) {
		if (g_Vars.lvframenum > 60 && !modSpawnEditIsOn()) {
			sysLogPrintf(LOG_NOTE, "SELFTEST START config stage 0x%02x", g_SpawnEditTestStage);
			modSpawnEditStart(g_SpawnEditTestStage);
			g_CfgTest.phase = 1;
		}

		return;
	}

	if (g_CfgTest.phase == 1) {
		static struct modspawn subset[16];
		s32 n = 0;

		if (!(modSpawnEditIsOn() && g_ModSpawnEditLoaded && g_Vars.stagenum == g_SpawnEditTestStage)) {
			if (++g_CfgTest.waited > TEST_LOADMAX) {
				modSpawnEditTestCheck(false, "editor opens", "the editor did not open in time");
				modSpawnEditTestFinish();
			}

			return;
		}

		if (++g_CfgTest.tick < 20) {
			return;
		}

		// keep a few of each kind (the Escape/Spawn and Guard first: both are the map's own)
		for (k = 0; k < MODSPAWN_NUMKINDS; k++) {
			s32 want = k == MODSPAWN_GUARD ? 4 : k == MODSPAWN_HUNTER ? 3 : k == MODSPAWN_OBJECTIVE ? 2 : 1;

			for (i = 0; i < g_ModNumSpawns && g_CfgTest.count[k] < want; i++) {
				if (g_ModSpawns[i].kind == k) {
					g_CfgTest.pos[k][g_CfgTest.count[k]++] = g_ModSpawns[i].pos;
					subset[n++] = g_ModSpawns[i];
				}
			}
		}

		// two Guaranteed Guards: the map has none, so they are made from a fifth and sixth Guard spawn
		{
			s32 guards = 0;

			for (i = 0; i < g_ModNumSpawns; i++) {
				if (g_ModSpawns[i].kind == MODSPAWN_GUARD && guards++ >= 4) {
					subset[n] = g_ModSpawns[i];
					subset[n].kind = MODSPAWN_GUARANTEED;
					g_CfgTest.pos[MODSPAWN_GUARANTEED][g_CfgTest.count[MODSPAWN_GUARANTEED]++] = subset[n].pos;
					n++;

					if (g_CfgTest.count[MODSPAWN_GUARANTEED] >= 2) {
						break; // two: the returning-guard check needs one that is free
					}
				}
			}
		}

		modSpawnEditApply(subset, n);
		modSpawnEditTestCheck(modSpawnEditSaveConfig(CFGTEST_NAME), "the test config saves", "save failed");
		{
			s32 other = g_SpawnEditTestStage == STAGE_DEFECTION ? STAGE_VILLA : STAGE_DEFECTION;
			modSpawnConfigUse(other, CFGTEST_NAME);
			modSpawnEditTestCheck(modSpawnConfigIsChosen(g_SpawnEditTestStage) && modSpawnConfigIsChosen(other),
					"choosing another map layout keeps this map's selection", "a layout selection was lost");
			modSpawnConfigOff(other);
			modSpawnEditTestCheck(modSpawnConfigIsChosen(g_SpawnEditTestStage) && !modSpawnConfigIsChosen(other),
					"restoring one map to default keeps the other layout", "the wrong layout selection was lost");
		}
		g_ModSpawnDirty = true; // Play saves pending edits to the current named layout.
		modSpawnEditTestCheck(modSpawnEditPlay(), "Play starts the map with the config", "Play refused");
		g_CfgTest.phase = 2;
		g_CfgTest.tick = 0;
		g_CfgTest.waited = 0;
		return;
	}

	if (g_CfgTest.phase == 3) {
		s32 total = 0;

		if (!(modSpawnEditIsOn() && g_ModSpawnEditLoaded && g_Vars.stagenum == g_SpawnEditTestStage)) {
			if (++g_CfgTest.waited > TEST_LOADMAX) {
				modSpawnEditTestCheck(false, "the editor reopens", "the editor did not open in time");
				modSpawnEditTestFinish();
			}

			return;
		}

		if (++g_CfgTest.tick < 20) {
			return;
		}

		for (k = 0; k < MODSPAWN_NUMKINDS; k++) {
			total += g_CfgTest.count[k];
		}

		snprintf(detail, sizeof(detail), "config \"%s\", %d spawns, expected %s with %d", g_ModSpawnConfigName,
				g_ModNumSpawns, CFGTEST_NAME, total);
		modSpawnEditTestCheck(strcmp(g_ModSpawnConfigName, CFGTEST_NAME) == 0 && g_ModNumSpawns == total,
				"the editor opens the setting chosen in the menu", detail);
		modSpawnEditTestCheck(modSpawnConfigChosenName(g_SpawnEditTestStage) != NULL, "the choice is still made", "no choice");
		modSpawnEditTestCheck(!g_CfgTest.cutscenePlayed, "editor and Play open without a cutscene", "a scene took the camera");
		modSpawnEditConfigPath(path, sizeof(path), g_SpawnEditTestStage, CFGTEST_NAME);
		remove(fsFullPath(path));
		modSpawnConfigOff(g_SpawnEditTestStage);
		// The end screen can retry the same map without a mission picker.
		mainChangeToStage(g_SpawnEditTestStage);
		modSpawnEditTestCheck(!modSpawnEditIsOn() && !modRandomIsArmed() && !modRunIsArenaMission(),
				"leaving the editor clears its mission modes before retry", "an editor mission mode is still active");
		g_CfgTest.phase = 4;
		return;
	}

	if (g_CfgTest.phase == 4) {
		modSpawnEditTestCheck(g_Vars.stagenum == g_SpawnEditTestStage && !modSpawnEditIsOn() && !modRandomIsOn(),
				"the retried map opens as a normal mission", "the retry loaded with the wrong mode");
		modSpawnEditTestCheck(!g_PlayerInvincible, "retry does not inherit editor invincibility", "the player is still protected");
		modSpawnEditTestFinish();
		return;
	}

	// phase 2: the map is being dealt with the config
	if (modSpawnConfigName() == NULL) {
		if (++g_CfgTest.waited > TEST_LOADMAX) {
			modSpawnEditTestCheck(false, "the roll reads the config", "the config was not in force after the map loaded");
			modSpawnEditTestFinish();
		}

		return;
	}

	if (++g_CfgTest.tick < 60) {
		return;
	}

	if (g_CfgTest.count[MODSPAWN_ESCAPE] > 0 && g_CfgTest.tick < TEST_LOADMAX
			&& (g_InCutscene || g_Vars.tickmode != TICKMODE_NORMAL || g_Vars.lvframe60 < TICKS(60))) {
		return;
	}

	modSpawnEditTestCheck(!g_PlayerInvincible, "Play does not inherit editor invincibility", "the player is still protected");

	for (k = 0; k < MODSPAWN_NUMKINDS; k++) {
		snprintf(detail, sizeof(detail), "kind %d: saved %d, in force %d", k, g_CfgTest.count[k], modSpawnConfigCount(k));
		modSpawnEditTestCheck(modSpawnConfigCount(k) == g_CfgTest.count[k], "the config's spawns became pads", detail);
	}

	{
		struct pad pad;
		bool padsok = true;

		for (k = 0; k < MODSPAWN_NUMKINDS; k++) {
			for (i = 0; i < g_CfgTest.count[k]; i++) {
				padUnpack(modSpawnConfigPad(k, i), PADFIELD_POS, &pad);

				if (fabsf(pad.pos.x - g_CfgTest.pos[k][i].x) > 0.5f || fabsf(pad.pos.z - g_CfgTest.pos[k][i].z) > 0.5f) {
					padsok = false;
				}
			}
		}

		modSpawnEditTestCheck(padsok, "the pads are where the config's spawns are", "a pad is elsewhere");
	}

	if (g_CfgTest.count[MODSPAWN_ESCAPE] > 0) {
		f32 d = modSpawnConfigTestDist(&player->prop->pos, &g_CfgTest.pos[MODSPAWN_ESCAPE][0]);

		snprintf(detail, sizeof(detail), "the player is %.0f from the Escape/Spawn", d);
		modSpawnEditTestCheck(d < 250.0f, "the player starts on the config's Escape/Spawn", detail);
	}

	{
		s32 near = 0;
		s32 numhunters = 0;
		struct waypoint *hunters = modSpawnConfigHunters(&numhunters);

		for (i = 0; i < chrsGetNumSlots(); i++) {
			struct chrdata *chr = &g_ChrSlots[i];

			if (chr->chrnum < 0 || chr->prop == NULL || chr->model == NULL || chr->prop == player->prop) {
				continue;
			}

			for (k = MODSPAWN_GUARD; k <= MODSPAWN_OBJECTIVE; k += 2) {
				s32 j;

				for (j = 0; j < g_CfgTest.count[k]; j++) {
					if (modSpawnConfigTestDist(&chr->prop->pos, &g_CfgTest.pos[k][j]) < 200.0f) {
						near++;
					}
				}
			}
		}

		if (g_CfgTest.count[MODSPAWN_GUARANTEED] > 0) {
			s32 here = 0;

			for (i = 0; i < chrsGetNumSlots(); i++) {
				struct chrdata *chr = &g_ChrSlots[i];

				if (chr->chrnum >= 0 && chr->prop && chr->model && chr->prop != player->prop
						&& modSpawnConfigTestDist(&chr->prop->pos, &g_CfgTest.pos[MODSPAWN_GUARANTEED][0]) < 150.0f) {
					here++;
				}
			}

			snprintf(detail, sizeof(detail), "%d characters at the Guaranteed Guard spawn", here);
			modSpawnEditTestCheck(here > 0, "a Guaranteed Guard stands at its spawn when the map loads", detail);
		}

		snprintf(detail, sizeof(detail), "%d characters stand near the config's Guard/Objective spawns", near);
		modSpawnEditTestCheck(near > 0, "a guard stands on one of the config's spawns", detail);
		snprintf(detail, sizeof(detail), "%d hunters in the pool, %d saved", numhunters, g_CfgTest.count[MODSPAWN_HUNTER]);
		modSpawnEditTestCheck(hunters != NULL && numhunters == g_CfgTest.count[MODSPAWN_HUNTER],
				"reinforcements draw from the config's Hunter spawns", detail);
	}

	{
		s32 spots = 0;
		s32 bad = 0;

		for (i = 0; i < 60; i++) {
			s32 pad = modRandomTestEventSpot(i & 1);

			if (pad >= 0) {
				spots++;

				if (!modSpawnConfigIsPadOfKind(MODSPAWN_HUNTER, pad)) {
					bad++;
				}
			}
		}

		snprintf(detail, sizeof(detail), "%d of %d event spots were not Hunter spawns", bad, spots);
		modSpawnEditTestCheck(bad == 0, "event waves arrive at the config's Hunter spawns", detail);
		modSpawnEditTestCheck(spots > 0 || g_CfgTest.count[MODSPAWN_HUNTER] == 0, "event waves find a Hunter spawn to arrive at",
				"no event spot was found in 60 tries");
		sysLogPrintf(LOG_NOTE, "SELFTEST INFO event spots found %d of 60", spots);
	}

	if (g_CfgTest.count[MODSPAWN_ESCAPE] > 0) {
		s32 land = modRunTestLandingPad();

		snprintf(detail, sizeof(detail), "landing pad %d is not one of the config's Escape/Spawn spawns", land);
		modSpawnEditTestCheck(land >= 0 && modSpawnConfigIsPadOfKind(MODSPAWN_ESCAPE, land),
				"an Endless Run lands on the config's Escape/Spawn", detail);
	}

	if (g_CfgTest.count[MODSPAWN_HUNTER] > 0) {
		s32 climax = modRandomTestClimaxPad();

		modSpawnEditTestCheck(climax >= 0 && modSpawnConfigIsPadOfKind(MODSPAWN_HUNTER, climax),
				"climax waves arrive at the config's Hunter spawns", "a climax guard did not get a Hunter spawn");
	}

	if (g_CfgTest.count[MODSPAWN_GUARANTEED] + g_CfgTest.count[MODSPAWN_GUARD] > 0) {
		s32 returned = modRandomTestReturningGuard();

		if (returned == -2) {
			sysLogPrintf(LOG_NOTE, "SELFTEST INFO every Guaranteed/Guard spawn was taken, so no returning guard could be placed");
		}

		modSpawnEditTestCheck(returned >= 0 || returned == -2, "a returning guard comes back at a Guaranteed Guard or Guard spawn",
				"it did not stand at one, and the places were not all taken");
	}

	modSpawnEditTestCheck(modSpawnEditTestStaleProps() == 0, "prop list sound with a config", "stale props");

	// and the editor opens the chosen config: by way of the end of the Play, as in the game
	modSpawnEditTestCheck(modSpawnEditPlayEnded(), "the end of a Play opens the editor again", "it did not");
	g_CfgTest.phase = 3;
	g_CfgTest.tick = 0;
	g_CfgTest.waited = 0;
}

/** Called every tick from modRunTick(). */
void modSpawnEditTestTick(void)
{
	if (g_SpawnConfigTest) {
		if (g_SpawnEditTestStage >= 0) {
			modSpawnConfigTestTick();
		}

		return;
	}

	struct player *player = g_Vars.currentplayer;
	char detail[96];
	s32 t;
	s32 i;

	if (g_SpawnEditTestStage < 0 || player == NULL || player != g_Vars.bond) {
		return;
	}

	// open the editor once the boot level is running
	if (!g_Test.started) {
		if (g_Vars.lvframenum > 60 && !modSpawnEditIsOn()) {
			g_Test.started = true;
			sysLogPrintf(LOG_NOTE, "SELFTEST START stage 0x%02x", g_SpawnEditTestStage);
			modSpawnEditStart(g_SpawnEditTestStage);
		}

		return;
	}

	if (!g_Test.loaded) {
		if (modSpawnEditIsOn() && g_ModSpawnEditLoaded && g_Vars.stagenum == g_SpawnEditTestStage) {
			g_Test.loaded = true;
			g_Test.tick = 0;
		} else if (++g_Test.waited > TEST_LOADMAX) {
			modSpawnEditTestCheck(false, "editor opens", "the editor did not open in time");
			modSpawnEditTestFinish();
		}

		return;
	}

	if (!modSpawnEditIsOn() || lvIsPaused()) {
		return;
	}

	t = ++g_Test.tick;

	// every tick: the lists are sound and the camera is in the map
	if (!g_Test.staleReported && modSpawnEditTestStaleProps() > 0) {
		g_Test.staleReported = true;
		snprintf(detail, sizeof(detail), "tick %d: a prop in the active list has lost its character/object", t);
		modSpawnEditTestCheck(false, "prop list sound", detail);
	}

	if (!g_Test.lastteleported && !g_Test.roomReported && player->prop->rooms[0] < 0) {
		g_Test.roomReported = true;
		snprintf(detail, sizeof(detail), "tick %d: the camera has no room", t);
		modSpawnEditTestCheck(false, "camera stays in the map", detail);
	}

	g_Test.lastteleported = false;

	if (g_ModSpawnGhostLaptop || g_ModSpawnGhostChr[0] || g_ModSpawnGhostChr[1] || g_ModSpawnGhostChr[2] || g_ModSpawnGhostChr[4]) {
		g_Test.ghostseen = true;
	}

	if (t == 1) {
		snprintf(detail, sizeof(detail), "cap is %d", modChrOnscreenCap());
		modSpawnEditTestCheck(modChrOnscreenCap() >= 800, "the editor draws more than stock's 30 characters a frame", detail);
		modSpawnEditTestCheck(modSpawnEditChrSlots() == MODSPAWN_EDIT_CHRSLOTS_MAX && MODSPAWN_MODELS == 800,
				"the editor's character pool is always 800", "pool is not 800");
		modSpawnEditTestCheck(g_ModNumSpawns > 0, "default spawns exist", "no spawns loaded");
		modSpawnEditTestCheck(g_ModSpawnCounts[MODSPAWN_ESCAPE] >= 1, "default Escape/Spawn exists", "no Escape/Spawn");
		modSpawnEditTestCheck(g_ObjectiveLastIndex < 0, "no objectives in the editor", "the stage's objectives are still there");
	}

	if (t == 5) {
		s32 kind;
		s32 placed = 0;
		s32 before = g_ModNumSpawns;
		static const f32 offsets[] = {0, 130, -130, 260, -260, 400, -400};

		for (kind = 0; kind < MODSPAWN_NUMKINDS; kind++) {
			for (i = 0; i < (s32)(sizeof(offsets) / sizeof(offsets[0])); i++) {
				struct coord at = player->prop->pos;

				at.x += offsets[i];
				at.z += (kind & 1) ? 70.0f : -70.0f;

				if (modSpawnEditTestAddAt(kind, at)) {
					placed++;

					if (kind == MODSPAWN_ESCAPE) {
						g_Test.escapeid = g_ModSpawns[g_ModNumSpawns - 1].id;
					}

					break;
				}
			}
		}

		snprintf(detail, sizeof(detail), "placed %d of %d", placed, MODSPAWN_NUMKINDS);
		modSpawnEditTestCheck(placed == MODSPAWN_NUMKINDS, "can place every kind near the start", detail);
		snprintf(detail, sizeof(detail), "had %d now %d", before, g_ModNumSpawns);
		modSpawnEditTestCheck(g_ModNumSpawns == before + placed, "placing adds spawns", detail);
	}

	if (t == 10 && g_ModNumSpawns > 0) {
		s32 n = g_ModNumSpawns;
		u8 kind = g_ModSpawns[n - 1].kind;

		g_ModSpawnAimed = n - 1;
		modSpawnEditDelete();
		snprintf(detail, sizeof(detail), "had %d now %d", n, g_ModNumSpawns);
		modSpawnEditTestCheck(g_ModNumSpawns == n - 1, "delete removes a spawn", detail);
		modSpawnEditUndo();
		snprintf(detail, sizeof(detail), "had %d now %d", n, g_ModNumSpawns);
		modSpawnEditTestCheck(g_ModNumSpawns == n && g_ModSpawns[n - 1].kind == kind, "undo puts it back", detail);
		g_ModSpawnAimed = -1;
	}

	if (t == 12) {
		// Esc steps back in the editor's menu and must not pause the game
		g_ModSpawnMenu = MODSPAWN_MENU_COMMANDS;
		g_ModSpawnMenuSel = MODSPAWN_OBJECTIVE;
		modSpawnEditTestCheck(modSpawnEditEatsEscape(), "Esc is kept from pausing while the menu is open", "Esc would pause");
		modSpawnEditTestCheck(modSpawnEditMenuOpen(), "the player is held still while the menu is open", "menu not reported open");
		modSpawnEditMenuTick(true);
		modSpawnEditTestCheck(g_ModSpawnMenu == MODSPAWN_MENU_MAIN && g_ModSpawnMenuSel == MODSPAWN_MENU_COMMANDS_ITEM,
				"Esc returns from Commands to the selected main menu item", "wrong menu or selection");
		modSpawnEditMenuTick(true);
		modSpawnEditTestCheck(modSpawnEditEatsEscape(), "Esc closing the menu cannot pause on the same frame", "Esc would pause");
		g_ModSpawnEscEatenFrame = -10;
		modSpawnEditTestCheck(!modSpawnEditMenuOpen(), "the player moves again when the menu is closed", "menu still reported open");
		modSpawnEditTestCheck(!modSpawnEditEatsEscape(), "Esc pauses as usual with the menu closed", "Esc is still eaten");
	}

	if (t == 14) {
		f32 health = playerGetHealthFrac();
		struct coord impact = {0, 0, 1};

		chrDamageByMisc(player->prop->chr, 2.0f, &impact, NULL, NULL);
		modSpawnEditTestCheck(g_PlayerInvincible && playerGetHealthFrac() == health,
				"editor player ignores damage", "damage changed the player's health");
		playerDie(true);
		modSpawnEditTestCheck(!player->isdead, "editor player survives forced death", "the player died in the editor");
	}

	if (t == 13) {
		s32 count = g_ModNumSpawns;
		s32 counts[MODSPAWN_NUMKINDS];
		memcpy(counts, g_ModSpawnCounts, sizeof(counts));

		for (s32 kind = 0; kind < MODSPAWN_NUMKINDS; kind++) {
			bool onlySelected;

			modSpawnEditClearKind(kind);
			onlySelected = g_ModNumSpawns == count - counts[kind] && g_ModSpawnCounts[kind] == 0;
			for (s32 other = 0; other < MODSPAWN_NUMKINDS; other++) {
				if (other != kind && g_ModSpawnCounts[other] != counts[other]) {
					onlySelected = false;
				}
			}
			modSpawnEditTestCheck(onlySelected, "clear removes only the selected spawn type", g_ModSpawnNames[kind]);

			// Clearing twice should leave the successful clear available to undo.
			modSpawnEditClearKind(kind);
			modSpawnEditUndo();
			modSpawnEditTestCheck(g_ModNumSpawns == count
					&& memcmp(g_ModSpawnCounts, counts, sizeof(counts)) == 0
					&& memcmp(g_ModSpawns, g_ModSpawnBeforeClear, count * sizeof(g_ModSpawns[0])) == 0,
					"undo restores the cleared spawns in order", g_ModSpawnNames[kind]);
		}
	}

	if (t == TEST_SETTLE) {
		s32 models = 0;
		struct coord lp;
		bool haslaptop = false;

		for (i = 0; i < MODSPAWN_MODELS; i++) {
			if (g_ModSpawnModels[i].id && g_ModSpawnModels[i].prop && g_ModSpawnModels[i].prop->chr) {
				models++;
			}
		}

		for (i = 0; i < g_ModNumSpawns; i++) {
			if (g_ModSpawns[i].kind == MODSPAWN_ESCAPE && modSpawnEditLaptopPos(g_ModSpawns[i].id, &lp)) {
				haslaptop = true;
			}
		}

		snprintf(detail, sizeof(detail), "%d character models", models);
		modSpawnEditTestCheck(models > 0, "characters stand at the spawns", detail);
		modSpawnEditTestCheck(g_Vars.bondcollisions == false || g_ModSpawnFlying, "walking passes through characters",
				"the player still collides with characters");
		modSpawnEditTestCheck(haslaptop, "a laptop stands at an Escape/Spawn", "no laptop shown");
		modSpawnEditTestCheck(g_Test.escapeid != 0 && modSpawnEditLaptopPos(g_Test.escapeid, &lp),
				"a newly placed Escape/Spawn gets its laptop", "the placed Escape/Spawn has no laptop");
		{
			// a character standing on a spot must not push the aim off it
			bool ok = false;
			bool tested = false;

			for (i = 0; i < MODSPAWN_MODELS && !tested; i++) {
				struct modspawnmodel *m = &g_ModSpawnModels[i];
				s32 idx = m->id && m->prop && m->prop->chr ? modSpawnEditFindId(m->id) : -1;

				if (idx >= 0 && g_ModSpawns[idx].kind != MODSPAWN_ESCAPE) {
					struct coord hit = g_ModSpawns[idx].pos;
					struct coord look = {0, 0, 0};
					struct coord outpos;
					RoomNum outroom;
					const char *why;

					struct coord other;
					bool r1;
					bool r2;

					// the answer must be the same whether or not the character is solid
					tested = true;
					propSetPerimEnabled(m->prop, true);
					r1 = modSpawnEditSnap(&hit, &look, &outpos, &outroom, &why);
					propSetPerimEnabled(m->prop, false);
					r2 = modSpawnEditSnap(&hit, &look, &other, &outroom, &why);
					ok = r1 == r2 && (!r1 || (fabsf(outpos.x - other.x) < 1.0f && fabsf(outpos.z - other.z) < 1.0f));
				}
			}

			modSpawnEditTestCheck(!tested || ok, "a character on a spot does not push the aim away (no doubled preview)",
					"the aim moved off a spot a character stands on");
		}

		g_Test.baseline = g_ModNumSpawns;
	}

	if (t > TEST_SETTLE && t <= TEST_SETTLE + TEST_CHURN) {
		s32 c = t - TEST_SETTLE;

		// fly about the map: characters and laptops are made and taken down
		if (c % 12 == 0 && g_ModNumSpawns > 0) {
			g_Test.teleportindex = (g_Test.teleportindex + 11) % g_ModNumSpawns;
			modSpawnEditTestTeleport(&g_ModSpawns[g_Test.teleportindex].pos);
		}

		// the preview's kind
		if (c % 20 == 0) {
			modSpawnEditSetKind((g_ModSpawnPlaceKind + 1) % MODSPAWN_NUMKINDS);
		}

		// walking and flying
		if (c % 90 == 0) {
			g_ModSpawnFlying = !g_ModSpawnFlying;
		}

		// a click: placed where the camera looks, if it can be
		if (c % 37 == 0) {
			s32 before = g_ModNumSpawns;

			modSpawnEditPlace();
			g_Test.places++;

			if (g_ModNumSpawns > before) {
				g_Test.placeok++;
			}
		}
	}

	if (t == TEST_SETTLE + TEST_CHURN + 1) {
		g_ModSpawnFlying = false;
		snprintf(detail, sizeof(detail), "%d markers vanished on their own (see 'marker ... lost' in the log)",
				g_ModSpawnLostCount);
		modSpawnEditTestCheck(g_ModSpawnLostCount == 0, "markers never vanish by themselves while moving around", detail);
	}

	if (t == TEST_SETTLE + TEST_CHURN + 30) {
		// configs: save, change, load; revert; a file for another map is refused
		struct coord firstpos;
		s32 n = g_ModNumSpawns;
		s32 counts[MODSPAWN_NUMKINDS];
		bool same = true;
		bool saved;
		FILE *f;
		char path[96];

		memcpy(counts, g_ModSpawnCounts, sizeof(counts));
		firstpos = g_ModSpawns[0].pos;
		saved = modSpawnEditSaveConfig("selftest cfg");
		modSpawnEditTestCheck(saved, "a config saves", "could not write the file");
		modSpawnEditRemove(n - 1);
		modSpawnEditRemove(0);
		modSpawnEditTestCheck(modSpawnEditLoadConfig("selftest cfg"), "a config loads", "load returned false");
		snprintf(detail, sizeof(detail), "had %d, loaded %d", n, g_ModNumSpawns);
		modSpawnEditTestCheck(g_ModNumSpawns == n, "load restores every spawn", detail);

		for (i = 0; i < MODSPAWN_NUMKINDS; i++) {
			if (counts[i] != g_ModSpawnCounts[i]) {
				same = false;
			}
		}

		modSpawnEditTestCheck(same, "load restores the counts of each kind", "kind counts differ");
		modSpawnEditTestCheck(g_ModNumSpawns > 0 && fabsf(g_ModSpawns[0].pos.x - firstpos.x) < 0.2f
				&& fabsf(g_ModSpawns[0].pos.z - firstpos.z) < 0.2f, "load restores positions", "first spawn moved");

		// a file for another stage
		modSpawnEditConfigPath(path, sizeof(path), g_ModSpawnEditStage, "selftest other");
		f = fsFileOpenWrite(path);

		if (f) {
			fprintf(f, "PerfectRandomSpawns 1\nstage 0x%02x\nname other\nguard 0 0 0 0\n", (g_ModSpawnEditStage + 1) & 0xff);
			fclose(f);
		}

		n = g_ModNumSpawns;
		modSpawnEditTestCheck(!modSpawnEditLoadConfig("selftest other") && g_ModNumSpawns == n,
				"a config for another map is refused", "it loaded");
		modSpawnEditTestCheck(!modSpawnEditSaveConfig("Default"), "Default cannot be overwritten", "it saved");
		modSpawnEditRevert();
		snprintf(detail, sizeof(detail), "default %d, now %d", g_ModNumDefaults, g_ModNumSpawns);
		modSpawnEditTestCheck(g_ModNumSpawns == g_ModNumDefaults, "revert restores the default spawns", detail);

		modSpawnEditConfigPath(path, sizeof(path), g_ModSpawnEditStage, "selftest cfg");
		remove(fsFullPath(path));
		modSpawnEditConfigPath(path, sizeof(path), g_ModSpawnEditStage, "selftest other");
		remove(fsFullPath(path));
	}

	if (t == TEST_SETTLE + TEST_CHURN + 60) {
		sysLogPrintf(LOG_NOTE, "SELFTEST INFO teleports %d, click placements %d of %d, preview seen %d",
				g_Test.teleports, g_Test.placeok, g_Test.places, g_Test.ghostseen);
		modSpawnEditTestCheck(g_Test.teleports > 5, "the test moved about the map", "fewer than 6 teleports");
		modSpawnEditTestCheck(modSpawnEditTestStaleProps() == 0, "prop list sound at the end", "stale props");
		modSpawnEditTestCheck(player->prop->rooms[0] >= 0, "camera in the map at the end", "no room");
		modSpawnEditTestFinish();
	}
}

#endif // !PLATFORM_N64
