#include <ultra64.h>
#include "game/modrandom.h"
#include "constants.h"
#include "game/cheats.h"
#include "game/chrai.h"
#include "game/game_00b820.h"
#include "game/chraction.h"
#include "game/playerreset.h"
#include "game/body.h"
#include "game/setuputils.h"
#include "bss.h"
#include "lib/memp.h"
#include "lib/rng.h"
#include "data.h"
#include "types.h"

struct prop *g_DangerousProps[12];

void resetSomeStageThings(void)
{
	s32 i;

	for (i = 0; i < ARRAYCOUNT(g_DangerousProps); i++) {
		g_DangerousProps[i] = NULL;
	}

	g_StageFlags = 0;

	g_EnemyAccuracyScale = 1;
	g_PlayerDamageRxScale = 1;
	g_PlayerDamageTxScale = 1;
	g_AttackWalkDurationScale = 1;

	g_NumChrsSeenPlayerRecently2 = 0;
}

#ifndef PLATFORM_N64
static bool stageSkipRandomBgScript(s32 id)
{
	if (id < 0x1400 && modRandomStripsStageScripts()) {
		return true;
	}

	if (g_Vars.stagenum == STAGE_AIRBASE && modRandomIsOn()) {
		switch (id) {
		case 0x1007: // Security console activation
		case 0x100d: // Laser control panel
		case 0x1011: // Laser grid timing
		case 0x1012:
		case 0x1013:
		case 0x1014:
		case 0x1015: // Reveals guards after laser overload
		case 0x1016: // Security console sounds
		case 0x1017: // First laser gate
		case 0x1020: // Weapon scanner alarm (Bond)
		case 0x1021: // Weapon scanner alarm (co-op)
		case 0x1026: // Alarm triggered by destroying the alarm box
		case 0x1028: // Deactivates both lifts until the console shuts security down
		case 0x1029: // Scripted lift door feedback
		case 0x102c: // Locks the security door on alarm
		case 0x102d: // Security console destruction
		case 0x102f: // Breaks the disguise on alarm
			return true;
		}
	}

	return false;
}
#endif

void stageAllocateBgChrs(void)
{
	bool modified;
	struct chrdata tmp;
	s32 i;

	g_BgChrs = NULL;
	g_BgChrnums = 0;
	g_NumBgChrs = 0;

	// MOD: bounds fix. Sized from the chr slots (set by chrmgrConfigure just
	// before this) instead of stock's 32 chrs a team, and made before the
	// early return below so a stage without ailists does not leave the lists
	// pointing into the last stage's freed pool.
	g_TeamListSize = teamListSizeFor(g_NumChrSlots);
	g_SquadronListSize = squadronListSizeFor(g_NumChrSlots);
	g_TeamList = mempAlloc(ALIGN16(g_TeamListSize * sizeof(s16)), MEMPOOL_STAGE);
	g_SquadronList = mempAlloc(ALIGN16(g_SquadronListSize * sizeof(s16)), MEMPOOL_STAGE);

	if (!g_StageSetup.ailists) {
		return;
	}

	// Count the number of background AI lists
	i = 0;

	while (g_StageSetup.ailists[i].list) {
		if (g_StageSetup.ailists[i].id >= 0x1000) {
			g_NumBgChrs++;
		}

		i++;
	}

	// Add one for cutscene
	g_NumBgChrs++;

	if (g_NumBgChrs > 0) {
		s32 count = 0;
		struct chrdata blankchr = {0};

		// Allocate BG chrs
		g_BgChrs = mempAlloc(ALIGN16(g_NumBgChrs * sizeof(struct chrdata)), MEMPOOL_STAGE);
		g_BgChrnums = mempAlloc(ALIGN16(g_NumBgChrs * sizeof(s16)), MEMPOOL_STAGE);

		// Initialise BG chrs
		i = 0;

		while (g_StageSetup.ailists[i].list) {
			if (g_StageSetup.ailists[i].id >= 0x1000) {
				g_BgChrs[count] = blankchr;

				if (g_StageSetup.ailists[i].id >= 0x1400) {
					g_BgChrs[count].chrnum = g_StageSetup.ailists[i].id - 0x460; // 4000+
					g_BgChrs[count].hidden2 |= CHRH2FLAG_TICKDURINGAUTOCUT;
				} else {
					g_BgChrs[count].chrnum = g_StageSetup.ailists[i].id - 0x60; // 4000+
				}

				g_BgChrs[count].ailist = g_StageSetup.ailists[i].list;
#ifndef PLATFORM_N64
				if (stageSkipRandomBgScript(g_StageSetup.ailists[i].id)) {
					g_BgChrs[count].ailist = NULL;
				}
#endif
				g_BgChrs[count].aioffset = 0;
				g_BgChrs[count].aireturnlist = -1;
				g_BgChrs[count].actiontype = ACT_NULL;
				count++;
			}

			i++;
		}

		// The final BG chr is for cutscene use
		g_BgChrs[count] = blankchr;
		g_BgChrs[count].chrnum = 4900;
		g_BgChrs[count].ailist = NULL;
		g_BgChrs[count].aioffset = 0;
		g_BgChrs[count].aireturnlist = -1;
		g_BgChrs[count].actiontype = ACT_NULL;
		g_BgChrs[count].hidden2 |= CHRH2FLAG_TICKDURINGAUTOCUT;
		count++;

		// Sort them
		do {
			modified = false;

			for (i = 0; i < count - 1; i++) {
				if (g_BgChrs[i + 1].chrnum < g_BgChrs[i].chrnum) {
					tmp = g_BgChrs[i];
					g_BgChrs[i] = g_BgChrs[i + 1];
					g_BgChrs[i + 1] = tmp;

					modified = true;
				}
			}
		} while (modified);

		// Populate chr numbers
		for (i = 0; i < count; i++) {
			g_BgChrnums[i] = g_BgChrs[i].chrnum;
		}
	}

}

void stageLoadAllAilistModels(void)
{
	u8 *cmd = g_StageSetup.ailists[0].list;
	s32 i = 0;
	u16 id;

	if (!cmd) {
		return;
	}

	do {
		while (true) {
			if (cmd[0] == AICMD_END) {
				break;
			}

			switch (cmd[0]) {
			case AICMD_DROPITEM:
				id = cmd[3] | (cmd[2] << 8);
				if (setupLoadModeldef(id));
				break;
			case AICMD_SPAWNCHRATPAD:
				bodyLoad(cmd[2]);
				if ((s8)cmd[3] >= 0 && bodyLoad((s8)cmd[3]));
				break;
			case AICMD_SPAWNCHRATCHR:
				bodyLoad(cmd[2]);
				if ((s8)cmd[3] >= 0 && bodyLoad((s8)cmd[3]));
				break;
			case AICMD_EQUIPWEAPON:
				setupLoadModeldef(cmd[3] | (cmd[2] << 8));
				if (modelmgrLoadProjectileModeldefs(cmd[4]));
				break;
			case AICMD_EQUIPHAT:
				setupLoadModeldef(cmd[3] | (cmd[2] << 8));
				break;
			}

			cmd += chraiGetCommandLength(cmd, 0);
		}

		i++;
		cmd = g_StageSetup.ailists[i].list;
	} while (cmd);
}

void func0f00c230(void)
{
	// empty
}
