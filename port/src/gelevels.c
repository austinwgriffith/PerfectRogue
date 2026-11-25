#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "data.h"
#include "bss.h"
#include "fs.h"
#include "system.h"
#include "romdata.h"
#include "game/stagetable.h"
#include "gelevels.h"

/**
 * GoldenEye's levels as Perfect Random maps. See gelevels.h for the shape of
 * it; tools/geconvert/README.md for how the files are made.
 */

#define GELEVELS_DIR    "mods/goldeneye"
#define GELEVELS_CONFIG GELEVELS_DIR "/modconfig.txt"
#define GELEVELS_STOCK_ROWS 61

// The stage pool's sizes a GoldenEye level loads with. The arenas' are too
// small for Dam or the Surfaces; the missions' are what a big level needs.
// The converter names the sky's cloud and water pictures 3-5 (Dabs added
// GoldenEye's three after Perfect Dark's three in g_TexSkyWaterConfigs). They
// are the same three pictures in the same order, so here they are 0-2.
#define GELEVELS_SKYTEX_FIRST 3

#define GELEVELS_ALLOC "-ml0 -me0 -mgfx200 -mgfxtra80 -mvtx200 -ma700"

struct gelevel {
	s16 stagenum;
	char name[32];
	bool hasfog;
	bool hasnofog;
	// GoldenEye's own music for it: main theme, background, X theme (gemusic.c)
	bool hasmusic;
	s32 music[3];
	// two rows: the engine fades from a fog row to the one after it
	struct fogenvironment fog[2];
	struct nofogenvironment nofog;
};

static struct gelevel g_GeLevels[GELEVELS_MAX_STAGES];
static s32 g_GeNumLevels = 0;
static s32 g_GeFirstFile = 0;
static s32 g_GeLastFile = -1;
static s32 g_GeTexOn = 0;

// GoldenEye's mission order, then its multiplayer-only maps. A level the list
// does not name goes after them, in the config's order.
static const char *g_GeOrder[] = {
	"Dam", "Facility", "Runway", "Surface", "Bunker 1", "Silo", "Frigate",
	"Surface 2", "Bunker", "Statue Park", "Archives", "Streets", "Depot",
	"Train", "Jungle", "Control", "Caverns", "Cradle", "Aztec", "Egyptian",
	"Temple", "Complex", "Caves", "Library", "Basement", "Stack",
};

// What the converter's names are shown as
static const char *gelevelsShownName(const char *name)
{
	if (strcmp(name, "Bunker") == 0) {
		return "Bunker 2";
	}

	return name;
}

/* ---- the config ---------------------------------------------------------- */

// One token: a "quoted string" or a run of non-space. Returns the position
// after it, or NULL at the end of the line.
static char *gelevelsToken(char *p, char *out, s32 outlen)
{
	s32 n = 0;

	while (*p == ' ' || *p == '\t') {
		p++;
	}

	if (*p == '\0' || *p == '\n' || *p == '\r') {
		return NULL;
	}

	if (*p == '"') {
		p++;
		while (*p && *p != '"' && *p != '\n') {
			if (n < outlen - 1) {
				out[n++] = *p;
			}
			p++;
		}
		if (*p == '"') {
			p++;
		}
	} else {
		while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
			if (n < outlen - 1) {
				out[n++] = *p;
			}
			p++;
		}
	}

	out[n] = '\0';

	return p;
}

static char *gelevelsNextLine(char *p)
{
	while (*p && *p != '\n') {
		p++;
	}

	return *p ? p + 1 : p;
}

// A file under mods/goldeneye/files/ given a file number, or 0
static s32 gelevelsAddFile(const char *rel)
{
	char path[FS_MAXPATH + 1];
	char name[128];
	s32 num;

	snprintf(path, sizeof(path), GELEVELS_DIR "/files/%s", rel);

	if (fsFileSize(path) <= 0) {
		sysLogPrintf(LOG_WARNING, "gelevels: missing %s", path);
		return 0;
	}

	// a name of its own, so it never matches one of the ROM's
	snprintf(name, sizeof(name), "goldeneye/%s", rel);
	num = romdataFileAdd(name, path);

	if (num <= 0) {
		return 0;
	}

	if (g_GeFirstFile == 0 || num < g_GeFirstFile) {
		g_GeFirstFile = num;
	}

	if (num > g_GeLastFile) {
		g_GeLastFile = num;
	}

	return num;
}

/** A fog row's seventeen values (geconvert.c's fogValue()). */
static bool gelevelsParseFog(s32 stagenum, const char *text, struct fogenvironment *env)
{
	s32 v[7], clouds[3], water[3], height;
	u32 sky, cloudrgb, waterrgb;

	if (sscanf(text, "%d %d %d %d %d %d %d %x %d %d %d %x %d %d %d %x %d",
				&v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &sky,
				&clouds[0], &clouds[1], &clouds[2], &cloudrgb,
				&water[0], &water[1], &water[2], &waterrgb, &height) != 17) {
		return false;
	}

	// the row's distances are 16-bit (Dam's far is 75000)
	for (s32 i = 0; i < 7; i++) {
		v[i] = v[i] > 32767 ? 32767 : (v[i] < -32768 ? -32768 : v[i]);
	}

	memset(env, 0, sizeof(*env));
	env->stage = stagenum;
	env->near = v[0];
	env->far = v[1];
	env->opaperc = v[2];
	env->xluperc = v[3];
	env->refdist = v[4];
	env->fogmin = v[5];
	env->fogmax = v[6];
	env->sky_r = sky >> 16;
	env->sky_g = sky >> 8;
	env->sky_b = sky;
	env->clouds_enabled = clouds[0];
	env->clouds_scale = clouds[1];
	env->clouds_type = clouds[2];
	env->clouds_r = cloudrgb >> 16;
	env->clouds_g = cloudrgb >> 8;
	env->clouds_b = cloudrgb;
	env->water_enabled = water[0];
	env->water_scale = water[1];
	env->water_type = water[2];
	env->water_r = waterrgb >> 16;
	env->water_g = waterrgb >> 8;
	env->water_b = waterrgb;
	env->clouds_height = height;

	if (env->clouds_type >= GELEVELS_SKYTEX_FIRST) {
		env->clouds_type -= GELEVELS_SKYTEX_FIRST;
	}

	if (env->water_type >= GELEVELS_SKYTEX_FIRST) {
		env->water_type -= GELEVELS_SKYTEX_FIRST;
	}

	if (env->clouds_type > 2 || env->water_type > 2) {
		env->clouds_type = 0;
		env->water_type = 0;
		env->clouds_enabled = 0;
		env->water_enabled = 0;
	}

	return true;
}

/** "nofog" and twelve values (geconvert.c's foglessValue()). */
static bool gelevelsParseNoFog(s32 stagenum, const char *text, struct nofogenvironment *env)
{
	s32 v[2], clouds[3], water[3], height;
	u32 sky, cloudrgb, waterrgb;

	if (sscanf(text, "%d %d %x %d %d %d %x %d %d %d %x %d",
				&v[0], &v[1], &sky,
				&clouds[0], &clouds[1], &clouds[2], &cloudrgb,
				&water[0], &water[1], &water[2], &waterrgb, &height) != 12) {
		return false;
	}

	memset(env, 0, sizeof(*env));
	env->stage = stagenum;
	env->near = v[0];
	env->far = v[1];
	env->sky_r = sky >> 16;
	env->sky_g = sky >> 8;
	env->sky_b = sky;
	env->clouds_enabled = clouds[0];
	env->clouds_scale = clouds[1];
	env->clouds_type = clouds[2];
	env->clouds_r = cloudrgb >> 16;
	env->clouds_g = cloudrgb >> 8;
	env->clouds_b = cloudrgb;
	env->water_enabled = water[0];
	env->water_scale = water[1];
	env->water_type = water[2];
	env->water_r = waterrgb >> 16;
	env->water_g = waterrgb >> 8;
	env->water_b = waterrgb;
	env->clouds_height = height;

	if (env->clouds_type >= GELEVELS_SKYTEX_FIRST) {
		env->clouds_type -= GELEVELS_SKYTEX_FIRST;
	}

	if (env->water_type >= GELEVELS_SKYTEX_FIRST) {
		env->water_type -= GELEVELS_SKYTEX_FIRST;
	}

	if (env->clouds_type > 2 || env->water_type > 2) {
		env->clouds_type = 0;
		env->water_type = 0;
		env->clouds_enabled = 0;
		env->water_enabled = 0;
	}
	env->transparency = false;

	return true;
}

// The lowest stage number nothing uses, below the title screen's: the game
// still asks "stagenum < STAGE_TITLE" for "is this a level" in places
static bool g_GeRetiredStages[STAGE_TITLE];

static s32 gelevelsNextStageId(void)
{
	for (s32 id = 0x51; id < STAGE_TITLE; id++) {
		if (!g_GeRetiredStages[id] && stageGetIndex(id) < 0) {
			return id;
		}
	}

	for (s32 id = 0x02; id < 0x51; id++) {
		if (!g_GeRetiredStages[id] && stageGetIndex(id) < 0) {
			return id;
		}
	}

	return 0;
}

struct gelevelsmap {
	char name[32];
	char bg[64], tiles[64], pads[64], setup[64];
	char fog[192];
	char music[48];
};

static void gelevelsAddMap(const struct gelevelsmap *m)
{
	const s32 tmpl = stageGetIndex(STAGE_MP_SKEDAR);
	const s32 row = GELEVELS_STOCK_ROWS + g_GeNumLevels;
	s32 bg, tiles, pads, setup, stagenum;
	struct gelevel *lv;

	if (g_GeNumLevels >= GELEVELS_MAX_STAGES || tmpl < 0) {
		return;
	}

	if (!m->bg[0] || !m->pads[0] || !m->setup[0]) {
		return;
	}

	// Left out of the map list for now: both crash the game
	{
		const char *shown = gelevelsShownName(m->name);

		if (strcmp(shown, "Runway") == 0 || strcmp(shown, "Caves") == 0) {
			sysLogPrintf(LOG_NOTE, "gelevels: %s is left out of the map list", shown);
			return;
		}
	}

	bg = gelevelsAddFile(m->bg);
	tiles = m->tiles[0] ? gelevelsAddFile(m->tiles) : 0;
	pads = gelevelsAddFile(m->pads);
	setup = gelevelsAddFile(m->setup);

	if (!bg || !pads || !setup) {
		sysLogPrintf(LOG_WARNING, "gelevels: %s is missing a file, left out", m->name);
		return;
	}

	stagenum = gelevelsNextStageId();

	if (!stagenum) {
		sysLogPrintf(LOG_WARNING, "gelevels: no stage number left for %s", m->name);
		return;
	}

	if (strcmp(m->name, "Temple") == 0 || strcmp(m->name, "Complex") == 0) {
		// Preserve the stage IDs used by the remaining maps and their spawn configs.
		g_GeRetiredStages[stagenum] = true;
		return;
	}

	// Skedar's arena row for the rest (lighting, scale, alarm)
	g_Stages[row] = g_Stages[tmpl];
	g_Stages[row].id = stagenum;
	g_Stages[row].bgfileid = bg;
	g_Stages[row].tilefileid = tiles ? tiles : g_Stages[tmpl].tilefileid;
	g_Stages[row].padsfileid = pads;
	// one setup: the arena's, whichever way the stage is loaded
	g_Stages[row].setupfileid = setup;
	g_Stages[row].mpsetupfileid = setup;

	lv = &g_GeLevels[g_GeNumLevels++];
	memset(lv, 0, sizeof(*lv));
	lv->stagenum = stagenum;
	snprintf(lv->name, sizeof(lv->name), "%s", gelevelsShownName(m->name));

	if (m->fog[0]) {
		if (strncmp(m->fog, "nofog ", 6) == 0) {
			lv->hasnofog = gelevelsParseNoFog(stagenum, m->fog + 6, &lv->nofog);
		} else if (gelevelsParseFog(stagenum, m->fog, &lv->fog[0])) {
			struct fogenvironment *fog = &lv->fog[0];
			struct nofogenvironment *env = &lv->nofog;
			env->stage = stagenum;
			env->near = fog->near;
			env->far = fog->far;
			env->sky_r = fog->sky_r;
			env->sky_g = fog->sky_g;
			env->sky_b = fog->sky_b;
			env->numsuns = fog->numsuns;
			env->suns = fog->suns;
			env->clouds_enabled = fog->clouds_enabled;
			env->clouds_scale = fog->clouds_scale;
			env->clouds_type = fog->clouds_type;
			env->clouds_r = fog->clouds_r;
			env->clouds_g = fog->clouds_g;
			env->clouds_b = fog->clouds_b;
			env->water_enabled = fog->water_enabled;
			env->water_scale = fog->water_scale;
			env->water_type = fog->water_type;
			env->water_r = fog->water_r;
			env->water_g = fog->water_g;
			env->water_b = fog->water_b;
			env->clouds_height = fog->clouds_height;
			lv->hasnofog = true;
		}
		if (lv->hasnofog && lv->nofog.far < 20000) {
			lv->nofog.far = 20000;
		}
	}

	if (m->music[0]) {
		s32 t[3] = { -1, -1, -1 };

		if (sscanf(m->music, "%d %d %d", &t[0], &t[1], &t[2]) >= 1) {
			lv->music[0] = t[0];
			lv->music[1] = t[1];
			lv->music[2] = t[2];
			lv->hasmusic = true;
		}
	}

	sysLogPrintf(LOG_NOTE, "gelevels: %s is stage 0x%02x (bg %d, tiles %d, pads %d, setup %d)",
			lv->name, stagenum, bg, tiles, pads, setup);
}

static bool gelevelsReadMap(char *p, struct gelevelsmap *m)
{
	char key[32];
	char value[192];

	memset(m, 0, sizeof(*m));

	// map "Name"
	p = gelevelsToken(p, value, sizeof(value));
	if (!p) {
		return false;
	}
	snprintf(m->name, sizeof(m->name), "%.31s", value);

	while ((p = gelevelsToken(p, key, sizeof(key))) != NULL) {
		p = gelevelsToken(p, value, sizeof(value));
		if (!p) {
			break;
		}

		if (strcmp(key, "bg") == 0) {
			snprintf(m->bg, sizeof(m->bg), "%s", value);
		} else if (strcmp(key, "tiles") == 0) {
			snprintf(m->tiles, sizeof(m->tiles), "%s", value);
		} else if (strcmp(key, "pads") == 0) {
			snprintf(m->pads, sizeof(m->pads), "%s", value);
		} else if (strcmp(key, "mpsetup") == 0) {
			snprintf(m->setup, sizeof(m->setup), "%s", value);
		} else if (strcmp(key, "fog") == 0) {
			snprintf(m->fog, sizeof(m->fog), "%s", value);
		} else if (strcmp(key, "music") == 0) {
			snprintf(m->music, sizeof(m->music), "%s", value);
		}
	}

	return m->name[0] != '\0';
}

static void gelevelsReadModel(char *p)
{
	char tok[64];
	char file[64];
	s32 slot;
	f32 scale;
	s32 fileid;

	p = gelevelsToken(p, tok, sizeof(tok));
	if (!p) {
		return;
	}
	slot = strtol(tok, NULL, 0);

	p = gelevelsToken(p, file, sizeof(file));
	if (!p) {
		return;
	}

	p = gelevelsToken(p, tok, sizeof(tok));
	scale = p ? strtof(tok, NULL) : 4096.0f;

	if (slot < 0 || slot >= NUM_REMAKE_MODELS) {
		return;
	}

	fileid = gelevelsAddFile(file);
	if (!fileid) {
		return;
	}

	g_ModelStates[MODEL_REMAKE_FIRST + slot].modeldef = NULL;
	g_ModelStates[MODEL_REMAKE_FIRST + slot].fileid = fileid;
	g_ModelStates[MODEL_REMAKE_FIRST + slot].scale = (u16)(scale + 0.5f);
}

void gelevelsInit(void)
{
	struct gelevelsmap maps[GELEVELS_MAX_STAGES];
	s32 nummaps = 0;
	u8 *used;
	u32 len = 0;
	char *data;
	char *p;
	char tok[32];
	s32 block = 0; // 1 maps, 2 models, 3 something else

	// the spare rows are nothing until filled
	for (s32 i = GELEVELS_STOCK_ROWS; i < GELEVELS_STOCK_ROWS + GELEVELS_MAX_STAGES; i++) {
		memset(&g_Stages[i], 0, sizeof(g_Stages[i]));
		g_Stages[i].id = -1;
	}

	gelevelsAutoConvert();

	if (fsFileSize(GELEVELS_CONFIG) <= 0) {
		sysLogPrintf(LOG_NOTE, "gelevels: no %s, no GoldenEye levels (put the GoldenEye ROM beside the game)", GELEVELS_CONFIG);
		return;
	}

	data = (char *)fsFileLoad(GELEVELS_CONFIG, &len);
	if (!data) {
		return;
	}

	for (p = data; *p; p = gelevelsNextLine(p)) {
		char *q = gelevelsToken(p, tok, sizeof(tok));

		if (!q || tok[0] == '#') {
			continue;
		}

		if (block == 0) {
			if (strcmp(tok, "maps") == 0) {
				block = 1;
			} else if (strcmp(tok, "models") == 0) {
				block = 2;
			} else {
				block = 3;
			}
			continue;
		}

		if (strcmp(tok, "}") == 0) {
			block = 0;
			continue;
		}

		if (block == 1 && strcmp(tok, "map") == 0 && nummaps < GELEVELS_MAX_STAGES) {
			if (gelevelsReadMap(q, &maps[nummaps])) {
				nummaps++;
			}
		} else if (block == 2) {
			gelevelsReadModel(p);
		}
	}

	sysMemFree(data);

	// in GoldenEye's order, then the rest
	used = calloc(nummaps ? nummaps : 1, 1);

	for (s32 o = 0; o < (s32)ARRAYCOUNT(g_GeOrder); o++) {
		for (s32 i = 0; i < nummaps; i++) {
			if (!used[i] && strcmp(maps[i].name, g_GeOrder[o]) == 0) {
				used[i] = 1;
				gelevelsAddMap(&maps[i]);
			}
		}
	}

	for (s32 i = 0; i < nummaps; i++) {
		if (!used[i]) {
			gelevelsAddMap(&maps[i]);
		}
	}

	free(used);

	sysLogPrintf(LOG_NOTE, "gelevels: %d GoldenEye levels, files %d-%d", g_GeNumLevels, g_GeFirstFile, g_GeLastFile);
}

/* ---- questions ----------------------------------------------------------- */

s32 gelevelsCount(void)
{
	return g_GeNumLevels;
}

s32 gelevelsStageAt(s32 index)
{
	return index >= 0 && index < g_GeNumLevels ? g_GeLevels[index].stagenum : 0;
}

static struct gelevel *gelevelsFind(s32 stagenum)
{
	for (s32 i = 0; i < g_GeNumLevels; i++) {
		if (g_GeLevels[i].stagenum == stagenum) {
			return &g_GeLevels[i];
		}
	}

	return NULL;
}

s32 gelevelsIsStage(s32 stagenum)
{
	return stagenum > 0 && gelevelsFind(stagenum) != NULL;
}

const char *gelevelsName(s32 stagenum)
{
	struct gelevel *lv = gelevelsFind(stagenum);

	return lv ? lv->name : NULL;
}

s32 gelevelsFileIsGe(s32 filenum)
{
	return g_GeNumLevels > 0 && filenum >= g_GeFirstFile && filenum <= g_GeLastFile;
}

const char *gelevelsAllocation(s32 stagenum)
{
	return gelevelsIsStage(stagenum) ? GELEVELS_ALLOC : NULL;
}

bool gelevelsGetEnv(s32 stagenum, struct fogenvironment **fog, struct nofogenvironment **nofog)
{
	struct gelevel *lv = gelevelsFind(stagenum);

	*fog = NULL;
	*nofog = NULL;

	if (!lv) {
		return false;
	}

	if (lv->hasfog) {
		*fog = &lv->fog[0];
		return true;
	}

	if (lv->hasnofog) {
		*nofog = &lv->nofog;
		return true;
	}

	return false;
}

/* ---- textures ------------------------------------------------------------ */

/**
 * Turns GoldenEye's texture set on or off for the textures loaded until the
 * next call; returns what it was, for the caller to put back.
 */
s32 gelevelsTexSet(s32 on)
{
	const s32 prev = g_GeTexOn;

	g_GeTexOn = on ? 1 : 0;

	return prev;
}

s32 gelevelsTexActive(void)
{
	return g_GeTexOn && g_GeNumLevels > 0;
}

s32 gelevelsTextureLoad(u16 num, void *dst, u32 dstSize)
{
	char path[FS_MAXPATH + 1];

	snprintf(path, sizeof(path), GELEVELS_DIR "/textures/%04x.bin", num);

	return fsFileLoadTo(path, dst, dstSize);
}

/**
 * The GoldenEye sequence numbers a level plays (main theme, background, X
 * theme; -1 for none), as the converter read them out of the ROM's table.
 */
bool gelevelsGetMusic(s32 stagenum, s32 tracks[3])
{
	for (s32 i = 0; i < g_GeNumLevels; i++) {
		if (g_GeLevels[i].stagenum == stagenum && g_GeLevels[i].hasmusic) {
			tracks[0] = g_GeLevels[i].music[0];
			tracks[1] = g_GeLevels[i].music[1];
			tracks[2] = g_GeLevels[i].music[2];
			return true;
		}
	}

	return false;
}
