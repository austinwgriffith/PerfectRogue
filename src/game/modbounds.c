#include <ultra64.h>
#include "constants.h"
#include "prcompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game/bg.h"
#include "game/chraction.h"
#include "game/modbounds.h"
#include "game/prop.h"
#include "bss.h"
#include "lib/vars.h"
#include "data.h"
#include "types.h"
#include "system.h"

/* ---------------------------------------------------------------------
 * Bounds-fix regression test, run with:  pd.x86_64.exe --selftest-bounds
 *
 * Covers the fixed-size arrays that more NPCs than stock's setup would write
 * past (see claude/npc-limits-research.md):
 *
 *  - g_Vars.onscreenprops (200 entries in stock), propsSort()'s depth buffer
 *    and bg.c's room list: propsCollectOnscreen() and propsSortList() are fed
 *    thousands of props, and the real stage's list is filled with every prop it
 *    has. Canaries after each buffer must stay put.
 *  - g_TeamList (264 entries) and g_SquadronList (272): the builders are fed
 *    up to 2000 chr slots, and a list that is too small must drop chrs rather
 *    than overrun.
 *
 * Boots the level, runs everything in one tick once the level is up (nothing
 * else runs in between), logs SELFTEST lines to pd.log and exits 0 if all
 * passed, 1 if not. test/run_tests.sh runs it for several boot stages.
 * --------------------------------------------------------------------- */

bool g_BoundsTest = false; // --selftest-bounds

#define BOUNDS_CANARY_PTR ((struct prop *)(uintptr_t)0xdeadbeefu)
#define BOUNDS_CANARY_S16 ((s16)0x5a5a)

static s32 g_BoundsPassed = 0;
static s32 g_BoundsFailed = 0;
static bool g_BoundsDone = false;

static void modBoundsCheck(bool ok, const char *name, const char *detail)
{
	if (ok) {
		g_BoundsPassed++;
		sysLogPrintf(LOG_NOTE, "SELFTEST PASS %s", name);
	} else {
		g_BoundsFailed++;
		sysLogPrintf(LOG_NOTE, "SELFTEST FAIL %s: %s", name, detail ? detail : "");
	}
}

/** Stock's selection sort, kept here as the reference the new sort must match. */
static void modBoundsReferenceSort(struct prop **list, s32 count)
{
	f32 *depths = malloc((count + 1) * sizeof(f32));
	s32 i;
	s32 j;

	for (i = 0; i < count; i++) {
		depths[i] = list[i]->z;
	}

	for (i = 0; i < count; i++) {
		s32 swapindex = -1;
		f32 depth = -4294967296;

		for (j = i; j < count; j++) {
			if (depths[j] > depth) {
				depth = depths[j];
				swapindex = j;
			}
		}

		if (swapindex >= 0) {
			struct prop *prop = list[i];

			depth = depths[i];
			list[i] = list[swapindex];
			depths[i] = depths[swapindex];
			list[swapindex] = prop;
			depths[swapindex] = depth;
		}
	}

	free(depths);
}

static u32 g_BoundsSeed = 12345;

static u32 modBoundsRand(void)
{
	g_BoundsSeed = g_BoundsSeed * 1664525u + 1013904223u;
	return g_BoundsSeed >> 8;
}

static void modBoundsTestSort(void)
{
	static const s32 sizes[] = {0, 1, 2, 3, 50, 199, 200, 201, 255, 256, 257, 300, 1000, 5000};
	char detail[128] = "";
	bool canaryok = true;
	bool sortedok = true;
	bool permok = true;
	bool sameok = true;
	s32 k;
	s32 i;

	for (k = 0; k < (s32)(sizeof(sizes) / sizeof(sizes[0])); k++) {
		s32 n = sizes[k];
		struct prop *props = calloc(n + 1, sizeof(struct prop));
		struct prop **list = malloc((n + 2) * sizeof(struct prop *));
		struct prop **ref = malloc((n + 2) * sizeof(struct prop *));
		u8 *seen = calloc(n + 1, 1);

		for (i = 0; i < n; i++) {
			// a small range so there are plenty of equal depths
			props[i].z = (f32)(modBoundsRand() % (n > 256 ? 1000 : 40));
			list[i] = &props[i];
			ref[i] = &props[i];
		}

		list[n] = BOUNDS_CANARY_PTR;

		propsSortList(list, n);

		if (list[n] != BOUNDS_CANARY_PTR) {
			canaryok = false;
			snprintf(detail, sizeof(detail), "n=%d: the entry after the list was overwritten", n);
		}

		for (i = 0; i + 1 < n; i++) {
			if (list[i]->z < list[i + 1]->z) {
				sortedok = false;
				snprintf(detail, sizeof(detail), "n=%d: out of order at %d", n, i);
				break;
			}
		}

		for (i = 0; i < n; i++) {
			s32 idx = (s32)(list[i] - props);

			if (idx < 0 || idx >= n || seen[idx]) {
				permok = false;
				snprintf(detail, sizeof(detail), "n=%d: a prop is missing or doubled at %d", n, i);
				break;
			}

			seen[idx] = 1;
		}

		if (n <= 256) {
			modBoundsReferenceSort(ref, n);

			for (i = 0; i < n; i++) {
				if (list[i] != ref[i]) {
					sameok = false;
					snprintf(detail, sizeof(detail), "n=%d: differs from stock's sort at %d", n, i);
					break;
				}
			}
		}

		free(props);
		free(list);
		free(ref);
		free(seen);
	}

	modBoundsCheck(canaryok, "propsSortList stays inside its list", detail);
	modBoundsCheck(sortedok, "propsSortList sorts furthest first (0 to 5000 props)", detail);
	modBoundsCheck(permok, "propsSortList keeps every prop once", detail);
	modBoundsCheck(sameok, "propsSortList matches stock's order up to 256 props", detail);
}

static void modBoundsTestCollect(void)
{
	char detail[128] = "";
	const s32 n = 1000;
	struct prop *props = calloc(n + 1, sizeof(struct prop));
	struct prop **list = malloc((n + 8) * sizeof(struct prop *));
	s32 matches = 0;
	s32 got;
	s32 i;
	bool orderok = true;

	for (i = 0; i < n; i++) {
		props[i].next = &props[i + 1];

		switch (i % 4) {
		case 0:
		case 1:
			props[i].flags = PROPFLAG_ONTHISSCREENTHISTICK | PROPFLAG_ENABLED;
			matches++;
			break;
		case 2:
			props[i].flags = PROPFLAG_ENABLED;
			break;
		default:
			props[i].flags = PROPFLAG_ONTHISSCREENTHISTICK;
			break;
		}
	}

	// room for fewer than there are: the rest must be left out, not written
	for (i = 0; i < n + 8; i++) {
		list[i] = BOUNDS_CANARY_PTR;
	}

	got = propsCollectOnscreen(&props[0], &props[n], list, 300);
	snprintf(detail, sizeof(detail), "returned %d, wanted 300", got);
	modBoundsCheck(matches > 300 && got == 300, "collect stops at the cap", detail);
	snprintf(detail, sizeof(detail), "list[300] is %p", (void *)list[300]);
	modBoundsCheck(list[300] == NULL, "collect ends the list with NULL at the cap", detail);
	modBoundsCheck(list[301] == BOUNDS_CANARY_PTR && list[n + 7] == BOUNDS_CANARY_PTR, "collect writes nothing past the NULL", "an entry past the end was written");

	// room for all of them
	for (i = 0; i < n + 8; i++) {
		list[i] = BOUNDS_CANARY_PTR;
	}

	got = propsCollectOnscreen(&props[0], &props[n], list, n);
	snprintf(detail, sizeof(detail), "returned %d, wanted %d", got, matches);
	modBoundsCheck(got == matches, "collect finds every enabled, onscreen prop", detail);
	modBoundsCheck(list[got] == NULL, "collect ends the list with NULL", "no NULL after the last prop");

	for (i = 0; i < got; i++) {
		if ((list[i]->flags & (PROPFLAG_ONTHISSCREENTHISTICK | PROPFLAG_ENABLED)) != (PROPFLAG_ONTHISSCREENTHISTICK | PROPFLAG_ENABLED)
				|| (i > 0 && list[i] <= list[i - 1])) {
			orderok = false;
			break;
		}
	}

	modBoundsCheck(orderok, "collect keeps only matching props, in list order", "a wrong prop or order");

	got = propsCollectOnscreen(&props[5], &props[5], list, n);
	modBoundsCheck(got == 0 && list[0] == NULL, "collect of an empty list is empty", "not empty");

	free(props);
	free(list);
}

/** The chrnums of one team/squadron list, starting at start. Returns how many, or -1 if it never ends. */
static s32 modBoundsWalk(s16 *list, s32 size, s32 start, s16 *out, s32 maxout)
{
	s32 n = 0;
	s32 i;

	for (i = start; i >= 0 && i < size; i++) {
		if (list[i] == -2) {
			return n;
		}

		if (n < maxout) {
			out[n] = list[i];
		}

		n++;
	}

	return -1;
}

static void modBoundsTestLists(void)
{
	static const s32 sizes[] = {10, 100, 300, 1000, 2000};
	char detail[128] = "";
	bool teamcanary = true;
	bool teamcontent = true;
	bool teamdropped = true;
	bool sqcanary = true;
	bool sqcontent = true;
	bool sqdropped = true;
	s32 k;
	s32 i;
	s32 t;

	modBoundsCheck(teamListSizeFor(0) >= (MAX_CHRSPERTEAM + 1) * MAX_TEAMS
			&& squadronListSizeFor(0) >= (MAX_CHRSPERSQUADRON + 1) * MAX_SQUADRONS,
			"the lists are never smaller than stock's", "smaller than stock");
	modBoundsCheck(teamListSizeFor(300) > (MAX_CHRSPERTEAM + 1) * MAX_TEAMS
			&& squadronListSizeFor(300) > (MAX_CHRSPERSQUADRON + 1) * MAX_SQUADRONS,
			"the lists grow with the chr slots", "300 slots fit stock's size");

	for (k = 0; k < (s32)(sizeof(sizes) / sizeof(sizes[0])); k++) {
		s32 n = sizes[k];
		struct chrdata *slots = calloc(n, sizeof(struct chrdata));
		s32 tsize = teamListSizeFor(n);
		s32 ssize = squadronListSizeFor(n);
		s16 *tl = malloc((tsize + 16) * sizeof(s16));
		s16 *sl = malloc((ssize + 16) * sizeof(s16));
		s16 *got = malloc((n + 1) * sizeof(s16));
		s32 dropped;

		for (i = 0; i < n; i++) {
			slots[i].chrnum = (i % 7 == 3) ? -1 : (s16)i;
			slots[i].team = (i % 5 == 0) ? 0xff : (u8)(1 << (i % 8));
			slots[i].squadron = i % 16;
		}

		for (i = 0; i < tsize + 16; i++) {
			tl[i] = (i >= tsize) ? BOUNDS_CANARY_S16 : 0;
		}

		for (i = 0; i < ssize + 16; i++) {
			sl[i] = (i >= ssize) ? BOUNDS_CANARY_S16 : 0;
		}

		dropped = teamListBuild(tl, tsize, slots, n);

		if (dropped != 0) {
			teamdropped = false;
			snprintf(detail, sizeof(detail), "n=%d: %d chrs dropped from a list sized for them", n, dropped);
		}

		for (i = tsize; i < tsize + 16; i++) {
			if (tl[i] != BOUNDS_CANARY_S16) {
				teamcanary = false;
				snprintf(detail, sizeof(detail), "n=%d: team list written past its size", n);
			}
		}

		for (t = 0; t < MAX_TEAMS; t++) {
			s32 start = (t == 0) ? MAX_TEAMS - 1 : tl[t - 1];
			s32 len = modBoundsWalk(tl, tsize, start, got, n);
			s32 want = 0;

			for (i = 0; i < n; i++) {
				if (slots[i].chrnum >= 0 && (slots[i].team & (1 << t))) {
					if (len >= 0 && want < len && got[want] != slots[i].chrnum) {
						len = -2;
					}

					want++;
				}
			}

			if (len != want) {
				teamcontent = false;
				snprintf(detail, sizeof(detail), "n=%d team %d: list has %d chrs, wanted %d", n, t, len, want);
			}
		}

		dropped = squadronListBuild(sl, ssize, slots, n);

		if (dropped != 0) {
			sqdropped = false;
			snprintf(detail, sizeof(detail), "n=%d: %d chrs dropped from a list sized for them", n, dropped);
		}

		for (i = ssize; i < ssize + 16; i++) {
			if (sl[i] != BOUNDS_CANARY_S16) {
				sqcanary = false;
				snprintf(detail, sizeof(detail), "n=%d: squadron list written past its size", n);
			}
		}

		for (t = 0; t < MAX_SQUADRONS; t++) {
			s32 start = (t == 0) ? MAX_SQUADRONS - 1 : sl[t - 1];
			s32 len = modBoundsWalk(sl, ssize, start, got, n);
			s32 want = 0;

			for (i = 0; i < n; i++) {
				if (slots[i].chrnum >= 0 && slots[i].squadron == t) {
					if (len >= 0 && want < len && got[want] != slots[i].chrnum) {
						len = -2;
					}

					want++;
				}
			}

			if (len != want) {
				sqcontent = false;
				snprintf(detail, sizeof(detail), "n=%d squadron %d: list has %d chrs, wanted %d", n, t, len, want);
			}
		}

		free(slots);
		free(tl);
		free(sl);
		free(got);
	}

	modBoundsCheck(teamdropped, "no chr is dropped from a team list sized for the slots", detail);
	modBoundsCheck(teamcanary, "the team list stays inside its size (10 to 2000 chrs)", detail);
	modBoundsCheck(teamcontent, "every team lists exactly its chrs, each ended by -2", detail);
	modBoundsCheck(sqdropped, "no chr is dropped from a squadron list sized for the slots", detail);
	modBoundsCheck(sqcanary, "the squadron list stays inside its size (10 to 2000 chrs)", detail);
	modBoundsCheck(sqcontent, "every squadron lists exactly its chrs, each ended by -2", detail);

	// too small a list: chrs are left out, nothing is written past the end,
	// and every team and squadron still ends where its header says
	{
		const s32 n = 300;
		const s32 tsize = 60;
		const s32 ssize = 60;
		struct chrdata *slots = calloc(n, sizeof(struct chrdata));
		s16 *tl = malloc((tsize + 16) * sizeof(s16));
		s16 *sl = malloc((ssize + 16) * sizeof(s16));
		s16 *got = malloc((n + 1) * sizeof(s16));
		bool ended = true;
		bool inside = true;
		s32 dtl;
		s32 dsl;

		for (i = 0; i < n; i++) {
			slots[i].chrnum = (s16)i;
			slots[i].team = 0xff;
			slots[i].squadron = i % 16;
		}

		for (i = 0; i < tsize + 16; i++) {
			tl[i] = (i >= tsize) ? BOUNDS_CANARY_S16 : 0;
			sl[i] = (i >= ssize) ? BOUNDS_CANARY_S16 : 0;
		}

		dtl = teamListBuild(tl, tsize, slots, n);
		dsl = squadronListBuild(sl, ssize, slots, n);

		for (i = tsize; i < tsize + 16; i++) {
			if (tl[i] != BOUNDS_CANARY_S16 || sl[i] != BOUNDS_CANARY_S16) {
				inside = false;
			}
		}

		for (t = 0; t < MAX_TEAMS; t++) {
			if (modBoundsWalk(tl, tsize, (t == 0) ? MAX_TEAMS - 1 : tl[t - 1], got, n) < 0) {
				ended = false;
			}
		}

		for (t = 0; t < MAX_SQUADRONS; t++) {
			if (modBoundsWalk(sl, ssize, (t == 0) ? MAX_SQUADRONS - 1 : sl[t - 1], got, n) < 0) {
				ended = false;
			}
		}

		snprintf(detail, sizeof(detail), "dropped teams %d squadrons %d", dtl, dsl);
		modBoundsCheck(dtl > 0 && dsl > 0, "an undersized list drops chrs rather than overrun", detail);
		modBoundsCheck(inside, "an undersized list is not written past its size", "the entries past the end changed");
		modBoundsCheck(ended, "an undersized list still ends every team and squadron", "a list has no -2");

		free(slots);
		free(tl);
		free(sl);
		free(got);
	}
}

/** The stage that is actually running: wiring of the real arrays. */
static void modBoundsTestStage(void)
{
	char detail[160] = "";
	struct prop *prop;
	struct prop **saved = NULL;
	u8 *flags;
	s32 numactive = 0;
	s32 i;
	bool sorted = true;
	s16 *got = malloc((g_NumChrSlots + 1) * sizeof(s16));
	bool teamsok = true;
	bool sqok = true;
	s32 t;

	for (prop = g_Vars.activeprops; prop && prop != g_Vars.pausedprops; prop = prop->next) {
		numactive++;
	}

	flags = malloc(numactive + 1);
	saved = malloc((numactive + 1) * sizeof(struct prop *));
	i = 0;

	for (prop = g_Vars.activeprops; prop && prop != g_Vars.pausedprops; prop = prop->next) {
		saved[i] = prop;
		flags[i] = prop->flags;
		prop->flags |= PROPFLAG_ONTHISSCREENTHISTICK | PROPFLAG_ENABLED;
		i++;
	}

	sysLogPrintf(LOG_NOTE, "SELFTEST INFO stage 0x%02x: %d active props, maxprops %d, onscreen cap %d, %d chr slots",
			g_Vars.stagenum, numactive, g_Vars.maxprops, g_PropsOnscreenCap, g_NumChrSlots);

	snprintf(detail, sizeof(detail), "cap %d, maxprops %d", g_PropsOnscreenCap, g_Vars.maxprops);
	modBoundsCheck(g_PropsOnscreenCap >= g_Vars.maxprops && g_PropsOnscreenCap >= 200, "the onscreen list can hold every prop", detail);

	propsSort();

	snprintf(detail, sizeof(detail), "%d onscreen, %d active", g_Vars.numonscreenprops, numactive);
	modBoundsCheck(g_Vars.numonscreenprops == numactive, "propsSort lists every prop when all are onscreen", detail);
	modBoundsCheck(g_Vars.onscreenprops[g_Vars.numonscreenprops] == NULL
			&& g_Vars.endonscreenprops == g_Vars.onscreenprops + g_Vars.numonscreenprops,
			"propsSort ends the list with NULL", "no NULL or a wrong end pointer");

	for (i = 0; i + 1 < g_Vars.numonscreenprops; i++) {
		if (g_Vars.onscreenprops[i]->z < g_Vars.onscreenprops[i + 1]->z) {
			sorted = false;
			break;
		}
	}

	modBoundsCheck(sorted, "propsSort orders the real list furthest first", "out of order");

	// bg.c's room list takes one entry per onscreen prop
	if (g_BgRoomNumsByProp) {
		for (i = 0; i < g_BgRoomNumsByPropLen; i++) {
			g_BgRoomNumsByProp[i] = 0;
		}
	}

	snprintf(detail, sizeof(detail), "len %d, cap %d", g_BgRoomNumsByPropLen, g_PropsOnscreenCap);
	modBoundsCheck(g_BgRoomNumsByProp != NULL && g_BgRoomNumsByPropLen >= g_PropsOnscreenCap + 1,
			"the render room list has an entry for every onscreen prop", detail);

	// put the props back as they were
	for (i = 0; i < numactive; i++) {
		saved[i]->flags = flags[i];
	}

	free(flags);
	free(saved);

	// the real team and squadron lists
	snprintf(detail, sizeof(detail), "team %d (want %d), squadron %d (want %d)", g_TeamListSize, teamListSizeFor(g_NumChrSlots),
			g_SquadronListSize, squadronListSizeFor(g_NumChrSlots));
	modBoundsCheck(g_TeamList != NULL && g_SquadronList != NULL
			&& g_TeamListSize == teamListSizeFor(g_NumChrSlots)
			&& g_SquadronListSize == squadronListSizeFor(g_NumChrSlots),
			"the stage's lists are sized from its chr slots", detail);

	if (g_TeamList && g_SquadronList) {
		rebuildTeams();
		rebuildSquadrons();

		for (t = 0; t < MAX_TEAMS; t++) {
			if (modBoundsWalk(g_TeamList, g_TeamListSize, (t == 0) ? MAX_TEAMS - 1 : g_TeamList[t - 1], got, g_NumChrSlots) < 0) {
				teamsok = false;
			}
		}

		for (t = 0; t < MAX_SQUADRONS; t++) {
			if (modBoundsWalk(g_SquadronList, g_SquadronListSize, (t == 0) ? MAX_SQUADRONS - 1 : g_SquadronList[t - 1], got, g_NumChrSlots) < 0) {
				sqok = false;
			}
		}
	}

	modBoundsCheck(teamsok, "the real team list ends every team inside its size", "a team has no -2 inside the list");
	modBoundsCheck(sqok, "the real squadron list ends every squadron inside its size", "a squadron has no -2 inside the list");

	free(got);
}

/** Called every tick from modRunTick(). */
void modBoundsTestTick(void)
{
	if (!g_BoundsTest || g_BoundsDone) {
		return;
	}

	if (g_Vars.currentplayer == NULL || g_Vars.currentplayer != g_Vars.bond || g_Vars.lvframenum < 60) {
		return;
	}

	g_BoundsDone = true;
	sysLogPrintf(LOG_NOTE, "SELFTEST START bounds stage 0x%02x", g_Vars.stagenum);

	modBoundsTestSort();
	modBoundsTestCollect();
	modBoundsTestLists();
	modBoundsTestStage();

	sysLogPrintf(LOG_NOTE, "SELFTEST RESULT bounds passed %d failed %d", g_BoundsPassed, g_BoundsFailed);
	exit(g_BoundsFailed ? 1 : 0);
}
