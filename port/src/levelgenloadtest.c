#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ultra64.h>
#include "constants.h"
#include "prcompat.h"
#include "types.h"
#include "data.h"
#include "bss.h"
#include "lib/vars.h"
#include "lib/collision.h"
#include "game/propobj.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/pad.h"
#include "game/bg.h"
#include "game/dlights.h"
#include "game/setuputils.h"
#include "game/file.h"
#include "game/modlevelgenfile.h"
#include "game/modcareer.h"
#include "game/modrandom.h"
#include "game/modoptions.h"
#include "game/modlevelgen.h"
#include "game/modlevelgenmap.h"
#include "game/player.h"
#include "levelgenstage.h"
#include "system.h"
#include "fs.h"
#ifdef _WIN32
#include <direct.h>
#define LGT_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define LGT_MKDIR(p) mkdir(p, 0777)
#endif
#include "game/prop.h"
#include <math.h>

/**
 * The generated level, loaded for real: --selftest-levelgen-load
 *
 * Boots the Institute, then (from modRunTick()) starts a generated level the
 * way the menu does, checks what it finds, starts a second one straight after
 * the first and checks that, then exits. Checks, per level:
 *
 *   - the game finds the floor under every pad, in the pad's own room
 *   - every opening lets a line through and every area's wall stops one
 *   - the player is alive, inside the level and standing near the floor a
 *     couple of seconds in
 *   - guards exist
 *
 * Everything it sees is logged, so a failure says what the level looked like.
 */

bool g_LevelGenLoadTest = false; // --selftest-levelgen-load

#define LGT_LEVELS     3   /* one of each layout: a hub, an underground lab, a street */
#define LGT_SAMPLE1    10     // frames into the level: where the player started
#define LGT_SAMPLE2    200    // and after a few seconds, once the screenshots are taken
#define LGT_SHOT0      40     // the first screenshot's frame
#define LGT_SHOTGAP    20     // frames between screenshots (the view is turned in between)
#define LGT_NUMSHOTS   6

// MOD: port/fast3d/gfx_pc.cpp: saves the next frame drawn as a BMP
extern void gfx_request_screenshot(const char *path);
extern void gfx_request_screenshot_small(const char *path, uint32_t maxw);

// The screenshots: the view from the start pad turned to each side, then up at
// the ceiling and down at the floor (yaw, pitch in degrees). Written to
// source/test-results/ (the tests run from the game's folder) for Claude to look at.
static const f32 g_LgtShotView[LGT_NUMSHOTS][2] = {
	{ 0.0f, 0.0f }, { 90.0f, 0.0f }, { 180.0f, 0.0f }, { 270.0f, 0.0f }, { 30.0f, 50.0f }, { 120.0f, -35.0f },
};
static u32 g_LgtShotsTaken = 0;
#define LGT_BOOTFRAMES 60
#define LGT_TIMEOUT    4000   // ticks without a result
#define LGT_WAIST      50.0f  // line tests run this high over the floor

static s32 g_LgtLevel = 0;        // levels checked so far
static s32 g_LgtPhase = 0;        // 0 boot, 1 waiting for a level, 2 level up, 3 done
static s32 g_LgtPrevFrame = 0;
static s32 g_LgtTicks = 0;
static s32 g_LgtPassed = 0;
static s32 g_LgtFailed = 0;
static bool g_LgtSampled1 = false;
static u32 g_LgtSeeds[LGT_LEVELS] = { 424242u, 777u, 109u };   /* 109: a street with a bridge (2026-11-14) */
static s32 g_LgtHalf[LGT_LEVELS][2];

static void lgtCheck(bool ok, const char *name, const char *detail)
{
	if (ok) {
		g_LgtPassed++;
		sysLogPrintf(LOG_NOTE, "SELFTEST PASS %s", name);
	} else {
		g_LgtFailed++;
		sysLogPrintf(LOG_NOTE, "SELFTEST FAIL %s: %s", name, detail ? detail : "");
	}
}

static void lgtFinish(void)
{
	sysLogPrintf(LOG_NOTE, "SELFTEST RESULT levelgen-load passed %d failed %d", g_LgtPassed, g_LgtFailed);
	exit(g_LgtFailed ? 1 : 0);
}

static s32 lgtCountGuards(void)
{
	s32 n = 0;
	s32 i;

	for (i = 0; i < chrsGetNumSlots(); i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr->model && chr->prop && chr->prop->type == PROPTYPE_CHR
				&& (!g_Vars.bond || chr->prop != g_Vars.bond->prop) && !chrIsDead(chr)) {
			n++;
		}
	}

	return n;
}

/** A line at waist height over the floor (floory) between two points, as the game's own line test sees it: true if geometry with one of the flags stops it */
static bool lgtLineBlockedBy(f32 x0, f32 z0, s32 room, f32 x1, f32 z1, f32 floory, u32 flags)
{
	struct coord a;
	struct coord b;
	RoomNum rooms[2];

	a.x = x0; a.y = floory + LGT_WAIST; a.z = z0;
	b.x = x1; b.y = floory + LGT_WAIST; b.z = z1;
	rooms[0] = room;
	rooms[1] = -1;

	return cdExamLos08(&a, rooms, &b, CDTYPE_BG, flags) == CDRESULT_COLLISION;
}

/** True if a wall stops walking along that line */
static bool lgtLineBlocked(f32 x0, f32 z0, s32 room, f32 x1, f32 z1, f32 floory)
{
	return lgtLineBlockedBy(x0, z0, room, x1, z1, floory, GEOFLAG_WALL);
}

/**
 * The level as the game sees it, against the layout it was made from:
 *  - every pad has a floor under it, in the pad's own room;
 *  - a line through every opening, from the nav pad one side to the one the
 *    other, is not stopped (the portals join the rooms and no wall is in it);
 *  - a line through the middle of one closed wall of every area is stopped.
 * One line each per kind and level, with the first failures logged.
 */
static void lgtCheckLevel(const char *tag)
{
	const LgLayout *lg = levelgenGetLayout();
	char name[96];
	char detail[200];
	s32 i;
	s32 bad;
	s32 shown;

	if (lg == NULL || g_PadsFile == NULL) {
		snprintf(name, sizeof(name), "levelgen-load: %s has a layout", tag);
		lgtCheck(false, name, "no layout or no pads file");
		return;
	}

	sysLogPrintf(LOG_NOTE, "levelgen test: %s: %d pads (layout %d), %d rooms (layout %d areas), %d tile rooms, %d portals",
			tag, g_PadsFile->numpads, lg->numpads, g_Vars.roomcount, lg->numareas, g_TileNumRooms, lg->numportals);

	snprintf(name, sizeof(name), "levelgen-load: %s rooms and pads match the layout", tag);
	snprintf(detail, sizeof(detail), "%d rooms for %d areas, %d pads for %d", g_Vars.roomcount, lg->numareas,
			g_PadsFile->numpads, lg->numpads);
	lgtCheck(g_Vars.roomcount == lg->numareas + 1 && g_PadsFile->numpads >= lg->numpads, name, detail);

	/* every portal as the layout has it: the game keeps its rooms (low side
	 * room1, high side room2) and its normal points into the high side, so
	 * the room through it is drawn from either side (2026-11-14: from a bridge
	 * the street on one side was culled, the portal's rooms swapped) */
	{
		s32 pbad = 0, pfirst = -1;

		for (i = 0; i < lg->numportals; i++) {
			const LgPortal *p = &lg->portals[i];
			// a point 20 into the high side across the middle of the opening
			// (not area b's centre: a street's can be on its bridge's side)
			const f32 bx = (f32)(lg->originx + (p->x0 + p->x1) * LG_CELL / 2) + (p->x0 == p->x1 ? 20.0f : 0.0f);
			const f32 bz = (f32)(lg->originz + (p->z0 + p->z1) * LG_CELL / 2) + (p->z0 == p->z1 ? 20.0f : 0.0f);
			const f32 by = (f32)lg->areas[p->b].floor + 50.0f;
			const f32 v = g_PortalMetrics[i].normal.x * bx + g_PortalMetrics[i].normal.y * by + g_PortalMetrics[i].normal.z * bz;

			if (g_BgPortals[i].roomnum1 != p->a + 1 || g_BgPortals[i].roomnum2 != p->b + 1 || !(v > g_PortalMetrics[i].max)) {
				if (pfirst < 0) {
					pfirst = i;
				}
				pbad++;
			}
		}
		snprintf(name, sizeof(name), "levelgen-load: %s every portal faces its high side", tag);
		if (pfirst >= 0) {
			snprintf(detail, sizeof(detail), "%d of %d wrong, first %d: rooms %d %d, layout %d %d", pbad, lg->numportals, pfirst,
					g_BgPortals[pfirst].roomnum1, g_BgPortals[pfirst].roomnum2, lg->portals[pfirst].a + 1, lg->portals[pfirst].b + 1);
		} else {
			snprintf(detail, sizeof(detail), "%d portals", lg->numportals);
		}
		lgtCheck(pbad == 0, name, detail);
	}

	/* the floor under every pad */
	bad = 0;
	shown = 0;
	detail[0] = '\0';

	for (i = 0; i < lg->numpads; i++) {
		struct pad pad;
		struct coord query;
		RoomNum rooms[2];
		RoomNum floorroom = -1;
		u16 flags = 0;
		f32 ground;

		padUnpack(i, PADFIELD_POS | PADFIELD_ROOM, &pad);
		rooms[0] = pad.room;
		rooms[1] = -1;
		query = pad.pos;
		query.y += 10.0f;
		ground = cdFindGroundInfoAtCyl(&query, 30, rooms, NULL, NULL, &flags, &floorroom, NULL, NULL);

		// the floor at the pad's own height: 0 mostly, a gallery's raised floor, a ramp's slope
		// on a ramp the search cylinder (30 across) finds the slope's high side: 0.4 x 30
		const f32 tol = lg->areas[lg->pads[i].area].ramp ? 15.0f : 1.0f;

		if (floorroom != pad.room || ground < lg->pads[i].y - tol || ground > lg->pads[i].y + tol || pad.room != lg->pads[i].area + 1) {
			if (shown++ < 5) {
				sysLogPrintf(LOG_NOTE, "levelgen test: %s: pad %d at (%.0f %.0f %.0f) room %d (layout area %d): ground %.1f floorroom %d",
						tag, i, pad.pos.x, pad.pos.y, pad.pos.z, pad.room, lg->pads[i].area, ground, (int)floorroom);
			}
			if (bad++ == 0) {
				snprintf(detail, sizeof(detail), "pad %d at (%.0f %.0f) room %d: ground %.1f in room %d",
						i, pad.pos.x, pad.pos.z, pad.room, ground, (int)floorroom);
			}
		}
	}

	snprintf(name, sizeof(name), "levelgen-load: %s every pad has a floor in its room (%d pads)", tag, lg->numpads);
	lgtCheck(bad == 0, name, detail);

	/* every opening can be seen and walked through */
	bad = 0;
	shown = 0;
	detail[0] = '\0';

	for (i = 0; i < lg->numportals; i++) {
		const LgPortal *pt = &lg->portals[i];
		const LgPad *pa;
		const LgPad *pb;

		if (pt->pada < 0 || pt->padb < 0) {
			continue;
		}

		pa = &lg->pads[pt->pada];
		pb = &lg->pads[pt->padb];

		if (lgtLineBlocked(pa->x, pa->z, pa->area + 1, pb->x, pb->z, pa->y > pb->y ? pa->y : pb->y)) {
			if (shown++ < 5) {
				sysLogPrintf(LOG_NOTE, "levelgen test: %s: opening %d (areas %d-%d): line (%d %d) to (%d %d) is stopped",
						tag, i, pt->a, pt->b, pa->x, pa->z, pb->x, pb->z);
			}
			if (bad++ == 0) {
				snprintf(detail, sizeof(detail), "opening %d between areas %d and %d", i, pt->a, pt->b);
			}
		}
	}

	snprintf(name, sizeof(name), "levelgen-load: %s every opening is open (%d)", tag, lg->numportals);
	lgtCheck(bad == 0, name, detail);

	/* a closed wall of every area stops a line through it */
	bad = 0;
	shown = 0;
	detail[0] = '\0';

	for (i = 0; i < lg->numareas; i++) {
		const LgArea *ar = &lg->areas[i];
		const f32 x0 = lg->originx + ar->x * LG_CELL;
		s32 cz;

		/* the -x side: the first cell edge with no opening (an upper-storey area's openings are in open2) */
		for (cz = ar->z; cz < ar->z + ar->d; cz++) {
			if (!(ar->x > 0 && (LG_OPENS(lg, ar->layer)[cz][ar->x - 1] & LG_OPEN_XP))) {
				break;
			}
		}

		if (cz >= ar->z + ar->d) {
			continue;
		}

		{
			const f32 z = lg->originz + cz * LG_CELL + LG_CELL / 2;

			if (!lgtLineBlocked(x0 + 40, z, i + 1, x0 - 40, z, modLevelGenFloorAt(lg, i, (s32)x0 + 40, (s32)z))) {
				if (shown++ < 5) {
					sysLogPrintf(LOG_NOTE, "levelgen test: %s: area %d's wall at x %.0f z %.0f lets a line through", tag, i, x0, z);
				}
				if (bad++ == 0) {
					snprintf(detail, sizeof(detail), "area %d's wall at x %.0f z %.0f", i, x0, z);
				}
			}
		}
	}

	snprintf(name, sizeof(name), "levelgen-load: %s every area's wall stops a line (%d areas)", tag, lg->numareas);
	lgtCheck(bad == 0, name, detail);
}

/** The Gauntlet's map pool: the generated level is in it, as its own kind, and can be ticked on and off. */
static void lgtCheckPool(void)
{
	const s32 count = modCareerPoolCount();
	s32 found = -1;
	s32 station = -1;
	bool retiredMaps = false;
	s32 i;
	bool was;

	for (i = 0; i < count; i++) {
		const s32 layout = LEVELGEN_POOLID_LAYOUT(modCareerPoolStage(i));

		if (layout == LG_LAYOUT_VILLA) {
			found = i;
		}
		if (layout == LG_LAYOUT_STATION && modCareerPoolKind(i) == MODCAREER_KIND_GEN
				&& !strcmp(modCareerPoolName(i), "(GEN) Switchback Station")) {
			station = i;
		}
		if (layout > LG_LAYOUT_BUNKER2) {
			retiredMaps = true;
		}
	}

	sysLogPrintf(LOG_NOTE, "levelgen test: gauntlet pool: %d maps, generated level at %d (%d of that kind)", count, found,
			modCareerKindCount(MODCAREER_KIND_GEN));
	lgtCheck(found >= 0 && modCareerPoolKind(found) == MODCAREER_KIND_GEN
			&& modCareerKindCount(MODCAREER_KIND_GEN) == LG_LAYOUT_COUNT - 1
			&& !strcmp(modCareerPoolName(found), "(GEN) Villa")
			&& station >= 0
			&& !retiredMaps,
			"levelgen-load: the gauntlet pool has Switchback Station and excludes retired layouts", "a layout is missing or incorrectly named");

	{
		const u32 seed = levelgenNewSeed();

		lgtCheck(seed >= 1 && seed <= 0x7fffffffu, "levelgen-load: a new level seed is in the Seed option's range", "out of range");
		lgtCheck(!strcmp(levelgenSizeName(LEVELGEN_SIZE_SMALL), "Small") && !strcmp(levelgenSizeName(LEVELGEN_SIZE_LARGE), "Large")
				&& g_LevelGenSizeSetting >= 0 && g_LevelGenSizeSetting <= 2, "levelgen-load: the size setting is a size", "bad size");
	}

	if (station >= 0) {
		was = modCareerPoolPicked(station);
		modCareerPoolSetPicked(station, true);
		lgtCheck(modCareerPoolPicked(station), "levelgen-load: Switchback Station can be ticked on", "still off");
		modCareerPoolSetPicked(station, false);
		lgtCheck(!modCareerPoolPicked(station), "levelgen-load: Switchback Station can be ticked off", "still on");
		modCareerPoolSetPicked(station, was);
	}
}

/**
 * The cover crates: the game's own crate props. Every crate the layout asked
 * for was made, with its model and size, and stands at the middle of the box
 * it replaced (a crate that is a little out would be drawn clear of the box
 * that stops bullets).
 */
static void lgtCheckCrates(const char *tag)
{
	const LgLayout *lg = levelgenGetLayout();
	s32 cars = 0;
	u32 *cmd = g_StageSetup.props;
	const LgObstacle *want[LG_MAXOBST];
	s32 numwant = 0;
	s32 found = 0, madeprops = 0, placed = 0, shaped = 0;
	char name[96];
	char detail[200];
	s32 i;

	if (lg == NULL || cmd == NULL) {
		return;
	}

	for (i = 0; i < lg->numobst; i++) {
		if (lg->obst[i].crate) {
			want[numwant++] = &lg->obst[i];
		}
	}

	while (((struct defaultobj *)cmd)->type != OBJTYPE_END) {
		struct defaultobj *obj = (struct defaultobj *)cmd;

		if (obj->type == OBJTYPE_BASIC && found < numwant) {
			const LgObstacle *o = want[found];
			const f32 floory = (f32)modLevelGenFloorAt(lg, o->area, o->x, o->z);
			// where its model's origin should be: a crate's middle; a car's under and behind its middle
			static const s8 dirx[4] = { 0, -1, 0, 1 }, dirz[4] = { 1, 0, -1, 0 };
			const LgPropDef *def = &g_LgProps[o->model < LG_PROP_MODELS ? o->model : 0];
			const bool car = o->model >= LG_CRATE_MODELS;
			const f32 wantx = (f32)(o->x - def->ahead * dirx[o->turn & 3]), wantz = (f32)(o->z - def->ahead * dirz[o->turn & 3]);
			const f32 wanty = floory + (car ? def->originy : o->height / 2);

			cars += car;

			if (obj->prop) {
				const struct coord *pos = &obj->prop->pos;

				madeprops++;

				if (pos->x > wantx - 4.0f && pos->x < wantx + 4.0f && pos->z > wantz - 4.0f && pos->z < wantz + 4.0f
						&& pos->y > wanty - 4.0f && pos->y < wanty + 6.0f) {
					placed++;
				} else if (found < 12 || car) {
					sysLogPrintf(LOG_NOTE, "levelgen test: %s: crate %d (model %d scale %d half %d) at (%.1f %.1f %.1f), box centre (%d %.0f %d), model scale %.3f, flags %x/%x/%x, rooms %d",
							tag, found, (int)o->model, (int)o->scale, (int)o->halfx, pos->x, pos->y, pos->z, (int)o->x, floory + o->height / 2, (int)o->z,
							obj->model ? obj->model->scale : -1.0f, (u32)obj->flags, (u32)obj->flags2, (u32)obj->flags3, (int)obj->prop->rooms[0]);
				}
			}

			if (car) {
				sysLogPrintf(LOG_NOTE, "levelgen test: %s: car (model %d turn %d) at (%.1f %.1f %.1f), wanted (%.1f %.1f %.1f), model scale %.3f",
						tag, (int)o->model, (int)o->turn, obj->prop ? obj->prop->pos.x : 0.0f, obj->prop ? obj->prop->pos.y : 0.0f, obj->prop ? obj->prop->pos.z : 0.0f,
						wantx, wanty, wantz, obj->model ? obj->model->scale : -1.0f);
			}

			if (obj->modelnum == (s16)def->model && obj->extrascale == (u16)o->scale) {
				shaped++;
			}

			found++;
		}

		cmd += setupGetCmdLength(cmd);
	}

	// the panes of glass: one for every window, each made, in its window
	{
		u32 *gc = g_StageSetup.props;
		s32 wantglass = 0, glass = 0, glassmade = 0, glassplaced = 0, gi, balconies = 0;

		for (gi = 0; gi < lg->numareas; gi++) {
			wantglass += lg->areas[gi].special == LG_SPECIAL_WINDOW;
			balconies += lg->areas[gi].special == LG_SPECIAL_BALCONY;
		}
		while (((struct defaultobj *)gc)->type != OBJTYPE_END) {
			struct defaultobj *obj = (struct defaultobj *)gc;

			if (obj->type == OBJTYPE_GLASS) {
				s32 k = glass, wi = -1;

				for (gi = 0; gi < lg->numareas; gi++) {
					if (lg->areas[gi].special == LG_SPECIAL_WINDOW && k-- == 0) {
						wi = gi;
						break;
					}
				}
				if (obj->prop) {
					glassmade++;
					if (wi >= 0) {
						const LgArea *w = &lg->areas[wi];
						const f32 x0 = (f32)(lg->originx + w->x * LG_CELL), z0 = (f32)(lg->originz + w->z * LG_CELL);
						const struct coord *pos = &obj->prop->pos;

						if (pos->x >= x0 && pos->x <= x0 + w->w * LG_CELL && pos->z >= z0 && pos->z <= z0 + w->d * LG_CELL
								&& pos->y >= w->floor - 1.0f && pos->y <= w->ceil + 1.0f) {
							glassplaced++;
						} else if (glass < 6) {
							sysLogPrintf(LOG_NOTE, "levelgen test: %s: pane %d at (%.1f %.1f %.1f), window %d x %.0f..%.0f y %d..%d z %.0f..%.0f",
									tag, glass, pos->x, pos->y, pos->z, wi, x0, x0 + w->w * LG_CELL, (int)w->floor, (int)w->ceil, z0, z0 + w->d * LG_CELL);
						}
					}
				}
				glass++;
			}
			gc += setupGetCmdLength(gc);
		}
		sysLogPrintf(LOG_NOTE, "levelgen test: %s: %d windows, %d panes in the setup, %d made, %d in their windows; %d balconies", tag, wantglass, glass, glassmade, glassplaced, balconies);
		if (lg->theme.layout == LG_LAYOUT_STREET) {
			snprintf(name, sizeof(name), "levelgen-load: %s every window has its pane of glass (%d)", tag, wantglass);
			snprintf(detail, sizeof(detail), "%d windows, %d panes, %d made, %d in place", wantglass, glass, glassmade, glassplaced);
			lgtCheck(wantglass > 0 && glass == wantglass && glassmade == wantglass && glassplaced == wantglass, name, detail);

			snprintf(name, sizeof(name), "levelgen-load: %s the street has balconies", tag);
			snprintf(detail, sizeof(detail), "%d balconies", balconies);
			lgtCheck(balconies > 0, name, detail);

			// out over each balcony's drop-down nothing stops a walker; across its railing the railing does
			{
				s32 pi, drops = 0, clear = 0, rails = 0, held = 0, bridged = 0, bridges = 0, bridgeclear = 0, gi;

				for (gi = 0; gi < lg->numareas; gi++) {
					const LgArea *g = &lg->areas[gi];
					const LgPortal *end[2] = { NULL, NULL };
					s32 ne = 0;

					if (g->special != LG_SPECIAL_BRIDGE) {
						continue;
					}
					bridges++;
					bridged += 2;
					for (pi = 0; pi < lg->numportals && ne < 2; pi++) {
						if ((lg->portals[pi].a == gi || lg->portals[pi].b == gi) && !lg->portals[pi].drop) {
							end[ne++] = &lg->portals[pi];
						}
					}
					if (ne == 2) {
						const f32 ax = (f32)(lg->originx + (end[0]->x0 + end[0]->x1) * LG_CELL / 2), az = (f32)(lg->originz + (end[0]->z0 + end[0]->z1) * LG_CELL / 2);
						const f32 bx = (f32)(lg->originx + (end[1]->x0 + end[1]->x1) * LG_CELL / 2), bz = (f32)(lg->originz + (end[1]->z0 + end[1]->z1) * LG_CELL / 2);
						const f32 len = sqrtf((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
						const f32 ux = (bx - ax) / len, uz = (bz - az) / len;
						const s32 from = end[0]->a == gi ? end[0]->b : end[0]->a;

						bridgeclear += !lgtLineBlockedBy(ax - ux * 70.0f, az - uz * 70.0f, from + 1, bx + ux * 70.0f, bz + uz * 70.0f, (f32)g->floor, GEOFLAG_WALL);
					}
				}

				for (pi = 0; pi < lg->numportals; pi++) {
					const LgPortal *pt = &lg->portals[pi];
					const s32 bal = lg->areas[pt->a].special == LG_SPECIAL_BALCONY ? pt->a : lg->areas[pt->b].special == LG_SPECIAL_BALCONY ? pt->b : -1;
					const s32 street = bal == pt->a ? pt->b : pt->a;

					if (bal >= 0 && pt->drop) {
						const bool alongz = pt->x0 == pt->x1;
						const f32 mx = (f32)(lg->originx + (pt->x0 + pt->x1) * LG_CELL / 2), mz = (f32)(lg->originz + (pt->z0 + pt->z1) * LG_CELL / 2);
						const f32 sign = pt->b == street ? 1.0f : -1.0f;
						const f32 ox = alongz ? sign : 0.0f, oz = alongz ? 0.0f : sign;
						const bool blocked = lgtLineBlockedBy(mx - ox * 70.0f, mz - oz * 70.0f, bal + 1, mx + ox * 150.0f, mz + oz * 150.0f,
								(f32)lg->areas[bal].floor, GEOFLAG_WALL);

						if (pt->norail) {
							drops++;
							clear += !blocked;
						} else {
							rails++;
							held += blocked;
						}
					}
				}
				snprintf(name, sizeof(name), "levelgen-load: %s nothing stops a walker off a balcony's drop-down", tag);
				snprintf(detail, sizeof(detail), "%d of %d clear", clear, drops);
				lgtCheck(drops >= balconies && clear == drops, name, detail);   // a bridged balcony has one either side of its bridge

				snprintf(name, sizeof(name), "levelgen-load: %s a balcony's railing stops a walker", tag);
				snprintf(detail, sizeof(detail), "%d of %d (%d balconies, %d bridged)", held, rails, balconies, bridged);
				// (every railed edge: since 2026-11-15 a balcony standing out over the street has its two ends railed too)
				lgtCheck(rails > 0 && held == rails, name, detail);

				// over every bridge from one balcony to the other nothing stops a walker
				snprintf(name, sizeof(name), "levelgen-load: %s nothing stops a walker across a bridge (%d)", tag, bridges);
				snprintf(detail, sizeof(detail), "%d of %d clear", bridgeclear, bridges);
				lgtCheck(bridges > 0 && bridgeclear == bridges, name, detail);
			}
		}
	}

	sysLogPrintf(LOG_NOTE, "levelgen test: %s: %d crates in the setup, %d wanted, %d made, %d in place, %d with the right model and size",
			tag, found, numwant, madeprops, placed, shaped);

	snprintf(name, sizeof(name), "levelgen-load: %s crates in the setup (%d)", tag, numwant);
	snprintf(detail, sizeof(detail), "%d crates, layout wants %d", found, numwant);
	lgtCheck(found == numwant && numwant > 0, name, detail);

	if (lg->theme.layout == LG_LAYOUT_STREET) {
		snprintf(name, sizeof(name), "levelgen-load: %s the street has parked cars", tag);
		snprintf(detail, sizeof(detail), "%d cars", cars);
		lgtCheck(cars > 0, name, detail);
	}

	snprintf(name, sizeof(name), "levelgen-load: %s every crate was made", tag);
	snprintf(detail, sizeof(detail), "%d of %d have a prop", madeprops, numwant);
	lgtCheck(madeprops == numwant, name, detail);

	snprintf(name, sizeof(name), "levelgen-load: %s every crate stands in its box", tag);
	snprintf(detail, sizeof(detail), "%d of %d", placed, numwant);
	lgtCheck(placed == numwant, name, detail);

	snprintf(name, sizeof(name), "levelgen-load: %s every crate has its model and size", tag);
	snprintf(detail, sizeof(detail), "%d of %d", shaped, numwant);
	lgtCheck(shaped == numwant, name, detail);
}

/**
 * The doors: every door the layout asked for was made (a prop), shut, and
 * found its own portal - door k is on the k-th doorway that has a door - and
 * stands at that portal's middle.
 */
static void lgtCheckDoors(const char *tag)
{
	const LgLayout *lg = levelgenGetLayout();
	char name[96];
	char detail[200];
	u32 *cmd = g_StageSetup.props;
	s32 want = 0;
	s32 found = 0;
	s32 madeprops = 0;
	s32 portalok = 0;
	s32 centred = 0;
	s32 shut = 0;
	s32 expected[LG_MAXPORTALS];
	s32 i;

	if (lg == NULL || cmd == NULL) {
		return;
	}

	for (i = 0; i < lg->numportals; i++) {
		if (lg->portals[i].door) {
			expected[want++] = i;
		}
	}

	while (((struct defaultobj *)cmd)->type != OBJTYPE_END) {
		struct defaultobj *obj = (struct defaultobj *)cmd;

		if (obj->type == OBJTYPE_DOOR && found < want) {
			struct doorobj *door = (struct doorobj *)obj;
			const LgPortal *pt = &lg->portals[expected[found]];
			struct coord centre;
			f32 cx = lg->originx + ((pt->x0 + pt->x1) * LG_CELL) / 2.0f;
			f32 cz = lg->originz + ((pt->z0 + pt->z1) * LG_CELL) / 2.0f;

			if (obj->prop) {
				madeprops++;
			}

			if (door->portalnum == expected[found]) {
				portalok++;
			} else if (found < 4) {
				sysLogPrintf(LOG_NOTE, "levelgen test: %s: door %d has portal %d, layout portal %d (rooms %d/%d) flags 0x%x",
						tag, found, (int)door->portalnum, expected[found], pt->a + 1, pt->b + 1, (u32)obj->flags);
			}

			if (door->frac == 0) {
				shut++;
			}

			padGetCentre(obj->pad, &centre);

			if (centre.x > cx - 3.0f && centre.x < cx + 3.0f && centre.z > cz - 3.0f && centre.z < cz + 3.0f) {
				centred++;
			} else {
				sysLogPrintf(LOG_NOTE, "levelgen test: %s: door %d centre (%.1f %.1f %.1f), portal %d middle (%.1f %.1f)",
						tag, found, centre.x, centre.y, centre.z, expected[found], cx, cz);
			}

			found++;
		} else if (obj->type == OBJTYPE_DOOR) {
			found++;
		}

		cmd += setupGetCmdLength(cmd);
	}

	sysLogPrintf(LOG_NOTE, "levelgen test: %s: %d doors in the setup, %d wanted, %d made, %d on their portal, %d centred, %d shut",
			tag, found, want, madeprops, portalok, centred, shut);

	snprintf(name, sizeof(name), "levelgen-load: %s doors in the setup (%d)", tag, want);
	snprintf(detail, sizeof(detail), "%d doors, layout wants %d", found, want);
	lgtCheck(found == want && want > 0, name, detail);

	snprintf(name, sizeof(name), "levelgen-load: %s every door was made", tag);
	snprintf(detail, sizeof(detail), "%d of %d have a prop", madeprops, want);
	lgtCheck(madeprops == want, name, detail);

	snprintf(name, sizeof(name), "levelgen-load: %s every door found its own portal", tag);
	snprintf(detail, sizeof(detail), "%d of %d", portalok, want);
	lgtCheck(portalok == want, name, detail);

	snprintf(name, sizeof(name), "levelgen-load: %s every door stands in the middle of its portal", tag);
	snprintf(detail, sizeof(detail), "%d of %d", centred, want);
	lgtCheck(centred == want, name, detail);

	snprintf(name, sizeof(name), "levelgen-load: %s every door starts shut", tag);
	snprintf(detail, sizeof(detail), "%d of %d", shut, want);
	lgtCheck(shut == want, name, detail);
}

/*
 * Brightness as the game has it: for the player's room and every room on
 * screen, its settled local and regional brightness and its range. Logged
 * with each screenshot, to tell what the picture shows.
 */
static void lgtLogBrightness(const char *tag, s32 shot)
{
	char line[1024];
	s32 len = 0;
	s32 room;
	s32 playerroom = (g_Vars.bond && g_Vars.bond->prop) ? g_Vars.bond->prop->rooms[0] : -1;

	len += snprintf(line + len, sizeof(line) - len, "levelgen test: %s shot %d: player room %d; on screen (room local/regional/max lights calced):",
			tag, shot, playerroom);

	for (room = 1; room < g_Vars.roomcount && len < (s32)sizeof(line) - 40; room++) {
		if ((g_Rooms[room].flags & ROOMFLAG_ONSCREEN) || room == playerroom) {
			u8 brmin = 0, brmax = 0;

			bgGetRoomBrightnessRange(room, &brmin, &brmax);
			len += snprintf(line + len, sizeof(line) - len, " %d:%d/%d/%d/%d/%d", room,
					g_Rooms[room].br_settled_local, g_Rooms[room].br_settled_regional, brmax,
					g_Rooms[room].numlights, (g_Rooms[room].flags & ROOMFLAG_BRIGHTNESS_CALCED) != 0);
		}
	}

	sysLogPrintf(LOG_NOTE, "%s", line);
}

/*
 * Turns the view for the screenshots and asks for each one in turn.
 */
static void lgtTickShots(const char *tag)
{
	const s32 frame = g_Vars.lvframenum;
	s32 i;

	if (g_Vars.bond == NULL || frame < LGT_SHOT0 - LGT_SHOTGAP / 2) {
		return;
	}

	for (i = 0; i < LGT_NUMSHOTS; i++) {
		const s32 at = LGT_SHOT0 + i * LGT_SHOTGAP;

		if (frame >= at - LGT_SHOTGAP / 2 && frame < at + LGT_SHOTGAP / 2) {
			// held every frame from half a gap before the shot, so the camera has settled
			g_Vars.bond->vv_theta = g_LgtShotView[i][0];
			g_Vars.bond->vv_verta = g_LgtShotView[i][1];

			if (frame >= at && (g_LgtShotsTaken & (1u << i)) == 0) {
				char path[160];

				g_LgtShotsTaken |= 1u << i;
				snprintf(path, sizeof(path), "source/test-results/levelgen-shot-%d-%d.bmp", g_LgtLevel, i);
				gfx_request_screenshot(path);
				sysLogPrintf(LOG_NOTE, "levelgen test: %s: screenshot %s (yaw %.0f pitch %.0f)", tag, path,
						g_LgtShotView[i][0], g_LgtShotView[i][1]);
				lgtLogBrightness(tag, i);
			}
		}
	}
}

/*
 * The level from above (modlevelgenmap.c) with where everyone is: the player
 * a white ring, each live guard a cross. Written to source/test-results/ for
 * Claude to look at with the screenshots.
 */
static void lgtWriteMap(const char *tag)
{
	const LgLayout *lg = levelgenGetLayout();
	LgImage img;
	char path[96];
	s32 i;
	s32 guards = 0;

	if (lg == NULL || !modLevelGenMapRender(lg, 0, 1, &img)) {
		return;
	}

	for (i = 0; i < chrsGetNumSlots(); i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr->model && chr->prop && chr->prop->type == PROPTYPE_CHR
				&& (!g_Vars.bond || chr->prop != g_Vars.bond->prop) && !chrIsDead(chr)) {
			modLevelGenMapMark(lg, &img, 0, 1, (s32)chr->prop->pos.x, (s32)chr->prop->pos.z, 0xff2020, 0);
			guards++;
		}
	}

	if (g_Vars.bond && g_Vars.bond->prop) {
		modLevelGenMapMark(lg, &img, 0, 1, (s32)g_Vars.bond->prop->pos.x, (s32)g_Vars.bond->prop->pos.z, 0xffffff, 1);
	}

	snprintf(path, sizeof(path), "source/test-results/levelgen-map-%d.png", g_LgtLevel);
	sysLogPrintf(LOG_NOTE, "levelgen test: %s: map with the player and %d guards: %s%s", tag, guards, path,
			modLevelGenImageWritePng(&img, path) ? "" : " (not written)");
	modLevelGenImageFree(&img);
}

/*
 * The lights: every room has the lights the layout gave it, a brightness
 * ceiling of its own, the lights the seed breaks are out and the rest are on,
 * and the room's brightness follows them. Shooting a light out dims the room.
 */
static void lgtCheckLights(const char *tag)
{
	const LgLayout *lg = levelgenGetLayout();
	char name[96];
	char detail[200];
	s32 room;
	s32 badcount = 0, badbright = 0, badstate = 0, brokenwant = 0, brokenhave = 0, totallights = 0;

	if (lg == NULL) {
		return;
	}

	for (room = 1; room < g_Vars.roomcount && room <= lg->numareas; room++) {
		u8 brmin = 0, brmax = 0;
		s32 j;

		if (g_Rooms[room].numlights != modLevelGenAreaLights(lg, room - 1)) {
			badcount++;
		}

		bgGetRoomBrightnessRange(room, &brmin, &brmax);

		if (brmin != 128 || brmax != modLevelGenAreaBrightness(lg, room - 1)) {
			badbright++;
		}

		for (j = 0; j < g_Rooms[room].numlights; j++) {
			const bool want = modLevelGenLightBroken(lg, room - 1, j) != 0;

			totallights++;
			brokenwant += want;
			brokenhave += !lightIsHealthy(room, j);

			if (want == lightIsHealthy(room, j) || (!want && !lightIsOn(room, j))) {
				badstate++;
			}
		}
	}

	sysLogPrintf(LOG_NOTE, "levelgen test: %s: %d lights, %d start broken", tag, totallights, brokenhave);

	snprintf(name, sizeof(name), "levelgen-load: %s every room has its layout's lights", tag);
	snprintf(detail, sizeof(detail), "%d rooms with the wrong light count", badcount);
	lgtCheck(badcount == 0 && totallights > 0, name, detail);

	snprintf(name, sizeof(name), "levelgen-load: %s every room has its brightness range", tag);
	snprintf(detail, sizeof(detail), "%d rooms with the wrong range", badbright);
	lgtCheck(badbright == 0, name, detail);

	snprintf(name, sizeof(name), "levelgen-load: %s the seed's broken lights are out, the others on", tag);
	snprintf(detail, sizeof(detail), "%d lights in the wrong state (%d should be broken, %d are)", badstate, brokenwant, brokenhave);
	lgtCheck(badstate == 0, name, detail);
}

/**
 * The pillars: a line that crosses one is stopped, from each of its four sides,
 * and a line that passes by it, a cell away, is not.
 */
static void lgtCheckPillars(const char *tag)
{
	const LgLayout *lg = levelgenGetLayout();
	char name[96];
	char detail[200];
	s32 i;
	s32 stopped = 0;
	s32 passed = 0;
	s32 bad = 0;
	s32 shotok = 0;
	s32 rails = 0;

	if (lg == NULL) {
		return;
	}

	for (i = 0; i < lg->numobst; i++) {
		const LgObstacle *o = &lg->obst[i];
		const LgArea *a = &lg->areas[o->area];
		const f32 px = o->x;
		const f32 pz = o->z;
		const s32 room = o->area + 1;
		const f32 rx = o->halfx + 30.0f, rz = o->halfz + 30.0f;
		const f32 fy = modLevelGenFloorAt(lg, o->area, o->x, o->z);   // a gallery's or terrace's raised floor
		bool ok = lgtLineBlocked(px - rx, pz, room, px + rx, pz, fy)
				&& lgtLineBlocked(px + rx, pz, room, px - rx, pz, fy)
				&& lgtLineBlocked(px, pz - rz, room, px, pz + rz, fy)
				&& lgtLineBlocked(px, pz + rz, room, px, pz - rz, fy);

		// shots and sight: stopped by a pillar, a crate or a wall, and not by a railing
		{
			const bool shot = lgtLineBlockedBy(px - rx, pz, room, px + rx, pz, fy, GEOFLAG_BLOCK_SHOOT)
					|| lgtLineBlockedBy(px, pz - rz, room, px, pz + rz, fy, GEOFLAG_BLOCK_SHOOT);
			const bool sight = lgtLineBlockedBy(px - rx, pz, room, px + rx, pz, fy, GEOFLAG_BLOCK_SIGHT)
					|| lgtLineBlockedBy(px, pz - rz, room, px, pz + rz, fy, GEOFLAG_BLOCK_SIGHT);

			rails += o->rail != 0;

			if (o->rail ? (!shot && !sight) : (shot && sight)) {
				shotok++;
			} else if (bad++ < 3) {
				sysLogPrintf(LOG_NOTE, "levelgen test: %s: obstacle %d (%s) at (%.0f %.0f): shots %s, sight %s", tag, i,
						o->rail ? "railing" : "solid", px, pz, shot ? "stopped" : "pass", sight ? "stopped" : "pass");
			}
		}

		if (ok) {
			stopped++;
		} else if (bad++ < 3) {
			sysLogPrintf(LOG_NOTE, "levelgen test: %s: a line through obstacle %d at (%.0f %.0f) in room %d is not stopped", tag, i, px, pz, room);
		}

		// a line that misses it, along its long side, 45 units off it, on the side
		// towards the middle of its area (a parapet stands at a ledge)
		{
			const f32 acx = lg->originx + (a->x + a->w / 2.0f) * LG_CELL, acz = lg->originz + (a->z + a->d / 2.0f) * LG_CELL;
			bool clear;

			if (o->rail && a->special == LG_SPECIAL_BALCONY) {
				// (a balcony's railings meet at its corners since 2026-11-15: a line inside one
				// runs into the other)
				clear = true;
			} else if (o->halfx >= o->halfz) {
				const f32 lz = pz + (acz >= pz ? 1.0f : -1.0f) * (o->halfz + 45.0f);
				clear = !lgtLineBlocked(px - rx, lz, room, px + rx, lz, fy);
			} else {
				const f32 lx = px + (acx >= px ? 1.0f : -1.0f) * (o->halfx + 45.0f);
				clear = !lgtLineBlocked(lx, pz - rz, room, lx, pz + rz, fy);
			}
			if (clear) {
				passed++;
			}
		}
	}

	sysLogPrintf(LOG_NOTE, "levelgen test: %s: %d pillars, %d stop a line from all four sides, %d lines beside them pass",
			tag, lg->numobst, stopped, passed);

	snprintf(name, sizeof(name), "levelgen-load: %s has pillars (%d)", tag, lg->numobst);
	lgtCheck(lg->numobst > 0, name, "the layout made none");

	snprintf(name, sizeof(name), "levelgen-load: %s every pillar stops a line", tag);
	snprintf(detail, sizeof(detail), "%d of %d stop it", stopped, lg->numobst);
	lgtCheck(stopped == lg->numobst, name, detail);

	snprintf(name, sizeof(name), "levelgen-load: %s railings are shot and seen through, the rest is not (%d railings)", tag, rails);
	snprintf(detail, sizeof(detail), "%d of %d as they should be", shotok, lg->numobst);
	lgtCheck(shotok == lg->numobst, name, detail);

	snprintf(name, sizeof(name), "levelgen-load: %s a line beside a pillar is not stopped", tag);
	snprintf(detail, sizeof(detail), "%d of %d pass", passed, lg->numobst);
	lgtCheck(passed == lg->numobst, name, detail);
}

static void lgtCheckPlayer(const char *tag, s32 level)
{
	char name[96];
	char detail[200];
	struct player *p = g_Vars.bond;
	f32 x, y, z;
	bool ok;

	if (!p || !p->prop) {
		snprintf(name, sizeof(name), "levelgen-load: %s player exists", tag);
		lgtCheck(false, name, "no player prop");
		return;
	}

	x = p->prop->pos.x;
	y = p->prop->pos.y;
	z = p->prop->pos.z;

	sysLogPrintf(LOG_NOTE, "levelgen test: %s: player at (%.1f %.1f %.1f) room %d, health %.2f, ground %.1f, %d guards",
			tag, x, y, z, (int)p->prop->rooms[0], p->bondhealth, p->vv_ground, lgtCountGuards());

	ok = x > -g_LgtHalf[level][0] && x < g_LgtHalf[level][0] && z > -g_LgtHalf[level][1] && z < g_LgtHalf[level][1]
			&& y > -50.0f && y < 1200.0f && p->bondhealth > 0.0f; // up to the hub's ceiling (a terrace start is high)
	snprintf(name, sizeof(name), "levelgen-load: %s player alive inside the level", tag);
	snprintf(detail, sizeof(detail), "at (%.1f %.1f %.1f) health %.2f, level half sizes %d x %d",
			x, y, z, p->bondhealth, g_LgtHalf[level][0], g_LgtHalf[level][1]);
	lgtCheck(ok, name, detail);
}

/* ------------------------------------------------------------------------
 * The room tour: --levelgen-tour SEED [--levelgen-size 0|1|2]
 *
 * Starts the level, then puts the player in every area in turn (rooms, the
 * hub's floor and terraces, side-room terraces, corridors), turns him to four
 * directions in each and saves a small picture per view in
 * source/test-results/tour-SEED-SIZE/ along with manifest.txt (which view is
 * which area, room, height and position, and where the player really ended
 * up) and map.png (the level from above with every stop marked).
 * tools/levelgen/lgtour.py turns that folder into labelled contact sheets.
 * ------------------------------------------------------------------------ */

bool g_LevelGenTour = false; // --levelgen-tour

#define LGT_MAXSTOPS   1200
#define LGT_TOURSETTLE 30     // frames after a turn before the picture
#define LGT_TOURGAP    33     // frames per stop
#define LGT_TOURWIDTH  480    // pictures are this many pixels across at most
#define LGT_TOURRETRY  2

typedef struct {
	s16 area;
	s16 view;     // 0..3 the four turns, 4.. extra views
	f32 x, floor, z;
	f32 yaw, pitch;
} LgtStop;

static LgtStop g_LgtStops[LGT_MAXSTOPS];
static s32 g_LgtNumStops = 0;
static s32 g_LgtStopIdx = 0;
static s32 g_LgtStopFrame = 0;
static s32 g_LgtStopTries = 0;
static s32 g_LgtTourPhase = 0;       // 0 boot, 1 waiting for the level, 2 touring
static s32 g_LgtTourPlaced = 0;
static s32 g_LgtTourShots = 0;
static bool g_LgtTourAreaOk[LG_MAXAREAS];
static bool g_LgtTourAreaSeen[LG_MAXAREAS];
static char g_LgtTourDir[128];
static char g_LgtTourManifest[160];
static s32 g_LgtTourSize = 1;
static u32 g_LgtTourSeed = 0;
static f32 g_LgtTourLastX = -1e9f, g_LgtTourLastZ = -1e9f, g_LgtTourLastFloor = -1e9f;
static bool g_LgtTourPlacedNow = false;
static s32 g_LgtTourRoomNow = -1;
static s32 g_LgtTourStoreyStops = 0;   // stops on a mezzanine, under it, or on its ramp or bay
static s32 g_LgtTourStoreyBad = 0;
static s32 g_LgtTourGlassStops = 0;    // stops on a glass floor
static s32 g_LgtTourGlassHeld = 0;     // of those, the ones where the panes held the player up     // of those, the ones where the game put the player in another room

static void lgtTourAdd(s32 area, s32 view, f32 x, f32 floory, f32 z, f32 yaw, f32 pitch)
{
	LgtStop *s;

	if (g_LgtNumStops >= LGT_MAXSTOPS) {
		return;
	}

	s = &g_LgtStops[g_LgtNumStops++];
	s->area = area;
	s->view = view;
	s->x = x;
	s->floor = floory;
	s->z = z;
	s->yaw = yaw;
	s->pitch = pitch;
}

/* where in the area to stand: the centre pad when it has one, else the middle,
 * stepped aside from anything standing there */
static void lgtTourSpot(const LgLayout *lg, s32 ai, f32 *outx, f32 *outz)
{
	const LgArea *a = &lg->areas[ai];
	f32 x = (f32)(lg->originx + a->x * LG_CELL + a->w * LG_CELL / 2);
	f32 z = (f32)(lg->originz + a->z * LG_CELL + a->d * LG_CELL / 2);
	static const s32 steps[9][2] = { {0,0}, {1,0}, {-1,0}, {0,1}, {0,-1}, {1,1}, {-1,-1}, {1,-1}, {-1,1} };
	s32 k, o;

	if (a->centrepad >= 0 && a->centrepad < lg->numpads && lg->pads[a->centrepad].area == ai) {
		*outx = (f32)lg->pads[a->centrepad].x;
		*outz = (f32)lg->pads[a->centrepad].z;
		return;
	}

	for (k = 0; k < 9; k++) {
		const f32 tx = x + steps[k][0] * 120.0f;
		const f32 tz = z + steps[k][1] * 120.0f;
		bool clear = tx > lg->originx + a->x * LG_CELL + 60 && tx < lg->originx + (a->x + a->w) * LG_CELL - 60
				&& tz > lg->originz + a->z * LG_CELL + 60 && tz < lg->originz + (a->z + a->d) * LG_CELL - 60;

		for (o = 0; clear && o < lg->numobst; o++) {
			const LgObstacle *ob = &lg->obst[o];

			if (ob->area == ai && fabsf(tx - ob->x) < ob->halfx + 50 && fabsf(tz - ob->z) < ob->halfz + 50) {
				clear = false;
			}
		}

		if (clear) {
			*outx = tx;
			*outz = tz;
			return;
		}
	}

	*outx = x;
	*outz = z;
}

/* LGMEASURE=1 (2026-11-16): each candidate prop model's bounding box, at its
 * model scale, in world units (what g_LgProps is filled from) */
static void lgtMeasureProps(void)
{
	static const s32 models[] = {
		0x03, 0x20, 0x21, 0x22, 0x175, 0x5a, 0x193, 0xc1, 0xe2, 0x23, 0x25, 0xba, 0x5e, 0x1a7, 0xac, 0x12d, 0x67, 0xec, 0x135,
		0x17f, 0xe6, 0xe7, 0xe8, 0xe9, 0x131, 0x132, 0x133, 0x134, 0x1b5, 0xb6, 0x86, 0x87, 0x127, 0x53, 0x54, 0x50, 0x52, 0x51,
		0x3a, 0x97, 0x85, 0x5b, 0x128, 0xc8, 0x76, 0xeb, 0xea, 0x33, 0x46, 0x47, 0x5f, 0x40, 0x41, 0x171, 0x172, 0x62, 0x63, 0x66,
		0x68, 0x65, 0x173, 0x174, 0x161, 0x162, 0x166, 0x160, 0xc4, 0x165, 0x163, 0x164, 0x15d, 0x15e, 0x158, 0x159, 0x16f, 0xbc,
		0xbb, 0x16a, 0x168, 0x169, 0x16b, 0x17b, 0x167, 0x16d, 0x16c, 0x16e, 0x13c, 0x185, 0x1a2, 0xf1, 0x32, 0x2f, 0x30, 0x31,
		0x3c, 0x3d, 0x3e, 0x59, 0x156, 0x14f, 0x155, 0x150, 0x17, 0xb0, 0x1aa, 0xb2, 0x12a, 0x9f, 0xbe, 0x129, 0xb1, 0xbd, 0x126,
		0x137, 0x146, 0x8c, 0x8d, 0x12c, 0x79, 0x7a, 0x143, 0x38, 0x55, 0x57, 0x39, 0x16, 0xdb, 0xdc, 0xdd, 0x18c, 0xca, 0xcb,
		0x1, 0x124, 0x196, 0x188, 0x64, 0x2, 0xe3,
	};
	s32 i, extra[32], nextra = 0;
	const char *list = getenv("LGMEASURELIST");

	while (list && *list && nextra < 32) {   // (models to measure instead, hex, comma-separated)
		char *end;

		extra[nextra++] = (s32)strtol(list, &end, 16);
		list = *end == ',' ? end + 1 : end;
		if (end == list && *end != ',') break;
	}
	for (i = 0; i < (nextra ? nextra : (s32)ARRAYCOUNT(models)); i++) {
		const s32 m = nextra ? extra[i] : models[i];
		struct modeldef *def;
		struct modelnode *node;
		struct modelrodata_bbox *bbox = NULL;
		f32 sc;

		setupLoadModeldef(m);
		def = g_ModelStates[m].modeldef;
		if (!def) {
			sysLogPrintf(LOG_NOTE, "LGMEASURE %#x: no model", m);
			continue;
		}
		node = def->rootnode;
		while (node) {
			if ((node->type & 0xff) == MODELNODETYPE_BBOX) {
				bbox = &node->rodata->bbox;
				break;
			}
			if (node->child) {
				node = node->child;
			} else {
				while (node && !node->next) {
					node = node->parent;
				}
				node = node ? node->next : NULL;
			}
		}
		sc = g_ModelStates[m].scale * (1.0f / 4096.0f);
		if (!bbox) {
			sysLogPrintf(LOG_NOTE, "LGMEASURE %#x: no box", m);
			continue;
		}
		sysLogPrintf(LOG_NOTE, "LGMEASURE %#x: x %.0f %.0f y %.0f %.0f z %.0f %.0f (scale %.4f)", m,
				bbox->xmin * sc, bbox->xmax * sc, bbox->ymin * sc, bbox->ymax * sc, bbox->zmin * sc, bbox->zmax * sc, sc);
	}
}

static void lgtTourBuild(const LgLayout *lg)
{
	if (getenv("LGMEASURE")) {
		lgtMeasureProps();
	}

	s32 ai, hubmain = -1, best = 0;

	g_LgtNumStops = 0;

	// the lifts (2026-11-17): each shaft looked at from every storey, 4 cells
	// out from each side, a little up (view 19), so the pictures show the
	// platform wherever it is on its way
	for (ai = 0; ai < lg->numlifts; ai++) {
		const LgArea *sh = &lg->areas[lg->lifts[ai].area];
		const f32 cx = (f32)(lg->originx + sh->x * LG_CELL + sh->w * LG_CELL / 2), cz = (f32)(lg->originz + sh->z * LG_CELL + sh->d * LG_CELL / 2);
		s32 layer, side;

		for (layer = 0; layer < LG_MAXLAYERS; layer++) {
			for (side = 0; side < 4; side++) {
				const f32 d = sh->w * LG_CELL * 0.5f + 300.0f;
				const f32 px = cx + (side == 0 ? -d : side == 1 ? d : 0.0f), pz = cz + (side == 2 ? -d : side == 3 ? d : 0.0f);
				const s32 gx = (s32)((px - lg->originx) / LG_CELL), gz = (s32)((pz - lg->originz) / LG_CELL);
				const f32 yaw = side == 0 ? 270.0f : side == 1 ? 90.0f : side == 2 ? 0.0f : 180.0f;
				s32 at;

				if (gx < 0 || gz < 0 || gx >= lg->gw || gz >= lg->gd) {
					continue;
				}
				at = LG_CELLS(lg, layer)[gz][gx] - 1;
				if (at >= 0 && !lg->areas[at].ramp && !LG_NOWALK(&lg->areas[at]) && !lg->areas[at].special) {
					lgtTourAdd(at, 19, px, (f32)lg->areas[at].floor, pz, yaw, layer == 2 ? -10.0f : 20.0f);
				}
			}
		}
	}

	// the pipeworks (2026-11-19): each ladder from 220 out at its foot, looking
	// at it (view 20); and the hall over the sludge from its four sides: from
	// the north side's high middle looking south and down (21), from the
	// gantry looking north (22), from the west's middle floor looking east
	// (23), from the east's low floor beside the pipes looking west (24)
	// the mall's washroom (2026-11-19): each toilet, from the stall's mouth (view 25)
	{
		s32 oi, shown = 0;

		for (oi = 0; oi < lg->numobst && shown < 2; oi++) {
			const LgObstacle *o = &lg->obst[oi];
			static const f32 fx[4] = { 0, -1, 0, 1 }, fz[4] = { 1, 0, -1, 0 };
			static const f32 yawv[4] = { 180.0f, 270.0f, 0.0f, 90.0f };

			if (lg->theme.layout == LG_LAYOUT_MALL && o->crate && o->model == LG_PROP_TOILET) {
				lgtTourAdd(o->area, 25, (f32)o->x + fx[o->turn & 3] * 125.0f, (f32)lg->areas[o->area].floor, (f32)o->z + fz[o->turn & 3] * 125.0f, yawv[o->turn & 3], -12.0f);
				shown++;
			}
		}
	}

	if (lg->theme.layout == LG_LAYOUT_PIPEWORKS) {
		s32 pit = -1, hi = -1, k;

		for (ai = 0; ai < lg->numareas; ai++) {
			if (lg->areas[ai].special == LG_SPECIAL_PIT && (pit < 0 || lg->areas[ai].w * lg->areas[ai].d > lg->areas[pit].w * lg->areas[pit].d)) {
				pit = ai;
			}
			if (lg->areas[ai].layer == 2 && hi < 0) {
				hi = ai;
			}
		}
		for (k = 0; k < lg->numladders; k++) {
			const LgLadder *ld = &lg->ladders[k];
			const f32 face = (f32)((ld->alongz ? lg->originx : lg->originz) + ld->line * LG_CELL);
			const f32 mid = (f32)((ld->alongz ? lg->originz : lg->originx) + ld->at * LG_CELL + LG_CELL / 2);
			const f32 sgn = ld->facing == 0 || ld->facing == 2 ? -1.0f : 1.0f;
			const f32 px = ld->alongz ? face + sgn * 220.0f : mid, pz = ld->alongz ? mid : face + sgn * 220.0f;
			// facing the face: from -x look +x (270), from +x look -x (90), from -z look +z (0), from +z look -z (180)
			const f32 yaw = ld->facing == 0 ? 270.0f : ld->facing == 1 ? 90.0f : ld->facing == 2 ? 0.0f : 180.0f;

			lgtTourAdd(ld->low, 20, px, (f32)lg->areas[ld->low].floor, pz, yaw, 18.0f);
		}
		if (pit >= 0) {
			const LgArea *pa = &lg->areas[pit];
			const f32 x0 = (f32)(lg->originx + pa->x * LG_CELL), z0 = (f32)(lg->originz + pa->z * LG_CELL);
			const f32 x1 = x0 + pa->w * LG_CELL, z1 = z0 + pa->d * LG_CELL;
			const f32 hx = hi >= 0 ? (f32)(lg->originx + lg->areas[hi].x * LG_CELL + LG_CELL) : (x0 + x1) / 2;
			const f32 spots[4][5] = {
				{ (x0 + x1) / 2, z1 + 60.0f, 180.0f, -24.0f, 21 },
				{ hx, z0 - 150.0f, 0.0f, -18.0f, 22 },
				{ x0 - 100.0f, z0 + (z1 - z0) * 0.78f, 270.0f, -14.0f, 23 },
				{ x1 + 100.0f, z0 + (z1 - z0) * 0.22f, 90.0f, -6.0f, 24 },
			};

			for (k = 0; k < 4; k++) {
				const s32 gx = (s32)((spots[k][0] - lg->originx) / LG_CELL), gz = (s32)((spots[k][1] - lg->originz) / LG_CELL);
				s32 at;

				if (gx < 0 || gz < 0 || gx >= lg->gw || gz >= lg->gd) {
					continue;
				}
				at = lg->cells[gz][gx] - 1;
				if (at >= 0 && !lg->areas[at].ramp && !LG_NOWALK(&lg->areas[at])) {
					lgtTourAdd(at, (s32)spots[k][4], spots[k][0], (f32)lg->areas[at].floor, spots[k][1], spots[k][2], spots[k][3]);
				}
			}
		}
	}

	// the rooftops' towers (2026-11-16): each looked at from its roof, 5 cells
	// out from the middle of each side, looking a little up (view 14)
	if (lg->theme.layout == LG_LAYOUT_ROOFTOP) {
		s32 rm;

		// Look up at the dish from both sides of the relay block.
		for (s32 i = 0; i < lg->numobst; i++) {
			const LgObstacle *dish = &lg->obst[i];
			if (!dish->crate || dish->model != LG_PROP_DISH) continue;

			for (s32 side = -1; side <= 1; side += 2) {
				for (s32 distance = 15; distance >= 6; distance--) {
					const f32 x = dish->x + side * distance * LG_CELL;
					const s32 gx = (x - lg->originx) / LG_CELL, gz = (dish->z - lg->originz) / LG_CELL;
					if (gx < 0 || gx >= lg->gw) continue;

					const s32 area = lg->cells[gz][gx] - 1;
					if (area >= 0 && lg->areas[area].open && !lg->areas[area].ramp && !LG_NOWALK(&lg->areas[area])) {
						lgtTourAdd(area, 14, x, lg->areas[area].floor, dish->z, side > 0 ? 90.0f : 270.0f,
								distance > 10 ? 36.0f : 56.0f);
						break;
					}
				}
			}
			break;
		}

		for (rm = 0; rm < lg->numrooms; rm++) {
			s32 x0 = 9999, z0 = 9999, x1 = -1, z1 = -1, tall = 0, side;

			for (ai = 0; ai < lg->numareas; ai++) {
				const LgArea *a = &lg->areas[ai];

				if (a->room == rm && !a->open) {
					tall |= a->layer == 2;
					x0 = a->x < x0 ? a->x : x0;
					z0 = a->z < z0 ? a->z : z0;
					x1 = a->x + a->w > x1 ? a->x + a->w : x1;
					z1 = a->z + a->d > z1 ? a->z + a->d : z1;
				}
			}
			for (side = 0; tall && side < 4; side++) {
				static const f32 yaws[4] = { 90.0f, 180.0f, 270.0f, 0.0f };
				s32 out, gx = -1, gz = -1;

				// as far out as the roof goes, up to 12 cells
				for (out = 12; out >= 3; out--) {
					gx = side == 0 ? x1 + out : side == 2 ? x0 - 1 - out : (x0 + x1) / 2;
					gz = side == 1 ? z1 + out : side == 3 ? z0 - 1 - out : (z0 + z1) / 2;
					if (gx >= 0 && gz >= 0 && gx < lg->gw && gz < lg->gd && lg->cells[gz][gx] > 0
							&& lg->areas[lg->cells[gz][gx] - 1].open && !LG_NOWALK(&lg->areas[lg->cells[gz][gx] - 1])
							&& lg->areas[lg->cells[gz][gx] - 1].floor >= 1600) {
						break;
					}
				}

				if (out >= 3) {
					const s32 under = lg->cells[gz][gx] - 1;
					const f32 px = (f32)(lg->originx + gx * LG_CELL + LG_CELL / 2), pz = (f32)(lg->originz + gz * LG_CELL + LG_CELL / 2);

					lgtTourAdd(under, 14, px, (f32)modLevelGenFloorAt(lg, under, (s32)px, (s32)pz), pz, yaws[side], 12.0f);
				}
			}
		}
	}

	for (ai = 0; ai < lg->numareas; ai++) {
		f32 x, z, fy;
		s32 v;

		if (lg->areas[ai].special == LG_SPECIAL_GLASS) {
			// a glass floor over a drop (the rooftops, 2026-11-16): stood on in its middle, looking
			// down through it (view 13); the player must stay up on the panes
			const LgArea *g = &lg->areas[ai];

			lgtTourAdd(ai, 13, (f32)(lg->originx + g->x * LG_CELL + g->w * LG_CELL / 2), (f32)g->floor,
					(f32)(lg->originz + g->z * LG_CELL + g->d * LG_CELL / 2), 0.0f, 60.0f);
			continue;
		}
		if (LG_NOWALK(&lg->areas[ai])) {
			continue;   // (a ravine's floor or a warehouse's rack: nobody stands there)
		}
		if (lg->areas[ai].special == LG_SPECIAL_WINDOW && lg->areas[ai].layer) {
			// an upper-storey window (2026-11-15) is also looked up at from across the street,
			// 500 back from its opening, so the picture shows the building's front (view 12)
			const LgArea *w = &lg->areas[ai];
			const f32 wx = (f32)(lg->originx + w->x * LG_CELL + w->w * LG_CELL / 2);
			const f32 wz = (f32)(lg->originz + w->z * LG_CELL + w->d * LG_CELL / 2);
			s32 pi;

			for (pi = 0; pi < lg->numportals; pi++) {
				const LgPortal *p = &lg->portals[pi];
				const s32 other = p->a == ai ? p->b : p->b == ai ? p->a : -1;

				if (other >= 0 && (lg->areas[other].flags & LG_AREAFLAG_HUB)) {
					const bool alongz = p->x0 == p->x1;
					const f32 line = (f32)(alongz ? lg->originx + p->x0 * LG_CELL : lg->originz + p->z0 * LG_CELL);
					const f32 sign = (alongz ? wx : wz) > line ? -1.0f : 1.0f;
					const f32 px = alongz ? line + sign * 500.0f : wx, pz = alongz ? wz : line + sign * 500.0f;
					const f32 yaw = alongz ? (sign > 0 ? 90.0f : 270.0f) : (sign > 0 ? 180.0f : 0.0f);
					const s32 gx = (s32)((px - lg->originx) / LG_CELL), gz = (s32)((pz - lg->originz) / LG_CELL);

					if (gx >= 0 && gz >= 0 && gx < lg->gw && gz < lg->gd && lg->cells[gz][gx] - 1 == other) {
						lgtTourAdd(other, 12, px, (f32)modLevelGenFloorAt(lg, other, (s32)px, (s32)pz), pz, yaw, 15.0f);
					}
					break;
				}
			}
		}
		if (lg->areas[ai].special == LG_SPECIAL_WINDOW) {
			// nobody stands in a window: it is looked at from the room and from the street,
			// 180 back from its opening on each side (views 8 and 9)
			const LgArea *w = &lg->areas[ai];
			const f32 wx = (f32)(lg->originx + w->x * LG_CELL + w->w * LG_CELL / 2);
			const f32 wz = (f32)(lg->originz + w->z * LG_CELL + w->d * LG_CELL / 2);
			s32 pi, nview = 8;

			g_LgtTourAreaOk[ai] = true;
			for (pi = 0; pi < lg->numportals && nview < 10; pi++) {
				const LgPortal *p = &lg->portals[pi];
				const s32 other = p->a == ai ? p->b : p->b == ai ? p->a : -1;

				if (other >= 0 && lg->areas[other].special == LG_SPECIAL_PIT) {
					continue;   // (a highrise's window over the street far below: nobody stands down there, 2026-11-17)
				}
				if (other >= 0) {
					const bool alongz = p->x0 == p->x1;   // the opening runs along z: it is looked through along x
					const f32 line = (f32)(alongz ? lg->originx + p->x0 * LG_CELL : lg->originz + p->z0 * LG_CELL);
					const f32 sign = (alongz ? wx : wz) > line ? -1.0f : 1.0f;   // from the window out to the other area
					const f32 px = alongz ? line + sign * 180.0f : wx, pz = alongz ? wz : line + sign * 180.0f;
					// look back at the window: yaw 0 looks towards +z, 90 -x, 180 -z, 270 +x
					const f32 yaw = alongz ? (sign > 0 ? 90.0f : 270.0f) : (sign > 0 ? 180.0f : 0.0f);

					lgtTourAdd(other, nview++, px, (f32)modLevelGenFloorAt(lg, other, (s32)px, (s32)pz), pz, yaw, 0.0f);
				}
			}
			continue;
		}

		// a rooftop's bridge: looked down from, through its grating and over its
		// sides, at the street far below (views 15-18, 2026-11-16)
		if (lg->areas[ai].special == LG_SPECIAL_BRIDGE && lg->theme.layout == LG_LAYOUT_ROOFTOP) {
			const LgArea *g = &lg->areas[ai];
			const f32 bx = (f32)(lg->originx + g->x * LG_CELL + g->w * LG_CELL / 2), bz = (f32)(lg->originz + g->z * LG_CELL + g->d * LG_CELL / 2);
			s32 k;

			for (k = 0; k < 4; k++) {
				lgtTourAdd(ai, 15 + k, bx, (f32)g->floor, bz, 90.0f * k, -60.0f);
			}
		}

		// (not on the rooftops: their street is a drop to the death, 2026-11-16)
		if (lg->areas[ai].special == LG_SPECIAL_BRIDGE && lg->theme.layout != LG_LAYOUT_ROOFTOP) {
			// a bridge is also looked at from the street, 450 along the street from it (view 11)
			const LgArea *g = &lg->areas[ai];
			const bool streetx = g->w < g->d;   // narrow along x: the street runs along x
			const f32 cx = (f32)(lg->originx + g->x * LG_CELL + g->w * LG_CELL / 2), cz = (f32)(lg->originz + g->z * LG_CELL + g->d * LG_CELL / 2);
			const f32 px = streetx ? cx + 450.0f : cx, pz = streetx ? cz : cz + 450.0f;
			const s32 gx = (s32)((px - lg->originx) / LG_CELL), gz = (s32)((pz - lg->originz) / LG_CELL);

			// (nor anywhere else the floor there kills: the pipeworks' sludge, 2026-11-19)
			if (gx >= 0 && gz >= 0 && gx < lg->gw && gz < lg->gd && lg->cells[gz][gx] > 0 && lg->areas[lg->cells[gz][gx] - 1].special != LG_SPECIAL_PIT) {
				const s32 under = lg->cells[gz][gx] - 1;

				// yaw 90 looks towards -x, 180 towards -z
				lgtTourAdd(under, 11, px, (f32)modLevelGenFloorAt(lg, under, (s32)px, (s32)pz), pz, streetx ? 90.0f : 180.0f, 15.0f);
			}
		}

		if (lg->areas[ai].special == LG_SPECIAL_BALCONY) {
			// a balcony is also looked up at from the street below, 380 out from its edge (view 10)
			s32 pi;

			for (pi = 0; pi < lg->numportals; pi++) {
				const LgPortal *p = &lg->portals[pi];
				const s32 other = p->a == ai ? p->b : p->b == ai ? p->a : -1;

				if (other >= 0 && p->drop) {
					const bool alongz = p->x0 == p->x1;
					const f32 mx = (f32)(lg->originx + (p->x0 + p->x1) * LG_CELL / 2), mz = (f32)(lg->originz + (p->z0 + p->z1) * LG_CELL / 2);
					const f32 sign = p->b == other ? 1.0f : -1.0f;   // b is on the high-coordinate side of the line
					const f32 px = alongz ? mx + sign * 380.0f : mx, pz = alongz ? mz : mz + sign * 380.0f;
					const f32 yaw = alongz ? (sign > 0 ? 90.0f : 270.0f) : (sign > 0 ? 180.0f : 0.0f);

					lgtTourAdd(other, 10, px, (f32)modLevelGenFloorAt(lg, other, (s32)px, (s32)pz), pz, yaw, 20.0f);
					break;
				}
			}
		}

		lgtTourSpot(lg, ai, &x, &z);
		fy = (f32)modLevelGenFloorAt(lg, ai, (s32)x, (s32)z);

		for (v = 0; v < 4; v++) {
			lgtTourAdd(ai, v, x, fy, z, v * 90.0f, 0.0f);
		}

		if ((lg->areas[ai].flags & LG_AREAFLAG_HUB) && lg->areas[ai].ramp == LG_RAMP_NONE && lg->areas[ai].floor == 0
				&& lg->areas[ai].w * lg->areas[ai].d > best) {
			best = lg->areas[ai].w * lg->areas[ai].d;
			hubmain = ai;
		}
	}

	// the hub's floor from each end, looking along it (the long sightline); yaw 90 looks towards -x
	// (seen in the first tour), yaw 0 towards +z
	if (hubmain >= 0) {
		const LgArea *a = &lg->areas[hubmain];
		const f32 cx = (f32)(lg->originx + a->x * LG_CELL + a->w * LG_CELL / 2);
		const f32 cz = (f32)(lg->originz + a->z * LG_CELL + a->d * LG_CELL / 2);
		const bool alongx = a->w >= a->d;
		const f32 half = (alongx ? a->w : a->d) * LG_CELL / 2.0f - 120.0f;

		lgtTourAdd(hubmain, 4, alongx ? cx - half : cx, 0.0f, alongx ? cz : cz - half, alongx ? 270.0f : 0.0f, 4.0f);
		lgtTourAdd(hubmain, 5, alongx ? cx + half : cx, 0.0f, alongx ? cz : cz + half, alongx ? 90.0f : 180.0f, 4.0f);
	}

	// each mezzanine: from the bay looking at it, from the ramp's foot looking up it, and from the
	// upper floor looking out over the bay (yaw 0 looks towards +z, 90 -x, 180 -z, 270 +x)
	for (ai = 0; ai < lg->numareas; ai++) {
		const LgArea *z = &lg->areas[ai];
		s32 pi, bay = -1, ramp = -1;
		f32 dx = 0, dz = 0, mx, mz;

		if (!(z->flags & LG_AREAFLAG_MEZZ) || lg->theme.layout >= LG_LAYOUT_STATION) {
			continue;
		}

		for (pi = 0; pi < lg->numportals; pi++) {
			const LgPortal *pt = &lg->portals[pi];
			const s32 other = pt->a == ai ? pt->b : pt->b == ai ? pt->a : -1;

			if (other < 0) {
				continue;
			}

			if (lg->areas[other].ramp) {
				ramp = other;
			} else if (bay < 0 && lg->areas[other].layer < z->layer) {
				bay = other;   /* (the floor below: a tower's storey also opens onto its own storey's next piece) */
			}

			if (bay == other || ramp == other) {
				const bool alongz = pt->x0 == pt->x1;   // the opening's line runs along z: it faces +-x
				const bool zlow = alongz ? (lg->areas[ai].x + lg->areas[ai].w == pt->x0) : (lg->areas[ai].z + lg->areas[ai].d == pt->z0);

				dx = alongz ? (zlow ? 1.0f : -1.0f) : 0.0f;   // from the mezzanine out through the opening
				dz = alongz ? 0.0f : (zlow ? 1.0f : -1.0f);
			}
		}

		if (bay < 0 || (dx == 0 && dz == 0)) {
			continue;
		}

		mx = (f32)(lg->originx + z->x * LG_CELL + z->w * LG_CELL / 2);
		mz = (f32)(lg->originz + z->z * LG_CELL + z->d * LG_CELL / 2);

		{
			// out from the mezzanine's centre: the bay's floor 450 and 750 past its edge
			const f32 half = (dx != 0 ? z->w : z->d) * LG_CELL / 2.0f;
			const f32 yawout = dz > 0 ? 0.0f : dz < 0 ? 180.0f : dx < 0 ? 90.0f : 270.0f;
			const f32 yawin = yawout >= 180.0f ? yawout - 180.0f : yawout + 180.0f;
			const f32 depth = (dx != 0 ? lg->areas[bay].w : lg->areas[bay].d) * LG_CELL - 80.0f;   // the bay's depth in front of the mezzanine
			const f32 d1 = depth < 300.0f ? depth : 300.0f, d2 = depth < 520.0f ? depth : 520.0f;

			// across the opening: the middle of the bay, not of the mezzanine (in a narrow room
			// the mezzanine's middle is on the line between the bay and the ramp)
			const f32 bx = dx != 0 ? mx : (f32)(lg->originx + lg->areas[bay].x * LG_CELL + lg->areas[bay].w * LG_CELL / 2);
			const f32 bz = dz != 0 ? mz : (f32)(lg->originz + lg->areas[bay].z * LG_CELL + lg->areas[bay].d * LG_CELL / 2);

			const LgArea *ba = &lg->areas[bay];
			const f32 bx0 = (f32)(lg->originx + ba->x * LG_CELL), bx1 = bx0 + ba->w * LG_CELL;
			const f32 bz0 = (f32)(lg->originz + ba->z * LG_CELL), bz1 = bz0 + ba->d * LG_CELL;
			const f32 p6x = bx + dx * (half + d1), p6z = bz + dz * (half + d1), p7x = bx + dx * (half + d2), p7z = bz + dz * (half + d2);

			// (only where that is in the bay: a highrise's top storey looks down a
			// void or shaft from two storeys up, its middle not in line, 2026-11-17)
			if (p6x > bx0 && p6x < bx1 && p6z > bz0 && p6z < bz1) {
				lgtTourAdd(bay, 6, p6x, (f32)ba->floor, p6z, yawin, 0.0f);
			}
			if (p7x > bx0 && p7x < bx1 && p7z > bz0 && p7z < bz1) {
				lgtTourAdd(bay, 7, p7x, (f32)ba->floor, p7z, yawin, 20.0f);
			}
			lgtTourAdd(ai, 8, mx + dx * (half - 130.0f), (f32)z->floor, mz + dz * (half - 130.0f), yawout, 0.0f);
			lgtTourAdd(ai, 9, mx, (f32)z->floor, mz, yawin, 0.0f);
		}

		if (ramp >= 0) {
			const LgArea *r = &lg->areas[ramp];
			const f32 rx = (f32)(lg->originx + r->x * LG_CELL + r->w * LG_CELL / 2);
			const f32 rz = (f32)(lg->originz + r->z * LG_CELL + r->d * LG_CELL / 2);
			const f32 len = (dx != 0 ? r->w : r->d) * LG_CELL / 2.0f - 80.0f;
			const f32 yawin = dz > 0 ? 180.0f : dz < 0 ? 0.0f : dx < 0 ? 270.0f : 90.0f;

			// the ramp's foot (the end away from the mezzanine), looking up it
			lgtTourAdd(ramp, 10, rx + dx * len, (f32)modLevelGenFloorAt(lg, ramp, (s32)(rx + dx * len), (s32)(rz + dz * len)), rz + dz * len, yawin, 0.0f);
		}
	}

	sysLogPrintf(LOG_NOTE, "levelgen tour: %d stops over %d areas (hub floor: area %d)", g_LgtNumStops, (int)lg->numareas, hubmain);
	if (getenv("LGTVIEWONLY")) {
		// (looking at one kind of view: the stops of that view only)
		const s32 want = atoi(getenv("LGTVIEWONLY"));
		s32 i, n = 0;

		for (i = 0; i < g_LgtNumStops; i++) {
			if (g_LgtStops[i].view == want || (n == 0 && i == g_LgtNumStops - 1)) {
				g_LgtStops[n++] = g_LgtStops[i];
			}
		}
		g_LgtNumStops = n;
	}
	if (getenv("LGTDROPONLY") && g_LgtNumStops > 1) {
		g_LgtNumStops = 1;   // (debugging the walks: one stop, then the walks)
	}
}

/* puts the player on the floor at (x, z) in the right rooms; false when the game finds no room there */
static bool lgtTourTeleport(f32 x, f32 floory, f32 z)
{
	struct player *p = g_Vars.bond;
	struct coord q;
	RoomNum in[21];
	RoomNum above[21];
	RoomNum best = -1;
	s32 i;
	const f32 eye = p->vv_eyeheight > 50.0f ? p->vv_eyeheight : 159.0f;

	q.x = x;
	q.y = floory + eye;
	q.z = z;
	in[0] = -1;
	above[0] = -1;
	bgFindRoomsByPos(&q, in, above, 20, &best);

	if (in[0] == -1) {
		return false;
	}
	if (getenv("LGTDBGROOMS")) {
		sysLogPrintf(LOG_NOTE, "levelgen tour: teleport %.0f %.0f %.0f: in %d %d %d %d above %d %d best %d", x, floory, z, in[0], in[1], in[2], in[3], above[0], above[1], best);
	}

	propDeregisterRooms(p->prop);
	p->prop->pos = q;
	p->bondprevpos = q;   // (else a stop straight over the last one, a storey up, is reached by a move from under it)

	for (i = 0; i < 7 && in[i] != -1; i++) {
		p->prop->rooms[i] = in[i];
	}

	p->prop->rooms[i] = -1;
	propRegisterRooms(p->prop);
	p->vv_manground = floory;
	p->vv_ground = floory;
	return true;
}

static void lgtTourMap(const LgLayout *lg)
{
	LgImage img;
	char path[160];
	s32 i;

	if (!modLevelGenMapRender(lg, 0, 1, &img)) {
		return;
	}

	for (i = 0; i < g_LgtNumStops; i++) {
		if (g_LgtStops[i].view == 0) {
			modLevelGenMapMark(lg, &img, 0, 1, (s32)g_LgtStops[i].x, (s32)g_LgtStops[i].z,
					g_LgtTourAreaOk[g_LgtStops[i].area] ? 0x40e0ff : 0xff2020, 0);
		}
	}

	snprintf(path, sizeof(path), "%s/map.png", g_LgtTourDir);
	sysLogPrintf(LOG_NOTE, "levelgen tour: map %s%s", path, modLevelGenImageWritePng(&img, path) ? "" : " (not written)");
	modLevelGenImageFree(&img);
}

// After the pictures, a street tour walks the player off its balconies (2026-11-14): for each
// balcony, put him on it 70 back from its drop-down and walk him (the game's own automatic walk,
// as in Extraction and the Duel) to a pad on the street beyond: he must end up on the street.
// Then put him behind the railed part and walk him at the street again: he must stay up.
#define LGT_DROPMAX    8      // drop-downs tried (a bridged balcony has two)
#define LGT_DROPSETTLE 60     // game ticks after the teleport before the walk (a landing's dip must be over)
#define LGT_DROPWALK   180    // game ticks (60 a second) the walk may take: timed in game time, not frames,
                              // since the user's machine draws over 200 frames a second (2026-11-14)

#define LGT_CROSSMAX   4      // bridges walked across
#define LGT_CROSSWALK  420    // game ticks a crossing may take

typedef struct {
	s16 balcony, street, pad;
	u8 railed;               // 0: through the drop-down; 1: at the railing, must not fall; 2: across a bridge
	f32 x, z, yaw;           // start
	f32 floor, streetfloor;
	f32 ox, oz, far;         // a crossing: its direction and where the far balcony starts along it
	f32 rx, rz, rhx, rhz;    // at a railing: the railing's middle and half sizes (its glass is looked for)
} LgtDrop;

static LgtDrop g_LgtDrops[LGT_DROPMAX * 2 + LGT_CROSSMAX];
static s32 g_LgtCross = 0, g_LgtCrossOf = 0, g_LgtRailedBalconies = 0, g_LgtDropRetries = 0;
static s32 g_LgtNumDrops = -1;    // -1: not built yet
static s32 g_LgtDropIdx = 0;
static s32 g_LgtDropFrame = 0;
static s32 g_LgtDropLanded = -1;  // game tick he came down (or got across), or -1
static s32 g_LgtDropStart = 0;    // game tick the walk (or the settling before it) began
static bool g_LgtDropWalking = false;
static f32 g_LgtDropLowest = 0;
static s32 g_LgtDropFell = 0, g_LgtDropFellOf = 0, g_LgtDropHeld = 0, g_LgtDropHeldOf = 0, g_LgtDropNoPad = 0;
static s32 g_LgtDropShotOut = 0;  // walks at a railing whose glass the guards had shot out first

// The guards within r of x z taken out of the level (the walks test the level, not the guards)
static s32 lgtClearGuards(struct player *p, f32 x, f32 z, f32 r)
{
	s32 ci, removed = 0;

	for (ci = 0; ci < chrsGetNumSlots(); ci++) {
		struct chrdata *chr = &g_ChrSlots[ci];
		struct prop *cp = chr->prop;

		if (chr->model && cp && cp->type == PROPTYPE_CHR && cp != p->prop && !chrIsDead(chr)) {
			const f32 dx = cp->pos.x - x, dz = cp->pos.z - z;

			if (dx * dx + dz * dz < r * r) {
				chrRemove(cp, true);
				propDelist(cp);
				propDisable(cp);
				propFree(cp);
				removed++;
			}
		}
	}
	return removed;
}

// The railing in front of a walk's start (2026-11-19): the street's railings are panes of
// glass, objects that gunfire shatters as it does the game's own windows. The guards in the
// street shot at the player standing behind one, broke it, and he walked through the hole:
// the railing walks' "falls" in the full tours. So the guards about are taken out before a
// walk at a railing, and a walk whose pane was shot out all the same is not counted.
static void lgtDropRail(const LgLayout *lg, LgtDrop *d, f32 mx, f32 mz)
{
	s32 i;
	f32 best = 1e9f;

	d->rhx = d->rhz = -1.0f;
	for (i = 0; i < lg->numobst; i++) {
		const LgObstacle *o = &lg->obst[i];
		const f32 dx = (f32)o->x - mx, dz = (f32)o->z - mz, dd = dx * dx + dz * dz;

		if (o->rail && o->area == d->balcony && dd < best) {
			best = dd;
			d->rx = (f32)o->x;
			d->rz = (f32)o->z;
			d->rhx = (f32)o->halfx;
			d->rhz = (f32)o->halfz;
		}
	}
}

// 1: the walk's railing is there (a pane of glass not shattered, or no glass at all)
static bool lgtDropRailThere(const LgtDrop *d)
{
	struct prop *pr;
	s32 panes = 0, whole = 0;

	if (d->rhx < 0) {
		return true;
	}
	// the props in play: the active list, then (its tail's next) the paused one; not the
	// whole array, whose free props keep stale object pointers
	for (pr = g_Vars.activeprops; pr != NULL; pr = pr->next) {
		struct defaultobj *obj;

		if (pr->type != PROPTYPE_OBJ || (obj = pr->obj) == NULL || obj->type != OBJTYPE_GLASS) {
			continue;
		}
		if (fabsf(pr->pos.x - d->rx) > 5.0f || fabsf(pr->pos.z - d->rz) > 5.0f) {
			continue;   /* (a pane stands at its railing's middle) */
		}
		panes++;
		whole += obj->prop == pr && !(obj->hidden2 & OBJH2FLAG_DESTROYED) && !(obj->hidden & OBJHFLAG_DELETING);
	}
	return panes == 0 || whole > 0;
}

// The walk goes straight ahead, the way the player faces: the game's automatic walk with no
// turning (turn speed 0) towards a pad far away, so it never slows down or stops on its own.
static s32 lgtDropPad(const LgLayout *lg, f32 x, f32 z)
{
	s32 i, best = -1;
	f32 bestd = 0;

	for (i = 0; i < lg->numpads; i++) {
		const f32 dx = (f32)lg->pads[i].x - x, dz = (f32)lg->pads[i].z - z;

		if (dx * dx + dz * dz > bestd) {
			bestd = dx * dx + dz * dz;
			best = i;
		}
	}
	return best;
}

static void lgtDropBuild(const LgLayout *lg)
{
	s32 pi, nb = 0;

	g_LgtNumDrops = 0;

	for (pi = 0; pi < lg->numportals; pi++) {
		const LgPortal *p = &lg->portals[pi];

		g_LgtRailedBalconies += p->drop && !p->norail
			&& (lg->areas[p->a].special == LG_SPECIAL_BALCONY || lg->areas[p->b].special == LG_SPECIAL_BALCONY);
	}
	for (pi = 0; pi < lg->numportals && nb < LGT_DROPMAX; pi++) {
		const LgPortal *p = &lg->portals[pi];
		const s32 bal = lg->areas[p->a].special == LG_SPECIAL_BALCONY ? p->a : lg->areas[p->b].special == LG_SPECIAL_BALCONY ? p->b : -1;
		const s32 street = bal == p->a ? p->b : p->a;
		s32 qi;

		if (bal < 0 || !p->drop || !p->norail) {
			continue;
		}
		/* (one drop-down walk a balcony, 2026-11-19: a balcony out over the
		 * two-street layout's street has its front in several pieces, and
		 * two of them took every walk) */
		{
			s32 k2, had = 0;

			for (k2 = 0; k2 < g_LgtNumDrops; k2++) {
				had |= g_LgtDrops[k2].balcony == bal;
			}
			if (had) {
				continue;
			}
		}
		for (qi = 0; qi < 2; qi++) {
			// qi 0: the drop-down itself; qi 1: the railed part of the same edge
			const LgPortal *q = p;
			s32 k;

			if (qi == 1) {
				q = NULL;
				for (k = 0; k < lg->numportals; k++) {
					const LgPortal *r = &lg->portals[k];

					// (on the drop-down's own edge, as said: a balcony's front, not
					// the one-cell railing across its end, 2026-11-19)
					if (r != p && r->drop && !r->norail && (r->a == bal || r->b == bal)
							&& (r->x0 == r->x1) == (p->x0 == p->x1) && (p->x0 == p->x1 ? r->x0 == p->x0 : r->z0 == p->z0)) {
						q = r;
					}
				}
				if (q == NULL) {
					continue;
				}
			}
			{
				LgtDrop *d = &g_LgtDrops[g_LgtNumDrops];
				const bool alongz = q->x0 == q->x1;   // the edge runs along z: the street is across x
				const f32 mx = (f32)(lg->originx + (q->x0 + q->x1) * LG_CELL / 2);
				const f32 mz = (f32)(lg->originz + (q->z0 + q->z1) * LG_CELL / 2);
				const f32 sign = q->b == street ? 1.0f : -1.0f;   // b is on the high-coordinate side
				const f32 ox = alongz ? sign : 0.0f, oz = alongz ? 0.0f : sign;
				// for the railing: the side away from the gap (along the edge)
				const f32 gx = (f32)(lg->originx + (p->x0 + p->x1) * LG_CELL / 2), gz = (f32)(lg->originz + (p->z0 + p->z1) * LG_CELL / 2);
				const f32 away = (mx - gx) * -oz + (mz - gz) * ox;

				d->balcony = (s16)bal;
				d->street = (s16)street;
				d->railed = (u8)qi;
				d->x = mx - ox * 70.0f;
				d->z = mz - oz * 70.0f;
				// yaw 0 looks towards +z, 90 -x, 180 -z, 270 +x
				d->yaw = alongz ? (sign > 0 ? 270.0f : 90.0f) : (sign > 0 ? 0.0f : 180.0f);
				d->floor = (f32)lg->areas[bal].floor;
				d->streetfloor = (f32)modLevelGenFloorAt(lg, street, (s32)(mx + ox * 150.0f), (s32)(mz + oz * 150.0f));
				d->pad = (s16)lgtDropPad(lg, d->x, d->z);
				d->rhx = -1.0f;
				if (qi == 1) {
					lgtDropRail(lg, d, mx, mz);
				}
				(void)away;
				if (d->pad < 0) {
					g_LgtDropNoPad++;
					sysLogPrintf(LOG_NOTE, "levelgen tour: balcony %d (%s): no street pad ahead to walk at", bal, qi ? "railing" : "drop-down");
					continue;
				}
				g_LgtNumDrops++;
			}
		}
		nb++;
	}
	// a balcony railed all round (no drop-down of its own, 2026-11-19: the
	// two-street layout has some): walked at its railing too, from its middle
	for (pi = 0; pi < lg->numportals && g_LgtNumDrops < LGT_DROPMAX * 2; pi++) {
		const LgPortal *q = &lg->portals[pi];
		const s32 bal = lg->areas[q->a].special == LG_SPECIAL_BALCONY ? q->a : lg->areas[q->b].special == LG_SPECIAL_BALCONY ? q->b : -1;
		const s32 street = bal == q->a ? q->b : q->a;
		s32 k, open = 0, done = 0;

		if (bal < 0 || !q->drop || q->norail) {
			continue;
		}
		for (k = 0; k < lg->numportals; k++) {
			const LgPortal *o = &lg->portals[k];

			open |= (o->a == bal || o->b == bal) && o->drop && o->norail;
			/* (its longest railed piece: the front) */
			if ((o->a == bal || o->b == bal) && o->drop && !o->norail
					&& (o->x1 - o->x0) + (o->z1 - o->z0) > (q->x1 - q->x0) + (q->z1 - q->z0)) {
				open |= 2;
			}
		}
		for (k = 0; k < g_LgtNumDrops; k++) {
			done |= g_LgtDrops[k].balcony == bal;
		}
		if (open || done) {
			continue;   /* (walked above, beside its drop-down) */
		}
		{
			LgtDrop *d = &g_LgtDrops[g_LgtNumDrops];
			const bool alongz = q->x0 == q->x1;
			const f32 mx = (f32)(lg->originx + (q->x0 + q->x1) * LG_CELL / 2);
			const f32 mz = (f32)(lg->originz + (q->z0 + q->z1) * LG_CELL / 2);
			const f32 sign = q->b == street ? 1.0f : -1.0f;
			const f32 ox = alongz ? sign : 0.0f, oz = alongz ? 0.0f : sign;

			d->balcony = (s16)bal;
			d->street = (s16)street;
			d->railed = 1;
			d->x = mx - ox * 70.0f;
			d->z = mz - oz * 70.0f;
			lgtDropRail(lg, d, mx, mz);
			d->yaw = alongz ? (sign > 0 ? 270.0f : 90.0f) : (sign > 0 ? 0.0f : 180.0f);
			d->floor = (f32)lg->areas[bal].floor;
			d->streetfloor = (f32)modLevelGenFloorAt(lg, street, (s32)(mx + ox * 150.0f), (s32)(mz + oz * 150.0f));
			d->pad = (s16)lgtDropPad(lg, d->x, d->z);
			if (d->pad < 0) {
				g_LgtDropNoPad++;
				sysLogPrintf(LOG_NOTE, "levelgen tour: balcony %d (railing all round): no street pad ahead to walk at", bal);
				continue;
			}
			g_LgtNumDrops++;
		}
	}
	// and across each bridge, from one balcony to the other
	{
		s32 gi, nc = 0;

		for (gi = 0; gi < lg->numareas && nc < LGT_CROSSMAX; gi++) {
			const LgPortal *end[2] = { NULL, NULL };
			s32 ne = 0;

			if (lg->areas[gi].special != LG_SPECIAL_BRIDGE) {
				continue;
			}
			for (pi = 0; pi < lg->numportals && ne < 2; pi++) {
				const LgPortal *p = &lg->portals[pi];

				if ((p->a == gi || p->b == gi) && !p->drop) {
					end[ne++] = p;
				}
			}
			if (ne == 2) {
				LgtDrop *d = &g_LgtDrops[g_LgtNumDrops];
				const LgPortal *p = end[0], *q = end[1];
				const s32 bal = p->a == gi ? p->b : p->a;
				const bool alongz = p->x0 == p->x1;   // the end runs along z: the bridge is crossed along x
				const f32 mx = (f32)(lg->originx + (p->x0 + p->x1) * LG_CELL / 2), mz = (f32)(lg->originz + (p->z0 + p->z1) * LG_CELL / 2);
				const f32 sign = p->b == gi ? 1.0f : -1.0f;   // the bridge is on the high side of this end
				const f32 ox = alongz ? sign : 0.0f, oz = alongz ? 0.0f : sign;

				d->balcony = (s16)bal;
				d->street = -1;
				d->railed = 2;
				d->rhx = -1.0f;
				d->x = mx - ox * 70.0f;
				d->z = mz - oz * 70.0f;
				d->yaw = alongz ? (sign > 0 ? 270.0f : 90.0f) : (sign > 0 ? 0.0f : 180.0f);
				d->floor = (f32)lg->areas[gi].floor;
				d->streetfloor = 0;
				d->ox = ox;
				d->oz = oz;
				d->far = alongz ? (f32)(lg->originx + q->x0 * LG_CELL) * ox : (f32)(lg->originz + q->z0 * LG_CELL) * oz;
				d->pad = (s16)lgtDropPad(lg, d->x, d->z);
				g_LgtNumDrops++;
				nc++;
			}
		}
	}
	sysLogPrintf(LOG_NOTE, "levelgen tour: %d walks off %d balconies and across bridges (%d without a pad to walk at)", g_LgtNumDrops, nb, g_LgtDropNoPad);
}

// one frame of the walks; returns true when all are done
static bool lgtDropTick(const LgLayout *lg, struct player *p)
{
	LgtDrop *d;
	const f32 eye = p->vv_eyeheight > 50.0f ? p->vv_eyeheight : 159.0f;
	f32 feet;

	if (g_LgtNumDrops < 0) {
		lgtDropBuild(lg);
		g_LgtDropIdx = 0;
		g_LgtDropFrame = 0;
	}
	if (g_LgtDropIdx >= g_LgtNumDrops) {
		return true;
	}

	d = &g_LgtDrops[g_LgtDropIdx];
	feet = p->prop->pos.y - eye;

	if (g_LgtDropFrame == 0) {
		playerSetTickMode(TICKMODE_NORMAL);
		lgtTourTeleport(d->x, d->floor, d->z);
		if (d->railed == 1) {
			const s32 n = lgtClearGuards(p, d->x, d->z, 1500.0f);

			if (n > 0) {
				sysLogPrintf(LOG_NOTE, "levelgen tour: walk %d: %d guards about taken out (their fire breaks the railing's glass)", g_LgtDropIdx, n);
			}
		}
		g_LgtDropLanded = -1;
		g_LgtDropLowest = 1e9f;
		g_LgtDropStart = g_Vars.lvframe60;
		g_LgtDropWalking = false;
	}

	// settling on the balcony: LGT_DROPSETTLE game ticks (half a second), and at least 3 frames
	if (!g_LgtDropWalking) {
		p->vv_theta = d->yaw;
		p->vv_verta = 0;
		// standing still: the last walk's speed carries over a teleport (he slid off a balcony
		// before his walk began)
		p->speedgo = 0;
		p->speedforwards = 0;
		p->speedsideways = 0;
		p->speedtheta = 0;
		p->speedmaxtime60 = 0;
		if (g_LgtDropFrame > 0 && g_LgtDropFrame % 8 == 0 && p->prop->pos.y - eye < d->floor - 30.0f) {
			lgtTourTeleport(d->x, d->floor, d->z);   // and put back if he went anyway
		}
		if (g_LgtDropFrame >= 3 && g_Vars.lvframe60 - g_LgtDropStart >= LGT_DROPSETTLE) {
			char path[200];

			snprintf(path, sizeof(path), "%s/drop%02d%s-a.bmp", g_LgtTourDir, g_LgtDropIdx, d->railed == 2 ? "cross" : d->railed ? "rail" : "");
			gfx_request_screenshot_small(path, LGT_TOURWIDTH);
			// on the balcony before the walk: if not, the walk proves nothing
			if (feet < d->floor - 30.0f) {
				sysLogPrintf(LOG_NOTE, "levelgen tour: walk %d: the player is not on balcony %d before the walk (feet %.0f, floor %.0f)",
						g_LgtDropIdx, d->balcony, feet, d->floor);
			}
			g_LgtDropLowest = feet;
			g_LgtDropStart = g_Vars.lvframe60;
			g_LgtDropWalking = true;
			if (getenv("LGTDROPLOG") && d->railed == 1) {
				sysLogPrintf(LOG_NOTE, "levelgen tour: walk %d starts: railing there %d", g_LgtDropIdx, lgtDropRailThere(d));
			}
			playerAutoWalk(d->pad, 60, 0, 0, 1);   // no turning: straight on
		}
	} else {
		if (feet < g_LgtDropLowest) {
			g_LgtDropLowest = feet;
		}
		const s32 ticks = g_Vars.lvframe60 - g_LgtDropStart;

		p->vv_theta = d->yaw;   // and held straight
		if (getenv("LGTDROPLOG") && (ticks % 6) == 0) {
			sysLogPrintf(LOG_NOTE, "levelgen tour: walk %d tick %d at %.0f %.0f feet %.0f", g_LgtDropIdx, ticks, p->prop->pos.x, p->prop->pos.z, feet);
		}
		if (d->railed == 2) {
			// across: past the far end, onto the other balcony, and never off the bridge
			if (g_LgtDropLanded < 0 && p->prop->pos.x * d->ox + p->prop->pos.z * d->oz > d->far + 40.0f) {
				g_LgtDropLanded = ticks;
			}
		} else if (g_LgtDropLanded < 0 && feet < d->streetfloor + 20.0f && feet > d->streetfloor - 20.0f) {
			g_LgtDropLanded = ticks;
		}
		if ((g_LgtDropLanded >= 0 && ticks >= g_LgtDropLanded + 20)
				|| ticks >= (d->railed == 2 ? LGT_CROSSWALK : LGT_DROPWALK)) {
			char path[200];

			snprintf(path, sizeof(path), "%s/drop%02d%s-b.bmp", g_LgtTourDir, g_LgtDropIdx, d->railed == 2 ? "cross" : d->railed ? "rail" : "");
			p->vv_theta = d->yaw + 180.0f;   // look back up at the balcony (or back over the bridge)
			p->vv_verta = d->railed == 2 ? 0 : 25;
			gfx_request_screenshot_small(path, LGT_TOURWIDTH);
			playerSetTickMode(TICKMODE_NORMAL);

			// a walk that failed with a guard in the way is tried again (twice at most): the
			// guards walk about, and one on the balcony stops the player as a wall would
			if (d->railed != 1 && g_LgtDropLanded < 0 && g_LgtDropRetries < 2) {
				f32 nearest = 1e9f;
				s32 ci;

				for (ci = 0; ci < chrsGetNumSlots(); ci++) {
					struct chrdata *chr = &g_ChrSlots[ci];

					if (chr->model && chr->prop && chr->prop->type == PROPTYPE_CHR && chr->prop != p->prop && !chrIsDead(chr)) {
						const f32 dx = chr->prop->pos.x - p->prop->pos.x, dz = chr->prop->pos.z - p->prop->pos.z;
						const f32 dd = sqrtf(dx * dx + dz * dz);

						if (dd < nearest) {
							nearest = dd;
						}
					}
				}
				if (nearest < 120.0f) {
					sysLogPrintf(LOG_NOTE, "levelgen tour: walk %d: a guard %.0f away was in the way; trying again without it", g_LgtDropIdx, nearest);
					// the walk tests the level, not the guards: the ones in the way are taken
					// out of it (one stood in a drop-down through both tries, 2026-11-14)
					for (ci = 0; ci < chrsGetNumSlots(); ci++) {
						struct chrdata *chr = &g_ChrSlots[ci];
						struct prop *cp = chr->prop;

						if (chr->model && cp && cp->type == PROPTYPE_CHR && cp != p->prop && !chrIsDead(chr)) {
							const f32 dx = cp->pos.x - p->prop->pos.x, dz = cp->pos.z - p->prop->pos.z, dy = cp->pos.y - p->prop->pos.y;

							if (dx * dx + dz * dz < 150.0f * 150.0f && dy < 300.0f && dy > -300.0f) {
								chrRemove(cp, true);
								propDelist(cp);
								propDisable(cp);
								propFree(cp);
							}
						}
					}
					g_LgtDropRetries++;
					g_LgtDropFrame = 0;
					return false;
				}
			}
			// a walk at a railing that fell with its glass still whole is tried again once,
			// with any guards come back about taken out again
			if (d->railed == 1 && g_LgtDropLowest <= d->floor - 30.0f && g_LgtDropRetries < 1 && lgtDropRailThere(d)) {
				sysLogPrintf(LOG_NOTE, "levelgen tour: walk %d at a railing went over it (lowest %.0f) with its glass whole; trying again",
						g_LgtDropIdx, g_LgtDropLowest);
				g_LgtDropRetries++;
				g_LgtDropFrame = 0;
				return false;
			}
			g_LgtDropRetries = 0;
			if (d->railed == 2) {
				g_LgtCrossOf++;
				g_LgtCross += g_LgtDropLanded >= 0 && g_LgtDropLowest > d->floor - 30.0f;
			} else if (d->railed && g_LgtDropLowest <= d->floor - 30.0f && !lgtDropRailThere(d)) {
				g_LgtDropShotOut++;
				sysLogPrintf(LOG_NOTE, "levelgen tour: walk %d: the glass of balcony %d's railing was shot out (not counted)", g_LgtDropIdx, d->balcony);
			} else if (d->railed) {
				g_LgtDropHeldOf++;
				g_LgtDropHeld += g_LgtDropLowest > d->floor - 30.0f;
			} else {
				g_LgtDropFellOf++;
				g_LgtDropFell += g_LgtDropLanded >= 0;
			}
			sysLogPrintf(LOG_NOTE, "levelgen tour: walk %d off balcony %d (%s, pad %d): balcony floor %.0f, street %.0f, lowest %.0f, %s at tick %d",
					g_LgtDropIdx, d->balcony, d->railed == 2 ? "across the bridge" : d->railed ? "at the railing" : "through the drop-down", d->pad, d->floor, d->streetfloor,
					g_LgtDropLowest, g_LgtDropLanded >= 0 ? (d->railed == 2 ? "on the far balcony" : "on the street") : (d->railed == 2 ? "not across" : "not on the street"), g_LgtDropLanded);
			g_LgtDropIdx++;
			g_LgtDropFrame = 0;
			return false;
		}
	}
	g_LgtDropFrame++;
	return false;
}

static void lgtCheckStationRampLandings(const LgLayout *lg)
{
	s32 checked = 0, blocked = 0;
	char detail[100];

	for (s32 i = 0; i < lg->numportals; i++) {
		const LgPortal *pt = &lg->portals[i];
		const LgArea *a = &lg->areas[pt->a], *b = &lg->areas[pt->b];
		if (pt->drop || a->layer == b->layer || (!a->ramp && !b->ramp)) {
			continue;
		}

		const f32 x = lg->originx + (pt->x0 + pt->x1) * LG_CELL * 0.5f;
		const f32 z = lg->originz + (pt->z0 + pt->z1) * LG_CELL * 0.5f;
		const f32 dx = pt->x0 == pt->x1 ? 70.0f : 0.0f;
		const f32 dz = pt->z0 == pt->z1 ? 70.0f : 0.0f;
		const f32 y = modLevelGenFloorAt(lg, pt->a, (s32)x, (s32)z);

		checked++;
		if (lgtLineBlocked(x - dx, z - dz, pt->a + 1, x + dx, z + dz, y)
				|| lgtLineBlocked(x + dx, z + dz, pt->b + 1, x - dx, z - dz, y)) {
			blocked++;
			sysLogPrintf(LOG_NOTE, "levelgen tour: blocked ramp landing at portal %d (areas %d and %d)", i, pt->a, pt->b);
		}
	}

	snprintf(detail, sizeof(detail), "%d of %d landings blocked", blocked, checked);
	lgtCheck(checked == 6 && blocked == 0, "levelgen-tour: all six station ramp landings are clear in both directions", detail);
}

static void lgtCheckPenthouseFireEscape(const LgLayout *lg)
{
	s32 checked = 0, blocked = 0;
	char detail[100];

	for (s32 i = 0; i < lg->numportals; i++) {
		const LgPortal *pt = &lg->portals[i];
		if (lg->areas[pt->a].special != LG_SPECIAL_FIREESCAPE && lg->areas[pt->b].special != LG_SPECIAL_FIREESCAPE) {
			continue;
		}
		const f32 x = lg->originx + (pt->x0 + pt->x1) * LG_CELL * 0.5f;
		const f32 z = lg->originz + (pt->z0 + pt->z1) * LG_CELL * 0.5f;
		const f32 dx = pt->x0 == pt->x1 ? 70.0f : 0.0f;
		const f32 dz = pt->z0 == pt->z1 ? 70.0f : 0.0f;
		const f32 y = modLevelGenFloorAt(lg, pt->a, (s32)x, (s32)z);

		checked++;
		if (pt->drop || lgtLineBlocked(x - dx, z - dz, pt->a + 1, x + dx, z + dz, y)
				|| lgtLineBlocked(x + dx, z + dz, pt->b + 1, x - dx, z - dz, y)) {
			blocked++;
			sysLogPrintf(LOG_NOTE, "levelgen tour: blocked fire escape portal %d (areas %d and %d)", i, pt->a, pt->b);
		}
	}
	snprintf(detail, sizeof(detail), "%d of %d connections blocked", blocked, checked);
	lgtCheck(checked == 7 && blocked == 0, "levelgen-tour: Penthouse fire escape is clear in both directions", detail);
}

static void lgtCheckRooftopServiceBlocks(const LgLayout *lg)
{
	s32 checked = 0, shielded = 0;
	for (s32 i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];
		if (a->special != LG_SPECIAL_RACK) continue;

		const s32 row = a->z + a->d / 2;
		const s32 left = lg->cells[row][a->x - 1] - 1;
		const s32 right = lg->cells[row][a->x + a->w] - 1;
		if (left < 0 || right < 0 || LG_NOWALK(&lg->areas[left]) || LG_NOWALK(&lg->areas[right])) continue;

		const f32 x0 = lg->originx + a->x * LG_CELL - 50.0f;
		const f32 x1 = lg->originx + (a->x + a->w) * LG_CELL + 50.0f;
		const f32 z = lg->originz + row * LG_CELL + 50.0f;
		const f32 y = lg->areas[left].floor + 100.0f;
		const u32 flags = GEOFLAG_BLOCK_SHOOT | GEOFLAG_BLOCK_SIGHT;
		checked++;
		shielded += lgtLineBlockedBy(x0, z, left + 1, x1, z, y, flags)
			&& lgtLineBlockedBy(x1, z, right + 1, x0, z, y, flags);
	}

	lgtCheck(checked == 3 && shielded == checked, "levelgen-tour: Rooftops service blocks screen their catwalks",
			"missing catwalks or a clear shot through a solid building");

	s32 routes = 0, blocked = 0;
	s32 safeForNpc = 0;
	for (s32 i = 0; i < lg->numportals; i++) {
		const LgPortal *pt = &lg->portals[i];
		const LgArea *a = &lg->areas[pt->a], *b = &lg->areas[pt->b];
		if (pt->drop || a->layer == b->layer
				|| !(a->ramp || b->ramp || a->special == LG_SPECIAL_BRIDGE || b->special == LG_SPECIAL_BRIDGE)) continue;

		const f32 x = lg->originx + (pt->x0 + pt->x1) * LG_CELL * 0.5f;
		const f32 z = lg->originz + (pt->z0 + pt->z1) * LG_CELL * 0.5f;
		const f32 dx = pt->x0 == pt->x1 ? 45.0f : 0.0f;
		const f32 dz = pt->z0 == pt->z1 ? 45.0f : 0.0f;
		const f32 y = modLevelGenFloorAt(lg, pt->a, (s32)x, (s32)z);
		const struct coord from = { .x = x - dx, .y = y + 100.0f, .z = z - dz };
		const struct coord to = { .x = x + dx, .y = y + 1.0f, .z = z + dz };
		routes++;
		safeForNpc += levelgenNpcPathHasFloor(&from, &to) && levelgenNpcPathHasFloor(&to, &from);
		if (lgtLineBlocked(x - dx, z - dz, pt->a + 1, x + dx, z + dz, y)
				|| lgtLineBlocked(x + dx, z + dz, pt->b + 1, x - dx, z - dz, y)) {
			blocked++;
			sysLogPrintf(LOG_NOTE, "levelgen tour: blocked Rooftops crossing %d (areas %d and %d)", i, pt->a, pt->b);
		}
	}
	lgtCheck(routes >= 16 && blocked == 0, "levelgen-tour: Rooftops bridges and tower ramp landings are clear in both directions",
			"missing or obstructed bridge or ramp landing");
	lgtCheck(routes > 0 && safeForNpc == routes, "levelgen-tour: NPCs can plan across Rooftops bridges and ramp landings",
			"the floor check rejected a supported route");

	/* Construct a tempting shortcut across a parallel gap beside a bridge.
	 * Both ends stand on playable roofs and the middle has only the street. */
	s32 gaps = 0, rejected = 0;
	for (s32 i = 0; i < lg->numareas; i++) {
		const LgArea *bridge = &lg->areas[i];
		if (bridge->special != LG_SPECIAL_BRIDGE) continue;
		const bool acrossX = bridge->w > bridge->d;

		for (s32 offset = -3; offset <= 3; offset += 6) {
			const s32 x = bridge->x + (acrossX ? 0 : bridge->w / 2 + offset);
			const s32 z = bridge->z + (acrossX ? bridge->d / 2 + offset : 0);
			const s32 x0 = x - acrossX, x1 = x + (acrossX ? bridge->w : 0);
			const s32 z0 = z - !acrossX, z1 = z + (!acrossX ? bridge->d : 0);
			if (x0 < 0 || z0 < 0 || x1 >= lg->gw || z1 >= lg->gd) continue;

			const s32 a = lg->cells[z0][x0] - 1, b = lg->cells[z1][x1] - 1;
			const s32 midx = (x0 + x1) / 2, midz = (z0 + z1) / 2;
			const s32 under = lg->cells[midz][midx] - 1;
			if (a < 0 || b < 0 || under < 0 || lg->cells2[midz][midx] || lg->areas[under].special != LG_SPECIAL_PIT
					|| LG_NOWALK(&lg->areas[a]) || LG_NOWALK(&lg->areas[b]) || !lg->areas[a].open || !lg->areas[b].open) continue;

			const struct coord from = { .x = lg->originx + x0 * LG_CELL + 50, .y = lg->areas[a].floor + 100,
				.z = lg->originz + z0 * LG_CELL + 50 };
			const struct coord to = { .x = lg->originx + x1 * LG_CELL + 50, .y = lg->areas[b].floor + 1,
				.z = lg->originz + z1 * LG_CELL + 50 };
			gaps++;
			rejected += !levelgenNpcPathHasFloor(&from, &to);
		}
	}
	lgtCheck(gaps > 0 && rejected == gaps, "levelgen-tour: NPC route checks reject Rooftops shortcuts over the street",
			"an open drop was accepted as a walking route");
}

static void lgtTourFinish(const LgLayout *lg)
{
	char name[96];
	char detail[300];
	s32 ai, missed = 0, len = 0;

	for (ai = 0; ai < lg->numareas; ai++) {
		if (!g_LgtTourAreaOk[ai] && !LG_NOWALK(&lg->areas[ai])) {   /* (nobody stands in a ravine) */
			if (len < (s32)sizeof(detail) - 20) {
				len += snprintf(detail + len, sizeof(detail) - len, " %d", ai);
			}
			missed++;
		}
	}

	lgtTourMap(lg);

	snprintf(name, sizeof(name), "levelgen-tour: seed %u every area was reached", g_LgtTourSeed);
	if (missed) {
		detail[0] = ' ';
	}
	lgtCheck(missed == 0, name, missed ? detail : "");

	if (g_LgtTourGlassStops > 0) {
		snprintf(name, sizeof(name), "levelgen-tour: seed %u the glass floors hold the player up", g_LgtTourSeed);
		snprintf(detail, sizeof(detail), "%d of %d", g_LgtTourGlassHeld, g_LgtTourGlassStops);
		lgtCheck(g_LgtTourGlassHeld == g_LgtTourGlassStops, name, detail);
	}

	if (g_LgtTourStoreyStops > 0) {
		snprintf(name, sizeof(name), "levelgen-tour: seed %u the game finds the player in the mezzanine's rooms", g_LgtTourSeed);
		snprintf(detail, sizeof(detail), "%d of %d stops in the wrong room", g_LgtTourStoreyBad, g_LgtTourStoreyStops);
		lgtCheck(g_LgtTourStoreyBad == 0, name, detail);
	}

	if (lg->theme.layout == LG_LAYOUT_STREET) {
		s32 ai, nb = 0;

		for (ai = 0; ai < lg->numareas; ai++) {
			nb += lg->areas[ai].special == LG_SPECIAL_BALCONY;
		}
		if (nb > 0) {
			snprintf(name, sizeof(name), "levelgen-tour: seed %u the player drops from the balconies to the street", g_LgtTourSeed);
			snprintf(detail, sizeof(detail), "%d of %d walks through a drop-down reached the street (%d balconies, %d without a pad to walk at)",
					g_LgtDropFell, g_LgtDropFellOf, nb, g_LgtDropNoPad);
			lgtCheck(g_LgtDropFellOf > 0 && g_LgtDropFell == g_LgtDropFellOf, name, detail);

			if (g_LgtCrossOf > 0) {
				snprintf(name, sizeof(name), "levelgen-tour: seed %u the player crosses every bridge", g_LgtTourSeed);
				snprintf(detail, sizeof(detail), "%d of %d crossings reached the far balcony without falling", g_LgtCross, g_LgtCrossOf);
				lgtCheck(g_LgtCross == g_LgtCrossOf, name, detail);
			}

			// (a level whose balconies are all bridged has no railing on them)
			if (g_LgtDropHeldOf > 0 || g_LgtRailedBalconies > 0) {
				snprintf(name, sizeof(name), "levelgen-tour: seed %u the balconies' railings hold the player", g_LgtTourSeed);
				snprintf(detail, sizeof(detail), "%d of %d walks at a railing stayed on the balcony (%d more at glass the guards had shot out)", g_LgtDropHeld, g_LgtDropHeldOf, g_LgtDropShotOut);
				lgtCheck(g_LgtDropHeldOf > 0 && g_LgtDropHeld == g_LgtDropHeldOf, name, detail);
			}
		}
	}

	snprintf(name, sizeof(name), "levelgen-tour: seed %u every view has a picture", g_LgtTourSeed);
	snprintf(detail, sizeof(detail), "%d of %d requested", g_LgtTourShots, g_LgtNumStops);
	lgtCheck(g_LgtTourShots == g_LgtNumStops && g_LgtNumStops > 0, name, detail);

	sysLogPrintf(LOG_NOTE, "levelgen tour: %d of %d stops placed, %d of %d areas reached; pictures in %s",
			g_LgtTourPlaced, g_LgtNumStops, (int)lg->numareas - missed, (int)lg->numareas, g_LgtTourDir);
	lgtFinish();
}

static void lgtTourTick(void)
{
	const LgLayout *lg;
	LgtStop *s;
	struct player *p;

	// the guards must not end the mission before the tour has got going
	if (g_Vars.bond && g_Vars.stagenum == LEVELGEN_STAGENUM) {
		g_Vars.bond->bondhealth = 1.0f;
	}

	if (g_LgtTourPhase == 0) {
		g_LgtPrevFrame = g_Vars.lvframenum;

		if (g_Vars.lvframenum > LGT_BOOTFRAMES && g_Vars.stagenum != LEVELGEN_STAGENUM) {
			g_LgtTourSeed = (u32)strtoul(sysArgGetString("--levelgen-tour"), NULL, 10);
			g_LgtTourSize = sysArgGetInt("--levelgen-size", 1);
			sysLogPrintf(LOG_NOTE, "levelgen tour: starting seed %u size %d", g_LgtTourSeed, g_LgtTourSize);
			g_ModOptions.runtint = 0;   // the sealed-room tint would wash out the pictures

			if (!levelgenStart(g_LgtTourSeed, g_LgtTourSize)) {
				lgtCheck(false, "levelgen-tour: the level starts", "levelgenStart failed");
				lgtFinish();
			}

			g_LgtTourPhase = 1;
			g_LgtPrevFrame = 1 << 30; // the new level's frame counter starts below this
		}

		return;
	}

	if (g_LgtTourPhase == 1) {
		if (g_Vars.lvframenum < g_LgtPrevFrame && g_Vars.stagenum == LEVELGEN_STAGENUM) {
			g_LgtTourPhase = 2;
			g_LgtStopIdx = 0;
			g_LgtStopFrame = -30; // a moment for the level to settle
		}

		g_LgtPrevFrame = g_Vars.lvframenum;
		return;
	}

	lg = levelgenGetLayout();
	p = g_Vars.bond;

	if (lg == NULL || p == NULL || p->prop == NULL) {
		return;
	}

	// the pictures are for looking at: nothing may kill the player on the way (full health
	// every frame did not stop one big hit: a long tour of a large lab ended in the death screen)
	p->bondhealth = 1.0f;
	g_PlayerInvincible = true;
	g_ModOptions.runtint = 0;   // every frame: the option came back on after the level started, and rooms outside the seal were blue

	if (g_LgtStopFrame < 0) {
		g_LgtStopFrame++;

		if (g_LgtStopFrame == 0) {
			char path[160];

			if (lg->theme.layout == LG_LAYOUT_STATION) {
				lgtCheckStationRampLandings(lg);
			} else if (lg->theme.layout == LG_LAYOUT_PENTHOUSE) {
				lgtCheckPenthouseFireEscape(lg);
			} else if (lg->theme.layout == LG_LAYOUT_ROOFTOP) {
				lgtCheckRooftopServiceBlocks(lg);
			}
			snprintf(g_LgtTourDir, sizeof(g_LgtTourDir), "source/test-results/tour-%u-%d", g_LgtTourSeed, g_LgtTourSize);
			// relative to the game's folder, like the screenshots (fsCreateDir() would put them under data/)
			LGT_MKDIR("source");
			LGT_MKDIR("source/test-results");
			LGT_MKDIR(g_LgtTourDir);
			lgtTourBuild(lg);
			snprintf(g_LgtTourManifest, sizeof(g_LgtTourManifest), "%s/manifest.txt", g_LgtTourDir);

			{
				FILE *fp = fopen(g_LgtTourManifest, "w");

				if (fp) {
					fprintf(fp, "# tour seed %u size %d areas %d rooms %d stops %d half %d %d\n", g_LgtTourSeed, g_LgtTourSize,
							(int)lg->numareas, (int)lg->numrooms, g_LgtNumStops, levelgenGetHalfX(), levelgenGetHalfZ());
					fprintf(fp, "# stop area room kind flags floor ceil ramp view yaw pitch x z placed playerroom file\n");
					fclose(fp);
				}
			}

			(void)path;
		}

		return;
	}

	if (g_LgtStopIdx >= g_LgtNumStops) {
		if (lg->theme.layout != LG_LAYOUT_STREET || lgtDropTick(lg, p)) {
			lgtTourFinish(lg);
		}
		return;
	}

	s = &g_LgtStops[g_LgtStopIdx];

	if (g_LgtStopFrame == 0) {
		if (s->x != g_LgtTourLastX || s->z != g_LgtTourLastZ || s->floor != g_LgtTourLastFloor || g_LgtStopTries > 0) {
			g_LgtTourPlacedNow = lgtTourTeleport(s->x, s->floor, s->z);
			g_LgtTourLastX = s->x;
			g_LgtTourLastZ = s->z;
			g_LgtTourLastFloor = s->floor;
		}
	}

	// the view is held every frame until the picture is taken
	p->vv_theta = s->yaw;
	p->vv_verta = s->pitch;

	if (g_LgtStopFrame == LGT_TOURSETTLE) {
		const f32 dx = p->prop->pos.x - s->x;
		const f32 dz = p->prop->pos.z - s->z;
		const bool near = dx * dx + dz * dz < 120.0f * 120.0f && p->prop->rooms[0] > 0;

		g_LgtTourRoomNow = p->prop->rooms[0];

		// (the lifts, 2026-11-17: where each is now; at a lift view, the
		// player presses activate at the shaft's edge to call it to his floor)
		// (2026-11-17, second go: the lifts have doors. At a lift view the
		// player activates the door of his storey, as he would standing at
		// it: the lift is called, or if it is here its door shuts and it goes)
		if (g_LgtStopTries == 0 && s->view == 19) {
			struct linkliftdoorobj *ll, *best = NULL;
			f32 bestd = 1e9f;

			for (ll = g_LiftDoors; ll; ll = ll->next) {
				if (ll->door) {
					const f32 dx = ll->door->pos.x - p->prop->pos.x, dy = ll->door->pos.y - p->vv_manground, dz = ll->door->pos.z - p->prop->pos.z;
					const f32 d = dx * dx + dz * dz + dy * dy * 4.0f;

					if (d < bestd) {
						bestd = d;
						best = ll;
					}
				}
			}
			if (best) {
				struct doorobj *dr = (struct doorobj *)best->door->obj;

				sysLogPrintf(LOG_NOTE, "levelgen tour: LIFT door of stop %d activated from y %.0f (door frac %.2f mode %d)", best->stopnum, p->vv_manground, dr->frac, dr->mode);
				doorsActivate(best->door, true);
			}
		}
		if (g_LgtStopTries == 0) {
			struct prop *lp = g_Vars.activeprops;

			while (lp) {
				if (lp->type == PROPTYPE_OBJ && lp->obj && lp->obj->type == OBJTYPE_LIFT) {
					struct liftobj *lo = (struct liftobj *)lp->obj;

					sysLogPrintf(LOG_NOTE, "levelgen tour: LIFT at %.0f %.0f %.0f stop %d aim %d doors %.2f %.2f %.2f frame %d", lp->pos.x, lp->pos.y, lp->pos.z, lo->levelcur, lo->levelaim,
							lo->doors[0] ? lo->doors[0]->frac : -1.0f, lo->doors[1] ? lo->doors[1]->frac : -1.0f, lo->doors[2] ? lo->doors[2]->frac : -1.0f, g_Vars.lvframe60);
				}
				lp = lp->next;
			}
		}

		if (lg->areas[s->area].special == LG_SPECIAL_GLASS && (near || g_LgtStopTries >= LGT_TOURRETRY)) {
			const f32 eye = p->vv_eyeheight > 50.0f ? p->vv_eyeheight : 159.0f;
			const f32 feet = p->prop->pos.y - eye;

			g_LgtTourGlassStops++;
			g_LgtTourGlassHeld += feet > s->floor - 30.0f;
			sysLogPrintf(LOG_NOTE, "levelgen tour: glass floor %d: feet %.0f, the panes at %.0f", s->area, feet, s->floor);
		}

		if (!near && g_LgtStopTries < LGT_TOURRETRY) {
			// the game moved the player (or found no room there): put him back and wait again
			g_LgtStopTries++;
			g_LgtStopFrame = 0;
			g_LgtTourLastX = -1e9f;
			return;
		}

		g_LgtTourPlacedNow = near;
		g_LgtTourAreaSeen[s->area] = true;

		// (and on a balcony, or in the porch under one: the game must tell the two storeys apart)
		// (not the views of a window or of a balcony or bridge from the street, views 8-12: they stand a set
		// distance from what they look at, which may be in the next area of the same room)
		if (s->view < 8 && ((lg->areas[s->area].flags & (LG_AREAFLAG_MEZZ | LG_AREAFLAG_UNDER | LG_AREAFLAG_BAY)) || lg->areas[s->area].special == LG_SPECIAL_BALCONY
				|| lg->areas[s->area].special == LG_SPECIAL_BRIDGE
				|| (lg->areas[s->area].kind == LG_AREA_CORRIDOR && s->view < 4 && lg->cells2[lg->areas[s->area].z][lg->areas[s->area].x] > 0))) {
			g_LgtTourStoreyStops++;
			g_LgtTourStoreyBad += g_LgtTourRoomNow != s->area + 1;
			if (g_LgtTourRoomNow != s->area + 1) {
				sysLogPrintf(LOG_NOTE, "levelgen tour: storey stop in area %d (view %d) found in room %d", s->area, s->view, g_LgtTourRoomNow - 1);
				{
					const s32 rr = g_LgtTourRoomNow, rw = s->area + 1;

					if (rr > 0 && rr < g_Vars.roomcount && rw < g_Vars.roomcount) {
						sysLogPrintf(LOG_NOTE, "levelgen tour:   player at %.0f %.0f %.0f feet %.0f rooms %d %d %d", g_Vars.currentplayer->prop->pos.x, g_Vars.currentplayer->prop->pos.y, g_Vars.currentplayer->prop->pos.z, g_Vars.currentplayer->vv_manground,
								g_Vars.currentplayer->prop->rooms[0], g_Vars.currentplayer->prop->rooms[1], g_Vars.currentplayer->prop->rooms[2]);
						{
							struct coord gp;
							RoomNum grooms[2];
							u16 col = 0;
							u8 ft = 0;

							gp.x = s->x; gp.y = s->floor + 60.0f; gp.z = s->z;
							grooms[0] = rw; grooms[1] = -1;
							sysLogPrintf(LOG_NOTE, "levelgen tour:   ground under the stop in its own room: %.0f", cdFindGroundAtCyl(&gp, 20.0f, grooms, &col, &ft));
						}
						sysLogPrintf(LOG_NOTE, "levelgen tour:   found room bbox y %.0f..%.0f x %.0f..%.0f z %.0f..%.0f; wanted %.0f..%.0f",
								g_Rooms[rr].bbmin[1], g_Rooms[rr].bbmax[1], g_Rooms[rr].bbmin[0], g_Rooms[rr].bbmax[0], g_Rooms[rr].bbmin[2], g_Rooms[rr].bbmax[2],
								g_Rooms[rw].bbmin[1], g_Rooms[rw].bbmax[1]);
					}
				}
			}
		}

		if (near) {
			g_LgtTourPlaced++;
			g_LgtTourAreaOk[s->area] = true;
		}

		{
			char file[48];
			char path[200];
			const LgArea *a = &lg->areas[s->area];
			FILE *fp;

			snprintf(file, sizeof(file), "s%03d.bmp", g_LgtStopIdx);
			snprintf(path, sizeof(path), "%s/%s", g_LgtTourDir, file);
			gfx_request_screenshot_small(path, LGT_TOURWIDTH);
			g_LgtTourShots++;
			if (getenv("LGTDRAWN")) {
				extern struct drawslot g_BgDrawSlots[];
				extern s32 g_BgNumDrawSlots;
				char line[8192];
				s32 n = 0, k;

				for (k = 0; k < g_BgNumDrawSlots && n < (s32)sizeof(line) - 8; k++) {
					n += snprintf(line + n, sizeof(line) - n, " %d%s", g_BgDrawSlots[k].roomnum, g_Rooms[g_BgDrawSlots[k].roomnum].loaded240 ? "" : "*");
				}
				line[n] = 0;
				sysLogPrintf(LOG_NOTE, "levelgen tour: %s drawn rooms (%d):%s", file, g_BgNumDrawSlots, line);
			}

			fp = fopen(g_LgtTourManifest, "a");

			if (fp) {
				fprintf(fp, "%d %d %d %s %d %d %d %d %d %.0f %.0f %.0f %.0f %d %d %s\n", g_LgtStopIdx, (int)s->area, (int)a->room,
						a->kind == LG_AREA_ROOM ? "room" : "corridor", (int)a->flags, (int)a->floor, (int)a->ceil, (int)a->ramp,
						(int)s->view, s->yaw, s->pitch, s->x, s->z, near ? 1 : 0, g_LgtTourRoomNow, file);
				fclose(fp);
			}
		}
	}

	g_LgtStopFrame++;

	if (g_LgtStopFrame >= LGT_TOURGAP) {
		g_LgtStopIdx++;
		g_LgtStopFrame = 0;
		g_LgtStopTries = 0;
	}
}

void levelgenLoadTestTick(void)
{
	char tag[32];

	if (!g_LevelGenLoadTest || g_Vars.currentplayer != g_Vars.bond) {
		return;
	}

	if (g_LevelGenTour) {
		if (++g_LgtTicks > 40000) {
			lgtCheck(false, "levelgen-tour: finished in time", "no result");
			lgtFinish();
		}

		lgtTourTick();
		return;
	}

	if (++g_LgtTicks > LGT_TIMEOUT) {
		lgtCheck(false, "levelgen-load: finished in time", "no result");
		lgtFinish();
	}

	// a new level instance is a frame counter that went back
	if (g_Vars.lvframenum < g_LgtPrevFrame) {
		g_LgtSampled1 = false;
		g_LgtShotsTaken = 0;

		if (g_LgtPhase == 1 && g_Vars.stagenum == LEVELGEN_STAGENUM) {
			g_LgtPhase = 2;
		}
	}

	g_LgtPrevFrame = g_Vars.lvframenum;

	if (g_LgtPhase == 0) {
		if (g_Vars.lvframenum > LGT_BOOTFRAMES && g_Vars.stagenum != LEVELGEN_STAGENUM) {
			lgtCheckPool();
			sysLogPrintf(LOG_NOTE, "levelgen test: starting level 0, seed %u", g_LgtSeeds[0]);
			modLevelGenForceTheme(LG_LAYOUT_HUB, -1);   // level 0 a hub, level 1 an underground lab, level 2 a street

			if (!levelgenStart(g_LgtSeeds[0], LEVELGEN_SIZE_MEDIUM)) {
				lgtCheck(false, "levelgen-load: level 0 starts", "levelgenStart failed");
				lgtFinish();
			}

			g_LgtHalf[0][0] = levelgenGetHalfX();
			g_LgtHalf[0][1] = levelgenGetHalfZ();
			g_LgtPhase = 1;
		}

		return;
	}

	if (g_LgtPhase != 2) {
		return;
	}

	snprintf(tag, sizeof(tag), "level %d", g_LgtLevel);

	if (!g_LgtSampled1 && g_Vars.lvframenum >= LGT_SAMPLE1) {
		g_LgtSampled1 = true;
		lgtCheckLevel(tag);
		lgtCheckDoors(tag);
		lgtCheckCrates(tag);
		lgtCheckPillars(tag);

		{
			char name[96];
			char detail[100];

			// the mission's own seed is the level's seed run through the game's stage mixing,
			// so it is not equal to it; what is checked is that one was dealt
			snprintf(name, sizeof(name), "levelgen-load: %s was dealt a mission", tag);
			snprintf(detail, sizeof(detail), "level seed %u, mission seed %u", levelgenGetSeed(), modRandomGetSeed());
			sysLogPrintf(LOG_NOTE, "levelgen test: %s: level seed %u, mission seed %u", tag, levelgenGetSeed(), modRandomGetSeed());
			lgtCheck(modRandomGetSeed() != 0, name, detail);
		}
		lgtCheckPlayer(tag, g_LgtLevel);
	}

	lgtTickShots(tag);

	if (g_Vars.lvframenum >= LGT_SAMPLE2) {
		char name[96];
		s32 guards = lgtCountGuards();

		lgtLogBrightness(tag, -1);
		lgtWriteMap(tag);

		lgtCheckPlayer(tag, g_LgtLevel);
		lgtCheckLights(tag);

		/* the room the player is in settled at full brightness: drawn the same
		 * before its brightness is worked out as after (no darkening) */
		if (g_Vars.bond && g_Vars.bond->prop && g_Vars.bond->prop->rooms[0] > 0) {
			const s32 rm = g_Vars.bond->prop->rooms[0];
			char detail[96];
			u8 brmin = 0, brmax = 0;

			bgGetRoomBrightnessRange(rm, &brmin, &brmax);
			snprintf(name, sizeof(name), "levelgen-load: %s the player's room is fully lit", tag);
			snprintf(detail, sizeof(detail), "room %d settled at %d of %d (calced %d)", rm,
					g_Rooms[rm].br_settled_regional, brmax, (g_Rooms[rm].flags & ROOMFLAG_BRIGHTNESS_CALCED) != 0);
			// the room's own ceiling, less what its lights' whole-number shares lose
			lgtCheck((g_Rooms[rm].flags & ROOMFLAG_BRIGHTNESS_CALCED) && g_Rooms[rm].br_settled_regional + 12 >= brmax, name, detail);
		}

		snprintf(name, sizeof(name), "levelgen-load: %s has guards", tag);
		lgtCheck(guards > 0, name, "no live guards");
		g_LgtLevel++;

		if (g_LgtLevel >= LGT_LEVELS) {
			g_LgtPhase = 3;
			lgtFinish();
		}

		sysLogPrintf(LOG_NOTE, "levelgen test: starting level %d, seed %u", g_LgtLevel, g_LgtSeeds[g_LgtLevel]);
		modLevelGenForceTheme(g_LgtLevel < LG_LAYOUT_COUNT ? g_LgtLevel : -1, -1);

		if (!levelgenStart(g_LgtSeeds[g_LgtLevel], LEVELGEN_SIZE_MEDIUM)) {
			lgtCheck(false, "levelgen-load: next level starts", "levelgenStart failed");
			lgtFinish();
		}

		g_LgtHalf[g_LgtLevel][0] = levelgenGetHalfX();
		g_LgtHalf[g_LgtLevel][1] = levelgenGetHalfZ();
		g_LgtPhase = 1;
		g_LgtSampled1 = false;
		g_LgtShotsTaken = 0;
	}
}
