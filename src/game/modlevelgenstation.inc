/* Switchback Station. Included after the shared portal and navigation helpers. */
enum { LG_STN_ENTRY, LG_STN_EXIT, LG_STN_DEPOT_LEFT, LG_STN_DEPOT_CENTER,
	LG_STN_DEPOT_RIGHT, LG_STN_PLATFORM_LEFT, LG_STN_PLATFORM_RIGHT,
	LG_STN_FIRST_CAR, LG_STN_ROOMS = LG_STN_FIRST_CAR + 3 };

static void lgStnArea(LgLayout *lg, const LgStMap *m, int ai, int room,
		int u, int v, int len, int width, int layer, int floor, int ceil,
		int rise, int flags, int theme)
{
	const LgRect rc = lgStRect(m, u, v, len, width);
	LgArea *a = &lg->areas[ai];

	lgSetRoomArea(a, &rc, theme == LG_STN_DEPOT ? LG_ROOM_STORAGE : LG_ROOM_HALL, room, 0, ceil);
	a->kind = room < 0 ? LG_AREA_CORRIDOR : LG_AREA_ROOM;
	a->theme = (uint8_t)theme;
	a->floorlo = (int16_t)floor;
	a->floor = (int16_t)(floor + lgAbs(rise));
	a->ramp = rise ? (uint8_t)lgStRampDir(m, rise > 0) : LG_RAMP_NONE;
	a->flags = (uint8_t)(flags | (layer ? LG_AREAFLAG_MEZZ : 0));
	a->layer = (uint8_t)layer;
	for (int z = a->z; z < a->z + a->d; z++) {
		for (int x = a->x; x < a->x + a->w; x++) {
			LG_CELLS(lg, layer)[z][x] = (int16_t)(ai + 1);
		}
	}
}

static int lgTryStation(LgLayout *lg, LgRng *r, char *err, int errlen)
{
	const LgStMap m = { lg->gw, lgRand(r) & 1, lgRand(r) & 1 };
	const int carlen = 8 + 2 * lg->size, gap = 3, span = 3 * carlen + 2 * gap;
	const int first = (lg->gw - span) / 2, last = first + span, vc = lg->gw / 2;
	const int access = 8, halltop = 980, platform = LG_STN_PLATFORM_Y, concourse = LG_STN_CONCOURSE_Y;
	const int under = LG_AREAFLAG_UNDER, bay = LG_AREAFLAG_BAY, depotceil = platform - LG_MEZZ_SLAB;
	const int alt = lgRand(r) & 1;
	int end, car, side, i;

	lg->numrooms = lg->numareas = LG_STN_ROOMS;
	for (end = 0; end < 2; end++) {
		const int approach = end ? last : first - access;
		const int lobby = end ? last + access : first - access - 7;

		lgStnArea(lg, &m, end, end, lobby, vc - 9, 7, 18, 0, 0, 400, 0, 0, LG_STN_OFFICE);
		lgStnArea(lg, &m, lg->numareas++, -1, approach, vc - 2, access, 4, 0, 0, depotceil, 0, 0, LG_STN_SERVICE);
		for (side = 0; side < 2; side++) {
			lgStnArea(lg, &m, lg->numareas++, -1, approach, vc + (side ? 3 : -6), access, 3,
				0, 0, halltop, end ? -platform : platform, bay, LG_STN_ACCESS);
		}
	}
	for (car = 0; car < 3; car++) {
		const int u = first + car * (carlen + gap), train = LG_STN_FIRST_CAR + car;

		for (side = 0; side < 3; side++) {
			const int depot = LG_STN_DEPOT_LEFT + side;
			lgStnArea(lg, &m, car ? lg->numareas++ : depot, depot, u, vc - 9 + side * 6,
				carlen, 6, 0, 0, depotceil, 0, under, LG_STN_DEPOT);
		}
		lgStnArea(lg, &m, train, train, u, vc - 2, carlen, 4, 1, platform, LG_STN_TRAIN_TOP, 0, under, LG_STN_CAR);
		/* A real roof area owns its geometry, bounds, collision and portals.
		 * Upper views must not depend on a car interior's low doorway portal. */
		lgStnArea(lg, &m, lg->numareas++, train, u, vc - 3, carlen, 6,
			2, concourse, halltop, 0, 0, LG_STN_CAR);
		for (side = 0; side < 2; side++) {
			const int room = LG_STN_PLATFORM_LEFT + side;
			const int inner = vc + (side ? 3 : -6), outer = vc + (side ? 6 : -9);
			const int face = vc + (side ? 2 : -3), reverser = side ^ alt;
			const int doorend = lgRand(r) & 1, door = doorend ? carlen - 3 : 1;
			const int window = doorend ? 1 : carlen - 3;

			lgStnArea(lg, &m, car ? lg->numareas++ : room, room, u, inner, carlen, 3,
				1, platform, halltop, 0, bay, LG_STN_PLATFORM);
			if (car == (reverser ? 2 : 0)) {
				lgStnArea(lg, &m, lg->numareas++, room, reverser ? u + carlen - 2 : u, outer,
					2, 3, 1, platform, halltop, 0, bay, LG_STN_ACCESS);
				lgStnArea(lg, &m, lg->numareas++, room, reverser ? u : u + 2, outer, carlen - 2, 3,
					1, platform, halltop, reverser ? platform - concourse : concourse - platform, bay, LG_STN_ACCESS);
			} else {
				lgStnArea(lg, &m, lg->numareas++, room, u, outer, carlen, 3, 1, platform, LG_STN_TRAIN_TOP, 0, under, LG_STN_PLATFORM);
				lgStnArea(lg, &m, lg->numareas++, room, u, outer, carlen, 3, 2, concourse, halltop, 0, 0, LG_STN_CONCOURSE);
			}
			lgStnArea(lg, &m, lg->numareas++, -1, u + door, face, 2, 1, 1, platform, platform + 260, 0, 0, LG_STN_VESTIBULE);
			lgStnArea(lg, &m, lg->numareas++, -1, u + window, face, 2, 1, 1,
				platform + LG_WIN_SILL, platform + LG_WIN_HEAD, 0, 0, LG_STN_VESTIBULE);
			lg->areas[lg->numareas - 1].special = LG_SPECIAL_WINDOW;
		}
		if (car < 2) {
			const int at = u + carlen;
			lgStnArea(lg, &m, lg->numareas++, -1, at, vc - 9, gap, 18, 0, 0, depotceil, 0, under, LG_STN_SERVICE);
			lgStnArea(lg, &m, lg->numareas++, -1, at, vc - 3, gap, 6, 1, platform, LG_STN_TRAIN_TOP, 0, under, LG_STN_VESTIBULE);
			lgStnArea(lg, &m, lg->numareas++, -1, at, vc - 3, gap, 6, 2, concourse, halltop, 0, 0, LG_STN_CONCOURSE);
			for (side = 0; side < 2; side++) {
				const int room = LG_STN_PLATFORM_LEFT + side, edge = vc + (side ? 3 : -9);
				lgStnArea(lg, &m, lg->numareas++, room, at, edge, gap, 6, 1, platform, LG_STN_TRAIN_TOP, 0, under, LG_STN_PLATFORM);
				lgStnArea(lg, &m, lg->numareas++, room, at, edge, gap, 6, 2, concourse, halltop, 0, 0, LG_STN_CONCOURSE);
			}
		}
	}
	for (g_LgPortalLayer = 0; g_LgPortalLayer < LG_MAXLAYERS; g_LgPortalLayer++) {
		if (!lgFindPortals(lg, r, err, errlen)) {
			g_LgPortalLayer = 0;
			return 0;
		}
	}
	g_LgPortalLayer = 0;
	if (!lgMezzPortals(lg, err, errlen)) return 0;
	for (i = 0; i < lg->numportals; i++) {
		LgPortal *p = &lg->portals[i];
		if (lg->areas[p->a].layer == 2 || lg->areas[p->b].layer == 2
				|| lg->areas[p->a].theme == LG_STN_VESTIBULE || lg->areas[p->b].theme == LG_STN_VESTIBULE) {
			p->door = 0;
		}
	}
	return lgAreaGraph(lg, r, err, errlen) && lgBuildNav(lg, err, errlen)
		&& lgBuildSpawns(lg, r, err, errlen)
		&& (lgPlaceObstacles(lg), lgPlaceCover(lg, r), 1)
		&& modLevelGenValidate(lg, err, errlen);
}
