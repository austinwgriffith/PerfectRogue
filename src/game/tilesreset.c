#include <ultra64.h>
#include "constants.h"
#include "game/tiles.h"
#include "game/bg.h"
#include "game/file.h"
#include "bss.h"
#include "data.h"
#include "types.h"

#ifndef PLATFORM_N64
#include <stddef.h>
#include <string.h>
#include "files.h"
#include "game/aztecladder.h"
#include "gelevels.h"
#include "lib/memp.h"

// Append integer quads to a room before stageParseTiles converts their bounds.
static u8 *tilesReserveQuads(s32 room, s32 count)
{
	const u32 tilesize = offsetof(struct geotilei, vertices) + 4 * sizeof(s16[3]);
	const u32 extra = count * tilesize;
	const u32 insert = g_TileRooms[room + 1];
	const u32 end = g_TileRooms[g_TileNumRooms];
	u8 *data = mempAlloc(ALIGN16(end + extra), MEMPOOL_STAGE);

	if (!data) {
		return NULL;
	}

	memcpy(data, g_TileFileData.u8, insert);
	memcpy(data + insert + extra, g_TileFileData.u8 + insert, end - insert);
	g_TileFileData.u8 = data;
	g_TileRooms = g_TileFileData.u32 + 1;
	for (s32 next = room + 1; next <= g_TileNumRooms; next++) {
		g_TileRooms[next] += extra;
	}

	return data + insert;
}

static void tilesAddAztecRavineLadder(void)
{
	static const struct geotilei ladder = {
		.header = { GEOTYPE_TILE_I, 4, GEOFLAG_LADDER_PLAYERONLY },
		.ymin = 0, .ymax = 1, .zmin = 0, .zmax = 2,
		.vertices = {
			{ AZTEC_LADDER_FACE_X, AZTEC_LADDER_BOTTOM, AZTEC_LADDER_ZMIN },
			{ AZTEC_LADDER_FACE_X, AZTEC_LADDER_LIP + 8, AZTEC_LADDER_ZMIN },
			{ AZTEC_LADDER_FACE_X, AZTEC_LADDER_LIP + 8, AZTEC_LADDER_ZMAX },
			{ AZTEC_LADDER_FACE_X, AZTEC_LADDER_BOTTOM, AZTEC_LADDER_ZMAX },
		},
	};

	if (g_TileNumRooms > AZTEC_LADDER_TRENCH_ROOM) {
		u8 *quad = tilesReserveQuads(AZTEC_LADDER_TRENCH_ROOM, 1);

		if (quad) {
			memcpy(quad, &ladder, offsetof(struct geotilei, vertices) + sizeof(s16[4][3]));
		}
	}
}

static void tilesEnableAztecLedgeClimb(void)
{
	enum { LEDGE_ROOM = 92 };
	static const s16 ledge[4][3] = {
		{ -5183, 11, 750 }, { -5330, 11, 750 },
		{ -5330, 137, 750 }, { -5183, 137, 750 },
	};

	if (g_TileNumRooms <= LEDGE_ROOM) return;

	u8 *cursor = g_TileFileData.u8 + g_TileRooms[LEDGE_ROOM];
	const u8 *end = g_TileFileData.u8 + g_TileRooms[LEDGE_ROOM + 1];
	while (cursor < end) {
		struct geotilei *tile = (struct geotilei *)cursor;
		if (tile->header.type != GEOTYPE_TILE_I) return;
		if ((tile->header.flags & GEOFLAG_WALL) && tile->header.numvertices == ARRAYCOUNT(ledge)
				&& memcmp(tile->vertices, ledge, sizeof(ledge)) == 0) {
			// The linked GoldenEye riser became a wall. Use the player's
			// ledge climb to reach the sloped floor attached to its top.
			tile->header.flags |= GEOFLAG_LADDER_PLAYERONLY;
			return;
		}
		cursor += offsetof(struct geotilei, vertices) + tile->header.numvertices * sizeof(s16[3]);
	}
}

static void tilesAddDefectionStreetWalls(void)
{
	enum { STREET_ROOM = 13, WALL_BOTTOM = -8700, WALL_TOP = -8100 };
	static const s16 edges[][4] = {
		// The street steps out to a wider platform. Leave the doorway at x = 925 open.
		{ 925, -330, 1038, -330 },
		{ 1038, -330, 1038, -399 },
		{ 1038, -399, 1386, -399 },
		{ 1386, -399, 1386, 408 },
		{ 1386, 408, 1038, 408 },
		{ 1038, 408, 1038, 330 },
		{ 1038, 330, 925, 330 },
		{ 925, 330, 925, 98 },
		{ 925, -90, 925, -330 },
	};
	if (g_TileNumRooms <= STREET_ROOM) {
		return;
	}

	const u32 tilesize = offsetof(struct geotilei, vertices) + 4 * sizeof(s16[3]);
	u8 *quads = tilesReserveQuads(STREET_ROOM, ARRAYCOUNT(edges));

	if (!quads) {
		return;
	}

	for (s32 i = 0; i < ARRAYCOUNT(edges); i++) {
		const s16 *edge = edges[i];
		struct geotilei tile = {
			.header = { GEOTYPE_TILE_I, 4, GEOFLAG_WALL },
			.xmin = edge[0] <= edge[2] ? 0 : 2,
			.xmax = edge[0] >= edge[2] ? 0 : 2,
			.ymin = 0,
			.ymax = 1,
			.zmin = edge[1] <= edge[3] ? 0 : 2,
			.zmax = edge[1] >= edge[3] ? 0 : 2,
			.vertices = {
				{ edge[0], WALL_BOTTOM, edge[1] },
				{ edge[0], WALL_TOP, edge[1] },
				{ edge[2], WALL_TOP, edge[3] },
				{ edge[2], WALL_BOTTOM, edge[3] },
			},
		};

		memcpy(quads + i * tilesize, &tile, tilesize);
	}
}
#endif

void stageParseTiles(void);

void tilesReset(void)
{
	s32 index = bgGetStageIndex(g_Vars.stagenum);

	if (index < 0) {
		index = 0;
	}

	g_LoadType = LOADTYPE_TILES;
	g_TileFileData.u8 = fileLoadToNew(g_Stages[index].tilefileid, FILELOADMETHOD_DEFAULT, LOADTYPE_TILES);
	g_TileNumRooms = *g_TileFileData.u32;
	g_TileRooms = g_TileFileData.u32 + 1;

#ifndef PLATFORM_N64
	if (g_Vars.stagenum == STAGE_DEFECTION && g_Stages[index].tilefileid == FILE_BG_AME_TILES) {
		tilesAddDefectionStreetWalls();
	}
	const char *levelname = gelevelsName(g_Vars.stagenum);
	if (levelname && strcmp(levelname, "Aztec") == 0) {
		tilesEnableAztecLedgeClimb();
		tilesAddAztecRavineLadder();
	}
#endif

	stageParseTiles();
}

#define mult6(a) (((a) << 1) + ((a) << 2))

void stageParseTiles(void)
{
	struct geo *geo = (struct geo *)(g_TileFileData.u8 + g_TileRooms[0]);
	struct geo *end = (struct geo *)(g_TileFileData.u8 + g_TileRooms[g_TileNumRooms]);

	while (geo < end) {
		if (geo->type == GEOTYPE_TILE_I) {
			struct geotilei *tile = (struct geotilei *) geo;
			tile->xmin = mult6(tile->xmin) + 14;
			tile->xmax = mult6(tile->xmax) + 14;
			tile->ymin = mult6(tile->ymin) + 16;
			tile->ymax = mult6(tile->ymax) + 16;
			tile->zmin = mult6(tile->zmin) + 18;
			tile->zmax = mult6(tile->zmax) + 18;
			geo = (struct geo *)((u8 *)geo + (uintptr_t)(geo->numvertices - 0x40) * 6 + 0x18e);
		} else if (geo->type == GEOTYPE_TILE_F) {
			geo = (struct geo *)((u8 *)geo + (uintptr_t)(geo->numvertices - 0x40) * 12 + 0x310);
		} else if (geo->type == GEOTYPE_BLOCK) {
			geo = (struct geo *)((u8 *)geo + sizeof(struct geoblock));
		} else if (geo->type == GEOTYPE_CYL) {
			geo = (struct geo *)((u8 *)geo + sizeof(struct geocyl));
		}
	}
}
