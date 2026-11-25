#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "data.h"
#include "bss.h"
#include "fs.h"
#include "system.h"
#include "romdata.h"
#include "gelevels.h"
#include "levelgenstage.h"
#include "game/file.h"
#include "game/stagetable.h"
#include "game/modrun.h"
#include "game/modoptions.h"
#include "game/hudmsg.h"
#include "game/dlights.h"
#include "lib/rng.h"
#include "game/modlevelgenfile.h"
#include "game/modlevelgen.h"
#include "game/modlevelgenmap.h"

/**
 * The level generator's stage. See levelgenstage.h.
 *
 * The four files live on disk under mods/levelgen/files/ and are added to the
 * file table the way GoldenEye's are (romdataFileAdd()); making a new level is
 * writing them again and telling the file table to read them again
 * (romdataFileReload()). Keeping them on disk costs a few milliseconds and
 * makes a bad level easy to look at afterwards.
 */

#define LEVELGEN_DIR   "mods/levelgen"
#define LEVELGEN_FILES LEVELGEN_DIR "/files"

// A row of g_Stages after the stock 61. GoldenEye uses the first ones
// (gelevels.c), so this is the last of the 32.
#define LEVELGEN_ROW (61 + 32 - 1)

// The stage pool. The box needs little, but the generator's later levels are
// big, and the pool is only reserved while the stage runs.
#define LEVELGEN_ALLOC "-ml0 -me0 -mgfx200 -mgfxtra80 -mvtx200 -ma700"

enum {
	LG_BG,
	LG_TILES,
	LG_PADS,
	LG_SETUP,
	LG_NUMFILES
};

static const char *g_LgFileNames[LG_NUMFILES] = {
	"bg_lgen.seg",
	"bg_lgen_tilesZ",
	"bg_lgen_padsZ",
	"Ump_setuplgenZ",
};

static s32 g_LgFileNums[LG_NUMFILES];
static bool g_LgRegistered = false;
static u32 g_LgSeed = 0;
static s32 g_LgSize = 0;
static s32 g_LgHalfX = 0;
static s32 g_LgHalfZ = 0;

static bool levelgenWriteFile(s32 which, const LgBuf *buf)
{
	char path[FS_MAXPATH + 1];
	FILE *fp;
	bool ok;

	snprintf(path, sizeof(path), LEVELGEN_FILES "/%s", g_LgFileNames[which]);
	fp = fsFileOpenWrite(path);

	if (!fp) {
		sysLogPrintf(LOG_ERROR, "levelgen: cannot write %s", path);
		return false;
	}

	ok = fwrite(buf->data, 1, buf->size, fp) == buf->size;
	fsFileFree(fp);

	if (!ok) {
		sysLogPrintf(LOG_ERROR, "levelgen: short write to %s", path);
	}

	return ok;
}

/* The layout made last; the load test reads it (levelgenGetLayout()) */
static LgLayout *g_LgLayout = NULL;

/**
 * Phase 1: the generator's layout for the seed and size (modlevelgen.c), then
 * its files (modlevelgenfile.c). The box of Phase 0 is still there for the
 * file tests (modLevelGenBoxFiles()).
 */
static bool g_LgFilesAhead = false;   // the files on disk are a level that has not loaded yet

static bool levelgenBuild(u32 seed, s32 size, LgFiles *files, s32 *halfx, s32 *halfz)
{
	LgParams prm;
	LgBoxParams th;
	char err[160];

	if (size < 0 || size >= LG_SIZE_COUNT) {
		size = LEVELGEN_SIZE_MEDIUM;
	}

	if (g_LgLayout == NULL) {
		g_LgLayout = malloc(sizeof(*g_LgLayout));

		if (g_LgLayout == NULL) {
			sysLogPrintf(LOG_ERROR, "levelgen: out of memory for the layout");
			return false;
		}
	}

	prm.seed = seed;
	prm.size = (u8)size;

	if (!modLevelGenBuild(g_LgLayout, &prm, err, sizeof(err))) {
		sysLogPrintf(LOG_ERROR, "levelgen: seed %u size %d: no layout: %s", seed, size, err);
		return false;
	}

	modLevelGenBoxDefaults(&th);

	if (!modLevelGenLayoutFiles(g_LgLayout, &th, files, err, sizeof(err))) {
		sysLogPrintf(LOG_ERROR, "levelgen: seed %u size %d: %s", seed, size, err);
		return false;
	}

	if (!modLevelGenLayoutCheck(g_LgLayout, files, err, sizeof(err))) {
		sysLogPrintf(LOG_ERROR, "levelgen: seed %u size %d: files fail their own check: %s", seed, size, err);
		modLevelGenFilesFree(files);
		return false;
	}

	*halfx = g_LgLayout->gw * LG_CELL / 2;
	*halfz = g_LgLayout->gd * LG_CELL / 2;

	sysLogPrintf(LOG_NOTE, "levelgen: seed %u size %d: %d areas (%d rooms), %d portals, %d pads, %d x %d cells; bg %u bytes",
			seed, size, g_LgLayout->numareas, g_LgLayout->numrooms, g_LgLayout->numportals, g_LgLayout->numpads,
			g_LgLayout->gw, g_LgLayout->gd, files->bg.size);

	return true;
}

/**
 * The level from above (modlevelgenmap.c), next to its files as
 * mods/levelgen/map.png, so the level being played can be looked at.
 */
static void levelgenWriteMap(void)
{
	LgImage img;
	uint8_t *png = NULL;
	u32 size = 0;
	FILE *fp;

	if (g_LgLayout == NULL || !modLevelGenMapRender(g_LgLayout, 0, 1, &img)) {
		return;
	}

	if (modLevelGenImagePng(&img, &png, &size)) {
		fp = fsFileOpenWrite(LEVELGEN_DIR "/map.png");

		if (fp) {
			fwrite(png, 1, size, fp);
			fsFileFree(fp);
			sysLogPrintf(LOG_NOTE, "levelgen: map written to " LEVELGEN_DIR "/map.png");
		}

		free(png);
	}

	modLevelGenImageFree(&img);
}

/* The pads after the layout's own are props' (the doors', then the crates'): nobody is put down on those */
bool levelgenPadIsProp(s32 padnum)
{
	return levelgenIsStage(g_Vars.stagenum) && g_LgLayout != NULL && padnum >= g_LgLayout->numpads;
}

const LgLayout *levelgenGetLayout(void)
{
	return g_LgLayout;
}

bool levelgenNpcPathHasFloor(const struct coord *from, const struct coord *to)
{
	const LgLayout *lg = g_LgLayout;
	if (!levelgenIsStage(g_Vars.stagenum) || !lg) {
		return true;
	}

	const f32 dx = to->x - from->x, dz = to->z - from->z;
	const s32 steps = (s32)ceilf(fmaxf(fabsf(dx), fabsf(dz)) / (LG_CELL * 0.25f));
	f32 ground = from->y;

	/* A clear collision ray can still cross a street or a breakable glass
	 * span. Trace continuous walkable floor at the current storey instead. */
	for (s32 i = 0; i <= steps; i++) {
		const f32 t = steps ? (f32)i / steps : 0.0f;
		const f32 x = from->x + t * dx, z = from->z + t * dz;
		const s32 gx = (s32)floorf((x - lg->originx) / LG_CELL);
		const s32 gz = (s32)floorf((z - lg->originz) / LG_CELL);
		const f32 low = ground - (i == 0 ? 170.0f : 45.0f);
		const f32 high = ground + 45.0f;
		f32 support = low - 1.0f;

		if (gx >= 0 && gz >= 0 && gx < lg->gw && gz < lg->gd) {
			for (s32 layer = 0; layer < LG_MAXLAYERS; layer++) {
				const s32 ai = LG_CELLS(lg, layer)[gz][gx] - 1;
				if (ai < 0 || LG_NOWALK(&lg->areas[ai]) || lg->areas[ai].special == LG_SPECIAL_WINDOW) {
					continue;
				}
				const f32 y = modLevelGenFloorAt(lg, ai, (s32)x, (s32)z);
				if (y >= low && y <= high && y > support) {
					support = y;
				}
			}
		}

		if (support < low) {
			/* A cylinder can overhang a ledge. Allow the first sample
			 * to be unsupported so an NPC can step back onto the roof. */
			if (i == 0 && steps) {
				ground = from->y - 90.0f;
				continue;
			}
			return false;
		}
		ground = support;
	}

	return to->y >= ground - 35.0f && to->y <= ground + 170.0f;
}

bool levelgenPrepare(u32 seed, s32 size)
{
	LgFiles files;
	const LgBuf *bufs[LG_NUMFILES];
	s32 i;
	s32 hx = 0, hz = 0;

	if (!g_LgRegistered) {
		sysLogPrintf(LOG_ERROR, "levelgen: the stage is not registered");
		return false;
	}

	if (!levelgenBuild(seed, size, &files, &hx, &hz)) {
		return false;
	}

	bufs[LG_BG] = &files.bg;
	bufs[LG_TILES] = &files.tiles;
	bufs[LG_PADS] = &files.pads;
	bufs[LG_SETUP] = &files.setup;

	// from here until the next stage loads its bg, the files on disk are not the level on screen's
	g_LgFilesAhead = true;

	for (i = 0; i < LG_NUMFILES; i++) {
		if (!levelgenWriteFile(i, bufs[i])) {
			modLevelGenFilesFree(&files);
			return false;
		}
	}

	modLevelGenFilesFree(&files);
	levelgenWriteMap();

	// read them again, and forget the sizes the last level left behind
	for (i = 0; i < LG_NUMFILES; i++) {
		romdataFileReload(g_LgFileNums[i]);
	}

	filesStop(4);

	g_LgSeed = seed;
	g_LgSize = size;
	g_LgHalfX = hx;
	g_LgHalfZ = hz;
	{
		char name[64];

		sysLogPrintf(LOG_NOTE, "levelgen: made level \"%s\" for seed %u size %d (stage 0x%02x)", levelgenLevelName(name, sizeof(name)), seed, size, LEVELGEN_STAGENUM);
	}
	return true;
}

s32 g_LevelGenSizeSetting = LEVELGEN_SIZE_MEDIUM;
s32 g_LevelGenLayoutSetting = -1;

// The level's name as the player sees it: "(PR) <Layout>, <Aesthetic>", from
// the level made last (before any is made: the themes of seed 0)
const char *levelgenLevelName(char *buf, s32 buflen)
{
	LgTheme theme = { 0, 0 };

	if (g_LgLayout) {
		theme = g_LgLayout->theme;
	}

	return modLevelGenLevelName(theme, buf, buflen);
}

const char *levelgenSizeName(s32 size)
{
	static const char *names[3] = { "Small", "Medium", "Large" };

	return size >= 0 && size < 3 ? names[size] : "?";
}

u32 levelgenNewSeed(void)
{
	u32 seed = g_ModOptions.randomseed ? (u32)g_ModOptions.randomseed : (u32)rngRandom();

	seed &= 0x7fffffff;
	return seed ? seed : 1;
}

// True while a generated level is on screen and the files on disk are already the next
// level's: the game loads rooms as they come into view, and one loaded now would be read
// from the new bg file at the old level's offsets (it crashed the load test about one run
// in ten, 2026-11-14). bgLoadRoom() skips the load; bgReset() ends the hold.
bool levelgenRoomLoadsHeld(void)
{
	return g_LgFilesAhead && g_Vars.stagenum == LEVELGEN_STAGENUM;
}

void levelgenRoomLoadsResume(void)
{
	g_LgFilesAhead = false;
}

/**
 * Claude Missions: a designed level (LG_LAYOUT_PIER...) as a Perfect Random
 * mission. The level is the same every time; the mission on it is not.
 */
bool levelgenStartDesigned(s32 layout)
{
	bool ok;

	if (!LG_LAYOUT_DESIGNED(layout)) {
		return false;
	}
	modLevelGenForceTheme(layout, -1);
	ok = levelgenStart(17, LG_SIZE_MEDIUM);
	modLevelGenForceTheme(-1, -1);
	return ok;
}

bool levelgenStart(u32 seed, s32 size)
{
	s32 keep;

	seed &= 0x7fffffff;

	if (seed == 0) {
		seed = 1;
	}

	if (!levelgenPrepare(seed, size)) {
		return false;
	}

	// The mission is dealt from the level's own seed, so that one number brings
	// back the whole level (Seed > Keep This Run)
	keep = g_ModOptions.randomseed;
	g_ModOptions.randomseed = (s32)seed;
	modRunStartArenaMission(LEVELGEN_STAGENUM);
	g_ModOptions.randomseed = keep;
	return true;
}

/** Once, a couple of seconds into a generated level: which level it is. */
void levelgenTick(void)
{
	static bool shown = false;

	if (!levelgenIsStage(g_Vars.stagenum) || g_Vars.currentplayer != g_Vars.bond) {
		return;
	}

	if (g_Vars.lvframenum < 90) {
		shown = false;
	}

	if (g_Vars.lvframenum < 90) {
		return;
	}

	if (!shown) {
		char text[128], name[64];

		shown = true;
		snprintf(text, sizeof(text), "%s: seed %u, %s\n", levelgenLevelName(name, sizeof(name)), g_LgSeed, levelgenSizeName(g_LgSize));
		hudmsgCreateWithFlags(text, HUDMSGTYPE_DEFAULT, HUDMSGFLAG_ONLYIFALIVE | HUDMSGFLAG_ALLOWDUPES);
		sysLogPrintf(LOG_NOTE, "levelgen: %s", text);
	}
}

void levelgenStageInit(void)
{
	const s32 tmpl = stageGetIndex(STAGE_MP_SKEDAR);
	s32 i;

	g_LgRegistered = false;

	if (tmpl < 0) {
		return;
	}

	if (gelevelsCount() >= GELEVELS_MAX_STAGES || stageGetIndex(LEVELGEN_STAGENUM) >= 0) {
		sysLogPrintf(LOG_WARNING, "levelgen: no free stage row or number, the generated level is off");
		return;
	}

	fsCreateDir("mods");
	fsCreateDir(LEVELGEN_DIR);
	fsCreateDir(LEVELGEN_FILES);

	// the files have to exist before they are added: romdataFileAdd() is cheap but
	// a missing file is only found when the stage loads
	{
		LgFiles files;
		const LgBuf *bufs[LG_NUMFILES];

		s32 hx, hz;

		if (!levelgenBuild(1, LEVELGEN_SIZE_MEDIUM, &files, &hx, &hz)) {
			return;
		}

		bufs[LG_BG] = &files.bg;
		bufs[LG_TILES] = &files.tiles;
		bufs[LG_PADS] = &files.pads;
		bufs[LG_SETUP] = &files.setup;

		for (i = 0; i < LG_NUMFILES; i++) {
			if (!levelgenWriteFile(i, bufs[i])) {
				modLevelGenFilesFree(&files);
				return;
			}
		}

		modLevelGenFilesFree(&files);
	}

	for (i = 0; i < LG_NUMFILES; i++) {
		char name[96];
		char path[FS_MAXPATH + 1];

		snprintf(name, sizeof(name), "levelgen/%s", g_LgFileNames[i]);
		snprintf(path, sizeof(path), LEVELGEN_FILES "/%s", g_LgFileNames[i]);
		g_LgFileNums[i] = romdataFileAdd(name, path);

		if (g_LgFileNums[i] <= 0) {
			sysLogPrintf(LOG_ERROR, "levelgen: no file slot for %s", name);
			return;
		}
	}

	// Skedar's arena row for the rest (lighting, scale, alarm), as GoldenEye's levels do
	g_Stages[LEVELGEN_ROW] = g_Stages[tmpl];
	g_Stages[LEVELGEN_ROW].id = LEVELGEN_STAGENUM;
	g_Stages[LEVELGEN_ROW].bgfileid = g_LgFileNums[LG_BG];
	g_Stages[LEVELGEN_ROW].tilefileid = g_LgFileNums[LG_TILES];
	g_Stages[LEVELGEN_ROW].padsfileid = g_LgFileNums[LG_PADS];
	g_Stages[LEVELGEN_ROW].setupfileid = g_LgFileNums[LG_SETUP];
	g_Stages[LEVELGEN_ROW].mpsetupfileid = g_LgFileNums[LG_SETUP];

	g_LgRegistered = true;
	sysLogPrintf(LOG_NOTE, "levelgen: stage 0x%02x registered (files %d %d %d %d)", LEVELGEN_STAGENUM,
			g_LgFileNums[LG_BG], g_LgFileNums[LG_TILES], g_LgFileNums[LG_PADS], g_LgFileNums[LG_SETUP]);
}

bool levelgenLiftsAuto(void)
{
	return g_LgLayout != NULL && g_LgLayout->theme.layout == LG_LAYOUT_PIPEWORKS;
}

s32 levelgenIsStage(s32 stagenum)
{
	return g_LgRegistered && stagenum == LEVELGEN_STAGENUM;
}

const char *levelgenAllocation(s32 stagenum)
{
	return levelgenIsStage(stagenum) ? LEVELGEN_ALLOC : NULL;
}

u32 levelgenGetSeed(void)
{
	return g_LgSeed;
}

s32 levelgenGetSize(void)
{
	return g_LgSize;
}

s32 levelgenPadsFileNum(void)
{
	return g_LgFileNums[LG_PADS];
}

s32 levelgenGetHalfX(void)
{
	return g_LgHalfX;
}

s32 levelgenGetHalfZ(void)
{
	return g_LgHalfZ;
}

/**
 * pd.x86_64.exe --levelgen-map SEED [--levelgen-size 0|1|2] [--levelgen-count N]
 *
 * Pictures of generated levels from above, without starting the game: one PNG
 * per seed (SEED, SEED+1, ...) in levelgen-maps/ next to the exe, and with a
 * count above one a sheet of them all side by side. Returns the number of
 * levels that could not be made or written.
 */
#ifdef _WIN32
#include <direct.h>
#define LEVELGEN_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define LEVELGEN_MKDIR(p) mkdir(p, 0777)
#endif

s32 levelgenMapCommand(u32 seed, s32 size, s32 count)
{
	static LgLayout lg;
	LgImage *small;
	u32 *seeds;
	char path[128];
	char err[160];
	s32 failed = 0;
	s32 made = 0;
	s32 i;

	if (size < 0 || size >= LG_SIZE_COUNT) {
		size = LG_SIZE_MEDIUM;
	}

	if (count < 1) {
		count = 1;
	}

	if (count > 64) {
		count = 64;
	}

	LEVELGEN_MKDIR("levelgen-maps");
	small = calloc(count, sizeof(*small));
	seeds = calloc(count, sizeof(*seeds));

	for (i = 0; i < count; i++) {
		LgParams prm;
		LgImage img;

		prm.seed = seed + (u32)i;
		prm.size = (u8)size;

		if (!modLevelGenBuild(&lg, &prm, err, sizeof(err))) {
			printf("seed %u: no level: %s\n", prm.seed, err);
			failed++;
			continue;
		}

		snprintf(path, sizeof(path), "levelgen-maps/map-%u-%s.png", prm.seed, levelgenSizeName(size));

		if (modLevelGenMapRender(&lg, 0, 1, &img) && modLevelGenImageWritePng(&img, path)) {
			printf("%s\n", path);
		} else {
			printf("%s: could not write\n", path);
			failed++;
		}

		modLevelGenImageFree(&img);

		if (count > 1 && small && seeds && modLevelGenMapRender(&lg, size == LG_SIZE_LARGE ? 4 : 6, 0, &small[made])) {
			seeds[made++] = prm.seed;
		}
	}

	if (made > 1) {
		LgImage sheet;
		s32 cols = 1;

		while (cols * cols < made) {
			cols++;
		}

		snprintf(path, sizeof(path), "levelgen-maps/sheet-%u-%s-x%d.png", seed, levelgenSizeName(size), made);

		if (modLevelGenImageSheet(small, seeds, made, cols, &sheet) && modLevelGenImageWritePng(&sheet, path)) {
			printf("%s\n", path);
		} else {
			failed++;
		}

		modLevelGenImageFree(&sheet);
	}

	for (i = 0; small && i < made; i++) {
		modLevelGenImageFree(&small[i]);
	}

	free(small);
	free(seeds);
	fflush(stdout);
	return failed;
}

/*
 * Skies (2026-11-15, the user: "Add skyboxes to the map generation. Skyboxes
 * should also be random."). Every generated level gets one of these, dealt
 * from its seed (so a seed always looks the same): the sky's colour, its
 * cloud layer (one of the game's two cloud textures, tinted, at a height and
 * scale) and its suns with their lens flares. No fog: it would grey out the
 * far side of the larger levels. env.c asks here before its own tables.
 */
#define RGBC(col) (col) >> 16, ((col) >> 8) & 0xff, (col) & 0xff
#define LGSKY_SUNS(arr) ARRAYCOUNT(arr), arr
#define LGSKY_NOSUN 0, NULL

static struct sun g_LgSunNoon[]    = { { 1, RGBC(0xffffff), { -1000000,  600000,   300000 }, 20, 48 } };
static struct sun g_LgSunLow[]     = { { 1, RGBC(0xffb060), {  1900000,  250000, -1400000 }, 26, 60 } };
static struct sun g_LgSunDawn[]    = { { 1, RGBC(0xffd7f2), { -1200000,  200000,   150000 }, 30, 60 } };
static struct sun g_LgSunWhite[]   = { { 0, RGBC(0xfff4e0), {   400000,  900000,  -600000 }, 16, 40 } };
static struct sun g_LgSunsAlien[]  = {
	{ 1, RGBC(0xffffff), {  -400000,  600000,  1000000 }, 20, 48 },
	{ 1, RGBC(0xffd0a0), {        0, 1000000,  1000000 }, 25, 60 },
	{ 1, RGBC(0xa0c0ff), {   400000,  600000,  1000000 }, 15, 36 },
};
static struct sun g_LgSunMoon[]    = { { 0, RGBC(0xc8d8ff), {   600000,  700000,   800000 }, 12, 30 } };

static const char *g_LgSkyNames[] = {
	"Clear Day", "Fair Weather", "Overcast", "Storm", "Sunset", "Dawn",
	"Dusk", "Night", "City Night", "Alien Suns", "Toxic Haze", "Arctic",
};

// stage (unused), near, far, opa, xlu, ref, sky, suns, clouds (on, colour, scale, type),
// water (on, colour, height, type), clouds_height, transparency
static struct nofogenvironment g_LgSkies[] = {
	{ 0, 15, 20000, 0, 0, 0, RGBC(0x46a0ff), LGSKY_SUNS(g_LgSunNoon),   1, RGBC(0xffffff), 5000, 0, 0, RGBC(0x000000), -5000, 0, 0, 1 },
	{ 0, 15, 20000, 0, 0, 0, RGBC(0x65b2ff), LGSKY_SUNS(g_LgSunWhite),  1, RGBC(0xf0f4ff), 3500, 1, 0, RGBC(0x000000), -5000, 0, 0, 1 },
	{ 0, 15, 20000, 0, 0, 0, RGBC(0x7c8894), LGSKY_NOSUN,               1, RGBC(0xc4c8cc), 3000, 1, 0, RGBC(0x000000), -5000, 0, 0, 1 },
	{ 0, 15, 20000, 0, 0, 0, RGBC(0x2a2e34), LGSKY_NOSUN,               1, RGBC(0x5a6068), 2500, 0, 0, RGBC(0x000000), -5000, 0, 0, 1 },
	{ 0, 15, 20000, 0, 0, 0, RGBC(0xc8582a), LGSKY_SUNS(g_LgSunLow),    1, RGBC(0xffc070), 4500, 0, 0, RGBC(0x000000), -5000, 0, 0, 1 },
	{ 0, 15, 20000, 0, 0, 0, RGBC(0xc890a8), LGSKY_SUNS(g_LgSunDawn),   1, RGBC(0xffe4ec), 4000, 1, 0, RGBC(0x000000), -5000, 0, 0, 1 },
	{ 0, 15, 20000, 0, 0, 0, RGBC(0x2d3e60), LGSKY_NOSUN,               1, RGBC(0xe08060), 5000, 0, 0, RGBC(0x000000), -5000, 0, 0, 1 },
	{ 0, 15, 20000, 0, 0, 0, RGBC(0x020610), LGSKY_SUNS(g_LgSunMoon),   1, RGBC(0x283048), 5000, 1, 0, RGBC(0x000000), -5000, 0, 0, 1 },
	{ 0, 15, 20000, 0, 0, 0, RGBC(0x080404), LGSKY_NOSUN,               1, RGBC(0x50280a), 5000, 0, 0, RGBC(0x000000), -5000, 0, 0, 1 },
	{ 0, 15, 20000, 0, 0, 0, RGBC(0x6565ff), LGSKY_SUNS(g_LgSunsAlien), 1, RGBC(0xffffff), 5000, 0, 0, RGBC(0x000000), -5000, 0, 0, 1 },
	{ 0, 15, 20000, 0, 0, 0, RGBC(0x304010), LGSKY_NOSUN,               1, RGBC(0x9aaa40), 3500, 1, 0, RGBC(0x000000), -5000, 0, 0, 1 },
	{ 0, 15, 20000, 0, 0, 0, RGBC(0xb0c8e0), LGSKY_SUNS(g_LgSunWhite),  1, RGBC(0xffffff), 4000, 1, 0, RGBC(0x000000), -5000, 0, 0, 1 },
};

static s32 g_LgForceSky = -1;

void levelgenForceSky(s32 sky)
{
	g_LgForceSky = sky >= 0 && sky < (s32)ARRAYCOUNT(g_LgSkies) ? sky : -1;
}

s32 levelgenSkyCount(void)
{
	return ARRAYCOUNT(g_LgSkies);
}

// the sky of the level made last: forced, or dealt from its seed
s32 levelgenGetSky(void)
{
	u32 h = g_LgSeed * 2654435761u;

	if (g_LgForceSky >= 0) {
		return g_LgForceSky;
	}

	// Claude Missions (2026-11-16): a designed level has its own sky. Pier 17: a moonlit night.
	if (g_LgLayout && g_LgLayout->theme.layout == LG_LAYOUT_PIER) {
		return 7;
	}
	// Hollow Creek: a fair day
	if (g_LgLayout && g_LgLayout->theme.layout == LG_LAYOUT_TRAIL) {
		return 1;
	}

	// the random docks and backwoods: skies that suit them (2026-11-17)
	if (g_LgLayout && (g_LgLayout->theme.layout == LG_LAYOUT_DOCKS || g_LgLayout->theme.layout == LG_LAYOUT_BACKWOODS)) {
		static const u8 docks[] = { 7, 6, 2, 4, 8 };
		static const u8 woods[] = { 0, 1, 2, 5, 4 };
		const u32 k = (h >> 7) % 5;

		return g_LgLayout->theme.layout == LG_LAYOUT_DOCKS ? docks[k] : woods[k];
	}

	// the pipeworks: indoors (its roof shuts the sky out); a night sky if seen at all
	if (g_LgLayout && g_LgLayout->theme.layout == LG_LAYOUT_PIPEWORKS) {
		return 8;
	}

	// the mall: a sunset, dusk, a fair day or the city's night over the car
	// park and through the skylight (2026-11-19)
	if (g_LgLayout && g_LgLayout->theme.layout == LG_LAYOUT_MALL) {
		static const u8 mall[] = { 4, 6, 1, 8 };

		return mall[(h >> 7) % 4];
	}

	// the penthouse: night (its lamps lit, the city's lights below); the
	// building site: by day (2026-11-17)
	if (g_LgLayout && LG_LAYOUT_HIGHRISE(g_LgLayout->theme.layout)) {
		static const u8 pent[] = { 7, 6, 8, 2 };
		static const u8 site[] = { 0, 1, 5, 4 };
		const u32 k = (h >> 7) % 4;

		return g_LgLayout->theme.layout == LG_LAYOUT_PENTHOUSE ? pent[k] : site[k];
	}

	// the market town: a fine day, or a golden evening (2026-11-18)
	if (g_LgLayout && g_LgLayout->theme.layout == LG_LAYOUT_PLAZA) {
		static const u8 town[] = { 0, 1, 4, 11 };

		return town[(h >> 7) % 4];
	}

	h ^= h >> 15;
	h *= 0x2c1b3c6du;
	h ^= h >> 12;
	return (s32)(h % ARRAYCOUNT(g_LgSkies));
}

const char *levelgenSkyName(s32 sky)
{
	return sky >= 0 && sky < (s32)ARRAYCOUNT(g_LgSkyNames) ? g_LgSkyNames[sky] : "?";
}

bool levelgenGetEnv(s32 stagenum, struct nofogenvironment **nofog)
{
	s32 sky;

	if (!levelgenIsStage(stagenum)) {
		return false;
	}

	sky = levelgenGetSky();
	*nofog = &g_LgSkies[sky];
	sysLogPrintf(LOG_NOTE, "levelgen: sky %d (%s) for seed %u", sky, levelgenSkyName(sky), g_LgSeed);
	return true;
}
