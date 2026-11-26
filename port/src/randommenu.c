#include <ultra64.h>
#include "constants.h"
#include "prcompat.h"
#include "game/game_006900.h"
#include "game/lang.h"
#include "game/challenge.h"
#include "game/stagetable.h"
#include "lib/rng.h"
#include "game/mainmenu.h"
#include "game/menu.h"
#include "game/modcareer.h"
#include "game/modghost.h"
#include "game/modoptions.h"
#include "game/modrandom.h"
#include "game/modrun.h"
#include "game/modspawnedit.h"
#include "game/mplayer/setup.h"
#include "bss.h"
#include "lang.h"
#include "lib/vars.h"
#include "data.h"
#include "types.h"
#include "langpack.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "modloader.h"
#include "gexplus.h"
#include "gechranims.h"
#include "game/lv.h"
#include "game/title.h"
#include "game/pdmode.h"
#include "lib/main.h"
#include "lib/vi.h"
#include "gelevels.h"
#include "levelgenstage.h"
#include "game/modlevelgen.h"
#ifdef PD_PHYSICS
#include "guardvoice.h"
#endif

static s32 g_PerfectRandomGeMission;
// MOD: GoldenEye levels (gelevels.c): GoldenEye's twenty missions, then its
// multiplayer maps, each a Perfect Random mission the way a Combat Map is
static s32 menuhandlerRandomGeMapStage(s32 option)
{
	return gelevelsStageAt(option);
}

static MenuItemHandlerResult menuhandlerRandomGeMap(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_GETOPTIONCOUNT) data->dropdown.value = gelevelsCount() > 0 ? gelevelsCount() : 1;
	else if (operation == MENUOP_GETOPTIONTEXT) {
		const char *name = gelevelsName(menuhandlerRandomGeMapStage(data->dropdown.value));
		return (intptr_t)(name ? name : "(run tools/geconvert first)");
	} else if (operation == MENUOP_SET) g_PerfectRandomGeMission = data->dropdown.value;
	else if (operation == MENUOP_GETSELECTEDINDEX) data->dropdown.value = g_PerfectRandomGeMission;
	return 0;
}

static MenuItemHandlerResult menuhandlerRandomGeStart(s32 operation, struct menuitem *item, union handlerdata *data)
{
	s32 stage = menuhandlerRandomGeMapStage(g_PerfectRandomGeMission);
	if (operation == MENUOP_CHECKDISABLED) return !stage;
	if (operation == MENUOP_SET && stage) {
		modGhostDisarmTrial();
		modRandomDisarmMission();
		g_ModOptions.randomendless = 0;
		g_ModOptions.randomversion = MODRANDOM_VERSION_DEFAULT;
		modRunStartArenaMission(stage);
	}
	return 0;
}

/**
 * The Randomizer - the mode's own corner of the main menu.
 *
 * Everything here was reachable before through Dab's Mod Options, which is
 * fine for a setting and wrong for a mode, and the Randomizer had grown into
 * two ways of playing rather than a preference about how a mission behaves:
 * a mission dealt again from its own pieces, and a run that keeps dealing
 * rooms across every map in the game until the player dies. So it gets a door
 * next to the one marked Solo Missions, and Ghost Trials is the pattern for
 * what is behind it.
 *
 * Random Mission does not clone the mission select, for the reason
 * ghostmenu.c gives: it arms the roll and pushes the same dialog the stock
 * Solo Missions item pushes, so the mission list, the difficulty, the
 * briefing and every unlock rule behind them are the ones the game already
 * has.
 *
 * Start Run pushes no dialog at all. A run picks its own map, so there is
 * nothing to choose on the way in and a mission select would be a list of
 * things the mode is going to ignore.
 */

static char g_RandomRowText[96];

// MOD: level generator (levelgenstage.c): a generated level as a Perfect Random mission
static MenuItemHandlerResult menuhandlerLevelGenStart(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) return !levelgenIsStage(LEVELGEN_STAGENUM);
	if (operation == MENUOP_SET) {
		const u32 seed = levelgenNewSeed();
		modGhostDisarmTrial();
		modRandomDisarmMission();
		g_ModOptions.randomendless = 0;
		g_ModOptions.randomversion = MODRANDOM_VERSION_DEFAULT;
		// the layout picked in this menu (Random: the seed picks), for this
		// level only: the generator's choice is global and the Gauntlet's
		// levels go on choosing their own
		modLevelGenForceTheme(g_LevelGenLayoutSetting, -1);
		levelgenStart(seed, g_LevelGenSizeSetting);
		modLevelGenForceTheme(-1, -1);
	}
	return 0;
}

// The layout of a generated level from this menu: Random, or one of the
// generator's layouts (modlevelgen.c), each with its own look. The market town
// is left out (removed 2026-11-19): option k is the k-th layout but for it.
static bool levelGenMenuHidden(s32 layout)
{
	return layout == LG_LAYOUT_PLAZA || layout == LG_LAYOUT_PIPEWORKS;   // (removed, 2026-11-19)
}

// option 1.. = the layouts in order but the removed ones, then the station
static s32 levelGenMenuLayout(s32 option)
{
	s32 layout, k = 0;

	for (layout = 0; layout < LG_LAYOUT_COUNT; layout++) {
		if (!levelGenMenuHidden(layout) && ++k == option) {
			return layout;
		}
	}
	return LG_LAYOUT_STATION;
}

static MenuItemHandlerResult menuhandlerLevelGenLayout(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !levelgenIsStage(LEVELGEN_STAGENUM);
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = LG_LAYOUT_COUNT;   // Random, the layouts but the two removed, the station
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)(data->dropdown.value == 0 ? "Random" : modLevelGenLayoutName(levelGenMenuLayout((s32)data->dropdown.value)));
	case MENUOP_SET:
		g_LevelGenLayoutSetting = data->dropdown.value == 0 ? -1 : levelGenMenuLayout((s32)data->dropdown.value);
		break;
	case MENUOP_GETSELECTEDINDEX: {
		s32 k;

		data->dropdown.value = 0;
		for (k = 1; k < LG_LAYOUT_COUNT; k++) {
			if (levelGenMenuLayout(k) == g_LevelGenLayoutSetting) {
				data->dropdown.value = k;
			}
		}
		break;
	}
	}

	return 0;
}

// The size of a generated level: Single Mission and the Gauntlet both use it
static MenuItemHandlerResult menuhandlerLevelGenSize(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !levelgenIsStage(LEVELGEN_STAGENUM);
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = 3;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)levelgenSizeName((s32)data->dropdown.value);
	case MENUOP_SET:
		g_LevelGenSizeSetting = (s32)data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_LevelGenSizeSetting;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerArenaMissionStart(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) return g_MpSetup.stagenum == STAGE_MP_RANDOM;
	if (operation == MENUOP_SET) {
		modGhostDisarmTrial();
		modRandomDisarmMission();
		g_ModOptions.randomendless = 0;
		g_ModOptions.randomversion = MODRANDOM_VERSION_DEFAULT;
		modRunStartArenaMission(g_MpSetup.stagenum);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCombatMissionDifficulty(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *names[] = {"Agent", "Special Agent", "Perfect Agent"};
	if (operation == MENUOP_GETOPTIONCOUNT) data->dropdown.value = 3;
	else if (operation == MENUOP_GETOPTIONTEXT) return (intptr_t)names[data->dropdown.value];
	else if (operation == MENUOP_GETSELECTEDINDEX) data->dropdown.value = modGetRunDifficulty() - DIFF_A;
	else if (operation == MENUOP_SET) g_ModOptions.rundifficulty = DIFF_A + data->dropdown.value;
	return 0;
}

static struct menuitem g_ArenaMissionMenuItems[] = {
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_DROPDOWN_ALLWIDTHS,
		(uintptr_t)"Map",
		0,
		mpArenaMenuHandler,
	},
	{MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Difficulty", 0, menuhandlerCombatMissionDifficulty},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Start Mission",
		0,
		menuhandlerArenaMissionStart,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_PerfectRandomArenaMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Perfect Rogue: Combat Maps",
	g_ArenaMissionMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

// Generated maps start as Perfect Random missions, rather than as generic
// Combat Simulator arenas, so they always have the mode's objectives, waves
// and rewards.
static s32 g_PerfectRandomGeneratedMission;

static s32 menuhandlerGeneratedMissionStage(s32 option)
{
	for (s32 i = 0; i < mpGetNumStages(); i++) {
		const s32 stage = g_MpArenas[i].stagenum;
		const char *name = modloaderGetStageMapName(stage);

		if (name && strncmp(name, "PR ", 3) == 0 && option-- == 0) {
			return stage;
		}
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerGeneratedMissionMap(s32 operation, struct menuitem *item, union handlerdata *data)
{
	s32 count = 0;
	for (s32 i = 0; i < mpGetNumStages(); i++) {
		const char *name = modloaderGetStageMapName(g_MpArenas[i].stagenum);
		if (name && strncmp(name, "PR ", 3) == 0) count++;
	}
	if (operation == MENUOP_GETOPTIONCOUNT) data->dropdown.value = count;
	else if (operation == MENUOP_GETOPTIONTEXT) {
		const char *name = modloaderGetStageMapName(menuhandlerGeneratedMissionStage(data->dropdown.value));
		return (intptr_t)(name ? name : "Generated mission");
	} else if (operation == MENUOP_SET) g_PerfectRandomGeneratedMission = data->dropdown.value;
	else if (operation == MENUOP_GETSELECTEDINDEX) data->dropdown.value = g_PerfectRandomGeneratedMission;
	return 0;
}

static MenuItemHandlerResult menuhandlerGeneratedMissionStart(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const s32 stage = menuhandlerGeneratedMissionStage(g_PerfectRandomGeneratedMission);

	if (operation == MENUOP_CHECKDISABLED) return !stage;
	if (operation == MENUOP_SET && stage) {
		modRunStop();
		modGhostDisarmTrial();
		modRandomArmMission();
		g_ModOptions.randomendless = 0;
		g_ModOptions.randomversion = MODRANDOM_VERSION_DEFAULT;
		gexPlusSetPdGuns(1);
		gexPlusSetPdEnemies(1);
		geChrAnimsSetPdDeaths(1);
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
	}

	return 0;
}

/**
 * Gauntlet: three Perfect Random missions in a row, each with tougher
 * enemies and more reinforcements than the last (modcareer.c). Like Generated
 * Missions it picks its own maps, so it pushes no dialog.
 */
static MenuItemHandlerResult menuhandlerGauntletStart(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return modCareerPoolNumPicked() == 0; // no map picked
	}

	if (operation == MENUOP_SET) {
		modRunStop();
		modGhostDisarmTrial();
		gexPlusSetPdGuns(1);
		gexPlusSetPdEnemies(1);
		geChrAnimsSetPdDeaths(1);
		modCareerStart();
	}

	return 0;
}

static char *menutextGauntletStatus(struct menuitem *item)
{
	return (char *)modCareerGetStatus();
}

static char *menutextGauntletObjectives(struct menuitem *item)
{
	return "Kill objectives rise from 2 to 5\n";
}

static MenuItemHandlerResult menuhandlerGauntletLength(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char text[32];
	const s32 steps = (MODCAREER_MAXMISSIONS - MODCAREER_MINMISSIONS) / 2 + 1;

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = steps + 1;
		break;
	case MENUOP_GETOPTIONTEXT:
		if (data->dropdown.value == steps) return (intptr_t)"Unlimited";
		snprintf(text, sizeof(text), "%d missions", MODCAREER_MINMISSIONS + 2 * (s32)data->dropdown.value);
		return (intptr_t)text;
	case MENUOP_SET:
		g_ModCareerLength = data->dropdown.value == steps ? MODCAREER_UNLIMITED
			: MODCAREER_MINMISSIONS + 2 * (s32)data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = modCareerLength() == MODCAREER_UNLIMITED ? steps
			: (modCareerLength() - MODCAREER_MINMISSIONS) / 2;
		break;
	}

	return 0;
}

/**
 * Which maps a gauntlet may deal: one scrolling list with a tick beside every
 * map, like the Combat Simulator's soundtrack picker. The first two rows are
 * Select All and Select None.
 */
#define GAUNTLET_LIST_EXTRA 2

static s32 gauntletMapListIndex(s32 row)
{
	const s32 count = modCareerPoolCount();
	for (s32 i = 0; i < count; i++) {
		if (modCareerPoolMapVisible(i) && row-- == 0) return i;
	}
	return -1;
}

static s32 gauntletMapListCount(void)
{
	const s32 count = modCareerPoolCount();
	s32 visible = 0;

	for (s32 i = 0; i < count; i++) {
		if (!modCareerPoolMapVisible(i)) continue;
		visible++;
	}
	return visible;
}

static MenuItemHandlerResult menuhandlerGauntletMapList(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char text[64];
	static const char *kinds[MODCAREER_NUMKINDS] = { "PD", "GE", "CS", "GN" };
	const s32 count = modCareerPoolCount();
	s32 i;

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->list.value = gauntletMapListCount() + GAUNTLET_LIST_EXTRA;
		break;
	case MENUOP_GETOPTIONTEXT:
		if (data->list.value == 0) {
			return (intptr_t)"Select All";
		}
		if (data->list.value == 1) {
			return (intptr_t)"Select None";
		}
		i = gauntletMapListIndex(data->list.value - GAUNTLET_LIST_EXTRA);
		if (i < 0) return (intptr_t)"";
		if (modCareerPoolKind(i) == MODCAREER_KIND_GEN) {
			snprintf(text, sizeof(text), "%s", modCareerPoolName(i));   // "(GEN) Lab"
		} else {
			snprintf(text, sizeof(text), "%s: %s", kinds[modCareerPoolKind(i)], modCareerPoolName(i));
		}
		return (intptr_t)text;
	case MENUOP_SET:
		if (data->list.value == 0 || data->list.value == 1) {
			for (i = 0; i < count; i++) {
				if (modCareerPoolMapVisible(i)) {
					modCareerPoolSetPicked(i, data->list.value == 0);
				}
			}
		} else if (data->list.unk04 == 0) {
			i = gauntletMapListIndex(data->list.value - GAUNTLET_LIST_EXTRA);
			if (i >= 0) modCareerPoolSetPicked(i, !modCareerPoolPicked(i));
		}
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->list.value = 0x000fffff; // nothing highlighted; ticks show the state
		break;
	case MENUOP_GETLISTITEMCHECKBOX:
		if (data->list.value >= GAUNTLET_LIST_EXTRA) {
			i = gauntletMapListIndex(data->list.value - GAUNTLET_LIST_EXTRA);
			data->list.unk04 = i >= 0 && modCareerPoolPicked(i);
		}
		break;
	}

	return 0;
}

static char *menutextGauntletMapCount(struct menuitem *item)
{
	static char text[64];

	snprintf(text, sizeof(text), "%d of %d maps in the gauntlet\n",
			modCareerPoolNumPicked(), gauntletMapListCount());

	return text;
}

static struct menuitem g_GauntletMissionItems[] = {
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextGauntletMapCount, 0, NULL },
	{ MENUITEMTYPE_LIST, 0, 0, 0x000000b4, 0x00000080, menuhandlerGauntletMapList },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_GauntletMissionDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Gauntlet Maps", g_GauntletMissionItems,
	NULL, MENUDIALOGFLAG_LITERAL_TEXT, NULL,
};

/**
 * Which special events a mission may have: a tick beside each, like the map list.
 */
static MenuItemHandlerResult menuhandlerGauntletEventList(s32 operation, struct menuitem *item, union handlerdata *data)
{
	s32 i;
	s32 kind;

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->list.value = MODCAREER_NUMEVENTS + GAUNTLET_LIST_EXTRA;
		break;
	case MENUOP_GETOPTIONTEXT:
		if (data->list.value == 0) {
			return (intptr_t)"Select All";
		}
		if (data->list.value == 1) {
			return (intptr_t)"Select None";
		}
		return (intptr_t)modCareerEventName(data->list.value - GAUNTLET_LIST_EXTRA);
	case MENUOP_SET:
		if (data->list.value == 0) {
			g_ModCareerEventsOff = 0;
		} else if (data->list.value == 1) {
			g_ModCareerEventsOff = MODCAREER_EVENT_SETTINGS_MASK;
		} else if (data->list.unk04 == 0) {
			kind = modCareerEventKind(data->list.value - GAUNTLET_LIST_EXTRA);
			g_ModCareerEventsOff ^= 1 << (kind - 1);
		}
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->list.value = 0x000fffff; // nothing highlighted; ticks show the state
		break;
	case MENUOP_GETLISTITEMCHECKBOX:
		if (data->list.value >= GAUNTLET_LIST_EXTRA) {
			i = data->list.value - GAUNTLET_LIST_EXTRA;
			kind = modCareerEventKind(i);
			data->list.unk04 = kind > 0 && (g_ModCareerEventsOff & (1 << (kind - 1))) == 0;
		}
		break;
	}

	return 0;
}

static char *menutextGauntletEventCount(struct menuitem *item)
{
	static char text[64];
	s32 i, on = 0;

	for (i = 0; i < MODCAREER_NUMEVENTS; i++) {
		on += modCareerEventAllowed(modCareerEventKind(i)) ? 1 : 0;
	}

	snprintf(text, sizeof(text), "%d of %d special events on\n", on, MODCAREER_NUMEVENTS);

	return text;
}

static struct menuitem g_GauntletEventItems[] = {
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextGauntletEventCount, 0, NULL },
	{ MENUITEMTYPE_LIST, 0, 0, 0x000000b4, 0x00000080, menuhandlerGauntletEventList },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_GauntletEventDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Special Events", g_GauntletEventItems,
	NULL, MENUDIALOGFLAG_LITERAL_TEXT, NULL,
};

/**
 * How much each stat grows with every mission: mission N has N times the step
 * added (the first mission is always the base game's). Param is the row.
 */
static const struct {
	s32 *value;
	s32 step;   // what one choice is worth
	s32 count;  // how many choices, the first being 0
	const char *fmt;
} g_GauntletSteps[] = {
	{ &g_ModCareerStepGuards,   1,  5, "+%d guards" },
	{ &g_ModCareerStepAccuracy, 10, 11, "+%d%%" },
	{ &g_ModCareerStepAkimbo,   5,  7, "+%d%%" },
	{ &g_ModCareerStepRespawn, 25,  5, "+%d%%" },
};

static MenuItemHandlerResult menuhandlerGauntletStep(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char text[32];
	const s32 row = item->param;

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = g_GauntletSteps[row].count;
		break;
	case MENUOP_GETOPTIONTEXT:
		snprintf(text, sizeof(text), g_GauntletSteps[row].fmt, (s32)data->dropdown.value * g_GauntletSteps[row].step);
		return (intptr_t)text;
	case MENUOP_SET:
		*g_GauntletSteps[row].value = data->dropdown.value * g_GauntletSteps[row].step;
		break;
	case MENUOP_GETSELECTEDINDEX: {
		s32 index = *g_GauntletSteps[row].value / g_GauntletSteps[row].step;
		data->dropdown.value = index >= g_GauntletSteps[row].count ? g_GauntletSteps[row].count - 1 : index;
		break;
	}
	}

	return 0;
}

static struct menuitem g_GauntletStepItems[] = {
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Hunters per wave", 0, menuhandlerGauntletStep },
	{ MENUITEMTYPE_DROPDOWN, 1, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Accuracy", 0, menuhandlerGauntletStep },
	{ MENUITEMTYPE_DROPDOWN, 2, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Akimbo Chance", 0, menuhandlerGauntletStep },
	{ MENUITEMTYPE_DROPDOWN, 3, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Respawn Chance", 0, menuhandlerGauntletStep },
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextGauntletObjectives, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_GauntletStepDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Increase Per Mission", g_GauntletStepItems,
	NULL, MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_SCROLLBAR, NULL,
};

static MenuItemHandlerResult menuhandlerGauntletPreset(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *const names[] = { "Agent", "Secret Agent", "Perfect Agent", "Custom" };
	if (operation == MENUOP_GETOPTIONCOUNT) data->dropdown.value = ARRAYCOUNT(names);
	else if (operation == MENUOP_GETOPTIONTEXT) return (intptr_t)names[data->dropdown.value];
	else if (operation == MENUOP_SET) g_ModCareerStartPreset = data->dropdown.value;
	else if (operation == MENUOP_GETSELECTEDINDEX) data->dropdown.value = g_ModCareerStartPreset < 0 ? 0
			: g_ModCareerStartPreset > 3 ? 3 : g_ModCareerStartPreset;
	return 0;
}

static const struct {
	s32 *value;
	const s32 *increase;
	s32 count;
} g_GauntletStartingPoints[] = {
	{ &g_ModCareerStartGuards, &g_ModCareerStepGuards, 11 },
	{ &g_ModCareerStartAccuracy, &g_ModCareerStepAccuracy, 11 },
	{ &g_ModCareerStartAkimbo, &g_ModCareerStepAkimbo, 11 },
	{ &g_ModCareerStartRespawn, NULL, 4 },
};

static MenuItemHandlerResult menuhandlerGauntletStartingPoint(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char text[24];
	const s32 row = item->param;
	if (operation == MENUOP_GETOPTIONCOUNT) data->dropdown.value = g_GauntletStartingPoints[row].count;
	else if (operation == MENUOP_GETOPTIONTEXT) {
		const s32 *increase = g_GauntletStartingPoints[row].increase;
		s32 value = (s32)data->dropdown.value * (increase ? *increase : 25);

		if (row == 0) {
			value += MODCAREER_BASE_WAVE_SIZE;
			if (value > MODRANDOM_MAX_WAVE) value = MODRANDOM_MAX_WAVE;
			snprintf(text, sizeof(text), "%d guards", value);
		} else {
			if (row == 1) value += 100;
			else {
				if (row == 3) value += MODCAREER_BASE_RESPAWN_CHANCE;
				if (value > 100) value = 100;
			}
			snprintf(text, sizeof(text), "%d%%", value);
		}

		return (intptr_t)text;
	} else if (operation == MENUOP_SET) {
		*g_GauntletStartingPoints[row].value = data->dropdown.value;
		g_ModCareerStartPreset = 3;
	} else if (operation == MENUOP_GETSELECTEDINDEX) {
		s32 value = *g_GauntletStartingPoints[row].value;
		data->dropdown.value = value < 0 ? 0 : value >= g_GauntletStartingPoints[row].count
				? g_GauntletStartingPoints[row].count - 1 : value;
	}
	return 0;
}

static struct menuitem g_GauntletStartPointItems[] = {
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Hunters per wave", 0, menuhandlerGauntletStartingPoint },
	{ MENUITEMTYPE_DROPDOWN, 1, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Accuracy", 0, menuhandlerGauntletStartingPoint },
	{ MENUITEMTYPE_DROPDOWN, 2, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Akimbo Chance", 0, menuhandlerGauntletStartingPoint },
	{ MENUITEMTYPE_DROPDOWN, 3, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Respawn Chance", 0, menuhandlerGauntletStartingPoint },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_GauntletStartPointDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Custom Baseline Difficulty", g_GauntletStartPointItems,
	NULL, MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_SCROLLBAR, NULL,
};

static struct menuitem g_GauntletStartingDifficultyItems[] = {
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Preset", 0, menuhandlerGauntletPreset },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Custom Baseline Difficulty", 0, (void *)&g_GauntletStartPointDialog },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Difficulty Increase", 0, (void *)&g_GauntletStepDialog },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_GauntletStartingDifficultyDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Gauntlet Difficulty", g_GauntletStartingDifficultyItems,
	NULL, MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_SCROLLBAR, NULL,
};

static char *menutextGauntletBest(struct menuitem *item)
{
	static char text[64];
	snprintf(text, sizeof(text), "High score: %d maps cleared\n", g_ModCareerBest);
	return text;
}

static struct menuitem g_GauntletItems[] = {
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Start Gauntlet", 0, menuhandlerGauntletStart },
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextGauntletStatus, 0, NULL },
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextGauntletBest, 0, NULL },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Length", 0, menuhandlerGauntletLength },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Difficulty", 0, menuhandlerCombatMissionDifficulty },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Maps", 0, (void *)&g_GauntletMissionDialog },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Gauntlet Difficulty", 0, (void *)&g_GauntletStartingDifficultyDialog },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_BIGFONT, L_OPTIONS_213, 0, NULL },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_GauntletMenuDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Perfect Rogue: Gauntlet", g_GauntletItems,
	NULL, MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_SCROLLBAR, NULL,
};

/**
 * The pick between gauntlet missions: a pop-up over the new mission, which
 * waits (paused) until a boon is chosen. Closing it any other way just asks
 * again (modCareerTick()).
 */
static MenuItemHandlerResult menuhandlerGauntletBoon(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		return item->param >= modCareerBoonCount(); // only the boons that were earned
	}

	if (operation == MENUOP_SET) {
		modCareerBoonTake(item->param);
	}

	return 0;
}

/** The achievement list of the mission just finished: hidden before the first mission has been played. */
static char g_GauntletAchText[3][80];

static MenuItemHandlerResult menuhandlerGauntletAch(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		return !modCareerAchievementsShown();
	}

	if (operation == MENUOP_GETCOLOUR) {
		const bool earned = g_GauntletAchText[item->param][0] == 'E';

		data->label.colour1 = data->label.colour2 = earned ? 0x60ff60ff : 0xff6060ff;
	}

	return 0;
}

#define ACHLINE(n) { MENUITEMTYPE_LABEL, n, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT | MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LABEL_CUSTOMCOLOUR, 0, 0, menuhandlerGauntletAch }
#define BOONSLOT(n) { MENUITEMTYPE_SELECTABLE, n, \
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT, \
		(uintptr_t)"", 0, menuhandlerGauntletBoon }

static struct menuitem g_GauntletBoonItems[] = {
	ACHLINE(0), ACHLINE(1), ACHLINE(2),
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Pick a starting item for the next mission.\n", 0, NULL },
	BOONSLOT(0), BOONSLOT(1), BOONSLOT(2), BOONSLOT(3), BOONSLOT(4), BOONSLOT(5), BOONSLOT(6), BOONSLOT(7), BOONSLOT(8), BOONSLOT(9),
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_GauntletBoonDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Mission Rewards", g_GauntletBoonItems,
	NULL, MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS, NULL,
};

/** True while the pick is up; opens it when the game is in a state to take it. */
bool randomMenuBoonShow(void)
{
	const s32 prevplayernum = g_MpPlayerNum;
	struct player *pl = g_Vars.currentplayer;
	const s32 pnum = g_Vars.currentplayernum;
	const s32 menu = g_Vars.mplayerisrunning ? g_Vars.playerstats[pnum].mpindex : 0;
	bool up = false;

	if (menu < 0 || menu >= MAX_PLAYERS) {
		return false;
	}

	if (g_Menus[menu].curdialog != NULL) {
		return g_Menus[menu].curdialog->definition == &g_GauntletBoonDialog;
	}

	// this mission's offers: their names are the items' own
	for (s32 slot = 0; slot < MODCAREER_NUM_OFFERS; slot++) {
		g_GauntletBoonItems[4 + slot].param2 = (uintptr_t)modCareerBoonLabel(slot);
	}

	for (s32 i = 0; i < 3; i++) {
		modCareerAchievementText(i, g_GauntletAchText[i], sizeof(g_GauntletAchText[i]));
		g_GauntletBoonItems[i].param2 = (uintptr_t)g_GauntletAchText[i];
	}

	if (!pl || !pl->prop || pl->isdead || g_Vars.in_cutscene || g_Menus[menu].openinhibit != 0
			|| pl->pausemode != PAUSEMODE_UNPAUSED) {
		return false;
	}

	if (!g_Vars.mplayerisrunning) {
		g_MpPlayerNum = 0;
		g_Menus[0].playernum = 0;
		menuPushRootDialog(&g_GauntletBoonDialog, MENUROOT_MAINMENU);
		lvSetPaused(true);
		pl->pausemode = PAUSEMODE_PAUSED;
	} else {
		g_MpPlayerNum = menu;
		g_Menus[menu].playernum = pnum;
		menuPushRootDialog(&g_GauntletBoonDialog, MENUROOT_MPPAUSE);
	}

	up = true;
	g_MpPlayerNum = prevplayernum;

	return up;
}

/**
 * The permanent boon: after every third mission cleared, before the mission
 * pick. Green (the "success" palette) so it can't be mistaken for the blue
 * mission pick. What is taken here lasts for the rest of the run. Closing it
 * any other way just asks again (modCareerTick()).
 */
static MenuItemHandlerResult menuhandlerGauntletPerm(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		return item->param >= modCareerPermCount();
	}

	if (operation == MENUOP_SET) {
		modCareerPermTake(item->param);
	}

	return 0;
}

#define PERMSLOT(n) { MENUITEMTYPE_SELECTABLE, n, \
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT, \
		(uintptr_t)"", 0, menuhandlerGauntletPerm }

static struct menuitem g_GauntletPermItems[] = {
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Kept for the rest of the run.\n", 0, NULL },
	PERMSLOT(0), PERMSLOT(1), PERMSLOT(2), PERMSLOT(3), PERMSLOT(4),
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_GauntletPermDialog = {
	MENUDIALOGTYPE_SUCCESS, (uintptr_t)"Permanent Boon", g_GauntletPermItems,
	NULL, MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS, NULL,
};

/** True while the permanent pick is up; opens it when the game is in a state to take it. */
bool randomMenuPermShow(void)
{
	const s32 prevplayernum = g_MpPlayerNum;
	struct player *pl = g_Vars.currentplayer;
	const s32 pnum = g_Vars.currentplayernum;
	const s32 menu = g_Vars.mplayerisrunning ? g_Vars.playerstats[pnum].mpindex : 0;

	if (menu < 0 || menu >= MAX_PLAYERS) {
		return false;
	}

	if (g_Menus[menu].curdialog != NULL) {
		return g_Menus[menu].curdialog->definition == &g_GauntletPermDialog;
	}

	for (s32 slot = 0; slot < MODCAREER_PERM_COUNT; slot++) {
		g_GauntletPermItems[1 + slot].param2 = (uintptr_t)modCareerPermLabel(slot);
	}

	if (!pl || !pl->prop || pl->isdead || g_Vars.in_cutscene || g_Menus[menu].openinhibit != 0
			|| pl->pausemode != PAUSEMODE_UNPAUSED) {
		return false;
	}

	if (!g_Vars.mplayerisrunning) {
		g_MpPlayerNum = 0;
		g_Menus[0].playernum = 0;
		menuPushRootDialog(&g_GauntletPermDialog, MENUROOT_MAINMENU);
		lvSetPaused(true);
		pl->pausemode = PAUSEMODE_PAUSED;
	} else {
		g_MpPlayerNum = menu;
		g_Menus[menu].playernum = pnum;
		menuPushRootDialog(&g_GauntletPermDialog, MENUROOT_MPPAUSE);
	}

	g_MpPlayerNum = prevplayernum;

	return true;
}

/**
 * The closing screen of a gauntlet, won or lost: what the whole run added up
 * to. Its one button leaves for the main menu, as the end screens do.
 */
static char g_GauntletSummaryTitle[32];
static char g_GauntletSummaryText[6][64];

#define SUMMARYLINE { MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT | MENUITEMFLAG_LITERAL_TEXT, 0, 0, NULL }

static struct menuitem g_GauntletSummaryItems[] = {
	SUMMARYLINE, SUMMARYLINE, SUMMARYLINE, SUMMARYLINE, SUMMARYLINE, SUMMARYLINE,
	{ MENUITEMTYPE_SELECTABLE, 0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Main Menu", 0, NULL },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_GauntletSummaryDialog = {
	MENUDIALOGTYPE_SUCCESS, 0, g_GauntletSummaryItems,
	NULL, MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS, NULL,
};

void randomMenuSummaryPush(void)
{
	const u32 prevplayernum = g_MpPlayerNum;

	modCareerSummaryText(g_GauntletSummaryText, g_GauntletSummaryTitle, sizeof(g_GauntletSummaryTitle));
	g_GauntletSummaryDialog.title = (uintptr_t)g_GauntletSummaryTitle;

	for (s32 i = 0; i < 6; i++) {
		g_GauntletSummaryItems[i].param2 = (uintptr_t)g_GauntletSummaryText[i];
	}

	g_MpPlayerNum = 0;
	g_Menus[g_MpPlayerNum].playernum = 0;
	menuPushRootDialog(&g_GauntletSummaryDialog, MENUROOT_ENDSCREEN);
	g_MpPlayerNum = prevplayernum;
}

/**
 * After a Gauntlet's closing screen: the Perfect Random page with the
 * Gauntlet page on top of it, in the Institute. True once it is up.
 */
extern struct menudialogdef g_PerfectRandomMenuDialog;

bool randomMenuGauntletPageShow(void)
{
	const s32 prevplayernum = g_MpPlayerNum;
	struct player *pl = g_Vars.currentplayer;

	if (!pl || !pl->prop || pl->isdead || g_Vars.in_cutscene || g_Menus[0].openinhibit != 0
			|| g_Menus[0].curdialog != NULL || pl->pausemode != PAUSEMODE_UNPAUSED) {
		return false;
	}

	g_MpPlayerNum = 0;
	g_Menus[0].playernum = 0;
	menuPushRootDialog(&g_PerfectRandomMenuDialog, MENUROOT_MAINMENU);
	menuPushDialog(&g_GauntletMenuDialog);
	lvSetPaused(true);
	pl->pausemode = PAUSEMODE_PAUSED;
	g_MpPlayerNum = prevplayernum;

	return true;
}

/**
 * The briefing before a gauntlet mission: what the enemy has been given so
 * far, and what the reinforcements will be. Comes up before the boon pick;
 * Continue moves on to it.
 */
#define BRIEF_LINES 11
static char g_GauntletBriefText[BRIEF_LINES][72];
static bool g_GauntletBriefHeader2Mod; // the second header line is a new modifier

static MenuItemHandlerResult menuhandlerGauntletBrief(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		modCareerBriefAck();
	}

	return 0;
}

/** The two headline lines are coloured: what got harder, and what was added. */
static MenuItemHandlerResult menuhandlerGauntletBriefHead(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_GETCOLOUR) {
		if (item->param == 0) {
			data->label.colour1 = data->label.colour2 = 0xffa000ff; // orange: the ramp
		} else if (g_GauntletBriefHeader2Mod) {
			data->label.colour1 = data->label.colour2 = 0xff5050ff; // red: a new modifier
		}
	}

	return 0;
}

#define BRIEFHEAD(n) { MENUITEMTYPE_LABEL, n, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LABEL_CUSTOMCOLOUR, 0, 0, menuhandlerGauntletBriefHead }
#define BRIEFLINE { MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT | MENUITEMFLAG_LITERAL_TEXT, 0, 0, NULL }

static struct menuitem g_GauntletBriefItems[] = {
	BRIEFHEAD(0), BRIEFHEAD(1),
	BRIEFLINE, BRIEFLINE, BRIEFLINE, BRIEFLINE, BRIEFLINE, BRIEFLINE, BRIEFLINE, BRIEFLINE, BRIEFLINE,
	{ MENUITEMTYPE_SELECTABLE, 0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Continue", 0, menuhandlerGauntletBrief },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_GauntletBriefDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Enemy Intel", g_GauntletBriefItems,
	NULL, MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS, NULL,
};

/** True while the briefing is up; opens it when the game is in a state to take it. */
bool randomMenuBriefShow(void)
{
	const s32 prevplayernum = g_MpPlayerNum;
	struct player *pl = g_Vars.currentplayer;
	const s32 pnum = g_Vars.currentplayernum;
	const s32 menu = g_Vars.mplayerisrunning ? g_Vars.playerstats[pnum].mpindex : 0;

	if (menu < 0 || menu >= MAX_PLAYERS) {
		return false;
	}

	if (g_Menus[menu].curdialog != NULL) {
		return g_Menus[menu].curdialog->definition == &g_GauntletBriefDialog;
	}

	if (!pl || !pl->prop || pl->isdead || g_Vars.in_cutscene || g_Menus[menu].openinhibit != 0
			|| pl->pausemode != PAUSEMODE_UNPAUSED) {
		return false;
	}

	{
		const char *ramp = modCareerRampNew();
		const char *newmod = modCareerModNew();
		const s32 length = modCareerLength();
		char mission[32];
		s32 n = 2;

		if (length == MODCAREER_UNLIMITED) {
			snprintf(mission, sizeof(mission), "Mission %d", modCareerMission() + 1);
		} else {
			snprintf(mission, sizeof(mission), "Mission %d of %d", modCareerMission() + 1, length);
		}

		// the headline: what just got harder, and any modifier added
		if (ramp) {
			snprintf(g_GauntletBriefText[0], sizeof(g_GauntletBriefText[0]), "Harder: %s\n", ramp);
		} else {
			snprintf(g_GauntletBriefText[0], sizeof(g_GauntletBriefText[0]), "%s\n", modCareerRampNote());
		}

		g_GauntletBriefHeader2Mod = newmod != NULL;

		if (newmod) {
			snprintf(g_GauntletBriefText[1], sizeof(g_GauntletBriefText[1]), "New modifier: %s\n", newmod);
		} else {
			snprintf(g_GauntletBriefText[1], sizeof(g_GauntletBriefText[1]), "%s\n", mission);
		}

		// the rest: how things stand. Only what is actually in play.
#define BRIEFADD(...) snprintf(g_GauntletBriefText[n++], sizeof(g_GauntletBriefText[0]), __VA_ARGS__)
		if (newmod) {
			BRIEFADD("%s\n", mission);
		}
		BRIEFADD("Enemy accuracy: +%d%%\n", (s32)((modCareerAccuracyScale() - 1.0f) * 100.0f + 0.5f));
		BRIEFADD("Akimbo chance: %d%%\n", modCareerAkimboChance());
		BRIEFADD("Enemy respawn chance: %d%%\n", modCareerRespawnChance());
		BRIEFADD("Reinforcements: %d per wave\n", modGetRandomSpawnCount());

		if (modCareerShieldChance() > 0) {
			BRIEFADD("Shielded enemies: %d%%\n", modCareerShieldChance());
		}

		if (modCareerGrenadierChance() > 0) {
			BRIEFADD("Grenade throwers: %d%%\n", modCareerGrenadierChance());
		}

		if (modCareerObjectiveShield()) {
			BRIEFADD("Objective shields: yes\n");
		}
#undef BRIEFADD

		for (s32 i = n; i < BRIEF_LINES; i++) {
			g_GauntletBriefText[i][0] = '\0';
		}
	}

	for (s32 i = 0; i < BRIEF_LINES; i++) {
		g_GauntletBriefItems[i].param2 = (uintptr_t)g_GauntletBriefText[i];
	}

	if (!g_Vars.mplayerisrunning) {
		g_MpPlayerNum = 0;
		g_Menus[0].playernum = 0;
		menuPushRootDialog(&g_GauntletBriefDialog, MENUROOT_MAINMENU);
		lvSetPaused(true);
		pl->pausemode = PAUSEMODE_PAUSED;
	} else {
		g_MpPlayerNum = menu;
		g_Menus[menu].playernum = pnum;
		menuPushRootDialog(&g_GauntletBriefDialog, MENUROOT_MPPAUSE);
	}

	g_MpPlayerNum = prevplayernum;

	return true;
}

static struct menuitem g_PerfectRandomGeneratedMenuItems[] = {
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_DROPDOWN_ALLWIDTHS,
		(uintptr_t)"Map", 0, menuhandlerGeneratedMissionMap },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Difficulty", 0, menuhandlerCombatMissionDifficulty },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_BIGFONT,
		(uintptr_t)"Start Mission", 0, menuhandlerGeneratedMissionStart },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_PerfectRandomGeneratedMenuDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Perfect Rogue: Generated Missions", g_PerfectRandomGeneratedMenuItems,
	NULL, MENUDIALOGFLAG_LITERAL_TEXT, NULL,
};

/**
 * Start a run: one room at a time across every map, until the first death.
 */
static MenuItemHandlerResult menuhandlerRunStart(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
#ifndef PLATFORM_N64
		// A run is not a trial and not an ordinary mission; both of the other
		// doors' arming comes off on the way through this one.
		modGhostDisarmTrial();
		modRandomDisarmMission();
#endif
		modRunStart();
	}

	return 0;
}

/**
 * A mission dealt again from its own pieces: the roll armed for one mission,
 * and then the game's own mission select.
 */
static MenuItemHandlerResult menuhandlerRandomMission(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_MissionConfig.iscoop = false;
		g_MissionConfig.isanti = false;
		modRunStop();
		g_ModOptions.randomendless = 0;
		g_ModOptions.randomversion = MODRANDOM_VERSION_DEFAULT;

#ifndef PLATFORM_N64
		modGhostDisarmTrial();
#endif
		modRandomArmMission();
		menuPushDialog(&g_SelectMissionMenuDialog);
	}

	return 0;
}

/**
 * The run's score, on a row of its own.
 *
 * What the run in progress has done while it is being made, and the best kept
 * otherwise. A death is a fade and a failure dialog, so a number shown there
 * is a number nobody reads: this is where a player goes looking for it
 * afterwards.
 */
static char *menutextRunBest(struct menuitem *item)
{
	if (modRunIsOn()) {
		snprintf(g_RandomRowText, sizeof(g_RandomRowText),
				langTr("This run: %d objectives, %d rooms (best %d)\n"),
				modRunGetScore(), modRunGetRooms(), modRunGetBestScore());
	} else if (modRunGetBestScore() > 0 || modRunGetBestRooms() > 0) {
		snprintf(g_RandomRowText, sizeof(g_RandomRowText),
				langTr("Best run: %d objectives, %d rooms\n"),
				modRunGetBestScore(), modRunGetBestRooms());
	} else {
		snprintf(g_RandomRowText, sizeof(g_RandomRowText), "%s", langTr("No run yet\n"));
	}

	return g_RandomRowText;
}

/**
 * Which maps a run may land in.
 *
 * Everything is the default and the point of the mode. The narrower two are
 * for a build where a particular map has spoiled a run: a mod's map that
 * faults takes an hour of play with it, and this is the only thing a player
 * can do about that without editing pd.ini.
 */
static MenuItemHandlerResult menuhandlerRunPool(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Solo Missions", "Missions + Arenas", "Everything" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.runpool = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = modGetRunPool();
	}

	return 0;
}

/**
 * The difficulty every room of a run is played on. One for the whole run:
 * the rooms are not missions and there is no briefing to choose it at.
 */
static MenuItemHandlerResult menuhandlerRunDifficulty(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Agent", "Special Agent", "Perfect Agent" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.rundifficulty = DIFF_A + data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = modGetRunDifficulty() - DIFF_A;
	}

	return 0;
}

/**
 * Sealed Rooms: whether the doors of a run's room are shut until its objective
 * is done.
 *
 * On is the mode. It is here as a switch because the mode ran the other way
 * first, and a run where the objective is an offer rather than the price of
 * the next door is a different game rather than a broken one - a tour of the
 * whole map pool at the player's own pace, scored on what they chose to stop
 * for.
 */
static MenuItemHandlerResult menuhandlerRunSeal(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModOptions.runseal;
	case MENUOP_SET:
		g_ModOptions.runseal = data->checkbox.value;
		break;
	}

	return 0;
}

/**
 * Room Size: how much of the level a sealed room is - the landing room and
 * the rooms one door away, or two.
 */
static MenuItemHandlerResult menuhandlerRunRoomSize(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Normal", "Large" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.runroomsize = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = modGetRunRoomSize();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRunTint(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModOptions.runtint;
	case MENUOP_SET:
		g_ModOptions.runtint = data->checkbox.value;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRunWalkIn(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModOptions.runwalkin;
	case MENUOP_SET:
		g_ModOptions.runwalkin = data->checkbox.value;
		break;
	}

	return 0;
}

/**
 * The seed: the run, or a fresh one every time.
 *
 * There is nowhere in this menu to type a number, and a seed is not a thing
 * anyone invents anyway - it is a thing they keep. So the choice is between
 * dealing something new every time and holding on to what was just dealt,
 * shown so it can be written down. A seed typed by hand goes in pd.ini as
 * Mod.RandomizerSeed, and one somebody else hands over arrives the same way.
 */
static MenuItemHandlerResult menuhandlerRandomSeed(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char text[32];

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = 2;
		break;
	case MENUOP_GETOPTIONTEXT:
		if (data->dropdown.value == 0) {
			return (intptr_t)langTr("New Each Time");
		}

		if (g_ModOptions.randomseed) {
			snprintf(text, sizeof(text), "%u v%d", (u32)g_ModOptions.randomseed, g_ModOptions.randomversion);
		} else if (modRunIsOn() && modRunGetSeed()) {
			snprintf(text, sizeof(text), langTr("Keep %u"), modRunGetSeed());
		} else if (modRandomGetSeed()) {
			snprintf(text, sizeof(text), langTr("Keep %u v%d"), modRandomGetSeed(), modRandomGetVersion());
		} else {
			snprintf(text, sizeof(text), "%s", langTr("Keep This Run"));
		}

		return (intptr_t)text;
	case MENUOP_SET:
		if (data->dropdown.value == 0) {
			g_ModOptions.randomseed = 0;
		} else if (g_ModOptions.randomseed == 0) {
			// The seed the last thing dealt came from - a run's if one is
			// under way, the last mission's otherwise - so that liking what
			// was dealt and keeping it is one press rather than a number to
			// copy out of a log.
			const u32 seed = modRunIsOn() && modRunGetSeed() ? modRunGetSeed() : modRandomGetSeed();

			g_ModOptions.randomseed = (s32)(seed & S32_MAX);
			g_ModOptions.randomversion = modRandomGetVersion();
		}
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_ModOptions.randomseed ? 1 : 0;
	}

	return 0;
}

/**
 * Endless Mode, which belongs to Random Mission rather than to a run: one
 * objective at a time across the whole map, dealt again the moment it is
 * finished, scored in rooms covered. A run has its own idea of both and turns
 * this off while it is playing.
 */
static MenuItemHandlerResult menuhandlerRandomEndless(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModOptions.randomendless;
	case MENUOP_SET:
		g_ModOptions.randomendless = data->checkbox.value;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerHideUnfunMaps(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_GET) return g_ModCareerHideUnfunMaps;
	if (operation == MENUOP_SET) g_ModCareerHideUnfunMaps = data->checkbox.value;
	return 0;
}

/**
 * The best an Endless Mode mission has managed, kept beside its checkbox for
 * the same reason the run's score is kept beside Start Run.
 */
static char g_RandomEndlessRowText[96];

static char *menutextEndlessBest(struct menuitem *item)
{
	if (modRandomIsEndless() && modRandomGetRooms() > 0) {
		snprintf(g_RandomEndlessRowText, sizeof(g_RandomEndlessRowText),
				langTr("This mission: %d rooms, %d objectives (best %d)\n"),
				modRandomGetRooms(), modRandomGetCleared(), g_ModOptions.endlessbest);
	} else if (g_ModOptions.endlessbest > 0) {
		snprintf(g_RandomEndlessRowText, sizeof(g_RandomEndlessRowText),
				langTr("Best endless mission: %d rooms\n"), g_ModOptions.endlessbest);
	} else {
		snprintf(g_RandomEndlessRowText, sizeof(g_RandomEndlessRowText), "%s", langTr("No endless mission yet\n"));
	}

	return g_RandomEndlessRowText;
}

struct menuitem g_RandomOptionsMenuItems[] = {
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Run Maps",
		0,
		menuhandlerRunPool,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Run Difficulty",
		0,
		menuhandlerRunDifficulty,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Seal Rooms Until Done",
		0,
		menuhandlerRunSeal,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Room Size",
		0,
		menuhandlerRunRoomSize,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Tint Outside the Room",
		0,
		menuhandlerRunTint,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Guards Walk In",
		0,
		menuhandlerRunWalkIn,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Seed",
		0,
		menuhandlerRandomSeed,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Endless Rogue Mission",
		0,
		menuhandlerRandomEndless,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextEndlessBest,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_RandomOptionsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Rogue Run Options",
	g_RandomOptionsMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/**
 * The rows are the main menu's, not the options menu's - MENUITEMFLAG_BIGFONT
 * and no trailing newline, the way every row of the Perfect Menu is written.
 * This page sits next to Solo Missions rather than inside Options, and reading
 * like a settings page was the wrong signal about what it is.
 */
struct menuitem g_RandomizerMenuItems[] = {
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Start Run",
		0,
		menuhandlerRunStart,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextRunBest,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Rogue Run Options",
		0,
		(void *)&g_RandomOptionsMenuDialog,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_BIGFONT,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_RandomizerMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Rogue Run",
	g_RandomizerMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

// Whole-map missions use the player's customized rules. The original
// room-to-room run remains on the Randomizer page.
static MenuItemHandlerResult menuhandlerPerfectRandomSpawns(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char text[32];
	// MOD: the wave sizes go 1 to 16 a step at a time, then jump to the big crowds
	static const s32 waves[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 20, 24, 32, 48, 64, 100};
	const s32 numwaves = (s32)(sizeof(waves) / sizeof(waves[0]));
	const bool seconds = item->param == 0;
	s32 i;
	s32 best = 0;
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = seconds ? 60 : numwaves;
		break;
	case MENUOP_GETOPTIONTEXT:
		snprintf(text, sizeof(text), seconds ? "%d seconds" : "%d",
				seconds ? (s32)data->dropdown.value + 1 : waves[data->dropdown.value < numwaves ? data->dropdown.value : numwaves - 1]);
		return (intptr_t)text;
	case MENUOP_SET:
		if (seconds) {
			g_ModOptions.randomspawnseconds = data->dropdown.value + 1;
		} else {
			g_ModOptions.randomspawncount = waves[data->dropdown.value < numwaves ? data->dropdown.value : numwaves - 1];
		}
		break;
	case MENUOP_GETSELECTEDINDEX:
		if (seconds) {
			data->dropdown.value = modGetRandomSpawnSeconds() - 1;
		} else {
			// a value set in pd.ini that is not in the list shows as the nearest one below it
			for (i = 0; i < numwaves; i++) {
				if (waves[i] <= modGetRandomSpawnCount()) {
					best = i;
				}
			}

			data->dropdown.value = best;
		}
		break;
	}
	return 0;
}

/** Perfect Random: how many reinforcements may be up at once (stock's 32 up to 800). */
static const s32 g_RandomMaxGuardChoices[] = {8, 16, 24, 32, 48, 64, 80, 100, 150, 200, 300, 400, 600, 800};

static MenuItemHandlerResult menuhandlerRandomMaxGuards(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char text[16];
	const s32 count = (s32)(sizeof(g_RandomMaxGuardChoices) / sizeof(g_RandomMaxGuardChoices[0]));
	s32 i;
	s32 best = 0;

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = count;
		break;
	case MENUOP_GETOPTIONTEXT:
		snprintf(text, sizeof(text), "%d", g_RandomMaxGuardChoices[data->dropdown.value < count ? data->dropdown.value : count - 1]);
		return (intptr_t)text;
	case MENUOP_SET:
		g_ModOptions.randommaxguards = g_RandomMaxGuardChoices[data->dropdown.value < count ? data->dropdown.value : count - 1];
		break;
	case MENUOP_GETSELECTEDINDEX:
		// a value set in pd.ini that is not in the list shows as the nearest one below it
		for (i = 0; i < count; i++) {
			if (g_RandomMaxGuardChoices[i] <= modGetRandomMaxGuards()) {
				best = i;
			}
		}

		data->dropdown.value = best;
		break;
	}

	return 0;
}

/** Chance each dead stage guard returns after an objective: 0..100% in tens. */
static MenuItemHandlerResult menuhandlerRandomRespawn(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char text[16];

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = 11;
		break;
	case MENUOP_GETOPTIONTEXT:
		snprintf(text, sizeof(text), "%d%%", (s32)data->dropdown.value * 10);
		return (intptr_t)text;
	case MENUOP_SET:
		g_ModOptions.randomrespawnpercent = data->dropdown.value * 10;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = (modGetRandomRespawnPercent() + 5) / 10;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRandomGuardModels(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *presets[] = {"Jo", "Maian", "Skedar", "dataDyne", "Carrington", "Femme Fatale", "Custom"};
	s32 role = item->param == 255 ? -1 : item->param;
	if (role == 4) {
		// the co-op partners: Velvet Dark, then the same looks the guards have
		if (operation == MENUOP_GETOPTIONCOUNT) data->dropdown.value = modRandomAllyModelCount();
		else if (operation == MENUOP_GETOPTIONTEXT) return (intptr_t)modRandomAllyModelName(data->dropdown.value);
		else if (operation == MENUOP_GETSELECTEDINDEX) data->dropdown.value = modRandomModelIndex(g_RandomAllyModel - 1) + 1;
		else if (operation == MENUOP_SET) g_RandomAllyModel = modRandomModelId((s32)data->dropdown.value - 1) + 1;
		return 0;
	}
	if (role >= 5 && role <= 7) {
		// civilian, kill target, KO target: the mode's own look, then the guard looks
		s32 *slot = &g_RandomExtraModels[role - 5];
		if (operation == MENUOP_GETOPTIONCOUNT) data->dropdown.value = modRandomAllyModelCount();
		else if (operation == MENUOP_GETOPTIONTEXT) return (intptr_t)modRandomExtraModelName(data->dropdown.value);
		else if (operation == MENUOP_GETSELECTEDINDEX) data->dropdown.value = modRandomModelIndex(*slot - 1) + 1;
		else if (operation == MENUOP_SET) *slot = modRandomModelId((s32)data->dropdown.value - 1) + 1;
		return 0;
	}
	if (operation == MENUOP_GETOPTIONCOUNT) data->dropdown.value = role < 0 ? ARRAYCOUNT(presets) : modRandomModelCount();
	else if (operation == MENUOP_GETOPTIONTEXT) return (intptr_t)(role < 0 ? presets[data->dropdown.value] : modRandomModelName(data->dropdown.value));
	else if (operation == MENUOP_GETSELECTEDINDEX) data->dropdown.value = role < 0 ? modRandomModelPreset() : MAX(0, modRandomModelIndex(g_RandomGuardModels[role]));
	else if (operation == MENUOP_SET) {
		if (role < 0) modRandomApplyModelPreset(data->dropdown.value);
		else g_RandomGuardModels[role] = modRandomModelId(data->dropdown.value);
	}
	return 0;
}
static struct menuitem g_RandomGuardModelItems[] = {
	{MENUITEMTYPE_DROPDOWN, -1, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_DROPDOWN_ALLWIDTHS, (uintptr_t)"Preset", 0, menuhandlerRandomGuardModels},
	{MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_DROPDOWN_ALLWIDTHS, (uintptr_t)"Stage guards", 0, menuhandlerRandomGuardModels},
	{MENUITEMTYPE_DROPDOWN, 1, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_DROPDOWN_ALLWIDTHS, (uintptr_t)"Hunters", 0, menuhandlerRandomGuardModels},
	{MENUITEMTYPE_DROPDOWN, 3, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_DROPDOWN_ALLWIDTHS, (uintptr_t)"Respawn", 0, menuhandlerRandomGuardModels},
	{MENUITEMTYPE_DROPDOWN, 2, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_DROPDOWN_ALLWIDTHS, (uintptr_t)"Punishment", 0, menuhandlerRandomGuardModels},
	{MENUITEMTYPE_DROPDOWN, 5, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_DROPDOWN_ALLWIDTHS, (uintptr_t)"Civilian", 0, menuhandlerRandomGuardModels},
	{MENUITEMTYPE_DROPDOWN, 4, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_DROPDOWN_ALLWIDTHS, (uintptr_t)"Co-op Partners", 0, menuhandlerRandomGuardModels},
	{MENUITEMTYPE_DROPDOWN, 6, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_DROPDOWN_ALLWIDTHS, (uintptr_t)"Kill target", 0, menuhandlerRandomGuardModels},
	{MENUITEMTYPE_DROPDOWN, 7, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_DROPDOWN_ALLWIDTHS, (uintptr_t)"KO target", 0, menuhandlerRandomGuardModels},
	{MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL},
	{MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL},
	{MENUITEMTYPE_END},
};
static struct menudialogdef g_RandomGuardModelDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Character Models", g_RandomGuardModelItems,
	NULL, MENUDIALOGFLAG_LITERAL_TEXT, NULL,
};
static MenuItemHandlerResult menuhandlerRandomDebug(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static s32 *const settings[] = {
		&g_ModRandomDebug, &g_ModRandomDebugTint, &g_ModRandomDebugRadar,
		&g_ModRandomDebugDefendersRadar, &g_ModRandomDebugStateCount,
	};
	s32 *setting = settings[item->param];

	if (operation == MENUOP_GET) {
		return *setting;
	}
	if (operation == MENUOP_SET) {
		*setting = data->checkbox.value;
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerRandomDebugCloak(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModRandomDebugCloak;
	case MENUOP_SET:
		g_ModRandomDebugCloak = data->checkbox.value;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRandomDebugShotguns(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModRandomDebugShotguns;
	case MENUOP_SET:
		g_ModRandomDebugShotguns = data->checkbox.value;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRandomDebugMaxGrenades(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_GET) return g_ModRandomDebugMaxGrenades;
	if (operation == MENUOP_SET) g_ModRandomDebugMaxGrenades = data->checkbox.value;
	return 0;
}

static MenuItemHandlerResult menuhandlerRandomDebugAlienWar(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModCareerDebugAlienWar;
	case MENUOP_SET:
		g_ModCareerDebugAlienWar = data->checkbox.value;
		break;
	}

	return 0;
}

static struct menuitem g_PerfectRandomDifficultyItems[] = {
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Spawn Interval", 0, menuhandlerPerfectRandomSpawns },
	{ MENUITEMTYPE_DROPDOWN, 1, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Enemies Per Wave", 0, menuhandlerPerfectRandomSpawns },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Max Reinforcements", 0, menuhandlerRandomMaxGuards },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Guard Respawn Chance", 0, menuhandlerRandomRespawn },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, 0, NULL },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_PerfectRandomDifficultyDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Rogue Difficulty",
	g_PerfectRandomDifficultyItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static struct menudialogdef g_PerfectRandomSingleMissionDialog; /* defined below */

static struct menuitem g_PerfectRandomDebugItems[] = {
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Test Single Mission\n", 0, (void *)&g_PerfectRandomSingleMissionDialog },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Debug Text", 0, menuhandlerRandomDebug },
	{ MENUITEMTYPE_CHECKBOX, 1, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Enemy AI State Shading", 0, menuhandlerRandomDebug },
	{ MENUITEMTYPE_CHECKBOX, 4, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"AI state count", 0, menuhandlerRandomDebug },
	{ MENUITEMTYPE_CHECKBOX, 2, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Aggressive Enemies on Radar", 0, menuhandlerRandomDebug },
	{ MENUITEMTYPE_CHECKBOX, 3, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Defenders on radar", 0, menuhandlerRandomDebug },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Invisible Player", 0, menuhandlerRandomDebugCloak },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"All Super Shotguns", 0, menuhandlerRandomDebugShotguns },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Max grenades", 0, menuhandlerRandomDebugMaxGrenades },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Add Alien War (performance)", 0, menuhandlerRandomDebugAlienWar },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, 0, NULL },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_PerfectRandomDebugDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Debug",
	g_PerfectRandomDebugItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

#ifdef PD_PHYSICS
static MenuItemHandlerResult menuhandlerGuardVoicePack(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const enum guardvoicesex sex = item->param;
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT: data->dropdown.value = guardVoicePackCount(sex); break;
	case MENUOP_GETOPTIONTEXT: return (intptr_t)guardVoicePackName(sex, data->dropdown.value);
	case MENUOP_GETSELECTEDINDEX: data->dropdown.value = guardVoicePackSelected(sex); break;
	case MENUOP_SET: guardVoicePackSelect(sex, data->dropdown.value); break;
	}
	return 0;
}

static s32 menudialogGuardVoicePack(s32 operation, struct menudialogdef *dialog, union handlerdata *data)
{
	if (operation == MENUOP_OPEN) guardVoiceRefreshPacks();
	return 0;
}

static struct menuitem g_GuardVoicePackItems[] = {
	{ MENUITEMTYPE_DROPDOWN, GUARDVOICE_MALE, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Male guards", 0, menuhandlerGuardVoicePack },
	{ MENUITEMTYPE_DROPDOWN, GUARDVOICE_FEMALE, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Female guards", 0, menuhandlerGuardVoicePack },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, 0, NULL },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_GuardVoicePackDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Voice Pack", g_GuardVoicePackItems,
	menudialogGuardVoicePack, MENUDIALOGFLAG_LITERAL_TEXT, NULL,
};
#endif

static struct menuitem g_PerfectRandomOptionsMenuItems[] = {
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Character Models\n", 0, (void *)&g_RandomGuardModelDialog },
#ifdef PD_PHYSICS
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Voice Pack\n", 0, (void *)&g_GuardVoicePackDialog },
#endif
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Special Events\n", 0, (void *)&g_GauntletEventDialog },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Debug\n", 0, (void *)&g_PerfectRandomDebugDialog },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Hide unfun maps", 0, menuhandlerHideUnfunMaps },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, 0, NULL },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_PerfectRandomOptionsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Perfect Rogue Options",
	g_PerfectRandomOptionsMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static struct menuitem g_PerfectRandomGeItems[] = {
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Map", 0, menuhandlerRandomGeMap },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Difficulty", 0, menuhandlerRunDifficulty },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_BIGFONT, (uintptr_t)"Start Mission", 0, menuhandlerRandomGeStart },
	{ MENUITEMTYPE_END },
};
static struct menudialogdef g_PerfectRandomGeDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Perfect Rogue: GoldenEye", g_PerfectRandomGeItems,
	NULL, MENUDIALOGFLAG_LITERAL_TEXT, NULL,
};

/**
 * The Spawn Customizer (modspawnedit.c): pick a map, fly through it, and see
 * where guards, hunters, objectives and the start go. Phase 1 is view only.
 */
static MenuItemHandlerResult menuhandlerSpawnEditCombat(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) return g_MpSetup.stagenum == STAGE_MP_RANDOM;
	if (operation == MENUOP_SET) {
		modGhostDisarmTrial();
		modRandomDisarmMission();
		g_ModOptions.randomendless = 0;
		g_ModOptions.randomversion = MODRANDOM_VERSION_DEFAULT;
		modSpawnEditStart(g_MpSetup.stagenum);
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerSpawnEditGe(s32 operation, struct menuitem *item, union handlerdata *data)
{
	s32 stage = menuhandlerRandomGeMapStage(g_PerfectRandomGeMission);
	if (operation == MENUOP_CHECKDISABLED) return !stage;
	if (operation == MENUOP_SET && stage) {
		modGhostDisarmTrial();
		modRandomDisarmMission();
		g_ModOptions.randomendless = 0;
		g_ModOptions.randomversion = MODRANDOM_VERSION_DEFAULT;
		modSpawnEditStart(stage);
	}
	return 0;
}

/**
 * The Spawn Customizer's page: Game, Mission (the list follows the game),
 * Load Setting (not written yet), Start Editor.
 */
#define SPAWNEDIT_GAME_PD     0
#define SPAWNEDIT_GAME_GE     1
#define SPAWNEDIT_GAME_COMBAT 2
#define SPAWNEDIT_NUMGAMES    3

static s32 g_PerfectRandomSpawnEditGame;
static s32 g_PerfectRandomSpawnEditMap;

/** The nth Combat Simulator arena the editor offers, as an index into g_MpArenas; -1 past the end. */
static s32 spawnEditArenaAt(s32 n)
{
	s32 i;
	s32 count = 0;

	for (i = 0; i < mpGetNumStages(); i++) {
		if (!challengeIsFeatureUnlocked(g_MpArenas[i].requirefeature)
				|| g_MpArenas[i].stagenum == STAGE_MP_RANDOM || stageGetIndex(g_MpArenas[i].stagenum) < 0) {
			continue;
		}

		if (count == n) {
			return i;
		}

		count++;
	}

	return -1;
}

static s32 spawnEditMapCount(void)
{
	s32 n = 0;

	switch (g_PerfectRandomSpawnEditGame) {
	case SPAWNEDIT_GAME_PD:
		return NUM_SOLOSTAGES;
	case SPAWNEDIT_GAME_GE:
		return gelevelsCount();
	default:
		while (spawnEditArenaAt(n) >= 0) {
			n++;
		}

		return n;
	}
}

/** The stage the page's Game and Mission name, or 0. */
static s32 spawnEditStage(void)
{
	s32 i = g_PerfectRandomSpawnEditMap;

	switch (g_PerfectRandomSpawnEditGame) {
	case SPAWNEDIT_GAME_PD:
		return i >= 0 && i < NUM_SOLOSTAGES ? (s32)g_SoloStages[i].stagenum : 0;
	case SPAWNEDIT_GAME_GE:
		return i >= 0 && i < gelevelsCount() ? gelevelsStageAt(i) : 0;
	default:
		i = spawnEditArenaAt(i);
		return i >= 0 ? g_MpArenas[i].stagenum : 0;
	}
}

static MenuItemHandlerResult menuhandlerSpawnEditGame(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *names[SPAWNEDIT_NUMGAMES] = { "Perfect Dark", "GoldenEye", "Combat Simulator" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = SPAWNEDIT_NUMGAMES;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)names[data->dropdown.value];
	case MENUOP_SET:
		if (g_PerfectRandomSpawnEditGame != data->dropdown.value) {
			g_PerfectRandomSpawnEditGame = data->dropdown.value;
			g_PerfectRandomSpawnEditMap = 0;
		}
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_PerfectRandomSpawnEditGame;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerSpawnEditMap(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = spawnEditMapCount() > 0 ? spawnEditMapCount() : 1;
		break;
	case MENUOP_GETOPTIONTEXT: {
		static char names[4][96];
		static s32 next;
		char *buf = names[next++ & 3];
		s32 i = data->dropdown.value;

		switch (g_PerfectRandomSpawnEditGame) {
		case SPAWNEDIT_GAME_PD:
			if (i >= 0 && i < NUM_SOLOSTAGES) {
				snprintf(buf, 96, "%s%s", langGet(g_SoloStages[i].name1), langGet(g_SoloStages[i].name2));
				return (intptr_t)buf;
			}
			break;
		case SPAWNEDIT_GAME_GE: {
			const char *name = i >= 0 && i < gelevelsCount() ? gelevelsName(gelevelsStageAt(i)) : NULL;
			return (intptr_t)(name ? name : "(run tools/geconvert first)");
		}
		default:
			i = spawnEditArenaAt(i);
			if (i >= 0) {
				return (intptr_t)langGet(g_MpArenas[i].name);
			}
			break;
		}

		return (intptr_t)"(none)";
	}
	case MENUOP_SET:
		g_PerfectRandomSpawnEditMap = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_PerfectRandomSpawnEditMap;
		break;
	}

	return 0;
}

/**
 * Setting: which saved spawn layout the chosen map uses, in the editor and in
 * Perfect Random missions on that map. "Default" is the map's own. Saving in
 * the editor chooses what was saved.
 */
static char g_PerfectRandomSpawnEditSettings[41][MODSPAWN_CONFIG_NAMELEN + 1];
static s32 g_PerfectRandomSpawnEditNumSettings;

static void spawnEditRefreshSettings(void)
{
	s32 stage = spawnEditStage();

	if (stage) {
		g_PerfectRandomSpawnEditNumSettings = modSpawnConfigList(stage, g_PerfectRandomSpawnEditSettings, 41);
	} else {
		snprintf(g_PerfectRandomSpawnEditSettings[0], MODSPAWN_CONFIG_NAMELEN + 1, "Default");
		g_PerfectRandomSpawnEditNumSettings = 1;
	}
}

static MenuItemHandlerResult menuhandlerSpawnEditLoad(s32 operation, struct menuitem *item, union handlerdata *data)
{
	s32 stage = spawnEditStage();
	s32 i;

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		spawnEditRefreshSettings();
		data->dropdown.value = g_PerfectRandomSpawnEditNumSettings;
		break;
	case MENUOP_GETOPTIONTEXT:
		i = data->dropdown.value;
		return (intptr_t)(i >= 0 && i < g_PerfectRandomSpawnEditNumSettings ? g_PerfectRandomSpawnEditSettings[i] : "Default");
	case MENUOP_SET:
		i = data->dropdown.value;

		if (stage && i > 0 && i < g_PerfectRandomSpawnEditNumSettings) {
			modSpawnConfigUse(stage, g_PerfectRandomSpawnEditSettings[i]);
		} else {
			modSpawnConfigOff(stage);
		}
		break;
	case MENUOP_GETSELECTEDINDEX: {
		const char *chosen = stage ? modSpawnConfigChosenName(stage) : NULL;

		data->dropdown.value = 0;

		for (i = 1; chosen && i < g_PerfectRandomSpawnEditNumSettings; i++) {
			if (strcasecmp(chosen, g_PerfectRandomSpawnEditSettings[i]) == 0) {
				data->dropdown.value = i;
			}
		}
		break;
	}
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerSpawnEditStart(s32 operation, struct menuitem *item, union handlerdata *data)
{
	s32 stage = spawnEditStage();

	if (operation == MENUOP_CHECKDISABLED) return !stage;

	if (operation == MENUOP_SET && stage) {
		modGhostDisarmTrial();
		modRandomDisarmMission();
		g_ModOptions.randomendless = 0;
		g_ModOptions.randomversion = MODRANDOM_VERSION_DEFAULT;
		modSpawnEditStart(stage);
	}

	return 0;
}

static struct menuitem g_PerfectRandomSpawnEditItems[] = {
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Game", 0, menuhandlerSpawnEditGame },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_DROPDOWN_ALLWIDTHS,
		(uintptr_t)"Mission", 0, menuhandlerSpawnEditMap },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_DROPDOWN_ALLWIDTHS,
		(uintptr_t)"Setting", 0, menuhandlerSpawnEditLoad },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_BIGFONT,
		(uintptr_t)"Start Editor", 0, menuhandlerSpawnEditStart },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_PerfectRandomSpawnEditDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Spawn Customizer", g_PerfectRandomSpawnEditItems,
	NULL, MENUDIALOGFLAG_LITERAL_TEXT, NULL,
};

static struct menuitem g_PerfectRandomGeneratedLevelItems[] = {
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Generated Level", 0, menuhandlerLevelGenStart },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Generated Level Layout", 0, menuhandlerLevelGenLayout },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Generated Level Size", 0, menuhandlerLevelGenSize },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_PerfectRandomGeneratedLevelDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Generated Levels (experimental)", g_PerfectRandomGeneratedLevelItems,
	NULL, MENUDIALOGFLAG_LITERAL_TEXT, NULL,
};

static struct menuitem g_PerfectRandomSingleMissionItems[] = {
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Perfect Dark Mission", 0, menuhandlerRandomMission },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"GoldenEye Mission", 0, (void *)&g_PerfectRandomGeDialog },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Combat Sim Mission", 0, (void *)&g_PerfectRandomArenaMenuDialog },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Generated Levels (experimental)", 0, (void *)&g_PerfectRandomGeneratedLevelDialog },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Rogue Difficulty", 0, (void *)&g_PerfectRandomDifficultyDialog },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_PerfectRandomSingleMissionDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Test Single Mission", g_PerfectRandomSingleMissionItems,
	NULL, MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_SCROLLBAR, NULL,
};

#define CREDITLINE(text) { MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT | MENUITEMFLAG_LITERAL_TEXT, \
		(uintptr_t)(text "\n"), 0, NULL }

static struct menuitem g_PerfectRandomCreditsItems[] = {
	CREDITLINE("Mod by Chickensalad"),
	CREDITLINE("Original ragdoll implementation: Chesluk-Industries"),
	CREDITLINE("PC Port: fgsfdsfgs"),
	CREDITLINE("Game: Rare"),
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_PerfectRandomCreditsDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Credits", g_PerfectRandomCreditsItems,
	NULL, MENUDIALOGFLAG_LITERAL_TEXT, NULL,
};

static struct menuitem g_PerfectRandomMenuItems[] = {
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_BIGFONT,
		(uintptr_t)"Gauntlet", 0, (void *)&g_GauntletMenuDialog },
	{
		MENUITEMTYPE_SELECTABLE, 0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Perfect Rogue Options", 0, (void *)&g_PerfectRandomOptionsMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE, 0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Spawn Customizer", 0, (void *)&g_PerfectRandomSpawnEditDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE, 0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Credits", 0, (void *)&g_PerfectRandomCreditsDialog,
	},
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{
		MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Seed", 0, menuhandlerRandomSeed,
	},
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{
		MENUITEMTYPE_SELECTABLE, 0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_BIGFONT,
		L_OPTIONS_213, 0, NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_PerfectRandomMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Perfect Rogue",
	g_PerfectRandomMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};
