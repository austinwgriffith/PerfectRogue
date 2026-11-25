#include <stdlib.h>
#include <stdio.h>
#include <PR/ultratypes.h>
#include <PR/ultrasched.h>
#include <PR/os_message.h>

#include "lib/main.h"
#include "bss.h"
#include "data.h"

#include "video.h"
#include "audio.h"
#include "input.h"
#include "fs.h"
#include "romdata.h"
#include "config.h"
#include "mod.h"
#include "patch.h" // MOD: light patches - by joshuarwood (perfect_dark PR #722)
#include "system.h"
#include "game/modcareer.h"
#include "game/modoptions.h"
#include "game/modrandom.h"
#include "game/modrun.h"
#include "game/modspawnedit.h"
#include "game/modlevelgen.h"
#include "game/modlevelgenfile.h"
#include "game/modlevelgenmap.h"
#include "game/stagetable.h"
#include "utils.h"
#ifdef PD_PHYSICS
#include "ragdoll.h" // MOD: ragdoll physics
#include "guardvoice.h"
#include "gelevels.h"
#include "levelgenstage.h"
#endif

u32 g_OsMemSize = 0;
s32 g_OsMemSizeMb = 16;
u8 g_Is4Mb = 0;
s8 g_Resetting = false;
OSSched g_Sched;

OSMesgQueue g_MainMesgQueue;
OSMesg g_MainMesgBuf[32];

u8 *g_MempHeap = NULL;
u32 g_MempHeapSize = 0;

u32 g_VmNumTlbMisses = 0;
u32 g_VmNumPageMisses = 0;
u32 g_VmNumPageReplaces = 0;
u8 g_VmShowStats = 0;

s32 g_TickRateDiv = 0;
s32 g_TickExtraSleep = true;

s32 g_SkipIntro = false;

s32 g_FileAutoSelect = -1;

extern s32 g_StageNum;

s32 bootGetMemSize(void)
{
	return (s32)g_OsMemSize;
}

void *bootAllocateStack(s32 threadid, s32 size)
{
	static u8 bruh[0x1000];
	return bruh;
}

void bootCreateSched(void)
{
	osCreateMesgQueue(&g_MainMesgQueue, g_MainMesgBuf, ARRAYCOUNT(g_MainMesgBuf));
	if (osTvType == OS_TV_MPAL) {
		osCreateScheduler(&g_Sched, NULL, OS_VI_MPAL_LAN1, 1);
	} else {
		osCreateScheduler(&g_Sched, NULL, OS_VI_NTSC_LAN1, 1);
	}
}

static void gameInit(void)
{
	// MOD: headroom for unlimited decals and ragdoll corpses (EXTRA_MEMP_MB,
	// video.h) and the 4x audio heap (snd.c)
	osMemSize = (g_OsMemSizeMb + EXTRA_MEMP_MB + 4) * 1024 * 1024;

	for (s32 i = 0; i < MAX_PLAYERS; ++i) {
		struct extplayerconfig *cfg = g_PlayerExtCfg + i;
		cfg->fovzoommult = cfg->fovzoom ? cfg->fovy / 60.0f : 1.0f;
	}

	if (g_HudCenter == HUDCENTER_NORMAL) {
		g_HudAlignModeL = G_ASPECT_CENTER_EXT;
		g_HudAlignModeR = G_ASPECT_CENTER_EXT;
	} else if (g_HudCenter == HUDCENTER_WIDE) {
		g_HudAlignModeL = G_ASPECT_LEFT_EXT | G_ASPECT_WIDE_EXT;
		g_HudAlignModeR = G_ASPECT_RIGHT_EXT | G_ASPECT_WIDE_EXT;
	}
}

static void cleanup(void)
{
	sysLogPrintf(LOG_NOTE, "shutdown");
	inputSaveBinds();
	configSave(CONFIG_PATH);
#ifdef PD_PHYSICS
	ragdollSysShutdown(); // MOD: ragdoll physics
#endif
	videoShutdown();
	crashShutdown();
	// TODO: actually shut down all subsystems
}

static void mainLevelGenPrint(const char *line)
{
	sysLogPrintf(LOG_NOTE, "%s", line);
}

int main(int argc, const char **argv)
{
	sysInitArgs(argc, argv);

	if (!sysArgCheck("--no-crash-handler")) {
		crashInit();
	}

	sysInit();
	fsInit();
	configInit();
#ifdef PD_PHYSICS
	guardVoiceInit();
#endif
	if (g_LevelGenLayoutSetting >= LG_LAYOUT_ALL || LG_LAYOUT_DESIGNED(g_LevelGenLayoutSetting)) {
		g_LevelGenLayoutSetting = -1;
	}
	videoInit();
	crashReinstall(); // MOD: crash logs (the graphics driver can replace the handler)
	inputInit();
	audioInit();
	romdataInit();
	patchInit(); // MOD: light patches - by joshuarwood (perfect_dark PR #722)
	gelevelsInit(); // MOD: GoldenEye levels (mods/goldeneye, tools/geconvert)
	levelgenStageInit(); // MOD: level generator stage 0x15 (levelgenstage.c)

	bool auditspawnfacing = sysArgCheck("--audit-spawn-facing");
	if (auditspawnfacing && !sysArgGetString("--boot-stage")) {
		for (s32 i = 0; i < ARRAYCOUNT(g_Stages); i++) {
			s32 stage = g_Stages[i].id;
			if (stage > 0 && STAGE_IS_LEVEL(stage) && g_Stages[i].padsfileid > 0) {
				sysLogPrintf(LOG_NOTE, "SPAWN FACING MAP 0x%02x", stage);
			}
		}
		return 0;
	}

	g_ValidGbcRomFound = romdataCheckGbcRom();

	gameInit();

#ifdef PD_PHYSICS
	ragdollSysInit(); // MOD: ragdoll physics
#endif
	crashReinstall(); // MOD: crash logs

	if (fsGetModDir()) {
		modConfigLoad(MOD_CONFIG_FNAME);
	}

	atexit(cleanup);

	bootCreateSched();

	g_OsMemSize = osGetMemSize();

	g_MempHeapSize = g_OsMemSize;
	g_MempHeap = sysMemZeroAlloc(g_MempHeapSize);
	if (!g_MempHeap) {
		sysFatalError("Could not alloc %u bytes for memp heap.", g_MempHeapSize);
	}

	sysLogPrintf(LOG_NOTE, "memp heap at %p - %p", g_MempHeap, g_MempHeap + g_MempHeapSize);
	sysLogPrintf(LOG_NOTE, "rom  file at %p - %p", g_RomFile, g_RomFile + g_RomFileSize);

	g_SndDisabled = sysArgCheck("--no-sound");

	g_StageNum = sysArgGetInt("--boot-stage", STAGE_TITLE);

	if (g_StageNum == STAGE_TITLE && (sysArgCheck("--skip-intro") || g_SkipIntro)) {
		// shorthand for --boot-stage 0x26
		g_StageNum = STAGE_CITRAINING;
	} else if (g_StageNum < 0x01 || (g_StageNum > 0x5d
			&& (!auditspawnfacing || !STAGE_IS_LEVEL(g_StageNum) || stageGetIndex(g_StageNum) < 0))) {
		// stage num out of range
		g_StageNum = STAGE_TITLE;
	}

	if (g_StageNum != STAGE_TITLE) {
		sysLogPrintf(LOG_NOTE, "boot stage set to 0x%02x", g_StageNum);
	}

	g_FileAutoSelect = sysArgGetInt("--profile", -1);

	// Perfect Random's command line (headless testing)
	// --random-run: start a Randomizer run from the first level loaded
	g_ModRunAutoStart = sysArgCheck("--random-run");
	// --random-mission: deal the booted mission again, as the page's Random
	// Mission item arms one
	if (sysArgCheck("--random-mission")) {
		modRandomArmMission();
	}
	if (auditspawnfacing && sysArgGetString("--audit-spawn-config")) {
		modSpawnConfigUse(g_StageNum, sysArgGetString("--audit-spawn-config"));
	}
	// --selftest-spawnedit [--selftest-stage N]: the Spawn Customizer's regression test
	{
		extern s32 g_SpawnEditTestStage;
		extern bool g_SpawnConfigTest;
		g_SpawnConfigTest = sysArgCheck("--selftest-spawnconfig");
		g_SpawnEditTestStage = (sysArgCheck("--selftest-spawnedit") || g_SpawnConfigTest) ? sysArgGetInt("--selftest-stage", 0x32) : -1;
	}
	// --selftest-stress N [--stress-seconds S] [--stress-stage 0x32]: the guard-cap stress test (modstress.c)
	{
		extern s32 g_StressGuards;
		extern s32 g_StressSeconds;
		extern s32 g_StressStage;
		g_StressGuards = sysArgGetInt("--selftest-stress", 0);
		g_StressSeconds = sysArgGetInt("--stress-seconds", 60);
		g_StressStage = sysArgGetInt("--stress-stage", 0x32);
	}
	// --selftest-bounds: the bounds-fix regression test (modbounds.c); --boot-stage picks the level
	{
		extern bool g_BoundsTest;
		g_BoundsTest = sysArgCheck("--selftest-bounds");
	}
	// --levelgen-layout N: every generated level gets layout N (0 hub, 1 underground
	// lab, 2 street), whatever its seed says; for the tests, the tours and the map pictures
	if (sysArgGetString("--levelgen-layout")) {
		modLevelGenForceTheme(sysArgGetInt("--levelgen-layout", -1), -1);
	}
	// --levelgen-sky N: every generated level gets sky N (levelgenstage.c), whatever its seed says
	if (sysArgGetString("--levelgen-sky")) {
		levelgenForceSky(sysArgGetInt("--levelgen-sky", -1));
	}
	// --selftest-levelgen [--levelgen-seeds N]: the level generator's layout stage
	// regression test (modlevelgen.c). It needs no stage, so it runs here and exits.
	if (sysArgCheck("--selftest-levelgen")) {
		const s32 failed = modLevelGenSelfTest(sysArgGetInt("--levelgen-seeds", 200), mainLevelGenPrint);
		exit(failed ? 1 : 0);
	}
	// --levelgen-map SEED [--levelgen-size 0|1|2] [--levelgen-count N]: pictures of
	// generated levels from above in levelgen-maps/ (levelgenstage.c), then exit
	if (sysArgGetString("--levelgen-map")) {
		const u32 seed = (u32)strtoul(sysArgGetString("--levelgen-map"), NULL, 10);
		exit(levelgenMapCommand(seed, sysArgGetInt("--levelgen-size", 1), sysArgGetInt("--levelgen-count", 1)) ? 1 : 0);
	}
	// --selftest-levelgen-map: the map pictures (modlevelgenmap.c), no stage needed
	if (sysArgCheck("--selftest-levelgen-map")) {
		exit(modLevelGenMapSelfTest(sysArgGetInt("--levelgen-seeds", 30), mainLevelGenPrint) ? 1 : 0);
	}
	g_LevelGenTour = sysArgGetString("--levelgen-tour") != NULL; // levelgenloadtest.c: a picture of every room
	g_LevelGenLoadTest = sysArgCheck("--selftest-levelgen-load") || g_LevelGenTour; // levelgenloadtest.c
	// --selftest-levelgen-files: the generator's file writers (modlevelgenfile.c), no stage needed
	if (sysArgCheck("--selftest-levelgen-files")) {
		const s32 failed = modLevelGenFileSelfTest(mainLevelGenPrint) + levelgenLoadersSelfTest(mainLevelGenPrint)
				+ modLevelGenLayoutSelfTest(sysArgGetInt("--levelgen-seeds", 100), mainLevelGenPrint);
		exit(failed ? 1 : 0);
	}
	g_ModRunAutoHop = sysArgGetInt("--run-autohop", 0);
	g_ModRunFirstStage = sysArgGetInt("--run-stage", -1);
	g_ModRunFirstRoom = sysArgGetInt("--run-room", -1);
	if (g_FileAutoSelect >= 0) {
		sysLogPrintf(LOG_NOTE, "player profile set to %d", g_FileAutoSelect);
	}

	mainProc();

	return 0;
}

PD_CONSTRUCTOR static void gameConfigInit(void)
{
	configRegisterInt("Game.MemorySize", &g_OsMemSizeMb, 4, 2048);
	configRegisterInt("Game.CenterHUD", &g_HudCenter, 0, 2);
	configRegisterInt("Game.MenuMouseControl", &g_MenuMouseControl, 0, 1);
	configRegisterFloat("Game.ScreenShakeIntensity", &g_ViShakeIntensityMult, 0.f, 10.f);
	configRegisterInt("Game.TickRateDivisor", &g_TickRateDiv, 0, 10);
	configRegisterInt("Game.ExtraSleep", &g_TickExtraSleep, 0, 1);
	configRegisterInt("Game.SkipIntro", &g_SkipIntro, 0, 1);
#ifdef PD_PHYSICS
	{
		extern s32 g_WitnessFieldOfView; // MOD: witness field of view (chraction.c)
		configRegisterInt("Game.WitnessFieldOfView", &g_WitnessFieldOfView, 0, 1);
	}
#endif
	configRegisterInt("Game.DisableMpDeathMusic", &g_MusicDisableMpDeath, 0, 1);
	configRegisterInt("Game.GEMuzzleFlashes", &g_BgunGeMuzzleFlashes, 0, 1);
	configRegisterInt("Game.SplitscreenGlares", &g_SplitscreenGlares, 0, 1); // MOD: split-screen glares - by joshuarwood (perfect_dark PR #760)
	configRegisterInt("Game.MaxExplosions", &g_MaxExplosions, 6, 96);

	// Perfect Random, the Randomizer and the Gauntlet (src/game/modrandom.c,
	// modrun.c, modcareer.c, modalarm.c): the same keys as on the Dabs fork,
	// so a pd.ini carried over keeps its settings
	configRegisterInt("Mod.GuardsAlerted", &g_ModOptions.guardsalerted, MODALARM_OFF, MODALARM_ON);
	configRegisterInt("Mod.AlertedGuards", &g_ModOptions.alertedguards, MODALARM_GUARDS_MIN, MODALARM_GUARDS_MAX);
	configRegisterInt("Mod.GuardSpawnSpeed", &g_ModOptions.guardspawnspeed, MODALARM_SPEED_MIN, MODALARM_SPEED_MAX);
	configRegisterInt("Mod.GuardWeapons", &g_ModOptions.guardweapons, MODALARM_WEAPONS_STAGE, MODALARM_WEAPONS_RANDOM);
	configRegisterInt("Mod.AlarmSound", &g_ModOptions.alarmsound, 0, 1);
	configRegisterInt("Mod.RandomizerSeed", &g_ModOptions.randomseed, 0, S32_MAX);
	configRegisterInt("Mod.RandomSpawnSeconds", &g_ModOptions.randomspawnseconds, 1, 60);
	configRegisterInt("Mod.RandomSpawnCount", &g_ModOptions.randomspawncount, 1, MODRANDOM_MAX_WAVE);
	configRegisterInt("Mod.RandomRespawnPercent", &g_ModOptions.randomrespawnpercent, 0, 100);
	configRegisterInt("Mod.RandomMaxGuards", &g_ModOptions.randommaxguards, MODALARM_GUARDS_MIN, MODALARM_GUARDS_MAX);
	configRegisterInt("Mod.RandomGuardModel", &g_RandomGuardModels[0], 0, modRandomModelMax() - 1);
	configRegisterInt("Mod.RandomReinforcementModel", &g_RandomGuardModels[1], 0, modRandomModelMax() - 1);
	configRegisterInt("Mod.RandomPunishmentModel", &g_RandomGuardModels[2], 0, modRandomModelMax() - 1);
	configRegisterInt("Mod.RandomReturningGuardModel", &g_RandomGuardModels[3], 0, modRandomModelMax() - 1);
	configRegisterInt("Mod.RandomAllyModel", &g_RandomAllyModel, 0, modRandomModelMax());
	configRegisterInt("Mod.RandomCivilianModel", &g_RandomExtraModels[0], 0, modRandomModelMax());
	configRegisterInt("Mod.RandomKillTargetModel", &g_RandomExtraModels[1], 0, modRandomModelMax());
	configRegisterInt("Mod.RandomKOTargetModel", &g_RandomExtraModels[2], 0, modRandomModelMax());
	configRegisterInt("Mod.RandomizerVersion", &g_ModOptions.randomversion, 1, S32_MAX);
	configRegisterInt("Mod.RandomizerEndless", &g_ModOptions.randomendless, 0, 1);
	configRegisterInt("Mod.EndlessBest", &g_ModOptions.endlessbest, 0, S32_MAX);
	configRegisterInt("Mod.RunMapPool", &g_ModOptions.runpool, 0, MODRUN_POOL_MAX);
	configRegisterInt("Mod.RunDifficulty", &g_ModOptions.rundifficulty, DIFF_A, DIFF_PA);
	configRegisterInt("Mod.RunSealRooms", &g_ModOptions.runseal, 0, 1);
	configRegisterInt("Mod.RunRoomSize", &g_ModOptions.runroomsize, MODRUN_ROOMS_NORMAL, MODRUN_ROOMS_LARGE);
	configRegisterInt("Mod.RunTintOutside", &g_ModOptions.runtint, 0, 1);
	configRegisterInt("Mod.RunGuardsWalkIn", &g_ModOptions.runwalkin, 0, 1);
	configRegisterInt("Mod.RunBestScore", &g_ModOptions.runbestscore, 0, S32_MAX);
	configRegisterInt("Mod.RunBestRooms", &g_ModOptions.runbestrooms, 0, S32_MAX);
	configRegisterInt("Mod.LevelGenSize", &g_LevelGenSizeSetting, 0, 2);
	configRegisterInt("Mod.LevelGenLayout", &g_LevelGenLayoutSetting, -1, S32_MAX);
	configRegisterInt("Mod.GauntletLength", &g_ModCareerLength, MODCAREER_UNLIMITED, MODCAREER_MAXMISSIONS);
	configRegisterInt("Mod.GauntletBest", &g_ModCareerBest, 0, S32_MAX);
	configRegisterInt("Mod.HideUnfunMaps", &g_ModCareerHideUnfunMaps, 0, 1);
	configRegisterInt("Mod.GauntletEventsOff", &g_ModCareerEventsOff, 0, MODCAREER_EVENT_SETTINGS_MASK);
	configRegisterInt("Mod.GauntletAlienWarDebug", &g_ModCareerDebugAlienWar, 0, 1);
	configRegisterString("Mod.GauntletOff", g_ModCareerOff, sizeof(g_ModCareerOff));
	configRegisterInt("Mod.GauntletStepGuards", &g_ModCareerStepGuards, 0, 4);
	configRegisterInt("Mod.GauntletStepSpawn", &g_ModCareerStepSpawn, 0, 30);
	configRegisterInt("Mod.GauntletStepAccuracy", &g_ModCareerStepAccuracy, 0, 100);
	configRegisterInt("Mod.GauntletStepAkimbo", &g_ModCareerStepAkimbo, 0, 100);
	configRegisterInt("Mod.GauntletStepRespawn", &g_ModCareerStepRespawn, 0, 100);
	configRegisterInt("Mod.GauntletStartPreset", &g_ModCareerStartPreset, 0, 3);
	configRegisterInt("Mod.GauntletStartGuards", &g_ModCareerStartGuards, 0, 10);
	configRegisterInt("Mod.GauntletStartAccuracy", &g_ModCareerStartAccuracy, 0, 10);
	configRegisterInt("Mod.GauntletStartAkimbo", &g_ModCareerStartAkimbo, 0, 10);
	configRegisterInt("Mod.GauntletStartRespawn", &g_ModCareerStartRespawn, 0, 4);
	configRegisterInt("Mod.GauntletStepDamage", &g_ModCareerStepDamage, 0, 100);
	configRegisterInt("Mod.RandomDebug", &g_ModRandomDebug, 0, 1);
	configRegisterInt("Mod.RandomDebugTint", &g_ModRandomDebugTint, 0, 1);
	configRegisterInt("Mod.RandomDebugRadar", &g_ModRandomDebugRadar, 0, 1);
	configRegisterInt("Mod.RandomDebugDefendersRadar", &g_ModRandomDebugDefendersRadar, 0, 1);
	configRegisterInt("Mod.RandomDebugStateCount", &g_ModRandomDebugStateCount, 0, 1);
	configRegisterInt("Mod.RandomDebugCloak", &g_ModRandomDebugCloak, 0, 1);
	configRegisterInt("Mod.RandomDebugShotguns", &g_ModRandomDebugShotguns, 0, 1);
	configRegisterInt("Mod.RandomDebugMaxGrenades", &g_ModRandomDebugMaxGrenades, 0, 1);
	for (s32 j = 0; j < MAX_PLAYERS; ++j) {
		const s32 i = j + 1;
		configRegisterFloat(strFmt("Game.Player%d.FovY", i), &g_PlayerExtCfg[j].fovy, 5.f, 175.f);
		configRegisterInt(strFmt("Game.Player%d.FovAffectsZoom", i), &g_PlayerExtCfg[j].fovzoom, 0, 1);
		configRegisterInt(strFmt("Game.Player%d.MouseAimMode", i), &g_PlayerExtCfg[j].mouseaimmode, 0, 1);
		configRegisterFloat(strFmt("Game.Player%d.MouseAimSpeedX", i), &g_PlayerExtCfg[j].mouseaimspeedx, 0.f, 10.f);
		configRegisterFloat(strFmt("Game.Player%d.MouseAimSpeedY", i), &g_PlayerExtCfg[j].mouseaimspeedy, 0.f, 10.f);
		configRegisterFloat(strFmt("Game.Player%d.RadialMenuSpeed", i), &g_PlayerExtCfg[j].radialmenuspeed, 0.f, 10.f);
		configRegisterFloat(strFmt("Game.Player%d.CrosshairSway", i), &g_PlayerExtCfg[j].crosshairsway, 0.f, 10.f);
		configRegisterFloat(strFmt("Game.Player%d.CrosshairEdgeBoundary", i), &g_PlayerExtCfg[j].crosshairedgeboundary, 0.0f, 1.0f);
		configRegisterInt(strFmt("Game.Player%d.CrouchMode", i), &g_PlayerExtCfg[j].crouchmode, 0, CROUCHMODE_TOGGLE_ANALOG);
		configRegisterInt(strFmt("Game.Player%d.ExtendedControls", i), &g_PlayerExtCfg[j].extcontrols, 0, 1);
		configRegisterUInt(strFmt("Game.Player%d.CrosshairColour", i), &g_PlayerExtCfg[j].crosshaircolour, 0, 0xFFFFFFFF);
		configRegisterUInt(strFmt("Game.Player%d.CrosshairSize", i), &g_PlayerExtCfg[j].crosshairsize, 0, 4);
		configRegisterInt(strFmt("Game.Player%d.CrosshairHealth", i), &g_PlayerExtCfg[j].crosshairhealth, 0, CROSSHAIR_HEALTH_ON_WHITE);
		configRegisterInt(strFmt("Game.Player%d.UseKeyReloads", i), &g_PlayerExtCfg[j].usereloads, 0, false);
	}
}
