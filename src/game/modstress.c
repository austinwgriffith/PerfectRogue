#include <ultra64.h>
#include "constants.h"
#include "prcompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game/cheats.h"
#include "game/lv.h"
#include "game/chr.h"
#include "game/modalarm.h"
#include "game/modoptions.h"
#include "game/modrandom.h"
#include "game/modrun.h"
#include "game/modstress.h"
#include "bss.h"
#include "lib/memp.h"
#include "lib/vars.h"
#include "data.h"
#include "types.h"
#include "system.h"
#include "video.h"

/* ---------------------------------------------------------------------
 * Guard-cap stress test, run with:
 *
 *   pd.x86_64.exe --selftest-stress N [--stress-seconds S] [--stress-stage 0x32]
 *
 * Boots, sets Perfect Random's Max Reinforcements to N and waves of 100 a second, then starts the
 * stage as a Perfect Random mission (the same path as the menu's Random
 * Mission) with the player invincible and lets the reinforcements come for S
 * seconds of real time (default 60). Every five seconds it logs how many
 * guards are up, the free chr slots, the free stage memory and the frame rate.
 *
 * The frame limiter is lifted for the run, so ms/frame is the real cost.
 *
 * It fails if the guards never get near N, if more than N + 12 are up, if the
 * game's prop list holds a prop whose chr or object has gone, or if the stage
 * pool runs dry. The frame rate is only reported: it depends on the machine,
 * and the numbers are what decide where the cap should sit.
 * --------------------------------------------------------------------- */

s32 g_StressGuards = 0;      // --selftest-stress N; 0 is off
s32 g_StressSeconds = 60;    // --stress-seconds
s32 g_StressStage = 0x32;    // --stress-stage

#define STRESS_LOADMAX   4000       // ticks to wait for the mission to load
#define STRESS_SAMPLE_US 5000000    // a log line every five seconds
#define STRESS_MINFREE   (64 * 1024) // the stage pool must never be down to this

static struct {
	s32 phase;
	s32 waited;
	s32 prevframe;
	bool reloaded;
	u64 measurestart;
	u64 last;
	u64 samplestart;
	s32 sampleframes;
	u64 samplesum;
	u64 samplemax;
	s32 totalframes;
	u64 totalsum;
	u64 halfsum; // the second half of the run only, when the guards are all up
	s32 halfframes;
	s32 peakalive;
	u32 minfree;
	s32 minfreeslots;
	bool staleseen;
	s32 passed;
	s32 failed;
} g_St;

static void modStressCheck(bool ok, const char *name, const char *detail)
{
	if (ok) {
		g_St.passed++;
		sysLogPrintf(LOG_NOTE, "SELFTEST PASS %s", name);
	} else {
		g_St.failed++;
		sysLogPrintf(LOG_NOTE, "SELFTEST FAIL %s: %s", name, detail ? detail : "");
	}
}

/** Props in the active list whose chr or object has gone. */
static s32 modStressStaleProps(void)
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

static void modStressFinish(void)
{
	char detail[160];
	f32 avgms = g_St.halfframes ? (f32)g_St.halfsum / g_St.halfframes / 1000.0f : 0.0f;
	f32 allms = g_St.totalframes ? (f32)g_St.totalsum / g_St.totalframes / 1000.0f : 0.0f;

	sysLogPrintf(LOG_NOTE, "SELFTEST INFO guards %d: peak up %d, fewest free chr slots %d, least stage memory free %u KB",
			g_StressGuards, g_St.peakalive, g_St.minfreeslots, g_St.minfree / 1024);
	sysLogPrintf(LOG_NOTE, "SELFTEST INFO guards %d: second half of the run %.1f ms/frame (%.1f fps), whole run %.1f ms/frame (%.1f fps)",
			g_StressGuards, avgms, avgms > 0 ? 1000.0f / avgms : 0.0f, allms, allms > 0 ? 1000.0f / allms : 0.0f);

	snprintf(detail, sizeof(detail), "peak %d of %d", g_St.peakalive, g_StressGuards);
	modStressCheck(g_St.peakalive * 10 >= g_StressGuards * 8, "the guards come close to the cap", detail);
	snprintf(detail, sizeof(detail), "peak %d, cap %d + 12", g_St.peakalive, g_StressGuards);
	modStressCheck(g_St.peakalive <= g_StressGuards + 12, "no more than the cap are up at once", detail);
	modStressCheck(!g_St.staleseen, "prop list sound throughout", "a prop in the active list lost its chr/object");
	snprintf(detail, sizeof(detail), "down to %u KB", g_St.minfree / 1024);
	modStressCheck(g_St.minfree >= STRESS_MINFREE, "the stage pool never runs dry", detail);

	sysLogPrintf(LOG_NOTE, "SELFTEST RESULT stress %d passed %d failed %d", g_StressGuards, g_St.passed, g_St.failed);
	exit(g_St.failed ? 1 : 0);
}

/** Called every tick from modRunTick(). */
void modStressTestTick(void)
{
	struct player *player = g_Vars.currentplayer;
	u64 now;
	u64 dt;
	s32 alive;
	s32 freeslots;
	u32 freemem;

	if (g_StressGuards <= 0 || player == NULL || player != g_Vars.bond) {
		return;
	}

	// boot level up: set the options and start the mission
	if (g_St.phase == 0) {
		if (g_Vars.lvframenum > 60) {
			sysLogPrintf(LOG_NOTE, "SELFTEST START stress %d guards, stage 0x%02x, %d seconds", g_StressGuards, g_StressStage, g_StressSeconds);
			// pd.ini caps the frame rate (FramerateLimit=240 -> 4.2 ms/frame whatever the
			// load), which hides the cost; lift it so ms/frame is what the frame takes
			videoSetFramerateLimit(5000);
			g_ModOptions.randommaxguards = g_StressGuards;
			g_ModOptions.randomspawncount = MODRANDOM_MAX_WAVE;
			g_ModOptions.randomspawnseconds = 1;
			g_St.prevframe = g_Vars.lvframenum;
			modRunStartArenaMission(g_StressStage);
			g_St.phase = 1;
		}

		return;
	}

	// the mission loading: the frame counter starts again, and Perfect Random is on
	if (g_St.phase == 1) {
		if (g_Vars.lvframenum < g_St.prevframe) {
			g_St.reloaded = true;
		}

		g_St.prevframe = g_Vars.lvframenum;

		if (g_St.reloaded && g_Vars.lvframenum > 120 && g_Vars.stagenum == g_StressStage && modRandomIsOn()) {
			cheatActivate(CHEAT_INVINCIBLE);
			g_St.phase = 2;
			g_St.minfree = 0xffffffffu;
			g_St.minfreeslots = 0x7fffffff;
			g_St.last = 0;
			sysLogPrintf(LOG_NOTE, "SELFTEST INFO mission up: %d chr slots, %d props, reserve %d, stage memory free %u KB",
					chrsGetNumSlots(), g_Vars.maxprops, modAlarmGetReserve(), mempGetStageFreeTotal() / 1024);
		} else if (++g_St.waited > STRESS_LOADMAX) {
			modStressCheck(false, "the mission starts", "Perfect Random did not come up in time");
			modStressFinish();
		}

		return;
	}

	if (lvIsPaused()) {
		g_St.last = 0;
		return;
	}

	// measuring
	player->invincible = 1;
	now = sysGetMicroseconds();

	if (g_St.measurestart == 0) {
		g_St.measurestart = now;
		g_St.samplestart = now;
	}

	if (g_St.last != 0) {
		dt = now - g_St.last;
		g_St.sampleframes++;
		g_St.samplesum += dt;

		if (dt > g_St.samplemax) {
			g_St.samplemax = dt;
		}

		g_St.totalframes++;
		g_St.totalsum += dt;

		if (now - g_St.measurestart > (u64)g_StressSeconds * 500000u) {
			g_St.halfframes++;
			g_St.halfsum += dt;
		}
	}

	g_St.last = now;

	alive = modAlarmGetAliveCount();
	freeslots = chrsGetNumFree();
	freemem = mempGetStageFreeTotal();

	if (alive > g_St.peakalive) {
		g_St.peakalive = alive;
	}

	if (freeslots < g_St.minfreeslots) {
		g_St.minfreeslots = freeslots;
	}

	if (freemem < g_St.minfree) {
		g_St.minfree = freemem;
	}

	if (modStressStaleProps() > 0) {
		g_St.staleseen = true;
	}

	if (now - g_St.samplestart >= STRESS_SAMPLE_US && g_St.sampleframes > 0) {
		f32 avgms = (f32)g_St.samplesum / g_St.sampleframes / 1000.0f;

		sysLogPrintf(LOG_NOTE, "SELFTEST INFO t=%ds up %d, free chr slots %d, stage memory free %u KB, %.1f ms/frame avg (%.1f fps), worst %.1f ms",
				(s32)((now - g_St.measurestart) / 1000000u), alive, freeslots, freemem / 1024,
				avgms, 1000.0f / avgms, (f32)g_St.samplemax / 1000.0f);
		g_St.samplestart = now;
		g_St.sampleframes = 0;
		g_St.samplesum = 0;
		g_St.samplemax = 0;
	}

	if (now - g_St.measurestart >= (u64)g_StressSeconds * 1000000u) {
		modStressFinish();
	}
}
