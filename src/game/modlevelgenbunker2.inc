/* Bunker 2 (2026-11-26). A from-scratch remake of GoldenEye's Severnaya
 * bunker as a designed level, built straight from areas (no GoldenEye data,
 * so none of the converter's collision trouble). Included after the shared
 * portal and navigation helpers, like Switchback Station.
 *
 * x east, z south, a 64 x 64 grid, every floor at 0 but the hall's galleries.
 *
 *   the ring        a three-wide corridor round the whole bunker, cut into
 *                   runs of about twelve cells with a blast door between
 *                   every two, so no stretch is a shooting gallery
 *   the hall        the control hall in the middle: four spokes lead in from
 *                   the ring; a gallery along its north side (a ramp up from
 *                   the west end) and another along its south side (a ramp
 *                   up from the east end) look down on the floor and its
 *                   cover
 *   inner rooms     the computer room (north-west), the vault with the CCTV
 *                   tapes (north-east), the server hall (south-west), the
 *                   barracks (south-east): each opens onto the ring twice
 *   outer rooms     north: the lift lobby (the start), the generator room,
 *                   the quarters; south: the washrooms, the cell block, the
 *                   mess, the armoury (the far end)
 */
#define LG_BK_GRID 64
#define LG_BK_HALLCEIL 600
#define LG_BK_GALLERY  160
/* texture themes beyond the five room types and the corridor */
#define LG_BK_VAULT   5
#define LG_BK_CELLS   6
#define LG_BK_MACHINE 7

static void lgBkRoom(LgLayout *lg, int x, int z, int w, int d, int type, int theme, int ceil)
{
	const int room = lg->numareas;
	LgArea *a = &lg->areas[lg->numareas++];
	LgRect rc;
	int cx, cz;

	rc.x = (int16_t)x; rc.z = (int16_t)z; rc.w = (int16_t)w; rc.d = (int16_t)d; rc.room = (int16_t)room;
	lgSetRoomArea(a, &rc, type, room, 0, ceil);
	if (theme >= 0) {
		a->theme = (uint8_t)theme;
	}
	for (cz = z; cz < z + d; cz++) {
		for (cx = x; cx < x + w; cx++) {
			lg->cells[cz][cx] = (int16_t)(room + 1);
		}
	}
}

/* a part of room `room` (the hall's foyers, ramps and galleries): open to the
 * room's main area along every shared edge */
static void lgBkWing(LgLayout *lg, int room, int x, int z, int w, int d, int lo, int hi, int ramp, int ceil)
{
	const int ai = lg->numareas;
	LgArea *a = &lg->areas[lg->numareas++];
	LgRect rc;
	int cx, cz;

	rc.x = (int16_t)x; rc.z = (int16_t)z; rc.w = (int16_t)w; rc.d = (int16_t)d; rc.room = (int16_t)room;
	lgSetRoomArea(a, &rc, LG_ROOM_HALL, room, 0, ceil);
	a->floorlo = (int16_t)(lo < hi ? lo : hi);
	a->floor = (int16_t)(lo < hi ? hi : lo);
	a->ramp = (uint8_t)ramp;
	a->ceil = (int16_t)ceil;
	for (cz = z; cz < z + d; cz++) {
		for (cx = x; cx < x + w; cx++) {
			lg->cells[cz][cx] = (int16_t)(ai + 1);
		}
	}
}

static void lgBkCorridor(LgLayout *lg, int x, int z, int w, int d)
{
	const int ai = lg->numareas;
	LgRect rc;
	int cx, cz;

	rc.x = (int16_t)x; rc.z = (int16_t)z; rc.w = (int16_t)w; rc.d = (int16_t)d; rc.room = -1;
	lgStCorridor(lg, &rc, 0, 0, 0, 0, 0);
	for (cz = z; cz < z + d; cz++) {
		for (cx = x; cx < x + w; cx++) {
			lg->cells[cz][cx] = (int16_t)(ai + 1);
		}
	}
}

static int lgPlaceBunker2(LgLayout *lg)
{
	int i;

	lg->numareas = 0;

	/* the rooms, in order: area i is room i */
	lgBkRoom(lg, 10, 5, 10, 7, LG_ROOM_OFFICE, -1, 340);       /* 0 the lift lobby: the start */
	lgBkRoom(lg, 21, 28, 22, 8, LG_ROOM_HALL, -1, LG_BK_HALLCEIL);   /* 1 the control hall: its floor */
	lgBkRoom(lg, 9, 15, 10, 12, LG_ROOM_LAB, -1, 400);        /* 2 the computer room */
	lgBkRoom(lg, 45, 15, 10, 12, LG_ROOM_STORAGE, LG_BK_VAULT, 360);   /* 3 the vault: the CCTV tapes */
	lgBkRoom(lg, 9, 39, 10, 10, LG_ROOM_LAB, -1, 400);        /* 4 the server hall */
	lgBkRoom(lg, 45, 39, 10, 10, LG_ROOM_OFFICE, -1, 340);    /* 5 the barracks */
	lgBkRoom(lg, 22, 52, 10, 9, LG_ROOM_STORAGE, LG_BK_CELLS, 340);   /* 6 the cell block */
	lgBkRoom(lg, 46, 52, 8, 9, LG_ROOM_STORAGE, -1, 380);    /* 7 the armoury: the far end */
	lgBkRoom(lg, 22, 4, 10, 8, LG_ROOM_HALL, LG_BK_MACHINE, 520);    /* 8 the generator room */
	lgBkRoom(lg, 46, 4, 8, 8, LG_ROOM_OFFICE, -1, 340);      /* 9 the quarters */
	lgBkRoom(lg, 10, 52, 10, 9, LG_ROOM_OFFICE, -1, 320);      /* 10 the washrooms: the south-west */
	lgBkRoom(lg, 34, 52, 10, 9, LG_ROOM_STORAGE, -1, 360);    /* 11 the mess */
	lg->numrooms = (int16_t)lg->numareas;

	/* the hall: a foyer, a ramp and a gallery along each of its long sides.
	 * North: up from the west end. South: up from the east end. */
	lgBkWing(lg, 1, 21, 24, 3, 4, 0, 0, 0, LG_BK_HALLCEIL);
	lgBkWing(lg, 1, 24, 24, 6, 4, 0, LG_BK_GALLERY, LG_RAMP_XP, LG_BK_HALLCEIL);
	lgBkWing(lg, 1, 30, 24, 13, 4, LG_BK_GALLERY, LG_BK_GALLERY, 0, LG_BK_HALLCEIL);
	lgBkWing(lg, 1, 40, 36, 3, 4, 0, 0, 0, LG_BK_HALLCEIL);
	lgBkWing(lg, 1, 34, 36, 6, 4, 0, LG_BK_GALLERY, LG_RAMP_XN, LG_BK_HALLCEIL);
	lgBkWing(lg, 1, 21, 36, 13, 4, LG_BK_GALLERY, LG_BK_GALLERY, 0, LG_BK_HALLCEIL);

	/* the ring, in runs with a door between each: north and south sides,
	 * west and east sides, the four corners */
	{
		static const int16_t runx[4][2] = { { 9, 12 }, { 21, 12 }, { 33, 12 }, { 45, 10 } };
		static const int16_t runz[3][2] = { { 15, 12 }, { 27, 12 }, { 39, 10 } };
		int k;

		lgBkCorridor(lg, 6, 12, 3, 3);
		lgBkCorridor(lg, 55, 12, 3, 3);
		lgBkCorridor(lg, 6, 49, 3, 3);
		lgBkCorridor(lg, 55, 49, 3, 3);
		for (k = 0; k < 4; k++) {
			lgBkCorridor(lg, runx[k][0], 12, runx[k][1], 3);
			lgBkCorridor(lg, runx[k][0], 49, runx[k][1], 3);
		}
		for (k = 0; k < 3; k++) {
			lgBkCorridor(lg, 6, runz[k][0], 3, runz[k][1]);
			lgBkCorridor(lg, 55, runz[k][0], 3, runz[k][1]);
		}
	}
	/* the four spokes into the hall */
	lgBkCorridor(lg, 21, 15, 3, 9);    /* north: to the north foyer */
	lgBkCorridor(lg, 40, 40, 3, 9);    /* south: to the south foyer */
	lgBkCorridor(lg, 9, 30, 12, 3);    /* west: to the hall's floor */
	lgBkCorridor(lg, 43, 30, 12, 3);   /* east */

	for (i = 0; i < lg->numareas; i++) {
		lg->areas[i].dist = -1;
		lg->areas[i].centrepad = -1;
	}
	return lg->numareas <= LG_MAXAREAS - 8;
}

static int lgTryBunker2(LgLayout *lg, LgRng *r, char *err, int errlen)
{
	int i;

	if (!lgPlaceBunker2(lg)) {
		lgSetErr(err, errlen, "bunker 2 did not fit (%d areas)", lg->numareas, 0);
		return 0;
	}
	if (!lgFindPortals(lg, r, err, errlen)) {
		return 0;
	}
	/* sliding doors in every doorway, and a blast door across the ring where
	 * its runs meet or a spoke joins it (each three cells wide, level) */
	for (i = 0; i < lg->numportals; i++) {
		LgPortal *p = &lg->portals[i];
		const int wide = (p->x1 - p->x0) + (p->z1 - p->z0);

		const int rooms = (lg->areas[p->a].kind == LG_AREA_ROOM) + (lg->areas[p->b].kind == LG_AREA_ROOM);

		/* (never inside the hall: its foyers and ramps are one space) */
		p->door = (uint8_t)(rooms < 2 && !p->drop && !p->window && wide >= 1 && wide <= 3);
	}
	return lgAreaGraph(lg, r, err, errlen) && lgBuildNav(lg, err, errlen)
		&& lgBuildSpawns(lg, r, err, errlen)
		&& (lgPlaceObstacles(lg), lgPlaceCover(lg, r), 1)
		&& modLevelGenValidate(lg, err, errlen);
}
