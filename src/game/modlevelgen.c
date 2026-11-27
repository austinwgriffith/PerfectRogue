/*
 * modlevelgen.c: Perfect Random level generator, layout stage.
 * See modlevelgen.h. No engine code, integer maths only.
 *
 * Standalone test build:
 *   gcc -O1 -Wall -Wextra -DMODLEVELGEN_MAIN -I src/include \
 *       -o levelgen src/game/modlevelgen.c
 *   ./levelgen --selftest 200
 *   ./levelgen --seed 1234 --size 1
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "game/modlevelgen.h"

#define LG_RETRIES 40

/* ------------------------------------------------------------------ */
/* Tables                                                               */

/* The hub layout (2026-11-14, user: "one large hub area and offshoots from
 * it; the hub with large variations in height and long sightlines; and
 * balconies"). The hub is a long hall: an open floor down its middle, end to
 * end, and down both long sides terraces of blocks at floor levels up to
 * maxlevel, joined along the side by ramps, every terrace edge a ledge over
 * the floor (a balcony). Rooms branch off it: beside a terrace block they open
 * onto that balcony at its height, at the ends onto the floor. Neighbouring
 * rooms are joined where a short passage fits. */
static const struct {
	int16_t gw;
	int8_t hubwmin, hubwmax;   /* the hub's length, cells */
	int8_t hubdmin, hubdmax;   /* and width */
	int8_t terrmin, terrmax;   /* a terrace's depth */
	int8_t blockmin, blockmax; /* a terrace block's length */
	int8_t minoff, maxoff;     /* rooms off the hub */
	int8_t maxlevel;           /* highest floor level */
	int8_t links;              /* passages wanted between neighbouring rooms */
} g_LgSizes[LG_SIZE_COUNT] = {
	{ 48, 16, 20, 12, 14, 3, 4, 4, 6,  4,  5, 2, 2 },
	{ 72, 26, 32, 16, 19, 4, 5, 4, 7,  6,  8, 3, 3 },
	{ 104, 36, 44, 20, 24, 4, 6, 5, 8,  9, 12, 3, 4 },
};

#define LG_LEVEL      160   /* one floor level's height */
#define LG_RAMPCELLS  4     /* cells of ramp one level needs (1 in 2.5) */
#define LG_HUB_CEIL   900   /* the hub's ceiling over its floor */
#define LG_RINGGAP    4     /* cells between different rooms: a passage and a cell either side */

static const struct {
	int8_t minw, maxw;
	int16_t ceil;
	uint8_t weight;
	uint8_t theme;
} g_LgRoomTypes[LG_ROOM_TYPECOUNT] = {
	{  8, 12, 300, 4, 1 }, /* office: side rooms a bit larger (2026-11-14) */
	{ 14, 20, 500, 1, 2 }, /* hall    */
	{  7, 10, 350, 2, 4 }, /* storage */
	{ 10, 14, 400, 3, 0 }, /* lab     */
};

#define LG_CORRIDOR_CEIL  300   /* a passage's ceiling over its highest floor */
#define LG_CORRIDOR_THEME 3
#define LG_STREET_THEME   5   /* outdoors: the streets (and the other layouts' open ground) */

#define LG_ROOMGAP        2    /* solid cells kept between rooms */
#define LG_NAV_INSET      (LG_CELL / 2)
#define LG_GUARD_MARGIN   80
#define LG_GUARD_STEP     200
#define LG_GUARD_JITTER   40
#define LG_DOOR_PCT       30

/* ------------------------------------------------------------------ */
/* Random numbers (own stream; the game seeds it from modRandomOpen)    */

typedef struct { uint32_t s; } LgRng;

static uint32_t lgRand(LgRng *r)
{
	uint32_t z;
	r->s += 0x9e3779b9u;
	z = r->s;
	z = (z ^ (z >> 16)) * 0x85ebca6bu;
	z = (z ^ (z >> 13)) * 0xc2b2ae35u;
	return z ^ (z >> 16);
}

static int lgRange(LgRng *r, int lo, int hi)
{
	if (hi <= lo) {
		return lo;
	}
	return lo + (int)(lgRand(r) % (uint32_t)(hi - lo + 1));
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */

static void lgSetErr(char *err, int errlen, const char *msg, int a, int b)
{
	if (err && errlen > 0) {
		snprintf(err, (size_t)errlen, msg, a, b);
	}
}

static int lgAbs(int v) { return v < 0 ? -v : v; }

static int32_t lgWorldX(const LgLayout *lg, int cx) { return lg->originx + cx * LG_CELL; }
static int32_t lgWorldZ(const LgLayout *lg, int cz) { return lg->originz + cz * LG_CELL; }

static int lgAreaAt(const LgLayout *lg, int x, int z)
{
	if (x < 0 || z < 0 || x >= lg->gw || z >= lg->gd) {
		return -1;
	}
	return lg->cells[z][x] - 1;
}

/* the cell is area ai's, in ai's own layer */
static int lgOwns(const LgLayout *lg, int ai, int x, int z)
{
	if (x < 0 || z < 0 || x >= lg->gw || z >= lg->gd) {
		return 0;
	}
	return LG_CELLS(lg, lg->areas[ai].layer)[z][x] == ai + 1;
}

int32_t modLevelGenFloorAt(const LgLayout *lg, int ai, int32_t x, int32_t z)
{
	const LgArea *a = &lg->areas[ai];
	const int32_t x0 = lgWorldX(lg, a->x), z0 = lgWorldZ(lg, a->z);
	const int32_t w = a->w * LG_CELL, d = a->d * LG_CELL;
	int32_t t, len;

	switch (a->ramp) {
	case LG_RAMP_XP: t = x - x0; len = w; break;
	case LG_RAMP_XN: t = x0 + w - x; len = w; break;
	case LG_RAMP_ZP: t = z - z0; len = d; break;
	case LG_RAMP_ZN: t = z0 + d - z; len = d; break;
	default: return a->floor;
	}
	if (t < 0) t = 0;
	if (t > len) t = len;
	return a->floorlo + (int32_t)(((int64_t)(a->floor - a->floorlo) * t) / len);
}

static int lgAddPad(LgLayout *lg, int kind, int32_t x, int32_t z, int angle, int area)
{
	LgPad *p;

	if (lg->numpads >= LG_MAXPADS) {
		return -1;
	}

	p = &lg->pads[lg->numpads];
	p->x = x;
	p->y = modLevelGenFloorAt(lg, area, x, z);
	p->z = z;
	p->angle = (int16_t)(((angle % 360) + 360) % 360);
	p->area = (int16_t)area;
	p->kind = (uint8_t)kind;
	p->numneigh = 0;

	return lg->numpads++;
}

static int lgHasNeigh(const LgPad *p, int other)
{
	int i;
	for (i = 0; i < p->numneigh; i++) {
		if (p->neigh[i] == other) {
			return 1;
		}
	}
	return 0;
}

/* Links two nav pads both ways. Returns 0 if either list is full. */
static int lgLink(LgLayout *lg, int a, int b)
{
	LgPad *pa = &lg->pads[a];
	LgPad *pb = &lg->pads[b];

	if (a == b || lgHasNeigh(pa, b)) {
		return 1;
	}
	if (pa->numneigh >= LG_MAXNEIGH || pb->numneigh >= LG_MAXNEIGH) {
		return 0;
	}

	pa->neigh[pa->numneigh++] = (int16_t)b;
	pb->neigh[pb->numneigh++] = (int16_t)a;
	return 1;
}

/* Angle (0 = +z, 90 = +x) for the dominant axis of a direction. */
static int lgAngleFor(int32_t dx, int32_t dz)
{
	if (lgAbs(dx) > lgAbs(dz)) {
		return dx > 0 ? 90 : 270;
	}
	return dz >= 0 ? 0 : 180;
}

/* ------------------------------------------------------------------ */
/* Step 1: rooms                                                        */

static int lgPickRoomType(LgRng *r)
{
	int total = 0, i, pick;

	for (i = 0; i < LG_ROOM_TYPECOUNT; i++) {
		total += g_LgRoomTypes[i].weight;
	}
	pick = lgRange(r, 0, total - 1);
	for (i = 0; i < LG_ROOM_TYPECOUNT; i++) {
		if (pick < g_LgRoomTypes[i].weight) {
			return i;
		}
		pick -= g_LgRoomTypes[i].weight;
	}
	return 0;
}

/* Room shapes. A room is a main rectangle and up to two wings: rectangles
 * set flush against one of its sides, sharing that edge as one wide opening
 * (lgAddPortalRun), so the room reads as one L, T, U or S shaped space. The
 * wings are areas of their own (every area stays a rectangle, which the
 * walls, tiles, navigation and lights rely on), kind LG_AREA_ROOM with the
 * main's room number, type, theme and ceiling. The mains are areas
 * 0..numrooms-1 (area index = room number), the wings come straight after. */
enum { LG_SHAPE_BOX, LG_SHAPE_L, LG_SHAPE_T, LG_SHAPE_U, LG_SHAPE_S, LG_SHAPE_COUNT };

/* chance of a shaped room, percent, by room type (office, hall, storage, lab) */
static const uint8_t g_LgShapePct[LG_ROOM_TYPECOUNT] = { 45, 55, 25, 50 };

typedef struct { int16_t x, z, w, d, room; } LgRect;

static int lgRectsClear(const LgRect *rects, int n, int x, int z, int w, int d, int room, int gw, int gd)
{
	int i;

	if (x < 1 || z < 1 || x + w > gw - 1 || z + d > gd - 1) {
		return 0;
	}
	for (i = 0; i < n; i++) {
		const LgRect *o = &rects[i];

		if (o->room == room) {
			/* its own room: no overlap at all, touching allowed */
			if (x < o->x + o->w && o->x < x + w && z < o->z + o->d && o->z < z + d) {
				return 0;
			}
		} else if (x < o->x + o->w + LG_RINGGAP && o->x < x + w + LG_RINGGAP
				&& z < o->z + o->d + LG_RINGGAP && o->z < z + d + LG_RINGGAP) {
			return 0;
		}
	}
	return 1;
}

/* A wing on side 0 +x, 1 -x, 2 +z, 3 -z of the main m, placed along that side
 * at where: 0 the low end, 1 the middle, 2 the high end. len along the side,
 * depth out from it. */
static LgRect lgWingRect(const LgRect *m, int side, int where, int len, int depth)
{
	LgRect w;
	const int edge = side < 2 ? m->d : m->w;
	const int at = where == 0 ? 0 : where == 1 ? (edge - len) / 2 : edge - len;

	w.room = m->room;
	if (side < 2) {
		w.x = (int16_t)(side == 0 ? m->x + m->w : m->x - depth);
		w.z = (int16_t)(m->z + at);
		w.w = (int16_t)depth;
		w.d = (int16_t)len;
	} else {
		w.x = (int16_t)(m->x + at);
		w.z = (int16_t)(side == 2 ? m->z + m->d : m->z - depth);
		w.w = (int16_t)len;
		w.d = (int16_t)depth;
	}
	return w;
}

/* Up to two wings for the main m; returns how many were found room for */
/* A wing's rectangle is rock, inside the grid, no other room comes within two
 * cells of it and no passage (laid first) touches it */
static int lgWingClear(const LgLayout *lg, const LgRect *w)
{
	int x, z;

	if (w->x < 1 || w->z < 1 || w->x + w->w > lg->gw - 1 || w->z + w->d > lg->gd - 1) {
		return 0;
	}
	for (z = w->z - 2; z < w->z + w->d + 2; z++) {
		for (x = w->x - 2; x < w->x + w->w + 2; x++) {
			int a;

			if (x < 0 || z < 0 || x >= lg->gw || z >= lg->gd || lg->cells[z][x] == 0) {
				continue;
			}
			if (x >= w->x && x < w->x + w->w && z >= w->z && z < w->z + w->d) {
				return 0;
			}
			a = lg->cells[z][x] - 1;
			if (lg->areas[a].kind != LG_AREA_ROOM) {
				/* a passage may come within a cell (no shared edge, no opening) */
				if (x >= w->x - 1 && x < w->x + w->w + 1 && z >= w->z - 1 && z < w->z + w->d + 1) {
					return 0;
				}
			} else if (lg->areas[a].room != w->room) {
				return 0;
			}
		}
	}
	return 1;
}

static int lgPickWings(const LgLayout *lg, LgRng *r, const LgRect *m, int type, LgRect out[2])
{
	int shape, side, edge, other, len, depth, tries;

	if (lgRange(r, 0, 99) >= g_LgShapePct[type]) {
		return 0;
	}
	for (tries = 0; tries < 6; tries++) {
		shape = lgRange(r, LG_SHAPE_L, LG_SHAPE_COUNT - 1);
		side = lgRange(r, 0, 3);
		edge = side < 2 ? m->d : m->w;
		other = side < 2 ? m->w : m->d;
		depth = lgRange(r, 3, other * 2 / 3 > 3 ? other * 2 / 3 : 3);

		if (shape == LG_SHAPE_U) {
			/* two prongs at both ends of one side, a notch of 2+ cells between */
			if (edge < 8) continue;
			len = lgRange(r, 3, (edge - 2) / 2);
			out[0] = lgWingRect(m, side, 0, len, depth);
			out[1] = lgWingRect(m, side, 2, len, depth);
		} else if (shape == LG_SHAPE_S) {
			/* wings on opposite sides at opposite ends */
			if (edge < 6) continue;
			len = lgRange(r, 3, edge * 2 / 3);
			out[0] = lgWingRect(m, side, 0, len, depth);
			out[1] = lgWingRect(m, side ^ 1, 2, len, depth);
		} else {
			/* L: one wing at an end; T: one in the middle */
			if (edge < 5) continue;
			len = lgRange(r, 3, edge * 2 / 3);
			out[0] = lgWingRect(m, side, shape == LG_SHAPE_T ? 1 : (lgRand(r) & 1) * 2, len, depth);
			if (lgWingClear(lg, &out[0])) {
				return 1;
			}
			continue;
		}
		if (lgWingClear(lg, &out[0]) && lgWingClear(lg, &out[1])
				&& (out[0].x + out[0].w + 2 <= out[1].x || out[1].x + out[1].w + 2 <= out[0].x
					|| out[0].z + out[0].d + 2 <= out[1].z || out[1].z + out[1].d + 2 <= out[0].z)) {
			return 2;
		}
	}
	return 0;
}

static void lgSetRoomArea(LgArea *a, const LgRect *rc, int type, int room, int level, int ceil)
{
	memset(a, 0, sizeof(*a));
	a->x = rc->x;
	a->z = rc->z;
	a->w = rc->w;
	a->d = rc->d;
	a->kind = LG_AREA_ROOM;
	a->roomtype = (uint8_t)type;
	a->theme = g_LgRoomTypes[type].theme;
	a->room = (int16_t)room;
	a->floor = a->floorlo = (int16_t)(level * LG_LEVEL);
	a->ceil = (int16_t)(a->floor + ceil);
	a->dist = -1;
	a->centrepad = -1;
}

/* The hub as it is planned before it becomes areas: its floor and terrace
 * blocks, and where rooms attach */
#define LG_MAXBLOCKS 32

typedef struct {
	LgRect rc;
	int16_t lo, hi;        /* floor at its two ends along the hub (equal unless a ramp) */
	uint8_t ramp;          /* LG_RAMP_*, or 0 */
} LgBlock;

typedef struct {
	LgRect floorrc;        /* the open floor, end to end */
	LgBlock blocks[LG_MAXBLOCKS];
	int nblocks;
	int alongx;            /* the hub's long axis runs along x */
} LgHubPlan;

/* One terrace along the hub's long side: blocks from one end to the other,
 * the first flat on the floor's level (the way up from the floor), each next
 * flat at the level reached or a ramp up or down one level. Returns 0 if it
 * did not climb to minpeak. */
static int lgTerrace(LgHubPlan *h, LgRng *r, int size, int along0, int len, int across0, int depth, int maxlevel, int minpeak)
{
	const int first = h->nblocks;
	int pos = 0, level = 0, peak = 0, k = 0, lastramp = 1;

	while (pos < len && h->nblocks < LG_MAXBLOCKS) {
		LgBlock *b = &h->blocks[h->nblocks];
		int bl = lgRange(r, g_LgSizes[size].blockmin, g_LgSizes[size].blockmax);
		int next = level;

		bl = len - pos - bl < 4 ? len - pos : bl;     /* no stub at the end */
		if (k > 0 && !lastramp && bl >= LG_RAMPCELLS && pos + bl < len) {   /* never ends on a ramp: a landing at the top */
			const int roll = lgRange(r, 0, 99);

			if (roll < 60 && level < maxlevel) next = level + 1;
			else if (roll < 75 && level > 0) next = level - 1;
		}
		b->ramp = 0;
		b->lo = (int16_t)(level * LG_LEVEL);
		b->hi = (int16_t)(next * LG_LEVEL);
		if (h->alongx) {
			b->rc.x = (int16_t)(along0 + pos); b->rc.w = (int16_t)bl;
			b->rc.z = (int16_t)across0; b->rc.d = (int16_t)depth;
			if (next != level) b->ramp = next > level ? LG_RAMP_XP : LG_RAMP_XN;
		} else {
			b->rc.z = (int16_t)(along0 + pos); b->rc.d = (int16_t)bl;
			b->rc.x = (int16_t)across0; b->rc.w = (int16_t)depth;
			if (next != level) b->ramp = next > level ? LG_RAMP_ZP : LG_RAMP_ZN;
		}
		b->rc.room = 0;
		lastramp = next != level;
		level = next;
		peak = level > peak ? level : peak;
		pos += bl;
		k++;
		h->nblocks++;
	}
	if (peak < minpeak) {
		h->nblocks = first;
		return 0;
	}
	return 1;
}

static void lgSetHubArea(LgArea *a, const LgRect *rc, int room, int lo, int hi, int ramp)
{
	LgRect copy = *rc;

	lgSetRoomArea(a, &copy, LG_ROOM_HALL, room, 0, LG_HUB_CEIL);
	a->flags = LG_AREAFLAG_HUB;
	a->ramp = (uint8_t)ramp;
	a->floor = (int16_t)(lo > hi ? lo : hi);
	a->floorlo = (int16_t)(lo < hi ? lo : hi);
	a->ceil = LG_HUB_CEIL;
}

/* A room's own terrace (side rooms climb too, 2026-11-14): a strip along one
 * of its walls - its back or a side, never the one facing the hub, where its
 * passage comes in - cut off the room: a flat entry on the room's floor, a
 * ramp up a level, a landing; halls and labs with room for it climb a second
 * level. The strip's edges over the room's floor are ledges (balconies over
 * the room). Shrinks the room to what is left; returns the blocks and how far
 * up the top one is. */
static const struct {
	uint8_t pct;          /* chance of a terrace */
	int8_t entry;         /* flat entry, cells */
	int8_t ramp;          /* ramp, cells */
	int16_t step;         /* the climb per ramp */
} g_LgTerrace[LG_ROOM_TYPECOUNT] = {
	{ 70, 1, 3, 100 },    /* office: a short climb, 1 in 3 */
	{ 85, 2, LG_RAMPCELLS, LG_LEVEL },
	{ 55, 1, 3, 100 },    /* storage */
	{ 75, 2, LG_RAMPCELLS, LG_LEVEL },
};

static int lgRoomTerrace(LgRng *r, LgRect *room, int type, int side, int base, LgBlock *out, int *raise)
{
	/* the room's face towards the hub: the side it lies on, turned round */
	const int hubface = side ^ 1;
	int faces[3], nf = 0, f, face, depth, len, perp, n = 0, pos, steps, i, flip;

	*raise = 0;
	const int entry = g_LgTerrace[type].entry, rc = g_LgTerrace[type].ramp, step = g_LgTerrace[type].step;

	if (lgRange(r, 0, 99) >= g_LgTerrace[type].pct) {
		return 0;
	}
	for (f = 0; f < 4; f++) {
		if (f != hubface) faces[nf++] = f;
	}
	face = faces[lgRange(r, 0, nf - 1)];
	len = face < 2 ? room->d : room->w;          /* faces 0 -x, 1 +x run along z */
	perp = face < 2 ? room->w : room->d;
	depth = type == LG_ROOM_HALL ? lgRange(r, 3, 4) : lgRange(r, 2, 3);
	if (len < entry + rc + 2 || perp - depth < 4) {
		return 0;
	}
	steps = (type == LG_ROOM_HALL || type == LG_ROOM_LAB) && len >= entry + 2 * rc + 4 ? 2 : 1;
	flip = lgRand(r) & 1;                          /* which end the way up starts at */

	/* entry, ramp, landing (, ramp, landing): lengths along the strip */
	{
		int lens[5], k, rest;

		lens[0] = entry;
		lens[1] = rc;
		if (steps == 2) {
			rest = len - entry - 2 * rc;
			lens[2] = rest / 2;
			lens[3] = rc;
			lens[4] = rest - rest / 2;
			n = 5;
		} else {
			lens[2] = len - entry - rc;
			n = 3;
		}
		pos = 0;
		for (k = 0; k < n; k++) {
			const int at = flip ? len - pos - lens[k] : pos;
			const int level = (k + 1) / 2, rising = k & 1;
			LgBlock *b = &out[k];

			b->lo = (int16_t)(base + level * step - (rising ? step : 0));
			b->hi = (int16_t)(base + level * step);
			if (!rising) b->lo = b->hi = (int16_t)(base + k / 2 * step);
			b->ramp = 0;
			if (face < 2) {
				b->rc.x = (int16_t)(face == 0 ? room->x : room->x + room->w - depth);
				b->rc.w = (int16_t)depth;
				b->rc.z = (int16_t)(room->z + at);
				b->rc.d = (int16_t)lens[k];
				if (rising) b->ramp = flip ? LG_RAMP_ZN : LG_RAMP_ZP;
			} else {
				b->rc.z = (int16_t)(face == 2 ? room->z : room->z + room->d - depth);
				b->rc.d = (int16_t)depth;
				b->rc.x = (int16_t)(room->x + at);
				b->rc.w = (int16_t)lens[k];
				if (rising) b->ramp = flip ? LG_RAMP_XN : LG_RAMP_XP;
			}
			b->rc.room = room->room;
			pos += lens[k];
		}
	}
	*raise = steps * step;

	/* what is left of the room */
	switch (face) {
	case 0: room->x = (int16_t)(room->x + depth); room->w = (int16_t)(room->w - depth); break;
	case 1: room->w = (int16_t)(room->w - depth); break;
	case 2: room->z = (int16_t)(room->z + depth); room->d = (int16_t)(room->d - depth); break;
	default: room->d = (int16_t)(room->d - depth); break;
	}
	(void)i;
	return n;
}

/* Step 1: the hub, the rooms off it, and their floor levels. Rooms are areas
 * 0..numrooms-1 with the hub's floor last; area 0 (the start) is a room on
 * the ground. The hub's terrace blocks follow (the hub's room number); then
 * attach[] says which hub area each room's passage goes to. */
static int lgPlaceHub(LgLayout *lg, LgRng *r, int16_t *attach)
{
	const int c = lg->gw / 2;
	const int maxlevel = g_LgSizes[lg->size].maxlevel;
	const int minpeak = maxlevel >= 2 ? 2 : 1;
	LgHubPlan h;
	LgRect rects[LG_MAXROOMS + 1], hubrc, rooms[LG_MAXROOMS];
	int types[LG_MAXROOMS], levels[LG_MAXROOMS], att[LG_MAXROOMS], sides[LG_MAXROOMS];
	int cand[LG_MAXBLOCKS + 4][4];  /* hub area (-1 floor), side 0 -x 1 +x 2 -z 3 +z, centre along, level */
	int ncand = 0, nrooms = 0, nrects = 0, want, i, k, x, z, tries;
	int len, wid, t1, t2;

	memset(&h, 0, sizeof(h));
	h.alongx = lgRand(r) & 1;
	len = lgRange(r, g_LgSizes[lg->size].hubwmin, g_LgSizes[lg->size].hubwmax);
	wid = lgRange(r, g_LgSizes[lg->size].hubdmin, g_LgSizes[lg->size].hubdmax);
	t1 = lgRange(r, g_LgSizes[lg->size].terrmin, g_LgSizes[lg->size].terrmax);
	t2 = lgRange(r, g_LgSizes[lg->size].terrmin, g_LgSizes[lg->size].terrmax);
	hubrc.room = -2;
	if (h.alongx) {
		hubrc.w = (int16_t)len; hubrc.d = (int16_t)wid;
	} else {
		hubrc.w = (int16_t)wid; hubrc.d = (int16_t)len;
	}
	hubrc.x = (int16_t)(c - hubrc.w / 2 + lgRange(r, -1, 1));
	hubrc.z = (int16_t)(c - hubrc.d / 2 + lgRange(r, -1, 1));

	/* the terraces down both long sides, then the floor between them */
	{
		const int along0 = h.alongx ? hubrc.x : hubrc.z, across0 = h.alongx ? hubrc.z : hubrc.x;

		for (tries = 0; tries < 8 && !lgTerrace(&h, r, lg->size, along0, len, across0, t1, maxlevel, minpeak); tries++);
		for (tries = 0; tries < 8 && !lgTerrace(&h, r, lg->size, along0, len, across0 + wid - t2, t2, maxlevel, 1); tries++);
		if (h.alongx) {
			h.floorrc.x = hubrc.x; h.floorrc.w = hubrc.w;
			h.floorrc.z = (int16_t)(hubrc.z + t1); h.floorrc.d = (int16_t)(wid - t1 - t2);
		} else {
			h.floorrc.z = hubrc.z; h.floorrc.d = hubrc.d;
			h.floorrc.x = (int16_t)(hubrc.x + t1); h.floorrc.w = (int16_t)(wid - t1 - t2);
		}
		h.floorrc.room = 0;
	}
	if (h.nblocks < 2) {
		return 0;
	}
	rects[nrects++] = hubrc;

	/* where rooms can attach: the outer side of every flat terrace block,
	 * and the floor at both ends of the hub */
	for (i = 0; i < h.nblocks; i++) {
		const LgBlock *b = &h.blocks[i];
		const int top = h.alongx ? b->rc.z == hubrc.z : b->rc.x == hubrc.x;

		if (b->ramp || (h.alongx ? b->rc.w : b->rc.d) < 4) {
			continue;
		}
		cand[ncand][0] = i;
		cand[ncand][1] = h.alongx ? (top ? 2 : 3) : (top ? 0 : 1);
		cand[ncand][2] = h.alongx ? b->rc.x + b->rc.w / 2 : b->rc.z + b->rc.d / 2;
		cand[ncand][3] = b->lo / LG_LEVEL;
		ncand++;
	}
	for (k = 0; k < 2; k++) {
		cand[ncand][0] = -1;
		cand[ncand][1] = h.alongx ? k : 2 + k;
		cand[ncand][2] = h.alongx ? h.floorrc.z + h.floorrc.d / 2 : h.floorrc.x + h.floorrc.w / 2;
		cand[ncand][3] = 0;
		ncand++;
	}
	for (i = ncand - 1; i > 0; i--) {
		const int j = lgRange(r, 0, i);
		int t[4];

		memcpy(t, cand[i], sizeof(t)); memcpy(cand[i], cand[j], sizeof(t)); memcpy(cand[j], t, sizeof(t));
	}

	/* the rooms: out from their attach side, centred on it, as close as fits */
	want = lgRange(r, g_LgSizes[lg->size].minoff, g_LgSizes[lg->size].maxoff);
	for (i = 0; i < ncand && nrooms < want && nrooms < LG_MAXROOMS - 1; i++) {
		const int side = cand[i][1];
		int type = lgPickRoomType(r), w, d, placed = 0;

		if (type == LG_ROOM_HALL && lg->size != LG_SIZE_LARGE) {
			type = LG_ROOM_LAB;
		}
		w = lgRange(r, g_LgRoomTypes[type].minw, g_LgRoomTypes[type].maxw);
		d = lgRange(r, g_LgRoomTypes[type].minw, g_LgRoomTypes[type].maxw);
		while (!placed && w >= 5 && d >= 5) {
			int gap;

			for (gap = LG_RINGGAP; gap <= LG_RINGGAP + 6 && !placed; gap++) {
				const int shift = lgRange(r, -1, 1);

				switch (side) {
				case 0: x = hubrc.x - gap - w; z = cand[i][2] - d / 2 + shift; break;
				case 1: x = hubrc.x + hubrc.w + gap; z = cand[i][2] - d / 2 + shift; break;
				case 2: z = hubrc.z - gap - d; x = cand[i][2] - w / 2 + shift; break;
				default: z = hubrc.z + hubrc.d + gap; x = cand[i][2] - w / 2 + shift; break;
				}
				placed = lgRectsClear(rects, nrects, x, z, w, d, -1, lg->gw, lg->gd);
			}
			if (!placed) {
				w -= 2;
				d -= 2;
			}
		}
		if (!placed) {
			continue;
		}
		rooms[nrooms].x = (int16_t)x; rooms[nrooms].z = (int16_t)z;
		rooms[nrooms].w = (int16_t)w; rooms[nrooms].d = (int16_t)d;
		rooms[nrooms].room = (int16_t)nrooms;
		types[nrooms] = type;
		levels[nrooms] = cand[i][3];
		att[nrooms] = cand[i][0];
		sides[nrooms] = side;
		rects[nrects++] = rooms[nrooms];
		nrooms++;
	}
	if (nrooms < 3) {
		return 0;
	}

	/* the start: a room on the ground goes first */
	for (i = 0; i < nrooms && levels[i] != 0; i++);
	if (i == nrooms) {
		return 0;
	}
	if (i != 0) {
		LgRect tr = rooms[0]; int ti;

		rooms[0] = rooms[i]; rooms[i] = tr;
		ti = types[0]; types[0] = types[i]; types[i] = ti;
		ti = levels[0]; levels[0] = levels[i]; levels[i] = ti;
		ti = att[0]; att[0] = att[i]; att[i] = ti;
		ti = sides[0]; sides[0] = sides[i]; sides[i] = ti;
	}

	/* the areas: the rooms, the hub's floor (its main area), its blocks, then
	 * the rooms' own terraces */
	{
		LgBlock terr[LG_MAXROOMS][5];
		int nterr[LG_MAXROOMS], raise[LG_MAXROOMS];

		for (i = 0; i < nrooms; i++) {
			rooms[i].room = (int16_t)i;
			nterr[i] = lgRoomTerrace(r, &rooms[i], types[i], sides[i], levels[i] * LG_LEVEL, terr[i], &raise[i]);
			lgSetRoomArea(&lg->areas[i], &rooms[i], types[i], i, levels[i], g_LgRoomTypes[types[i]].ceil);
			if (raise[i] + 280 > g_LgRoomTypes[types[i]].ceil) {
				lg->areas[i].ceil = (int16_t)(lg->areas[i].floor + raise[i] + 280);   /* headroom on the terrace */
			}
		}
		lgSetHubArea(&lg->areas[nrooms], &h.floorrc, nrooms, 0, 0, 0);
		lg->numrooms = (int16_t)(nrooms + 1);
		lg->numareas = lg->numrooms;
		for (k = 0; k < h.nblocks; k++) {
			lgSetHubArea(&lg->areas[lg->numareas++], &h.blocks[k].rc, nrooms, h.blocks[k].lo, h.blocks[k].hi, h.blocks[k].ramp);
		}
		for (i = 0; i < nrooms; i++) {
			for (k = 0; k < nterr[i]; k++) {
				LgArea *a = &lg->areas[lg->numareas++];

				*a = lg->areas[i];
				a->x = terr[i][k].rc.x; a->z = terr[i][k].rc.z; a->w = terr[i][k].rc.w; a->d = terr[i][k].rc.d;
				a->flags = LG_AREAFLAG_TERRACE;
				a->ramp = terr[i][k].ramp;
				a->floor = (int16_t)(terr[i][k].lo > terr[i][k].hi ? terr[i][k].lo : terr[i][k].hi);
				a->floorlo = (int16_t)(terr[i][k].lo < terr[i][k].hi ? terr[i][k].lo : terr[i][k].hi);
			}
		}
	}
	for (i = 0; i < nrooms; i++) {
		attach[i] = (int16_t)(att[i] < 0 ? nrooms : nrooms + 1 + att[i]);
	}

	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];
		for (z = a->z; z < a->z + a->d; z++) {
			for (x = a->x; x < a->x + a->w; x++) {
				lg->cells[z][x] = (int16_t)(i + 1);
			}
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* The underground lab layout                                           */

/* Hallways leading to rooms (2026-11-14). A lattice of corridors: lines each
 * way, a node where two cross, a segment between neighbouring nodes. Some
 * segments are cut so the halls bend and no hall runs straight for long. The
 * blocks between the lines hold the rooms, each set flush against the halls
 * round it (a doorway on every hall it touches), and more rooms line the
 * outside. Several blocks together make the atrium, a hall two storeys high.
 * One side of the lab is a level up: the halls crossing over are ramps.
 * Rooms get wings, galleries, mezzanines and terraces as in the hub layout. */
#define LG_LAB_MAXLINES 6
#define LG_LAB_MAXCORR  (LG_LAB_MAXLINES * LG_LAB_MAXLINES * 3)

static const struct {
	int8_t lines;          /* corridor lines each way */
	int8_t edge;           /* cells outside the lattice, for the outer rooms */
	int8_t mergew, merged; /* the atrium: blocks along and across */
	int8_t cutpct;         /* segments cut, percent */
	int8_t maxrooms;
	int8_t mezzpct;        /* chance a room that fits a mezzanine gets one */
	int8_t span;           /* the lab's extent each way, cells (it sits in the middle of the grid) */
} g_LgLab[LG_SIZE_COUNT] = {
	{ 3,  7, 2, 1, 10, 10, 100, 40 },
	{ 3,  9, 2, 1, 10, 12, 100, 52 },
	{ 4,  9, 2, 2, 15, 18, 60, 68 },
};

typedef struct {
	int n;
	int16_t px[LG_LAB_MAXLINES], pw[LG_LAB_MAXLINES];   /* the lines along z: x and width */
	int16_t pz[LG_LAB_MAXLINES], pd[LG_LAB_MAXLINES];   /* the lines along x: z and width */
	uint8_t hseg[LG_LAB_MAXLINES][LG_LAB_MAXLINES];     /* [j][i]: on row j from column i to i + 1 */
	uint8_t vseg[LG_LAB_MAXLINES][LG_LAB_MAXLINES];     /* [j][i]: on column i from row j to j + 1 */
	uint8_t gone[LG_LAB_MAXLINES][LG_LAB_MAXLINES];     /* [j][i]: the node is inside the atrium */
	int8_t level[LG_LAB_MAXLINES][LG_LAB_MAXLINES];     /* [j][i]: the node's floor level */
	int mi, mj, mw, md;                                 /* the atrium's blocks */
} LgLabPlan;

typedef struct {
	LgRect rc;
	int16_t lo, hi;
	uint8_t ramp;
} LgLabCorr;

static int lgLabDegree(const LgLabPlan *p, int i, int j)
{
	return (i > 0 && p->hseg[j][i - 1]) + (i < p->n - 1 && p->hseg[j][i])
		+ (j > 0 && p->vseg[j - 1][i]) + (j < p->n - 1 && p->vseg[j][i]);
}

/* every node that is left has two halls or more and all are joined */
static int lgLabSound(const LgLabPlan *p)
{
	uint8_t seen[LG_LAB_MAXLINES][LG_LAB_MAXLINES];
	int8_t qi[LG_LAB_MAXLINES * LG_LAB_MAXLINES], qj[LG_LAB_MAXLINES * LG_LAB_MAXLINES];
	int i, j, head = 0, tail = 0, total = 0;

	memset(seen, 0, sizeof(seen));
	for (j = 0; j < p->n; j++) {
		for (i = 0; i < p->n; i++) {
			if (p->gone[j][i]) {
				continue;
			}
			if (lgLabDegree(p, i, j) < 2) {
				return 0;
			}
			if (total++ == 0) {
				seen[j][i] = 1; qi[tail] = (int8_t)i; qj[tail++] = (int8_t)j;
			}
		}
	}
	while (head < tail) {
		i = qi[head]; j = qj[head++];
#define LG_LAB_VISIT(c, ni, nj) if ((c) && !seen[nj][ni]) { seen[nj][ni] = 1; qi[tail] = (int8_t)(ni); qj[tail++] = (int8_t)(nj); }
		LG_LAB_VISIT(i > 0 && p->hseg[j][i - 1], i - 1, j)
		LG_LAB_VISIT(i < p->n - 1 && p->hseg[j][i], i + 1, j)
		LG_LAB_VISIT(j > 0 && p->vseg[j - 1][i], i, j - 1)
		LG_LAB_VISIT(j < p->n - 1 && p->vseg[j][i], i, j + 1)
#undef LG_LAB_VISIT
	}
	return tail == total;
}

/* cuts a segment if the lattice stays sound; returns 1 if it was cut */
static int lgLabCut(LgLabPlan *p, int horiz, int i, int j)
{
	uint8_t *s = horiz ? &p->hseg[j][i] : &p->vseg[j][i];

	if (!*s) {
		return 0;
	}
	*s = 0;
	if (lgLabSound(p)) {
		return 1;
	}
	*s = 1;
	return 0;
}

/* the corridor (index + 1) on each cell, while the rooms are fitted */
typedef struct { uint8_t c[LG_GRIDMAX][LG_GRIDMAX]; } LgLabGrid;

/* What lies along side f (0 -x, 1 +x, 2 -z, 3 +z) of the rectangle: cells of
 * flat corridor at the level wanted (returned), and whether any other corridor
 * (a ramp, another level) touches it (*bad). level < 0: any flat one counts,
 * and *found gets one such level. */
static int lgLabSide(const LgLabGrid *g, const LgLabCorr *corr, const LgRect *rc, int f, int level, int *bad, int *found)
{
	const int len = f < 2 ? rc->d : rc->w;
	int k, good = 0;

	*bad = 0;
	for (k = 0; k < len; k++) {
		const int x = f == 0 ? rc->x - 1 : f == 1 ? rc->x + rc->w : rc->x + k;
		const int z = f == 2 ? rc->z - 1 : f == 3 ? rc->z + rc->d : rc->z + k;
		const LgLabCorr *c;

		if (x < 0 || z < 0 || x >= LG_GRIDMAX || z >= LG_GRIDMAX || !g->c[z][x]) {
			continue;
		}
		c = &corr[g->c[z][x] - 1];
		if (c->ramp || (level >= 0 && c->lo != level * LG_LEVEL)) {
			*bad = 1;
		} else {
			good++;
			if (found) *found = c->lo / LG_LEVEL;
		}
	}
	return good;
}

static void lgLabInset(LgRect *rc, int f, int by)
{
	switch (f) {
	case 0: rc->x = (int16_t)(rc->x + by); rc->w = (int16_t)(rc->w - by); break;
	case 1: rc->w = (int16_t)(rc->w - by); break;
	case 2: rc->z = (int16_t)(rc->z + by); rc->d = (int16_t)(rc->d - by); break;
	default: rc->d = (int16_t)(rc->d - by); break;
	}
}

/* Fits a room into the space rc: at the level of a hall beside it, kept a
 * cell back from every hall it cannot open onto (a ramp, a hall on another
 * level), never against halls on all four sides (a mezzanine needs a side
 * wall), now and then smaller than its space. Returns its level, or -1 if no
 * room goes here; *faces gets the sides that open on a hall. */
static int lgLabFitRoom(const LgLabGrid *g, const LgLabCorr *corr, LgRng *r, LgRect *rc, int maxw, int vary, int *faces)
{
	int levels[4], nl = 0, f, level, bad, found = 0, touch[4], nt = 0, tries;
	LgRect keep;

	for (f = 0; f < 4; f++) {
		if (lgLabSide(g, corr, rc, f, -1, &bad, &found) >= 3) {
			levels[nl++] = found;
		}
	}
	if (nl == 0) {
		return -1;
	}
	level = levels[lgRange(r, 0, nl - 1)];
	for (f = 0; f < 4; f++) {
		lgLabSide(g, corr, rc, f, level, &bad, NULL);
		if (bad) {
			lgLabInset(rc, f, 1);
		}
	}
	keep = *rc;
	for (tries = 0; tries < 2; tries++) {
		nt = 0;
		for (f = 0; f < 4; f++) {
			touch[f] = lgLabSide(g, corr, rc, f, level, &bad, NULL) >= 3 && !bad;
			nt += touch[f];
		}
		if (tries == 1) {
			break;
		}
		/* against halls all round: back off one of them */
		if (nt == 4) {
			f = lgRange(r, 0, 3);
			lgLabInset(rc, f, 1);
			touch[f] = 0;
			nt = 3;
		}
		/* smaller than the space: pull a side in (a wall side first), and no
		 * wider than the room type likes */
		for (f = 0; f < 4; f++) {
			const int span = f < 2 ? rc->w : rc->d;
			int by = 0;

			if (touch[f] && nt == 1) {
				continue;   /* its only hall */
			}
			if (span > maxw && !touch[f] && !touch[f ^ 1]) {
				by = (span - maxw + 1) / 2;
			} else if (span > maxw && !touch[f]) {
				by = span - maxw;
			} else if (vary && lgRange(r, 0, 99) < (touch[f] ? 12 : 30)) {
				by = lgRange(r, 1, 2);
			}
			if (by > 0 && span - by >= 6) {
				lgLabInset(rc, f, by);
				if (touch[f]) {
					touch[f] = 0;
					nt--;
				}
			}
		}
	}
	if (nt == 0) {
		/* pulled away from its halls: the space as it was */
		*rc = keep;
		nt = 0;
		for (f = 0; f < 4; f++) {
			touch[f] = lgLabSide(g, corr, rc, f, level, &bad, NULL) >= 3 && !bad;
			nt += touch[f];
		}
		if (nt == 4) {
			lgLabInset(rc, 0, 1);
			touch[0] = 0;
			nt = 3;
		}
	}
	if (nt == 0 || rc->w < 5 || rc->d < 5) {
		return -1;
	}
	*faces = touch[0] | touch[1] << 1 | touch[2] << 2 | touch[3] << 3;
	return level;
}

static int lgLabTouches(const LgLabGrid *g, const LgRect *rc)
{
	int x, z;

	for (z = rc->z - 1; z <= rc->z + rc->d; z++) {
		for (x = rc->x - 1; x <= rc->x + rc->w; x++) {
			const int inx = x >= rc->x && x < rc->x + rc->w, inz = z >= rc->z && z < rc->z + rc->d;

			if (inx != inz && x >= 0 && z >= 0 && x < LG_GRIDMAX && z < LG_GRIDMAX && g->c[z][x]) {
				return 1;
			}
		}
	}
	return 0;
}

static int lgPlaceLab(LgLayout *lg, LgRng *r)
{
	static LgLabGrid grid;   /* scratch, cleared here every time */
	LgLabGrid *g = &grid;
	LgLabPlan p;
	LgLabCorr corr[LG_LAB_MAXCORR];
	LgRect space[LG_MAXROOMS * 3], rooms[LG_MAXROOMS];
	uint8_t outer[LG_MAXROOMS * 3], atrium[LG_MAXROOMS * 3];
	int types[LG_MAXROOMS], levels[LG_MAXROOMS], faces[LG_MAXROOMS], isatrium[LG_MAXROOMS], isouter[LG_MAXROOMS];
	const int n = g_LgLab[lg->size].lines, span = g_LgLab[lg->size].span, off = (lg->gw - span) / 2;
	const int edge = off + g_LgLab[lg->size].edge, gw = off + span + off;   /* the lab's own extent: off .. gw - off */
	int ncorr = 0, nspace = 0, nrooms = 0, i, j, k, x, z, cuts, want, start;

	memset(&p, 0, sizeof(p));
	memset(g, 0, sizeof(*g));
	p.n = n;

	/* the lines: most three cells wide, some two (service tunnels) */
	for (i = 0; i < n; i++) {
		p.pw[i] = (int16_t)(lgRange(r, 0, 99) < 30 ? 2 : 3);
		p.pd[i] = (int16_t)(lgRange(r, 0, 99) < 30 ? 2 : 3);
	}
	for (i = 0; i < n; i++) {
		const int x1 = gw - edge - p.pw[n - 1], z1 = gw - edge - p.pd[n - 1];

		p.px[i] = (int16_t)(edge + (x1 - edge) * i / (n - 1) + (i > 0 && i < n - 1 ? lgRange(r, -1, 1) : 0));
		p.pz[i] = (int16_t)(edge + (z1 - edge) * i / (n - 1) + (i > 0 && i < n - 1 ? lgRange(r, -1, 1) : 0));
	}
	for (j = 0; j < n; j++) {
		for (i = 0; i < n; i++) {
			p.hseg[j][i] = (uint8_t)(i < n - 1);
			p.vseg[j][i] = (uint8_t)(j < n - 1);
		}
	}

	/* the atrium: blocks merged into one hall, the halls inside it gone */
	p.mw = g_LgLab[lg->size].mergew;
	p.md = g_LgLab[lg->size].merged;
	if (p.mw != p.md && (lgRand(r) & 1)) {
		const int t = p.mw; p.mw = p.md; p.md = t;
	}
	p.mi = lgRange(r, 0, n - 1 - p.mw);
	p.mj = lgRange(r, 0, n - 1 - p.md);
	for (j = p.mj; j < p.mj + p.md; j++) {
		for (i = p.mi + 1; i < p.mi + p.mw; i++) p.vseg[j][i] = 0;
	}
	for (j = p.mj + 1; j < p.mj + p.md; j++) {
		for (i = p.mi; i < p.mi + p.mw; i++) p.hseg[j][i] = 0;
	}
	for (j = p.mj + 1; j < p.mj + p.md; j++) {
		for (i = p.mi + 1; i < p.mi + p.mw; i++) p.gone[j][i] = 1;
	}

	/* the levels: one side of a line a level up (and on a large lab, now and
	 * then, one side of a line the other way another level) */
	{
		const int alongx = lgRand(r) & 1, flip = lgRand(r) & 1;
		const int k1 = lgRange(r, 1, n - 1);
		const int two = lg->size == LG_SIZE_LARGE && lgRange(r, 0, 99) < 50, k2 = lgRange(r, 1, n - 1), flip2 = lgRand(r) & 1;

		for (j = 0; j < n; j++) {
			for (i = 0; i < n; i++) {
				const int a = alongx ? i : j, b = alongx ? j : i;

				p.level[j][i] = (int8_t)(((a >= k1) != flip) + (two && ((b >= k2) != flip2)));
			}
		}
	}

	/* cuts: first the middle of any three segments in a line, then at random */
	for (j = 0; j < n; j++) {
		for (i = 1; i + 1 < n - 1; i++) {
			if (p.hseg[j][i - 1] && p.hseg[j][i] && p.hseg[j][i + 1]) lgLabCut(&p, 1, i, j);
			if (p.vseg[i - 1][j] && p.vseg[i][j] && p.vseg[i + 1][j]) lgLabCut(&p, 0, j, i);
		}
	}
	cuts = 2 * n * (n - 1) * g_LgLab[lg->size].cutpct / 100;
	for (k = 0; k < 4 * n * n && cuts > 0; k++) {
		const int horiz = lgRand(r) & 1;

		i = lgRange(r, 0, horiz ? n - 2 : n - 1);
		j = lgRange(r, 0, horiz ? n - 1 : n - 2);
		cuts -= lgLabCut(&p, horiz, i, j);
	}
	if (!lgLabSound(&p)) {
		return 0;
	}

	/* the corridors: nodes, then segments (flat, or a ramp between levels) */
	for (j = 0; j < n; j++) {
		for (i = 0; i < n; i++) {
			LgLabCorr *c = &corr[ncorr];

			if (p.gone[j][i]) {
				continue;
			}
			c->rc.x = p.px[i]; c->rc.z = p.pz[j]; c->rc.w = p.pw[i]; c->rc.d = p.pd[j]; c->rc.room = -1;
			c->lo = c->hi = (int16_t)(p.level[j][i] * LG_LEVEL);
			c->ramp = 0;
			ncorr++;
		}
	}
	for (j = 0; j < n; j++) {
		for (i = 0; i < n; i++) {
			for (k = 0; k < 2; k++) {
				LgLabCorr *c = &corr[ncorr];
				const int la = p.level[j][i], lb = k == 0 ? (i < n - 1 ? p.level[j][i + 1] : 0) : (j < n - 1 ? p.level[j + 1][i] : 0);

				if (!(k == 0 ? p.hseg[j][i] : p.vseg[j][i])) {
					continue;
				}
				if (k == 0) {
					c->rc.x = (int16_t)(p.px[i] + p.pw[i]); c->rc.w = (int16_t)(p.px[i + 1] - c->rc.x);
					c->rc.z = p.pz[j]; c->rc.d = p.pd[j];
				} else {
					c->rc.z = (int16_t)(p.pz[j] + p.pd[j]); c->rc.d = (int16_t)(p.pz[j + 1] - c->rc.z);
					c->rc.x = p.px[i]; c->rc.w = p.pw[i];
				}
				c->rc.room = -1;
				c->lo = (int16_t)((la < lb ? la : lb) * LG_LEVEL);
				c->hi = (int16_t)((la > lb ? la : lb) * LG_LEVEL);
				c->ramp = (uint8_t)(la == lb ? 0 : k == 0 ? (lb > la ? LG_RAMP_XP : LG_RAMP_XN) : (lb > la ? LG_RAMP_ZP : LG_RAMP_ZN));
				if (c->ramp && (k == 0 ? c->rc.w : c->rc.d) < (c->hi - c->lo) / LG_LEVEL * LG_RAMPCELLS) {
					return 0;
				}
				ncorr++;
			}
		}
	}
	for (k = 0; k < ncorr; k++) {
		for (z = corr[k].rc.z; z < corr[k].rc.z + corr[k].rc.d; z++) {
			for (x = corr[k].rc.x; x < corr[k].rc.x + corr[k].rc.w; x++) {
				g->c[z][x] = (uint8_t)(k + 1);
			}
		}
	}

	/* the spaces rooms go in: the atrium, the other blocks, the outside */
	space[nspace].x = (int16_t)(p.px[p.mi] + p.pw[p.mi]); space[nspace].w = (int16_t)(p.px[p.mi + p.mw] - space[nspace].x);
	space[nspace].z = (int16_t)(p.pz[p.mj] + p.pd[p.mj]); space[nspace].d = (int16_t)(p.pz[p.mj + p.md] - space[nspace].z);
	outer[nspace] = 0; atrium[nspace++] = 1;
	for (j = 0; j < n - 1; j++) {
		for (i = 0; i < n - 1; i++) {
			if (i >= p.mi && i < p.mi + p.mw && j >= p.mj && j < p.mj + p.md) {
				continue;
			}
			space[nspace].x = (int16_t)(p.px[i] + p.pw[i]); space[nspace].w = (int16_t)(p.px[i + 1] - space[nspace].x);
			space[nspace].z = (int16_t)(p.pz[j] + p.pd[j]); space[nspace].d = (int16_t)(p.pz[j + 1] - space[nspace].z);
			outer[nspace] = 0; atrium[nspace++] = 0;
		}
	}
	for (i = 0; i < n - 1; i++) {
		for (k = 0; k < 4; k++) {
			LgRect *s = &space[nspace];

			if (k < 2) {            /* beyond the first and the last row */
				s->x = (int16_t)(p.px[i] + p.pw[i]); s->w = (int16_t)(p.px[i + 1] - s->x);
				s->z = (int16_t)(k == 0 ? off + 1 : p.pz[n - 1] + p.pd[n - 1]);
				s->d = (int16_t)(k == 0 ? p.pz[0] - off - 1 : gw - off - 1 - s->z);
			} else {                /* beyond the first and the last column */
				s->z = (int16_t)(p.pz[i] + p.pd[i]); s->d = (int16_t)(p.pz[i + 1] - s->z);
				s->x = (int16_t)(k == 2 ? off + 1 : p.px[n - 1] + p.pw[n - 1]);
				s->w = (int16_t)(k == 2 ? p.px[0] - off - 1 : gw - off - 1 - s->x);
			}
			outer[nspace] = 1; atrium[nspace++] = 0;
		}
	}
	/* the outer ones in a random order: not all of them get a room */
	for (i = nspace - 1; i > 0 && outer[i]; i--) {
		const int first = nspace - 4 * (n - 1);
		const int q = lgRange(r, first, i);
		const LgRect t = space[i];

		space[i] = space[q]; space[q] = t;
	}

	want = g_LgLab[lg->size].maxrooms;
	for (k = 0; k < nspace && nrooms < want && nrooms < LG_MAXROOMS; k++) {
		LgRect rc = space[k];
		int type, level, fc = 0;

		if (atrium[k]) {
			type = LG_ROOM_HALL;
		} else {
			const int pick = lgRange(r, 0, 9);

			if (!outer[k] && lgRange(r, 0, 99) < 8) {
				continue;   /* solid rock: a block with no room */
			}
			type = pick < 5 ? LG_ROOM_LAB : pick < 8 ? LG_ROOM_OFFICE : LG_ROOM_STORAGE;
		}
		rc.room = 0;
		level = lgLabFitRoom(g, corr, r, &rc, atrium[k] ? LG_GRIDMAX : g_LgRoomTypes[type].maxw + 2, !atrium[k], &fc);
		if (level < 0) {
			if (atrium[k]) {
				return 0;
			}
			continue;
		}
		rooms[nrooms] = rc;
		types[nrooms] = type;
		levels[nrooms] = level;
		faces[nrooms] = fc;
		isatrium[nrooms] = atrium[k];
		isouter[nrooms] = outer[k];
		nrooms++;
	}
	if (nrooms < 4) {
		return 0;
	}

	/* the start: a room on the ground, an outer one for choice, never the atrium */
	start = -1;
	for (i = 0; i < nrooms; i++) {
		if (levels[i] == 0 && !isatrium[i] && (start < 0 || (isouter[i] && !isouter[start]))) {
			start = i;
		}
	}
	if (start < 0) {
		return 0;
	}
	if (start != 0) {
		const LgRect tr = rooms[0];
		int t;

		rooms[0] = rooms[start]; rooms[start] = tr;
		t = types[0]; types[0] = types[start]; types[start] = t;
		t = levels[0]; levels[0] = levels[start]; levels[start] = t;
		t = faces[0]; faces[0] = faces[start]; faces[start] = t;
		t = isatrium[0]; isatrium[0] = isatrium[start]; isatrium[start] = t;
		t = isouter[0]; isouter[0] = isouter[start]; isouter[start] = t;
	}

	/* the areas: the rooms, the corridors, then the terraces of rooms with a
	 * single hall side (the strip must not bring a raised floor to a hall) */
	{
		LgBlock terr[LG_MAXROOMS][5];
		int nterr[LG_MAXROOMS], raise[LG_MAXROOMS];

		for (i = 0; i < nrooms; i++) {
			const LgRect whole = rooms[i];
			const int fc = faces[i];

			rooms[i].room = (int16_t)i;
			nterr[i] = 0;
			raise[i] = 0;
			if (i > 0 && (fc == 1 || fc == 2 || fc == 4 || fc == 8)) {
				const int face = fc == 1 ? 0 : fc == 2 ? 1 : fc == 4 ? 2 : 3;

				nterr[i] = lgRoomTerrace(r, &rooms[i], types[i], face ^ 1, levels[i] * LG_LEVEL, terr[i], &raise[i]);
				for (k = 0; k < nterr[i]; k++) {
					if ((terr[i][k].lo != levels[i] * LG_LEVEL || terr[i][k].hi != levels[i] * LG_LEVEL) && lgLabTouches(g, &terr[i][k].rc)) {
						break;
					}
				}
				if (k < nterr[i]) {
					rooms[i] = whole;
					rooms[i].room = (int16_t)i;
					nterr[i] = 0;
					raise[i] = 0;
				}
			}
			lgSetRoomArea(&lg->areas[i], &rooms[i], types[i], i, levels[i], g_LgRoomTypes[types[i]].ceil);
			if (raise[i] + 280 > g_LgRoomTypes[types[i]].ceil) {
				lg->areas[i].ceil = (int16_t)(lg->areas[i].floor + raise[i] + 280);
			}
		}
		lg->numrooms = (int16_t)nrooms;
		lg->numareas = lg->numrooms;
		for (k = 0; k < ncorr; k++) {
			LgArea *a = &lg->areas[lg->numareas++];

			memset(a, 0, sizeof(*a));
			a->x = corr[k].rc.x; a->z = corr[k].rc.z; a->w = corr[k].rc.w; a->d = corr[k].rc.d;
			a->kind = LG_AREA_CORRIDOR;
			a->theme = LG_CORRIDOR_THEME;
			a->room = -1;
			a->floor = corr[k].hi;
			a->floorlo = corr[k].lo;
			a->ramp = corr[k].ramp;
			a->ceil = (int16_t)(a->floor + LG_CORRIDOR_CEIL);
			a->dist = -1;
			a->centrepad = -1;
		}
		for (i = 0; i < nrooms; i++) {
			for (k = 0; k < nterr[i]; k++) {
				LgArea *a = &lg->areas[lg->numareas++];

				*a = lg->areas[i];
				a->x = terr[i][k].rc.x; a->z = terr[i][k].rc.z; a->w = terr[i][k].rc.w; a->d = terr[i][k].rc.d;
				a->flags = LG_AREAFLAG_TERRACE;
				a->ramp = terr[i][k].ramp;
				a->floor = (int16_t)(terr[i][k].lo > terr[i][k].hi ? terr[i][k].lo : terr[i][k].hi);
				a->floorlo = (int16_t)(terr[i][k].lo < terr[i][k].hi ? terr[i][k].lo : terr[i][k].hi);
			}
		}
	}

	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];

		for (z = a->z; z < a->z + a->d; z++) {
			for (x = a->x; x < a->x + a->w; x++) {
				lg->cells[z][x] = (int16_t)(i + 1);
			}
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* The street layout                                                    */

/* Two long narrow streets side by side (2026-11-19, the user's: "remove the
 * alleys and make it building street building street building"; first
 * 2026-11-14 as one street with alleys behind its two rows), outdoors: no
 * ceiling. Across them: a row of buildings, street 0, a middle row, street 1,
 * a row of buildings. The streets run in pieces that step sideways together
 * and climb a hill or two together (a ramp piece), and at each end a cross
 * street joins them round the middle row's end: a ring. Every building has a
 * front door (a short doorway passage through its wall) onto its street; the
 * middle row has a door onto each street, giving routes between the two
 * besides the ends. Buildings stand two cells apart (a party
 * wall). The streets are one room (the hub flag: low cover, no pillars, no
 * mezzanine). Buildings get mezzanines (their second storey), wings, cover. */
#define LG_ST_MAXSEG   8
#define LG_ST_OPENTOP  640   /* how high the walls stand round the open areas */
#define LG_ST_ENDW     4     /* a cross street's width */

static const struct {
	int8_t len;            /* the streets' length, cells */
	int8_t segs;           /* flat pieces */
	int8_t ramps;          /* hills */
	int8_t off;            /* from the middle line to each street's middle */
	int8_t outer;          /* from a street's middle to the outer rows' backs */
	uint8_t grid;
} g_LgStreet[LG_SIZE_COUNT] = {
	{ 40, 2, 1, 10, 14, 52 },
	{ 58, 3, 1, 11, 15, 72 },
	{ 90, 4, 2, 11, 16, 104 },
};

typedef struct {
	int gw, mirror, swap;
} LgStMap;

/* a rectangle given along (u) and across (v) the street, on the grid */
static LgRect lgStRect(const LgStMap *m, int u, int v, int lu, int lv)
{
	LgRect rc;

	if (m->mirror) {
		u = m->gw - u - lu;
	}
	rc.x = (int16_t)(m->swap ? v : u); rc.w = (int16_t)(m->swap ? lv : lu);
	rc.z = (int16_t)(m->swap ? u : v); rc.d = (int16_t)(m->swap ? lu : lv);
	rc.room = -1;
	return rc;
}

static int lgStRampDir(const LgStMap *m, int up)
{
	const int pos = up != m->mirror;   /* rises towards + on the grid */

	return m->swap ? (pos ? LG_RAMP_ZP : LG_RAMP_ZN) : (pos ? LG_RAMP_XP : LG_RAMP_XN);
}

static void lgStCorridor(LgLayout *lg, const LgRect *rc, int lo, int hi, int ramp, int open, int top)
{
	LgArea *a = &lg->areas[lg->numareas++];

	memset(a, 0, sizeof(*a));
	a->x = rc->x; a->z = rc->z; a->w = rc->w; a->d = rc->d;
	a->kind = LG_AREA_CORRIDOR;
	a->theme = (uint8_t)(open ? LG_STREET_THEME : LG_CORRIDOR_THEME);
	a->room = -1;
	a->floor = (int16_t)(lo > hi ? lo : hi);
	a->floorlo = (int16_t)(lo < hi ? lo : hi);
	a->ramp = (uint8_t)ramp;
	a->ceil = (int16_t)(open ? top : a->floor + LG_CORRIDOR_CEIL);
	a->open = (uint8_t)open;
	a->dist = -1;
	a->centrepad = -1;
}

#define LG_ST_MAXSTEPS (LG_MAXROOMS * 2)
#define LG_ST_MAXWINS  (LG_MAXROOMS * 6)

typedef struct {
	LgStMap m;
	LgRect steps[LG_ST_MAXSTEPS], wins[LG_ST_MAXWINS];
	int steplevel[LG_ST_MAXSTEPS], winlevel[LG_ST_MAXWINS];
	int nsteps, nwins;
} LgStFronts;

/* a building's front on line vf (its front wall's outer face; the porch and
 * windows two cells deep from vs, the street's edge or the line), cells
 * cur..cur+bw-1 along: a porch dw wide at du (or none, du < 0), and windows
 * two cells wide either side of it, a cell clear of the porch, of each other
 * and of the corners */
static void lgStFront2(LgStFronts *f, int cur, int bw, int du, int dw, int vs, int level)
{
	int wu;

	if (du >= 0 && f->nsteps < LG_ST_MAXSTEPS) {
		f->steps[f->nsteps] = lgStRect(&f->m, du, vs, dw, 2);
		f->steplevel[f->nsteps++] = level;
	}
	for (wu = cur + 1; wu < cur + bw - 1 && f->nwins < LG_ST_MAXWINS; ) {
		int run = 0;

		while (wu + run < cur + bw - 1 && (du < 0 || wu + run < du - 1 || wu + run > du + dw)) {
			run++;
		}
		if (run >= 1) {
			const int ww = run >= 2 ? 2 : 1;

			f->wins[f->nwins] = lgStRect(&f->m, wu, vs, ww, 2);
			f->winlevel[f->nwins++] = level;
			wu += ww + 1;
		} else {
			wu++;
		}
	}
}

static int lgPlaceStreet(LgLayout *lg, LgRng *r)
{
	/* the streets' pieces along u: flat ones and ramps; v and w each street's */
	struct { int u, len, v[2], w[2], level, ramp, up; } pc[LG_ST_MAXSEG * 2];
	static LgStFronts f;
	LgRect rooms[LG_MAXROOMS];
	int types[LG_MAXROOMS], levels[LG_MAXROOMS];
	const int size = lg->size, len = g_LgStreet[size].len, S = g_LgStreet[size].off, R = g_LgStreet[size].outer;
	const int vc = lg->gw / 2, u0 = (lg->gw - len) / 2;
	const int vlo = vc - S - R, vhi = vc + S + R;
	int npc = 0, nrooms = 0, i, k, x, z, row, top, maxlevel = 0;
	int nseg = g_LgStreet[size].segs, nramp = g_LgStreet[size].ramps, ramps[LG_ST_MAXSEG], seglen, u, c, level;

	memset(&f, 0, sizeof(f));
	f.m.gw = lg->gw;
	f.m.mirror = lgRand(r) & 1;
	f.m.swap = lgRand(r) & 1;
	if (vlo < 2 || vhi > lg->gw - 2 || u0 - LG_ST_ENDW < 2) {
		return 0;
	}

	/* which flat pieces a hill follows */
	memset(ramps, 0, sizeof(ramps));
	for (i = 0; i < nramp; i++) {
		int at = lgRange(r, 0, nseg - 2), tries = 0;

		while (ramps[at] && tries++ < 8) at = (at + 1) % (nseg - 1);
		ramps[at] = 1;
	}
	seglen = (len - nramp * LG_RAMPCELLS) / nseg;
	if (seglen < 12) {
		return 0;
	}

	u = u0;
	c = lgRange(r, -1, 1);
	level = 0;
	for (i = 0; i < nseg; i++) {
		const int w0 = lgRange(r, 5, 7), w1 = lgRange(r, 5, 7);
		const int l = i == nseg - 1 ? u0 + len - u : seglen + (i + 1 < nseg ? lgRange(r, -1, 1) : 0);
		int s;

		if (i > 0) {
			/* a step sideways, both streets together, each still three cells open to its last piece */
			const int was = c;

			c += (lgRand(r) & 1 ? 1 : -1) * lgRange(r, 2, 3);
			if (c > 3) c = was - lgRange(r, 2, 3);
			if (c < -3) c = was + lgRange(r, 2, 3);
			for (s = 0; s < 2; s++) {
				const int w = s ? w1 : w0, v = vc + (s ? S : -S) + c - w / 2;
				const int pv = pc[npc - 1].v[s], pw = pc[npc - 1].w[s];
				const int lo = v > pv ? v : pv, hi = v + w < pv + pw ? v + w : pv + pw;

				if (hi - lo < 3) {
					c = was;
				}
			}
		}
		pc[npc].u = u; pc[npc].len = l; pc[npc].level = level; pc[npc].ramp = 0; pc[npc].up = 0;
		pc[npc].w[0] = w0; pc[npc].w[1] = w1;
		pc[npc].v[0] = vc - S + c - w0 / 2;
		pc[npc].v[1] = vc + S + c - w1 / 2;
		npc++;
		u += l;
		if (i < nseg - 1 && ramps[i]) {
			const int up = level == 0 || (level < 2 && lgRange(r, 0, 99) < 60);

			pc[npc] = pc[npc - 1];
			pc[npc].u = u; pc[npc].len = LG_RAMPCELLS; pc[npc].ramp = 1; pc[npc].up = up;
			pc[npc].level = up ? level : level - 1;   /* its low end */
			level += up ? 1 : -1;
			if (level > maxlevel) maxlevel = level;
			npc++;
			u += LG_RAMPCELLS;
		}
	}
	top = maxlevel * LG_LEVEL + LG_ST_OPENTOP;

	/* the buildings, piece by piece: the middle row first, then the outer
	 * rows, which mostly copy its buildings so that porches face porches
	 * across each street (bridges) */
	for (i = 0; i < npc; i++) {
		int mcur[16], mbw[16], mdu[16], nm = 0;
		const int copy0 = lgRange(r, 0, 99) < 90, copy2 = lgRange(r, 0, 99) < 90;

		if (pc[i].ramp) {
			continue;
		}
		for (row = 0; row < 3; row++) {
			const int order = row == 0 ? 1 : row == 1 ? 0 : 2;   /* middle, then the outer two */
			const int s0e = pc[i].v[0] + pc[i].w[0], s1v = pc[i].v[1];
			/* the building's span across: from bv0 to bv1 */
			const int flush = lgRange(r, 0, 99) < 65;
			const int bv0 = order == 0 ? vlo + (flush ? 0 : 1) : order == 1 ? s0e + 2 : pc[i].v[1] + pc[i].w[1] + 2;
			const int bv1 = order == 0 ? pc[i].v[0] - 2 : order == 1 ? s1v - 2 : vhi - (flush ? 0 : 1);
			const int copy = order == 0 ? copy0 : order == 2 ? copy2 : 0;
			int cur = pc[i].u + 2, end = pc[i].u + pc[i].len - 2, n = 0;

			if (bv1 - bv0 < 5) {
				continue;
			}
			while (end - cur >= 6 && nrooms < LG_MAXROOMS - 2) {
				const int left = end - cur;
				int bw = left >= 16 ? lgRange(r, 7, left - 9 < 11 ? left - 9 : 11) : (left > 12 ? 12 : left);
				const int pick = lgRange(r, 0, 9);
				int dw, du, k2;

				/* an outer row's building copies the middle row's opposite it */
				if (order != 1 && copy && n < nm && mcur[n] == cur && mbw[n] <= left) {
					bw = mbw[n];
				}
				dw = bw >= 10 ? 4 : 3;
				/* the porch towards one end of the front, so the rest of it has room for windows */
				du = (lgRand(r) & 1) ? cur + 1 : cur + bw - dw - 1;
				/* across the street from a middle porch: line this one up with it,
				 * so that their balconies face each other (a bridge) */
				for (k2 = 0; order != 1 && k2 < nm; k2++) {
					if (mdu[k2] >= cur + 1 && mdu[k2] + dw <= cur + bw - 1) {
						du = mdu[k2];
						break;
					}
				}
				rooms[nrooms] = lgStRect(&f.m, cur, bv0, bw, bv1 - bv0);
				/* (the middle row's tall enough for an upper floor: its balconies
				 * are one end of every bridge) */
				types[nrooms] = order == 1 ? (pick < 5 ? LG_ROOM_STORAGE : pick < 8 ? LG_ROOM_LAB : LG_ROOM_HALL)
					: pick < 4 ? LG_ROOM_OFFICE : pick < 7 ? LG_ROOM_STORAGE : pick < 9 ? LG_ROOM_LAB : LG_ROOM_HALL;
				levels[nrooms] = pc[i].level;
				if (order == 0) {
					lgStFront2(&f, cur, bw, du, dw, bv1, pc[i].level);
				} else if (order == 2) {
					lgStFront2(&f, cur, bw, du, dw, pc[i].v[1] + pc[i].w[1], pc[i].level);
				} else {
					/* Keep the seeded building placements after the old door rolls. */
					(void)lgRange(r, 0, 99);
					(void)lgRand(r);
					lgStFront2(&f, cur, bw, du, dw, s0e, pc[i].level);
					lgStFront2(&f, cur, bw, du, dw, bv1, pc[i].level);
					if (nm < 16) {
						mcur[nm] = cur;
						mbw[nm] = bw;
						mdu[nm] = du;
						nm++;
					}
				}
				nrooms++;
				n++;
				cur += bw + 2;   /* a party wall */
			}
		}
	}
	if (nrooms < 3) {
		return 0;
	}

	/* the areas: buildings, the streets (one room, last, like the hub), then
	 * the cross streets, the porches and the windows */
	for (i = 0; i < nrooms; i++) {
		LgArea *a = &lg->areas[i];

		rooms[i].room = (int16_t)i;
		lgSetRoomArea(a, &rooms[i], types[i], i, levels[i], g_LgRoomTypes[types[i]].ceil);
	}
	lg->numrooms = (int16_t)(nrooms + 1);
	lg->numareas = lg->numrooms;
	for (k = 0; k < 2; k++) {
		for (i = 0; i < npc; i++) {
			LgArea *a = k == 0 && i == 0 ? &lg->areas[nrooms] : &lg->areas[lg->numareas++];
			LgRect rc = lgStRect(&f.m, pc[i].u, pc[i].v[k], pc[i].len, pc[i].w[k]);
			const int lo = pc[i].level * LG_LEVEL, hi = (pc[i].level + (pc[i].ramp ? 1 : 0)) * LG_LEVEL;

			lgSetHubArea(a, &rc, nrooms, lo, hi, pc[i].ramp ? lgStRampDir(&f.m, pc[i].up) : 0);
			a->open = 1;
			a->theme = LG_STREET_THEME;
			a->ceil = (int16_t)top;
		}
	}
	/* the cross streets at the two ends, from street 0's outer edge to street 1's */
	for (k = 0; k < 2; k++) {
		const int pi = k ? npc - 1 : 0;
		const int v0 = pc[pi].v[0], v1 = pc[pi].v[1] + pc[pi].w[1];
		LgArea *a = &lg->areas[lg->numareas++];
		LgRect rc = lgStRect(&f.m, k ? u0 + len : u0 - LG_ST_ENDW, v0, LG_ST_ENDW, v1 - v0);
		const int lv = pc[pi].level * LG_LEVEL;

		lgSetHubArea(a, &rc, nrooms, lv, lv, 0);
		a->open = 1;
		a->theme = LG_STREET_THEME;
		a->ceil = (int16_t)top;
	}
	for (i = 0; i < f.nsteps; i++) {
		const int lv = f.steplevel[i] * LG_LEVEL;

		lgStCorridor(lg, &f.steps[i], lv, lv, 0, 0, top);
		lg->areas[lg->numareas - 1].ceil = (int16_t)(lv + LG_MEZZ_UNDER);   /* low: a balcony's floor is its roof */
	}
	for (i = 0; i < f.nwins && lg->numareas < LG_MAXAREAS - 40; i++) {
		LgArea *a;

		lgStCorridor(lg, &f.wins[i], f.winlevel[i] * LG_LEVEL + LG_WIN_SILL, f.winlevel[i] * LG_LEVEL + LG_WIN_SILL, 0, 0, top);
		a = &lg->areas[lg->numareas - 1];
		a->ceil = (int16_t)(f.winlevel[i] * LG_LEVEL + LG_WIN_HEAD);
		a->special = LG_SPECIAL_WINDOW;
	}

	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];

		if (a->x < 1 || a->z < 1 || a->x + a->w > lg->gw - 1 || a->z + a->d > lg->gd - 1) {
			return 0;
		}
		for (z = a->z; z < a->z + a->d; z++) {
			for (x = a->x; x < a->x + a->w; x++) {
				if (lg->cells[z][x]) {
					return 0;   /* two things on one cell: the plan was wrong */
				}
				lg->cells[z][x] = (int16_t)(i + 1);
			}
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Step 2: passages between rooms                                       */

/* A passage is one straight piece, or two legs and the square at the turn.
 * Each piece is a corridor area; a piece that climbs is a ramp. */
typedef struct {
	int16_t x, z, w, d;
	int8_t travel;          /* the way it runs from room a to room b: 0 -x, 1 +x, 2 -z, 3 +z */
} LgSeg;

static const int8_t g_LgDx[4] = { -1, 1, 0, 0 }, g_LgDz[4] = { 0, 0, -1, 1 };

/* The eight ways of laying the grid over itself: bit 0 swaps x and z, bit 1
 * mirrors x, bit 2 mirrors z. Passages are worked out in a frame where they
 * leave room a along +x (and turn along +z), then put back. */
static LgRect lgXform(LgRect rc, int t, int g, int inverse)
{
	int16_t v;

	if (!inverse && (t & 1)) { v = rc.x; rc.x = rc.z; rc.z = v; v = rc.w; rc.w = rc.d; rc.d = v; }
	if (t & 2) rc.x = (int16_t)(g - rc.x - rc.w);
	if (t & 4) rc.z = (int16_t)(g - rc.z - rc.d);
	if (inverse && (t & 1)) { v = rc.x; rc.x = rc.z; rc.z = v; v = rc.w; rc.w = rc.d; rc.d = v; }
	return rc;
}

/* a direction in the working frame, back in the grid's */
static int lgXformDir(int dir, int t)
{
	int dx = g_LgDx[dir], dz = g_LgDz[dir], v, i;

	if (t & 2) dx = -dx;
	if (t & 4) dz = -dz;
	if (t & 1) { v = dx; dx = dz; dz = v; }
	for (i = 0; i < 4; i++) {
		if (g_LgDx[i] == dx && g_LgDz[i] == dz) return i;
	}
	return 0;
}

static int lgInSegs(const LgSeg *s, int ns, int x, int z)
{
	int i;

	for (i = 0; i < ns; i++) {
		if (x >= s[i].x && x < s[i].x + s[i].w && z >= s[i].z && z < s[i].z + s[i].d) {
			return i;
		}
	}
	return -1;
}

/* the passage's cells are all rock, and it touches nothing but itself, room
 * a through its first piece's back end and room b through its last's front */
static int lgSegsFree(const LgLayout *lg, const LgSeg *s, int ns, int ra, int rb)
{
	int k, x, z, dir;

	for (k = 0; k < ns; k++) {
		if (s[k].w < 1 || s[k].d < 1 || s[k].x < 1 || s[k].z < 1 || s[k].x + s[k].w > lg->gw - 1 || s[k].z + s[k].d > lg->gd - 1) {
			return 0;
		}
		for (z = s[k].z; z < s[k].z + s[k].d; z++) {
			for (x = s[k].x; x < s[k].x + s[k].w; x++) {
				if (lg->cells[z][x] != 0) {
					return 0;
				}
				for (dir = 0; dir < 4; dir++) {
					const int nx = x + g_LgDx[dir], nz = z + g_LgDz[dir];
					int other;

					if (lgInSegs(s, ns, nx, nz) >= 0 || lg->cells[nz][nx] == 0) {
						continue;
					}
					other = lg->cells[nz][nx] - 1;
					if (other == ra && k == 0 && dir == (s[0].travel ^ 1)) {
						continue;
					}
					if (other == rb && k == ns - 1 && dir == s[ns - 1].travel) {
						continue;
					}
					return 0;
				}
			}
		}
	}
	return 1;
}

/* Joins rooms ra and rb (their main areas) with a passage: the shortest of
 * every straight or one-turn passage that fits (ties broken by the seed). The
 * climb between their floors goes on one straight piece long enough for it.
 * Returns 1 if a passage was laid. */
typedef struct {
	LgSeg seg[3];
	int ns, ramp, score;
} LgPassageCand;

static void lgTryPassage(const LgLayout *lg, LgRng *r, int ra, int rb, int t, const LgRect *s, int ns,
		const int *lens, int need, LgPassageCand *best)
{
	LgPassageCand c;
	int k, total = 0;

	c.ramp = -1;
	if (need > 0) {
		c.ramp = ns == 1 ? 0 : (lens[0] >= lens[2] ? 0 : 2);
		if (lens[c.ramp] < need) {
			return;
		}
	}
	for (k = 0; k < ns; k++) total += s[k].w * s[k].d;
	c.score = total * 4 + (int)(lgRand(r) & 3);
	if (best->ns && c.score >= best->score) {
		return;
	}
	for (k = 0; k < ns; k++) {
		const LgRect back = lgXform(s[k], t, lg->gw, 1);

		c.seg[k].x = back.x; c.seg[k].z = back.z; c.seg[k].w = back.w; c.seg[k].d = back.d;
		c.seg[k].travel = (int8_t)lgXformDir(ns == 3 && k >= 1 ? 3 : 1, t);
	}
	c.ns = ns;
	if (lgSegsFree(lg, c.seg, ns, ra, rb)) {
		*best = c;
	}
}

static int lgPassage(LgLayout *lg, LgRng *r, int ra, int rb, int maxcells, char *err, int errlen)
{
	const LgArea *A = &lg->areas[ra], *B = &lg->areas[rb];
	const int g = lg->gw;
	const int ha = A->floor, hb = B->floor;
	const int need = (ha > hb ? ha - hb : hb - ha) / LG_LEVEL * LG_RAMPCELLS;
	LgPassageCand best;
	int t, wd, k;

	best.ns = 0;
	for (t = 0; t < 8; t++) {
		LgRect a, b, s[3];
		int lens[3];

		a.x = A->x; a.z = A->z; a.w = A->w; a.d = A->d; a.room = 0;
		b.x = B->x; b.z = B->z; b.w = B->w; b.d = B->d; b.room = 0;
		a = lgXform(a, t, g, 0);
		b = lgXform(b, t, g, 0);
		if (b.x < a.x + a.w + 1) {
			continue;   /* b must lie beyond a along +x */
		}
		for (wd = 2; wd <= 3; wd++) {
			const int lo = (a.z > b.z ? a.z : b.z) + 1;
			const int hi = (a.z + a.d < b.z + b.d ? a.z + a.d : b.z + b.d) - 1;
			const int zlo = a.z + 1, zhi = a.z + a.d - 1 - wd;
			const int xlo = (b.x + 1 > a.x + a.w + 1 ? b.x + 1 : a.x + a.w + 1), xhi = b.x + b.w - 1 - wd;
			int zs, xc;

			/* straight across */
			for (zs = lo; zs <= hi - wd; zs++) {
				s[0].x = (int16_t)(a.x + a.w); s[0].z = (int16_t)zs; s[0].w = (int16_t)(b.x - (a.x + a.w)); s[0].d = (int16_t)wd;
				lens[0] = s[0].w;
				lgTryPassage(lg, r, ra, rb, t, s, 1, lens, need, &best);
			}
			/* along +x from a, turn, along +z into b */
			for (zs = zlo; zs <= zhi; zs++) {
				if (b.z < zs + wd + 1) {
					continue;
				}
				for (xc = xlo; xc <= xhi; xc++) {
					s[0].x = (int16_t)(a.x + a.w); s[0].z = (int16_t)zs; s[0].w = (int16_t)(xc - (a.x + a.w)); s[0].d = (int16_t)wd;
					s[1].x = (int16_t)xc; s[1].z = (int16_t)zs; s[1].w = (int16_t)wd; s[1].d = (int16_t)wd;
					s[2].x = (int16_t)xc; s[2].z = (int16_t)(zs + wd); s[2].w = (int16_t)wd; s[2].d = (int16_t)(b.z - (zs + wd));
					lens[0] = s[0].w;
					lens[1] = 0;
					lens[2] = s[2].d;
					lgTryPassage(lg, r, ra, rb, t, s, 3, lens, need, &best);
				}
			}
		}
	}
	if (!best.ns || (maxcells > 0 && best.score / 4 > maxcells)) {
		return 0;
	}
	if (lg->numareas + best.ns > LG_MAXAREAS) {
		lgSetErr(err, errlen, "too many areas (%d)", lg->numareas, 0);
		return 0;
	}

	/* lay it: pieces before the climb at a's floor, after it at b's */
	for (k = 0; k < best.ns; k++) {
		const LgSeg *sg = &best.seg[k];
		LgArea *ar = &lg->areas[lg->numareas];
		const int h = best.ramp < 0 ? ha : (k < best.ramp ? ha : hb);
		int x, z;

		memset(ar, 0, sizeof(*ar));
		ar->x = sg->x; ar->z = sg->z; ar->w = sg->w; ar->d = sg->d;
		ar->kind = LG_AREA_CORRIDOR;
		ar->theme = LG_CORRIDOR_THEME;
		ar->room = -1;
		ar->floor = ar->floorlo = (int16_t)h;
		if (k == best.ramp && ha != hb) {
			const int up = hb > ha ? sg->travel : (sg->travel ^ 1);

			ar->ramp = (uint8_t)(up == 0 ? LG_RAMP_XN : up == 1 ? LG_RAMP_XP : up == 2 ? LG_RAMP_ZN : LG_RAMP_ZP);
			ar->floor = (int16_t)(ha > hb ? ha : hb);
			ar->floorlo = (int16_t)(ha < hb ? ha : hb);
		}
		ar->ceil = (int16_t)(ar->floor + LG_CORRIDOR_CEIL);
		ar->dist = -1;
		ar->centrepad = -1;
		for (z = ar->z; z < ar->z + ar->d; z++) {
			for (x = ar->x; x < ar->x + ar->w; x++) {
				lg->cells[z][x] = (int16_t)(lg->numareas + 1);
			}
		}
		lg->numareas++;
	}
	return 1;
}

/* Step 3: wings, once the passages are down, wherever they fit round them.
 * They come after the passages in the areas. */
static int lgAddWings(LgLayout *lg, LgRng *r, char *err, int errlen)
{
	int i, k, x, z;

	for (i = 0; i < lg->numrooms; i++) {
		LgRect m, got[2];
		int n;

		if (lg->areas[i].flags & LG_AREAFLAG_HUB) {
			continue;   /* the hub keeps its own shape */
		}
		m.x = lg->areas[i].x; m.z = lg->areas[i].z; m.w = lg->areas[i].w; m.d = lg->areas[i].d;
		m.room = (int16_t)i;
		n = lgPickWings(lg, r, &m, lg->areas[i].roomtype, got);
		for (k = 0; k < n; k++) {
			LgArea *a;

			if (lg->numareas >= LG_MAXAREAS) {
				lgSetErr(err, errlen, "too many areas (%d)", lg->numareas, 0);
				return 0;
			}
			a = &lg->areas[lg->numareas];
			*a = lg->areas[i];
			a->x = got[k].x; a->z = got[k].z; a->w = got[k].w; a->d = got[k].d;
			a->flags = 0;
			for (z = a->z; z < a->z + a->d; z++) {
				for (x = a->x; x < a->x + a->w; x++) {
					lg->cells[z][x] = (int16_t)(lg->numareas + 1);
				}
			}
			lg->numareas++;
		}
	}
	return 1;
}

/* Step 2: each room's passage to the hub area it attaches to (all of them, or
 * the layout is tried again), then passages between rooms next to each other
 * round the hub where a short one fits: loops through the side rooms */
static int lgConnect(LgLayout *lg, LgRng *r, const int16_t *attach, char *err, int errlen)
{
	const int n = lg->numrooms - 1, hub = n;
	const LgArea *H = &lg->areas[hub];
	const int32_t hcx = H->x * 2 + H->w, hcz = H->z * 2 + H->d;   /* the hub floor's centre, half cells */
	int16_t order[LG_MAXROOMS];
	int32_t key[LG_MAXROOMS];
	int i, j, made = 0;

	for (i = 0; i < n; i++) {
		if (!lgPassage(lg, r, i, attach[i], 0, err, errlen)) {
			lgSetErr(err, errlen, "no passage from room %d to the hub", i, 0);
			return 0;
		}
	}

	/* the rooms in order round the hub: by quadrant, then by slope */
	for (i = 0; i < n; i++) {
		const LgArea *a = &lg->areas[i];
		const int32_t dx = a->x * 2 + a->w - hcx, dz = a->z * 2 + a->d - hcz;
		const int q = dx >= 0 ? (dz >= 0 ? 0 : 3) : (dz >= 0 ? 1 : 2);
		const int32_t ax = dx < 0 ? -dx : dx, az = dz < 0 ? -dz : dz;
		int32_t frac = (q == 0 || q == 2) ? (az * 1000) / (ax + az + 1) : (ax * 1000) / (ax + az + 1);

		key[i] = q * 1001 + frac;
		order[i] = (int16_t)i;
	}
	for (i = 1; i < n; i++) {
		const int16_t t = order[i];

		for (j = i - 1; j >= 0 && key[order[j]] > key[t]; j--) order[j + 1] = order[j];
		order[j + 1] = t;
	}
	for (i = 0; i < n && made < g_LgSizes[lg->size].links; i++) {
		made += lgPassage(lg, r, order[i], order[(i + 1) % n], 48, err, errlen);
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Step 4: portals (openings between areas)                             */

/* alongz: the boundary is the line x = line, running along z from start.
 * Otherwise the line z = line, running along x. a is on the low side. */
/* the layer lgFindPortals works on (2026-11-16: the storeys over the ground
 * too, for the rooftops' towers): its cells and open edges */
static int g_LgPortalLayer = 0;

static int lgStationRampTop(const LgArea *a, int alongz, int line)
{
	return alongz
		? (a->ramp == LG_RAMP_XP && line == a->x + a->w) || (a->ramp == LG_RAMP_XN && line == a->x)
		: (a->ramp == LG_RAMP_ZP && line == a->z + a->d) || (a->ramp == LG_RAMP_ZN && line == a->z);
}

static int lgStationRoofEdge(const LgLayout *lg, int ai, int bi)
{
	const LgArea *a = &lg->areas[ai], *b = &lg->areas[bi];
	const LgArea *roof = a->layer == 2 ? a : b;
	const LgArea *platform = roof == a ? b : a;

	return lg->theme.layout == LG_LAYOUT_STATION && roof->layer == 2
		&& roof->theme == LG_STN_CAR && roof->floor == LG_STN_CONCOURSE_Y
		&& platform->layer == 1 && platform->theme == LG_STN_PLATFORM
		&& (platform->flags & LG_AREAFLAG_BAY) && platform->floor == LG_STN_PLATFORM_Y;
}

static int lgAddPortalRun(LgLayout *lg, LgRng *r, int alongz, int line, int start, int len,
		int a, int b, char *err, int errlen)
{
	int aroom = lg->areas[a].kind == LG_AREA_ROOM;
	int broom = lg->areas[b].kind == LG_AREA_ROOM;
	int off = 0, olen = len, i;
	LgPortal *p;

	if (lg->theme.layout == LG_LAYOUT_STATION) {
		const LgArea *aa = &lg->areas[a], *bb = &lg->areas[b];
		if ((lgStationRampTop(aa, alongz, line) && aa->floor != bb->floor)
				|| (lgStationRampTop(bb, alongz, line) && bb->floor != aa->floor)) return 1;
	}
	if (aroom && broom && lg->areas[a].room != lg->areas[b].room) {
		return 1; /* rooms never open straight into each other */
	}

	/* a mezzanine's undercroft, ramp and bay open only on the room's main: a
	 * terrace or wing of the same room alongside them is walled off */
	if (aroom && broom) {
		const uint8_t set = LG_AREAFLAG_UNDER | LG_AREAFLAG_BAY;
		const int ina = (lg->areas[a].flags & set) != 0, inb = (lg->areas[b].flags & set) != 0;

		if (ina != inb && (ina ? b : a) != lg->areas[a].room) {
			return 1;
		}
	}

	if (aroom && broom) {
		/* a room's main and its wing: the whole shared edge is open, one space */
		aroom = broom = 0;
	}

	if (aroom || broom) {
		if (len > 3) {
			off = (len - 3) / 2;
			olen = 3;
		}
	}

	if (lg->numportals >= LG_MAXPORTALS) {
		lgSetErr(err, errlen, "too many portals (%d)", lg->numportals, 0);
		return 0;
	}

	for (i = start + off; i < start + off + olen; i++) {
		if (alongz) {
			LG_OPENS(lg, g_LgPortalLayer)[i][line - 1] |= LG_OPEN_XP;
		} else {
			LG_OPENS(lg, g_LgPortalLayer)[line - 1][i] |= LG_OPEN_ZP;
		}
	}

	p = &lg->portals[lg->numportals++];
	if (alongz) {
		p->x0 = p->x1 = (int16_t)line;
		p->z0 = (int16_t)(start + off);
		p->z1 = (int16_t)(start + off + olen);
	} else {
		p->z0 = p->z1 = (int16_t)line;
		p->x0 = (int16_t)(start + off);
		p->x1 = (int16_t)(start + off + olen);
	}
	p->a = (int16_t)a;
	p->b = (int16_t)b;
	p->doorway = (uint8_t)(aroom || broom);
	p->door = (uint8_t)(p->doorway && olen <= 3 && lgRange(r, 0, 99) < LG_DOOR_PCT);
	/* a ledge: the floors differ somewhere along it (a terrace over the hub floor) */
	{
		const int32_t x0 = lgWorldX(lg, p->x0), x1 = lgWorldX(lg, p->x1), z0 = lgWorldZ(lg, p->z0), z1 = lgWorldZ(lg, p->z1);

		p->drop = (uint8_t)(modLevelGenFloorAt(lg, a, x0, z0) != modLevelGenFloorAt(lg, b, x0, z0)
				|| modLevelGenFloorAt(lg, a, x1, z1) != modLevelGenFloorAt(lg, b, x1, z1)
				|| modLevelGenFloorAt(lg, a, (x0 + x1) / 2, (z0 + z1) / 2) != modLevelGenFloorAt(lg, b, (x0 + x1) / 2, (z0 + z1) / 2));
		if (p->drop) {
			p->door = 0;
		}
	}
	/* a window: its sill is a ledge on both sides, with no railing and no door */
	if (lg->areas[a].special == LG_SPECIAL_WINDOW || lg->areas[b].special == LG_SPECIAL_WINDOW) {
		p->window = 1;
		p->drop = 1;
		p->norail = 1;
		p->door = 0;
	}
	p->pada = p->padb = -1;

	return 1;
}

static int lgFindPortals(LgLayout *lg, LgRng *r, char *err, int errlen)
{
	int16_t (*cells)[LG_GRIDMAX] = LG_CELLS(lg, g_LgPortalLayer);
	int x, z;

	/* boundaries between columns x and x + 1 */
	for (x = 0; x + 1 < lg->gw; x++) {
		z = 0;
		while (z < lg->gd) {
			int a = cells[z][x], b = cells[z][x + 1], start;

			if (a == 0 || b == 0 || a == b) {
				z++;
				continue;
			}

			start = z;
			while (z < lg->gd && cells[z][x] == a && cells[z][x + 1] == b) {
				z++;
			}

			if (!lgAddPortalRun(lg, r, 1, x + 1, start, z - start, a - 1, b - 1, err, errlen)) {
				return 0;
			}
		}
	}

	/* boundaries between rows z and z + 1 */
	for (z = 0; z + 1 < lg->gd; z++) {
		x = 0;
		while (x < lg->gw) {
			int a = cells[z][x], b = cells[z + 1][x], start;

			if (a == 0 || b == 0 || a == b) {
				x++;
				continue;
			}

			start = x;
			while (x < lg->gw && cells[z][x] == a && cells[z + 1][x] == b) {
				x++;
			}

			if (!lgAddPortalRun(lg, r, 0, z + 1, start, x - start, a - 1, b - 1, err, errlen)) {
				return 0;
			}
		}
	}

	return 1;
}

/* ------------------------------------------------------------------ */
/* Step 4b: split levels                                                */

/* Some deep wings of halls and labs become a raised gallery: the wing is cut
 * in two along its depth, the part against the main room a ramp rising away
 * from it, the far part a platform at the ramp's top. Both stay the room's
 * areas with whole open edges (main | ramp | platform), so the floor runs on
 * without a step anywhere and nothing new needs walls or risers. A wing only
 * qualifies if it opens on nothing but its own main: a corridor can't climb. */
#define LG_RAISE_PCT 50

static const struct {
	int16_t height;      /* platform height */
	int8_t ramp;         /* ramp length, cells (rise 1 in 3 or gentler) */
} g_LgRaise[LG_ROOM_TYPECOUNT] = {
	{ 0, 0 },            /* office: ceilings too low */
	{ 120, 4 },          /* hall: ceiling 500 */
	{ 0, 0 },            /* storage */
	{ 100, 3 },          /* lab: ceiling 400 */
};

static int lgRaiseWings(LgLayout *lg, LgRng *r, char *err, int errlen)
{
	const int numareas = lg->numareas;
	int wi, i, x, z;

	for (wi = lg->numrooms; wi < numareas; wi++) {
		LgArea *wing = &lg->areas[wi];
		const LgArea *m;
		LgArea plat;
		LgPortal *p;
		int portal = -1, others = 0, side, depth, rl;

		if (wing->kind != LG_AREA_ROOM || (wing->flags & (LG_AREAFLAG_HUB | LG_AREAFLAG_TERRACE)) || wing->ramp || wing->floor != lg->areas[wing->room].floor) {
			continue;
		}
		m = &lg->areas[wing->room];
		rl = g_LgRaise[m->roomtype].ramp;
		if (rl == 0) {
			continue;
		}
		for (i = 0; i < lg->numportals; i++) {
			const LgPortal *q = &lg->portals[i];

			if (q->a == wi || q->b == wi) {
				if ((q->a == wing->room || q->b == wing->room) && portal < 0) {
					portal = i;
				} else {
					others++;
				}
			}
		}
		if (portal < 0 || others) {
			continue;
		}
		/* which side of the main the wing is on, and how deep it is from it */
		p = &lg->portals[portal];
		if (p->x0 == p->x1) {
			side = wing->x == p->x0 ? LG_RAMP_XP : LG_RAMP_XN;
			depth = wing->w;
		} else {
			side = wing->z == p->z0 ? LG_RAMP_ZP : LG_RAMP_ZN;
			depth = wing->d;
		}
		if (depth < rl + 2 || lgRange(r, 0, 99) >= LG_RAISE_PCT) {
			continue;
		}
		if (lg->numareas >= LG_MAXAREAS || lg->numportals >= LG_MAXPORTALS) {
			lgSetErr(err, errlen, "no room to raise wing %d", wi, 0);
			return 0;
		}

		/* the platform: the far part; the wing keeps the near part as its ramp */
		plat = *wing;
		plat.floor = plat.floorlo = (int16_t)(m->floor + g_LgRaise[m->roomtype].height);
		plat.ramp = LG_RAMP_NONE;
		switch (side) {
		case LG_RAMP_XP: plat.x = (int16_t)(wing->x + rl); plat.w = (int16_t)(wing->w - rl); wing->w = (int16_t)rl; break;
		case LG_RAMP_XN: plat.w = (int16_t)(wing->w - rl); wing->x = (int16_t)(wing->x + plat.w); wing->w = (int16_t)rl; break;
		case LG_RAMP_ZP: plat.z = (int16_t)(wing->z + rl); plat.d = (int16_t)(wing->d - rl); wing->d = (int16_t)rl; break;
		default: plat.d = (int16_t)(wing->d - rl); wing->z = (int16_t)(wing->z + plat.d); wing->d = (int16_t)rl; break;
		}
		wing->floor = plat.floor;
		wing->floorlo = m->floor;
		wing->ramp = (uint8_t)side;
		lg->areas[lg->numareas] = plat;
		for (z = plat.z; z < plat.z + plat.d; z++) {
			for (x = plat.x; x < plat.x + plat.w; x++) {
				lg->cells[z][x] = (int16_t)(lg->numareas + 1);
			}
		}

		/* the whole edge between ramp and platform is open; the low side is a */
		p = &lg->portals[lg->numportals];
		memset(p, 0, sizeof(*p));
		p->pada = p->padb = -1;
		if (side == LG_RAMP_XP || side == LG_RAMP_XN) {
			const int line = side == LG_RAMP_XP ? wing->x + wing->w : wing->x;

			p->x0 = p->x1 = (int16_t)line;
			p->z0 = wing->z;
			p->z1 = (int16_t)(wing->z + wing->d);
			p->a = (int16_t)(side == LG_RAMP_XP ? wi : lg->numareas);
			p->b = (int16_t)(side == LG_RAMP_XP ? lg->numareas : wi);
			for (z = p->z0; z < p->z1; z++) lg->open[z][line - 1] |= LG_OPEN_XP;
		} else {
			const int line = side == LG_RAMP_ZP ? wing->z + wing->d : wing->z;

			p->z0 = p->z1 = (int16_t)line;
			p->x0 = wing->x;
			p->x1 = (int16_t)(wing->x + wing->w);
			p->a = (int16_t)(side == LG_RAMP_ZP ? wi : lg->numareas);
			p->b = (int16_t)(side == LG_RAMP_ZP ? lg->numareas : wi);
			for (x = p->x0; x < p->x1; x++) lg->open[line - 1][x] |= LG_OPEN_ZP;
		}
		lg->numportals++;
		lg->numareas++;
	}
	return 1;
}


/* ------------------------------------------------------------------ */
/* Step 4a: mezzanines                                                  */

/* A hall or lab can get a mezzanine over one end: an upper floor (layer 1)
 * LG_MEZZ_UNDER + LG_MEZZ_SLAB up, over an undercroft whose ceiling is the
 * slab. The room's end is cut into areas that stay the room's own:
 *   U  the undercroft, LG_MEZZ_UD deep across the whole room, low ceiling
 *   R  a ramp LG_MEZZ_RL long and 3 wide against one side wall, rising
 *      towards U's end up to the mezzanine's floor
 *   B  the bay beside the ramp, in front of the mezzanine
 *   M  the rest of the room (the main keeps its number)
 * and Z, the mezzanine itself, a layer 1 area over U's cells, which you step
 * onto from the ramp's top and look down from over a railing (and one gap
 * left open, a drop-down) at the bay. */
#define LG_MEZZ_UD  4
#define LG_MEZZ_RL  6
#define LG_MEZZ_RLMIN 5   /* the shortest ramp up: 1 in 2 */
#define LG_MEZZ_PCT 100

/* the cell at distance u from a room's end wall, v across (either may lie outside) */
static void lgMezzCell(const LgArea *m, int alongx, int lowend, int u, int v, int *x, int *z)
{
	if (alongx) {
		*x = lowend ? m->x + u : m->x + m->w - 1 - u;
		*z = m->z + v;
	} else {
		*z = lowend ? m->z + u : m->z + m->d - 1 - u;
		*x = m->x + v;
	}
}

static void lgMezzRect(const LgArea *m, int alongx, int lowend, int u0, int u1, int v0, int v1, LgArea *out)
{
	if (alongx) {
		out->x = (int16_t)(lowend ? m->x + u0 : m->x + m->w - u1);
		out->w = (int16_t)(u1 - u0);
		out->z = (int16_t)(m->z + v0);
		out->d = (int16_t)(v1 - v0);
	} else {
		out->z = (int16_t)(lowend ? m->z + u0 : m->z + m->d - u1);
		out->d = (int16_t)(u1 - u0);
		out->x = (int16_t)(m->x + v0);
		out->w = (int16_t)(v1 - v0);
	}
}

/* Which end of a street building is its front (the side its porch is on), as
 * the mezzanine loop numbers ends: alongx | lowend << 1; -1 if none */
static int lgStFront(const LgLayout *lg, const LgArea *m)
{
	int side, i;

	for (side = 0; side < 4; side++) {
		const int len = side < 2 ? m->d : m->w;

		for (i = 0; i < len; i++) {
			const int x = side == 0 ? m->x - 1 : side == 1 ? m->x + m->w : m->x + i;
			const int z = side == 2 ? m->z - 1 : side == 3 ? m->z + m->d : m->z + i;
			const int ai = (x < 0 || z < 0 || x >= lg->gw || z >= lg->gd) ? -1 : lg->cells[z][x] - 1;

			if (ai >= 0 && lg->areas[ai].kind == LG_AREA_CORRIDOR && !lg->areas[ai].open && !lg->areas[ai].special) {
				return side == 0 ? 3 : side == 1 ? 1 : side == 2 ? 2 : 0;
			}
		}
	}
	return -1;
}

static void lgMezzFill(LgLayout *lg, const LgArea *a, int idx)
{
	int x, z;

	for (z = a->z; z < a->z + a->d; z++) {
		for (x = a->x; x < a->x + a->w; x++) {
			LG_CELLS(lg, a->layer)[z][x] = (int16_t)(idx + 1);
		}
	}
}

/* a cell beside the mezzanine's footprint is acceptable: solid, a corridor
 * where the floor is flat (allowcorr), or an area
 * of room ri that has a cell next to the main's remaining part (u >= from) */
static int lgMezzRingOk(const LgLayout *lg, const LgArea *m0, int alongx, int lowend, int from, int L, int W, int ri,
		int x, int z, int allowcorr)
{
	int ai, cx, cz, u, v;
	const LgArea *t;

	if (x < 0 || z < 0 || x >= lg->gw || z >= lg->gd || lg->cells[z][x] == 0) {
		return 1;
	}
	ai = lg->cells[z][x] - 1;
	t = &lg->areas[ai];
	if (t->kind == LG_AREA_CORRIDOR) {
		return allowcorr;   /* a doorway on a flat floor is fine; not beside the ramp */
	}
	if (t->kind != LG_AREA_ROOM || t->room != ri || (t->flags & LG_AREAFLAG_HUB) || ai == ri) {
		return 0;
	}
	for (cz = t->z; cz < t->z + t->d; cz++) {
		for (cx = t->x; cx < t->x + t->w; cx++) {
			int dx[4] = { 1, -1, 0, 0 }, dz[4] = { 0, 0, 1, -1 }, k;

			for (k = 0; k < 4; k++) {
				const int nx = cx + dx[k], nz = cz + dz[k];

				if (nx < 0 || nz < 0 || nx >= lg->gw || nz >= lg->gd || lg->cells[nz][nx] != ri + 1) {
					continue;
				}
				/* a main cell: is it past the ramp? */
				u = alongx ? (lowend ? nx - m0->x : m0->x + m0->w - 1 - nx) : (lowend ? nz - m0->z : m0->z + m0->d - 1 - nz);
				v = alongx ? nz - m0->z : nx - m0->x;
				if (u >= from && u < L && v >= 0 && v < W) {
					return 1;
				}
			}
		}
	}
	return 0;
}

static int lgAddMezzanines(LgLayout *lg, char *err, int errlen)
{
	LgRng mr;
	int ri;

	mr.s = lg->seed ^ 0x6d657a7au ^ (0x9e3779b1u * (uint32_t)(lg->attempt + 1));

	for (ri = 1; ri < lg->numrooms; ri++) {
		LgArea m0 = lg->areas[ri];
		int o, k;

		if ((m0.flags & (LG_AREAFLAG_HUB | LG_AREAFLAG_TERRACE)) || m0.ramp || m0.open || m0.kind != LG_AREA_ROOM
				|| lgRange(&mr, 0, 99) >= (lg->theme.layout == LG_LAYOUT_LAB ? g_LgLab[lg->size].mezzpct : LG_MEZZ_PCT)) {
			continue;
		}
		o = lgRange(&mr, 0, 3);
		if (lg->theme.layout == LG_LAYOUT_STREET) {
			/* a street's building: the upper floor at the front first (over the
			 * door, where its balcony goes), the other ends after */
			const int f = lgStFront(lg, &m0);

			if (f >= 0) {
				o = f;
			}
		}
		for (k = 0; k < 4; k++) {
			const int t = (o + k) & 3, alongx = t & 1, lowend = t >> 1;
			const int L = alongx ? m0.w : m0.d, W = alongx ? m0.d : m0.w;
			const int ud = LG_MEZZ_UD;
			int rl = LG_MEZZ_RL;
			int u, v, x, z, clear = 1, c0;
			LgArea *U, *R, *B, *Z, *M = &lg->areas[ri];
			int ui, rli, bi, zi;
			int16_t tall;

			if (L < ud + rl + 1 && L >= ud + LG_MEZZ_RLMIN + 1) {
				rl = L - ud - 1 < LG_MEZZ_RL ? L - ud - 1 : LG_MEZZ_RL;   /* a steeper ramp in a smaller room */
			}
			if (L < ud + rl + 1 || W < 6) {
				continue;
			}
			c0 = lgRange(&mr, 0, 1) ? 0 : W - 3;
			/* the end, and the two sides along it: solid outside the room, or a
			 * part of this same room (a terrace, a wing) that also reaches the
			 * rest of the room (the main past the ramp) so it stays connected */
			for (v = 0; v < W && clear; v++) {
				lgMezzCell(&m0, alongx, lowend, -1, v, &x, &z);
				clear = lgMezzRingOk(lg, &m0, alongx, lowend, ud + rl, L, W, ri, x, z, 1);
			}
			for (u = 0; u < ud + rl && clear; u++) {
				lgMezzCell(&m0, alongx, lowend, u, -1, &x, &z);
				clear = lgMezzRingOk(lg, &m0, alongx, lowend, ud + rl, L, W, ri, x, z, !(u >= ud && c0 == 0));
				lgMezzCell(&m0, alongx, lowend, u, W, &x, &z);
				clear = clear && lgMezzRingOk(lg, &m0, alongx, lowend, ud + rl, L, W, ri, x, z, !(u >= ud && c0 != 0));
			}
			if (!clear) {
				continue;
			}
			if (lg->numareas + 4 > LG_MAXAREAS) {
				lgSetErr(err, errlen, "no room for a mezzanine (%d areas)", lg->numareas, 0);
				return 0;
			}
			tall = (int16_t)(m0.floor + LG_MEZZ_UNDER + LG_MEZZ_SLAB + LG_MEZZ_HEAD);
			ui = lg->numareas; rli = ui + 1; bi = ui + 2; zi = ui + 3;
			U = &lg->areas[ui]; R = &lg->areas[rli]; B = &lg->areas[bi]; Z = &lg->areas[zi];
			*U = *R = *B = *Z = m0;
			U->flags = LG_AREAFLAG_UNDER;
			lgMezzRect(&m0, alongx, lowend, 0, ud, 0, W, U);
			U->ceil = (int16_t)(m0.floor + LG_MEZZ_UNDER);
			R->flags = LG_AREAFLAG_BAY;
			lgMezzRect(&m0, alongx, lowend, ud, ud + rl, c0, c0 + 3, R);
			R->ramp = (uint8_t)(alongx ? (lowend ? LG_RAMP_XN : LG_RAMP_XP) : (lowend ? LG_RAMP_ZN : LG_RAMP_ZP));
			R->floorlo = m0.floor;
			R->floor = (int16_t)(m0.floor + LG_MEZZ_UNDER + LG_MEZZ_SLAB);
			R->ceil = tall;
			B->flags = LG_AREAFLAG_BAY;
			lgMezzRect(&m0, alongx, lowend, ud, ud + rl, c0 == 0 ? 3 : 0, c0 == 0 ? W : W - 3, B);
			B->ceil = tall;
			Z->flags = LG_AREAFLAG_MEZZ;
			Z->layer = 1;
			lgMezzRect(&m0, alongx, lowend, 0, ud, 0, W, Z);
			Z->floor = Z->floorlo = (int16_t)(m0.floor + LG_MEZZ_UNDER + LG_MEZZ_SLAB);
			Z->ceil = tall;
			Z->ramp = LG_RAMP_NONE;
			lgMezzRect(&m0, alongx, lowend, ud + rl, L, 0, W, M);
			if (M->ceil < tall) {
				M->ceil = tall;
			}
			lg->numareas += 4;
			lgMezzFill(lg, U, ui);
			lgMezzFill(lg, R, rli);
			lgMezzFill(lg, B, bi);
			lgMezzFill(lg, Z, zi);
			break;
		}
	}
	return 1;
}

/* The mezzanines' openings, after the ground's: along each edge of a layer 1
 * area, a run of ground cells of one area is one portal. Where the floors
 * meet (the ramp's top) it is level; where they differ (the bay below) it is
 * a ledge, with one gap three cells wide left without a railing. */
static int lgMezzPortals(LgLayout *lg, char *err, int errlen)
{
	LgRng mr;
	int ai;

	mr.s = lg->seed ^ 0x706f7274u ^ (0x9e3779b1u * (uint32_t)(lg->attempt + 1));

	for (ai = 0; ai < lg->numareas; ai++) {
		const LgArea *a = &lg->areas[ai];
		int side;

		if (!(a->flags & LG_AREAFLAG_MEZZ)) {
			continue;
		}
		for (side = 0; side < 4; side++) {
			const int alongz = side < 2;                /* the edge runs along z (sides -x, +x) */
			const int len = alongz ? a->d : a->w;
			const int line = side == 0 ? a->x : side == 1 ? a->x + a->w : side == 2 ? a->z : a->z + a->d;
			const int zhigh = side == 0 || side == 2;   /* the area is on the high-coordinate side of the line */
			int i = 0;

			while (i < len) {
				int nx, nz, nb, j, start = i;

				if (alongz) { nx = zhigh ? a->x - 1 : a->x + a->w; nz = a->z + i; }
				else        { nz = zhigh ? a->z - 1 : a->z + a->d; nx = a->x + i; }
				/* (the layer under this one's; nothing beside it in its own, 2026-11-16) */
				nb = (nx < 0 || nz < 0 || nx >= lg->gw || nz >= lg->gd || LG_CELLS(lg, a->layer)[nz][nx]) ? -1 : LG_CELLS(lg, a->layer - 1)[nz][nx] - 1;
				/* (a highrise's shaft or void, 2026-11-17: open from the ground
				 * up past this storey, nothing of the storey between) */
				if (nb < 0 && a->layer == 2 && LG_LAYOUT_HIGHRISE(lg->theme.layout) && nx >= 0 && nz >= 0 && nx < lg->gw && nz < lg->gd
						&& !LG_CELLS(lg, 2)[nz][nx] && lg->cells[nz][nx] && lg->areas[lg->cells[nz][nx] - 1].ceil > a->floor + 100) {
					nb = lg->cells[nz][nx] - 1;
				}
				if (nb < 0 || !(lg->areas[nb].flags & LG_AREAFLAG_BAY)) {
					i++;
					continue;
				}
				for (j = i + 1; j < len; j++) {
					int mx, mz;

					if (alongz) { mx = nx; mz = a->z + j; } else { mz = nz; mx = a->x + j; }
					if ((LG_CELLS(lg, a->layer - 1)[mz][mx] - 1 != nb && !(a->layer == 2 && !LG_CELLS(lg, 1)[mz][mx] && lg->cells[mz][mx] - 1 == nb))
							|| LG_CELLS(lg, a->layer)[mz][mx]) {
						break;
					}
				}
				i = j;
				{
					/* sub-runs for a ledge of 5 or more cells: the drop-down and the railed rest */
					int cuts[4], norail[3], n = 0, q;
					const LgArea *b = &lg->areas[nb];
					const int32_t wl = alongz ? lgWorldX(lg, line) : 0, wm = alongz ? 0 : lgWorldZ(lg, line);
					const int32_t m0 = alongz ? lgWorldZ(lg, a->z + start) : lgWorldX(lg, a->x + start);
					const int32_t m1 = alongz ? lgWorldZ(lg, a->z + i) : lgWorldX(lg, a->x + i);
					const int32_t mm = (m0 + m1) / 2;
					const int32_t ax0 = alongz ? wl : m0, az0 = alongz ? m0 : wm, ax1 = alongz ? wl : m1, az1 = alongz ? m1 : wm;
					const int ledge = modLevelGenFloorAt(lg, ai, ax0, az0) != modLevelGenFloorAt(lg, nb, ax0, az0)
							|| modLevelGenFloorAt(lg, ai, ax1, az1) != modLevelGenFloorAt(lg, nb, ax1, az1)
							|| modLevelGenFloorAt(lg, ai, alongz ? wl : mm, alongz ? mm : wm) != modLevelGenFloorAt(lg, nb, alongz ? wl : mm, alongz ? mm : wm);

					(void)b;
					if (ledge && i - start >= 5) {
						/* a drop-down two cells wide at one end, the rest railed */
						cuts[0] = start;
						if (lgRange(&mr, 0, 1)) {
							cuts[1] = start + 2; cuts[2] = i;
							norail[0] = 1; norail[1] = 0;
						} else {
							cuts[1] = i - 2; cuts[2] = i;
							norail[0] = 0; norail[1] = 1;
						}
						n = 2;
					} else {
						cuts[0] = start; cuts[1] = i;
						norail[0] = 0;
						n = 1;
					}
					for (q = 0; q < n; q++) {
						LgPortal *p;
						int c;

						if (lg->numportals >= LG_MAXPORTALS) {
							lgSetErr(err, errlen, "too many portals (%d)", lg->numportals, 0);
							return 0;
						}
						p = &lg->portals[lg->numportals++];
						memset(p, 0, sizeof(*p));
						if (alongz) {
							p->x0 = p->x1 = (int16_t)line;
							p->z0 = (int16_t)(a->z + cuts[q]);
							p->z1 = (int16_t)(a->z + cuts[q + 1]);
						} else {
							p->z0 = p->z1 = (int16_t)line;
							p->x0 = (int16_t)(a->x + cuts[q]);
							p->x1 = (int16_t)(a->x + cuts[q + 1]);
						}
						p->a = (int16_t)(zhigh ? nb : ai);
						p->b = (int16_t)(zhigh ? ai : nb);
						p->drop = (uint8_t)ledge;
						p->norail = (uint8_t)(ledge && norail[q]);
						p->pada = p->padb = -1;
						for (c = cuts[q]; c < cuts[q + 1]; c++) {
							if (alongz) {
								LG_OPENS(lg, a->layer)[a->z + c][line - 1] |= LG_OPEN_XP;
								if (lg->theme.layout == LG_LAYOUT_STATION && b->layer) {
									LG_OPENS(lg, b->layer)[a->z + c][line - 1] |= LG_OPEN_XP;
								}
							} else {
								LG_OPENS(lg, a->layer)[line - 1][a->x + c] |= LG_OPEN_ZP;
								if (lg->theme.layout == LG_LAYOUT_STATION && b->layer) {
									LG_OPENS(lg, b->layer)[line - 1][a->x + c] |= LG_OPEN_ZP;
								}
							}
						}
					}
				}
			}
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Step 5: area graph distances, start, extraction, objectives          */

static int lgAreaGraph(LgLayout *lg, LgRng *r, char *err, int errlen)
{
	int16_t queue[LG_MAXAREAS];
	int head = 0, tail = 0, i, best = -1;
	int16_t cand[LG_MAXROOMS];
	int numcand = 0;

	for (i = 0; i < lg->numareas; i++) {
		lg->areas[i].dist = -1;
	}

	lg->startarea = 0;
	lg->areas[0].dist = 0;
	queue[tail++] = 0;

	while (head < tail) {
		int cur = queue[head++];

		for (i = 0; i < lg->numportals; i++) {
			const LgPortal *p = &lg->portals[i];
			int other = p->a == cur ? p->b : p->b == cur ? p->a : -1;

			if (other >= 0 && !p->window && lg->areas[other].dist < 0) {
				lg->areas[other].dist = (int16_t)(lg->areas[cur].dist + 1);
				queue[tail++] = (int16_t)other;
			}
		}
	}

	for (i = 0; i < lg->numareas; i++) {
		if (lg->areas[i].dist < 0 && lg->areas[i].special != LG_SPECIAL_WINDOW
				&& !(LG_LAYOUT_HIGHRISE(lg->theme.layout) && lg->areas[i].special == LG_SPECIAL_PIT)) {   /* (a site's street: seen from its open edges only) */
			lgSetErr(err, errlen, "area %d unreachable from start", i, 0);
			return 0;
		}
	}

	for (i = 1; i < lg->numrooms; i++) {
		if (LG_NOWALK(&lg->areas[i])) {
			continue;   /* (a ravine's floor kills; nobody stands on a rack) */
		}
		if (best < 0 || lg->areas[i].dist > lg->areas[best].dist) {
			best = i;
		}
	}

	if (best < 0) {
		lgSetErr(err, errlen, "no room for extraction", 0, 0);
		return 0;
	}

	lg->extractarea = (int16_t)best;
	lg->maxdist = lg->areas[best].dist;
	lg->areas[0].flags |= LG_AREAFLAG_START;
	lg->areas[best].flags |= LG_AREAFLAG_EXTRACT;

	/* objectives: rooms in the middle third of the distance */
	for (i = 1; i < lg->numrooms; i++) {
		int d = lg->areas[i].dist;
		if (i != best && !LG_NOWALK(&lg->areas[i]) && d * 3 >= lg->maxdist && d * 3 <= lg->maxdist * 2) {
			cand[numcand++] = (int16_t)i;
		}
	}

	/* fall back to any room that is neither start nor extraction */
	if (numcand == 0) {
		for (i = 1; i < lg->numrooms; i++) {
			if (i != best && !LG_NOWALK(&lg->areas[i])) {
				cand[numcand++] = (int16_t)i;
			}
		}
	}

	for (i = numcand - 1; i > 0; i--) {
		int j = lgRange(r, 0, i);
		int16_t t = cand[i];
		cand[i] = cand[j];
		cand[j] = t;
	}

	for (i = 0; i < numcand && lg->numobj < LG_MAXOBJ; i++) {
		lg->objareas[lg->numobj++] = cand[i];
		lg->areas[cand[i]].flags |= LG_AREAFLAG_OBJECTIVE;
	}

	return 1;
}

/* ------------------------------------------------------------------ */
/* Step 6: navigation pads and links                                    */

static int lgBuildNav(LgLayout *lg, char *err, int errlen)
{
	int i, j, k;
	int16_t list[LG_MAXPADS];

	/* one pad at every area centre */
	for (i = 0; i < lg->numareas; i++) {
		LgArea *a = &lg->areas[i];
		int32_t cx = lgWorldX(lg, a->x) + a->w * LG_CELL / 2;
		int32_t cz = lgWorldZ(lg, a->z) + a->d * LG_CELL / 2;
		int pad;

		if (a->special == LG_SPECIAL_WINDOW || LG_NOWALK(a)) {
			a->centrepad = -1;   /* nobody stands in a window, or on a ravine's floor */
			continue;
		}
		pad = lgAddPad(lg, LG_PAD_NAV, cx, cz, 0, i);
		if (pad < 0) {
			goto full;
		}
		a->centrepad = (int16_t)pad;
	}

	/* extra pads along long areas, every 6 cells */
	for (i = 0; i < lg->numareas; i++) {
		LgArea *a = &lg->areas[i];
		int alongx = a->w >= a->d;
		int len = alongx ? a->w : a->d;
		int pos;

		if (len < 10 || LG_NOWALK(a)) {
			continue;
		}

		for (pos = 3; pos < len - 2; pos += 6) {
			int32_t x, z;

			if (lgAbs(pos - len / 2) <= 2) {
				continue;
			}

			if (alongx) {
				x = lgWorldX(lg, a->x + pos) + LG_CELL / 2;
				z = lgWorldZ(lg, a->z) + a->d * LG_CELL / 2;
			} else {
				x = lgWorldX(lg, a->x) + a->w * LG_CELL / 2;
				z = lgWorldZ(lg, a->z + pos) + LG_CELL / 2;
			}

			if (lgAddPad(lg, LG_PAD_NAV, x, z, 0, i) < 0) {
				goto full;
			}
		}
	}

	/* one pad either side of every portal, half a cell in */
	for (i = 0; i < lg->numportals; i++) {
		LgPortal *p = &lg->portals[i];
		int32_t mx = (lgWorldX(lg, p->x0) + lgWorldX(lg, p->x1)) / 2;
		int32_t mz = (lgWorldZ(lg, p->z0) + lgWorldZ(lg, p->z1)) / 2;
		int pa, pb;

		if (p->drop) {
			continue;   /* a ledge: no way across, so no pads to cross by */
		}
		if (LG_NOWALK(&lg->areas[p->a]) || LG_NOWALK(&lg->areas[p->b])) {
			continue;   /* between pieces of a ravine's floor: nobody walks there */
		}
		if (p->x0 == p->x1) {
			pa = lgAddPad(lg, LG_PAD_NAV, mx - LG_NAV_INSET, mz, 270, p->a);
			pb = lgAddPad(lg, LG_PAD_NAV, mx + LG_NAV_INSET, mz, 90, p->b);
		} else {
			pa = lgAddPad(lg, LG_PAD_NAV, mx, mz - LG_NAV_INSET, 180, p->a);
			pb = lgAddPad(lg, LG_PAD_NAV, mx, mz + LG_NAV_INSET, 0, p->b);
		}

		if (pa < 0 || pb < 0) {
			goto full;
		}

		p->pada = (int16_t)pa;
		p->padb = (int16_t)pb;

		if (!lgLink(lg, pa, pb)) {
			lgSetErr(err, errlen, "nav pad %d has too many links", pa, 0);
			return 0;
		}
	}

	/* inside an area every pad sees every other (areas are open rectangles):
	 * link each to the centre first, then all pairs while room remains */
	for (i = 0; i < lg->numareas; i++) {
		int n = 0, centre = lg->areas[i].centrepad;

		if (centre < 0) {
			continue;
		}
		for (j = 0; j < lg->numpads; j++) {
			if (lg->pads[j].kind == LG_PAD_NAV && lg->pads[j].area == i) {
				list[n++] = (int16_t)j;
			}
		}

		for (j = 0; j < n; j++) {
			if (!lgLink(lg, centre, list[j])) {
				lgSetErr(err, errlen, "area %d centre pad has too many links", i, 0);
				return 0;
			}
		}

		for (j = 0; j < n; j++) {
			for (k = j + 1; k < n; k++) {
				lgLink(lg, list[j], list[k]); /* optional, may be full */
			}
		}
	}

	return 1;

full:
	lgSetErr(err, errlen, "too many pads (%d)", lg->numpads, 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Step 7: guard, start and objective pads                              */

static int lgAreaTouchesStart(const LgLayout *lg, int area)
{
	int i;
	for (i = 0; i < lg->numportals; i++) {
		const LgPortal *p = &lg->portals[i];
		if ((p->a == area && p->b == lg->startarea) || (p->b == area && p->a == lg->startarea)) {
			return 1;
		}
	}
	return 0;
}

/* Fills spots with points on a jittered grid inside the area, at least
 * margin from its edges. Returns the count. */
static int lgAreaSpots(const LgLayout *lg, LgRng *r, int area, int margin, int step,
		int32_t *xs, int32_t *zs, int max)
{
	const LgArea *a = &lg->areas[area];
	int32_t x0 = lgWorldX(lg, a->x) + margin, x1 = lgWorldX(lg, a->x + a->w) - margin;
	int32_t z0 = lgWorldZ(lg, a->z) + margin, z1 = lgWorldZ(lg, a->z + a->d) - margin;
	int32_t x, z;
	int n = 0;

	for (z = z0; z <= z1 && n < max; z += step) {
		for (x = x0; x <= x1 && n < max; x += step) {
			int32_t jx = x + lgRange(r, -LG_GUARD_JITTER, LG_GUARD_JITTER);
			int32_t jz = z + lgRange(r, -LG_GUARD_JITTER, LG_GUARD_JITTER);

			if (jx < x0) jx = x0;
			if (jx > x1) jx = x1;
			if (jz < z0) jz = z0;
			if (jz > z1) jz = z1;

			xs[n] = jx;
			zs[n] = jz;
			n++;
		}
	}

	return n;
}

static void lgShuffleSpots(LgRng *r, int32_t *xs, int32_t *zs, int n)
{
	int i;
	for (i = n - 1; i > 0; i--) {
		int j = lgRange(r, 0, i);
		int32_t t;
		t = xs[i]; xs[i] = xs[j]; xs[j] = t;
		t = zs[i]; zs[i] = zs[j]; zs[j] = t;
	}
}

static int lgFirstPortalAngle(const LgLayout *lg, int area, int32_t fromx, int32_t fromz)
{
	int i;
	for (i = 0; i < lg->numportals; i++) {
		const LgPortal *p = &lg->portals[i];
		if (p->a == area || p->b == area) {
			int32_t mx = (lgWorldX(lg, p->x0) + lgWorldX(lg, p->x1)) / 2;
			int32_t mz = (lgWorldZ(lg, p->z0) + lgWorldZ(lg, p->z1)) / 2;
			return lgAngleFor(mx - fromx, mz - fromz);
		}
	}
	return 0;
}

static int lgIsLiftShaft(const LgLayout *lg, int ai)
{
	for (int i = 0; i < lg->numlifts; i++) {
		if (lg->lifts[i].area == ai) {
			return 1;
		}
	}
	return 0;
}

static int lgBuildSpawns(LgLayout *lg, LgRng *r, char *err, int errlen)
{
	int32_t xs[64], zs[64];
	int i, j, n;

	/* start pads: up to 3 in the start room, facing its first opening */
	{
		const LgArea *a = &lg->areas[lg->startarea];
		int32_t cx = lgWorldX(lg, a->x) + a->w * LG_CELL / 2;
		int32_t cz = lgWorldZ(lg, a->z) + a->d * LG_CELL / 2;
		int alongx = a->w >= a->d;
		int32_t half = (alongx ? a->w : a->d) * LG_CELL / 2 - LG_GUARD_MARGIN;
		int offs[3] = { 0, -150, 150 };

		for (j = 0; j < 3; j++) {
			int32_t x = cx, z = cz;

			if (lgAbs(offs[j]) > half) {
				continue;
			}
			if (alongx) {
				x += offs[j];
			} else {
				z += offs[j];
			}

			if (lgAddPad(lg, LG_PAD_START, x, z, lgFirstPortalAngle(lg, lg->startarea, x, z),
						lg->startarea) < 0) {
				goto full;
			}
		}
	}

	/* guard pads */
	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];
		int want;

		if (i == lg->startarea || lgIsLiftShaft(lg, i) || a->special == LG_SPECIAL_BALCONY || a->special == LG_SPECIAL_BRIDGE
				|| a->special == LG_SPECIAL_FIREESCAPE || LG_NOWALK(a)) {
			continue;   /* (a balcony or bridge is narrow: a guard standing on one blocks its drop-down or the way across; a pit kills) */
		}

		if (a->kind == LG_AREA_ROOM) {
			want = 2 + a->w * a->d / 40;
			if (want > 6) {
				want = 6;
			}

			n = lgAreaSpots(lg, r, i, LG_GUARD_MARGIN, LG_GUARD_STEP, xs, zs, 64);
			lgShuffleSpots(r, xs, zs, n);

			for (j = 0; j < n && j < want; j++) {
				if (lgAddPad(lg, LG_PAD_GUARD, xs[j], zs[j], lgRange(r, 0, 7) * 45, i) < 0) {
					goto full;
				}
			}
		} else {
			int len = a->w > a->d ? a->w : a->d;

			if (len >= 4 && !lgAreaTouchesStart(lg, i)) {
				n = lgAreaSpots(lg, r, i, LG_GUARD_MARGIN / 2, LG_GUARD_STEP, xs, zs, 64);
				if (n > 0) {
					j = lgRange(r, 0, n - 1);
					if (lgAddPad(lg, LG_PAD_GUARD, xs[j], zs[j], lgRange(r, 0, 7) * 45, i) < 0) {
						goto full;
					}
				}
			}
		}
	}

	/* objective pads: 2 per objective room */
	for (i = 0; i < lg->numobj; i++) {
		int area = lg->objareas[i];

		n = lgAreaSpots(lg, r, area, 100, LG_GUARD_STEP, xs, zs, 64);
		lgShuffleSpots(r, xs, zs, n);

		for (j = 0; j < n && j < 2; j++) {
			if (lgAddPad(lg, LG_PAD_OBJECTIVE, xs[j], zs[j], lgRange(r, 0, 7) * 45, area) < 0) {
				goto full;
			}
		}
	}

	return 1;

full:
	lgSetErr(err, errlen, "too many pads (%d)", lg->numpads, 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Pillars                                                              */

/* Squared distance from a point to the segment a-b */
static int64_t lgSegDist2(int32_t px, int32_t pz, int32_t ax, int32_t az, int32_t bx, int32_t bz)
{
	const int64_t dx = bx - ax, dz = bz - az, len2 = dx * dx + dz * dz;
	int64_t t, qx, qz;

	if (len2 == 0) {
		return (int64_t)(px - ax) * (px - ax) + (int64_t)(pz - az) * (pz - az);
	}
	t = ((int64_t)(px - ax) * dx + (int64_t)(pz - az) * dz);
	if (t < 0) t = 0;
	if (t > len2) t = len2;
	qx = ax + dx * t / len2;
	qz = az + dz * t / len2;
	return (px - qx) * (px - qx) + (pz - qz) * (pz - qz);
}

/* Whether a pillar may stand in cell (cx,cz) of a room: well inside it, clear of
 * every pad, off the walkways between the area's nav pads, and not next to
 * another pillar. The same rules are what the validator holds it to. */
static int lgPillarOk(const LgLayout *lg, int ai, int cx, int cz)
{
	const LgArea *a = &lg->areas[ai];
	const int32_t px = lgWorldX(lg, cx) + LG_CELL / 2, pz = lgWorldZ(lg, cz) + LG_CELL / 2;
	int i, j;

	if (a->kind != LG_AREA_ROOM || a->ramp || (a->flags & (LG_AREAFLAG_MEZZ | LG_AREAFLAG_UNDER)) || cx - a->x < 2 || a->x + a->w - 1 - cx < 2 || cz - a->z < 2 || a->z + a->d - 1 - cz < 2) {
		return 0;
	}
	for (i = 0; i < lg->numpads; i++) {
		const LgPad *p = &lg->pads[i];

		if (p->area != ai) {
			continue;
		}
		if (lgAbs(p->x - px) < 150 && lgAbs(p->z - pz) < 150) {
			return 0;
		}
		if (p->kind == LG_PAD_NAV) {
			for (j = i + 1; j < lg->numpads; j++) {
				const LgPad *q = &lg->pads[j];

				if (q->area == ai && q->kind == LG_PAD_NAV && lgSegDist2(px, pz, p->x, p->z, q->x, q->z) < 130 * 130) {
					return 0;
				}
			}
		}
	}
	for (i = 0; i < lg->numobst; i++) {
		if (lgAbs(lg->obst[i].cx - cx) < 2 && lgAbs(lg->obst[i].cz - cz) < 2) {
			return 0;
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Cover                                                                */

/* The crates as the A51 crates are in the game's own setups (invincible,
 * 0x100, not falling). The cars as Chicago's taxis and police cars are
 * (invincible, 0x100, keeping their own colours: 0x400 and 0x1000), but not
 * falling: they stand where their pad puts them.
 * Taxi: box x -99.5..99.5, y -44.7..121.1, z -192.1..223.8. Police car: x
 * -100..100, y -29.5..105.8, z -209.7..242.8 (model units x 0.0999). */
const LgPropDef g_LgProps[LG_PROP_MODELS] = {
	{ 0x0020, 50, 50, 100, 50, 0, 0, 0x00020100u },
	{ 0x0021, 50, 50, 100, 50, 0, 0, 0x00020100u },
	{ 0x0022, 50, 50, 100, 50, 0, 0, 0x00020100u },
	{ 0x0086, 100, 208, 166, 45, 16, 256, 0x00021500u },
	{ 0x0087, 100, 227, 135, 30, 17, 256, 0x00021500u },
	{ 0x0003, 58, 58, 116, 58, 0, 256, 0x00020100u },   /* BORG_CRATE */
	{ 0x00e2, 35, 35, 88, 44, 0, 256, 0x00020100u },   /* BARREL */
	{ 0x005a, 49, 53, 98, 49, 0, 256, 0x00020100u },   /* HOVERCRATE1 */
	{ 0x0067, 40, 80, 60, 30, 0, 256, 0x00020100u },   /* A51TROLLEY */
	{ 0x0046, 88, 38, 75, 37, 0, 256, 0x00020100u },   /* DD_DESK */
	{ 0x0040, 102, 56, 98, 49, 0, 256, 0x00020100u },   /* DD_REDSOFA */
	{ 0x005f, 117, 70, 76, 38, 0, 256, 0x00020100u },   /* DD_STONEDESK */
	{ 0x0062, 91, 51, 90, 45, 0, 256, 0x00020100u },   /* A51TABLE */
	{ 0x00bc, 93, 56, 82, 41, 0, 256, 0x00020100u },   /* LAB_TABLE */
	{ 0x0160, 84, 50, 270, 135, 0, 256, 0x00020100u },   /* CI_CABINET */
	{ 0x0166, 92, 33, 58, 29, 0, 256, 0x00020100u },   /* CI_TABLE */
	{ 0x003c, 25, 23, 152, 76, 0, 256, 0x00020100u },   /* DD_PLANTRUBBER */
	{ 0x003e, 57, 54, 124, 62, 0, 256, 0x00020100u },   /* DD_PLANTSPIDER */
	{ 0x0068, 120, 21, 184, 92, 0, 256, 0x00020100u },   /* A51DIVIDE */
	{ 0x00ac, 92, 54, 144, 72, 0, 256, 0x00021500u },   /* DUMPSTER */
	{ 0x0127, 146, 328, 124, 62, 0, 256, 0x00021500u },   /* LIMO */
	{ 0x0053, 175, 488, 175, 87, 0, 256, 0x00021500u },   /* DD_HOVCAR */
	{ 0x0054, 150, 250, 200, 100, 0, 256, 0x00021500u },   /* DD_HOVCAB */
	{ 0x003a, 92, 100, 112, 56, 0, 256, 0x00021500u },   /* HOVBIKE */
	{ 0x0085, 247, 464, 413, 150, -9, 256, 0x00021500u },   /* DDJUMPSHIP */
	{ 0x00eb, 487, 487, 305, 115, 0, 256, 0x00021500u },   /* ELVIS_SAUCER */
	{ 0x00c8, 320, 403, 383, 161, 64, 256, 0x00021500u },   /* SK_SHUTTLE */
	{ 0x0133, 92, 150, 400, 200, 0, 256, 0x00020100u },   /* SK_JONRUBBLE5 */
	{ 0x0132, 97, 350, 300, 150, 0, 256, 0x00020100u },   /* SK_JONRUBBLE4 */
	{ 0x0129, 399, 382, 764, 382, 0, 256, 0x00021500u },   /* A51DISH */
	{ 0x016e, 85, 102, 90, 45, 0, 256, 0x00020100u },   /* CV_F_BED */
	{ 0x016c, 40, 40, 134, 67, 0, 256, 0x00020100u },   /* CV_LAMP */
	{ 0x0168, 41, 35, 162, 81, 0, 256, 0x00020100u },   /* CV_CHAIR1 */
	{ 0x00f1, 46, 44, 94, 47, 0, 256, 0x00020100u },   /* AF1_PASSCHAIR */
	{ 0x002f, 37, 38, 220, 110, 0, 256, 0x00020100u },   /* AIVILLABOT1 */
	{ 0x0039, 117, 112, 110, 55, 0, 256, 0x00020100u },   /* DD_FANWALL */
	{ 0x015d, 37, 38, 148, 74, 0, 256, 0x00020100u },   /* G5_CHAIR */
	{ 0x0041, 55, 56, 100, 50, 0, 256, 0x00020100u },   /* DD_REDARM */
	{ 0x0186, 22, 35, 76, 38, 0, 256, 0x00020100u },   /* AF1_TOILET (size guessed: a toilet's) */
};

#define LG_COVER_PADGAP  70   /* clear floor between low cover and a pad */
#define LG_COVER_GAP     90   /* and between two pieces: room to walk between */

/* A piece of low cover fits: on a flat floor of the area, inside it by
 * margin, clear of its pads and of every other obstacle */
static int lgCoverOkGap(const LgLayout *lg, int ai, int32_t x, int32_t z, int hx, int hz, int margin, int padgap, int gap);

static int lgCoverOk(const LgLayout *lg, int ai, int32_t x, int32_t z, int hx, int hz, int margin, int padgap)
{
	return lgCoverOkGap(lg, ai, x, z, hx, hz, margin, padgap, LG_COVER_GAP);
}

/* gap: clear floor to the other pieces */
static int lgCoverOkGap(const LgLayout *lg, int ai, int32_t x, int32_t z, int hx, int hz, int margin, int padgap, int gap)
{
	const LgArea *a = &lg->areas[ai];
	const int32_t x0 = lgWorldX(lg, a->x), z0 = lgWorldZ(lg, a->z);
	int i;

	if (a->ramp || x - hx < x0 + margin || x + hx > x0 + a->w * LG_CELL - margin
			|| z - hz < z0 + margin || z + hz > z0 + a->d * LG_CELL - margin) {
		return 0;
	}
	for (i = 0; i < lg->numpads; i++) {
		const LgPad *p = &lg->pads[i];

		if (p->area == ai && lgAbs(p->x - x) < hx + padgap && lgAbs(p->z - z) < hz + padgap) {
			return 0;
		}
	}
	for (i = 0; i < lg->numobst; i++) {
		const LgObstacle *o = &lg->obst[i];

		if (o->area == ai && lgAbs(o->x - x) < o->halfx + hx + gap && lgAbs(o->z - z) < o->halfz + hz + gap) {
			return 0;
		}
	}
	return 1;
}

static void lgAddCover(LgLayout *lg, int ai, int32_t x, int32_t z, int hx, int hz, int height)
{
	LgObstacle *o = &lg->obst[lg->numobst++];

	memset(o, 0, sizeof(*o));
	o->area = (int16_t)ai;
	o->x = x;
	o->z = z;
	o->cx = (int16_t)((x - lg->originx) / LG_CELL);
	o->cz = (int16_t)((z - lg->originz) / LG_CELL);
	o->halfx = (int16_t)hx;
	o->halfz = (int16_t)hz;
	o->height = (int16_t)height;
}

/* The game's props each layout is dressed with (2026-11-16,
 * claude/levelgen-props.md): what stands on its floors as cover (a crate is 0,
 * the other crates come with it), what furnishes its rooms against the walls,
 * and its one big set piece. -1 ends a list. */
static const int8_t g_LgCoverPools[LG_LAYOUT_ALL][8] = {
	/* hub */      { 0, LG_PROP_BORGCRATE, LG_PROP_BARREL, LG_PROP_HOVERCRATE, LG_PROP_TROLLEY, -1 },
	/* lab */      { 0, LG_PROP_BORGCRATE, LG_PROP_TROLLEY, LG_PROP_LABTABLE, LG_PROP_A51TABLE, -1 },
	/* streets */  { 0, LG_PROP_BARREL, LG_PROP_DUMPSTER, LG_PROP_HOVERCRATE, -1 },
	/* ravine */   { 0, LG_PROP_BARREL, LG_PROP_ROCK1, LG_PROP_ROCK2, LG_PROP_BORGCRATE, -1 },
	/* rooftops */ { 0, LG_PROP_BARREL, LG_PROP_HOVERCRATE, LG_PROP_ROOFVENT, LG_PROP_ROOFVENT, -1 },
	/* villa */    { LG_PROP_PLANTSPIDER, LG_PROP_PLANTRUBBER, -1 },   /* (potted plants; its gardens do the rest) */
	/* docks */    { 0, 0, LG_PROP_BARREL, LG_PROP_BARREL, LG_PROP_HOVERCRATE, LG_PROP_BORGCRATE, LG_PROP_DUMPSTER, -1 },
	/* backwoods */ { -1 },
	/* penthouse */ { LG_PROP_PLANTSPIDER, LG_PROP_PLANTRUBBER, LG_PROP_REDARM, LG_PROP_CITABLE, -1 },
	/* site */     { 0, 0, LG_PROP_BORGCRATE, LG_PROP_BARREL, LG_PROP_HOVERCRATE, LG_PROP_TROLLEY, -1 },
	/* plaza */    { LG_PROP_BARREL, LG_PROP_BARREL, 0, LG_PROP_TROLLEY, LG_PROP_PLANTSPIDER, -1 },
	/* mall */     { LG_PROP_PLANTRUBBER, LG_PROP_PLANTSPIDER, LG_PROP_REDSOFA, -1 },
	/* pipeworks */ { 0, 1, 2, LG_PROP_BARREL, LG_PROP_BARREL, LG_PROP_HOVERCRATE, -1 },
	/* pier 17 */  { 0, 0, LG_PROP_BARREL, LG_PROP_BARREL, LG_PROP_HOVERCRATE, LG_PROP_BORGCRATE, LG_PROP_DUMPSTER, -1 },
	/* hollow creek */ { -1 },   /* (dressed by hand: lgTryTrail) */
	/* station */ { 0, LG_PROP_TROLLEY, LG_PROP_BARREL, -1 },
	/* bunker 2 */ { 0, 0, LG_PROP_BORGCRATE, LG_PROP_BARREL, LG_PROP_TROLLEY, -1 },
};
static const int8_t g_LgFurnishPools[LG_LAYOUT_ALL][10] = {
	/* hub */      { LG_PROP_DDDESK, LG_PROP_REDSOFA, LG_PROP_REDARM, LG_PROP_STONEDESK, LG_PROP_CICABINET, LG_PROP_PLANTRUBBER, LG_PROP_PLANTSPIDER, LG_PROP_DIVIDER, LG_PROP_G5CHAIR, -1 },
	/* lab */      { LG_PROP_LABTABLE, LG_PROP_A51TABLE, LG_PROP_CICABINET, LG_PROP_TROLLEY, LG_PROP_DIVIDER, LG_PROP_PLANTRUBBER, -1 },
	/* streets */  { LG_PROP_DDDESK, LG_PROP_REDSOFA, LG_PROP_CITABLE, LG_PROP_PLANTSPIDER, LG_PROP_CICABINET, -1 },
	/* ravine */   { LG_PROP_A51TABLE, LG_PROP_CICABINET, LG_PROP_TROLLEY, -1 },
	/* rooftops */ { LG_PROP_DDDESK, LG_PROP_REDSOFA, LG_PROP_REDARM, LG_PROP_PLANTRUBBER, LG_PROP_PLANTSPIDER, LG_PROP_CITABLE, -1 },
	/* villa */    { LG_PROP_BED, LG_PROP_LAMP, LG_PROP_VILLACHAIR, LG_PROP_ARMCHAIR, LG_PROP_REDSOFA, LG_PROP_PLANTSPIDER, LG_PROP_CITABLE, -1 },
	/* docks */    { LG_PROP_DDDESK, LG_PROP_A51TABLE, LG_PROP_CICABINET, LG_PROP_TROLLEY, LG_PROP_DIVIDER, LG_PROP_G5CHAIR, LG_PROP_CITABLE, -1 },
	/* backwoods */ { LG_PROP_REDSOFA, LG_PROP_ARMCHAIR, LG_PROP_CITABLE, LG_PROP_LAMP, LG_PROP_BED, LG_PROP_CICABINET, LG_PROP_A51TABLE, LG_PROP_DDDESK, LG_PROP_PLANTSPIDER, -1 },
	/* penthouse */ { LG_PROP_CICABINET, LG_PROP_REDSOFA, LG_PROP_LAMP, LG_PROP_PLANTSPIDER, LG_PROP_PLANTRUBBER, LG_PROP_STONEDESK, LG_PROP_ARMCHAIR, LG_PROP_BED, LG_PROP_DDDESK, -1 },
	/* site */     { LG_PROP_CICABINET, LG_PROP_A51TABLE, LG_PROP_TROLLEY, LG_PROP_DIVIDER, LG_PROP_BARREL, -1 },
	/* plaza */    { LG_PROP_CITABLE, LG_PROP_VILLACHAIR, LG_PROP_CICABINET, LG_PROP_A51TABLE, LG_PROP_BARREL, LG_PROP_PLANTRUBBER, LG_PROP_ARMCHAIR, LG_PROP_LAMP, LG_PROP_BED, -1 },
	/* mall */     { LG_PROP_CICABINET, LG_PROP_CICABINET, LG_PROP_DIVIDER, LG_PROP_PLANTSPIDER, LG_PROP_LAMP, LG_PROP_A51TABLE, -1 },
	/* pipeworks */ { LG_PROP_CICABINET, LG_PROP_BARREL, LG_PROP_LABTABLE, -1 },
	/* pier 17 */  { LG_PROP_DDDESK, LG_PROP_A51TABLE, LG_PROP_CICABINET, LG_PROP_TROLLEY, LG_PROP_DIVIDER, LG_PROP_G5CHAIR, LG_PROP_CITABLE, -1 },
	/* hollow creek */ { LG_PROP_REDSOFA, LG_PROP_ARMCHAIR, LG_PROP_CITABLE, LG_PROP_LAMP, LG_PROP_BED, LG_PROP_CICABINET, LG_PROP_A51TABLE, LG_PROP_DDDESK, LG_PROP_PLANTSPIDER, -1 },
	/* station */ { LG_PROP_REDSOFA, LG_PROP_REDARM, LG_PROP_CICABINET, LG_PROP_TROLLEY, -1 },
	/* bunker 2 */ { LG_PROP_A51TABLE, LG_PROP_LABTABLE, LG_PROP_CICABINET, LG_PROP_DIVIDER, LG_PROP_TROLLEY, LG_PROP_G5CHAIR, -1 },
};
static const int8_t g_LgSetPieces[LG_LAYOUT_ALL][4] = {
	/* hub */      { -1 },
	/* lab */      { -1 },
	/* streets */  { -1 },
	/* ravine */   { LG_PROP_SAUCER, LG_PROP_SHUTTLE, LG_PROP_DISH, -1 },
	/* rooftops */ { -1 },
	/* villa */    { LG_PROP_LIMO, -1 },
	/* docks */    { -1 },
	/* backwoods */ { -1 },
	/* penthouse */ { LG_PROP_JUMPSHIP, LG_PROP_HOVCAB, -1 },   /* (on the landing pad) */
	/* site */     { -1 },
	/* plaza */    { -1 },
	/* mall */     { -1 },
	/* pipeworks */ { -1 },
	/* pier 17 */  { -1 },   /* (its own, by hand: lgTryPier) */
	/* hollow creek */ { -1 },
	/* station */ { -1 },
	/* bunker 2 */ { -1 },
};

static int lgPoolPick(const int8_t *pool, int len, LgRng *r)
{
	int n = 0;

	while (n < len && pool[n] >= 0) {
		n++;
	}
	return n ? pool[lgRange(r, 0, n - 1)] : -1;
}

/* One of g_LgProps (not a crate) at x, z facing turn (0 +z, 1 -x, 2 -z, 3
 * +x), if it fits: on the floor, under the ceiling, clear by margin and gap */
static int lgTryProp(LgLayout *lg, int ai, int prop, int32_t x, int32_t z, int turn, int margin, int padgap, int gap)
{
	const LgPropDef *d = &g_LgProps[prop];
	const LgArea *a = &lg->areas[ai];
	const int hx = (turn & 1) ? d->halfl : d->halfw, hz = (turn & 1) ? d->halfw : d->halfl;
	LgObstacle *o;

	if (prop < LG_CRATE_MODELS || lg->numobst >= LG_MAXOBST || (!a->open && a->ceil - a->floor < d->height + 20)
			|| !lgCoverOkGap(lg, ai, x, z, hx, hz, margin, padgap, gap)) {
		return 0;
	}
	lgAddCover(lg, ai, x, z, hx, hz, d->height);
	o = &lg->obst[lg->numobst - 1];
	o->crate = 1;
	o->model = (uint8_t)prop;
	o->turn = (uint8_t)turn;
	o->scale = d->scale;
	return 1;
}

/* Furniture: in the rooms of a building, a few pieces with their backs to
 * the walls, facing in, clear of the doors and the pads (2026-11-16) */
static void lgPlaceFurniture(LgLayout *lg, LgRng *r)
{
	const int8_t *pool = g_LgFurnishPools[lg->theme.layout];
	int ai;

	for (ai = 0; ai < lg->numareas; ai++) {
		const LgArea *a = &lg->areas[ai];
		int want, tries;

		if (a->open || a->ramp || LG_NOWALK(a) || a->special || a->kind != LG_AREA_ROOM || a->w < 3 || a->d < 3
				|| ai == lg->startarea || lgIsLiftShaft(lg, ai) || a->ceil - a->floor < 200 || (LG_LAYOUT_WOODSLIKE(lg->theme.layout) && a->theme == 4)) {
			continue;
		}
		want = 1 + a->w * a->d / 14;
		want = want > 5 ? 5 : want;
		for (tries = 0; tries < 30 && want > 0 && lg->numobst < LG_MAXOBST; tries++) {
			const int prop = lgPoolPick(pool, 10, r);
			const LgPropDef *d;
			const int wall = lgRange(r, 0, 3);   /* the wall it stands against: 0 -z, 1 +x, 2 +z, 3 -x */
			const int32_t x0 = lgWorldX(lg, a->x), z0 = lgWorldZ(lg, a->z), x1 = x0 + a->w * LG_CELL, z1 = z0 + a->d * LG_CELL;
			int turn, hx, hz;
			int32_t x, z;

			if (prop < 0) {
				break;
			}
			d = &g_LgProps[prop];
			/* facing away from its wall */
			turn = wall == 0 ? 0 : wall == 1 ? 1 : wall == 2 ? 2 : 3;
			hx = (turn & 1) ? d->halfl : d->halfw;
			hz = (turn & 1) ? d->halfw : d->halfl;
			if (2 * hx + 100 > x1 - x0 || 2 * hz + 100 > z1 - z0) {
				continue;
			}
			/* (40 off it: a line along it, 30 past its ends, still clears the wall) */
			x = wall == 1 ? x1 - 40 - hx : wall == 3 ? x0 + 40 + hx : lgRange(r, x0 + 40 + hx, x1 - 40 - hx);
			z = wall == 2 ? z1 - 40 - hz : wall == 0 ? z0 + 40 + hz : lgRange(r, z0 + 40 + hz, z1 - 40 - hz);
			/* not across a doorway: no portal of the room along its wall within reach */
			{
				int q, blocked = 0;

				for (q = 0; q < lg->numportals && !blocked; q++) {
					const LgPortal *pt = &lg->portals[q];
					int32_t px0, px1, pz0, pz1;

					if (pt->a != ai && pt->b != ai) {
						continue;
					}
					px0 = lgWorldX(lg, pt->x0) - 110; px1 = lgWorldX(lg, pt->x1) + 110;
					pz0 = lgWorldZ(lg, pt->z0) - 110; pz1 = lgWorldZ(lg, pt->z1) + 110;
					blocked = x + hx > px0 && x - hx < px1 && z + hz > pz0 && z - hz < pz1;
				}
				if (blocked) {
					continue;
				}
			}
			if (lgTryProp(lg, ai, prop, x, z, turn, 35, LG_COVER_PADGAP, LG_COVER_GAP)) {
				want--;
			}
		}
	}
}

/* The layout's set piece: one, on open ground it fits on (the first of its
 * pool, from a random start, that fits anywhere) */
static void lgPlaceSetPiece(LgLayout *lg, LgRng *r)
{
	const int8_t *pool = g_LgSetPieces[lg->theme.layout];
	int n = 0, k, first;

	while (n < 4 && pool[n] >= 0) {
		n++;
	}
	if (n == 0) {
		return;
	}
	first = lgRange(r, 0, n - 1);
	for (k = 0; k < n; k++) {
		const int prop = pool[(first + k) % n];
		const LgPropDef *d = &g_LgProps[prop];
		int ai, tries;

		for (ai = 0; ai < lg->numareas; ai++) {
			const LgArea *a = &lg->areas[ai];

			if (!a->open || a->layer || a->ramp || LG_NOWALK(a) || a->special || ai == lg->startarea) {
				continue;
			}
			for (tries = 0; tries < 6; tries++) {
				const int turn = lgRange(r, 0, 3);
				const int hx = (turn & 1) ? d->halfl : d->halfw, hz = (turn & 1) ? d->halfw : d->halfl;

				if (a->w * LG_CELL < 2 * hx + 240 || a->d * LG_CELL < 2 * hz + 240) {
					continue;
				}
				if (lgTryProp(lg, ai, prop, lgWorldX(lg, a->x) + lgRange(r, 120 + hx, a->w * LG_CELL - 120 - hx),
						lgWorldZ(lg, a->z) + lgRange(r, 120 + hz, a->d * LG_CELL - 120 - hz), turn, 100, 120, 120)) {
					return;
				}
			}
		}
	}
}

/* Low cover in the hub: crates and low walls scattered over the floor on a
 * loose grid, and parapets along the balconies' edges with gaps in them */
static void lgPlaceCover(LgLayout *lg, LgRng *r)
{
	int ai, i;

	lgPlaceSetPiece(lg, r);   /* (first: the big piece needs the open ground) */

	for (ai = 0; ai < lg->numareas; ai++) {
		const LgArea *a = &lg->areas[ai];
		int gx, gz;

		/* the hub's floor; in a lab every room but the start (benches and
		 * partitions to fight round) */
		if (a->ramp || a->kind != LG_AREA_ROOM || LG_NOWALK(a) || a->layer || a->special == LG_SPECIAL_FIREESCAPE
				|| (ai != a->room && !((lg->theme.layout == LG_LAYOUT_STREET || lg->theme.layout == LG_LAYOUT_RAVINE || lg->theme.layout == LG_LAYOUT_VILLA
					|| LG_LAYOUT_SKYLIKE(lg->theme.layout) || LG_LAYOUT_FLATBUILT(lg->theme.layout))   /* (and the rooftops' roofs, 2026-11-16) */
					&& (a->flags & LG_AREAFLAG_HUB)))   /* every flat piece of a street, a ravine's ground (not its pit or bridges) */
				|| !((a->flags & LG_AREAFLAG_HUB) || (lg->theme.layout != LG_LAYOUT_HUB && ai != lg->startarea))) {
			continue;
		}
		/* a street: cars parked at the kerb first, one or two a piece, nose to
		 * tail along it, well clear of doorways (the cover below then keeps
		 * clear of them) */
		if (lg->theme.layout == LG_LAYOUT_STREET && (a->flags & LG_AREAFLAG_HUB)) {
			const int alongx = a->w > a->d;
			const int len = (alongx ? a->w : a->d) * LG_CELL, wid = (alongx ? a->d : a->w) * LG_CELL;
			int want = lgRange(r, 1, len >= 1600 ? 2 : 1), tries;

			for (tries = 0; tries < 80 && want > 0 && lg->numobst < LG_MAXOBST && len >= 900; tries++) {
				/* (and the game's other cars, 2026-11-16) */
				static const int8_t cars[] = { LG_PROP_TAXI, LG_PROP_TAXI, LG_PROP_TAXI, LG_PROP_POLICE, LG_PROP_POLICE, LG_PROP_LIMO, LG_PROP_HOVCAB, LG_PROP_HOVBIKE };
				const int model = cars[lgRange(r, 0, (int)sizeof(cars) - 1)];
				const LgPropDef *d = &g_LgProps[model];
				const int far = lgRand(r) & 1, turn = (alongx ? 1 : 0) + 2 * (int)(lgRand(r) & 1);
				const int along = lgRange(r, 150 + d->halfl, len - 150 - d->halfl);
				const int across = far ? wid - 40 - d->halfw : 40 + d->halfw;
				const int32_t x = lgWorldX(lg, a->x) + (alongx ? along : across);
				const int32_t z = lgWorldZ(lg, a->z) + (alongx ? across : along);
				const int hx = alongx ? d->halfl : d->halfw, hz = alongx ? d->halfw : d->halfl;

				/* guard pads in its way move out to the street side of it; nav pads
				 * (doorways, the middle) are never moved: the car goes elsewhere */
				int q, blocked = 0;

				for (q = 0; q < lg->numpads && !blocked; q++) {
					const LgPad *p = &lg->pads[q];

					blocked = p->area == ai && p->kind != LG_PAD_GUARD
						&& lgAbs(p->x - x) < hx + LG_COVER_PADGAP && lgAbs(p->z - z) < hz + LG_COVER_PADGAP;
				}
				/* and not under a bridge, nor where whoever drops off a balcony lands:
				 * the row of street in front of a drop-down (2026-11-15). Under a
				 * balcony's railed stretch a car fits (166 high, 220 under it) */
				{
					const int gx0 = (x - hx - 20 - lg->originx) / LG_CELL, gx1 = (x + hx + 20 - lg->originx) / LG_CELL;
					const int gz0 = (z - hz - 20 - lg->originz) / LG_CELL, gz1 = (z + hz + 20 - lg->originz) / LG_CELL;
					int qx, qz, pq;

					for (qz = gz0; qz <= gz1 && !blocked; qz++) {
						for (qx = gx0; qx <= gx1 && !blocked; qx++) {
							const int up = (qx >= 0 && qz >= 0 && qx < lg->gw && qz < lg->gd) ? lg->cells2[qz][qx] - 1 : -1;

							blocked = up >= 0 && lg->areas[up].special == LG_SPECIAL_BRIDGE;
						}
					}
					for (pq = 0; pq < lg->numportals && !blocked; pq++) {
						const LgPortal *pt = &lg->portals[pq];
						const int balside = lg->areas[pt->a].special == LG_SPECIAL_BALCONY ? 0 : lg->areas[pt->b].special == LG_SPECIAL_BALCONY ? 1 : -1;
						int lx0, lx1, lz0, lz1;

						if (balside < 0 || !pt->drop || !pt->norail || (balside ? pt->a : pt->b) != ai) {
							continue;
						}
						/* the landing cells: the row on the street's side of the line (a car
						 * at the kerb reaches across it) */
						if (pt->x0 == pt->x1) {
							lx0 = lx1 = balside ? pt->x0 - 1 : pt->x0;
							lz0 = pt->z0; lz1 = pt->z1 - 1;
						} else {
							lz0 = lz1 = balside ? pt->z0 - 1 : pt->z0;
							lx0 = pt->x0; lx1 = pt->x1 - 1;
						}
						blocked = gx0 <= lx1 && lx0 <= gx1 && gz0 <= lz1 && lz0 <= gz1;
					}
				}
				if (!blocked && wid >= 2 * d->halfw + 40 + 160 && lgCoverOk(lg, ai, x, z, hx, hz, 30, -100000)) {
					LgObstacle *o;
					const int clear = far ? wid - 40 - 2 * d->halfw - 80 : 40 + 2 * d->halfw + 80;   /* across the street: 80 clear of the car */

					for (q = 0; q < lg->numpads; q++) {
						LgPad *p = &lg->pads[q];

						if (p->area == ai && p->kind == LG_PAD_GUARD
								&& lgAbs(p->x - x) < hx + LG_COVER_PADGAP && lgAbs(p->z - z) < hz + LG_COVER_PADGAP) {
							if (alongx) {
								p->z = lgWorldZ(lg, a->z) + clear;
							} else {
								p->x = lgWorldX(lg, a->x) + clear;
							}
						}
					}

					lgAddCover(lg, ai, x, z, hx, hz, d->height);
					o = &lg->obst[lg->numobst - 1];
					o->crate = 1;
					o->model = (uint8_t)model;
					o->turn = (uint8_t)turn;
					o->scale = d->scale;
					want--;
				}
			}
		}
		/* the floor: a piece every 2-3 cells, now and then left out */
		for (gz = a->z + 2; gz < a->z + a->d - 2; gz += lgRange(r, 2, 3)) {
			for (gx = a->x + 2; gx < a->x + a->w - 2; gx += lgRange(r, 2, 3)) {
				const int32_t x = lgWorldX(lg, gx) + lgRange(r, 0, LG_CELL), z = lgWorldZ(lg, gz) + lgRange(r, 0, LG_CELL);
				const int kind = lgRange(r, 0, 99);
				int hx, hz, tries;
				const int crate = kind >= 20 && kind < 65;

				if (kind < 20) {
					continue;
				}
				/* a villa (2026-11-16): no crates or low walls; outdoors a potted
				 * plant now and then, among its pools, beds and hedges */
				if (LG_LAYOUT_WOODSLIKE(lg->theme.layout)) {
					continue;   /* (Hollow Creek: dressed by hand; the backwoods, by lgRdDress) */
				}
				if (lg->theme.layout == LG_LAYOUT_VILLA) {
					if (a->open && kind < 50) {
						lgTryProp(lg, ai, lgPoolPick(g_LgCoverPools[LG_LAYOUT_VILLA], 8, r), x, z, lgRange(r, 0, 3), 150, LG_COVER_PADGAP, LG_COVER_GAP);
					}
					continue;
				}
				/* (the layout's own props in among the crates, 2026-11-16) */
				if (crate) {
					const int prop = lgPoolPick(g_LgCoverPools[lg->theme.layout], 8, r);

					if (prop >= LG_CRATE_MODELS) {
						for (tries = 0; tries < 3; tries++) {
							if (lgTryProp(lg, ai, prop, x, z, lgRange(r, 0, 3), 150, LG_COVER_PADGAP, LG_COVER_GAP)) {
								break;
							}
						}
						continue;
					}
				}
				for (tries = 0; tries < 3 && lg->numobst < LG_MAXOBST; tries++) {
					if (crate) {                        /* a crate: the game's own, 90 / 100 / 110 on a side */
						hx = hz = 45 + 5 * lgRange(r, 0, 2);
					} else if (lgRand(r) & 1) {         /* a low wall along x */
						hx = lgRange(r, 90, 150); hz = 15;
					} else {                            /* along z */
						hx = 15; hz = lgRange(r, 90, 150);
					}
					if (lgCoverOk(lg, ai, x, z, hx, hz, 150, LG_COVER_PADGAP)) {
						lgAddCover(lg, ai, x, z, hx, hz, crate ? 2 * hx : LG_COVER_HEIGHT);

						if (crate) {
							LgObstacle *o = &lg->obst[lg->numobst - 1];

							o->crate = 1;
							o->model = (uint8_t)lgRange(r, 0, LG_CRATE_MODELS - 1);
							o->turn = (uint8_t)lgRange(r, 0, 3);
							o->scale = (int16_t)(hx * 256 / (LG_CRATE_SIZE / 2));
						}
						break;
					}
				}
			}
		}
		/* a lab's room never has less than two pieces to fight round, if
		 * two crates fit anywhere on its floor a cell in from the walls */
		if (lg->theme.layout != LG_LAYOUT_HUB && lg->theme.layout != LG_LAYOUT_VILLA && !(a->flags & LG_AREAFLAG_HUB)) {
			int have = 0, tries;

			for (i = 0; i < lg->numobst; i++) {
				have += lg->obst[i].area == ai && lg->obst[i].height > 0;
			}
			for (tries = 0; tries < 40 && have < 2 && lg->numobst < LG_MAXOBST; tries++) {
				const int hx = 45 + 5 * lgRange(r, 0, 2);
				const int32_t x = lgWorldX(lg, a->x) + lgRange(r, 100 + hx, a->w * LG_CELL - 100 - hx);
				const int32_t z = lgWorldZ(lg, a->z) + lgRange(r, 100 + hx, a->d * LG_CELL - 100 - hx);

				if (a->w >= 4 && a->d >= 4 && lgCoverOk(lg, ai, x, z, hx, hx, 100, LG_COVER_PADGAP)) {
					LgObstacle *o;

					lgAddCover(lg, ai, x, z, hx, hx, 2 * hx);
					o = &lg->obst[lg->numobst - 1];
					o->crate = 1;
					o->model = (uint8_t)lgRange(r, 0, LG_CRATE_MODELS - 1);
					o->turn = (uint8_t)lgRange(r, 0, 3);
					o->scale = (int16_t)(hx * 256 / (LG_CRATE_SIZE / 2));
					have++;
				}
			}
		}
	}

	/* the parapets: along each ledge's high side, a little in from the edge */
	for (i = 0; i < lg->numportals && lg->numobst < LG_MAXOBST; i++) {
		const LgPortal *p = &lg->portals[i];
		const int32_t x0 = lgWorldX(lg, p->x0), x1 = lgWorldX(lg, p->x1), z0 = lgWorldZ(lg, p->z0), z1 = lgWorldZ(lg, p->z1);
		const int32_t mx = (x0 + x1) / 2, mz = (z0 + z1) / 2;
		int hi, alongx, len, pos;
		int32_t inx, inz;

		if (!p->drop || p->norail) {
			continue;
		}
		hi = modLevelGenFloorAt(lg, p->a, mx, mz) > modLevelGenFloorAt(lg, p->b, mx, mz) ? p->a : p->b;
		if (lg->areas[hi].ramp) {
			continue;   /* not along a slope */
		}
		alongx = p->z0 == p->z1;
		len = alongx ? x1 - x0 : z1 - z0;
		/* the high side: b is on the high coordinate side of the line */
		inx = alongx ? 0 : (hi == p->b ? 25 : -25);
		inz = alongx ? (hi == p->b ? 25 : -25) : 0;
		/* a balcony's railed piece is short (two cells): one rail along it, ends close in */
		/* (a standing-out balcony's end, one cell: close to both corners) */
		const int endgap = lg->areas[hi].special == LG_SPECIAL_BALCONY ? (len <= LG_CELL ? 10 : 40) : 60;   /* the load test draws lines 30 past its ends */
		const int minhalf = lg->areas[hi].special == LG_SPECIAL_BALCONY ? 30 : 40;
		const int e0 = endgap, e1 = endgap;

		for (pos = e0; pos < len - e1 && lg->numobst < LG_MAXOBST; ) {
			const int seglen = lg->areas[hi].special == LG_SPECIAL_BALCONY ? len : lgRange(r, 150, 260);
			const int gap = lgRange(r, 90, 160);
			const int half = (pos + seglen > len - e1 ? len - e1 - pos : seglen) / 2;

			if (half >= minhalf) {
				const int32_t cx = alongx ? x0 + pos + half : x0 + inx, cz = alongx ? z0 + inz : z0 + pos + half;
				const int hx = alongx ? half : 10, hz = alongx ? 10 : half;

				/* thin: guards may stand close; a balcony's end railing meets its front one at the corner */
				if (lgCoverOkGap(lg, hi, cx, cz, hx, hz, 10, 35, lg->areas[hi].special == LG_SPECIAL_BALCONY ? 0 : LG_COVER_GAP)) {
					lgAddCover(lg, hi, cx, cz, hx, hz, LG_RAIL_HEIGHT);
					lg->obst[lg->numobst - 1].rail = 1;
				}
			}
			pos += 2 * half + gap;
		}
	}

	lgPlaceFurniture(lg, r);
}

/* Pillars in the rooms, from a stream of their own so that the layout
 * underneath a seed is the one it always was. The start room is left alone. */
static void lgPlaceObstacles(LgLayout *lg)
{
	LgRng r;
	int ai;

	r.s = lg->seed ^ 0x0b57ac1eu ^ ((uint32_t)lg->attempt * 0x9e3779b1u);
	lg->numobst = 0;

	for (ai = 0; ai < lg->numareas; ai++) {
		const LgArea *a = &lg->areas[ai];
		int per, want, tries;

		if (a->kind != LG_AREA_ROOM || a->open || ai == lg->startarea || lgIsLiftShaft(lg, ai) || (a->flags & (LG_AREAFLAG_START | LG_AREAFLAG_HUB))) {
			continue;   /* the hub gets low cover instead, its sightlines kept */
		}
		switch (a->roomtype) {
		case LG_ROOM_STORAGE: per = 10; break;
		case LG_ROOM_HALL:    per = 14; break;
		case LG_ROOM_LAB:     per = 20; break;
		default:              per = 30; break;
		}
		want = a->w * a->d / per;
		if (want > 12) {
			want = 12;
		}
		for (tries = 0; tries < want * 8 && want > 0 && lg->numobst < LG_MAXOBST; tries++) {
			const int cx = lgRange(&r, a->x + 2, a->x + a->w - 3), cz = lgRange(&r, a->z + 2, a->z + a->d - 3);

			if (lgPillarOk(lg, ai, cx, cz)) {
				LgObstacle *o = &lg->obst[lg->numobst++];

				memset(o, 0, sizeof(*o));
				o->cx = (int16_t)cx;
				o->cz = (int16_t)cz;
				o->area = (int16_t)ai;
				o->x = lgWorldX(lg, cx) + LG_CELL / 2;
				o->z = lgWorldZ(lg, cz) + LG_CELL / 2;
				o->halfx = o->halfz = LG_PILLAR_HALF;
				if (--want == 0) {
					break;
				}
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* Build                                                                */

/* The hub layout: a long hall with terraces, offshoot rooms round it */
static int lgTryHub(LgLayout *lg, const LgParams *p, LgRng *r, char *err, int errlen)
{
	int16_t attach[LG_MAXROOMS];

	if (!lgPlaceHub(lg, r, attach)) {
		lgSetErr(err, errlen, "only %d rooms placed", lg->numrooms, 0);
		return 0;
	}

	(void)p;

	return lgConnect(lg, r, attach, err, errlen)
		&& lgAddWings(lg, r, err, errlen)
		&& lgAddMezzanines(lg, err, errlen)
		&& lgFindPortals(lg, r, err, errlen)
		&& lgMezzPortals(lg, err, errlen)
		&& lgRaiseWings(lg, r, err, errlen)
		&& lgAreaGraph(lg, r, err, errlen)
		&& lgBuildNav(lg, err, errlen)
		&& lgBuildSpawns(lg, r, err, errlen)
		&& (lgPlaceObstacles(lg), lgPlaceCover(lg, r), 1)
		&& modLevelGenValidate(lg, err, errlen);
}

/* Doors across the lab's halls, so that nobody sees or shoots down the whole
 * length of one: wherever a hall runs straight through a crossing, a door on
 * one side of the crossing (on a flat side: never at the end of a ramp). */
static void lgLabHallDoors(LgLayout *lg, LgRng *r)
{
	int ai, axis, i;

	for (ai = lg->numrooms; ai < lg->numareas; ai++) {
		const LgArea *a = &lg->areas[ai];

		if (a->kind != LG_AREA_CORRIDOR || a->ramp || a->w > 3 || a->d > 3) {
			continue;   /* not a crossing */
		}
		for (axis = 0; axis < 2; axis++) {
			int side[2] = { -1, -1 }, flat[2] = { 0, 0 }, pick;

			for (i = 0; i < lg->numportals; i++) {
				const LgPortal *p = &lg->portals[i];
				const int o = p->a == ai ? p->b : p->b == ai ? p->a : -1;
				const int alongz = p->x0 == p->x1;   /* the opening runs along z: it is crossed along x */

				if (o < 0 || p->drop || lg->areas[o].kind != LG_AREA_CORRIDOR || alongz != (axis == 0)) {
					continue;
				}
				if (axis == 0) {
					pick = p->x0 == a->x ? 0 : 1;
				} else {
					pick = p->z0 == a->z ? 0 : 1;
				}
				side[pick] = i;
				flat[pick] = !lg->areas[o].ramp && (axis == 0 ? p->z1 - p->z0 : p->x1 - p->x0) <= 3;
			}
			if (side[0] < 0 || side[1] < 0 || (!flat[0] && !flat[1])) {
				continue;
			}
			pick = flat[0] && flat[1] ? (int)(lgRand(r) & 1) : flat[0] ? 0 : 1;
			lg->portals[side[pick]].door = 1;
			lg->portals[side[pick]].doorway = 1;
		}
	}
}

/* The underground lab: a lattice of halls, rooms in and round it */
static int lgTryLab(LgLayout *lg, const LgParams *p, LgRng *r, char *err, int errlen)
{
	(void)p;

	if (!lgPlaceLab(lg, r)) {
		lgSetErr(err, errlen, "the lab did not fit (%d rooms)", lg->numrooms, 0);
		return 0;
	}

	return lgAddWings(lg, r, err, errlen)
		&& lgAddMezzanines(lg, err, errlen)
		&& lgFindPortals(lg, r, err, errlen)
		&& lgMezzPortals(lg, err, errlen)
		&& (lgLabHallDoors(lg, r), 1)
		&& lgRaiseWings(lg, r, err, errlen)
		&& lgAreaGraph(lg, r, err, errlen)
		&& lgBuildNav(lg, err, errlen)
		&& lgBuildSpawns(lg, r, err, errlen)
		&& (lgPlaceObstacles(lg), lgPlaceCover(lg, r), 1)
		&& modLevelGenValidate(lg, err, errlen);
}

/* Balconies (2026-11-14): where a building's mezzanine stands behind its
 * porch, a layer 1 area over the porch, outdoors, level with the mezzanine: a
 * doorway from the upper floor onto it, and a ledge over the street: a
 * drop-down with no railing at one end, railed the rest.
 * Bridges (2026-11-14): two balconies facing each other across the street,
 * at the same height and along it by two cells or more, are joined by a
 * bridge two cells wide over the street: a layer 1 area, outdoors, with
 * railings (and their gaps) along both sides. A bridged balcony's edge is the
 * bridge's end, and the rest of it is open: drop-downs. So both upper floors
 * have a second way up: the other building's ramp. */
typedef struct {
	int bi, zi, sarea, sside;
	LgArea c;          /* the porch under it */
	LgArea r;          /* the balcony: the porch's cells, and one row out over the street if it stands out */
	int proj;          /* 1: it stands out over the street */
	int bridge, k0;    /* the bridge's area and where it starts along the edge, or -1 */
} LgStBal;

/* an opening on the upper storey: along z (the line x = line) or along x */
static int lgStPortal2(LgLayout *lg, int alongz, int line, int from, int to, int lowarea, int higharea, int drop, int norail,
		char *err, int errlen)
{
	LgPortal *p;
	int q;

	if (to <= from) {
		return 1;
	}
	if (lg->numportals >= LG_MAXPORTALS) {
		lgSetErr(err, errlen, "too many portals (%d)", lg->numportals, 0);
		return 0;
	}
	p = &lg->portals[lg->numportals++];
	memset(p, 0, sizeof(*p));
	if (alongz) {
		p->x0 = p->x1 = (int16_t)line;
		p->z0 = (int16_t)from; p->z1 = (int16_t)to;
	} else {
		p->z0 = p->z1 = (int16_t)line;
		p->x0 = (int16_t)from; p->x1 = (int16_t)to;
	}
	p->a = (int16_t)lowarea;
	p->b = (int16_t)higharea;
	p->drop = (uint8_t)drop;
	p->norail = (uint8_t)norail;
	p->pada = p->padb = -1;
	/* (open in the grid of each area's layer over the ground: a ground area's
	 * own edge stays as it was; both areas' if both are upper ones, 2026-11-16) */
	{
		const int la = lg->areas[lowarea].layer, lb = lg->areas[higharea].layer;
		int l;

		for (l = 1; l < LG_MAXLAYERS; l++) {
			if (l != la && l != lb && !(l == 1 && !la && !lb)) {
				continue;
			}
			for (q = from; q < to; q++) {
				if (alongz) {
					LG_OPENS(lg, l)[q][line - 1] |= LG_OPEN_XP;
				} else {
					LG_OPENS(lg, l)[line - 1][q] |= LG_OPEN_ZP;
				}
			}
		}
	}
	return 1;
}

/* the cell beside the rectangle on its side (0 -x, 1 +x, 2 -z, 3 +z), i along it */
static void lgStSideCell(const LgArea *c, int side, int i, int *x, int *z)
{
	*x = side == 0 ? c->x - 1 : side == 1 ? c->x + c->w : c->x + i;
	*z = side == 2 ? c->z - 1 : side == 3 ? c->z + c->d : c->z + i;
}

static int lgStBalconies(LgLayout *lg, char *err, int errlen)
{
	LgStBal bal[LG_MAXROOMS];
	const int numareas = lg->numareas;
	int nbal = 0, ci, i, j, k;

	/* the balconies, each with its doorway from the upper floor */
	for (ci = lg->numrooms; ci < numareas && nbal < LG_MAXROOMS; ci++) {
		const LgArea c = lg->areas[ci];
		int side, zi = -1, mside = -1, run0 = 0, run1 = 0, sside, sarea = -1, ok, len, proj = 0;

		if (c.kind != LG_AREA_CORRIDOR || c.open || c.special || c.layer || c.ramp
				|| c.ceil - c.floor != LG_MEZZ_UNDER || lg->cells2[c.z][c.x]) {
			continue;   /* not a porch */
		}
		/* the mezzanine behind it: the longest run of one mezzanine's cells along a side */
		for (side = 0; side < 4; side++) {
			int start = -1, zarea = -1;

			len = side < 2 ? c.d : c.w;
			for (i = 0; i <= len; i++) {
				int x, z, up = -1;

				if (i < len) {
					lgStSideCell(&c, side, i, &x, &z);
					up = (x < 0 || z < 0 || x >= lg->gw || z >= lg->gd) ? -1 : lg->cells2[z][x] - 1;
				}
				if (up >= 0 && (lg->areas[up].flags & LG_AREAFLAG_MEZZ) && lg->areas[up].floor == c.ceil + LG_MEZZ_SLAB
						&& (zarea < 0 || zarea == up)) {
					zarea = up;
					if (start < 0) start = i;
				} else if (start >= 0) {
					if (i - start >= 2 && i - start > run1 - run0) {
						zi = zarea; mside = side; run0 = start; run1 = i;
					}
					start = -1;
				}
			}
		}
		if (zi < 0) {
			continue;
		}
		if (run1 > run0 + 3) {
			run1 = run0 + 3;
		}
		/* the street along the whole of the opposite side */
		sside = mside ^ 1;
		len = sside < 2 ? c.d : c.w;
		ok = 1;
		for (i = 0; i < len && ok; i++) {
			int x, z, gr;

			lgStSideCell(&c, sside, i, &x, &z);
			gr = (x < 0 || z < 0 || x >= lg->gw || z >= lg->gd) ? -1 : lg->cells[z][x] - 1;
			ok = gr >= 0 && (lg->areas[gr].flags & LG_AREAFLAG_HUB) && lg->areas[gr].open && !lg->areas[gr].ramp
				&& lg->areas[gr].floor == c.floor && (sarea < 0 || sarea == gr);
			sarea = gr;
		}
		if (!ok || sarea < 0) {
			continue;
		}
		/* out over the street by a cell (2026-11-15, the user's request:
		 * "balconies that stick out over the street"): where the row in front of
		 * the porch, and the street a cell either end of it, are clear street
		 * with more street beyond to drop onto */
		{
			const int dx = sside == 0 ? -1 : sside == 1 ? 1 : 0, dz = sside == 2 ? -1 : sside == 3 ? 1 : 0;

			proj = 1;
			for (i = -1; i <= len && proj; i++) {
				int x, z, two, one;

				if (sside < 2) {
					x = sside == 0 ? c.x - 1 : c.x + c.w; z = c.z + i;
				} else {
					x = c.x + i; z = sside == 2 ? c.z - 1 : c.z + c.d;
				}
				if (x + 2 * dx < 0 || z + 2 * dz < 0 || x + 2 * dx >= lg->gw || z + 2 * dz >= lg->gd || x < 0 || z < 0) {
					proj = 0;
					break;
				}
				one = lg->cells[z][x] - 1;
				two = lg->cells[z + dz][x + dx] - 1;
				proj = one == sarea && lg->cells2[z][x] == 0 && (i < 0 || i >= len || two == sarea);
			}
		}
		if (lg->numareas >= LG_MAXAREAS) {
			lgSetErr(err, errlen, "too many areas for a balcony (%d)", lg->numareas, 0);
			return 0;
		}
		{
			const int bi = lg->numareas++;
			LgArea *b = &lg->areas[bi];
			const int alongz = mside < 2;
			const int line = mside == 0 ? c.x : mside == 1 ? c.x + c.w : mside == 2 ? c.z : c.z + c.d;
			const int base = alongz ? c.z : c.x;
			const int low = mside == 0 || mside == 2;

			memset(b, 0, sizeof(*b));
			b->x = c.x; b->z = c.z; b->w = c.w; b->d = c.d;
			if (proj) {
				switch (sside) {
				case 0: b->x--; b->w++; break;
				case 1: b->w++; break;
				case 2: b->z--; b->d++; break;
				default: b->d++; break;
				}
			}
			b->kind = LG_AREA_ROOM;
			b->roomtype = lg->areas[zi].roomtype;
			b->theme = LG_STREET_THEME;
			b->room = lg->areas[zi].room;
			b->floor = b->floorlo = (int16_t)(c.ceil + LG_MEZZ_SLAB);
			b->ceil = lg->areas[sarea].ceil;
			b->layer = 1;
			b->open = 1;
			b->special = LG_SPECIAL_BALCONY;
			b->dist = -1;
			b->centrepad = -1;
			lgMezzFill(lg, b, bi);
			if (!lgStPortal2(lg, alongz, line, base + run0, base + run1, low ? zi : bi, low ? bi : zi, 0, 0, err, errlen)) {
				return 0;
			}
			/* a door on it (2026-11-15, the user's request) */
			lg->portals[lg->numportals - 1].doorway = 1;
			lg->portals[lg->numportals - 1].door = 1;
			bal[nbal].bi = bi;
			bal[nbal].zi = zi;
			bal[nbal].sarea = sarea;
			bal[nbal].sside = sside;
			bal[nbal].c = c;
			bal[nbal].r = *b;
			bal[nbal].proj = proj;
			bal[nbal].bridge = -1;
			bal[nbal].k0 = 0;
			nbal++;
		}
	}

	/* bridges between balconies facing each other over the same piece of street */
	for (i = 0; i < nbal; i++) {
		for (j = i + 1; j < nbal && bal[i].bridge < 0; j++) {
			const LgStBal *a = &bal[i], *o = &bal[j];
			const int acrossx = a->sside < 2;   /* the street is crossed along x */
			int lo, hi, k0, x0, x1, z0, z1, x, z, clear = 1;

			if (o->bridge >= 0 || o->sarea != a->sarea || (a->sside ^ o->sside) != 1
					|| lg->areas[a->bi].floor != lg->areas[o->bi].floor) {
				continue;
			}
			lo = acrossx ? (a->c.z > o->c.z ? a->c.z : o->c.z) : (a->c.x > o->c.x ? a->c.x : o->c.x);
			hi = acrossx ? (a->c.z + a->c.d < o->c.z + o->c.d ? a->c.z + a->c.d : o->c.z + o->c.d)
				: (a->c.x + a->c.w < o->c.x + o->c.w ? a->c.x + a->c.w : o->c.x + o->c.w);
			if (hi - lo < 2) {
				continue;
			}
			k0 = lo + (hi - lo - 2) / 2;
			if (acrossx) {
				const LgArea *w = a->sside == 1 ? &a->r : &o->r, *e = a->sside == 1 ? &o->r : &a->r;

				x0 = w->x + w->w; x1 = e->x; z0 = k0; z1 = k0 + 2;
			} else {
				const LgArea *n = a->sside == 3 ? &a->r : &o->r, *f = a->sside == 3 ? &o->r : &a->r;

				z0 = n->z + n->d; z1 = f->z; x0 = k0; x1 = k0 + 2;
			}
			if (x1 - x0 < 2 || z1 - z0 < 2) {
				continue;
			}
			for (z = z0; z < z1 && clear; z++) {
				for (x = x0; x < x1 && clear; x++) {
					clear = lg->cells[z][x] - 1 == a->sarea && lg->cells2[z][x] == 0;
				}
			}
			if (!clear) {
				continue;
			}
			if (lg->numareas >= LG_MAXAREAS) {
				lgSetErr(err, errlen, "too many areas for a bridge (%d)", lg->numareas, 0);
				return 0;
			}
			{
				const int gi = lg->numareas++;
				LgArea *g = &lg->areas[gi];

				memset(g, 0, sizeof(*g));
				g->x = (int16_t)x0; g->z = (int16_t)z0; g->w = (int16_t)(x1 - x0); g->d = (int16_t)(z1 - z0);
				g->kind = LG_AREA_ROOM;
				g->roomtype = lg->areas[a->sarea].roomtype;
				g->theme = LG_STREET_THEME;
				g->room = lg->areas[a->sarea].room;
				g->floor = g->floorlo = lg->areas[a->bi].floor;
				g->ceil = lg->areas[a->sarea].ceil;
				g->layer = 1;
				g->open = 1;
				g->special = LG_SPECIAL_BRIDGE;
				g->dist = -1;
				g->centrepad = -1;
				lgMezzFill(lg, g, gi);
				bal[i].bridge = gi; bal[i].k0 = k0;
				bal[j].bridge = gi; bal[j].k0 = k0;
				/* its sides over the street: ledges, railed (the railings' gaps are drop-downs) */
				if (acrossx) {
					if (!lgStPortal2(lg, 0, z0, x0, x1, a->sarea, gi, 1, 0, err, errlen)
							|| !lgStPortal2(lg, 0, z1, x0, x1, gi, a->sarea, 1, 0, err, errlen)) {
						return 0;
					}
				} else {
					if (!lgStPortal2(lg, 1, x0, z0, z1, a->sarea, gi, 1, 0, err, errlen)
							|| !lgStPortal2(lg, 1, x1, z0, z1, gi, a->sarea, 1, 0, err, errlen)) {
						return 0;
					}
				}
			}
		}
	}

	/* each balcony's edge over the street: the bridge's end, and drop-downs
	 * either side of it; or, with no bridge, a drop-down at one end (a cell
	 * on a balcony three wide, two on a wider one) and a railing the rest */
	for (i = 0; i < nbal; i++) {
		const LgStBal *a = &bal[i];
		const int sside = a->sside, alongz = sside < 2;
		const int line = sside == 0 ? a->r.x : sside == 1 ? a->r.x + a->r.w : sside == 2 ? a->r.z : a->r.z + a->r.d;
		const int base = alongz ? a->c.z : a->c.x, blen = alongz ? a->c.d : a->c.w;
		const int low = sside == 0 || sside == 2;   /* the street is on the low-coordinate side */
		const int bi = a->bi;
		int seg[3][4], ns = 0;   /* from, to, other area, kind: 0 drop-down, 1 railed, 2 level */

		if (a->bridge >= 0) {
			seg[ns][0] = base; seg[ns][1] = a->k0; seg[ns][2] = a->sarea; seg[ns++][3] = 0;
			seg[ns][0] = a->k0; seg[ns][1] = a->k0 + 2; seg[ns][2] = a->bridge; seg[ns++][3] = 2;
			seg[ns][0] = a->k0 + 2; seg[ns][1] = base + blen; seg[ns][2] = a->sarea; seg[ns++][3] = 0;
		} else {
			const int gap = blen <= 3 ? 1 : 2, atlow = ((a->c.x + a->c.z) & 1) == 0;

			seg[ns][0] = atlow ? base : base + blen - gap; seg[ns][1] = atlow ? base + gap : base + blen;
			seg[ns][2] = a->sarea; seg[ns++][3] = 0;
			seg[ns][0] = atlow ? base + gap : base; seg[ns][1] = atlow ? base + blen : base + blen - gap;
			seg[ns][2] = a->sarea; seg[ns++][3] = 1;
		}
		for (k = 0; k < ns; k++) {
			const int other = seg[k][2];

			if (!lgStPortal2(lg, alongz, line, seg[k][0], seg[k][1], low ? other : bi, low ? bi : other,
					seg[k][3] != 2, seg[k][3] == 0, err, errlen)) {
				return 0;
			}
		}
		/* standing out: the row's two ends over the street, railed; but open
		 * (a drop-down too) beside a drop-down in the front, whose one cell an
		 * end railing narrowed so that the player caught on it */
		if (a->proj) {
			const int row = sside == 0 ? a->r.x : sside == 1 ? a->r.x + a->r.w - 1 : sside == 2 ? a->r.z : a->r.z + a->r.d - 1;
			int openlo = 0, openhi = 0;

			for (k = 0; k < ns; k++) {
				openlo |= seg[k][0] == base && seg[k][3] == 0;
				openhi |= seg[k][1] == base + blen && seg[k][3] == 0;
			}
			if (!lgStPortal2(lg, !alongz, base, row, row + 1, a->sarea, bi, 1, openlo, err, errlen)
					|| !lgStPortal2(lg, !alongz, base + blen, row, row + 1, bi, a->sarea, 1, openhi, err, errlen)) {
				return 0;
			}
		}
	}
	return 1;
}

/* Upper-storey windows (2026-11-15, the user's request: "upper-storey
 * windows"): over every ground-floor window of a building whose front
 * mezzanine stands behind it along the window's whole width, a second window
 * through the wall at the upper floor: a layer 1 corridor-kind area on the
 * same cells, its sill LG_WIN_SILL and its head LG_WIN_HEAD over the
 * mezzanine's floor, glass across it, openings (ledges nobody crosses) to the
 * mezzanine and to the street. Stacked over the ground window so the upper
 * storey's cells stand over ground ones, as every layer 1 area does. */
static int lgStUpperWindows(LgLayout *lg, char *err, int errlen)
{
	const int numareas = lg->numareas;
	int wi;

	for (wi = 0; wi < numareas; wi++) {
		const LgArea w = lg->areas[wi];
		int q, room = -1, street = -1, rline = -1, sline = -1, alongz = -1, mezz = -1, ok = 1, x, z, i;

		if (w.special != LG_SPECIAL_WINDOW || w.layer) {
			continue;
		}
		/* its two openings: the building's room on one side, the street on the other */
		for (q = 0; q < lg->numportals; q++) {
			const LgPortal *pt = &lg->portals[q];
			const int o = pt->a == wi ? pt->b : pt->b == wi ? pt->a : -1;

			if (o < 0) {
				continue;
			}
			alongz = pt->x0 == pt->x1;
			if ((lg->areas[o].flags & LG_AREAFLAG_HUB) && lg->areas[o].open) {
				street = o; sline = alongz ? pt->x0 : pt->z0;
			} else {
				room = o; rline = alongz ? pt->x0 : pt->z0;
			}
		}
		if (room < 0 || street < 0 || alongz < 0) {
			continue;
		}
		/* the mezzanine on the room side, along the whole window, and nothing over the window yet */
		for (i = 0; i < (alongz ? w.d : w.w) && ok; i++) {
			const int cx = alongz ? (rline == w.x ? w.x - 1 : w.x + w.w) : w.x + i;
			const int cz = alongz ? w.z + i : (rline == w.z ? w.z - 1 : w.z + w.d);
			const int up = lg->cells2[cz][cx] - 1;

			ok = up >= 0 && (lg->areas[up].flags & LG_AREAFLAG_MEZZ) && (mezz < 0 || mezz == up);
			mezz = up;
		}
		for (z = w.z; z < w.z + w.d && ok; z++) {
			for (x = w.x; x < w.x + w.w && ok; x++) {
				ok = lg->cells2[z][x] == 0;
			}
		}
		if (!ok || mezz < 0 || lg->areas[mezz].floor + LG_WIN_HEAD > lg->areas[street].ceil
				|| lg->areas[mezz].floor + LG_WIN_HEAD > lg->areas[mezz].ceil) {
			continue;
		}
		if (lg->numareas >= LG_MAXAREAS) {
			lgSetErr(err, errlen, "too many areas for an upper window (%d)", lg->numareas, 0);
			return 0;
		}
		{
			const int ui = lg->numareas++;
			LgArea *u = &lg->areas[ui];
			const int mfloor = lg->areas[mezz].floor;

			*u = w;
			u->layer = 1;
			u->floor = u->floorlo = (int16_t)(mfloor + LG_WIN_SILL);
			u->ceil = (int16_t)(mfloor + LG_WIN_HEAD);
			u->flags = 0;
			u->dist = -1;
			u->centrepad = -1;
			for (z = w.z; z < w.z + w.d; z++) {
				for (x = w.x; x < w.x + w.w; x++) {
					lg->cells2[z][x] = (int16_t)(ui + 1);
				}
			}
			/* its openings, as the ground window's (the low side's area first) */
			{
				const int lo0 = alongz ? w.z : w.x, hi0 = alongz ? w.z + w.d : w.x + w.w;
				const int wlo = alongz ? w.x : w.z;

				if (!lgStPortal2(lg, alongz, rline, lo0, hi0, rline == wlo ? mezz : ui, rline == wlo ? ui : mezz, 1, 1, err, errlen)) {
					return 0;
				}
				lg->portals[lg->numportals - 1].window = 1;
				if (!lgStPortal2(lg, alongz, sline, lo0, hi0, sline == wlo ? street : ui, sline == wlo ? ui : street, 1, 1, err, errlen)) {
					return 0;
				}
				lg->portals[lg->numportals - 1].window = 1;
			}
		}
	}
	return 1;
}

#include "modlevelgenstation.inc"
#include "modlevelgenbunker2.inc"

/* The streets: two side by side outdoors, three rows of buildings along them */
static int lgTryStreet(LgLayout *lg, const LgParams *p, LgRng *r, char *err, int errlen)
{
	(void)p;

	if (!lgPlaceStreet(lg, r)) {
		lgSetErr(err, errlen, "the street did not fit (%d areas)", lg->numareas, 0);
		return 0;
	}

	return lgAddWings(lg, r, err, errlen)
		&& lgAddMezzanines(lg, err, errlen)
		&& lgFindPortals(lg, r, err, errlen)
		&& lgMezzPortals(lg, err, errlen)
		&& lgRaiseWings(lg, r, err, errlen)
		&& lgStBalconies(lg, err, errlen)
		&& lgStUpperWindows(lg, err, errlen)
		&& lgAreaGraph(lg, r, err, errlen)
		&& lgBuildNav(lg, err, errlen)
		&& lgBuildSpawns(lg, r, err, errlen)
		&& (lgPlaceObstacles(lg), lgPlaceCover(lg, r), 1)
		&& modLevelGenValidate(lg, err, errlen);
}


/* ------------------------------------------------------------------ */
/* The ravine (2026-11-15, the user's design: "even more verticality, and
 * kill floors. Like the current Ravine map in combat simulator"; "the
 * straight line, and then an L shaped ravine"; "ravine floor should be
 * instant death"; "Railings should be glass, catwalks should use a
 * seethrough texture"). This is the straight one.
 *
 * Across (v): hut | doorway | side 0's ground | the ravine | side 1's ground |
 * doorway | hut. Along (u): each side's ground is bands at three heights (0,
 * 2 or 4 levels), neighbours two levels apart. Between two bands, a strip: a
 * ramp at the outer edge, rock, and beside the ravine a tongue of the higher
 * band that ends in a drop-down ledge over the lower one. The ravine's floor
 * (the pit) is LG_RV_PITDEPTH under the lowest ground and kills. Bridges cross
 * where the two sides' bands are at one height; a low catwalk runs along
 * inside the ravine between low ground on the two sides. All the outdoor
 * areas are one room (the pit its main, like the street); the huts are the
 * rooms (start, objectives, extraction). */
#define LG_RV_PITDEPTH  480
/* the ground's lowest height: the pit's floor is at 0, like every other level's
 * lowest floor (a floor under 0 lay outside its room's box and portals: seen
 * from the ground, the pit was not drawn) */
#define LG_RV_BASE      0
/* (2026-11-15: raising the whole ravine by LG_RV_PITDEPTH so the pit's floor is
 * at 0 was tried: the tour then fell through two bridges of three to the kill
 * floor, so the pit stays under 0 and the parts of its floor outside the
 * ground's openings are not drawn from up high) */
#define LG_RV_RAMP      8      /* cells: a ramp climbs two levels */
#define LG_RV_TONGUE    4
#define LG_RV_MAXBANDS  4

/* The L (2026-11-15: "the map should always be in an L shape, make the
 * medium sized map larger if you have to"): two legs, each the straight
 * ravine's cross-section, meeting at a corner. Leg 0 runs along x, its side 0
 * (low z) the outside of the L; leg 1 runs along z, its side 1 (high x) the
 * outside. At the corner: a square of pit, the outside ground wrapping round
 * it (two pieces), and on the inside leg 0's last band reaching the corner.
 * The whole L is turned one of four ways. The ravine's own grid, wider than
 * the other layouts' so each leg has room for its bands. */
static const struct {
	int8_t grid, chasm, side, hut, bands;   /* bands: at most, a leg */
} g_LgRavine[LG_SIZE_COUNT] = {
	{ 64, 6, 10, 6, 2 },
	{ 80, 8, 13, 7, 2 },
	{ 108, 10, 16, 8, 3 },
};
#define LG_RV_MINBAND 12

typedef struct {
	int u0, u1;    /* the bands' run along the leg */
	int vc0;       /* the ravine's near edge across */
	int vs[2];     /* each side's ground, from its low-v edge */
	int nb;
	int bu0[LG_RV_MAXBANDS], bu1[LG_RV_MAXBANDS];
	int lev[2][LG_RV_MAXBANDS];
} LgRvLeg;

/* what lgPlaceRavine leaves for lgRvBridges */
static struct {
	int gw, fx, fz, cw, sw, pit;
	LgRvLeg leg[2];
} g_LgRv;

static LgRect lgRvFlip(LgRect rc)
{
	if (g_LgRv.fx) rc.x = (int16_t)(g_LgRv.gw - rc.x - rc.w);
	if (g_LgRv.fz) rc.z = (int16_t)(g_LgRv.gw - rc.z - rc.d);
	return rc;
}

/* a rectangle along (u) and across (v) leg l, on the grid */
static LgRect lgRvRect(int l, int u, int v, int lu, int lv)
{
	LgStMap m;

	m.gw = g_LgRv.gw;
	m.mirror = 0;
	m.swap = l;
	return lgRvFlip(lgStRect(&m, u, v, lu, lv));
}

static LgRect lgRvGrid(int x, int z, int w, int d)
{
	LgRect rc;

	rc.x = (int16_t)x; rc.z = (int16_t)z; rc.w = (int16_t)w; rc.d = (int16_t)d; rc.room = -1;
	return lgRvFlip(rc);
}

static int lgRvRampDir(int l, int up)
{
	int dir = l ? (up ? LG_RAMP_ZP : LG_RAMP_ZN) : (up ? LG_RAMP_XP : LG_RAMP_XN);

	if (g_LgRv.fx && (dir == LG_RAMP_XP || dir == LG_RAMP_XN)) dir = dir == LG_RAMP_XP ? LG_RAMP_XN : LG_RAMP_XP;
	if (g_LgRv.fz && (dir == LG_RAMP_ZP || dir == LG_RAMP_ZN)) dir = dir == LG_RAMP_ZP ? LG_RAMP_ZN : LG_RAMP_ZP;
	return dir;
}

/* is rc (grown by grow cells) clear of everything marked in occ */
static int lgRvFree(const uint8_t (*occ)[LG_GRIDMAX], const LgRect *rc, int grow, int gw)
{
	int x, z;

	for (z = rc->z - grow; z < rc->z + rc->d + grow; z++) {
		for (x = rc->x - grow; x < rc->x + rc->w + grow; x++) {
			const int in = x >= rc->x && z >= rc->z && x < rc->x + rc->w && z < rc->z + rc->d;

			if (x < 1 || z < 1 || x >= gw - 1 || z >= gw - 1) {
				if (in) {
					return 0;   /* off the grid (the margin may run off it) */
				}
				continue;
			}
			if (occ[z][x]) {
				return 0;
			}
		}
	}
	return 1;
}

static void lgRvMark(uint8_t (*occ)[LG_GRIDMAX], const LgRect *rc)
{
	int x, z;

	for (z = rc->z; z < rc->z + rc->d; z++) {
		for (x = rc->x; x < rc->x + rc->w; x++) {
			occ[z][x] = 1;
		}
	}
}

/* the next band's height: two levels from the last */
static int lgRvNextLev(LgRng *r, int prev)
{
	return prev == 0 ? 2 : prev == 4 ? 2 : (lgRand(r) & 1 ? 0 : 4);
}

static int lgPlaceRavine(LgLayout *lg, LgRng *r)
{
	static uint8_t occ[LG_GRIDMAX][LG_GRIDMAX];
	const int size = lg->size, cw = g_LgRavine[size].chasm, sw = g_LgRavine[size].side, hd = g_LgRavine[size].hut;
	const int G = lg->gw, half = hd + 2 + sw;
	const int vcA = 1 + half, cA1 = vcA + cw;            /* leg 0's ravine across z */
	const int vcB = G - 1 - half - cw, cB1 = vcB + cw;   /* leg 1's across x */
	const int top = LG_RV_BASE + 4 * LG_LEVEL + LG_ST_OPENTOP;
	LgRect rects[LG_MAXAREAS], huts[LG_MAXROOMS], doors[LG_MAXROOMS];
	int rlo[LG_MAXAREAS], rhi[LG_MAXAREAS], rramp[LG_MAXAREAS], rpit[LG_MAXAREAS];
	int hutlev[LG_MAXROOMS], nhuts = 0, nr = 0, l, s, k, i, x, z, hub, outer;

	g_LgRv.gw = G;
	g_LgRv.fx = lgRand(r) & 1;
	g_LgRv.fz = lgRand(r) & 1;
	g_LgRv.cw = cw;
	g_LgRv.sw = sw;
	memset(occ, 0, sizeof(occ));

	/* the legs: along, their bands; across, the cross-section */
	g_LgRv.leg[0].u0 = 2;
	g_LgRv.leg[0].u1 = vcB;
	g_LgRv.leg[0].vc0 = vcA;
	g_LgRv.leg[1].u0 = cA1 + sw;   /* past leg 0's inside ground at the corner */
	g_LgRv.leg[1].u1 = G - 2;
	g_LgRv.leg[1].vc0 = vcB;
	for (l = 0; l < 2; l++) {
		LgRvLeg *lgl = &g_LgRv.leg[l];
		const int len = lgl->u1 - lgl->u0;
		int nb = (len + LG_RV_RAMP) / (LG_RV_MINBAND + LG_RV_RAMP), bl;

		if (nb > g_LgRavine[size].bands) nb = g_LgRavine[size].bands;
		if (nb < 1) {
			return 0;
		}
		bl = (len - (nb - 1) * LG_RV_RAMP) / nb;
		lgl->nb = nb;
		lgl->vs[0] = lgl->vc0 - sw;
		lgl->vs[1] = lgl->vc0 + cw;
		for (k = 0; k < nb; k++) {
			lgl->bu0[k] = lgl->u0 + k * (bl + LG_RV_RAMP);
			lgl->bu1[k] = k == nb - 1 ? lgl->u1 : lgl->bu0[k] + bl;
		}
	}

	/* the heights: neighbours two levels apart. Leg 0's first bands level with
	 * each other (a bridge); leg 1's first bands as leg 0's last on the same
	 * side of the L (outside: leg 0's side 0, leg 1's side 1) */
	{
		LgRvLeg *a = &g_LgRv.leg[0], *b = &g_LgRv.leg[1];

		a->lev[0][0] = a->lev[1][0] = 2 * lgRange(r, 0, 2);
		for (s = 0; s < 2; s++) {
			for (k = 1; k < a->nb; k++) a->lev[s][k] = lgRvNextLev(r, a->lev[s][k - 1]);
		}
		b->lev[1][0] = a->lev[0][a->nb - 1];
		b->lev[0][0] = a->lev[1][a->nb - 1];
		for (s = 0; s < 2; s++) {
			for (k = 1; k < b->nb; k++) b->lev[s][k] = lgRvNextLev(r, b->lev[s][k - 1]);
		}
		outer = a->lev[0][a->nb - 1];
	}

	/* the outdoor pieces, before the huts so the huts can keep clear of them */
#define LG_RV_ADD(rc_, lo_, hi_, ramp_, pit_) do { rects[nr] = (rc_); rlo[nr] = (lo_); rhi[nr] = (hi_); \
		rramp[nr] = (ramp_); rpit[nr] = (pit_); lgRvMark(occ, &rects[nr]); nr++; } while (0)
	LG_RV_ADD(lgRvRect(0, 2, vcA, vcB - 2, cw), 0, 0, 0, 1);                  /* leg 0's ravine: the main */
	LG_RV_ADD(lgRvGrid(vcB, vcA, cw, cw), 0, 0, 0, 1);                        /* the corner's */
	LG_RV_ADD(lgRvRect(1, cA1, vcB, G - 2 - cA1, cw), 0, 0, 0, 1);            /* leg 1's */
	LG_RV_ADD(lgRvGrid(vcB, vcA - sw, cw + sw, sw), outer, outer, 0, 0);       /* outside the corner */
	LG_RV_ADD(lgRvGrid(cB1, vcA, sw, cw + sw), outer, outer, 0, 0);
	for (l = 0; l < 2; l++) {
		const LgRvLeg *lgl = &g_LgRv.leg[l];

		for (s = 0; s < 2; s++) {
			for (k = 0; k < lgl->nb; k++) {
				const int lv = lgl->lev[s][k];

				LG_RV_ADD(lgRvRect(l, lgl->bu0[k], lgl->vs[s], lgl->bu1[k] - lgl->bu0[k], sw), lv, lv, 0, 0);
			}
			/* the strips between bands: a ramp at the outer edge, a tongue of the higher band by the ravine */
			for (k = 0; k + 1 < lgl->nb; k++) {
				const int lo = lgl->lev[s][k] < lgl->lev[s][k + 1] ? lgl->lev[s][k] : lgl->lev[s][k + 1];
				const int hi = lgl->lev[s][k] + lgl->lev[s][k + 1] - lo;
				const int up = lgl->lev[s][k + 1] > lgl->lev[s][k];
				const int su = lgl->bu1[k];

				LG_RV_ADD(lgRvRect(l, su, s == 0 ? lgl->vs[0] : lgl->vs[1] + sw - 3, LG_RV_RAMP, 3), lo, hi, lgRvRampDir(l, up), 0);
				LG_RV_ADD(lgRvRect(l, su, s == 0 ? lgl->vc0 - LG_RV_TONGUE : lgl->vc0 + cw, LG_RV_RAMP, LG_RV_TONGUE), hi, hi, 0, 0);
			}
		}
	}
#undef LG_RV_ADD

	/* the huts: one on each side of each leg where there is room, behind a
	 * band, a doorway in; at least three */
	for (l = 0; l < 2; l++) {
		const LgRvLeg *lgl = &g_LgRv.leg[l];

		for (s = 0; s < 2; s++) {
			int tries = 0;

			while (tries++ < 20 && nhuts < LG_MAXROOMS - 2) {
				const int kk = lgRange(r, 0, lgl->nb - 1), b0 = lgl->bu0[kk], b1 = lgl->bu1[kk];
				const int hl = lgRange(r, 6, 9);
				const int hv = s == 0 ? lgl->vs[0] - 2 - hd : lgl->vs[1] + sw + 2;
				const int dv = s == 0 ? lgl->vs[0] - 2 : lgl->vs[1] + sw;
				int hu;
				LgRect hut, door, dside;

				if (b1 - b0 < hl + 4) {
					continue;
				}
				hu = lgRange(r, b0 + 2, b1 - hl - 2);
				hut = lgRvRect(l, hu, hv, hl, hd);
				door = lgRvRect(l, hu + hl / 2 - 1, dv, 3, 2);
				dside = lgRvRect(l, hu + hl / 2 - 2, dv, 5, 2);   /* the doorway and a cell either side */
				if (!lgRvFree((const uint8_t (*)[LG_GRIDMAX])occ, &hut, 1, G) || !lgRvFree((const uint8_t (*)[LG_GRIDMAX])occ, &dside, 0, G)) {
					continue;
				}
				huts[nhuts] = hut;
				doors[nhuts] = door;
				hutlev[nhuts] = lgl->lev[s][kk];
				lgRvMark(occ, &hut);
				lgRvMark(occ, &door);
				nhuts++;
				break;
			}
		}
	}
	if (nhuts < 3) {
		return 0;
	}

	/* the areas: the huts (the rooms), the pit (the outdoor room's main), the rest, the doorways */
	for (i = 0; i < nhuts; i++) {
		const int pick = lgRange(r, 0, 9), type = pick < 5 ? LG_ROOM_STORAGE : LG_ROOM_OFFICE;

		huts[i].room = (int16_t)i;
		lgSetRoomArea(&lg->areas[i], &huts[i], type, i, 0, g_LgRoomTypes[type].ceil);
		lg->areas[i].floor = lg->areas[i].floorlo = (int16_t)(LG_RV_BASE + hutlev[i] * LG_LEVEL);
		lg->areas[i].ceil = (int16_t)(lg->areas[i].floor + g_LgRoomTypes[type].ceil);
	}
	hub = nhuts;
	lg->numrooms = (int16_t)(nhuts + 1);
	lg->numareas = nhuts;
	for (i = 0; i < nr; i++) {
		LgArea *a = &lg->areas[lg->numareas++];

		if (rpit[i]) {
			lgSetHubArea(a, &rects[i], hub, LG_RV_BASE - LG_RV_PITDEPTH, LG_RV_BASE - LG_RV_PITDEPTH, 0);
			a->special = LG_SPECIAL_PIT;
		} else {
			lgSetHubArea(a, &rects[i], hub, LG_RV_BASE + rlo[i] * LG_LEVEL, LG_RV_BASE + rhi[i] * LG_LEVEL, rramp[i]);
		}
		a->open = 1;
		a->theme = LG_STREET_THEME;
		a->ceil = (int16_t)top;
	}
	for (i = 0; i < nhuts; i++) {
		lgStCorridor(lg, &doors[i], LG_RV_BASE + hutlev[i] * LG_LEVEL, LG_RV_BASE + hutlev[i] * LG_LEVEL, 0, 1, top);
	}

	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];

		if (a->x < 1 || a->z < 1 || a->x + a->w > lg->gw - 1 || a->z + a->d > lg->gd - 1) {
			return 0;
		}
		for (z = a->z; z < a->z + a->d; z++) {
			for (x = a->x; x < a->x + a->w; x++) {
				if (lg->cells[z][x]) {
					return 0;
				}
				lg->cells[z][x] = (int16_t)(i + 1);
			}
		}
	}
	g_LgRv.pit = hub;
	return 1;
}

/* Take the cells from..to of the line out of the ledges between areas a and b
 * (a bridge lands there), and their open bits */
static int lgRvCut(LgLayout *lg, int alongz, int line, int from, int to, int a, int b, char *err, int errlen)
{
	int i, q;

	for (i = 0; i < lg->numportals; i++) {
		LgPortal *p = &lg->portals[i];
		const int pal = p->x0 == p->x1, pline = pal ? p->x0 : p->z0;
		const int p0 = pal ? p->z0 : p->x0, p1 = pal ? p->z1 : p->x1;

		if (pal != alongz || pline != line || !((p->a == a && p->b == b) || (p->a == b && p->b == a)) || p1 <= from || p0 >= to) {
			continue;
		}
		if (p0 < from && p1 > to) {
			/* split in two: this one keeps the low part, a copy the high part */
			LgPortal *n;

			if (lg->numportals >= LG_MAXPORTALS) {
				lgSetErr(err, errlen, "too many portals (%d)", lg->numportals, 0);
				return 0;
			}
			n = &lg->portals[lg->numportals++];
			*n = *p;
			if (pal) { n->z0 = (int16_t)to; p->z1 = (int16_t)from; } else { n->x0 = (int16_t)to; p->x1 = (int16_t)from; }
		} else if (p0 < from) {
			if (pal) p->z1 = (int16_t)from; else p->x1 = (int16_t)from;
		} else if (p1 > to) {
			if (pal) p->z0 = (int16_t)to; else p->x0 = (int16_t)to;
		} else {
			lg->portals[i] = lg->portals[--lg->numportals];   /* all of it */
			i--;
		}
	}
	for (q = from; q < to; q++) {
		if (alongz) {
			lg->open[q][line - 1] &= (uint8_t)~LG_OPEN_XP;
		} else {
			lg->open[line - 1][q] &= (uint8_t)~LG_OPEN_ZP;
		}
	}
	return 1;
}

/* 0: layer 1 pieces drop onto a ravine's pit only; 1: onto any lower floor of
 * their own room (a warehouse's gantry over the hall, 2026-11-15) */
static int g_LgPieceAnyDrop;

/* What is beside layer 1 piece gi at cell x z: 1 another piece made before it
 * (level), 2 ground at its height, 3 the pit (or a lower floor of its room), 0
 * nothing to open on */
static int lgRvBeside(const LgLayout *lg, int gi, int x, int z, int *other)
{
	const int up = lg->cells2[z][x] - 1, down = lg->cells[z][x] - 1;

	*other = -1;
	if (up >= 0) {
		*other = up;
		return up < gi ? 1 : 0;
	}
	if (down >= 0 && lg->areas[down].special == LG_SPECIAL_PIT) {
		*other = down;
		return 3;
	}
	if (down >= 0 && lg->areas[down].floor == lg->areas[gi].floor && !lg->areas[down].ramp && !lg->areas[down].layer) {
		*other = down;
		return 2;
	}
	if (g_LgPieceAnyDrop && down >= 0 && !lg->areas[down].layer && lg->areas[down].kind == LG_AREA_ROOM
			&& lg->areas[down].room == lg->areas[gi].room && lg->areas[down].floor < lg->areas[gi].floor) {
		*other = down;
		return 3;
	}
	return 0;
}

/* A layer 1 piece over the ravine: openings on each of its edges, by what is
 * beside it: another piece (level), ground at its height (level, the ground's
 * ledge over the pit cut back), or the pit (a ledge, railed unless norail) */
static int lgRvPieceEdges(LgLayout *lg, int gi, int norail, char *err, int errlen)
{
	const LgArea g = lg->areas[gi];
	const int pit = lg->cells[g.z][g.x] - 1;   /* the piece of ravine under it */
	int side;

	for (side = 0; side < 4; side++) {
		const int alongz = side < 2, n = alongz ? g.d : g.w;
		const int line = side == 0 ? g.x : side == 1 ? g.x + g.w : side == 2 ? g.z : g.z + g.d;
		const int base = alongz ? g.z : g.x;
		int i = 0;

		while (i < n) {
			int kind, other, j;

			kind = lgRvBeside(lg, gi, side == 0 ? g.x - 1 : side == 1 ? g.x + g.w : g.x + i,
					side == 2 ? g.z - 1 : side == 3 ? g.z + g.d : g.z + i, &other);
			for (j = i + 1; j < n; j++) {
				int o2;
				const int k2 = lgRvBeside(lg, gi, side == 0 ? g.x - 1 : side == 1 ? g.x + g.w : g.x + j,
						side == 2 ? g.z - 1 : side == 3 ? g.z + g.d : g.z + j, &o2);

				if (k2 != kind || o2 != other) {
					break;
				}
			}
			if (kind) {
				const int lowo = side == 0 || side == 2;   /* the other is on the low side */

				if (kind == 2 && !lgRvCut(lg, alongz, line, base + i, base + j, other, pit, err, errlen)) {
					return 0;
				}
				if (!lgStPortal2(lg, alongz, line, base + i, base + j, lowo ? other : gi, lowo ? gi : other, kind == 3, kind == 3 && norail, err, errlen)) {
					return 0;
				}
			}
			i = j;
		}
	}
	return 1;
}

/* A layer 1 piece (a bridge, catwalk or gantry) at floor `floor` over rc, of
 * room `base`'s room: over the pit only (pitonly) or over any lower floor of
 * that room; -1 if it isn't, -2 if full */
static int lgLayerPiece(LgLayout *lg, const LgRect *rc, int base, int floor, int pitonly, char *err, int errlen)
{
	const int gi = lg->numareas;
	LgArea *g;
	int x, z;

	for (z = rc->z; z < rc->z + rc->d; z++) {
		for (x = rc->x; x < rc->x + rc->w; x++) {
			const int below = lg->cells[z][x] - 1;

			if (below < 0 || lg->cells2[z][x] || lg->areas[below].layer
					|| (pitonly ? lg->areas[below].special != LG_SPECIAL_PIT
						: (lg->areas[below].room != lg->areas[base].room || lg->areas[below].floor >= floor))) {
				return -1;   /* not over open space */
			}
		}
	}
	if (gi >= LG_MAXAREAS) {
		lgSetErr(err, errlen, "too many areas for a bridge (%d)", gi, 0);
		return -2;
	}
	g = &lg->areas[lg->numareas++];
	memset(g, 0, sizeof(*g));
	g->x = rc->x; g->z = rc->z; g->w = rc->w; g->d = rc->d;
	g->kind = LG_AREA_ROOM;
	g->roomtype = lg->areas[base].roomtype;
	g->theme = lg->areas[base].theme;
	g->room = lg->areas[base].room;
	g->floor = g->floorlo = (int16_t)floor;
	g->ceil = lg->areas[base].ceil;
	g->layer = 1;
	g->open = 1;
	g->special = LG_SPECIAL_BRIDGE;
	g->dist = -1;
	g->centrepad = -1;
	lgMezzFill(lg, g, gi);
	return gi;
}

static int lgRvPiece(LgLayout *lg, const LgRect *rc, int level, char *err, int errlen)
{
	return lgLayerPiece(lg, rc, g_LgRv.pit, LG_RV_BASE + level * LG_LEVEL, 1, err, errlen);
}

/* The bridges, wherever the two sides are level with each other (the first
 * wide and railed, the others narrow and unrailed half the time), and the
 * low catwalk along inside the ravine between low ground on the two sides */
static int lgRvBridges(LgLayout *lg, LgRng *r, char *err, int errlen)
{
	const int cw = g_LgRv.cw;
	int l, k, made = 0;

	for (l = 0; l < 2; l++) {
		const LgRvLeg *lgl = &g_LgRv.leg[l];
		const int vc0 = lgl->vc0;

		for (k = 0; k < lgl->nb; k++) {
			if (lgl->lev[0][k] == lgl->lev[1][k]) {
				const int narrow = made > 0 && (lgRand(r) & 1), bw = narrow ? 2 : 3;
				const int b0 = lgl->bu0[k], b1 = lgl->bu1[k];
				int bu, gi;
				LgRect rc;

				if (b1 - b0 < bw + 4) {
					continue;
				}
				bu = lgRange(r, b0 + 2, b1 - bw - 2);
				rc = lgRvRect(l, bu, vc0, bw, cw);
				gi = lgRvPiece(lg, &rc, lgl->lev[0][k], err, errlen);
				if (gi == -2 || (gi >= 0 && !lgRvPieceEdges(lg, gi, narrow, err, errlen))) {
					return 0;
				}
				made += gi >= 0;
			}
		}
		/* the catwalk: from low ground on side 0 to low ground on side 1 in another band */
		{
			int k0 = -1, k1 = -1;

			for (k = 0; k < lgl->nb; k++) {
				if (lgl->lev[0][k] == 0 && k0 < 0) k0 = k;
				if (lgl->lev[1][k] == 0) k1 = k;
			}
			if (k0 >= 0 && k1 >= 0 && k0 != k1) {
				const int uw = lgRange(r, lgl->bu0[k0] + 2, lgl->bu1[k0] - 4);
				const int ue = lgRange(r, lgl->bu0[k1] + 2, lgl->bu1[k1] - 4);
				const int vm = vc0 + cw / 2 - 1, lo = uw < ue ? uw : ue, hi = uw < ue ? ue : uw;
				const LgRect armw = lgRvRect(l, uw, vc0, 2, vm - vc0);
				const LgRect run = lgRvRect(l, lo, vm, hi + 2 - lo, 2);
				const LgRect arme = lgRvRect(l, ue, vm + 2, 2, vc0 + cw - vm - 2);
				int a, b, c;

				/* all three over open ravine, or none */
				a = lgRvPiece(lg, &armw, 0, err, errlen);
				b = a >= 0 ? lgRvPiece(lg, &run, 0, err, errlen) : -1;
				c = b >= 0 ? lgRvPiece(lg, &arme, 0, err, errlen) : -1;
				if (a == -2 || b == -2 || c == -2) {
					return 0;
				}
				if (c < 0) {
					/* undo what was made (it was the last areas) */
					int x, z, gi;

					for (gi = lg->numareas - 1; gi >= 0 && (gi == a || gi == b); gi--) {
						for (z = lg->areas[gi].z; z < lg->areas[gi].z + lg->areas[gi].d; z++) {
							for (x = lg->areas[gi].x; x < lg->areas[gi].x + lg->areas[gi].w; x++) {
								lg->cells2[z][x] = 0;
							}
						}
						lg->numareas--;
					}
				} else if (!lgRvPieceEdges(lg, a, 1, err, errlen) || !lgRvPieceEdges(lg, b, 1, err, errlen) || !lgRvPieceEdges(lg, c, 1, err, errlen)) {
					return 0;
				}
			}
		}
	}
	if (made == 0) {
		lgSetErr(err, errlen, "a ravine with no bridge", 0, 0);
		return 0;
	}
	return 1;
}

static int lgTryRavine(LgLayout *lg, const LgParams *p, LgRng *r, char *err, int errlen)
{
	(void)p;

	if (!lgPlaceRavine(lg, r)) {
		lgSetErr(err, errlen, "the ravine did not fit (%d areas)", lg->numareas, 0);
		return 0;
	}
	return lgFindPortals(lg, r, err, errlen)
		&& lgRvBridges(lg, r, err, errlen)
		&& lgAreaGraph(lg, r, err, errlen)
		&& lgBuildNav(lg, err, errlen)
		&& lgBuildSpawns(lg, r, err, errlen)
		&& (lgPlaceObstacles(lg), lgPlaceCover(lg, r), 1)
		&& modLevelGenValidate(lg, err, errlen);
}

/* ------------------------------------------------------------------ */
/* Rooftops                                                             */

/* The rooftops (2026-11-16; user: "The warehouse layout is very unfun, scrap
 * it, and work on a rooftop layout with multiple roofs connected by
 * platforms, some with enterable buildings on them always with glass windows
 * or even walls and floors. The glass floors should be over death pits. One
 * or two roofs should have a 3 story tower on it, so you'll have to add
 * support for multiple stories above 2.").
 *
 * A city block seen from its roofs: buildings in rows and columns with the
 * streets between them a long way down (the street is a kill floor:
 * LG_SPECIAL_PIT at LG_RT_STREET). Each building's roof is flat, at one of
 * three heights, LG_RT_STEP apart; its edges are ledges over the street.
 * Platforms join the roofs across the streets: bridges (layer 1 pieces over
 * the street, as a ravine's, glass-railed), each level with a roof at each
 * end - where the roofs differ, a ramp up to a landing on the lower roof - and
 * now and then a glass walkway instead (LG_SPECIAL_GLASS: a glass floor over
 * the drop, breakable; the guards never walk on it, so it is never the only
 * way). On the roofs:
 * - huts (a room, a door, glass windows); some glass houses, whose walls are
 *   windows all along; some with a glass floor over a well down into the
 *   building (LG_SPECIAL_GLASS again, the well's bottom a kill floor);
 * - on one or two roofs, a tower of three storeys (the third storey is layer
 *   2: cells3): the ground storey's undercroft under the second, its ramp up
 *   in a stairwell open to the second (as a mezzanine's), the second's
 *   undercroft under the third and its ramp up the same way; windows on every
 *   storey.
 * Everything is laid out as it stands (no turning: the blocks are random). */
#define LG_RT_STREET   0                        /* the street: the pits' floor */
#define LG_RT_BASE     1600                     /* the lowest roofs */
#define LG_RT_STEP     160                      /* a roof's height over the next: a ramp of four */
#define LG_RT_ROOFCEIL 280                      /* a hut's ceiling */
#define LG_RT_WELL     900                      /* a glass floor's drop in a building */
#define LG_RT_STOREY   300                      /* a tower's storey: undercroft 280, slab 20 */
#define LG_RT_MARGIN   3                        /* the street round the outside */
#define LG_RT_ROOF_THEME   5
#define LG_RT_BRIDGE_THEME 6
#define LG_RT_STREET_THEME 7
#define LG_RT_GLASS_THEME  8
#define LG_RT_HUT_THEME    1
#define LG_RT_TOWER_THEME  0

/* Six roof islands at every size. Larger variants widen the decks. */
static const struct {
	int8_t grid, width, depth, towers;
} g_LgRooftop[LG_SIZE_COUNT] = {
	{ 62, 16, 18, 1 },
	{ 68, 18, 20, 1 },
	{ 74, 20, 22, 2 },
};

#define LG_RT_BLOCK_THEME  9
#define LG_RT_RELAY_THEME 10
#define LG_RT_CATWALK_THEME 11

#define LG_RT_MAXB 36

static struct {
	int nbx, nbz, nb;
	int bx0[8], bx1[8], bz0[8], bz1[8];          /* the blocks' columns and rows */
	int level[LG_RT_MAXB];                       /* each block's roof height, in steps */
	uint8_t occ[LG_GRIDMAX][LG_GRIDMAX];         /* 1 taken on the roof (a structure, a ramp) */
	int outdoor;                                 /* the outdoors' room (roofs, streets, platforms) */
	int towers, huts, glasshouses, wells, glasswalks, bridges, landings;
} g_LgRt;

#define LG_RT_BLOCK(i, j) ((j) * g_LgRt.nbx + (i))

static int lgRtFloor(int b)
{
	return LG_RT_BASE + g_LgRt.level[b] * LG_RT_STEP;
}

static int lgRtFree(int x0, int z0, int w, int d)
{
	int x, z;

	for (z = z0; z < z0 + d; z++) {
		for (x = x0; x < x0 + w; x++) {
			if (x < 0 || z < 0 || x >= LG_GRIDMAX || z >= LG_GRIDMAX || g_LgRt.occ[z][x]) {
				return 0;
			}
		}
	}
	return 1;
}

static void lgRtMark(int x0, int z0, int w, int d)
{
	int x, z;

	for (z = z0; z < z0 + d; z++) {
		for (x = x0; x < x0 + w; x++) {
			g_LgRt.occ[z][x] = 1;
		}
	}
}

/* a new area at x0, z0, layer 0 unless set after */
static LgArea *lgRtArea(LgLayout *lg, int x0, int z0, int w, int d)
{
	LgArea *a;

	if (lg->numareas >= LG_MAXAREAS - 4) {
		return NULL;
	}
	a = &lg->areas[lg->numareas++];
	memset(a, 0, sizeof(*a));
	a->x = (int16_t)x0; a->z = (int16_t)z0; a->w = (int16_t)w; a->d = (int16_t)d;
	a->dist = -1;
	a->centrepad = -1;
	return a;
}

/* an outdoor piece of the roofs or streets: the outdoors' room, at floor lo..hi */
static LgArea *lgRtOutdoor(LgLayout *lg, int x0, int z0, int w, int d, int lo, int hi, int ramp, int theme)
{
	LgArea *a = lgRtArea(lg, x0, z0, w, d);
	LgRect rc;

	if (a == NULL) {
		return NULL;
	}
	rc.x = (int16_t)x0; rc.z = (int16_t)z0; rc.w = (int16_t)w; rc.d = (int16_t)d; rc.room = -1;
	lgSetHubArea(a, &rc, g_LgRt.outdoor, lo, hi, ramp);
	a->open = 1;
	a->ceil = (int16_t)(LG_RT_BASE + 4 * LG_RT_STEP + 4 * LG_RT_STOREY + LG_ST_OPENTOP);
	a->theme = (uint8_t)theme;
	return a;
}

/* fills x0..x1, z0..z1 round what is taken already with outdoor pieces at
 * floor lo (special, theme): runs of free cells along each row, a run the
 * same as the one above it joined to it, the largest first */
static int lgRtFill(LgLayout *lg, int x0, int z0, int x1, int z1, int lo, int special, int theme, int mark)
{
	static int runs[4096][4];
	int nr = 0, x, z, a, c;

	for (z = z0; z < z1; z++) {
		x = x0;
		while (x < x1) {
			int e, k, joined = 0;

			if (g_LgRt.occ[z][x] || lg->cells[z][x]) { x++; continue; }
			for (e = x; e < x1 && !g_LgRt.occ[z][e] && !lg->cells[z][e]; e++);
			for (k = 0; k < nr && !joined; k++) {
				if (runs[k][0] == x && runs[k][1] == e && runs[k][3] == z) {
					runs[k][3] = z + 1;
					joined = 1;
				}
			}
			if (!joined) {
				if (nr >= 4096) return 0;
				runs[nr][0] = x; runs[nr][1] = e; runs[nr][2] = z; runs[nr][3] = z + 1;
				nr++;
			}
			x = e;
		}
	}
	for (a = 0; a < nr; a++) {
		for (c = a + 1; c < nr; c++) {
			if ((runs[c][1] - runs[c][0]) * (runs[c][3] - runs[c][2]) > (runs[a][1] - runs[a][0]) * (runs[a][3] - runs[a][2])) {
				int t[4];

				memcpy(t, runs[a], sizeof(t)); memcpy(runs[a], runs[c], sizeof(t)); memcpy(runs[c], t, sizeof(t));
			}
		}
	}
	for (a = 0; a < nr; a++) {
		LgArea *ar = lgRtOutdoor(lg, runs[a][0], runs[a][2], runs[a][1] - runs[a][0], runs[a][3] - runs[a][2], lo, lo, 0, theme);

		if (ar == NULL) {
			return 0;
		}
		ar->special = (uint8_t)special;
		lgMezzFill(lg, ar, lg->numareas - 1);
		if (mark) {
			lgRtMark(ar->x, ar->z, ar->w, ar->d);
		}
	}
	return 1;
}

/* A hut on block b at x0, z0 (its walls included), w x d: a room, a door on
 * the face `doorside` (0 -x, 1 +x, 2 -z, 3 +z), glass windows. kind 0 a
 * plain hut (a window on each other face), 1 a glass house (its walls
 * windows all along, a cell of wall between), 2 a glass floor over a well
 * down into the building in its middle */
static int lgRtHut(LgLayout *lg, LgRng *r, int b, int x0, int z0, int w, int d, int doorside, int kind)
{
	const int F = lgRtFloor(b), room = lg->numrooms++;
	const int ix0 = x0 + 1, iz0 = z0 + 1, iw = w - 2, id = d - 2;
	LgArea *a;
	int side;
	LgRect rc;

	if (room >= LG_MAXROOMS - 1) {
		return 0;
	}
	rc.x = (int16_t)ix0; rc.z = (int16_t)iz0; rc.w = (int16_t)iw; rc.d = (int16_t)id; rc.room = -1;
	if (kind == 2 && iw >= 7 && id >= 7) {
		/* the room round the well: the main (all but the middle 3 x 3 and the
		 * cells round it on one side), wings round it; the well itself */
		const int wx = ix0 + (iw - 3) / 2, wz = iz0 + (id - 3) / 2;
		int pieces[4][4], k;

		/* rows above and below the well's, the columns either side of it */
		pieces[0][0] = ix0; pieces[0][1] = iz0; pieces[0][2] = iw; pieces[0][3] = wz - iz0;
		pieces[1][0] = ix0; pieces[1][1] = wz + 3; pieces[1][2] = iw; pieces[1][3] = iz0 + id - wz - 3;
		pieces[2][0] = ix0; pieces[2][1] = wz; pieces[2][2] = wx - ix0; pieces[2][3] = 3;
		pieces[3][0] = wx + 3; pieces[3][1] = wz; pieces[3][2] = ix0 + iw - wx - 3; pieces[3][3] = 3;
		for (k = 0; k < 4; k++) {
			LgRect pr;

			pr.x = (int16_t)pieces[k][0]; pr.z = (int16_t)pieces[k][1]; pr.w = (int16_t)pieces[k][2]; pr.d = (int16_t)pieces[k][3]; pr.room = -1;
			if ((a = lgRtArea(lg, pr.x, pr.z, pr.w, pr.d)) == NULL) {
				return 0;
			}
			lgSetRoomArea(a, &pr, LG_ROOM_OFFICE, room, 0, 0);
			a->floor = a->floorlo = (int16_t)F;
			a->ceil = (int16_t)(F + LG_RT_ROOFCEIL);
			a->theme = LG_RT_HUT_THEME;
		}
		if ((a = lgRtArea(lg, wx, wz, 3, 3)) == NULL) {
			return 0;
		}
		{
			LgRect wr;

			wr.x = (int16_t)wx; wr.z = (int16_t)wz; wr.w = 3; wr.d = 3; wr.room = -1;
			lgSetRoomArea(a, &wr, LG_ROOM_OFFICE, room, 0, 0);
		}
		a->floor = (int16_t)F;
		a->floorlo = (int16_t)(F - LG_RT_WELL);
		a->ceil = (int16_t)(F + LG_RT_ROOFCEIL);
		a->special = LG_SPECIAL_GLASS;
		a->theme = LG_RT_GLASS_THEME;
		g_LgRt.wells++;
	} else {
		if ((a = lgRtArea(lg, ix0, iz0, iw, id)) == NULL) {
			return 0;
		}
		lgSetRoomArea(a, &rc, kind == 1 ? LG_ROOM_HALL : lgRange(r, 0, 2) ? LG_ROOM_OFFICE : LG_ROOM_STORAGE, room, 0, 0);
		a->floor = a->floorlo = (int16_t)F;
		a->ceil = (int16_t)(F + LG_RT_ROOFCEIL);
		a->theme = kind == 1 ? 2 : LG_RT_HUT_THEME;
		if (kind == 1) {
			g_LgRt.glasshouses++;
		}
	}
	g_LgRt.huts++;
	(void)side;
	return 1;
}

/* The hut's door and windows, after the rooms (corridor-kind areas come after
 * every room): through its wall, one cell thick */
static int lgRtHutOpenings(LgLayout *lg, LgRng *r, int b, int x0, int z0, int w, int d, int doorside, int kind)
{
	const int F = lgRtFloor(b);
	int side;

	for (side = 0; side < 4; side++) {
		const int alongz = side < 2, len = alongz ? d - 2 : w - 2;
		const int line = side == 0 ? x0 : side == 1 ? x0 + w - 1 : side == 2 ? z0 : z0 + d - 1;
		const int base = alongz ? z0 + 1 : x0 + 1;
		int u;
		LgRect rc;

		if (len < 3) {
			continue;
		}
		if (side == doorside) {
			u = base + lgRange(r, 0, len - 3);
			rc.x = (int16_t)(alongz ? line : u); rc.z = (int16_t)(alongz ? u : line);
			rc.w = (int16_t)(alongz ? 1 : 3); rc.d = (int16_t)(alongz ? 3 : 1); rc.room = -1;
			lgStCorridor(lg, &rc, F, F, 0, 0, 0);
			lg->areas[lg->numareas - 1].ceil = (int16_t)(F + LG_RT_ROOFCEIL - 20);
			continue;
		}
		if (kind == 1) {
			/* a glass house: windows the whole way along, a cell of wall between */
			for (u = base; u + 3 <= base + len; u += 4) {
				rc.x = (int16_t)(alongz ? line : u); rc.z = (int16_t)(alongz ? u : line);
				rc.w = (int16_t)(alongz ? 1 : 3); rc.d = (int16_t)(alongz ? 3 : 1); rc.room = -1;
				lgStCorridor(lg, &rc, F + 20, F + 20, 0, 0, 0);
				lg->areas[lg->numareas - 1].ceil = (int16_t)(F + LG_RT_ROOFCEIL - 20);
				lg->areas[lg->numareas - 1].special = LG_SPECIAL_WINDOW;
			}
		} else {
			u = base + (len - 3) / 2;
			rc.x = (int16_t)(alongz ? line : u); rc.z = (int16_t)(alongz ? u : line);
			rc.w = (int16_t)(alongz ? 1 : 3); rc.d = (int16_t)(alongz ? 3 : 1); rc.room = -1;
			lgStCorridor(lg, &rc, F + LG_WIN_SILL, F + LG_WIN_SILL, 0, 0, 0);
			lg->areas[lg->numareas - 1].ceil = (int16_t)(F + LG_WIN_HEAD);
			lg->areas[lg->numareas - 1].special = LG_SPECIAL_WINDOW;
		}
	}
	(void)kind;
	return lg->numareas < LG_MAXAREAS - 4;
}

/* A tower on block b at x0, z0: 10 x 10 with its walls (one cell), the
 * inside ix0..ix0+8, iz0..iz0+8 (u, v below from there). Its storeys, one room:
 *   storey 1 (layer 0): R1, the ramp up, u 1..7 x v 0..3 rising +u, entered
 *     at its foot from U1c (u 0), its top onto storey 2 over U1a (u 7); U1b
 *     the rest (v 3..8), the main;
 *   storey 2 (layer 1) over storey 1's undercrofts: U2a, U2c over U1a, U1c;
 *     U2b v 3..5; R2 u 1..7 x v 5..8 rising -u, its foot by U2d (u 7), its
 *     top onto storey 3 over U2e (u 0);
 *   storey 3 (layer 2) over U2a, U2b, U2c, U2d, U2e.
 * Its door on +v by U1b, windows on every storey (lgRtTowerOpenings). */
#define LG_RT_TOWER 10
static int lgRtTower(LgLayout *lg, int b, int x0, int z0)
{
	const int F = lgRtFloor(b), room = lg->numrooms++;
	const int X = x0 + 1, Z = z0 + 1;
	const int f1 = F, f2 = F + LG_RT_STOREY, f3 = F + 2 * LG_RT_STOREY;
	const int c1 = f1 + LG_RT_STOREY - LG_MEZZ_SLAB, c2 = f2 + LG_RT_STOREY - LG_MEZZ_SLAB, c3 = f3 + LG_RT_STOREY;
	const int U = LG_AREAFLAG_UNDER, B = LG_AREAFLAG_BAY, Mz = LG_AREAFLAG_MEZZ;
	/* u, v, w, d, layer, floorlo, floor, ceil, ramp, flags */
	const int pc[][10] = {
		{ 0, 3, 8, 5, 0, f1, f1, c1, 0, U },                    /* U1b: the main */
		{ 7, 0, 1, 3, 0, f1, f1, c1, 0, U },                    /* U1a */
		{ 0, 0, 1, 3, 0, f1, f1, c1, 0, U },                    /* U1c */
		{ 1, 0, 6, 3, 0, f1, f2, c2, LG_RAMP_XP, B },           /* R1 */
		{ 7, 0, 1, 3, 1, f2, f2, c2, 0, Mz | U },               /* U2a */
		{ 0, 0, 1, 3, 1, f2, f2, c2, 0, Mz | U },               /* U2c */
		{ 0, 3, 8, 2, 1, f2, f2, c2, 0, Mz | U },               /* U2b */
		{ 7, 5, 1, 3, 1, f2, f2, c2, 0, Mz | U },               /* U2d */
		{ 0, 5, 1, 3, 1, f2, f2, c2, 0, Mz | U },               /* U2e */
		{ 1, 5, 6, 3, 1, f2, f3, c3, LG_RAMP_XN, Mz | B },      /* R2 */
		{ 7, 0, 1, 3, 2, f3, f3, c3, 0, Mz },                   /* Z3a */
		{ 0, 0, 1, 3, 2, f3, f3, c3, 0, Mz },                   /* Z3c */
		{ 0, 3, 8, 2, 2, f3, f3, c3, 0, Mz },                   /* Z3b */
		{ 7, 5, 1, 3, 2, f3, f3, c3, 0, Mz },                   /* Z3d */
		{ 0, 5, 1, 3, 2, f3, f3, c3, 0, Mz },                   /* Z3e */
	};
	int k;

	if (room >= LG_MAXROOMS - 1) {
		return 0;
	}
	for (k = 0; k < (int)(sizeof(pc) / sizeof(pc[0])); k++) {
		LgArea *a = lgRtArea(lg, X + pc[k][0], Z + pc[k][1], pc[k][2], pc[k][3]);
		LgRect rc;

		if (a == NULL) {
			return 0;
		}
		rc.x = (int16_t)(X + pc[k][0]); rc.z = (int16_t)(Z + pc[k][1]); rc.w = (int16_t)pc[k][2]; rc.d = (int16_t)pc[k][3]; rc.room = -1;
		lgSetRoomArea(a, &rc, LG_ROOM_LAB, room, 0, 0);
		a->layer = (uint8_t)pc[k][4];
		a->floorlo = (int16_t)pc[k][5];
		a->floor = (int16_t)pc[k][6];
		a->ceil = (int16_t)pc[k][7];
		a->ramp = (uint8_t)pc[k][8];
		a->flags = (uint8_t)pc[k][9];
		a->theme = LG_RT_TOWER_THEME;
		lgMezzFill(lg, a, lg->numareas - 1);
	}
	g_LgRt.towers++;
	return 1;
}

/* The tower's door and windows (corridor-kind, after the rooms): one face
 * cell, one opening */
static int lgRtTowerOpenings(LgLayout *lg, int b, int x0, int z0)
{
	const int F = lgRtFloor(b);
	const int X = x0 + 1, Z = z0 + 1, zf = z0 + LG_RT_TOWER - 1, xf = x0 + LG_RT_TOWER - 1;
	/* x, z, w, d, layer, sill (over the storey's floor), kind (0 door, 1 window) */
	const int op[][7] = {
		{ X, zf, 3, 1, 0, 0, 0 },                       /* the door, +z, by U1b */
		{ X + 5, zf, 3, 1, 0, LG_WIN_SILL, 1 },         /* storey 1, +z */
		{ x0, Z + 4, 1, 3, 0, LG_WIN_SILL, 1 },         /* storey 1, -x */
		{ xf, Z + 5, 1, 3, 1, LG_WIN_SILL, 1 },         /* storey 2, +x by U2d */
		{ xf, Z, 1, 3, 2, LG_WIN_SILL, 1 },             /* storey 3, +x by Z3a */
		{ x0, Z, 1, 3, 2, LG_WIN_SILL, 1 },             /* storey 3, -x by Z3c */
	};
	int k;

	for (k = 0; k < (int)(sizeof(op) / sizeof(op[0])); k++) {
		const int f = F + op[k][4] * LG_RT_STOREY;
		LgRect rc;
		LgArea *a;

		rc.x = (int16_t)op[k][0]; rc.z = (int16_t)op[k][1]; rc.w = (int16_t)op[k][2]; rc.d = (int16_t)op[k][3]; rc.room = -1;
		if (lg->numareas >= LG_MAXAREAS - 4) {
			return 0;
		}
		lgStCorridor(lg, &rc, f + op[k][5], f + op[k][5], 0, 0, 0);
		a = &lg->areas[lg->numareas - 1];
		a->layer = (uint8_t)op[k][4];
		if (op[k][6]) {
			a->ceil = (int16_t)(f + LG_WIN_HEAD);
			a->special = LG_SPECIAL_WINDOW;
		} else {
			a->ceil = (int16_t)(f + LG_RT_STOREY - 40);
		}
		lgMezzFill(lg, a, lg->numareas - 1);
	}
	return 1;
}

typedef struct {
	int b0, b1, alongx;    /* blocks; alongx: the street between runs along x (b1 is +z of b0) */
	int u0, u1;            /* where the two blocks face each other along the street */
	int g0, g1;            /* the street's cells across */
	int glass;
	int far;              /* prefer the high-coordinate end of this edge */
} LgRtLink;

static void lgPlaceRooftop(LgLayout *lg, LgRng *r, LgRtLink *links, int *nlinks)
{
	const int width = g_LgRooftop[lg->size].width;
	const int depth = g_LgRooftop[lg->size].depth;
	const int south = (lg->gd - 2 * depth - 4) / 2;
	const int raised = lgRange(r, 0, 1);
	const int stagger = lgRange(r, 0, 1);

	memset(&g_LgRt, 0, sizeof(g_LgRt));
	g_LgRt.nbx = 3;
	g_LgRt.nbz = 2;
	g_LgRt.nb = 6;
	g_LgRt.outdoor = -1;
	for (int col = 0; col < 3; col++) {
		g_LgRt.bx0[col] = LG_RT_MARGIN + col * (width + 4);
		g_LgRt.bx1[col] = g_LgRt.bx0[col] + width;
	}
	for (int row = 0; row < 2; row++) {
		g_LgRt.bz0[row] = south + row * (depth + 4);
		g_LgRt.bz1[row] = g_LgRt.bz0[row] + depth;
		for (int col = 0; col < 3; col++) {
			g_LgRt.level[LG_RT_BLOCK(col, row)] = row == raised;
		}
	}

	/* Connect every neighbouring pair. Stagger the two crossings along each
	 * row, then add a breakable bridge opposite the relay's solid crossing. */
	*nlinks = 0;
	for (int col = 0; col < 3; col++) {
		const LgRtLink crossing = {
			LG_RT_BLOCK(col, 0), LG_RT_BLOCK(col, 1), 1,
			g_LgRt.bx0[col], g_LgRt.bx1[col], g_LgRt.bz1[0], g_LgRt.bz0[1], 0, (col + stagger) & 1,
		};
		links[(*nlinks)++] = crossing;
	}
	for (int row = 0; row < 2; row++) {
		for (int col = 0; col < 2; col++) {
			const LgRtLink crossing = {
				LG_RT_BLOCK(col, row), LG_RT_BLOCK(col + 1, row), 0,
				g_LgRt.bz0[row], g_LgRt.bz1[row], g_LgRt.bx1[col], g_LgRt.bx0[col + 1], 0,
				(col + row + stagger) & 1,
			};
			links[(*nlinks)++] = crossing;
		}
	}
	LgRtLink glass = links[3];
	glass.glass = 1;
	glass.far = !glass.far;
	links[(*nlinks)++] = glass;
}

/* Solid service cores rise above the catwalks. The north relay block has
 * the satellite dish and keeps an open service route along all four sides. */
static int lgRtPlaceServiceBlocks(LgLayout *lg)
{
	const int blocks[][6] = {
		/* island, x/z inset, width, depth, rise */
		{ 1, 3, 4, 10, 10, 1110 },
		{ 4, 3, 5,  8, 10, 1050 },
		{ 2, 5, 3,  7, 10,  980 },
	};

	for (int i = 0; i < (int)(sizeof(blocks) / sizeof(blocks[0])); i++) {
		const int *block = blocks[i];
		const int x = g_LgRt.bx0[block[0] % 3] + block[1];
		const int z = g_LgRt.bz0[block[0] / 3] + block[2];
		const int top = lgRtFloor(block[0]) + block[5];
		LgArea *a = lgRtOutdoor(lg, x, z, block[3], block[4], top, top, LG_RAMP_NONE,
				i == 0 ? LG_RT_RELAY_THEME : LG_RT_BLOCK_THEME);
		if (!a) return 0;

		a->special = LG_SPECIAL_RACK;
		lgRtMark(x, z, block[3], block[4]);
	}

	return 1;
}

static int lgTryRooftop(LgLayout *lg, const LgParams *p, LgRng *r, char *err, int errlen)
{
	static LgRtLink links[LG_RT_MAXB * 2];
	static struct { int b, x, z, w, d, door, kind; } huts[LG_RT_MAXB];
	static struct { int b, x, z; } towers[2];
	const int G = lg->gw;
	int nlinks = 0, nhuts = 0, ntowers = 0, b, k, i;
	int bridgeat[LG_RT_MAXB * 2][4];   /* each link's crossing: x, z, w, d (-1: none) */

	(void)p;
	lgPlaceRooftop(lg, r, links, &nlinks);
	if (!lgRtPlaceServiceBlocks(lg)) return 0;

	/* where each link crosses its street: three cells wide, and the ramp up to a
	 * landing on the lower roof where the two differ (taken on the roofs now) */
	for (k = 0; k < nlinks; k++) {
		LgRtLink *l = &links[k];
		const int span = l->u1 - l->u0, gw = l->g1 - l->g0;
		const int f0 = lgRtFloor(l->b0), f1 = lgRtFloor(l->b1), diff = f0 > f1 ? f0 - f1 : f1 - f0;
		const int lo = f0 < f1 ? l->b0 : l->b1;   /* the lower roof's block */
		const int L = diff / 40;                   /* the ramp's cells (a rise of 40 a cell) */
		int tries, made = 0;

		bridgeat[k][0] = -1;
		if (span < 3 + 2 * 2) {
			continue;
		}
		for (tries = 0; tries < (l->alongx ? 2 : span - 6) && !made; tries++) {
			const int offset = l->alongx ? 0 : 2 + tries;
			const int far = l->alongx ? l->far ^ tries : l->far;
			const int u = far ? l->u1 - 3 - offset : l->u0 + offset;
			/* the landing (3 x 3) and ramp on the lower roof, back from the street */
			const int lofar = lo == l->b0;               /* the lower block is on the street's low side */
			const int edge = lofar ? l->g0 : l->g1;      /* its edge on the street */
			const int ldep = diff ? 3 + L : 0;           /* how far back into the lower roof */
			int ok;

			if (l->alongx) {
				ok = lgRtFree(u, lofar ? edge - ldep - 1 : edge, 3, ldep + 1) || !diff;
			} else {
				ok = lgRtFree(lofar ? edge - ldep - 1 : edge, u - 1, ldep + 1, 5) || !diff;
			}
			/* (and the crossing's ends clear on both roofs) */
			if (l->alongx) {
				ok = ok && lgRtFree(u, l->g0 - 1, 3, 1) && lgRtFree(u, l->g1, 3, 1);
			} else {
				ok = ok && lgRtFree(l->g0 - 1, u, 1, 3) && lgRtFree(l->g1, u, 1, 3);
			}
			if (!ok || (l->glass && diff)) {
				continue;
			}
			if (diff) {
				/* landing against the street, the ramp back from it, rising to it */
				const int hi = f0 > f1 ? f0 : f1, flo = f0 < f1 ? f0 : f1;
				LgArea *a;

				if (l->alongx) {
					const int lz = lofar ? edge - 3 : edge, rz = lofar ? edge - 3 - L : edge + 3;

					if ((a = lgRtOutdoor(lg, u, lz, 3, 3, hi, hi, 0, LG_RT_ROOF_THEME)) == NULL) return 0;
					lgMezzFill(lg, a, lg->numareas - 1);
					if ((a = lgRtOutdoor(lg, u, rz, 3, L, flo, hi, lofar ? LG_RAMP_ZP : LG_RAMP_ZN, LG_RT_ROOF_THEME)) == NULL) return 0;
					lgMezzFill(lg, a, lg->numareas - 1);
					lgRtMark(u, lofar ? rz - 1 : lz, 3, L + 3 + 1);
				} else {
					const int lx = lofar ? edge - 3 : edge, rx = lofar ? edge - 3 - L : edge + 3;

					if ((a = lgRtOutdoor(lg, lx, u, 3, 3, hi, hi, 0, LG_RT_ROOF_THEME)) == NULL) return 0;
					lgMezzFill(lg, a, lg->numareas - 1);
					if ((a = lgRtOutdoor(lg, rx, u, L, 3, flo, hi, lofar ? LG_RAMP_XP : LG_RAMP_XN, LG_RT_ROOF_THEME)) == NULL) return 0;
					lgMezzFill(lg, a, lg->numareas - 1);
					lgRtMark(lofar ? rx - 1 : lx, u - 1, L + 3 + 1, 5);
				}
				g_LgRt.landings++;
			}
			/* the crossing's ends kept clear on both roofs */
			if (l->alongx) {
				lgRtMark(u - 1, l->g0 - 2, 5, 2);
				lgRtMark(u - 1, l->g1, 5, 2);
				bridgeat[k][0] = u; bridgeat[k][1] = l->g0; bridgeat[k][2] = 3; bridgeat[k][3] = gw;
			} else {
				lgRtMark(l->g0 - 2, u - 1, 2, 5);
				lgRtMark(l->g1, u - 1, 2, 5);
				bridgeat[k][0] = l->g0; bridgeat[k][1] = u; bridgeat[k][2] = gw; bridgeat[k][3] = 3;
			}
			made = 1;
		}
		/* (a link of the tree that could not be made: try again) */
		if (!made) {
			lgSetErr(err, errlen, "no crossing for link %d", k, 0);
			return 0;
		}
	}
	/* (the landings and ramps are made: their cells, and the marks they left,
	 * must not be roofed over; the marks off the roof itself are cleared) */

	/* The climbable towers flank the relay, with a glass greenhouse on
	 * the remaining service roof. Keep their sites clear of the crossings. */
	{
		const int towerBlocks[] = { 0, 5, 3 };
		const int wanted = g_LgRooftop[lg->size].towers;

		for (k = 0; k < 3; k++) {
			const int bb = towerBlocks[k], col = bb % 3, row = bb / 3;
			const int x0 = g_LgRt.bx0[col], x1 = g_LgRt.bx1[col];
			const int z0 = g_LgRt.bz0[row], z1 = g_LgRt.bz1[row];
			const int tower = ntowers < wanted && k < 2;
			const int w = tower ? LG_RT_TOWER : 8, d = tower ? LG_RT_TOWER : 8;
			int placed = 0;

			for (int z = z0 + 2; !placed && z + d + 3 <= z1; z++) {
				for (int x = x0 + 2; x + w + 2 <= x1; x++) {
					if (!lgRtFree(x - 1, z - 1, w + 2, d + 3)) continue;

					lgRtMark(x - 1, z - 1, w + 2, d + 3);
					if (tower) {
						towers[ntowers].b = bb;
						towers[ntowers].x = x;
						towers[ntowers++].z = z;
					} else {
						huts[nhuts].b = bb;
						huts[nhuts].x = x;
						huts[nhuts].z = z;
						huts[nhuts].w = w;
						huts[nhuts].d = d;
						huts[nhuts].door = 3;
						huts[nhuts++].kind = bb == 3 ? 1 : 0;
					}
					placed = 1;
					break;
				}
			}
		}
		if (ntowers < wanted || !nhuts) {
			lgSetErr(err, errlen, "no clear roof for a tower or the greenhouse", 0, 0);
			return 0;
		}
	}

	/* the areas. Rooms first (huts, towers: their mains are their numbers);
	 * then the outdoors' main, the rest of it; then doors and windows */
	{
		/* (what was made already - landings and ramps - goes after the rooms:
		 * keep it aside and put it back) */
		static LgArea early[64];
		const int nearly = lg->numareas;
		int x, z;

		if (nearly > 64) {
			return 0;
		}
		memcpy(early, lg->areas, sizeof(LgArea) * (size_t)nearly);
		memset(lg->cells, 0, sizeof(lg->cells));
		lg->numareas = 0;
		lg->numrooms = 0;
		for (i = 0; i < nhuts; i++) {
			if (!lgRtHut(lg, r, huts[i].b, huts[i].x, huts[i].z, huts[i].w, huts[i].d, huts[i].door, huts[i].kind)) {
				lgSetErr(err, errlen, "no room for hut %d", i, 0);
				return 0;
			}
		}
		for (i = 0; i < ntowers; i++) {
			if (!lgRtTower(lg, towers[i].b, towers[i].x, towers[i].z)) {
				lgSetErr(err, errlen, "no room for tower %d", i, 0);
				return 0;
			}
		}
		/* (rooms' areas so far are each room's pieces in order; a room's main
		 * must be its number: check) */
		for (i = 0; i < lg->numrooms; i++) {
			/* each room's main: its biggest piece on the ground, at its number */
			int m, best = -1;

			for (m = i; m < lg->numareas; m++) {
				if (lg->areas[m].room == i && !lg->areas[m].layer && !lg->areas[m].ramp && !lg->areas[m].special
						&& (best < 0 || lg->areas[m].w * lg->areas[m].d > lg->areas[best].w * lg->areas[best].d)) {
					best = m;
				}
			}
			if (best < 0) {
				return 0;
			}
			if (best != i) {
				const LgArea t = lg->areas[i];

				lg->areas[i] = lg->areas[best];
				lg->areas[best] = t;
			}
		}
		g_LgRt.outdoor = lg->numrooms++;
		/* every room's cells (after the shuffle) */
		for (i = 0; i < lg->numareas; i++) {
			lgMezzFill(lg, &lg->areas[i], i);
		}
		/* the roofs: each block's free cells (the largest piece of all first:
		 * the outdoors' main), the landings and ramps, the streets, the glass
		 * walkways */
		{
			static uint8_t keep[LG_GRIDMAX][LG_GRIDMAX];

			memcpy(keep, g_LgRt.occ, sizeof(keep));
			/* (structures' cells are cells already; the landings' too, below) */
			memset(g_LgRt.occ, 0, sizeof(g_LgRt.occ));
			for (i = 0; i < nearly; i++) {
				lgRtMark(early[i].x, early[i].z, early[i].w, early[i].d);
			}
			/* (the structures' walls: solid, neither roof nor street) */
			for (i = 0; i < nhuts; i++) {
				lgRtMark(huts[i].x, huts[i].z, huts[i].w, huts[i].d);
			}
			for (i = 0; i < ntowers; i++) {
				lgRtMark(towers[i].x, towers[i].z, LG_RT_TOWER, LG_RT_TOWER);
			}
			{
				/* the main: the biggest block's free cells, first */
				int best = 0, bs = -1;

				for (b = 0; b < g_LgRt.nb; b++) {
					const int ib = b % g_LgRt.nbx, jb = b / g_LgRt.nbx;
					const int s = (g_LgRt.bx1[ib] - g_LgRt.bx0[ib]) * (g_LgRt.bz1[jb] - g_LgRt.bz0[jb]);

					if (s > bs) { bs = s; best = b; }
				}
				for (k = 0; k < g_LgRt.nb; k++) {
					const int bb = k == 0 ? best : (k <= best ? k - 1 : k);
					const int ib = bb % g_LgRt.nbx, jb = bb / g_LgRt.nbx;

					const int theme = bb == 1 || bb == 4 || bb == 2 ? LG_RT_CATWALK_THEME : LG_RT_ROOF_THEME;
					if (!lgRtFill(lg, g_LgRt.bx0[ib], g_LgRt.bz0[jb], g_LgRt.bx1[ib], g_LgRt.bz1[jb], lgRtFloor(bb), 0, theme, 0)) {
						lgSetErr(err, errlen, "too many roof pieces (%d areas)", lg->numareas, 0);
						return 0;
					}
					if (k == 0 && lg->numareas == g_LgRt.outdoor) {
						return 0;
					}
				}
			}
			/* the landings and ramps */
			for (i = 0; i < nearly; i++) {
				LgArea *a = &lg->areas[lg->numareas++];

				if (lg->numareas >= LG_MAXAREAS - 4) {
					return 0;
				}
				*a = early[i];
				a->room = (int16_t)g_LgRt.outdoor;
				lgMezzFill(lg, a, lg->numareas - 1);
			}
			/* the glass walkways (in the street's cells) */
			for (k = 0; k < nlinks; k++) {
				if (links[k].glass && bridgeat[k][0] >= 0) {
					const int f = lgRtFloor(links[k].b0);
					LgArea *a = lgRtOutdoor(lg, bridgeat[k][0], bridgeat[k][1], bridgeat[k][2], bridgeat[k][3], f, f, 0, LG_RT_GLASS_THEME);

					if (a == NULL) {
						return 0;
					}
					a->floorlo = LG_RT_STREET;
					a->special = LG_SPECIAL_GLASS;
					lgMezzFill(lg, a, lg->numareas - 1);
					g_LgRt.glasswalks++;
				}
			}
			/* the streets: everything else */
			if (!lgRtFill(lg, 1, 1, G - 1, G - 1, LG_RT_STREET, LG_SPECIAL_PIT, LG_RT_STREET_THEME, 0)) {
				lgSetErr(err, errlen, "too many street pieces (%d areas)", lg->numareas, 0);
				return 0;
			}
			(void)keep;
		}
		/* doors and windows */
		for (i = 0; i < nhuts; i++) {
			if (!lgRtHutOpenings(lg, r, huts[i].b, huts[i].x, huts[i].z, huts[i].w, huts[i].d, huts[i].door, huts[i].kind)) {
				return 0;
			}
		}
		for (i = 0; i < ntowers; i++) {
			if (!lgRtTowerOpenings(lg, towers[i].b, towers[i].x, towers[i].z)) {
				return 0;
			}
		}
		for (i = 0; i < lg->numareas; i++) {
			const LgArea *a = &lg->areas[i];

			for (z = a->z; z < a->z + a->d; z++) {
				for (x = a->x; x < a->x + a->w; x++) {
					if (LG_CELLS(lg, a->layer)[z][x] && LG_CELLS(lg, a->layer)[z][x] != i + 1) {
						lgSetErr(err, errlen, "area %d overlaps another", i, 0);
						return 0;
					}
					LG_CELLS(lg, a->layer)[z][x] = (int16_t)(i + 1);
				}
			}
		}
	}

	/* openings: the ground's, each storey's, the towers' stairwells */
	g_LgPortalLayer = 0;
	if (!lgFindPortals(lg, r, err, errlen)) {
		return 0;
	}
	for (g_LgPortalLayer = 1; g_LgPortalLayer < LG_MAXLAYERS; g_LgPortalLayer++) {
		if (!lgFindPortals(lg, r, err, errlen)) {
			g_LgPortalLayer = 0;
			return 0;
		}
	}
	g_LgPortalLayer = 0;
	if (!lgMezzPortals(lg, err, errlen)) {
		return 0;
	}
	/* the towers' upper windows look out onto the roof */
	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];

		if (a->special == LG_SPECIAL_WINDOW && a->layer) {
			/* the side away from the storey: the cell beside the window with
			 * nothing of its layer (and the roof under it) */
			int side;

			for (side = 0; side < 4; side++) {
				const int alongz = side < 2;
				const int nx = side == 0 ? a->x - 1 : side == 1 ? a->x + a->w : a->x;
				const int nz = side == 2 ? a->z - 1 : side == 3 ? a->z + a->d : a->z;
				const int line = side == 0 ? a->x : side == 1 ? a->x + a->w : side == 2 ? a->z : a->z + a->d;
				const int out = lg->cells[nz][nx] - 1;

				if ((alongz ? a->d : a->w) != 3 || LG_CELLS(lg, a->layer)[nz][nx] || out < 0 || lg->areas[out].room != g_LgRt.outdoor) {
					continue;
				}
				if (!lgStPortal2(lg, alongz, line, alongz ? a->z : a->x, alongz ? a->z + a->d : a->x + a->w,
						side == 0 || side == 2 ? out : i, side == 0 || side == 2 ? i : out, 1, 1, err, errlen)) {
					return 0;
				}
				lg->portals[lg->numportals - 1].window = 1;
			}
		}
	}
	/* the bridges over the streets (layer 1, as a ravine's: level onto the
	 * roofs or landings at their ends, ledges over the street, glass-railed) */
	for (k = 0; k < nlinks; k++) {
		if (!links[k].glass && bridgeat[k][0] >= 0) {
			const int f0 = lgRtFloor(links[k].b0), f1 = lgRtFloor(links[k].b1);
			LgRect rc;
			int gi, pit;

			rc.x = (int16_t)bridgeat[k][0]; rc.z = (int16_t)bridgeat[k][1]; rc.w = (int16_t)bridgeat[k][2]; rc.d = (int16_t)bridgeat[k][3]; rc.room = -1;
			pit = lg->cells[rc.z][rc.x] - 1;
			if (pit < 0) {
				return 0;
			}
			gi = lgLayerPiece(lg, &rc, pit, f0 > f1 ? f0 : f1, 1, err, errlen);
			if (gi == -2 || gi == -1) {
				lgSetErr(err, errlen, "bridge %d not over the street", k, 0);
				return 0;
			}
			lg->areas[gi].theme = LG_RT_BRIDGE_THEME;
			lg->areas[gi].room = (int16_t)g_LgRt.outdoor;
			if (!lgRvPieceEdges(lg, gi, 0, err, errlen)) {
				return 0;
			}
			g_LgRt.bridges++;
		}
	}
	/* the street's openings and the glass's: no railings (nobody stands
	 * there); the roofs' ledges over the street railed, every other one not */
	for (i = 0; i < lg->numportals; i++) {
		LgPortal *pt = &lg->portals[i];
		const LgArea *a = &lg->areas[pt->a], *c = &lg->areas[pt->b];

		if (a->special == LG_SPECIAL_GLASS || c->special == LG_SPECIAL_GLASS) {
			pt->norail = 1;
		} else if ((a->special == LG_SPECIAL_PIT) != (c->special == LG_SPECIAL_PIT) && !a->layer && !c->layer) {
			pt->norail = (uint8_t)(lgRand(r) & 1);
		} else if (a->special == LG_SPECIAL_PIT && c->special == LG_SPECIAL_PIT) {
			pt->norail = 1;
		}
	}
	/* (a glass floor over a drop, one at least: a walkway or a well) */
	for (i = 0; i < lg->numareas && lg->areas[i].special != LG_SPECIAL_GLASS; i++);
	if (i == lg->numareas) {
		lgSetErr(err, errlen, "no glass floor", 0, 0);
		return 0;
	}
	if (!lgAreaGraph(lg, r, err, errlen) || !lgBuildNav(lg, err, errlen) || !lgBuildSpawns(lg, r, err, errlen)) return 0;

	lgPlaceObstacles(lg);
	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];
		if (a->theme == LG_RT_RELAY_THEME) {
			const int cx = lgWorldX(lg, a->x) + a->w * LG_CELL / 2;
			const int cz = lgWorldZ(lg, a->z) + a->d * LG_CELL / 2;
			if (!lgTryProp(lg, i, LG_PROP_DISH, cx, cz, 0, 35, LG_COVER_PADGAP, LG_COVER_GAP)) {
				lgSetErr(err, errlen, "no room for the rooftop relay dish", 0, 0);
				return 0;
			}
			break;
		}
	}
	lgPlaceCover(lg, r);
	return modLevelGenValidate(lg, err, errlen);
}

/* ------------------------------------------------------------------ */
/* Highrise: the penthouse and the building site                        */

/* (2026-11-17; user: "Design two new layouts, both utilizing elevators. One of
 * them should be a skyscraper penthouse with all glass windows you can fall
 * out of and a lot of furniture. The penthouse itself should be at least 3
 * floors. The second layout is up to you, but must have an elevator.")
 *
 * Both are the top of a tower, the rooftops' street far below (a kill floor),
 * three storeys stacked as the rooftops' towers are (lgRtTower: each storey a
 * layer, every cell of one over an undercroft of the one under, one room):
 * - the core: R1, a ramp up from storey 1 to 2 (layer 0, a bay), beside it R2
 *   from storey 2 to 3 (layer 1), a cell of floor between;
 * - the lift: a shaft (a bay on layer 0, LG_HR_SHAFT square) open from just
 *   under storey 1's floor (the platform's pit) to storey 3's ceiling; each
 *   storey's floor stops at its edge, a ledge railed but on one side, the
 *   side the lift is boarded from (lgMezzPortals reaches it down two layers);
 * - the outside walls glass, floor to head height, pane after pane (a cell of
 *   wall between): shoot one out and walk off the edge.
 * The penthouse (LG_LAYOUT_PENTHOUSE): marble, walnut, carpet, furnished
 * heavily (lgHrFurnish: sofas round tables, desks, cabinets, plants, beds), a
 * terrace with a landing pad on one side, its doors in the glass.
 * The site (LG_LAYOUT_SITE): a tower going up, its slabs bare concrete, its
 * outer walls not built yet (the openings have no glass: walk straight off),
 * voids through all three storeys where the slabs are not poured (railed
 * but for a gap), crates, barrels and trolleys of material, a hoist (the
 * lift) up the middle. */
#define LG_HR_RAMPL   6                        /* a ramp's cells: a storey of 300 at 50 a cell */
#define LG_HR_SHAFT   3                        /* the shaft's side */
#define LG_HR_SILL    12                       /* a window's sill: a step, walked over once the glass is gone */
#define LG_HR_WINH    250                      /* a window's (an opening's) height over its storey's floor */
#define LG_HR_DOORH   260
#define LG_HR_PIECE   12                       /* a storey's pieces at most this square */
#define LG_HR_S1_THEME     0
#define LG_HR_S2_THEME     1
#define LG_HR_S3_THEME     2
#define LG_HR_RAMP_THEME   3
#define LG_HR_SHAFT_THEME  4
#define LG_HR_TERRACE_THEME 5
#define LG_HR_PAD_THEME    6
#define LG_HR_STREET_THEME 7
#define LG_HR_VOID_THEME   8
#define LG_HR_ESCAPE_THEME 9
#define LG_HR_ESCAPE_W     4

static const struct {
	int8_t grid, w, d, terrace;
} g_LgHighrise[LG_SIZE_COUNT] = {
	{ 46, 26, 20, 8 },
	{ 52, 30, 22, 9 },
	{ 62, 36, 28, 10 },
};

enum { LG_HR_NONE, LG_HR_FLOORU, LG_HR_FLOOR, LG_HR_R1, LG_HR_R2, LG_HR_SHAFTC, LG_HR_VOID, LG_HR_LOBBYU, LG_HR_LOBBY, LG_HR_DONE = 0x80 };

static struct {
	uint8_t cls[LG_MAXLAYERS][LG_GRIDMAX][LG_GRIDMAX];
	int X, Z, W, D;              /* the inside: X..X+W, Z..Z+D */
	int cu, cv;                  /* the core */
	int su, sv, sside;           /* the shaft, the side it is boarded from (0 -x, 1 +x, 2 -z, 3 +z) */
	int shaft;                   /* its area */
	int street;                  /* the street's room */
	int lobby[LG_MAXLAYERS];     /* each storey's landing in front of the lift's doors */
	uint8_t wall[LG_MAXLAYERS][LG_GRIDMAX][LG_GRIDMAX];   /* the penthouse's partitions, each storey's own */
	int nrooms[LG_MAXLAYERS];
	int dlo, dhi;                /* where storey 1's doors may be along the terrace's side */
	int nvoid;
	int vu[3], vv[3], vw[3], vd[3];
	int windows, doors, furniture;
	int escape[LG_MAXLAYERS], escapeDoor[LG_MAXLAYERS];
} g_LgHr;

static int lgHrFloor(int layer)
{
	return LG_RT_BASE + layer * LG_RT_STOREY;
}

/* rectangles of cls == c on a layer, as big as fit up to LG_HR_PIECE square */
static int lgHrPieces(LgLayout *lg, int layer, int c, int room)
{
	const int f = lgHrFloor(layer);
	int x, z;

	for (z = g_LgHr.Z; z < g_LgHr.Z + g_LgHr.D; z++) {
		for (x = g_LgHr.X; x < g_LgHr.X + g_LgHr.W; x++) {
			int w, d, k, ok;
			LgArea *a;
			LgRect rc;

			if (g_LgHr.cls[layer][z][x] != c) {
				continue;
			}
			for (w = 1; w < LG_HR_PIECE && x + w < g_LgHr.X + g_LgHr.W && g_LgHr.cls[layer][z][x + w] == c; w++);
			for (d = 1, ok = 1; d < LG_HR_PIECE && z + d < g_LgHr.Z + g_LgHr.D && ok; ) {
				for (k = 0; k < w; k++) {
					if (g_LgHr.cls[layer][z + d][x + k] != c) {
						ok = 0;
						break;
					}
				}
				if (ok) {
					d++;
				}
			}
			for (k = 0; k < d; k++) {
				memset(&g_LgHr.cls[layer][z + k][x], c | LG_HR_DONE, (size_t)w);
			}
			if ((a = lgRtArea(lg, x, z, w, d)) == NULL) {
				return 0;
			}
			rc.x = (int16_t)x; rc.z = (int16_t)z; rc.w = (int16_t)w; rc.d = (int16_t)d; rc.room = -1;
			lgSetRoomArea(a, &rc, LG_ROOM_LAB, room, 0, 0);
			a->layer = (uint8_t)layer;
			a->floorlo = a->floor = (int16_t)f;
			a->ceil = (int16_t)(layer == 2 ? f + LG_RT_STOREY : f + LG_RT_STOREY - LG_MEZZ_SLAB);
			a->flags = (uint8_t)((layer ? LG_AREAFLAG_MEZZ : 0) | (c == LG_HR_FLOORU || c == LG_HR_LOBBYU ? LG_AREAFLAG_UNDER : 0));
			if (c == LG_HR_LOBBYU || c == LG_HR_LOBBY) {
				g_LgHr.lobby[layer] = lg->numareas - 1;
			}
			a->theme = (uint8_t)(layer == 0 ? LG_HR_S1_THEME : layer == 1 ? LG_HR_S2_THEME : LG_HR_S3_THEME);
		}
	}
	return 1;
}

/* the core's ramps, the shaft and the voids: one area each */
static LgArea *lgHrBay(LgLayout *lg, int room, int x, int z, int w, int d, int layer, int lo, int hi, int ceil, int ramp, int theme)
{
	LgArea *a = lgRtArea(lg, x, z, w, d);
	LgRect rc;

	if (a == NULL) {
		return NULL;
	}
	rc.x = (int16_t)x; rc.z = (int16_t)z; rc.w = (int16_t)w; rc.d = (int16_t)d; rc.room = -1;
	lgSetRoomArea(a, &rc, LG_ROOM_LAB, room, 0, 0);
	a->layer = (uint8_t)layer;
	a->floorlo = (int16_t)lo;
	a->floor = (int16_t)hi;
	a->ceil = (int16_t)ceil;
	a->ramp = (uint8_t)ramp;
	a->flags = (uint8_t)(LG_AREAFLAG_BAY | (layer ? LG_AREAFLAG_MEZZ : 0));
	a->theme = (uint8_t)theme;
	return a;
}

static int lgHrFree(int u, int v, int w, int d, int gap)
{
	int x, z;

	if (u - gap < 1 || v - gap < 1 || u + w + gap > g_LgHr.W - 1 || v + d + gap > g_LgHr.D - 1) {
		return 0;
	}
	for (z = v - gap; z < v + d + gap; z++) {
		for (x = u - gap; x < u + w + gap; x++) {
			if (g_LgHr.cls[0][g_LgHr.Z + z][g_LgHr.X + x] != LG_HR_FLOORU) {
				return 0;
			}
		}
	}
	return 1;
}

static void lgHrSet(int layer, int u, int v, int w, int d, int c)
{
	int x, z;

	for (z = v; z < v + d; z++) {
		for (x = u; x < u + w; x++) {
			g_LgHr.cls[layer][g_LgHr.Z + z][g_LgHr.X + x] = (uint8_t)c;
		}
	}
}

/* one storey's openings along one side of the outside wall: windows (the
 * penthouse's glass, the site's open edges) three cells wide, a cell of wall
 * between, wherever the floor inside is level the whole width; the
 * terrace's doors among them on storey 1 */
static int lgHrOpenings(LgLayout *lg, LgRng *r, int layer, int side, int doors)
{
	const int f = lgHrFloor(layer);
	const int alongz = side < 2;
	const int len = alongz ? g_LgHr.D : g_LgHr.W;
	const int line = side == 0 ? g_LgHr.X - 1 : side == 1 ? g_LgHr.X + g_LgHr.W : side == 2 ? g_LgHr.Z - 1 : g_LgHr.Z + g_LgHr.D;
	const int inl = side == 0 ? line + 1 : side == 1 ? line - 1 : side == 2 ? line + 1 : line - 1;
	const int base = alongz ? g_LgHr.Z : g_LgHr.X;
	int u, k, ndoor = 0;
	const int doorat0 = doors ? lgRange(r, (g_LgHr.dlo - base) > 2 ? g_LgHr.dlo - base : 2, len / 2 - 2) : -99, doorat1 = doors ? lgRange(r, len / 2 + 1, (g_LgHr.dhi - base - 4) < len - 5 ? g_LgHr.dhi - base - 4 : len - 5) : -99;

	for (u = 1; u + 3 <= len - 1; u += 4) {
		int ok = 1, door;

		if (lg->theme.layout == LG_LAYOUT_PENTHOUSE && side == 3) {
			const int end = 1 + LG_MAXLAYERS * LG_HR_ESCAPE_W + (LG_MAXLAYERS - 1) * LG_HR_RAMPL;
			if (u < end) continue;
		}
		for (k = 0; k < 3 && ok; k++) {
			const int cx = alongz ? inl : base + u + k, cz = alongz ? base + u + k : inl;
			const int c = g_LgHr.cls[layer][cz][cx] & ~LG_HR_DONE;

			ok = c == LG_HR_FLOORU || c == LG_HR_FLOOR;
		}
		if (!ok) {
			continue;
		}
		door = doors && layer == 0 && ((u <= doorat0 && doorat0 < u + 4) || (u <= doorat1 && doorat1 < u + 4) || (ndoor == 0 && u + 7 >= len - 1));
		/* (onto the terrace's floor only, all three cells) */
		if (door && (base + u < g_LgHr.dlo || base + u + 3 > g_LgHr.dhi)) {
			door = 0;
		}
		{
			LgRect rc;
			LgArea *a;

			if (lg->numareas >= LG_MAXAREAS - 4) {
				return 0;
			}
			rc.x = (int16_t)(alongz ? line : base + u); rc.z = (int16_t)(alongz ? base + u : line);
			rc.w = (int16_t)(alongz ? 1 : 3); rc.d = (int16_t)(alongz ? 3 : 1); rc.room = -1;
			lgStCorridor(lg, &rc, door ? f : f + LG_HR_SILL, door ? f : f + LG_HR_SILL, 0, 0, 0);
			a = &lg->areas[lg->numareas - 1];
			a->layer = (uint8_t)layer;
			if (door) {
				a->ceil = (int16_t)(f + LG_HR_DOORH);
				ndoor++;
				g_LgHr.doors++;
			} else {
				a->ceil = (int16_t)(f + LG_HR_SILL + LG_HR_WINH);
				a->special = LG_SPECIAL_WINDOW;
				g_LgHr.windows++;
			}
			a->theme = (uint8_t)(layer == 0 ? LG_HR_S1_THEME : layer == 1 ? LG_HR_S2_THEME : LG_HR_S3_THEME);
		}
	}
	return !doors || layer || ndoor > 0;
}


/* An exterior flight between each pair of floors along the rear facade. */
static int lgHrFireEscape(LgLayout *lg)
{
	const int z = g_LgHr.Z + g_LgHr.D;
	int x = g_LgHr.X + 1;

	for (int layer = 0; layer < LG_MAXLAYERS; layer++) {
		const int y = lgHrFloor(layer);
		int x0 = x, width = 3;

		/* A partition may meet the facade. Open the clear part of the landing. */
		for (; width >= 2; width--) {
			for (x0 = x; x0 + width <= x + LG_HR_ESCAPE_W; x0++) {
				int k;
				for (k = 0; k < width; k++) {
					const int c = g_LgHr.cls[layer][z - 1][x0 + k] & ~LG_HR_DONE;
					if (c != LG_HR_FLOORU && c != LG_HR_FLOOR) break;
				}
				if (k == width) break;
			}
			if (x0 + width <= x + LG_HR_ESCAPE_W) break;
		}
		if (width < 2) return 0;
		g_LgHr.escapeDoor[layer] = lg->numareas;
		LgArea *door = lgHrBay(lg, -1, x0, z, width, 1, layer, y, y, y + LG_HR_DOORH, LG_RAMP_NONE, LG_HR_ESCAPE_THEME);
		if (!door) return 0;
		door->kind = LG_AREA_CORRIDOR;
		door->flags = layer ? LG_AREAFLAG_MEZZ : 0;

		g_LgHr.escape[layer] = lg->numareas;
		LgArea *landing = lgRtOutdoor(lg, x, z + 1, LG_HR_ESCAPE_W, 3, y, y, LG_RAMP_NONE, LG_HR_ESCAPE_THEME);
		if (!landing) return 0;
		landing->special = LG_SPECIAL_FIREESCAPE;
		x += LG_HR_ESCAPE_W;
		if (layer + 1 < LG_MAXLAYERS) {
			LgArea *ramp = lgRtOutdoor(lg, x, z + 1, LG_HR_RAMPL, 3, y, y + LG_RT_STOREY, LG_RAMP_XP, LG_HR_ESCAPE_THEME);
			if (!ramp) return 0;
			ramp->special = LG_SPECIAL_FIREESCAPE;
			x += LG_HR_RAMPL;
		}
	}
	lgRtMark(g_LgHr.X + 1, z + 1, x - g_LgHr.X - 1, 3);
	return 1;
}

/* Furniture over a storey's open floor, not just its walls: a sitting group
 * (sofa facing a table, armchairs either side), a desk and its chair, a bed,
 * a cabinet back to back with another, plants; or (the site) stacks of
 * material. On a loose grid, each spot a group or nothing. */
static void lgHrFurnish(LgLayout *lg, LgRng *r, int site)
{
	static const int8_t groups[][4][4] = {
		/* prop, dx, dz (in 10s), turn; -1 ends (each 60 clear of the next: the validator's gap) */
		{ { LG_PROP_REDSOFA, 0, -11, 0 }, { LG_PROP_CITABLE, 0, 7, 0 }, { LG_PROP_REDARM, -24, 7, 1 }, { LG_PROP_REDARM, 24, 7, 3 } },
		{ { LG_PROP_DDDESK, 0, 0, 0 }, { LG_PROP_G5CHAIR, 0, -14, 0 }, { LG_PROP_PLANTRUBBER, 19, 0, 0 }, { -1 } },
		{ { LG_PROP_BED, 0, 0, 0 }, { LG_PROP_LAMP, 19, -8, 0 }, { LG_PROP_LAMP, -19, -8, 0 }, { -1 } },
		{ { LG_PROP_STONEDESK, 0, 0, 0 }, { LG_PROP_VILLACHAIR, -8, -17, 0 }, { LG_PROP_VILLACHAIR, 8, -17, 0 }, { -1 } },
		{ { LG_PROP_CICABINET, 0, -9, 0 }, { LG_PROP_CICABINET, 0, 9, 2 }, { -1 } },
		{ { LG_PROP_ARMCHAIR, -10, 0, 1 }, { LG_PROP_ARMCHAIR, 10, 0, 3 }, { LG_PROP_PLANTSPIDER, 0, 18, 0 }, { -1 } },
		{ { LG_PROP_DIVIDER, 0, 0, 0 }, { LG_PROP_PLANTSPIDER, 25, 0, 0 }, { LG_PROP_PLANTSPIDER, -25, 0, 0 }, { -1 } },
	};
	static const int8_t stacks[][4][4] = {
		{ { 0, 0, 0, 0 }, { 0, 17, 0, 0 }, { LG_PROP_BARREL, -15, 0, 0 }, { -1 } },
		{ { LG_PROP_BORGCRATE, 0, 0, 0 }, { LG_PROP_BARREL, 16, 0, 0 }, { -1 } },
		{ { LG_PROP_TROLLEY, 0, 0, 1 }, { 0, 0, 16, 0 }, { -1 } },
		{ { LG_PROP_HOVERCRATE, 0, 0, 0 }, { LG_PROP_HOVERCRATE, 17, 0, 0 }, { LG_PROP_BARREL, 0, 16, 0 }, { -1 } },
		{ { LG_PROP_A51TABLE, 0, 0, 0 }, { LG_PROP_CICABINET, 0, 17, 2 }, { -1 } },
		{ { LG_PROP_DUMPSTER, 0, 0, 0 }, { -1 } },
	};
	int ai;

	for (ai = 0; ai < lg->numareas; ai++) {
		const LgArea *a = &lg->areas[ai];
		int32_t x, z;

		if (a->room != 0 || a->ramp || a->special || (a->flags & LG_AREAFLAG_BAY) || a->w < 3 || a->d < 3) {   /* (the start too: its pads are kept clear) */
			continue;
		}
		for (z = lgWorldZ(lg, a->z) + 170; z <= lgWorldZ(lg, a->z + a->d) - 170; z += 330) {
			for (x = lgWorldX(lg, a->x) + 170; x <= lgWorldX(lg, a->x + a->w) - 170; x += 330) {
				int tryg, placed = 0;

				if (lgRange(r, 0, 99) >= (site ? 45 : 85)) {
					continue;
				}
				/* (a group that does not fit here: another, up to four) */
				for (tryg = 0; tryg < 4 && !placed; tryg++) {
					const int g = lgRange(r, 0, site ? 5 : 6);
					const int8_t (*grp)[4] = site ? stacks[g] : groups[g];
					const int turn = lgRange(r, 0, 3);
					int k;

					for (k = 0; k < 4 && grp[k][0] >= 0; k++) {
						/* the group turned with its first piece */
						const int dx = grp[k][1] * 10, dz = grp[k][2] * 10;
						const int rx = turn == 0 ? dx : turn == 1 ? -dz : turn == 2 ? -dx : dz;
						const int rz = turn == 0 ? dz : turn == 1 ? dx : turn == 2 ? -dz : -dx;
						const int32_t px = x + rx, pz = z + rz;

						if (px < lgWorldX(lg, a->x) + 60 || px > lgWorldX(lg, a->x + a->w) - 60 || pz < lgWorldZ(lg, a->z) + 60 || pz > lgWorldZ(lg, a->z + a->d) - 60) {
							continue;
						}
						if (lgTryProp(lg, ai, grp[k][0], px, pz, (turn + grp[k][3]) & 3, 30, LG_COVER_PADGAP, 62)) {
							g_LgHr.furniture++;
							placed++;
						} else if (k == 0) {
							break;   /* (the group's main piece: without it, none of it) */
						}
					}
				}
			}
		}
	}
}

/* A storey of the penthouse parted into two to four rooms by walls a cell
 * thick across it (one; two side by side; one across the other; or one with
 * a second off it to one side), each length of wall with a doorway three
 * wide (two on a long one). The walls keep clear of the core (the ramps and
 * their landings), the lift's walls and lobby; each storey's are its own:
 * a wall stands on the floor of the storey under it (lgTryHighrise). */
static int lgHrLineOk(int vertical, int at, int from, int to)
{
	int k;

	for (k = from; k < to; k++) {
		const int u = vertical ? at : k, v = vertical ? k : at;
		const int x = g_LgHr.X + u, z = g_LgHr.Z + v;
		int l;

		for (l = 0; l < LG_MAXLAYERS; l++) {
			const int c = g_LgHr.cls[l][z][x] & ~LG_HR_DONE;

			if (c != LG_HR_NONE && c != LG_HR_FLOORU && c != LG_HR_FLOOR) {
				return 0;
			}
		}
		/* (two clear of the core's landings and the lift's lobby) */
		if (u >= g_LgHr.cu - 1 && u < g_LgHr.cu + LG_HR_RAMPL + 3 && v >= g_LgHr.cv - 1 && v < g_LgHr.cv + 8) {
			return 0;
		}
		if (u >= g_LgHr.su - 3 && u < g_LgHr.su + LG_HR_SHAFT + 3 && v >= g_LgHr.sv - 3 && v < g_LgHr.sv + LG_HR_SHAFT + 3) {
			return 0;
		}
	}
	return 1;
}

static void lgHrWallRun(LgRng *r, int layer, int vertical, int at, int from, int to)
{
	const int len = to - from;
	const int ndoor = len >= 16 ? 2 : 1;
	int k, dpos[2];

	for (k = from; k < to; k++) {
		g_LgHr.wall[layer][g_LgHr.Z + (vertical ? k : at)][g_LgHr.X + (vertical ? at : k)] = 1;
	}
	/* the doorways: 3 wide, a cell clear of either end */
	if (ndoor == 1) {
		dpos[0] = lgRange(r, from + 1, to - 4);
	} else {
		dpos[0] = lgRange(r, from + 1, from + len / 2 - 4);
		dpos[1] = lgRange(r, from + len / 2 + 1, to - 4);
	}
	for (k = 0; k < ndoor; k++) {
		int j;

		for (j = dpos[k]; j < dpos[k] + 3; j++) {
			g_LgHr.wall[layer][g_LgHr.Z + (vertical ? j : at)][g_LgHr.X + (vertical ? at : j)] = 0;
		}
	}
}

static int lgHrPartition(LgRng *r, int layer)
{
	const int W = g_LgHr.W, D = g_LgHr.D;
	int tries;

	for (tries = 0; tries < 300; tries++) {
		const int plan = lgRange(r, 0, 3);   /* 0: 2 rooms; 1: 3 side by side; 2: 4; 3: 3, one wall off another */
		const int vertical = W >= D ? 1 : lgRange(r, 0, 1);
		const int span = vertical ? W : D, other = vertical ? D : W;
		const int a = lgRange(r, 4, span - 5);
		int b = -1, x, z, n = 0;

		memset(g_LgHr.wall[layer], 0, sizeof(g_LgHr.wall[layer]));
		if (!lgHrLineOk(vertical, a, 0, other)) {
			continue;
		}
		if (plan == 1) {
			b = lgRange(r, 4, span - 5);
			if (b - a < 6 && a - b < 6) {
				continue;
			}
			if (!lgHrLineOk(vertical, b, 0, other)) {
				continue;
			}
			lgHrWallRun(r, layer, vertical, a, 0, other);
			lgHrWallRun(r, layer, vertical, b, 0, other);
			n = 3;
		} else if (plan >= 2) {
			/* the cross wall: across the whole storey (4 rooms) or one side of the first (3) */
			const int c = lgRange(r, 5, other - 6);   /* (each run of the first wall 5 or more) */
			const int side = lgRange(r, 0, 1);
			const int from = plan == 2 || side == 0 ? 0 : a + 1, to = plan == 2 || side == 1 ? span : a;

			if (to - from < 6 || !lgHrLineOk(!vertical, c, from, to)) {
				continue;
			}
			lgHrWallRun(r, layer, vertical, a, 0, c);
			lgHrWallRun(r, layer, vertical, a, c + 1, other);
			if (plan == 2) {
				lgHrWallRun(r, layer, !vertical, c, 0, a);
				lgHrWallRun(r, layer, !vertical, c, a + 1, span);
				/* (the crossing itself) */
				g_LgHr.wall[layer][g_LgHr.Z + (vertical ? c : a)][g_LgHr.X + (vertical ? a : c)] = 1;
				n = 4;
			} else {
				lgHrWallRun(r, layer, !vertical, c, from, to);
				g_LgHr.wall[layer][g_LgHr.Z + (vertical ? c : a)][g_LgHr.X + (vertical ? a : c)] = 1;
				n = 3;
			}
		} else {
			lgHrWallRun(r, layer, vertical, a, 0, other);
			n = 2;
		}
		for (z = g_LgHr.Z; z < g_LgHr.Z + D; z++) {
			for (x = g_LgHr.X; x < g_LgHr.X + W; x++) {
				if (g_LgHr.wall[layer][z][x]) {
					g_LgHr.cls[layer][z][x] = LG_HR_NONE;
				}
			}
		}
		g_LgHr.nrooms[layer] = n;
		return 1;
	}
	return 0;
}

static int lgTryHighrise(LgLayout *lg, const LgParams *p, LgRng *r, char *err, int errlen)
{
	const int site = lg->theme.layout == LG_LAYOUT_SITE;
	const int G = lg->gw, size = lg->size, L = LG_HR_RAMPL, S = LG_HR_SHAFT;
	const int W = g_LgHighrise[size].w, D = g_LgHighrise[size].d, T = site ? 5 : g_LgHighrise[size].terrace;
	const int f1 = lgHrFloor(0), f2 = lgHrFloor(1), f3 = lgHrFloor(2), c3 = f3 + LG_RT_STOREY;
	int i, k, l, x, z, tries, room = 0, nliftdoor = 0;

	(void)p;
	memset(&g_LgHr, 0, sizeof(g_LgHr));
	memset(&g_LgRt, 0, sizeof(g_LgRt));
	lg->numareas = 0;
	lg->numrooms = 0;
	g_LgHr.W = W; g_LgHr.D = D;
	g_LgHr.X = (G - W) / 2;
	g_LgHr.Z = (G - (D + T + 1)) / 2 + T + 1;

	/* storey 1: all floor, then the core, the shaft, the voids taken out */
	lgHrSet(0, 0, 0, W, D, LG_HR_FLOORU);
	for (tries = 0; tries < 60; tries++) {
		const int cu = lgRange(r, 2, W - L - 4), cv = lgRange(r, 2, D - 9);

		if (lgHrFree(cu, cv, L + 2, 7, 0)) {
			g_LgHr.cu = cu; g_LgHr.cv = cv;
			lgHrSet(0, cu, cv, L + 2, 7, LG_HR_NONE);   /* (marked for now) */
			break;
		}
	}
	if (tries == 60) {
		return 0;
	}
	for (tries = 0; tries < 200; tries++) {
		const int su = lgRange(r, 3, W - S - 3), sv = lgRange(r, 3, D - S - 3);

		if (lgHrFree(su, sv, S, S, 2)) {
			const int sd = lgRange(r, 0, 3);
			/* the lobby: the two rows in front of the boarding side */
			const int lu = sd == 0 ? su - 2 : sd == 1 ? su + S : su, lv = sd == 2 ? sv - 2 : sd == 3 ? sv + S : sv;
			const int lw = sd < 2 ? 2 : S, ld = sd < 2 ? S : 2;

			g_LgHr.su = su; g_LgHr.sv = sv; g_LgHr.sside = sd;
			/* (2026-11-17: an enclosed shaft, a lift car in it and a sliding
			 * door on each storey, as dataDyne's lifts are: a wall round it
			 * but where its doors are) */
			lgHrSet(0, su - 1, sv - 1, S + 2, S + 2, LG_HR_NONE);
			lgHrSet(0, su, sv, S, S, LG_HR_SHAFTC);
			lgHrSet(0, lu, lv, lw, ld, LG_HR_LOBBYU);
			break;
		}
	}
	if (tries == 200) {
		lgSetErr(err, errlen, "no room for the lift", 0, 0);
		return 0;
	}
	if (site) {
		const int want = lgRange(r, 1, size == LG_SIZE_SMALL ? 1 : 2);

		for (tries = 0; tries < 200 && g_LgHr.nvoid < want; tries++) {
			const int vw = lgRange(r, 3, 5), vd = lgRange(r, 3, 5);
			const int vu = lgRange(r, 3, W - vw - 3), vv = lgRange(r, 3, D - vd - 3);

			if (lgHrFree(vu, vv, vw, vd, 2)) {
				lgHrSet(0, vu, vv, vw, vd, LG_HR_VOID);
				g_LgHr.vu[g_LgHr.nvoid] = vu; g_LgHr.vv[g_LgHr.nvoid] = vv;
				g_LgHr.vw[g_LgHr.nvoid] = vw; g_LgHr.vd[g_LgHr.nvoid] = vd;
				g_LgHr.nvoid++;
			}
		}
	}
	/* the core: R1 v cv..cv+2, R2 over v cv+4..cv+6, floor round them */
	lgHrSet(0, g_LgHr.cu, g_LgHr.cv, L + 2, 7, LG_HR_FLOORU);
	lgHrSet(0, g_LgHr.cu + 1, g_LgHr.cv, L, 3, LG_HR_R1);
	/* storeys 2 and 3 over the undercrofts of the ones under */
	for (l = 1; l < LG_MAXLAYERS; l++) {
		for (z = g_LgHr.Z; z < g_LgHr.Z + D; z++) {
			for (x = g_LgHr.X; x < g_LgHr.X + W; x++) {
				const int c0 = g_LgHr.cls[l - 1][z][x];

				g_LgHr.cls[l][z][x] = c0 == LG_HR_FLOORU ? (l == 2 ? LG_HR_FLOOR : LG_HR_FLOORU)
					: c0 == LG_HR_LOBBYU ? (l == 2 ? LG_HR_LOBBY : LG_HR_LOBBYU) : LG_HR_NONE;
			}
		}
	}
	lgHrSet(1, g_LgHr.cu + 1, g_LgHr.cv + 4, L, 3, LG_HR_R2);
	for (z = g_LgHr.Z; z < g_LgHr.Z + D; z++) {
		for (x = g_LgHr.X; x < g_LgHr.X + W; x++) {
			if (g_LgHr.cls[1][z][x] == LG_HR_R2) {
				g_LgHr.cls[2][z][x] = LG_HR_NONE;
			}
		}
	}
	/* the penthouse's storeys: two to four rooms each, every storey its own
	 * partitions (lgHrPartition) */
	if (!site) {
		for (l = 0; l < LG_MAXLAYERS; l++) {
			if (!lgHrPartition(r, l)) {
				lgSetErr(err, errlen, "storey %d would not part into rooms", l + 1, 0);
				return 0;
			}
		}
	}

	/* the areas: storey 1's (the biggest piece the room's main, first) */
	/* (one room, the three storeys: as a rooftop tower's, so that the game
	 * draws and finds them as one; its main the biggest piece of storey 1) */
	lg->numrooms = 1;
	if (!lgHrPieces(lg, 0, LG_HR_FLOORU, room)) {
		return 0;
	}
	{
		int best = 0;

		for (i = 1; i < lg->numareas; i++) {
			if (lg->areas[i].w * lg->areas[i].d > lg->areas[best].w * lg->areas[best].d) {
				best = i;
			}
		}
		if (best) {
			const LgArea t = lg->areas[0];

			lg->areas[0] = lg->areas[best];
			lg->areas[best] = t;
		}
	}
	if (!lgHrPieces(lg, 0, LG_HR_LOBBYU, room) || !lgHrPieces(lg, 1, LG_HR_FLOORU, room) || !lgHrPieces(lg, 1, LG_HR_LOBBYU, room)
			|| !lgHrPieces(lg, 2, LG_HR_FLOOR, room) || !lgHrPieces(lg, 2, LG_HR_LOBBY, room)) {
		return 0;
	}
	if (!lgHrBay(lg, room, g_LgHr.X + g_LgHr.cu + 1, g_LgHr.Z + g_LgHr.cv, L, 3, 0, f1, f2, f3 - LG_MEZZ_SLAB, LG_RAMP_XP, LG_HR_RAMP_THEME)
			|| !lgHrBay(lg, room, g_LgHr.X + g_LgHr.cu + 1, g_LgHr.Z + g_LgHr.cv + 4, L, 3, 1, f2, f3, c3, LG_RAMP_XN, LG_HR_RAMP_THEME)) {
		return 0;
	}
	g_LgHr.shaft = lg->numareas;
	if (!lgHrBay(lg, room, g_LgHr.X + g_LgHr.su, g_LgHr.Z + g_LgHr.sv, S, S, 0, f1, f1, c3, 0, LG_HR_SHAFT_THEME)) {
		return 0;
	}
	for (k = 0; k < g_LgHr.nvoid; k++) {
		if (!lgHrBay(lg, room, g_LgHr.X + g_LgHr.vu[k], g_LgHr.Z + g_LgHr.vv[k], g_LgHr.vw[k], g_LgHr.vd[k], 0, f1, f1, c3, 0, LG_HR_VOID_THEME)) {
			return 0;
		}
	}

	/* the outdoors: the terrace (the penthouse's), its landing pad, and the
	 * street far below round it all */
	/* (room 1: the terrace, or the site's loading deck; room 2: the street) */
	g_LgRt.outdoor = lg->numrooms++;
	lgRtMark(g_LgHr.X - 1, g_LgHr.Z - 1, W + 2, D + 2);
	{
		const int px = lgRange(r, g_LgHr.X + 1, g_LgHr.X + W - 9), pz = g_LgHr.Z - 1 - T + 1;
		LgArea *a;

		/* the penthouse's pad: 7 x 7 against the terrace's outer edge */
		if (!site) {
			if ((a = lgRtOutdoor(lg, px, pz, 7, 7, f1, f1, 0, LG_HR_PAD_THEME)) == NULL) {
				return 0;
			}
			lgRtMark(px, pz, 7, 7);
		}
		g_LgHr.dlo = g_LgHr.X + (site ? W / 4 : 0);
		g_LgHr.dhi = g_LgHr.X + W - (site ? W / 4 : 0);
		if (!lgRtFill(lg, g_LgHr.X - 1 + (site ? W / 4 : 0), g_LgHr.Z - 1 - T, g_LgHr.X + W + 1 - (site ? W / 4 : 0), g_LgHr.Z - 1, f1, 0, LG_HR_TERRACE_THEME, 1)) {
			return 0;
		}
	}
	if (!site && !lgHrFireEscape(lg)) {
		lgSetErr(err, errlen, "no clear fire escape doorway", 0, 0);
		return 0;
	}
	g_LgHr.street = g_LgRt.outdoor = lg->numrooms++;
	if (!lgRtFill(lg, 1, 1, G - 1, G - 1, LG_RT_STREET, LG_SPECIAL_PIT, LG_HR_STREET_THEME, 0)) {
		lgSetErr(err, errlen, "too many street pieces (%d areas)", lg->numareas, 0);
		return 0;
	}
	/* the openings in the outside walls, every storey; the doors onto the terrace */
	for (l = 0; l < LG_MAXLAYERS; l++) {
		for (k = 0; k < 4; k++) {
			if (!lgHrOpenings(lg, r, l, k, l == 0 && k == 2)) {
				lgSetErr(err, errlen, "no doors onto the terrace", 0, 0);
				return 0;
			}
		}
	}

	/* the cells */
	memset(lg->cells, 0, sizeof(lg->cells));
	memset(lg->cells2, 0, sizeof(lg->cells2));
	memset(lg->cells3, 0, sizeof(lg->cells3));
	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];

		for (z = a->z; z < a->z + a->d; z++) {
			for (x = a->x; x < a->x + a->w; x++) {
				if (LG_CELLS(lg, a->layer)[z][x]) {
					lgSetErr(err, errlen, "area %d overlaps another", i, 0);
					return 0;
				}
				LG_CELLS(lg, a->layer)[z][x] = (int16_t)(i + 1);
			}
		}
	}

	/* openings: each storey's, the ledges over the core, shaft and voids */
	for (g_LgPortalLayer = 0; g_LgPortalLayer < LG_MAXLAYERS; g_LgPortalLayer++) {
		if (!lgFindPortals(lg, r, err, errlen)) {
			g_LgPortalLayer = 0;
			return 0;
		}
	}
	g_LgPortalLayer = 0;
	if (!lgMezzPortals(lg, err, errlen)) {
		return 0;
	}
	if (!site) {
		for (l = 1; l < LG_MAXLAYERS; l++) {
			const LgArea *door = &lg->areas[g_LgHr.escapeDoor[l]];
			if (!lgStPortal2(lg, 0, door->z + 1, door->x, door->x + door->w,
					g_LgHr.escapeDoor[l], g_LgHr.escape[l], 0, 0, err, errlen)) return 0;
		}
	}
	/* the upper storeys' windows: out onto nothing (the street far below) */
	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];
		int side;

		if (a->special != LG_SPECIAL_WINDOW || !a->layer) {
			continue;
		}
		for (side = 0; side < 4; side++) {
			const int alongz = side < 2;
			const int nx = side == 0 ? a->x - 1 : side == 1 ? a->x + a->w : a->x;
			const int nz = side == 2 ? a->z - 1 : side == 3 ? a->z + a->d : a->z;
			const int line = side == 0 ? a->x : side == 1 ? a->x + a->w : side == 2 ? a->z : a->z + a->d;
			const int len = alongz ? a->d : a->w, at0 = alongz ? a->z : a->x;
			int q, run;

			if (len != 3 || LG_CELLS(lg, a->layer)[nz][nx] || lg->cells[nz][nx] < 1 || lg->areas[lg->cells[nz][nx] - 1].room < 1) {
				continue;
			}
			/* (one opening a piece of the outdoors along it: the pad and the terrace) */
			for (q = 0; q < len; q = run) {
				const int out = alongz ? lg->cells[nz + q][nx] - 1 : lg->cells[nz][nx + q] - 1;

				for (run = q + 1; run < len && (alongz ? lg->cells[nz + run][nx] : lg->cells[nz][nx + run]) - 1 == out; run++);
				if (out < 0 || lg->areas[out].room < 1) {
					continue;
				}
				if (!lgStPortal2(lg, alongz, line, at0 + q, at0 + run,
						side == 0 || side == 2 ? out : i, side == 0 || side == 2 ? i : out, 1, 1, err, errlen)) {
					return 0;
				}
				lg->portals[lg->numportals - 1].window = 1;
			}
		}
	}
	/* railings: none on the street's edges (nobody stands there), the
	 * terrace's edge over the street railed, the shaft's ledges railed but on
	 * its boarding side, a void's ledges as lgMezzPortals made them */
	for (i = 0; i < lg->numportals; i++) {
		LgPortal *pt = &lg->portals[i];
		const LgArea *a = &lg->areas[pt->a], *c = &lg->areas[pt->b];

		if (!site && (a->theme == LG_HR_ESCAPE_THEME || c->theme == LG_HR_ESCAPE_THEME)) {
			pt->door = 0;
		}
		if (a->special == LG_SPECIAL_PIT && c->special == LG_SPECIAL_PIT) {
			pt->norail = 1;
		} else if (a->special == LG_SPECIAL_WINDOW || c->special == LG_SPECIAL_WINDOW) {
			pt->norail = 1;   /* (the glass is the railing) */
		} else if (pt->a == g_LgHr.shaft || pt->b == g_LgHr.shaft) {
			/* the lift's doors: a storey's lobby onto the shaft (door 2 + the storey) */
			const int other = pt->a == g_LgHr.shaft ? pt->b : pt->a;
			const int st = lg->areas[other].layer;

			if (other != g_LgHr.lobby[st] || nliftdoor & (1 << st)) {
				lgSetErr(err, errlen, "the shaft opens onto area %d", other, 0);
				return 0;
			}
			nliftdoor |= 1 << st;
			pt->door = (uint8_t)(LG_DOOR_LIFT + st);
			pt->norail = 1;
		}
	}
	if (nliftdoor != 7) {
		lgSetErr(err, errlen, "the lift has doors on storeys %x", nliftdoor, 0);
		return 0;
	}
	/* the lift: a stop on each storey */
	{
		LgLift *lf = &lg->lifts[lg->numlifts++];

		lf->area = (int16_t)g_LgHr.shaft;
		lf->side = (int16_t)g_LgHr.sside;
		lf->nstops = 3;
		lf->stop[0] = (int16_t)(f1 + 2); lf->stop[1]   /* (just over the shaft's floor at the bottom) */ = (int16_t)f2; lf->stop[2] = (int16_t)f3;
	}
	if (!lgAreaGraph(lg, r, err, errlen) || !lgBuildNav(lg, err, errlen) || !lgBuildSpawns(lg, r, err, errlen)) {
		return 0;
	}
	lgPlaceObstacles(lg);
	lgHrFurnish(lg, r, site);
	lgPlaceCover(lg, r);
	return modLevelGenValidate(lg, err, errlen);
}

/* ------------------------------------------------------------------ */
/* Pipeworks (2026-11-19)                                               */
/* ------------------------------------------------------------------ */

/* The user: "Note the compact map, the death floor, the pipes going over
 * it, the way higher stories can also be dropped down onto this pipe
 * crossed death floor, and the multiple ways to get up including automatic
 * elevator platforms and ladders. Also the cross crossing catwalks that drop
 * onto lower floors. Create your own layout ... with custom textures. There
 * must be pipes to walk on."
 *
 * One sealed hall (room 0) round a sump of sludge: the pit, a kill floor at
 * LG_PW_PIT. Round the sump a band of floor at three heights, L1 L2 L3
 * (LG_PW_STEP apart):
 * - the south side all at L1, but for the gantry: a block at L3 on the
 *   sump's edge, reached by an automatic lift (a platform going up and down
 *   on its own, no doors), a ladder up its back and the high catwalks;
 * - the west and east sides L1 to the south, L2 to the north: stairs (a
 *   ramp) up the outer edge, beside them a wall, and on the sump's side the
 *   step from L1 to L2 a ledge with a ladder;
 * - the north side L2 at its corners, L3 in the middle: stairs up the outer
 *   edge at each end, and on the sump's side the same ledges, one laddered.
 * Over the sump, layer 1: the pipes (LG_SPECIAL_PIPE: big pipes, walked
 * along their tops, nothing to hold) at L1 from the west side to the east,
 * now and then a short pipe between two of them; the low catwalks at L2,
 * west to east too, over the sump's north part. Layer 2: the high catwalks
 * at L3, north to south from the north side's middle to the gantry, over the
 * pipes and across over the low catwalks. The catwalks are railed with
 * gaps; every ledge over something lower is a drop-down onto it (the
 * pipes, the catwalks, or the sludge). Off the hall through doorways: the
 * pump room (west, L1), the control room (east, L2), the boiler gallery
 * (north, L3) and the store (south, L1). */
#define LG_PW_PIT    0
#define LG_PW_L1     480
#define LG_PW_STEP   320
#define LG_PW_L2     (LG_PW_L1 + LG_PW_STEP)
#define LG_PW_L3     (LG_PW_L1 + 2 * LG_PW_STEP)
#define LG_PW_CEIL   (LG_PW_L3 + 640)
#define LG_PW_RAMP   8        /* cells: stairs up a step (40 a cell) */
#define LG_PW_ROOMH  300
#define LG_PW_G1_THEME     0
#define LG_PW_G2_THEME     1
#define LG_PW_G3_THEME     2
#define LG_PW_STAIR_THEME  3
#define LG_PW_PIPE_THEME   4
#define LG_PW_SUMP_THEME   5
#define LG_PW_CAT_THEME    6
#define LG_PW_LIFT_THEME   7
#define LG_PW_PUMP_THEME   8
#define LG_PW_CTRL_THEME   9
#define LG_PW_BOIL_THEME   10
#define LG_PW_STORE_THEME  11

static const struct {
	uint8_t grid, pw, pd, band, rd;
} g_LgPipe[LG_SIZE_COUNT] = {
	{ 48, 20, 18, 6, 5 },
	{ 54, 24, 22, 7, 5 },
	{ 64, 30, 26, 8, 6 },
};

enum { PWC_WALL, PWC_G1, PWC_G2, PWC_G3, PWC_PIT, PWC_TAKEN };

#define LG_PW_MAXRUN 8
static struct {
	int hx, hz, W, D, B, pw, pd, vsplit, rs, a1, b0;
	int npipes, pipev[LG_PW_MAXRUN];
	int ncross;
	int nlow, lowv[LG_PW_MAXRUN];
	int nhigh, highu[LG_PW_MAXRUN];
	int s0, s1, shaftu, shaft;
	int pipes, crosses, lowcats, highcats, ladders, drops, gaps, furniture;
	uint8_t cls[LG_GRIDMAX][LG_GRIDMAX];
	uint8_t snap[LG_GRIDMAX][LG_GRIDMAX];   /* cls before the pieces are cut from it: the ceilings look at it */
	int pumpz, ctrlz, boildu, storedu;      /* where the side rooms' doorways are, in the hall's cells */
	int ladu, walls, bulks;                       /* walled runs and bulkheads put in */
} g_LgPw;

static void lgPwSet(int u, int v, int c)
{
	g_LgPw.cls[g_LgPw.hz + v][g_LgPw.hx + u] = (uint8_t)c;
}

static void lgPwRect(int u, int v, int w, int d, int c)
{
	int x, z;

	for (z = v; z < v + d; z++) {
		for (x = u; x < u + w; x++) {
			lgPwSet(x, z, c);
		}
	}
}

/* Enclosed hallways (2026-11-19, the user: "The pipes level is far too open,
 * there should be more cielings on the different stories, and enclosed
 * hallways with minimal sightlines"). The outer three cells of the hall's
 * band, all the way round, are a hallway with a ceiling of its own: a wall a
 * cell thick (LG_PW_CORR in) between it and the gallery on the sump's edge,
 * with a doorway two wide every so often, and across the hallway a bulkhead
 * every nine or ten cells, its doorway two of the three wide, on alternate
 * sides. The hallway climbs by the same stairs as the band did and the side
 * rooms' doors open off it. */
#define LG_PW_CORR 3
#define LG_PW_LOWC 400   /* a ceiling this far over a floor that has nothing higher beside it */

static int lgPwAt(int u, int v)
{
	return g_LgPw.cls[g_LgPw.hz + v][g_LgPw.hx + u];
}

/* a wall along the line (v = line for alongx, u = line otherwise) from a to b (exclusive), over
 * floor class c only, a doorway two wide every nine to twelve cells, one at the very least */
static void lgPwWallRun(LgRng *r, int alongx, int line, int a, int b, int c)
{
	int t, gap = a + lgRange(r, 2, 6), gaps = 0;

	if (b - a < 4) {
		return;
	}
	for (t = a; t < b; t++) {
		const int u = alongx ? t : line, v = alongx ? line : t;

		if (t == gap || t == gap + 1) {
			if (t == gap + 1) {
				gap = t + 1 + lgRange(r, 7, 10);
				gaps++;
			}
			continue;
		}
		if (lgPwAt(u, v) == c) {
			lgPwSet(u, v, PWC_WALL);
		}
	}
	if (gaps == 0 && b - a >= 6) {
		/* (the first doorway fell past the end: one in the middle) */
		const int m = a + (b - a) / 2;

		for (t = m; t < m + 2; t++) {
			if (lgPwAt(alongx ? t : line, alongx ? line : t) == PWC_WALL) {
				lgPwSet(alongx ? t : line, alongx ? line : t, c);
			}
		}
	}
	g_LgPw.walls++;
}

/* a bulkhead across a hallway three wide at position pos along it (alongx: the hallway runs along x, at rows
 * v0..v0+2), a doorway two of the three, alternating sides by k, over floor class c only */
static void lgPwBulkhead(int alongx, int pos, int v0, int k, int c)
{
	int q;

	for (q = 0; q < LG_PW_CORR; q++) {
		const int open = (k & 1) ? q >= 1 : q < 2;
		const int u = alongx ? pos : v0 + q, v = alongx ? v0 + q : pos;

		if (!open && lgPwAt(u, v) == c) {
			lgPwSet(u, v, PWC_WALL);
		}
	}
	g_LgPw.bulks++;
}

/* bulkheads along a hallway from lo to hi (exclusive), none within a cell or two of an avoided doorway */
static void lgPwBulkheads(LgRng *r, int alongx, int v0, int lo, int hi, int c, int avoid)
{
	int pos = lo + lgRange(r, 0, 3), k = lgRange(r, 0, 1);

	for (; pos < hi; pos += 9 + lgRange(r, 0, 2), k++) {
		if (avoid >= 0 && pos >= avoid - 2 && pos <= avoid + 3) {
			continue;
		}
		lgPwBulkhead(alongx, pos, v0, k, c);
	}
}

/* what ceiling a piece of the hall's ground has: tall (the hall's) if a higher floor is beside it (a
 * ladder goes up there, past the ceiling), else LG_PW_LOWC over the floor; stairs LG_PW_LOWC over their top */
static int lgPwCeilFor(int x, int z, int w, int d, int lo, int hi, int ramp)
{
	int i, higher = 0, cl = lo == LG_PW_L1 ? PWC_G1 : lo == LG_PW_L2 ? PWC_G2 : PWC_G3;

	if (ramp) {
		return hi + LG_PW_LOWC;
	}
	for (i = 0; i < w + d && !higher; i++) {
		const int xs[4] = { x - 1, x + w, x + (i < w ? i : 0), x + (i < w ? i : 0) };
		const int zs[4] = { z + (i < d ? i : 0), z + (i < d ? i : 0), z - 1, z + d };
		int k;

		for (k = 0; k < 4; k++) {
			if (xs[k] >= 0 && zs[k] >= 0 && xs[k] < LG_GRIDMAX && zs[k] < LG_GRIDMAX) {
				const int n = g_LgPw.snap[zs[k]][xs[k]];

				higher |= (n == PWC_G2 || n == PWC_G3) && n > cl && cl >= PWC_G1;
			}
		}
	}
	return higher ? LG_PW_CEIL : lo + LG_PW_LOWC;
}

static int lgPwFloorOf(int c)
{
	return c == PWC_G1 ? LG_PW_L1 : c == PWC_G2 ? LG_PW_L2 : LG_PW_L3;
}

/* an area of the hall on the ground (layer 0): floor lo..hi, a ramp or not */
static LgArea *lgPwGround(LgLayout *lg, int x, int z, int w, int d, int lo, int hi, int ramp, int theme)
{
	LgArea *a = lgRtArea(lg, x, z, w, d);
	LgRect rc;

	if (a == NULL) {
		return NULL;
	}
	rc.x = (int16_t)x; rc.z = (int16_t)z; rc.w = (int16_t)w; rc.d = (int16_t)d; rc.room = -1;
	lgSetHubArea(a, &rc, 0, lo, hi, ramp);
	a->ceil = (int16_t)lgPwCeilFor(x, z, w, d, lo, hi, ramp);
	a->theme = (uint8_t)theme;
	lgMezzFill(lg, a, lg->numareas - 1);
	return a;
}

/* the hall's floor of class c: pieces as big as fit, up to LG_HR_PIECE */
static int lgPwPieces(LgLayout *lg, int c, int theme)
{
	const int f = lgPwFloorOf(c);
	int x, z;

	for (z = 0; z < lg->gd; z++) {
		for (x = 0; x < lg->gw; x++) {
			int w, d, k, ok;

			if (g_LgPw.cls[z][x] != c) {
				continue;
			}
			for (w = 1; w < LG_HR_PIECE && x + w < lg->gw && g_LgPw.cls[z][x + w] == c; w++);
			for (d = 1, ok = 1; d < LG_HR_PIECE && z + d < lg->gd && ok; ) {
				for (k = 0; k < w; k++) {
					if (g_LgPw.cls[z + d][x + k] != c) {
						ok = 0;
						break;
					}
				}
				if (ok) {
					d++;
				}
			}
			for (k = 0; k < d; k++) {
				memset(&g_LgPw.cls[z + k][x], PWC_TAKEN, (size_t)w);
			}
			if (lgPwGround(lg, x, z, w, d, f, f, 0, theme) == NULL) {
				return 0;
			}
		}
	}
	return 1;
}

/* a side room at x z (w x d) at floor f, its doorway (dx dz, dw x dd) through the hall's wall */
static int lgPwRoom(LgLayout *lg, int x, int z, int w, int d, int f, int type, int theme, int dx, int dz, int dw, int dd)
{
	const int room = lg->numrooms++;
	LgArea *a = lgRtArea(lg, x, z, w, d);
	LgRect rc;

	if (a == NULL) {
		return 0;
	}
	rc.x = (int16_t)x; rc.z = (int16_t)z; rc.w = (int16_t)w; rc.d = (int16_t)d; rc.room = -1;
	lgSetRoomArea(a, &rc, type, room, 0, LG_PW_ROOMH);
	a->floor = a->floorlo = (int16_t)f;
	a->ceil = (int16_t)(f + LG_PW_ROOMH);
	a->theme = (uint8_t)theme;
	lgMezzFill(lg, a, lg->numareas - 1);
	if (lg->numareas >= LG_MAXAREAS - 4) {
		return 0;
	}
	rc.x = (int16_t)dx; rc.z = (int16_t)dz; rc.w = (int16_t)dw; rc.d = (int16_t)dd; rc.room = -1;
	lgStCorridor(lg, &rc, f, f, 0, 0, 0);
	a = &lg->areas[lg->numareas - 1];
	a->ceil = (int16_t)(f + LG_HR_DOORH);
	a->theme = (uint8_t)theme;
	lgMezzFill(lg, a, lg->numareas - 1);
	return 1;
}

/* A piece over the sump on layer 1 or 2 at floor f: over the sludge, and on
 * layer 2 over what layer 1 has there with headroom; -1 if not, -2 if full */
static int lgPwPiece(LgLayout *lg, int layer, int x, int z, int w, int d, int f, int special, int theme)
{
	const int gi = lg->numareas;
	LgArea *g;
	int cx, cz;

	for (cz = z; cz < z + d; cz++) {
		for (cx = x; cx < x + w; cx++) {
			const int below = lg->cells[cz][cx] - 1, mid = lg->cells2[cz][cx] - 1;

			if (below < 0 || lg->areas[below].special != LG_SPECIAL_PIT || LG_CELLS(lg, layer)[cz][cx]
					|| (layer == 2 && mid >= 0 && lg->areas[mid].floor + LG_MEZZ_SLAB + LG_MEZZ_HEAD > f)
					|| (layer == 1 && lg->cells3[cz][cx])) {
				return -1;
			}
		}
	}
	if (gi >= LG_MAXAREAS - 4) {
		return -2;
	}
	g = &lg->areas[lg->numareas++];
	memset(g, 0, sizeof(*g));
	g->x = (int16_t)x; g->z = (int16_t)z; g->w = (int16_t)w; g->d = (int16_t)d;
	g->kind = LG_AREA_ROOM;
	g->roomtype = LG_ROOM_HALL;
	g->theme = (uint8_t)theme;
	g->room = 0;
	g->floor = g->floorlo = (int16_t)f;
	g->ceil = LG_PW_CEIL;
	g->layer = (uint8_t)layer;
	g->special = (uint8_t)special;
	g->dist = -1;
	g->centrepad = -1;
	lgMezzFill(lg, g, gi);
	return gi;
}

/* What a piece (layer 1 or 2) has beside it at cell x z: 1 a piece of its
 * own layer level with it made before it, 2 a floor level with it under its
 * layer, 3 a lower floor (a drop onto it), 0 nothing to open onto (or the
 * other's to open) */
static int lgPwBeside(const LgLayout *lg, int gi, int x, int z, int *other)
{
	const LgArea *g = &lg->areas[gi];
	int l, o;

	*other = -1;
	if (x < 0 || z < 0 || x >= lg->gw || z >= lg->gd) {
		return 0;
	}
	o = LG_CELLS(lg, g->layer)[z][x] - 1;
	if (o >= 0) {
		*other = o;
		if (lg->areas[o].floor == g->floor) {
			return o < gi ? 1 : 0;
		}
		return lg->areas[o].floor < g->floor && !lg->areas[o].ramp ? 3 : 0;
	}
	for (l = g->layer - 1; l >= 0; l--) {
		o = LG_CELLS(lg, l)[z][x] - 1;
		if (o >= 0) {
			*other = o;
			if (lg->areas[o].ramp || lg->areas[o].kind != LG_AREA_ROOM || lg->areas[o].room != g->room) {
				return 0;
			}
			if (lg->areas[o].floor == g->floor) {
				return 2;
			}
			return lg->areas[o].floor < g->floor ? 3 : 0;
		}
	}
	return 0;
}

/* A piece's openings, by what is beside it: level onto a piece or floor
 * (the floor's ledge over the sludge cut back there), drops onto anything
 * lower; rails 1: the drops railed in runs of three, every third open */
static int lgPwEdges(LgLayout *lg, int gi, int rails, char *err, int errlen)
{
	const LgArea g = lg->areas[gi];
	int side, seg = 0;

	for (side = 0; side < 4; side++) {
		const int alongz = side < 2, n = alongz ? g.d : g.w;
		const int line = side == 0 ? g.x : side == 1 ? g.x + g.w : side == 2 ? g.z : g.z + g.d;
		const int base = alongz ? g.z : g.x;
		const int lowo = side == 0 || side == 2;   /* the other is on the low side */
		int i = 0;

		while (i < n) {
			int kind, other, j, k;
#define LG_PW_NX(q) (side == 0 ? g.x - 1 : side == 1 ? g.x + g.w : g.x + (q))
#define LG_PW_NZ(q) (side == 2 ? g.z - 1 : side == 3 ? g.z + g.d : g.z + (q))
#define LG_PW_IX(q) (side == 0 ? g.x : side == 1 ? g.x + g.w - 1 : g.x + (q))
#define LG_PW_IZ(q) (side == 2 ? g.z : side == 3 ? g.z + g.d - 1 : g.z + (q))
			kind = lgPwBeside(lg, gi, LG_PW_NX(i), LG_PW_NZ(i), &other);
			for (j = i + 1; j < n; j++) {
				int o2;

				if (lgPwBeside(lg, gi, LG_PW_NX(j), LG_PW_NZ(j), &o2) != kind || o2 != other) {
					break;
				}
			}
			if (kind == 2 && !lg->areas[other].layer) {
				/* the floor's ledge over the sludge, where the piece meets it */
				for (k = i; k < j; k++) {
					const int pit = lg->cells[LG_PW_IZ(k)][LG_PW_IX(k)] - 1;

					if (pit >= 0 && !lgRvCut(lg, alongz, line, base + k, base + k + 1, other, pit, err, errlen)) {
						return 0;
					}
				}
			}
			if (kind == 3 && rails) {
				for (k = i; k < j; k += 3) {
					const int e = k + 3 < j ? k + 3 : j, gap = seg++ % 3 == 2;

					if (!lgStPortal2(lg, alongz, line, base + k, base + e, lowo ? other : gi, lowo ? gi : other, 1, gap, err, errlen)) {
						return 0;
					}
					g_LgPw.gaps += gap;
				}
				g_LgPw.drops++;
			} else if (kind) {
				if (!lgStPortal2(lg, alongz, line, base + i, base + j, lowo ? other : gi, lowo ? gi : other, kind == 3, kind == 3, err, errlen)) {
					return 0;
				}
				g_LgPw.drops += kind == 3;
			}
#undef LG_PW_NX
#undef LG_PW_NZ
#undef LG_PW_IX
#undef LG_PW_IZ
			i = j;
		}
	}
	return 1;
}

/* a ladder up the face on the line (x or z) between cells lo..hi */
static void lgPwLadder(LgLayout *lg, int alongz, int line, int at, int facing, int low, int high)
{
	LgLadder *ld;

	if (lg->numladders >= LG_MAXLADDERS || low < 0 || high < 0) {
		return;
	}
	ld = &lg->ladders[lg->numladders++];
	ld->alongz = (uint8_t)alongz;
	ld->line = (int16_t)line;
	ld->at = (int16_t)at;
	ld->facing = (uint8_t)facing;
	ld->low = (int16_t)low;
	ld->high = (int16_t)high;
	ld->lo = lg->areas[low].floor;
	ld->hi = lg->areas[high].floor;
	g_LgPw.ladders++;
}

/* the portal on the line between a and b over cell at, left open (no rail) */
static void lgPwOpenAt(LgLayout *lg, int alongz, int line, int at, int a, int b)
{
	int i;

	for (i = 0; i < lg->numportals; i++) {
		LgPortal *p = &lg->portals[i];
		const int pal = p->x0 == p->x1, pline = pal ? p->x0 : p->z0;
		const int p0 = pal ? p->z0 : p->x0, p1 = pal ? p->z1 : p->x1;

		if (pal == alongz && pline == line && p0 <= at && p1 > at && ((p->a == a && p->b == b) || (p->a == b && p->b == a))) {
			p->norail = 1;
		}
	}
}

/* Things on the floors: crates and barrels of the works' stores, drums and
 * cabinets in the rooms; the props' gap 62 */
static void lgPwFurnish(LgLayout *lg, LgRng *r)
{
	static const int8_t yard[][4][4] = {
		{ { 0, 0, 0, 0 }, { 1, 17, 0, 0 }, { -1 } },
		{ { LG_PROP_BARREL, 0, 0, 0 }, { LG_PROP_BARREL, 13, 0, 0 }, { LG_PROP_BARREL, 6, 12, 0 }, { -1 } },
		{ { LG_PROP_HOVERCRATE, 0, 0, 0 }, { -1 } },
		{ { 2, 0, 0, 0 }, { LG_PROP_BARREL, 16, 0, 0 }, { -1 } },
	};
	static const int8_t room[][4][4] = {
		{ { LG_PROP_CICABINET, 0, 0, 0 }, { LG_PROP_CICABINET, 13, 0, 0 }, { -1 } },
		{ { LG_PROP_DDDESK, 0, 0, 0 }, { LG_PROP_G5CHAIR, 0, -14, 0 }, { -1 } },
		{ { LG_PROP_LABTABLE, 0, 0, 0 }, { -1 } },
		{ { LG_PROP_BARREL, 0, 0, 0 }, { LG_PROP_BARREL, 13, 0, 0 }, { -1 } },
		{ { LG_PROP_TROLLEY, 0, 0, 0 }, { -1 } },
	};
	int ai;

	for (ai = 0; ai < lg->numareas; ai++) {
		const LgArea *a = &lg->areas[ai];
		const int8_t (*set)[4][4];
		int nset, step, chance;
		int32_t x, z;

		if (a->ramp || a->special || a->layer || a->kind != LG_AREA_ROOM || a->w < 3 || a->d < 3 || ai == g_LgPw.shaft) {
			continue;
		}
		if (a->room == 0) {
			set = yard; nset = 4; step = 420; chance = 40;
		} else {
			set = room; nset = 5; step = 260; chance = 75;
		}
		for (z = lgWorldZ(lg, a->z) + 130; z <= lgWorldZ(lg, a->z + a->d) - 130; z += step) {
			for (x = lgWorldX(lg, a->x) + 130; x <= lgWorldX(lg, a->x + a->w) - 130; x += step) {
				const int8_t (*grp)[4];
				int k, turn;

				if (lgRange(r, 0, 99) >= chance) {
					continue;
				}
				grp = set[lgRange(r, 0, nset - 1)];
				turn = lgRange(r, 0, 3);
				for (k = 0; k < 4 && grp[k][0] >= 0; k++) {
					const int dx = grp[k][1] * 10, dz = grp[k][2] * 10;
					const int rx = turn == 0 ? dx : turn == 1 ? -dz : turn == 2 ? -dx : dz;
					const int rz = turn == 0 ? dz : turn == 1 ? dx : turn == 2 ? -dz : -dx;
					const int32_t px = x + rx, pz = z + rz;

					if (px < lgWorldX(lg, a->x) + 60 || px > lgWorldX(lg, a->x + a->w) - 60 || pz < lgWorldZ(lg, a->z) + 60 || pz > lgWorldZ(lg, a->z + a->d) - 60) {
						if (k == 0) break;
						continue;
					}
					if (lgTryProp(lg, ai, grp[k][0], px, pz, (turn + grp[k][3]) & 3, 30, LG_COVER_PADGAP, 62)) {
						g_LgPw.furniture++;
					} else if (k == 0) {
						break;
					}
				}
			}
		}
	}
}

/* the portals in order of the two areas they join, then of where (the game
 * merges a room's screen box only over a run of portals to the same room:
 * a catwalk's many openings onto the sludge, in among its openings onto the
 * pipes and catwalks under it, left the sludge drawn in a sliver) */
static int lgPwPortalCmp(const void *pa, const void *pb)
{
	const LgPortal *a = (const LgPortal *)pa, *b = (const LgPortal *)pb;
	const int a0 = a->a < a->b ? a->a : a->b, a1 = a->a < a->b ? a->b : a->a;
	const int b0 = b->a < b->b ? b->a : b->b, b1 = b->a < b->b ? b->b : b->a;

	if (a0 != b0) return a0 - b0;
	if (a1 != b1) return a1 - b1;
	if (a->x0 != b->x0) return a->x0 - b->x0;
	if (a->z0 != b->z0) return a->z0 - b->z0;
	if (a->x1 != b->x1) return a->x1 - b->x1;
	return a->z1 - b->z1;
}

static int lgTryPipeworks(LgLayout *lg, const LgParams *p, LgRng *r, char *err, int errlen)
{
	const int size = lg->size, G = lg->gw;
	const int B = g_LgPipe[size].band, PW = g_LgPipe[size].pw, PD = g_LgPipe[size].pd, RD = g_LgPipe[size].rd;
	const int W = PW + 2 * B, D = PD + 2 * B, M = RD + 3;   /* (rooms LG_ROOMGAP off the hall: its wall two thick) */
	int i, k, t, u, v, low, high;
	int hx, hz;

	(void)p;
	memset(&g_LgRt, 0, sizeof(g_LgRt));
	memset(&g_LgPw, 0, sizeof(g_LgPw));
	lg->numareas = 0;
	lg->numrooms = 1;
	lg->numladders = 0;
	memset(lg->cells, 0, sizeof(lg->cells));
	memset(lg->cells2, 0, sizeof(lg->cells2));
	memset(lg->cells3, 0, sizeof(lg->cells3));
	if (M + W + M > G || M + D + M > G) {
		lgSetErr(err, errlen, "the works do not fit the grid (%d)", G, 0);
		return 0;
	}
	hx = g_LgPw.hx = M + (G - 2 * M - W) / 2;
	hz = g_LgPw.hz = M + (G - 2 * M - D) / 2;
	g_LgPw.W = W; g_LgPw.D = D; g_LgPw.B = B; g_LgPw.pw = PW; g_LgPw.pd = PD;
	g_LgPw.vsplit = B + PD / 2;
	g_LgPw.rs = g_LgPw.vsplit - LG_PW_RAMP / 2;
	g_LgPw.a1 = B;
	g_LgPw.b0 = W - B - LG_PW_RAMP;

	/* the band, and the sump in it */
	lgPwRect(0, 0, W, D, PWC_G1);
	lgPwRect(B, B, PW, PD, PWC_PIT);
	{
		const int vs = g_LgPw.vsplit, rs = g_LgPw.rs, R = LG_PW_RAMP;
		int side;

		for (side = 0; side < 2; side++) {
			const int uo = side ? W - 3 : 0, uw = side ? W - 4 : 3, ui = side ? W - B : 4, iw = B - 4;

			lgPwRect(uo, rs + R, 3, D - rs - R, PWC_G2);      /* outer: L2 north of the stairs */
			lgPwRect(uo, rs, 3, R, PWC_TAKEN);                /* the stairs */
			lgPwRect(uw, rs, 1, R, PWC_WALL);                 /* the wall beside them */
			lgPwRect(uw, rs + R, 1, D - rs - R, PWC_G2);
			lgPwRect(ui, vs, iw, D - vs, PWC_G2);             /* inner: L2 north of the step */
		}
		/* the north side's middle: L3, its stairs at each end of the outer strip */
		{
			const int a1 = g_LgPw.a1, b0 = g_LgPw.b0, vi = B + PD, vw = D - 4, vo = D - 3;

			lgPwRect(a1, vi, W - 2 * B, D - vi, PWC_G2);
			lgPwRect(a1 + R / 2, vi, b0 - a1, vw - vi, PWC_G3);   /* inner */
			lgPwRect(a1 + R, vw, b0 - a1 - R, 4, PWC_G3);          /* the wall row's and outer strip's middle */
			lgPwRect(a1, vo, R, 3, PWC_TAKEN);
			lgPwRect(b0, vo, R, 3, PWC_TAKEN);
			lgPwRect(a1, vw, R, 1, PWC_WALL);
			lgPwRect(b0, vw, R, 1, PWC_WALL);
		}
	}

	/* over the sump: the pipes' rows (south), the low catwalks' (north), the high catwalks' columns */
	for (t = B + 1 + (size ? lgRange(r, 0, 1) : 0); t <= g_LgPw.vsplit - 3 && g_LgPw.npipes < LG_PW_MAXRUN; t += 2 + lgRange(r, 2, 3)) {
		g_LgPw.pipev[g_LgPw.npipes++] = t;
	}
	for (t = g_LgPw.vsplit + 1 + lgRange(r, 0, 1); t <= B + PD - 3 && g_LgPw.nlow < LG_PW_MAXRUN; t += 2 + lgRange(r, 2, 4)) {
		g_LgPw.lowv[g_LgPw.nlow++] = t;
	}
	{
		const int lo = g_LgPw.a1 + LG_PW_RAMP / 2 + 1, hi = g_LgPw.b0 + LG_PW_RAMP / 2 - 3;
		const int want = size == 2 ? 3 : 2;

		for (t = lgRange(r, lo, lo + 2); t <= hi && g_LgPw.nhigh < want; t += 2 + lgRange(r, 3, 5)) {
			g_LgPw.highu[g_LgPw.nhigh++] = t;
		}
	}
	if (g_LgPw.npipes < 2 || g_LgPw.nlow < 1 || g_LgPw.nhigh < 2) {
		lgSetErr(err, errlen, "too few pipes or catwalks (%d %d)", g_LgPw.npipes, g_LgPw.nhigh);
		return 0;
	}

	/* the gantry: L3 on the sump's south edge under the high catwalks' ends;
	 * the lift beside it, a cell back from the sump */
	g_LgPw.s0 = g_LgPw.highu[0] - 1;
	g_LgPw.s1 = g_LgPw.highu[g_LgPw.nhigh - 1] + 3;
	lgPwRect(g_LgPw.s0, B - 3, g_LgPw.s1 - g_LgPw.s0, 3, PWC_G3);
	{
		const int west = lgRange(r, 0, 1);
		const int su = west ? g_LgPw.s0 - 3 : g_LgPw.s1;

		if (su < 1 || su + 3 > W - 1) {
			lgSetErr(err, errlen, "no room for the lift", 0, 0);
			return 0;
		}
		g_LgPw.shaftu = su;
		lgPwRect(su, B - 4, 3, 3, PWC_TAKEN);
		lgPwRect(su, B - 1, 3, 1, PWC_WALL);
	}

	/* where the side rooms' doorways go (drawn here: the hallways' bulkheads keep clear of them) */
	{
		const int rs = g_LgPw.rs, R = LG_PW_RAMP, rw = 7;
		const int m0 = g_LgPw.a1 + R, m1 = g_LgPw.b0;

		g_LgPw.pumpz = lgRange(r, 1, rs - 1 - rw > 1 ? rs - 1 - rw : 1);
		g_LgPw.ctrlz = lgRange(r, rs + R + 1, D - 1 - rw > rs + R + 1 ? D - 1 - rw : rs + R + 1);
		g_LgPw.boildu = lgRange(r, m0, m1 - 2);
		g_LgPw.storedu = lgRange(r, B, W - B - 2);
	}

	/* the enclosed hallways: the outer three cells all round, walled from the galleries, with bulkheads */
	{
		const int rs = g_LgPw.rs, R = LG_PW_RAMP, C = LG_PW_CORR, rw = 7;
		const int a1 = g_LgPw.a1, b0 = g_LgPw.b0, vw = D - 4;

		/* the walls: south, west and east on the ground floor, then up the L2 north halves and across the north */
		lgPwWallRun(r, 1, C, C, W - C, PWC_G1);
		/* (the gantry's ladder climbs from a doorway in it) */
		g_LgPw.ladu = lgRange(r, g_LgPw.s0 + 1, g_LgPw.s1 - 2);
		{
			int q;

			for (q = g_LgPw.ladu; q < g_LgPw.ladu + 2 && q < g_LgPw.s1; q++) {
				if (lgPwAt(q, C) == PWC_WALL) {
					lgPwSet(q, C, PWC_G1);
				}
			}
		}
		lgPwWallRun(r, 0, C, C, rs, PWC_G1);
		lgPwWallRun(r, 0, W - C - 1, C, rs, PWC_G1);
		lgPwWallRun(r, 0, C, rs + R, vw, PWC_G2);
		lgPwWallRun(r, 0, W - C - 1, rs + R, vw, PWC_G2);
		lgPwWallRun(r, 1, vw, C, a1, PWC_G2);
		lgPwWallRun(r, 1, vw, b0 + R, W - C, PWC_G2);
		lgPwWallRun(r, 1, vw, a1 + R, b0, PWC_G3);
		/* the bulkheads across them */
		lgPwBulkheads(r, 1, 0, C + 3, W - C - 3, PWC_G1, g_LgPw.storedu);
		lgPwBulkheads(r, 0, 0, C + 3, rs - 3, PWC_G1, g_LgPw.pumpz + rw / 2);
		lgPwBulkheads(r, 0, W - C, C + 3, rs - 3, PWC_G1, -1);
		lgPwBulkheads(r, 0, 0, rs + R + 3, vw - 3, PWC_G2, -1);
		lgPwBulkheads(r, 0, W - C, rs + R + 3, vw - 3, PWC_G2, g_LgPw.ctrlz + rw / 2);
		lgPwBulkheads(r, 1, D - C, a1 + R + 3, b0 - 3, PWC_G3, g_LgPw.boildu);
	}
	memcpy(g_LgPw.snap, g_LgPw.cls, sizeof(g_LgPw.snap));

	/* the areas: the floors of each height, the stairs, the lift's shaft, the sump */
	if (!lgPwPieces(lg, PWC_G1, LG_PW_G1_THEME) || !lgPwPieces(lg, PWC_G2, LG_PW_G2_THEME) || !lgPwPieces(lg, PWC_G3, LG_PW_G3_THEME)) {
		lgSetErr(err, errlen, "too many floor pieces (%d)", lg->numareas, 0);
		return 0;
	}
	{
		const int rs = g_LgPw.rs, R = LG_PW_RAMP;

		if (!lgPwGround(lg, hx, hz + rs, 3, R, LG_PW_L1, LG_PW_L2, LG_RAMP_ZP, LG_PW_STAIR_THEME)
				|| !lgPwGround(lg, hx + W - 3, hz + rs, 3, R, LG_PW_L1, LG_PW_L2, LG_RAMP_ZP, LG_PW_STAIR_THEME)
				|| !lgPwGround(lg, hx + g_LgPw.a1, hz + D - 3, R, 3, LG_PW_L2, LG_PW_L3, LG_RAMP_XP, LG_PW_STAIR_THEME)
				|| !lgPwGround(lg, hx + g_LgPw.b0, hz + D - 3, R, 3, LG_PW_L2, LG_PW_L3, LG_RAMP_XN, LG_PW_STAIR_THEME)) {
			return 0;
		}
	}
	g_LgPw.shaft = lg->numareas;
	if (!lgHrBay(lg, 0, hx + g_LgPw.shaftu, hz + B - 4, LG_HR_SHAFT, LG_HR_SHAFT, 0, LG_PW_L1, LG_PW_L1, LG_PW_CEIL, 0, LG_PW_LIFT_THEME)) {
		return 0;
	}
	lg->areas[g_LgPw.shaft].flags = LG_AREAFLAG_HUB;   /* (not a bay: it opens on all its sides) */
	lgMezzFill(lg, &lg->areas[g_LgPw.shaft], g_LgPw.shaft);
	{
		const int first = lg->numareas;

		if (!lgRtFill(lg, hx + B, hz + B, hx + B + PW, hz + B + PD, LG_PW_PIT, LG_SPECIAL_PIT, LG_PW_SUMP_THEME, 0)) {
			lgSetErr(err, errlen, "too many sump pieces (%d)", lg->numareas, 0);
			return 0;
		}
		for (i = first; i < lg->numareas; i++) {
			lg->areas[i].open = 0;
			lg->areas[i].ceil = LG_PW_CEIL;
			lg->areas[i].room = 0;
			lg->areas[i].flags = LG_AREAFLAG_HUB;
		}
	}

	/* the rooms off the hall, each through a doorway in its wall */
	{
		const int rs = g_LgPw.rs, R = LG_PW_RAMP;
		int rw, rz, rx;

		/* the pump room, west, at L1 (beside the south half's outer strip) */
		rw = 7; rz = g_LgPw.pumpz;
		if (!lgPwRoom(lg, hx - 2 - RD, hz + rz, RD, rw, LG_PW_L1, LG_ROOM_HALL, LG_PW_PUMP_THEME, hx - 2, hz + rz + rw / 2 - 1, 2, 2)) return 0;
		/* the control room, east, at L2 */
		rz = g_LgPw.ctrlz;
		if (!lgPwRoom(lg, hx + W + 2, hz + rz, RD, rw, LG_PW_L2, LG_ROOM_LAB, LG_PW_CTRL_THEME, hx + W, hz + rz + rw / 2 - 1, 2, 2)) return 0;
		/* the boiler gallery, north, at L3 (its doorway in the outer strip's L3 middle) */
		{
			const int du = g_LgPw.boildu;

			rw = 8;
			rx = du + 1 - rw / 2;
			if (!lgPwRoom(lg, hx + rx, hz + D + 2, rw, RD, LG_PW_L3, LG_ROOM_HALL, LG_PW_BOIL_THEME, hx + du, hz + D, 2, 2)) return 0;
		}
		/* the store, south, at L1 */
		{
			const int du = g_LgPw.storedu;

			rw = 8;
			rx = du + 1 - rw / 2;
			if (rx < -M + 2) rx = -M + 2;
			if (rx + rw > W + M - 2) rx = W + M - 2 - rw;
			if (!lgPwRoom(lg, hx + rx, hz - 2 - RD, rw, RD, LG_PW_L1, LG_ROOM_STORAGE, LG_PW_STORE_THEME, hx + du, hz - 2, 2, 2)) return 0;
		}
	}

	/* the openings on the ground */
	g_LgPortalLayer = 0;
	if (!lgFindPortals(lg, r, err, errlen)) {
		return 0;
	}

	/* the pipes (layer 1, L1), west to east */
	for (k = 0; k < g_LgPw.npipes; k++) {
		const int gi = lgPwPiece(lg, 1, hx + B, hz + g_LgPw.pipev[k], PW, 2, LG_PW_L1, LG_SPECIAL_PIPE, LG_PW_PIPE_THEME);

		if (gi < 0) {
			lgSetErr(err, errlen, "pipe %d not over the sump", k, 0);
			return 0;
		}
		g_LgPw.pipes++;
	}
	/* the short pipes between two of them */
	for (k = 0; k + 1 < g_LgPw.npipes; k++) {
		const int z0 = g_LgPw.pipev[k] + 2, z1 = g_LgPw.pipev[k + 1];

		if (z1 - z0 >= 2 && (lgRange(r, 0, 99) < 70 || (k + 2 == g_LgPw.npipes && !g_LgPw.crosses))) {
			const int cu = lgRange(r, B + 2, B + PW - 4);
			const int gi = lgPwPiece(lg, 1, hx + cu, hz + z0, 2, z1 - z0, LG_PW_L1, LG_SPECIAL_PIPE, LG_PW_PIPE_THEME);

			if (gi == -2) return 0;
			g_LgPw.crosses += gi >= 0;
		}
	}
	/* the low catwalks (layer 1, L2), west to east */
	for (k = 0; k < g_LgPw.nlow; k++) {
		const int gi = lgPwPiece(lg, 1, hx + B, hz + g_LgPw.lowv[k], PW, 2, LG_PW_L2, LG_SPECIAL_BRIDGE, LG_PW_CAT_THEME);

		if (gi < 0) {
			lgSetErr(err, errlen, "low catwalk %d not over the sump", k, 0);
			return 0;
		}
		g_LgPw.lowcats++;
	}
	/* the high catwalks (layer 2, L3), north to south over the rest */
	for (k = 0; k < g_LgPw.nhigh; k++) {
		const int gi = lgPwPiece(lg, 2, hx + g_LgPw.highu[k], hz + B, 2, PD, LG_PW_L3, LG_SPECIAL_BRIDGE, LG_PW_CAT_THEME);

		if (gi < 0) {
			lgSetErr(err, errlen, "high catwalk %d not over the sump", k, 0);
			return 0;
		}
		g_LgPw.highcats++;
	}
	/* their openings: each piece in turn (the higher of two makes the drop between) */
	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];

		if (a->layer && (a->special == LG_SPECIAL_PIPE || a->special == LG_SPECIAL_BRIDGE)) {
			if (!lgPwEdges(lg, i, a->special == LG_SPECIAL_BRIDGE, err, errlen)) {
				return 0;
			}
		}
	}

	/* rails: the floors' ledges over the sludge in runs of three, every third
	 * open; a step down onto lower floor open (a drop-down); the sludge's own none */
	{
		const int n0 = lg->numportals;

		for (i = 0; i < n0; i++) {
			LgPortal *pt = &lg->portals[i];
			const LgArea *a = &lg->areas[pt->a], *c = &lg->areas[pt->b];

			if (!pt->drop || a->layer || c->layer) {
				continue;
			}
			if (a->special == LG_SPECIAL_PIT && c->special == LG_SPECIAL_PIT) {
				pt->norail = 1;
			} else if ((a->special == LG_SPECIAL_PIT) != (c->special == LG_SPECIAL_PIT)) {
				const int alongz = pt->x0 == pt->x1, p0 = alongz ? pt->z0 : pt->x0, p1 = alongz ? pt->z1 : pt->x1;
				int q, seg = lgRange(r, 0, 2);

				/* (split into runs of three) */
				for (q = p0 + 3; q < p1; q += 3) {
					LgPortal *n;

					if (lg->numportals >= LG_MAXPORTALS) {
						break;
					}
					n = &lg->portals[lg->numportals++];
					*n = *pt;
					if (alongz) { n->z0 = (int16_t)q; n->z1 = (int16_t)(q + 3 < p1 ? q + 3 : p1); }
					else { n->x0 = (int16_t)q; n->x1 = (int16_t)(q + 3 < p1 ? q + 3 : p1); }
					n->norail = (uint8_t)(++seg % 3 == 0);
					pt = &lg->portals[i];
				}
				if (alongz) pt->z1 = (int16_t)(p0 + 3 < p1 ? p0 + 3 : p1); else pt->x1 = (int16_t)(p0 + 3 < p1 ? p0 + 3 : p1);
				pt->norail = 0;
			} else {
				pt->norail = 1;
			}
		}
	}

	/* the lift: an automatic platform from L1 to the gantry */
	{
		LgLift *lf = &lg->lifts[lg->numlifts++];

		lf->area = (int16_t)g_LgPw.shaft;
		lf->side = (int16_t)(g_LgPw.shaftu < g_LgPw.s0 ? 1 : 0);
		lf->nstops = 2;
		lf->stop[0] = (int16_t)(LG_PW_L1 + 2);
		lf->stop[1] = (int16_t)LG_PW_L3;
	}

	/* the ladders: up the step on each side (L1 to L2), up the north side's
	 * step (L2 to L3) at one end, up the gantry's back (L1 to L3) */
	for (k = 0; k < 2; k++) {
		const int ui = k ? W - B + (B - 4) / 2 : 4 + (B - 4) / 2;

		u = hx + ui; v = hz + g_LgPw.vsplit;
		/* (a hallway wall may be on the spot: the nearest cell along the step with floor both sides) */
		for (i = 0; i < 12 && (lg->cells[v - 1][u] <= 0 || lg->cells[v][u] <= 0); i++) {
			u += (i & 1) ? -(i + 1) / 2 : (i + 1) / 2;
		}
		low = lg->cells[v - 1][u] - 1; high = lg->cells[v][u] - 1;
		lgPwLadder(lg, 0, v, u, 2, low, high);
		lgPwOpenAt(lg, 0, v, u, low, high);
	}
	{
		const int east = lgRange(r, 0, 1);
		const int line = hx + (east ? g_LgPw.b0 + LG_PW_RAMP / 2 : g_LgPw.a1 + LG_PW_RAMP / 2);
		int vz = hz + B + PD + lgRange(r, 0, D - 4 - B - PD - 1);

		for (i = 0; i < 12 && (lg->cells[vz][line] <= 0 || lg->cells[vz][line - 1] <= 0); i++) {
			vz += (i & 1) ? -(i + 1) / 2 : (i + 1) / 2;
		}
		low = lg->cells[vz][east ? line : line - 1] - 1; high = lg->cells[vz][east ? line - 1 : line] - 1;
		lgPwLadder(lg, 1, line, vz, east ? 1 : 0, low, high);
		lgPwOpenAt(lg, 1, line, vz, low, high);
	}
	{
		int lu = hx + g_LgPw.ladu;
		const int line = hz + B - 3;

		/* (the hallway's wall is in the way but for its doorways: one of those, nearest the pick) */
		{
			int best = -1, c;

			for (c = hx + g_LgPw.s0 + 1; c <= hx + g_LgPw.s1 - 2; c++) {
				if (lg->cells[line - 1][c] > 0 && lg->cells[line][c] > 0 && (best < 0 || abs(c - lu) < abs(best - lu))) {
					best = c;
				}
			}
			if (best >= 0) {
				lu = best;
			}
		}
		low = lg->cells[line - 1][lu] - 1; high = lg->cells[line][lu] - 1;
		lgPwLadder(lg, 0, line, lu, 2, low, high);
		lgPwOpenAt(lg, 0, line, lu, low, high);
	}
	/* the lift's top: boarded from the gantry, no rail */
	for (i = 0; i < lg->numportals; i++) {
		LgPortal *pt = &lg->portals[i];

		if (pt->a == g_LgPw.shaft || pt->b == g_LgPw.shaft) {
			pt->norail = 1;
		}
	}
	qsort(lg->portals, (size_t)lg->numportals, sizeof(lg->portals[0]), lgPwPortalCmp);

	if (!lgAreaGraph(lg, r, err, errlen) || !lgBuildNav(lg, err, errlen) || !lgBuildSpawns(lg, r, err, errlen)) {
		return 0;
	}
	lgPlaceObstacles(lg);
	lgPwFurnish(lg, r);
	lgPlaceCover(lg, r);
	return modLevelGenValidate(lg, err, errlen);
}

/* ------------------------------------------------------------------ */
/* Mall (2026-11-19)                                                    */
/* ------------------------------------------------------------------ */

/* The user: "Make a mall layout. A two story room should have a second
 * floor that overhangs over the first in a plus shaped layout. Stores are
 * walled to the walking area by glass walls, and escalators go up to the
 * second floor and down. There should also be occasional hallways that
 * connect the backs of stores, and an elevator. Design it with custom
 * textures, with an 80s mall flair. There should also be a bathroom."
 *
 * Two storeys, stacked as the highrises' are (one room, layer 0 and 1, every
 * upper piece over an undercroft or a wall), round a plus: two crossing arms
 * LG_ML_A wide. On the ground floor the arms are all concourse; upstairs a
 * gallery LG_ML_GAL wide runs along each side of each arm and across its
 * end, overhanging the concourse, the middle LG_ML_VOIDW open to the skylit
 * roof (lgMezzPortals rails it, with a gap here and there to drop through):
 * the void is the plus. In one arm a pair of escalators (two ramps side by
 * side, one rising each way, a strip of floor between) climb from the
 * concourse to bridges across the void.
 *
 * The four quadrants between the arms, on both floors, from the arms back:
 * the shops' glass fronts (a pane, a cell of pillar, a pane; a doorway in
 * each), the shops LG_ML_SD deep, their back wall (now and then a back
 * door), a service hallway LG_ML_HALLW wide behind them all, and in a
 * quadrant's far corner a stockroom. Each run of shops has a passage
 * through it from the concourse to the hallway. One shop slot on the ground
 * floor is the washroom (no front, its door off the hallway, stalls along
 * its wall). One quadrant's corner shop is the lift: a car in a walled
 * shaft, a sliding door on each floor onto a lobby facing the arm (the
 * highrises' lifts). Each arm's end: the department stores, two storeys
 * of them, their fronts on the arms' ends, the west, east and north; the
 * south arm's end the mall's glass entrance, onto the car park. Behind the
 * north store, the loading yard. */
#define LG_ML_A       11                       /* an arm's width */
#define LG_ML_GAL     3                        /* a gallery's */
#define LG_ML_VOIDW   5                        /* the void's: two escalators and a cell between */
#define LG_ML_END     3                        /* the gallery across an arm's end */
#define LG_ML_SD      5                        /* a shop's depth */
#define LG_ML_HALLW   2                        /* the hallway behind */
#define LG_ML_PARK    9                        /* the car park's depth */
#define LG_ML_YARD    6                        /* the loading yard's */
#define LG_ML_CONC_THEME   0
#define LG_ML_GAL_THEME    1
#define LG_ML_VOID_THEME   2
#define LG_ML_ESC_THEME    3
#define LG_ML_SHAFT_THEME  4
#define LG_ML_LOT_THEME    5
#define LG_ML_STOREA_THEME 6
#define LG_ML_STOREB_THEME 7
#define LG_ML_HALL_THEME   8
#define LG_ML_BATH_THEME   9
#define LG_ML_STOREC_THEME 10
#define LG_ML_ANCHOR_THEME 11

static const struct {
	uint8_t grid, ad, qx, qs, qn;
} g_LgMall[LG_SIZE_COUNT] = {
	{ 62, 6, 14, 12, 12 },
	{ 67, 7, 16, 14, 14 },
	{ 74, 8, 19, 16, 17 },
};

enum { ML_NONE, ML_CONC, ML_VOID, ML_GAL, ML_STOREA, ML_STOREB, ML_STOREC, ML_HALL, ML_STOCK, ML_BATH, ML_LOBBY, ML_ANCHOR, ML_RAMP, ML_SHAFT, ML_DOOR };

#define LG_ML_MAXOPEN 400
static struct {
	int ad, qx, qs, qn, xc, zc;
	int liftq, bathq, bathleg;
	int bathdone;
	int stores, windows, doors, backdoors, passages, stalls, stock, ramps, furniture, toilets;
	int nstall;
	/* a washroom stall (2026-11-19): the middle of the cells along its back wall, in
	 * half cells (the sum of two cells' coordinates), and the way from that wall in */
	struct { int16_t pu, pv; int8_t du, dv; } stall[16];
	int nopen;
	struct { int8_t layer, kind; int16_t x, z, w, d; } open[LG_ML_MAXOPEN];   /* kind 0 doorway out, 1 window */
} g_LgMl;

static int lgMlFloor(int layer)
{
	return LG_RT_BASE + layer * LG_RT_STOREY;
}

static int lgMlTop(void)
{
	return LG_RT_BASE + 2 * LG_RT_STOREY - LG_MEZZ_SLAB;
}

static void lgMlSet(int layer, int u, int v, int c)
{
	g_LgHr.cls[layer][g_LgHr.Z + v][g_LgHr.X + u] = (uint8_t)c;
}

static void lgMlRect(int layer, int u, int v, int w, int d, int c)
{
	int x, z;

	for (z = v; z < v + d; z++) {
		for (x = u; x < u + w; x++) {
			lgMlSet(layer, x, z, c);
		}
	}
}

/* a quadrant's cell (i out from the north-south arm, j out from the east-west) */
static void lgMlQuadUV(int q, int i, int j, int *u, int *v)
{
	*u = (q & 1) ? g_LgMl.xc + LG_ML_A + i : g_LgMl.xc - 1 - i;
	*v = (q & 2) ? g_LgMl.zc + LG_ML_A + j : g_LgMl.zc - 1 - j;
}

static int lgMlQuadJ(int q)
{
	return (q & 2) ? g_LgMl.qn : g_LgMl.qs;
}

static void lgMlQSet(int layer, int q, int i, int j, int c)
{
	int u, v;

	lgMlQuadUV(q, i, j, &u, &v);
	lgMlSet(layer, u, v, c);
}

/* an opening to make once the pieces are: a doorway out (kind 0) or a
 * window (1), absolute; a doorway inside (2) is a piece of its own, set in
 * the grid as one at once, on each layer of the mask */
static void lgMlOpen(int layer, int kind, int x, int z, int w, int d)
{
	if (kind == 2) {
		int a, b;

		for (b = z; b < z + d; b++) {
			for (a = x; a < x + w; a++) {
				g_LgHr.cls[layer][b][a] = ML_DOOR;
			}
		}
		return;
	}
	if (g_LgMl.nopen < LG_ML_MAXOPEN) {
		g_LgMl.open[g_LgMl.nopen].layer = (int8_t)layer;
		g_LgMl.open[g_LgMl.nopen].kind = (int8_t)kind;
		g_LgMl.open[g_LgMl.nopen].x = (int16_t)x;
		g_LgMl.open[g_LgMl.nopen].z = (int16_t)z;
		g_LgMl.open[g_LgMl.nopen].w = (int16_t)w;
		g_LgMl.open[g_LgMl.nopen].d = (int16_t)d;
		g_LgMl.nopen++;
	}
}

/* a glass front n cells long from u, v (local), along z or x, on the layers
 * of the mask (the same on both, so that a doorway is over a doorway and a
 * pane over a pane): a doorway (door 1: one, 2 wide or 3; door 2: the
 * entrance's two, 3 wide, doorways out) and panes up to 3 wide in the rest,
 * a cell of pillar between each */
static void lgMlFront(LgRng *r, int mask, int u, int v, int alongz, int n, int door)
{
	uint8_t kind[64];
	int k, a, b, ds = -9, dw = 0, l;

	if (n > 64 || n < 1) {
		return;
	}
	memset(kind, 0, sizeof(kind));   /* 0 pillar, 1 pane, 2 doorway */
	if (door == 2 && n >= 11) {
		for (k = 1; k < 4; k++) kind[k] = 2;
		for (k = n - 4; k < n - 1; k++) kind[k] = 2;
		for (k = 5; k < n - 5; k++) kind[k] = 1;
	} else {
		if (door) {
			dw = n >= 7 ? 3 : n >= 2 ? 2 : 1;
			ds = lgRange(r, 0, n - dw);
			for (k = ds; k < ds + dw; k++) kind[k] = 2;
		}
		/* panes: in the runs either side of the doorway, a pillar between */
		for (a = 0; a < n; a = b + 1) {
			if (door && a >= ds - 1 && a <= ds + dw) {
				b = a;
				continue;
			}
			for (b = a; b < n && !(door && b >= ds - 1 && b <= ds + dw); b++);
			b--;
			for (k = a; b - k + 1 >= 2; k += 4) {
				const int ww = b - k + 1 >= 3 ? 3 : 2;
				int m;

				for (m = k; m < k + ww; m++) kind[m] = 1;
			}
		}
	}
	/* (each run of the same kind one opening) */
	for (a = 0; a < n; a = b) {
		for (b = a + 1; b < n && kind[b] == kind[a]; b++);
		if (!kind[a]) {
			continue;
		}
		for (l = 0; l < 2; l++) {
			const int x = g_LgHr.X + u + (alongz ? 0 : a), z = g_LgHr.Z + v + (alongz ? a : 0);

			if (!(mask & (1 << l))) {
				continue;
			}
			lgMlOpen(l, kind[a] == 1 ? 1 : door == 2 ? 0 : 2, x, z, alongz ? 1 : b - a, alongz ? b - a : 1);
			if (kind[a] == 1) {
				g_LgMl.windows++;
			} else {
				g_LgMl.doors++;
			}
		}
	}
}

/* a quadrant's front cells t0..t1 (leg 0: along j at i 0; leg 1: along i at j 0) */
static void lgMlQuadFront(LgRng *r, int mask, int q, int leg, int t0, int t1, int door)
{
	int u0, v0, u1, v1;

	if (leg == 0) {
		lgMlQuadUV(q, 0, t0, &u0, &v0);
		lgMlQuadUV(q, 0, t1, &u1, &v1);
		lgMlFront(r, mask, u0, v0 < v1 ? v0 : v1, 1, t1 - t0 + 1, door);
	} else {
		lgMlQuadUV(q, t0, 0, &u0, &v0);
		lgMlQuadUV(q, t1, 0, &u1, &v1);
		lgMlFront(r, mask, u0 < u1 ? u0 : u1, v0, 0, t1 - t0 + 1, door);
	}
}

/* a doorway (a piece of its own) through a quadrant's wall line at depth
 * dd, cells t..t+n-1 along the leg, on the layers of the mask */
static void lgMlQuadDoor(int mask, int q, int leg, int dd, int t, int n)
{
	int k, l, u, v;

	for (l = 0; l < 2; l++) {
		if (mask & (1 << l)) {
			for (k = t; k < t + n; k++) {
				if (leg == 0) {
					lgMlQuadUV(q, dd, k, &u, &v);
				} else {
					lgMlQuadUV(q, k, dd, &u, &v);
				}
				lgMlSet(l, u, v, ML_DOOR);
			}
		}
	}
}

/* one quadrant, both floors: the shop runs either side of its corner shop
 * (or the lift), the hallway, the stockroom. The walls are the same on both
 * floors, the shops' kinds each their own. */
static int lgMlQuadrant(LgRng *r, int q)
{
	const int S = LG_ML_SD, H = LG_ML_HALLW, Qi = g_LgMl.qx, Qj = lgMlQuadJ(q);
	const int back = S + H + 2;              /* the hallway's back wall */
	const int stock = Qi - back - 1 >= 3 && Qj - back - 1 >= 3;
	const int pleg = Qi > Qj ? 1 : Qj > Qi ? 0 : lgRange(r, 0, 1);   /* the leg with the passage to the hallway */
	int i, j, l, leg, li;

	/* the hallway (and, if the stockroom has no room, the rest behind it) */
	for (l = 0; l < 2; l++) {
		for (j = 0; j < Qj; j++) {
			for (i = 0; i < Qi; i++) {
				const int d = i < j ? i : j;

				if (d >= S + 2 && (d <= S + 1 + H || (!stock && d >= back))) {
					lgMlQSet(l, q, i, j, ML_HALL);
				} else if (stock && d > back) {
					lgMlQSet(l, q, i, j, ML_STOCK);
				}
			}
		}
	}
	if (stock) {
		/* (its door off the hallway, on the longer side) */
		const int alongi = Qi - back - 1 >= Qj - back - 1;
		const int at = lgRange(r, back + 1, (alongi ? Qi : Qj) - 3);

		lgMlQuadDoor(3, q, alongi ? 1 : 0, back, at, 2);
		g_LgMl.stock += 2;
	}

	/* the corner shop, or the lift: its lobby on the east-west arm */
	if (q == g_LgMl.liftq) {
		int u, v, u2, v2;

		for (l = 0; l < 2; l++) {
			for (j = 0; j <= 1; j++) {
				for (i = 2; i <= 4; i++) {
					lgMlQSet(l, q, i, j, ML_LOBBY);
				}
			}
		}
		for (j = 2; j <= 4; j++) {
			for (i = 2; i <= 4; i++) {
				lgMlQSet(0, q, i, j, ML_SHAFT);
			}
		}
		lgMlQuadUV(q, 2, 2, &u, &v);
		lgMlQuadUV(q, 4, 4, &u2, &v2);
		g_LgHr.su = u < u2 ? u : u2;
		g_LgHr.sv = v < v2 ? v : v2;
		/* (the lobby's on the shaft's side towards the arm: -z for the north quadrants, +z the south) */
		g_LgHr.sside = (q & 2) ? 2 : 3;
	} else {
		const int dside = lgRange(r, 0, 1);

		for (l = 0; l < 2; l++) {
			const int c = ML_STOREA + lgRange(r, 0, 2);

			for (j = 1; j <= S; j++) {
				for (i = 1; i <= S; i++) {
					lgMlQSet(l, q, i, j, c);
				}
			}
			g_LgMl.stores++;
		}
		/* its fronts on both arms, a doorway on one */
		lgMlQuadFront(r, 3, q, 0, 1, S, dside == 0);
		lgMlQuadFront(r, 3, q, 1, 1, S, dside == 1);
	}

	/* the runs of shops along each leg, out from the corner: their slots
	 * the same on both floors; a passage the first slot of the longer leg */
	for (li = 0; li < 2; li++) {
		const int Qt = (leg = li ^ g_LgMl.bathleg) == 0 ? Qj : Qi;
		int t = S + 2, nslot = 0;

		while (t < Qt) {
			const int rem = Qt - t;
			int w = lgRange(r, 4, 7), a, b;

			if (rem < 3) {
				break;
			}
			if (leg == pleg && nslot == 0) {
				/* the passage: hallway from the front to the hallway behind */
				w = 2;
				for (l = 0; l < 2; l++) {
					for (a = t; a < t + w; a++) {
						for (b = 1; b <= S + 1; b++) {
							if (leg == 0) lgMlQSet(l, q, b, a, ML_HALL);
							else lgMlQSet(l, q, a, b, ML_HALL);
						}
					}
					g_LgMl.passages++;
				}
				lgMlQuadDoor(3, q, leg, 0, t, w);
			} else {
				int bath, mask = 3;

				if (rem - w - 1 < 4) {
					w = rem <= 8 ? rem : rem - 5;
				}
				if (w > rem) {
					w = rem;
				}
				bath = !g_LgMl.bathdone && q != g_LgMl.liftq && w >= 5;
				for (l = 0; l < 2; l++) {
					const int c = l == 0 && bath ? ML_BATH : ML_STOREA + lgRange(r, 0, 2);

					for (a = t; a < t + w; a++) {
						for (b = 1; b <= S; b++) {
							if (leg == 0) lgMlQSet(l, q, b, a, c);
							else lgMlQSet(l, q, a, b, c);
						}
					}
				}
				if (bath) {
					/* the washroom: no front; stalls (a partition a cell
					 * thick every third cell) along the wall towards the
					 * concourse, the door off the hallway */
					{
						int sa, nst = 0;

						/* a stall is the two cells between partitions, two deep:
						 * the first before the first partition, one after each */
						for (sa = t; sa + 1 < t + w && nst < 16; sa += 3) {
							int u0, v0, u1, v1, u2, v2, u3, v3;

							if (sa > t && sa - 1 >= t + w) {
								break;
							}
							if (leg == 0) {
								lgMlQuadUV(q, 1, sa, &u0, &v0); lgMlQuadUV(q, 1, sa + 1, &u1, &v1); lgMlQuadUV(q, 2, sa, &u2, &v2);
							} else {
								lgMlQuadUV(q, sa, 1, &u0, &v0); lgMlQuadUV(q, sa + 1, 1, &u1, &v1); lgMlQuadUV(q, sa, 2, &u2, &v2);
							}
							(void)u3; (void)v3;
							g_LgMl.stall[nst].pu = (int16_t)(u0 + u1);
							g_LgMl.stall[nst].pv = (int16_t)(v0 + v1);
							g_LgMl.stall[nst].du = (int8_t)(u2 - u0);
							g_LgMl.stall[nst].dv = (int8_t)(v2 - v0);
							nst++;
						}
						g_LgMl.nstall = nst;
					}
					for (a = t + 2; a + 2 < t + w; a += 3) {
						for (b = 1; b <= 2; b++) {
							if (leg == 0) lgMlQSet(0, q, b, a, ML_NONE);
							else lgMlQSet(0, q, a, b, ML_NONE);
						}
						g_LgMl.stalls++;
					}
					g_LgMl.stalls++;
					lgMlQuadDoor(1, q, leg, S + 1, t + w - 2, 2);
					g_LgMl.bathdone = 1;
					mask = 2;
				}
				lgMlQuadFront(r, mask, q, leg, t, t + w - 1, 1);
				g_LgMl.stores += bath ? 1 : 2;
				/* now and then a back door into the hallway */
				if (lgRange(r, 0, 99) < 45) {
					lgMlQuadDoor(mask, q, leg, S + 1, lgRange(r, t, t + w - 2), 2);
					g_LgMl.backdoors++;
				}
			}
			t += w + 1;
			nslot++;
		}
	}
	return 1;
}

/* pieces of class c on a layer, as big as fit up to LG_HR_PIECE square */
static int lgMlPieces(LgLayout *lg, int layer, int c, int theme, int bay)
{
	const int f = lgMlFloor(layer), top = lgMlTop();
	int x, z;

	for (z = g_LgHr.Z; z < g_LgHr.Z + g_LgHr.D; z++) {
		for (x = g_LgHr.X; x < g_LgHr.X + g_LgHr.W; x++) {
			int w, d, k, ok;
			LgArea *a;
			LgRect rc;

			if (g_LgHr.cls[layer][z][x] != c) {
				continue;
			}
			for (w = 1; w < LG_HR_PIECE && x + w < g_LgHr.X + g_LgHr.W && g_LgHr.cls[layer][z][x + w] == c; w++);
			for (d = 1, ok = 1; d < LG_HR_PIECE && z + d < g_LgHr.Z + g_LgHr.D && ok; ) {
				for (k = 0; k < w; k++) {
					if (g_LgHr.cls[layer][z + d][x + k] != c) {
						ok = 0;
						break;
					}
				}
				if (ok) {
					d++;
				}
			}
			for (k = 0; k < d; k++) {
				memset(&g_LgHr.cls[layer][z + k][x], c | LG_HR_DONE, (size_t)w);
			}
			if ((a = lgRtArea(lg, x, z, w, d)) == NULL) {
				return 0;
			}
			rc.x = (int16_t)x; rc.z = (int16_t)z; rc.w = (int16_t)w; rc.d = (int16_t)d; rc.room = -1;
			lgSetRoomArea(a, &rc, c == ML_HALL || c == ML_STOCK ? LG_ROOM_STORAGE : LG_ROOM_LAB, 0, 0, 0);
			a->layer = (uint8_t)layer;
			a->floorlo = a->floor = (int16_t)f;
			if (bay) {
				a->ceil = (int16_t)top;
				a->flags = LG_AREAFLAG_BAY;
			} else {
				a->ceil = (int16_t)(layer ? top : f + LG_RT_STOREY - LG_MEZZ_SLAB);
				a->flags = (uint8_t)(layer ? LG_AREAFLAG_MEZZ : LG_AREAFLAG_UNDER);
			}
			a->theme = (uint8_t)theme;
			if (c == ML_LOBBY) {
				g_LgHr.lobby[layer] = lg->numareas - 1;
			}
		}
	}
	return 1;
}

/* a toilet in each stall of the washroom, against its back wall, facing out
 * (before the furniture, which would take the room) */
static void lgMlToilets(LgLayout *lg)
{
	{
		int k;

		for (k = 0; k < g_LgMl.nstall; k++) {
			const int du = g_LgMl.stall[k].du, dv = g_LgMl.stall[k].dv;
			const int32_t x = lgWorldX(lg, g_LgHr.X) + g_LgMl.stall[k].pu * LG_CELL / 2 + LG_CELL / 2 - du * (LG_CELL / 2 - 41);
			const int32_t z = lgWorldZ(lg, g_LgHr.Z) + g_LgMl.stall[k].pv * LG_CELL / 2 + LG_CELL / 2 - dv * (LG_CELL / 2 - 41);
			const int turn = dv > 0 ? 0 : du < 0 ? 1 : dv < 0 ? 2 : 3;
			int ai;

			for (ai = 0; ai < lg->numareas; ai++) {
				const LgArea *a = &lg->areas[ai];

				if (a->theme == LG_ML_BATH_THEME && a->layer == 0 && x >= lgWorldX(lg, a->x) && x < lgWorldX(lg, a->x + a->w)
						&& z >= lgWorldZ(lg, a->z) && z < lgWorldZ(lg, a->z + a->d)) {
					g_LgMl.toilets += lgTryProp(lg, ai, LG_PROP_TOILET, x, z, turn, 5, 4, 6);
					break;
				}
			}
		}
	}
}

/* Things on the floors: groups as the penthouse's (lgHrFurnish), each
 * piece 60 clear of the next. The shops' shelving back to back, clothes
 * rails, display tables, a counter; the food court's tables round the
 * crossing; benches and planters along the concourse and galleries; carts
 * and crates in the hallways; cars in the car park; crates, skips and a
 * van in the yard. */
static void lgMlFurnish(LgLayout *lg, LgRng *r)
{
	static const int8_t shop[][4][4] = {
		{ { LG_PROP_CICABINET, 0, -9, 0 }, { LG_PROP_CICABINET, 0, 9, 2 }, { -1 } },
		{ { LG_PROP_DIVIDER, 0, 0, 0 }, { LG_PROP_DIVIDER, 0, 14, 0 }, { -1 } },
		{ { LG_PROP_A51TABLE, 0, 0, 0 }, { LG_PROP_PLANTSPIDER, 19, 0, 0 }, { -1 } },
		{ { LG_PROP_DDDESK, 0, 0, 0 }, { LG_PROP_G5CHAIR, 0, -14, 0 }, { -1 } },
		{ { LG_PROP_CITABLE, 0, 0, 0 }, { LG_PROP_LAMP, 17, 0, 0 }, { -1 } },
		{ { LG_PROP_LABTABLE, 0, 0, 0 }, { -1 } },
	};
	static const int8_t food[][4][4] = {
		{ { LG_PROP_CITABLE, 0, 0, 0 }, { LG_PROP_VILLACHAIR, -8, -13, 0 }, { LG_PROP_VILLACHAIR, 8, -13, 0 }, { LG_PROP_VILLACHAIR, 0, 13, 2 } },
		{ { LG_PROP_CITABLE, 0, 0, 1 }, { LG_PROP_VILLACHAIR, -13, 0, 1 }, { LG_PROP_VILLACHAIR, 13, 0, 3 }, { -1 } },
		{ { LG_PROP_PLANTRUBBER, 0, 0, 0 }, { -1 } },
	};
	static const int8_t walk[][4][4] = {
		{ { LG_PROP_REDSOFA, 0, 0, 0 }, { LG_PROP_PLANTRUBBER, 19, 0, 0 }, { -1 } },
		{ { LG_PROP_PLANTSPIDER, 0, 0, 0 }, { -1 } },
		{ { LG_PROP_ARMCHAIR, -10, 0, 1 }, { LG_PROP_ARMCHAIR, 10, 0, 3 }, { -1 } },
		{ { LG_PROP_PLANTRUBBER, 0, 0, 0 }, { -1 } },
	};
	static const int8_t back[][4][4] = {
		{ { LG_PROP_TROLLEY, 0, 0, 0 }, { -1 } },
		{ { 0, 0, 0, 0 }, { LG_PROP_BARREL, 15, 0, 0 }, { -1 } },
		{ { LG_PROP_BORGCRATE, 0, 0, 0 }, { -1 } },
		{ { LG_PROP_CICABINET, 0, 0, 0 }, { -1 } },
	};
	static const int8_t bath[][4][4] = {
		{ { LG_PROP_LABTABLE, 0, 0, 0 }, { -1 } },   /* the basins' counter */
		{ { LG_PROP_PLANTSPIDER, 0, 0, 0 }, { -1 } },
	};
	static const int8_t lot[][4][4] = {
		{ { LG_PROP_TAXI, 0, 0, 0 }, { -1 } },
		{ { LG_PROP_POLICE, 0, 0, 0 }, { -1 } },
		{ { LG_PROP_HOVCAR, 0, 0, 0 }, { -1 } },
		{ { LG_PROP_LIMO, 0, 0, 0 }, { -1 } },
	};
	static const int8_t yard[][4][4] = {
		{ { LG_PROP_HOVCAB, 0, 0, 0 }, { -1 } },
		{ { LG_PROP_DUMPSTER, 0, 0, 0 }, { -1 } },
		{ { 0, 0, 0, 0 }, { 0, 17, 0, 0 }, { LG_PROP_BARREL, -15, 0, 0 } , { -1 } },
		{ { LG_PROP_HOVERCRATE, 0, 0, 0 }, { LG_PROP_BORGCRATE, 17, 0, 0 }, { -1 } },
	};
	int ai;

	for (ai = 0; ai < lg->numareas; ai++) {
		const LgArea *a = &lg->areas[ai];
		const int8_t (*set)[4][4];
		int nset, step, chance;
		int32_t x, z;

		if (a->ramp || a->special || a->kind != LG_AREA_ROOM || a->w < 2 || a->d < 2) {
			continue;
		}
		if (a->room == 1) {
			set = lot; nset = 4; step = 520; chance = 70;
		} else if (a->room == 2) {
			set = yard; nset = 4; step = 420; chance = 60;
		} else if (a->theme == LG_ML_STOREA_THEME || a->theme == LG_ML_STOREB_THEME || a->theme == LG_ML_STOREC_THEME || a->theme == LG_ML_ANCHOR_THEME) {
			set = shop; nset = 6; step = a->theme == LG_ML_ANCHOR_THEME ? 260 : 300; chance = a->theme == LG_ML_ANCHOR_THEME ? 90 : 80;
		} else if (a->theme == LG_ML_VOID_THEME) {
			set = food; nset = 3; step = 330; chance = 85;
		} else if (a->theme == LG_ML_CONC_THEME || a->theme == LG_ML_GAL_THEME) {
			set = walk; nset = 4; step = 600; chance = 30;
		} else if (a->theme == LG_ML_HALL_THEME) {
			set = back; nset = 4; step = 500; chance = 35;
		} else if (a->theme == LG_ML_BATH_THEME) {
			set = bath; nset = 2; step = 250; chance = 70;
		} else {
			continue;
		}
		for (z = lgWorldZ(lg, a->z) + 130; z <= lgWorldZ(lg, a->z + a->d) - 130; z += step) {
			for (x = lgWorldX(lg, a->x) + 130; x <= lgWorldX(lg, a->x + a->w) - 130; x += step) {
				int tryg, placed = 0;

				if (lgRange(r, 0, 99) >= chance) {
					continue;
				}
				for (tryg = 0; tryg < 3 && !placed; tryg++) {
					const int8_t (*grp)[4] = set[lgRange(r, 0, nset - 1)];
					const int turn = a->room == 1 ? lgRange(r, 0, 1) * 2 : lgRange(r, 0, 3);
					int k;

					for (k = 0; k < 4 && grp[k][0] >= 0; k++) {
						const int dx = grp[k][1] * 10, dz = grp[k][2] * 10;
						const int rx = turn == 0 ? dx : turn == 1 ? -dz : turn == 2 ? -dx : dz;
						const int rz = turn == 0 ? dz : turn == 1 ? dx : turn == 2 ? -dz : -dx;
						const int32_t px = x + rx, pz = z + rz;

						if (px < lgWorldX(lg, a->x) + 50 || px > lgWorldX(lg, a->x + a->w) - 50 || pz < lgWorldZ(lg, a->z) + 50 || pz > lgWorldZ(lg, a->z + a->d) - 50) {
							if (k == 0) break;
							continue;
						}
						if (lgTryProp(lg, ai, grp[k][0], px, pz, (turn + grp[k][3]) & 3, 30, LG_COVER_PADGAP, 62)) {
							g_LgMl.furniture++;
							placed++;
						} else if (k == 0) {
							break;
						}
					}
				}
			}
		}
	}
}

static int lgTryMall(LgLayout *lg, const LgParams *p, LgRng *r, char *err, int errlen)
{
	const int size = lg->size, G = lg->gw, A = LG_ML_A, L = LG_HR_RAMPL;
	const int ad = g_LgMall[size].ad, qx = g_LgMall[size].qx, qs = g_LgMall[size].qs, qn = g_LgMall[size].qn;
	const int W = 2 * (ad + 1) + 2 * qx + A, D = qs + A + qn + 1 + ad;
	const int f1 = lgMlFloor(0), f2 = lgMlFloor(1), top = lgMlTop();
	int i, l, q, x, z, u, v, nliftdoor = 0;
	int xc, zc, e0, east;

	(void)p;
	memset(&g_LgHr, 0, sizeof(g_LgHr));
	memset(&g_LgRt, 0, sizeof(g_LgRt));
	memset(&g_LgMl, 0, sizeof(g_LgMl));
	memset(lg->cells, 0, sizeof(lg->cells));
	memset(lg->cells2, 0, sizeof(lg->cells2));
	memset(lg->cells3, 0, sizeof(lg->cells3));
	lg->numareas = 0;
	lg->numrooms = 0;
	g_LgHr.W = W; g_LgHr.D = D;
	g_LgHr.X = (G - W) / 2;
	g_LgHr.Z = LG_ML_PARK + 2;
	if (g_LgHr.X < 2 || g_LgHr.Z + D + 1 + LG_ML_YARD > G - 1) {
		lgSetErr(err, errlen, "the mall does not fit the grid (%d)", G, 0);
		return 0;
	}
	g_LgMl.ad = ad; g_LgMl.qx = qx; g_LgMl.qs = qs; g_LgMl.qn = qn;
	xc = g_LgMl.xc = ad + 1 + qx;
	zc = g_LgMl.zc = qs;
	g_LgMl.liftq = lgRange(r, 0, 3);
	g_LgMl.bathq = (g_LgMl.liftq + lgRange(r, 1, 3)) & 3;
	g_LgMl.bathleg = lgRange(r, 0, 1);

	/* the arms: concourse below, gallery above; the plus of void in the
	 * middle of them, open from the concourse to the roof */
	for (l = 0; l < 2; l++) {
		const int c = l ? ML_GAL : ML_CONC;

		lgMlRect(l, ad + 1, zc, W - 2 * (ad + 1), A, c);
		lgMlRect(l, xc, 0, A, D - ad - 1, c);
	}
	lgMlRect(0, ad + 1 + LG_ML_END, zc + LG_ML_GAL, W - 2 * (ad + 1) - 2 * LG_ML_END, LG_ML_VOIDW, ML_VOID);
	lgMlRect(0, xc + LG_ML_GAL, LG_ML_END, LG_ML_VOIDW, D - ad - 1 - 2 * LG_ML_END, ML_VOID);
	lgMlRect(1, ad + 1 + LG_ML_END, zc + LG_ML_GAL, W - 2 * (ad + 1) - 2 * LG_ML_END, LG_ML_VOIDW, ML_NONE);
	lgMlRect(1, xc + LG_ML_GAL, LG_ML_END, LG_ML_VOIDW, D - ad - 1 - 2 * LG_ML_END, ML_NONE);

	/* the escalators: in the east or west half of the east-west arm's void,
	 * a bridge either end of them */
	east = lgRange(r, 0, 1);
	{
		const int lo = east ? xc + A : ad + 1 + LG_ML_END, hi = east ? W - ad - 1 - LG_ML_END : xc;   /* the half's void */
		const int vs = zc + LG_ML_GAL;

		if (hi - lo < L + 4) {
			lgSetErr(err, errlen, "no room for the escalators (%d)", hi - lo, 0);
			return 0;
		}
		e0 = lgRange(r, lo, hi - L - 4);
		/* bridges: gallery over concourse */
		lgMlRect(0, e0, vs, 2, LG_ML_VOIDW, ML_CONC);
		lgMlRect(1, e0, vs, 2, LG_ML_VOIDW, ML_GAL);
		lgMlRect(0, e0 + 2 + L, vs, 2, LG_ML_VOIDW, ML_CONC);
		lgMlRect(1, e0 + 2 + L, vs, 2, LG_ML_VOIDW, ML_GAL);
		/* the two ramps (made as bays below), a strip of void between */
		lgMlRect(0, e0 + 2, vs, L, 2, ML_RAMP);
		lgMlRect(0, e0 + 2, vs + 3, L, 2, ML_RAMP);
	}

	/* the department stores at the arms' ends, two storeys each, parted
	 * into departments; their glass fronts onto the arms' ends */
	for (l = 0; l < 2; l++) {
		int k;

		lgMlRect(l, 0, 0, ad, D, ML_ANCHOR);
		lgMlRect(l, W - ad, 0, ad, D, ML_ANCHOR);
		lgMlRect(l, ad + 1, D - ad, W - 2 * (ad + 1), ad, ML_ANCHOR);
		for (k = 0; k < 2; k++) {
			const int pv = k ? zc + A + 5 : zc - 6;
			const int pu = k ? xc + A + 5 : xc - 6;
			int s;

			if (pv >= 3 && pv <= D - 4) {
				for (s = 0; s < 2; s++) {
					const int gap = lgRange(r, 1, ad - 4);

					lgMlRect(l, s ? W - ad : 0, pv, ad, 1, ML_NONE);
					lgMlRect(l, (s ? W - ad : 0) + gap, pv, 3, 1, ML_ANCHOR);
				}
			}
			if (pu >= ad + 3 && pu <= W - ad - 4) {
				const int gap = lgRange(r, 1, ad - 4);

				lgMlRect(l, pu, D - ad, 1, ad, ML_NONE);
				lgMlRect(l, pu, D - ad + gap, 1, 3, ML_ANCHOR);
			}
		}
		g_LgMl.stores += 3;
	}
	lgMlFront(r, 3, ad, zc, 1, A, 1);
	lgMlFront(r, 3, W - ad - 1, zc, 1, A, 1);
	lgMlFront(r, 3, xc, D - ad - 1, 0, A, 1);

	/* the quadrants */
	for (q = 0; q < 4; q++) {
		if (!lgMlQuadrant(r, (g_LgMl.bathq + q) & 3)) {
			return 0;
		}
	}
	if (!g_LgMl.bathdone) {
		lgSetErr(err, errlen, "no washroom", 0, 0);
		return 0;
	}

	/* the areas: the ground floor's concourse first (its biggest piece the
	 * room's main), then the rest; the ramps, the shaft */
	lg->numrooms = 1;
	if (!lgMlPieces(lg, 0, ML_CONC, LG_ML_CONC_THEME, 0)) {
		return 0;
	}
	{
		int best = 0;

		for (i = 1; i < lg->numareas; i++) {
			if (lg->areas[i].w * lg->areas[i].d > lg->areas[best].w * lg->areas[best].d) {
				best = i;
			}
		}
		if (best) {
			const LgArea t = lg->areas[0];

			lg->areas[0] = lg->areas[best];
			lg->areas[best] = t;
		}
	}
	for (l = 0; l < 2; l++) {
		if ((l && !lgMlPieces(lg, 1, ML_GAL, LG_ML_GAL_THEME, 0))
				|| !lgMlPieces(lg, l, ML_STOREA, LG_ML_STOREA_THEME, 0)
				|| !lgMlPieces(lg, l, ML_STOREB, LG_ML_STOREB_THEME, 0)
				|| !lgMlPieces(lg, l, ML_STOREC, LG_ML_STOREC_THEME, 0)
				|| !lgMlPieces(lg, l, ML_ANCHOR, LG_ML_ANCHOR_THEME, 0)
				|| !lgMlPieces(lg, l, ML_HALL, LG_ML_HALL_THEME, 0)
				|| !lgMlPieces(lg, l, ML_STOCK, LG_ML_HALL_THEME, 0)
				|| !lgMlPieces(lg, l, ML_BATH, LG_ML_BATH_THEME, 0)
				|| !lgMlPieces(lg, l, ML_LOBBY, l ? LG_ML_GAL_THEME : LG_ML_CONC_THEME, 0)) {
			return 0;
		}
		/* the doorways inside: on the ground floor up to the slab (the
		 * shutter's), upstairs door height */
		i = lg->numareas;
		if (!lgMlPieces(lg, l, ML_DOOR, l ? LG_ML_GAL_THEME : LG_ML_CONC_THEME, 0)) {
			return 0;
		}
		for (; l && i < lg->numareas; i++) {
			lg->areas[i].ceil = (int16_t)(f2 + LG_HR_DOORH);
		}
	}
	if (!lgMlPieces(lg, 0, ML_VOID, LG_ML_VOID_THEME, 1)) {
		return 0;
	}
	{
		const int vs = zc + LG_ML_GAL;

		if (!lgHrBay(lg, 0, g_LgHr.X + e0 + 2, g_LgHr.Z + vs, L, 2, 0, f1, f2, top, LG_RAMP_XP, LG_ML_ESC_THEME)
				|| !lgHrBay(lg, 0, g_LgHr.X + e0 + 2, g_LgHr.Z + vs + 3, L, 2, 0, f1, f2, top, LG_RAMP_XN, LG_ML_ESC_THEME)) {
			return 0;
		}
		g_LgMl.ramps = 2;
	}
	g_LgHr.shaft = lg->numareas;
	if (!lgHrBay(lg, 0, g_LgHr.X + g_LgHr.su, g_LgHr.Z + g_LgHr.sv, LG_HR_SHAFT, LG_HR_SHAFT, 0, f1, f1, top, 0, LG_ML_SHAFT_THEME)) {
		return 0;
	}

	/* the outdoors: the car park in front (room 1), the loading yard behind (room 2) */
	lgRtMark(g_LgHr.X - 1, g_LgHr.Z - 1, W + 2, D + 2);
	g_LgRt.outdoor = lg->numrooms++;
	if (!lgRtFill(lg, 1, 1, G - 1, g_LgHr.Z - 1, f1, 0, LG_ML_LOT_THEME, 1)) {
		return 0;
	}
	g_LgRt.outdoor = lg->numrooms++;
	if (!lgRtFill(lg, g_LgHr.X - 1, g_LgHr.Z + D + 1, g_LgHr.X + W + 1, g_LgHr.Z + D + 1 + LG_ML_YARD, f1, 0, LG_ML_LOT_THEME, 1)) {
		return 0;
	}

	/* the entrance's glass doors, the windows over them; the yard's door */
	lgMlFront(r, 1, xc, -1, 0, A, 2);
	lgMlFront(r, 2, xc, -1, 0, A, 0);
	{
		const int du = lgRange(r, xc, xc + A - 3);

		lgMlOpen(0, 0, g_LgHr.X + du, g_LgHr.Z + D, 3, 1);
	}

	/* the openings */
	for (i = 0; i < g_LgMl.nopen; i++) {
		const int ly = g_LgMl.open[i].layer, f = lgMlFloor(ly);
		LgRect rc;
		LgArea *a;

		if (lg->numareas >= LG_MAXAREAS - 4) {
			lgSetErr(err, errlen, "too many areas (%d)", lg->numareas, 0);
			return 0;
		}
		rc.x = g_LgMl.open[i].x; rc.z = g_LgMl.open[i].z; rc.w = g_LgMl.open[i].w; rc.d = g_LgMl.open[i].d; rc.room = -1;
		if (g_LgMl.open[i].kind == 1) {
			lgStCorridor(lg, &rc, f + LG_HR_SILL, f + LG_HR_SILL, 0, 0, 0);
			a = &lg->areas[lg->numareas - 1];
			a->ceil = (int16_t)(f + LG_HR_SILL + LG_HR_WINH);
			a->special = LG_SPECIAL_WINDOW;
		} else {
			lgStCorridor(lg, &rc, f, f, 0, 0, 0);
			a = &lg->areas[lg->numareas - 1];
			a->ceil = (int16_t)(f + LG_HR_DOORH);
		}
		a->layer = (uint8_t)ly;
		a->theme = (uint8_t)(ly ? LG_ML_GAL_THEME : LG_ML_CONC_THEME);
	}

	/* the cells */
	memset(lg->cells, 0, sizeof(lg->cells));
	memset(lg->cells2, 0, sizeof(lg->cells2));
	memset(lg->cells3, 0, sizeof(lg->cells3));
	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];

		for (z = a->z; z < a->z + a->d; z++) {
			for (x = a->x; x < a->x + a->w; x++) {
				if (LG_CELLS(lg, a->layer)[z][x]) {
					lgSetErr(err, errlen, "area %d overlaps another", i, 0);
					return 0;
				}
				LG_CELLS(lg, a->layer)[z][x] = (int16_t)(i + 1);
			}
		}
	}

	/* the openings between: each floor's, the gallery's ledges over the void */
	for (g_LgPortalLayer = 0; g_LgPortalLayer < LG_MAXLAYERS; g_LgPortalLayer++) {
		if (!lgFindPortals(lg, r, err, errlen)) {
			g_LgPortalLayer = 0;
			return 0;
		}
	}
	g_LgPortalLayer = 0;
	if (!lgMezzPortals(lg, err, errlen)) {
		return 0;
	}
	/* the windows over the entrance: out onto the car park below */
	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];
		int out;

		if (a->special != LG_SPECIAL_WINDOW || a->layer != 1 || a->z != g_LgHr.Z - 1) {
			continue;
		}
		out = lg->cells[a->z - 1][a->x] - 1;
		if (out < 0 || lg->areas[out].room != 1 || LG_CELLS(lg, 1)[a->z - 1][a->x]) {
			continue;
		}
		{
			int q2, run;

			for (q2 = 0; q2 < a->w; q2 = run) {
				const int o2 = lg->cells[a->z - 1][a->x + q2] - 1;

				for (run = q2 + 1; run < a->w && lg->cells[a->z - 1][a->x + run] - 1 == o2; run++);
				if (o2 < 0 || lg->areas[o2].room != 1) {
					continue;
				}
				if (!lgStPortal2(lg, 0, a->z, a->x + q2, a->x + run, o2, i, 1, 1, err, errlen)) {
					return 0;
				}
				lg->portals[lg->numportals - 1].window = 1;
			}
		}
	}
	/* railings: none at the glass (the glass is one); the lift's doors, and
	 * no others (the shops' and the entrance's doorways stand open) */
	for (i = 0; i < lg->numportals; i++) {
		LgPortal *pt = &lg->portals[i];
		const LgArea *a = &lg->areas[pt->a], *c = &lg->areas[pt->b];

		pt->door = 0;
		if (a->special == LG_SPECIAL_WINDOW || c->special == LG_SPECIAL_WINDOW) {
			pt->norail = 1;
		} else if (pt->a == g_LgHr.shaft || pt->b == g_LgHr.shaft) {
			const int other = pt->a == g_LgHr.shaft ? pt->b : pt->a;
			const int st = lg->areas[other].layer;

			if (other != g_LgHr.lobby[st] || nliftdoor & (1 << st)) {
				lgSetErr(err, errlen, "the shaft opens onto area %d", other, 0);
				return 0;
			}
			nliftdoor |= 1 << st;
			pt->door = (uint8_t)(LG_DOOR_LIFT + st);
			pt->norail = 1;
		}
	}
	if (nliftdoor != 3) {
		lgSetErr(err, errlen, "the lift has doors on floors %x", nliftdoor, 0);
		return 0;
	}
	{
		LgLift *lf = &lg->lifts[lg->numlifts++];

		lf->area = (int16_t)g_LgHr.shaft;
		lf->side = (int16_t)g_LgHr.sside;
		lf->nstops = 2;
		lf->stop[0] = (int16_t)(f1 + 2);
		lf->stop[1] = (int16_t)f2;
	}
	(void)u; (void)v;
	if (!lgAreaGraph(lg, r, err, errlen) || !lgBuildNav(lg, err, errlen) || !lgBuildSpawns(lg, r, err, errlen)) {
		return 0;
	}
	lgPlaceObstacles(lg);
	lgMlToilets(lg);
	lgMlFurnish(lg, r);
	lgPlaceCover(lg, r);
	return modLevelGenValidate(lg, err, errlen);
}

/* ------------------------------------------------------------------ */
/* Villa                                                                */

/* The villa (2026-11-15; user: "Do the villa, using textures like the
 * Carrington villa textures, sloped roofs with ramps going to it, a large
 * central compound with many windows and small rooms, and a dock over
 * water"). Redesigned the same day (user: "The large building with the
 * sloped roof should be about 50% of the total map real estate, with
 * multiple floors, large flat balconies, and many small rooms. It should not
 * be a single straight hallway. Furthermore, the outside area shouldn't just
 * be a bunch of chest high walls and crates. The outside walls should be
 * tall, but flat enough to hold guard spawns. They should have guard spawns.
 * Long sightlines should not exist on villa, unless the player is on a
 * roof.").
 *
 * Laid out with the water to +z, then turned one of eight ways:
 * - the grounds' walls (the rampart): three cells thick and LG_VL_WALLTOP
 *   high along the three landward sides, their tops walked on (guards stand
 *   there), with ramps up from the garden; walls of the same height and
 *   thickness run out from them across the garden ring (dividers), a gate
 *   left by the house, so that no path round the house runs straight far;
 * - the house, about half the grounds: four wings round a courtyard, apart at
 *   the corners by passages (LG_VL_GAP) from the ring to the courtyard. Each
 *   wing a building with a pitched roof (ridge along its length, eaves on its
 *   long sides). The back wing, and sometimes a side wing, stands on a plinth
 *   a storey up (the upper floor, LG_VL_UPPER), with a broad terrace in front
 *   of it over the courtyard (a balcony, railed), reached by a grand ramp
 *   from the courtyard. Inside every wing: small rooms either side of a
 *   corridor that changes sides every few rooms (a cross passage between),
 *   so no hallway runs far; windows on both faces; doors at the ends and onto
 *   the garden (a raised wing's only onto its terrace);
 * - roofs (layer 1, LG_SPECIAL_ROOF): ramps up to landings at the eaves from
 *   the garden ring (the low wings) or the terraces (the raised ones; a low
 *   wing's eave is level with the terrace, so a terrace walks onto it);
 * - the courtyard has a stone block in its middle (a fountain's plinth,
 *   LG_SPECIAL_RACK), the quay stone hedges across it;
 * - the water along +z (a kill floor) with wooden piers out over it. */
#define LG_VL_FLOOR   LG_LEVEL                  /* the grounds: the water is a level below */
#define LG_VL_ROOMCEIL 300                      /* every room's ceiling over the floor */
#define LG_VL_UPPER   (LG_VL_FLOOR + LG_VL_ROOMCEIL + 20)   /* a raised wing's floor and its terrace (480) */
#define LG_VL_EAVE    LG_VL_UPPER               /* a low wing's eave: level with the terraces */
#define LG_VL_EAVE2   (LG_VL_UPPER + LG_VL_ROOMCEIL + 20)   /* a raised wing's eave (800) */
#define LG_VL_PITCH   54                        /* a ramp's rise a cell */
#define LG_VL_RAMPL   6                         /* so a ramp up a storey is six cells */
#define LG_VL_ROOFPITCH 100                     /* a roof's rise a cell: 45 degrees, steep enough that a
                                                 * body slides off (the ragdolls' slope grip holds ~35) */
#define LG_VL_WATER   8                         /* the water's depth, cells */
#define LG_VL_BAND    2                         /* a building's wall: windows and doors run through it */
#define LG_VL_WALLTOP (LG_VL_FLOOR + 300)       /* the grounds' walls: their tops, walked on */
#define LG_VL_WALLW   3                         /* their thickness */
#define LG_VL_GAP     3                         /* the passages between the wings */
#define LG_VL_TERRACE 8                         /* a terrace's depth: a landing and a ramp up to the roof fit */
#define LG_VL_SEGMIN  7                         /* a corridor's run before it crosses over */
#define LG_VL_SEGMAX  11
#define LG_ROOF_THEME  6
#define LG_WATER_THEME 7
#define LG_DOCK_THEME  8
#define LG_VL_TERRACE_THEME 1                   /* terracotta */
#define LG_VL_WALL_THEME    4                   /* flagstone and stone */
/* the gardens (2026-11-16: "pools and gardens in the outdoor areas, not
 * stacks of chest high walls and crates") */
#define LG_VL_HEDGE_THEME   9                   /* a clipped hedge: green all over */
#define LG_VL_POOL_THEME    10                  /* a pool: water in a low stucco basin */
#define LG_VL_BED_THEME     11                  /* a flower bed in a low stone kerb */
#define LG_VL_POOLRIM (LG_VL_FLOOR + 30)
#define LG_VL_BEDTOP  (LG_VL_FLOOR + 45)

static const struct {
	int8_t grid, ring, wingd, quay, piers;
} g_LgVilla[LG_SIZE_COUNT] = {
	{ 72, 6, 18, 8, 1 },
	{ 90, 7, 20, 10, 2 },
	{ 112, 8, 22, 11, 3 },
};

#define LG_VL_MAXBLD 4

static struct {
	int G, fx, fz, swap;
	int nb;
	LgRect bld[LG_VL_MAXBLD];    /* the wings, turned */
	int ridgealongx[LG_VL_MAXBLD];
	int eave[LG_VL_MAXBLD];
	int raised[LG_VL_MAXBLD];
	int garden;                  /* the garden's main area (and its room) */
} g_LgVl;

static LgRect lgVlRect(int x, int z, int w, int d)
{
	LgRect rc;

	if (g_LgVl.swap) {
		int t = x; x = z; z = t;
		t = w; w = d; d = t;
	}
	if (g_LgVl.fx) x = g_LgVl.G - x - w;
	if (g_LgVl.fz) z = g_LgVl.G - z - d;
	rc.x = (int16_t)x; rc.z = (int16_t)z; rc.w = (int16_t)w; rc.d = (int16_t)d; rc.room = -1;
	return rc;
}

/* a ramp's direction (LG_RAMP_*) as laid out, turned */
static int lgVlRampDir(int dir)
{
	if (g_LgVl.swap) {
		dir = dir == LG_RAMP_ZP ? LG_RAMP_XP : dir == LG_RAMP_ZN ? LG_RAMP_XN : dir == LG_RAMP_XP ? LG_RAMP_ZP : dir == LG_RAMP_XN ? LG_RAMP_ZN : dir;
	}
	if (g_LgVl.fx && (dir == LG_RAMP_XP || dir == LG_RAMP_XN)) dir = dir == LG_RAMP_XP ? LG_RAMP_XN : LG_RAMP_XP;
	if (g_LgVl.fz && (dir == LG_RAMP_ZP || dir == LG_RAMP_ZN)) dir = dir == LG_RAMP_ZP ? LG_RAMP_ZN : LG_RAMP_ZP;
	return dir;
}

typedef struct {
	LgRect rc;           /* before turning */
	int kind;            /* 0 a room, 1 a corridor, 2 a window, 3 a garden feature, 4 water, 5 garden, 6 a stone block */
	int type, room;      /* a room's */
	int lo, hi, ramp;    /* floors (a window: sill and head); ramp: LG_RAMP_* before turning, 0 none */
	int theme;           /* garden features: 0 the garden's own */
	int norail;          /* garden features: their ledges unrailed (the walls' tops) */
} LgVlPiece;

static int lgVlFree(const uint8_t (*occ)[LG_GRIDMAX], int x0, int z0, int w, int d, int G)
{
	int x, z;

	if (x0 < 1 || z0 < 1 || x0 + w > G - 1 || z0 + d > G - 1) {
		return 0;
	}
	for (z = z0; z < z0 + d; z++) {
		for (x = x0; x < x0 + w; x++) {
			if (occ[z][x]) {
				return 0;
			}
		}
	}
	return 1;
}

static void lgVlMark(uint8_t (*occ)[LG_GRIDMAX], int x0, int z0, int w, int d, int G, int v)
{
	int x, z;

	for (z = z0 < 0 ? 0 : z0; z < z0 + d && z < G; z++) {
		for (x = x0 < 0 ? 0 : x0; x < x0 + w && x < G; x++) {
			occ[z][x] = (uint8_t)v;
		}
	}
}

/* what lgPlaceVilla builds before it becomes areas */
static struct {
	LgVlPiece pc[LG_MAXAREAS + 64];
	int np, nrooms;
	uint8_t occ[LG_GRIDMAX][LG_GRIDMAX];
	uint8_t door[LG_GRIDMAX][LG_GRIDMAX];   /* in front of a door or a window: kept clear */
} g_LgVp;

/* free and nobody's doorstep or window */
static int lgVlFreeNoDoor(int x0, int z0, int w, int d, int G)
{
	int x, z;

	if (!lgVlFree((const uint8_t (*)[LG_GRIDMAX])g_LgVp.occ, x0, z0, w, d, G)) {
		return 0;
	}
	for (z = z0; z < z0 + d; z++) {
		for (x = x0; x < x0 + w; x++) {
			if (g_LgVp.door[z][x]) {
				return 0;
			}
		}
	}
	return 1;
}


static int lgVlAdd(int x, int z, int w, int d, int kind, int type, int room, int lo, int hi, int ramp, int theme)
{
	LgVlPiece *q;

	if (g_LgVp.np >= (int)(sizeof(g_LgVp.pc) / sizeof(g_LgVp.pc[0])) || w < 1 || d < 1) {
		return -1;
	}
	q = &g_LgVp.pc[g_LgVp.np];
	q->rc.x = (int16_t)x; q->rc.z = (int16_t)z; q->rc.w = (int16_t)w; q->rc.d = (int16_t)d; q->rc.room = -1;
	q->kind = kind; q->type = type; q->room = room; q->lo = lo; q->hi = hi; q->ramp = ramp; q->theme = theme;
	q->norail = 0;
	if (kind != 0 && kind != 1 && kind != 2 && kind != 5) {
		lgVlMark(g_LgVp.occ, x, z, w, d, g_LgVl.G, 1);
	}
	return g_LgVp.np++;
}

/* A wing: at x0, z0 (as laid out), its length along x (alongx) or z, depth
 * WD across (v, from its low coordinate side). Its corridor runs down the
 * middle in two lanes side by side, changing lanes every few rooms (the runs
 * overlap by three cells, where one lane opens into the other), so no
 * corridor runs straight far. Small rooms either side of each run: on the
 * lane's own side out to the wall, on the other side across the other lane
 * out to the other wall. Faces: v low and v high; face[] 0 nothing outside,
 * 1 the garden level with the floor (doors and windows), 2 a terrace level
 * with the floor (doors), 3 a drop (windows only); openings only between
 * olo[] and ohi[] along it, and not on rlo[]..rhi[] (a ramp against it). A
 * door is a passage from the corridor out through the rooms' row and the
 * wall. ends: doors out at its two ends. Returns the doors made. */
typedef struct {
	int nseg, segs[16][2], lane[16];
	int pass[2][8], npass[2];                    /* door passages per face (u) */
} LgVlWingPlan;

/* (planned first, its door passages chosen, so that the ramp up its roof can
 * keep clear of them; rlo, rhi -100 here) */
static int lgVlWing(LgRng *r, int x0, int z0, int alongx, int Lw, int WD, int floor, const int face[2],
		const int olo[2], const int ohi[2], int ends, const int rlo[2], const int rhi[2], LgVlWingPlan *pl, int build)
{
	const int U0 = LG_VL_BAND, U1 = Lw - LG_VL_BAND, V0 = LG_VL_BAND, V1 = WD - LG_VL_BAND;
	const int c = V0 + (V1 - V0) / 2;            /* lane 0 is c-3..c, lane 1 c..c+3 */
	const int sill = floor + LG_WIN_SILL, head = floor + LG_WIN_HEAD;
	int k, doors = 0, side;
	int last[2] = { -100, -100 };                /* the last room's end, each side of the middle */
#define nseg (pl->nseg)
#define segs (pl->segs)
#define lane (pl->lane)
#define pass (pl->pass)
#define npass (pl->npass)

#define VLUV(u_, v_, du_, dv_, kind_, type_, room_, lo_, hi_) \
	lgVlAdd(alongx ? x0 + (u_) : x0 + (v_), alongx ? z0 + (v_) : z0 + (u_), alongx ? (du_) : (dv_), alongx ? (dv_) : (du_), kind_, type_, room_, lo_, hi_, 0, 0)
	/* whether a 3-cell opening at u on a face is allowed */
	/* the three cells outside an opening at u on face f (or an end, f 2 low, 3 high) */
#define VLDOOR(f_, u_, v_) do { int q_; for (q_ = 0; q_ < 3; q_++) { \
		const int du_ = (f_) == 2 ? -1 : (f_) == 3 ? Lw : (u_) + q_, dv_ = (f_) == 2 || (f_) == 3 ? (v_) + q_ : (f_) ? WD : -1; \
		const int gx_ = alongx ? x0 + du_ : x0 + dv_, gz_ = alongx ? z0 + dv_ : z0 + du_; \
		if (gx_ >= 0 && gz_ >= 0 && gx_ < LG_GRIDMAX && gz_ < LG_GRIDMAX) g_LgVp.door[gz_][gx_] = 1; } } while (0)
#define VLCLEAR(f_, u_) ((u_) >= olo[f_] && (u_) + 3 <= ohi[f_] && ((u_) + 3 <= rlo[f_] - 1 || (u_) >= rhi[f_] + 1))

	if (build) {
		goto built;
	}
	npass[0] = npass[1] = 0;
	/* the runs: SEGMIN..SEGMAX long, overlapping by three */
	{
		const int U = U1 - U0;
		int n = (U - 3) / (LG_VL_SEGMAX - 3) + 1, pos;

		while (n > 1 && (U + 3 * (n - 1)) / n < LG_VL_SEGMIN) {
			n--;
		}
		/* (more if the last would run long) */
		while ((U + 3 * (n - 1)) / n > LG_VL_SEGMAX - 1 && n < 16) {
			n++;
		}
		if (n > 16) n = 16;
		pos = U0;
		for (k = 0; k < n; k++) {
			const int remain = U1 - pos;
			int len = (remain + 3 * (n - 1 - k)) / (n - k);

			if (k < n - 1) {
				len += k < n - 2 ? lgRange(r, -1, 1) : 0;
				if (len < 7) len = 7;
			} else {
				len = remain;
			}
			segs[k][0] = pos; segs[k][1] = pos + len;
			pos += len - 3;
		}
		nseg = n;
	}
	/* the first run's lane: a raised wing's on its terrace's side (its doors) */
	side = face[1] == 2 ? 1 : face[0] == 2 ? 0 : (int)(lgRand(r) & 1);
	for (k = 0; k < nseg; k++) {
		lane[k] = (side + k) & 1;
	}

	/* the door passages: from a run of the lane on that face's side, out
	 * through the rooms and the wall (a terrace's face: two; the garden's: one) */
	for (side = 0; side < 2; side++) {
		const int want = face[side] == 2 ? (ohi[side] - olo[side] >= 32 ? 2 : 1) : face[side] == 1 ? 1 : 0;
		int cand[128], ncand = 0, kk, u;

		/* every place one may go, then some of them, apart */
		for (kk = 0; kk < nseg; kk++) {
			for (u = segs[kk][0] + 1; u + 4 <= segs[kk][1] && ncand < 128; u++) {
				if (lane[kk] == side && VLCLEAR(side, u)) {
					cand[ncand++] = u;
				}
			}
		}
		while (npass[side] < want && ncand > 0) {
			const int pick = lgRange(r, 0, ncand - 1), pu = cand[pick];
			int q;

			pass[side][npass[side]++] = pu;
			/* (the rest: only those well apart from it) */
			for (q = 0; q < ncand; ) {
				if (cand[q] + 3 + 6 > pu && cand[q] < pu + 3 + 6) {
					cand[q] = cand[--ncand];
				} else {
					q++;
				}
			}
		}
		/* (sorted along the wing) */
		for (k = 0; k < npass[side]; k++) {
			int q;

			for (q = k + 1; q < npass[side]; q++) {
				if (pass[side][q] < pass[side][k]) {
					const int t = pass[side][q]; pass[side][q] = pass[side][k]; pass[side][k] = t;
				}
			}
		}
	}
	return npass[0] + npass[1] + (ends ? 2 : 0);

built:
	for (side = 0; side < 2; side++) {
		for (k = 0; k < npass[side]; k++) {
			if (side) {
				VLUV(pass[side][k], c + 3, 3, WD - c - 3, 1, 0, -1, floor, floor);
				VLDOOR(1, pass[side][k], 0);
			} else {
				VLUV(pass[side][k], 0, 3, c - 3, 1, 0, -1, floor, floor);
				VLDOOR(0, pass[side][k], 0);
			}
			doors++;
		}
	}

	/* the runs, the ends' doors and the rooms */
	for (k = 0; k < nseg; k++) {
		const int u0 = segs[k][0], u1 = segs[k][1], l = lane[k];
		int pass2;

		VLUV(u0, l ? c : c - 3, u1 - u0, 3, 1, 0, -1, floor, floor);
		if (ends && k == 0) {
			VLUV(0, l ? c : c - 3, U0, 3, 1, 0, -1, floor, floor);
			VLDOOR(2, 0, l ? c : c - 3);
			doors++;
		}
		if (ends && k == nseg - 1) {
			VLUV(U1, l ? c : c - 3, Lw - U1, 3, 1, 0, -1, floor, floor);
			VLDOOR(3, 0, l ? c : c - 3);
			doors++;
		}
		/* rooms: pass2 0 on the lane's own side (v from the wall to the lane),
		 * 1 the other side (across the other lane, where it is not a run) */
		for (pass2 = 0; pass2 < 2; pass2++) {
			const int s = pass2 ? !l : l;           /* the side of the middle: 0 low v */
			const int ra = pass2 ? (k > 0 ? u0 + 3 : u0) : u0;
			const int rb = pass2 ? (k < nseg - 1 ? u1 - 3 : u1) : u1;
			const int v0 = s ? (pass2 ? c : c + 3) : V0;
			const int v1 = s ? V1 : (pass2 ? c : c - 3);
			int cx = last[s] + LG_ROOMGAP > ra ? last[s] + LG_ROOMGAP : ra;

			while (cx + 4 <= rb && g_LgVp.nrooms < LG_MAXROOMS - 2) {
				int rw = lgRange(r, 4, 6), end = rb, q, a;
				const int type = lgRange(r, 0, 9) < 7 ? LG_ROOM_OFFICE : LG_ROOM_STORAGE;

				/* up to the next door passage on this side */
				for (q = 0; q < npass[s]; q++) {
					if (pass[s][q] + 3 > cx && pass[s][q] < end) {
						end = pass[s][q];
					}
				}
				if (end - cx < 4) {
					if (end == rb) {
						break;
					}
					cx = end + 3;   /* past the passage */
					continue;
				}
				/* (no sliver at the end: one room to it, or two halves) */
				if (end - cx <= 9) {
					rw = end - cx;
				} else if (end - cx - rw - LG_ROOMGAP < 4) {
					rw = (end - cx - LG_ROOMGAP) / 2;
				}
				VLUV(cx, v0, rw, v1 - v0, 0, type, g_LgVp.nrooms, floor, floor);
				g_LgVp.nrooms++;
				last[s] = cx + rw;
				/* its windows on the face it reaches (one, two in a wide room) */
				if (face[s]) {
					for (a = 0; a < (rw >= 8 ? 2 : 1); a++) {
						const int wu = rw >= 8 ? cx + a * (rw - 3) : cx + (rw - 3 + 1) / 2;

						if (VLCLEAR(s, wu)) {
							VLUV(wu, s ? V1 : 0, 3, LG_VL_BAND, 2, 0, -1, sill, head);
						}
					}
				}
				cx += rw + LG_ROOMGAP;
			}
		}
	}
#undef VLUV
#undef VLCLEAR
#undef VLDOOR
#ifdef LGVL_DEBUG
	if (!doors) {
		fprintf(stderr, "wing Lw %d WD %d nseg %d face %d %d olo %d %d ohi %d %d rlo %d %d lanes", Lw, WD, nseg, face[0], face[1], olo[0], olo[1], ohi[0], ohi[1], rlo[0], rlo[1]);
		for (k = 0; k < nseg; k++) fprintf(stderr, " [%d,%d)L%d", segs[k][0], segs[k][1], lane[k]);
		fprintf(stderr, "\n");
	}
#endif
#undef nseg
#undef segs
#undef lane
#undef pass
#undef npass
	return doors;
}

/* Fills x0..x1, z0..z1 (as laid out) round whatever is there already with
 * pieces of `kind` at floor lo: runs of free cells along each row, a run the
 * same as the one above it joined to it; the largest first. Returns 0 if
 * there were too many. */
static int lgVlFill(int x0, int z0, int x1, int z1, int kind, int lo, int theme)
{
	static int runs[4096][4];   /* x0, x1, z0, z1 */
	int nr = 0, x, z, a, c, first = g_LgVp.np;

	for (z = z0; z < z1; z++) {
		x = x0;
		while (x < x1) {
			int e, k, joined = 0;

			if (g_LgVp.occ[z][x]) { x++; continue; }
			for (e = x; e < x1 && !g_LgVp.occ[z][e]; e++);
			for (k = 0; k < nr && !joined; k++) {
				if (runs[k][0] == x && runs[k][1] == e && runs[k][3] == z) {
					runs[k][3] = z + 1;
					joined = 1;
				}
			}
			if (!joined) {
				if (nr >= 4096) return 0;
				runs[nr][0] = x; runs[nr][1] = e; runs[nr][2] = z; runs[nr][3] = z + 1;
				nr++;
			}
			x = e;
		}
	}
	for (a = 0; a < nr; a++) {
		for (c = a + 1; c < nr; c++) {
			if ((runs[c][1] - runs[c][0]) * (runs[c][3] - runs[c][2]) > (runs[a][1] - runs[a][0]) * (runs[a][3] - runs[a][2])) {
				int t[4];

				memcpy(t, runs[a], sizeof(t)); memcpy(runs[a], runs[c], sizeof(t)); memcpy(runs[c], t, sizeof(t));
			}
		}
	}
	for (a = 0; a < nr; a++) {
		if (lgVlAdd(runs[a][0], runs[a][2], runs[a][1] - runs[a][0], runs[a][3] - runs[a][2], kind, 0, -1, lo, lo, 0, theme) < 0) {
			return 0;
		}
	}
	(void)first;
	return 1;
}

/* A stone hedge (a block nobody walks on) at x, z: windows it stands
 * against are walled up */
static void lgVlHedge(int x, int z, int w, int d)
{
	int i;

	for (i = 0; i < g_LgVp.np; i++) {
		LgVlPiece *q = &g_LgVp.pc[i];
		const int ox = q->rc.x < x + w && x < q->rc.x + q->rc.w, oz = q->rc.z < z + d && z < q->rc.z + q->rc.d;

		if (q->kind == 2 && q->lo < LG_VL_WALLTOP && ((ox && (q->rc.z + q->rc.d == z || z + d == q->rc.z)) || (oz && (q->rc.x + q->rc.w == x || x + w == q->rc.x)))) {
			q->kind = -1;
		}
	}
	lgVlAdd(x, z, w, d, 6, 0, -1, LG_VL_WALLTOP, LG_VL_WALLTOP, 0, LG_VL_HEDGE_THEME);
}

/* A ramp up the side of something (a wing's face or the grounds' wall):
 * three cells out from the line `line` (the face's outside edge: low v side
 * out (out = -1) or high (out = 1)), six long along it and rising to a
 * landing three cells long at `hi`, against the line. alongx: the line runs
 * along x. u: where the footprint starts along it. dirup: the ramp rises
 * towards +u (landing at u + 6) or -u (landing at u). Returns 1 if made. */
static int lgVlSideRamp(int alongx, int line, int out, int u, int dirup, int lo, int hi, int theme, int lotheme)
{
	const int G = g_LgVl.G;
	const int v0 = out < 0 ? line - 3 : line;
	const int ru = dirup ? u : u + 3, lu = dirup ? u + 6 : u;
	const int dir = alongx ? (dirup ? LG_RAMP_XP : LG_RAMP_XN) : (dirup ? LG_RAMP_ZP : LG_RAMP_ZN);

	if (alongx ? !lgVlFree((const uint8_t (*)[LG_GRIDMAX])g_LgVp.occ, u - 1, v0 - (out < 0 ? 1 : 0), 11, 4, G)
			: !lgVlFree((const uint8_t (*)[LG_GRIDMAX])g_LgVp.occ, v0 - (out < 0 ? 1 : 0), u - 1, 4, 11, G)) {
		return 0;
	}
	if (alongx) {
		lgVlAdd(ru, v0, 6, 3, 3, 0, -1, lo, hi, dir, lotheme);
		lgVlAdd(lu, v0, 3, 3, 3, 0, -1, hi, hi, 0, theme);
	} else {
		lgVlAdd(v0, ru, 3, 6, 3, 0, -1, lo, hi, dir, lotheme);
		lgVlAdd(v0, lu, 3, 3, 3, 0, -1, hi, hi, 0, theme);
	}
	return 1;
}

#ifdef LGVL_DEBUG
#define LGVL_FAIL() fprintf(stderr, "villa: failed at line %d\n", __LINE__)
#else
#define LGVL_FAIL() ((void)0)
#endif
/* The pieces (g_LgVp) made into the layout's areas: the rooms (0..n-1), the
 * garden's main (n), the rest of the garden and its features, the water, then
 * corridors and windows. top: the open areas' ceiling. */
static int lgVlPiecesToAreas(LgLayout *lg, int top)
{
	LgVlPiece *pc = g_LgVp.pc;
	int i, x, z, gardenroom, np;

	np = g_LgVp.np;

	/* the areas: the rooms (0..n-1), the garden's main (n), the rest of the
	 * garden and its features, the water, then corridors and windows */
	gardenroom = g_LgVp.nrooms;
	lg->numrooms = (int16_t)(gardenroom + 1);
	lg->numareas = 0;
	{
		int pass, mainpc = -1;

		for (i = 0; i < np && mainpc < 0; i++) {
			if (pc[i].kind == 5) {
				mainpc = i;
			}
		}
		for (pass = 0; pass < 5; pass++) {
			for (i = 0; i < np; i++) {
				const LgVlPiece *q = &pc[i];
				const int want = q->kind == 0 ? 0 : i == mainpc ? 1 : (q->kind >= 3 && q->kind <= 6) ? 2 : q->kind == 1 ? 3 : q->kind == 2 ? 4 : -1;
				const LgRect rc = lgVlRect(q->rc.x, q->rc.z, q->rc.w, q->rc.d);
				LgArea *a;

				if (want != pass) {
					continue;
				}
				if (lg->numareas >= LG_MAXAREAS - 8) {
					{ LGVL_FAIL(); return 0; }
				}
				if (q->kind == 0) {
					a = &lg->areas[lg->numareas++];
					lgSetRoomArea(a, &rc, q->type, q->room, 0, 0);
					a->floor = a->floorlo = (int16_t)q->lo;
					a->ceil = (int16_t)(q->lo + LG_VL_ROOMCEIL);
				} else if (q->kind == 1) {
					lgStCorridor(lg, &rc, q->lo, q->hi, 0, 0, 0);
					lg->areas[lg->numareas - 1].ceil = (int16_t)(q->lo + LG_VL_ROOMCEIL);
				} else if (q->kind == 2) {
					lgStCorridor(lg, &rc, q->lo, q->lo, 0, 0, 0);
					a = &lg->areas[lg->numareas - 1];
					a->ceil = (int16_t)q->hi;
					a->special = LG_SPECIAL_WINDOW;
				} else {
					/* the garden's: its pieces and features, the water */
					a = &lg->areas[lg->numareas++];
					lgSetHubArea(a, &rc, gardenroom, q->lo, q->hi, q->ramp ? lgVlRampDir(q->ramp) : 0);
					a->open = 1;
					a->ceil = (int16_t)top;
					a->theme = (uint8_t)(q->kind == 4 ? LG_WATER_THEME : q->theme ? q->theme : LG_STREET_THEME);
					if (q->kind == 4) {
						a->special = LG_SPECIAL_PIT;
					} else if (q->kind == 6) {
						a->special = LG_SPECIAL_RACK;
					}
				}
			}
			if (pass == 0 && lg->numareas != gardenroom) {
				{ LGVL_FAIL(); return 0; }
			}
			if (pass == 1 && lg->numareas == gardenroom) {
				{ LGVL_FAIL(); return 0; }   /* (no garden) */
			}
		}
	}
	g_LgVl.garden = gardenroom;

	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];

		if (a->x < 1 || a->z < 1 || a->x + a->w > lg->gw - 1 || a->z + a->d > lg->gd - 1) {
#ifdef LGVL_DEBUG
			fprintf(stderr, "villa: area %d (%d,%d %dx%d k%d) out of bounds\n", i, a->x, a->z, a->w, a->d, a->kind);
#endif
			{ LGVL_FAIL(); return 0; }
		}
		for (z = a->z; z < a->z + a->d; z++) {
			for (x = a->x; x < a->x + a->w; x++) {
				if (lg->cells[z][x]) {
#ifdef LGVL_DEBUG
					fprintf(stderr, "villa: area %d (%d,%d %dx%d k%d f%d) overlaps %d (%d,%d %dx%d k%d f%d)\n", i, a->x, a->z, a->w, a->d, a->kind, a->floor, lg->cells[z][x] - 1, lg->areas[lg->cells[z][x] - 1].x, lg->areas[lg->cells[z][x] - 1].z, lg->areas[lg->cells[z][x] - 1].w, lg->areas[lg->cells[z][x] - 1].d, lg->areas[lg->cells[z][x] - 1].kind, lg->areas[lg->cells[z][x] - 1].floor);
#endif
					{ LGVL_FAIL(); return 0; }
				}
				lg->cells[z][x] = (int16_t)(i + 1);
			}
		}
	}
	return 1;
}


static int lgPlaceVilla(LgLayout *lg, LgRng *r)
{
	uint8_t (*occ)[LG_GRIDMAX] = g_LgVp.occ;
	LgVlPiece *pc = g_LgVp.pc;
	const int size = lg->size, G = lg->gw, WD = g_LgVilla[size].wingd, RING = g_LgVilla[size].ring;
	const int WW = LG_VL_WALLW, T = LG_VL_TERRACE;
	const int zq = G - 1 - LG_VL_WATER;           /* the quay's edge: the grounds above, the water below */
	const int cx0 = 1 + WW + RING, cx1 = G - 1 - WW - RING, cz0 = 1 + WW + RING, cz1 = zq - g_LgVilla[size].quay;
	const int top = LG_VL_EAVE2 + (WD / 2 + 1) * LG_VL_ROOFPITCH + LG_ST_OPENTOP;
	int i, b, x, z, gardenroom, np, nwings, front;
	int bx[LG_VL_MAXBLD], bz[LG_VL_MAXBLD], bw[LG_VL_MAXBLD], bd[LG_VL_MAXBLD], balongx[LG_VL_MAXBLD], braised[LG_VL_MAXBLD], bkind[LG_VL_MAXBLD];
	int sideraised;   /* -1 none, else the raised side wing's kind (2 west, 3 east) */
	int cyz1;         /* the courtyard's far edge */
	int plx0, plx1;   /* the planter across the courtyard by the front wing */

	g_LgVl.G = G;
	g_LgVl.fx = lgRand(r) & 1;
	g_LgVl.fz = lgRand(r) & 1;
	g_LgVl.swap = lgRand(r) & 1;
	memset(occ, 0, sizeof(g_LgVp.occ));
	memset(g_LgVp.door, 0, sizeof(g_LgVp.door));
	g_LgVp.np = 0;
	g_LgVp.nrooms = 0;

	if (cx1 - cx0 < 2 * WD + 2 * T) {
		{ LGVL_FAIL(); return 0; }
	}
	/* a front wing too (a courtyard closed all round) if the courtyard still
	 * has room for the terrace and the grand ramp up to it below that */
	front = cz1 - cz0 - 2 * WD >= T + 8 + 4;
	cyz1 = front ? cz1 - WD : cz1;

	/* the wings: the back (raised), the front (by the quay), west and east between */
	nwings = 0;
#define VLWING(k_, x_, z_, w_, d_, ax_) do { bkind[nwings] = (k_); bx[nwings] = (x_); bz[nwings] = (z_); bw[nwings] = (w_); bd[nwings] = (d_); balongx[nwings] = (ax_); nwings++; } while (0)
	VLWING(0, cx0, cz0, cx1 - cx0, WD, 1);
	if (front) {
		VLWING(1, cx0, cz1 - WD, cx1 - cx0, WD, 1);
	}
	VLWING(2, cx0, cz0 + WD + LG_VL_GAP, WD, (front ? cz1 - WD - LG_VL_GAP : cz1) - (cz0 + WD + LG_VL_GAP), 0);
	VLWING(3, cx1 - WD, cz0 + WD + LG_VL_GAP, WD, (front ? cz1 - WD - LG_VL_GAP : cz1) - (cz0 + WD + LG_VL_GAP), 0);
#undef VLWING
	/* (a raised side wing: room in the courtyard for its terrace, and along it
	 * for the ramp up its roof and its doors) */
	sideraised = lgRange(r, 0, 99) < 60 && cx1 - cx0 - 2 * WD >= 2 * T + 6 && bd[nwings - 1] >= 22 ? 2 + (int)(lgRand(r) & 1) : -1;
	/* (with a front wing, the passages either side of it face each other
	 * across the courtyard: a planter between, against the front wing,
	 * where it has no openings) */
	plx0 = plx1 = -100;
	if (front) {
		/* (two of them, one by each passage's mouth, two cells in) */
		const int span = cx1 - cx0 - 2 * WD - 4;

		if (span >= 10) {
			plx0 = cx0 + WD + 2;
			plx1 = cx1 - WD - 2;
		}
	}
	for (b = 0; b < nwings; b++) {
		braised[b] = bkind[b] == 0 || bkind[b] == sideraised;
		lgVlMark(occ, bx[b], bz[b], bw[b], bd[b], G, 1);
	}
	g_LgVl.nb = nwings;

	/* each wing: the ramp up its roof (from its terrace, a raised wing; the
	 * ring or the quay, a low one), then its inside */
	for (b = 0; b < nwings; b++) {
		const int k = bkind[b], alongx = balongx[b], Lw = alongx ? bw[b] : bd[b];
		const int floor = braised[b] ? LG_VL_UPPER : LG_VL_FLOOR;
		const int eave = braised[b] ? LG_VL_EAVE2 : LG_VL_EAVE;
		int face[2], olo[2] = { 0, 0 }, ohi[2] = { Lw, Lw }, rlo[2] = { -100, -100 }, rhi[2] = { -100, -100 }, rf, umin, umax, tries;
		LgVlWingPlan plan;

		/* faces: v low and v high. Back wing: ring | terrace. Front: courtyard | quay.
		 * West: ring | courtyard (or terrace). East: courtyard (or terrace) | ring.
		 * A raised wing's face over the ground is windows only. */
		switch (k) {
		case 0: face[0] = 3; face[1] = 2; olo[1] = WD; ohi[1] = Lw - WD; break;
		case 1: face[0] = 1; face[1] = 1; break;
		case 2: face[0] = braised[b] ? 3 : 1; face[1] = braised[b] ? 2 : 1; break;
		default: face[0] = braised[b] ? 2 : 1; face[1] = braised[b] ? 3 : 1; break;
		}
		/* a low side wing's courtyard face is the back terrace's plinth at first */
		if (k >= 2 && !braised[b]) {
			olo[k == 2 ? 1 : 0] = cz0 + WD + T - bz[b] + 1;
		}
		/* the front wing's courtyard face: not behind the planter */
		if (k == 1 && plx0 >= 0) {
			const int pw = (plx1 - plx0) / 3 > 8 ? 8 : (plx1 - plx0) / 3;

			olo[0] = plx0 + pw + 1 - bx[b]; ohi[0] = plx1 - pw - 1 - bx[b];
		}
		/* the roof's ramp: on the terrace's face (raised) or the outer face (low) */
		rf = k == 0 ? 1 : k == 1 ? 1 : k == 2 ? (braised[b] ? 1 : 0) : (braised[b] ? 0 : 1);
		umin = -6; umax = Lw - 3;
		if (k == 0) {
			umin = WD + 1; umax = Lw - WD - 10;   /* on the terrace, the terrace at both ends */
		} else if (braised[b]) {
			umin = 1 - LG_VL_GAP; umax = Lw - 10;
		}
		if (!lgVlWing(r, bx[b], bz[b], alongx, Lw, WD, floor, face, olo, ohi, !braised[b], rlo, rhi, &plan, 0)) {
			{ LGVL_FAIL(); return 0; }   /* (no way in) */
		}
		for (tries = 0; tries < 60; tries++) {
			const int u = lgRange(r, umin, umax), up = (int)(lgRand(r) & 1);
			const int lu = up ? u + 6 : u;   /* the landing, along the face */
			const int line = alongx ? (rf ? bz[b] + bd[b] : bz[b]) : (rf ? bx[b] + bw[b] : bx[b]);
			const int along = (alongx ? bx[b] : bz[b]) + u;
			int q, clash = 0;

			if (lu < 0 || lu + 3 > Lw || umax < umin) {
				continue;
			}
			/* (clear of the doors on that face) */
			for (q = 0; q < plan.npass[rf]; q++) {
				clash |= plan.pass[rf][q] + 3 + 1 > u && plan.pass[rf][q] < u + 9 + 1;
			}
			if (clash) {
				continue;
			}
			if (lgVlSideRamp(alongx, line, rf ? 1 : -1, along, up, braised[b] ? LG_VL_UPPER : LG_VL_FLOOR, eave,
					braised[b] ? LG_VL_TERRACE_THEME : 0, braised[b] ? LG_VL_TERRACE_THEME : 0)) {
				rlo[rf] = u; rhi[rf] = u + 9;
				break;
			}
		}
		if (tries == 60) {
			{ LGVL_FAIL(); return 0; }   /* (no way up its roof) */
		}
		if (!lgVlWing(r, bx[b], bz[b], alongx, Lw, WD, floor, face, olo, ohi, !braised[b], rlo, rhi, &plan, 1)) {
			{ LGVL_FAIL(); return 0; }   /* (no way in) */
		}
		g_LgVl.bld[b] = lgVlRect(bx[b], bz[b], bw[b], bd[b]);
		g_LgVl.ridgealongx[b] = alongx ^ g_LgVl.swap;
		g_LgVl.eave[b] = eave;
		g_LgVl.raised[b] = braised[b];
	}

	/* the grand ramp from the courtyard up to the back terrace, and the
	 * courtyard's planters: one across the line of the passages between the
	 * wings by the front, a fountain's block in the middle */
	{
		const int tx0 = cx0 + WD + (sideraised == 2 ? T : 0), tx1 = cx1 - WD - (sideraised == 3 ? T : 0);
		const int gz = cz0 + WD + T;
		int tries;

		for (tries = 0; tries < 40; tries++) {
			const int gx = lgRange(r, tx0 + 1, tx1 - 4);

			if (lgVlFree((const uint8_t (*)[LG_GRIDMAX])occ, gx - 1, gz, 5, 9, G)) {
				lgVlAdd(gx, gz, 3, 8, 3, 0, -1, LG_VL_FLOOR, LG_VL_UPPER, LG_RAMP_ZN, LG_VL_TERRACE_THEME);
				break;
			}
		}
		if (tries == 40) {
			{ LGVL_FAIL(); return 0; }
		}
		if (plx0 >= 0) {
			/* a planter at each end, the middle open: the front wing's doors there */
			const int pw = (plx1 - plx0) / 3 > 8 ? 8 : (plx1 - plx0) / 3;

			if (lgVlFree((const uint8_t (*)[LG_GRIDMAX])occ, plx0, cyz1 - LG_VL_GAP - 1, pw + 1, LG_VL_GAP + 1, G)) {
				lgVlHedge(plx0, cyz1 - LG_VL_GAP, pw, LG_VL_GAP);
			}
			if (lgVlFree((const uint8_t (*)[LG_GRIDMAX])occ, plx1 - pw - 1, cyz1 - LG_VL_GAP - 1, pw + 1, LG_VL_GAP + 1, G)) {
				lgVlHedge(plx1 - pw, cyz1 - LG_VL_GAP, pw, LG_VL_GAP);
			}
		}
		{
			const int qz0 = gz + 2, qz1 = cyz1 - (front ? LG_VL_GAP + 2 : 3), qx0 = tx0 + 2, qx1 = tx1 - 2;
			const int fw = (qx1 - qx0) / 3 > 6 ? 6 : (qx1 - qx0) / 3, fd = (qz1 - qz0) / 2 > 5 ? 5 : (qz1 - qz0) / 2;

			if (fw >= 2 && fd >= 2) {
				int t2;

				for (t2 = 0; t2 < 20; t2++) {
					const int fx = lgRange(r, qx0, qx1 - fw), fz = lgRange(r, qz0, qz1 - fd);

					if (lgVlFree((const uint8_t (*)[LG_GRIDMAX])occ, fx - 2, fz - 2, fw + 4, fd + 4, G)) {
						lgVlAdd(fx, fz, fw, fd, 6, 0, -1, LG_VL_POOLRIM, LG_VL_POOLRIM, 0, LG_VL_POOL_THEME);   /* (the courtyard's pool) */
						break;
					}
				}
			}
		}
	}

	/* the terraces (round the ramps on them): the back wing's across the
	 * courtyard's width, a raised side wing's down its side below that */
	{
		const int tx0 = cx0 + WD, tx1 = cx1 - WD, tz0 = cz0 + WD;

		if (!lgVlFill(tx0, tz0, tx1, tz0 + T, 3, LG_VL_UPPER, LG_VL_TERRACE_THEME)) {
			{ LGVL_FAIL(); return 0; }
		}
		for (b = 0; b < nwings; b++) {
			if (bkind[b] == sideraised) {
				const int sx = sideraised == 2 ? tx0 : tx1 - T;

				if (!lgVlFill(sx, tz0 + T, sx + T, bz[b] + bd[b], 3, LG_VL_UPPER, LG_VL_TERRACE_THEME)) {
					{ LGVL_FAIL(); return 0; }
				}
			}
		}
	}

	/* the grounds' walls: west and east the whole way to the water, north between */
	lgVlAdd(1, 1, WW, zq - 1, 3, 0, -1, LG_VL_WALLTOP, LG_VL_WALLTOP, 0, LG_VL_WALL_THEME);
	lgVlAdd(G - 1 - WW, 1, WW, zq - 1, 3, 0, -1, LG_VL_WALLTOP, LG_VL_WALLTOP, 0, LG_VL_WALL_THEME);
	lgVlAdd(1 + WW, 1, G - 2 - 2 * WW, WW, 3, 0, -1, LG_VL_WALLTOP, LG_VL_WALLTOP, 0, LG_VL_WALL_THEME);

	/* across the ring, every few cells along each side: a wall out from the
	 * grounds' wall (a gate left by the house), or a ramp up onto it along it.
	 * The roofs' ramps stand out from the house between them. */
	{
		int s;

		for (s = 0; s < 3; s++) {
			/* s 0 west (u along z), 1 east, 2 north (u along x) */
			const int u0 = 1 + WW + 1, u1 = s < 2 ? zq - 1 : G - 2 - WW;
			int u = u0 + lgRange(r, 0, 3), n = 0, ramps = 0;

			while (u + 3 <= u1) {
				/* walls out from the grounds' wall and hedges out from the house by
				 * turns (their gates at opposite sides), now and then a ramp up */
				const int ramp = (n & 1) == 0 && (ramps == 0 ? n >= 2 || (lgRand(r) & 1) : lgRange(r, 0, 3) == 0);
				const int hedge = n & 1;
				int ok;

				if (hedge) {
					const int len = RING - 3;
					int x, zz, w, d;

					if (s == 0) { x = cx0 - len; zz = u; w = len; d = 3; }
					else if (s == 1) { x = cx1; zz = u; w = len; d = 3; }
					else { x = u; zz = cz0 - len; w = 3; d = len; }
					ok = lgVlFree((const uint8_t (*)[LG_GRIDMAX])occ, s == 2 ? x - 1 : x - (s == 1 ? 0 : 1), zz - 1, s == 2 ? 5 : w + 1, s == 2 ? d + 1 : 5, G)
						&& lgVlFreeNoDoor(x, zz, w, d, G);
					if (ok) {
						lgVlHedge(x, zz, w, d);
					}
				} else if (ramp) {
					const int alongx = s == 2, line = s == 0 ? 1 + WW : s == 1 ? G - 1 - WW : 1 + WW;

					ok = u + 9 <= u1 && lgVlSideRamp(alongx, line, s == 1 ? -1 : 1, u, (int)(lgRand(r) & 1), LG_VL_FLOOR, LG_VL_WALLTOP, LG_VL_WALL_THEME, LG_VL_WALL_THEME);
					ramps += ok;
				} else {
					const int len = RING - 3;
					int x, zz, w, d, cx, cz, cw, cd;

					if (s == 0) { x = 1 + WW; zz = u; w = len; d = 3; cx = x; cz = zz - 1; cw = w + 1; cd = 5; }
					else if (s == 1) { x = G - 1 - WW - len; zz = u; w = len; d = 3; cx = x - 1; cz = zz - 1; cw = w + 1; cd = 5; }
					else { x = u; zz = 1 + WW; w = 3; d = len; cx = x - 1; cz = zz; cw = 5; cd = d + 1; }
					ok = lgVlFree((const uint8_t (*)[LG_GRIDMAX])occ, cx, cz, cw, cd, G);
					if (ok) {
						lgVlAdd(x, zz, w, d, 3, 0, -1, LG_VL_WALLTOP, LG_VL_WALLTOP, 0, LG_VL_WALL_THEME);
					}
				}
				if (!ok) {
					u++;
					continue;
				}
				n++;
				u += (ramp ? 9 : 3) + lgRange(r, 4, 7);
			}
			if (ramps == 0) {
				{ LGVL_FAIL(); return 0; }
			}
		}
	}

	/* the quay: stone hedges across it from the water, a gate by the house */
	{
		const int qd = zq - cz1;
		int u = cx0 + lgRange(r, 4, 8);

		int n = 0;

		while (u + 3 < cx1 - 2) {
			/* (from the water, a gate by the house; or from the house, a gate by the water) */
			const int fromhouse = n & 1;
			const int hz = fromhouse ? cz1 : cz1 + 4, hd = qd - 4;

			if (qd >= 7 && lgVlFree((const uint8_t (*)[LG_GRIDMAX])occ, u - 1, fromhouse ? cz1 : cz1 + 3, 5, fromhouse ? hd + 1 : qd - 3, G)
					&& lgVlFreeNoDoor(u, hz, 3, hd, G)) {
				lgVlHedge(u, hz, 3, hd);
				u += 11 + lgRange(r, 0, 4);
				n++;
			} else {
				u += 2;
			}
		}
	}

	/* the gardens: pools, flower beds and clipped hedges standing in the
	 * grounds wherever two cells stay clear all round them (the paths between) */
	{
		static const int8_t npools[LG_SIZE_COUNT] = { 2, 3, 4 }, nbeds[LG_SIZE_COUNT] = { 4, 6, 8 }, ntop[LG_SIZE_COUNT] = { 5, 8, 11 };
		int pass;

		for (pass = 0; pass < 3; pass++) {
			const int kind = pass == 1 ? 2 : pass == 2 ? 1 : 0;   /* pools, beds, then hedges in what is left */
			int want = kind == 0 ? npools[size] : kind == 1 ? ntop[size] : nbeds[size], tries;

			for (tries = 0; tries < 400 && want > 0; tries++) {
				const int w = kind == 0 ? lgRange(r, 4, 7) : kind == 1 ? lgRange(r, 2, 4) : lgRange(r, 2, 5);
				const int d = kind == 0 ? lgRange(r, 3, 5) : kind == 1 ? (w > 2 ? 2 : lgRange(r, 2, 4)) : lgRange(r, 2, 3);
				const int x = lgRange(r, 2 + WW + 2, G - 2 - WW - 2 - w), z = lgRange(r, 2 + WW + 2, zq - 2 - d);

				if (x < 1 || z < 1 || z + d + 2 > zq || !lgVlFree((const uint8_t (*)[LG_GRIDMAX])occ, x - 2, z - 2, w + 4, d + 4, G)
						|| !lgVlFreeNoDoor(x - 1, z - 1, w + 2, d + 2, G)) {
					continue;
				}
				if (kind == 0) {
					lgVlAdd(x, z, w, d, 6, 0, -1, LG_VL_POOLRIM, LG_VL_POOLRIM, 0, LG_VL_POOL_THEME);
				} else if (kind == 1) {
					lgVlAdd(x, z, w, d, 6, 0, -1, LG_VL_WALLTOP, LG_VL_WALLTOP, 0, LG_VL_HEDGE_THEME);
				} else {
					lgVlAdd(x, z, w, d, 6, 0, -1, LG_VL_BEDTOP, LG_VL_BEDTOP, 0, LG_VL_BED_THEME);
				}
				want--;
			}
		}
	}

	/* the water and the piers out over it */
	{
		const int np0 = g_LgVilla[size].piers, PL = LG_VL_WATER - 2;
		int px[4], n = 0, k, wx = 1;

		for (k = 0; k < np0; k++) {
			const int span = (G - 2) / np0;

			px[n++] = 1 + k * span + lgRange(r, 2, span - 6);
			if (px[n - 1] < 2 + WW) px[n - 1] = 2 + WW;
			if (px[n - 1] > G - 6 - WW) px[n - 1] = G - 6 - WW;
			/* (onto the quay's garden: not in front of a hedge or a ramp) */
			{
				int d, best = -1;

				for (d = 0; d < G && best < 0; d++) {
					int c, sgn;

					for (sgn = 0; sgn < 2 && best < 0; sgn++) {
						const int xx = px[n - 1] + (sgn ? -d : d);

						if (xx < 2 + WW || xx > G - 6 - WW || (k > 0 && xx < px[n - 2] + 4)) continue;
						for (c = 0; c < 3 && !occ[zq - 1][xx + c]; c++);
						best = c == 3 ? xx : -1;
					}
				}
				if (best < 0) {
					{ LGVL_FAIL(); return 0; }
				}
				px[n - 1] = best;
			}
		}
		for (k = 0; k < n; k++) {
			lgVlAdd(px[k], zq, 3, PL, 3, 0, -1, LG_VL_FLOOR, LG_VL_FLOOR, 0, LG_DOCK_THEME);
			if (px[k] > wx) {
				lgVlAdd(wx, zq, px[k] - wx, LG_VL_WATER, 4, 0, -1, 0, 0, 0, 0);
			}
			lgVlAdd(px[k], zq + PL, 3, LG_VL_WATER - PL, 4, 0, -1, 0, 0, 0, 0);
			wx = px[k] + 3;
		}
		if (wx < G - 1) {
			lgVlAdd(wx, zq, G - 1 - wx, LG_VL_WATER, 4, 0, -1, 0, 0, 0, 0);
		}
	}

#ifdef LGVL_DEBUG
	if (getenv("LGVL_DUMP")) {
		fprintf(stderr, "swap %d fx %d fz %d side %d front %d cx %d..%d cz %d..%d\n", g_LgVl.swap, g_LgVl.fx, g_LgVl.fz, sideraised, front, cx0, cx1, cz0, cz1);
		for (b = 0; b < nwings; b++) fprintf(stderr, "wing k%d %d,%d %dx%d raised %d\n", bkind[b], bx[b], bz[b], bw[b], bd[b], braised[b]);
		for (i = 0; i < g_LgVp.np; i++) if (pc[i].kind == 3 || pc[i].kind == 6) fprintf(stderr, "pc %d k%d %d,%d %dx%d lo %d hi %d ramp %d th %d\n", i, pc[i].kind, pc[i].rc.x, pc[i].rc.z, pc[i].rc.w, pc[i].rc.d, pc[i].lo, pc[i].hi, pc[i].ramp, pc[i].theme);
	}
#endif
	/* the garden: the grounds less everything above */
	if (!lgVlFill(1, 1, G - 1, zq, 5, LG_VL_FLOOR, 0)) {
		{ LGVL_FAIL(); return 0; }
	}
	return lgVlPiecesToAreas(lg, top);
}

/* Water's edges, the piers, the stone blocks and the walls' tops have no
 * railings */
static void lgVlWaterPortals(LgLayout *lg)
{
	int i;

	for (i = 0; i < lg->numportals; i++) {
		LgPortal *p = &lg->portals[i];
		const LgArea *a = &lg->areas[p->a], *b = &lg->areas[p->b];

		if (a->special == LG_SPECIAL_PIT || b->special == LG_SPECIAL_PIT || a->special == LG_SPECIAL_RACK || b->special == LG_SPECIAL_RACK
				|| (a->theme == LG_VL_WALL_THEME && !a->ramp && a->floor == LG_VL_WALLTOP && a->open)
				|| (b->theme == LG_VL_WALL_THEME && !b->ramp && b->floor == LG_VL_WALLTOP && b->open)) {
			p->norail = 1;
		}
	}
}

/* A roof piece (layer 1, the garden's room) over rc: floor from lo at its
 * eave to hi at the ridge, rising along dir. Returns its area or -1 */
static int lgVlRoofPiece(LgLayout *lg, const LgRect *rc, int lo, int hi, int dir, char *err, int errlen)
{
	const int gi = lg->numareas, base = g_LgVl.garden;
	LgArea *g;
	int x, z;

	for (z = rc->z; z < rc->z + rc->d; z++) {
		for (x = rc->x; x < rc->x + rc->w; x++) {
			if (lg->cells2[z][x]) {
				lgSetErr(err, errlen, "a roof over another at cell %d", z * lg->gw + x, 0);
				return -1;
			}
		}
	}
	if (gi >= LG_MAXAREAS) {
		lgSetErr(err, errlen, "too many areas for a roof (%d)", gi, 0);
		return -1;
	}
	g = &lg->areas[lg->numareas++];
	memset(g, 0, sizeof(*g));
	g->x = rc->x; g->z = rc->z; g->w = rc->w; g->d = rc->d;
	g->kind = LG_AREA_ROOM;
	g->roomtype = lg->areas[base].roomtype;
	g->theme = LG_ROOF_THEME;
	g->room = lg->areas[base].room;
	g->floor = (int16_t)hi;
	g->floorlo = (int16_t)lo;
	g->ramp = (uint8_t)dir;
	g->ceil = lg->areas[base].ceil;
	g->layer = 1;
	g->open = 1;
	g->special = LG_SPECIAL_ROOF;
	g->dist = -1;
	g->centrepad = -1;
	lgMezzFill(lg, g, gi);
	return gi;
}

/* A roof piece's openings, cell by cell round it: onto the other slope at
 * the ridge (level), onto a landing at the eave (level), onto the garden
 * below (a drop, no railing); elsewhere nothing */
static int lgVlRoofEdges(LgLayout *lg, int gi, char *err, int errlen)
{
	const LgArea g = lg->areas[gi];
	int side;

	for (side = 0; side < 4; side++) {
		const int alongz = side < 2, n = alongz ? g.d : g.w;
		const int line = side == 0 ? g.x : side == 1 ? g.x + g.w : side == 2 ? g.z : g.z + g.d;
		const int base = alongz ? g.z : g.x;
		int i = 0, kind[LG_GRIDMAX], other[LG_GRIDMAX];

		for (i = 0; i < n; i++) {
			const int nx = side == 0 ? g.x - 1 : side == 1 ? g.x + g.w : g.x + i;
			const int nz = side == 2 ? g.z - 1 : side == 3 ? g.z + g.d : g.z + i;
			/* the roof's floor at the middle of this cell's edge */
			const int32_t ex = lgWorldX(lg, alongz ? line : base + i) + (alongz ? 0 : LG_CELL / 2);
			const int32_t ez = lgWorldZ(lg, alongz ? base + i : line) + (alongz ? LG_CELL / 2 : 0);
			const int32_t rf = modLevelGenFloorAt(lg, gi, ex, ez);

			kind[i] = 0; other[i] = -1;
			if (nx < 0 || nz < 0 || nx >= lg->gw || nz >= lg->gd) {
				continue;
			}
			if (lg->cells2[nz][nx] > 0) {
				const int up = lg->cells2[nz][nx] - 1;

				if (up < gi && lg->areas[up].special == LG_SPECIAL_ROOF) {
					kind[i] = 1; other[i] = up;      /* the other slope */
				}
				continue;
			}
			if (lg->cells[nz][nx] > 0) {
				const int dn = lg->cells[nz][nx] - 1;
				const LgArea *o = &lg->areas[dn];

				if (o->room != g.room || o->layer || o->special == LG_SPECIAL_PIT) {
					continue;
				}
				if (!o->ramp && o->floor == rf) {
					kind[i] = 2; other[i] = dn;      /* a landing */
				} else if (o->floor < g.floorlo && o->floor < rf) {
					kind[i] = 3; other[i] = dn;      /* over the garden */
				}
			}
		}
		i = 0;
		while (i < n) {
			int j;

			for (j = i + 1; j < n && kind[j] == kind[i] && other[j] == other[i]; j++);
			if (kind[i]) {
				const int lowo = side == 0 || side == 2;

				if (!lgStPortal2(lg, alongz, line, base + i, base + j, lowo ? other[i] : gi, lowo ? gi : other[i], kind[i] == 3, 1, err, errlen)) {
					return 0;
				}
			}
			i = j;
		}
	}
	return 1;
}

/* Every building's roof: two slopes from the eaves on its long sides up to a
 * ridge along the middle */
static int lgVlRoofs(LgLayout *lg, char *err, int errlen)
{
	int b, k, made[LG_VL_MAXBLD][2];

	for (b = 0; b < g_LgVl.nb; b++) {
		const LgRect B = g_LgVl.bld[b];
		const int alongx = g_LgVl.ridgealongx[b];   /* (the ridge runs along the building's length, the eaves where its ramps come up) */
		const int depth = alongx ? B.d : B.w, h1 = depth / 2, h2 = depth - h1;
		const int eave = g_LgVl.eave[b], ridge = eave + LG_VL_ROOFPITCH * (h1 < h2 ? h1 : h2);

		for (k = 0; k < 2; k++) {
			LgRect rc = B;
			int dir;

			if (alongx) {
				rc.z = (int16_t)(k ? B.z + h1 : B.z); rc.d = (int16_t)(k ? h2 : h1);
				dir = k ? LG_RAMP_ZN : LG_RAMP_ZP;
			} else {
				rc.x = (int16_t)(k ? B.x + h1 : B.x); rc.w = (int16_t)(k ? h2 : h1);
				dir = k ? LG_RAMP_XN : LG_RAMP_XP;
			}
			made[b][k] = lgVlRoofPiece(lg, &rc, eave, ridge, dir, err, errlen);
			if (made[b][k] < 0) {
				return 0;
			}
		}
	}
	for (b = 0; b < g_LgVl.nb; b++) {
		for (k = 0; k < 2; k++) {
			if (!lgVlRoofEdges(lg, made[b][k], err, errlen)) {
				return 0;
			}
		}
	}
	return 1;
}

static int lgTryVilla(LgLayout *lg, const LgParams *p, LgRng *r, char *err, int errlen)
{
	(void)p;

	if (!lgPlaceVilla(lg, r)) {
		lgSetErr(err, errlen, "the villa did not fit (%d areas)", lg->numareas, 0);
		return 0;
	}
	if (!lgFindPortals(lg, r, err, errlen)) {
		return 0;
	}
	lgVlWaterPortals(lg);
	return lgVlRoofs(lg, err, errlen)
		&& lgAreaGraph(lg, r, err, errlen)
		&& lgBuildNav(lg, err, errlen)
		&& lgBuildSpawns(lg, r, err, errlen)
		&& (lgPlaceObstacles(lg), lgPlaceCover(lg, r), 1)
		&& modLevelGenValidate(lg, err, errlen);
}

/* ------------------------------------------------------------------ */
/* Claude Missions: Pier 17 (2026-11-16)                                */
/* ------------------------------------------------------------------ */

/* A designed level, the same every time (the user: "Design a level, not
 * random, like the villa, streets, and datadyne investigation ... focused on
 * fun snappy gameplay"). A smugglers' container terminal at night, laid out
 * by hand on a 60 x 60 grid with the villa's pieces (rooms, passages,
 * windows, blocks, platforms, ramps, water):
 *
 *   north    the warehouse (a hall with two loading doors onto the yard, a
 *            back door and a side door; an office and a control room off it,
 *            the control room's window over the yard) and the customs house
 *            (two rooms), an alley between them and one behind
 *   middle   the container yard: stacks one and two containers high set so
 *            that no lane runs straight for long; two containers stand open
 *            at both ends (tunnels); two stacks are climbed by ramps (the
 *            perches: the yard's high ground, railed)
 *   south    the quay with the harbour master's hut (the start), a landing
 *            pad with the smugglers' jumpship on it, two piers and the water
 *
 * Three ways north from the quay (the west lane past the tunnel, the middle
 * past the perch, the east past the second perch), each crossing the others,
 * and three ways into the warehouse; the far end is the customs house or the
 * office, whichever the generator's distances make the extraction.
 */
#define LG_PIER_GRID 60
#define LG_PR_F   LG_VL_FLOOR
#define LG_PR_H1  256   /* one container (its texture's height) */
#define LG_PR_H2  512   /* two */
#define LG_PR_BOX_THEME   9    /* a container: its top, its sides */
#define LG_PR_PERCH_THEME 10   /* a perch's deck and the landing pad */

static int lgPlacePier(LgLayout *lg)
{
	static const int16_t stacks[][5] = {
		/* x, z, w, d, containers high */
		{ 3, 23, 6, 2, 2 }, { 3, 25, 6, 2, 1 },
		{ 12, 24, 2, 6, 1 },
		{ 22, 23, 6, 2, 1 }, { 22, 25, 6, 2, 2 },
		{ 30, 24, 2, 6, 1 },
		{ 37, 24, 2, 6, 1 },
		{ 44, 24, 6, 2, 2 }, { 44, 26, 6, 2, 1 },
		{ 54, 22, 2, 6, 2 },
		{ 16, 35, 2, 6, 2 }, { 18, 35, 2, 6, 1 },
		{ 3, 35, 6, 2, 1 }, { 3, 37, 6, 2, 2 }, { 3, 39, 6, 2, 2 },
		{ 24, 38, 6, 2, 2 },
		{ 36, 37, 2, 6, 1 },
		{ 41, 35, 2, 6, 2 },
		{ 12, 41, 6, 2, 1 },
		{ 46, 40, 6, 2, 2 },
		{ 20, 30, 2, 3, 1 },
		{ 38, 30, 6, 2, 1 },
	};
	const int F = LG_PR_F, G = LG_PIER_GRID;
	const int top = F + LG_PR_H2 + 240 + LG_ST_OPENTOP;
	int i;

	memset(&g_LgVl, 0, sizeof(g_LgVl));
	g_LgVl.G = G;
	memset(g_LgVp.occ, 0, sizeof(g_LgVp.occ));
	memset(g_LgVp.door, 0, sizeof(g_LgVp.door));
	g_LgVp.np = 0;

#define ROOM(x, z, w, d, type, n) lgVlAdd(x, z, w, d, 0, type, n, F, F, 0, 0)
#define PASS(x, z, w, d)          lgVlAdd(x, z, w, d, 1, 0, -1, F, F, 0, 0)
#define WIN(x, z, w, d)           lgVlAdd(x, z, w, d, 2, 0, -1, F + LG_WIN_SILL, F + LG_WIN_HEAD, 0, 0)
#define SOLID(x, z, w, d)         lgVlMark(g_LgVp.occ, x, z, w, d, G, 1)

	/* 0: the harbour master's hut on the quay (the start) */
	SOLID(2, 43, 8, 6);
	ROOM(3, 44, 6, 4, LG_ROOM_OFFICE, 0);
	PASS(9, 45, 1, 2);            /* its door, east onto the quay */
	WIN(4, 43, 3, 1);             /* north, up the west lane */

	/* the warehouse: 1 the hall, 2 the office, 3 the control room */
	SOLID(5, 4, 31, 17);
	ROOM(6, 5, 19, 15, LG_ROOM_HALL, 1);
	ROOM(27, 5, 8, 6, LG_ROOM_OFFICE, 2);
	ROOM(27, 13, 8, 7, LG_ROOM_LAB, 3);
	PASS(9, 20, 3, 1);            /* the loading doors */
	PASS(18, 20, 3, 1);
	PASS(5, 9, 1, 2);             /* the side door, west */
	PASS(14, 4, 2, 1);            /* the back door, north */
	PASS(25, 7, 2, 2);            /* hall to office */
	PASS(25, 15, 2, 2);           /* hall to control room */
	PASS(30, 11, 2, 2);           /* office to control room */
	PASS(35, 7, 1, 2);            /* the office's door onto the alley */
	WIN(29, 20, 3, 1);            /* the control room's, over the yard */
	WIN(29, 4, 3, 1);
	WIN(5, 14, 1, 3);

	/* the customs house: 4 the front office, 5 the back room */
	SOLID(42, 5, 13, 16);
	ROOM(43, 14, 11, 6, LG_ROOM_OFFICE, 4);
	ROOM(43, 6, 11, 6, LG_ROOM_STORAGE, 5);
	PASS(47, 12, 2, 2);
	PASS(48, 20, 2, 1);           /* its front door, onto the yard */
	PASS(42, 8, 1, 2);            /* the back room's, onto the alley */
	WIN(42, 15, 1, 3);
	WIN(54, 15, 1, 3);
	WIN(46, 5, 3, 1);
	g_LgVp.nrooms = 6;

	/* two containers open at both ends */
	SOLID(8, 30, 6, 4);
	PASS(8, 31, 6, 2);
	SOLID(46, 29, 4, 6);
	PASS(47, 29, 2, 6);

	/* the stacks */
	for (i = 0; i < (int)(sizeof(stacks) / sizeof(stacks[0])); i++) {
		const int h = F + (stacks[i][4] == 2 ? LG_PR_H2 : LG_PR_H1);

		lgVlAdd(stacks[i][0], stacks[i][1], stacks[i][2], stacks[i][3], 6, 0, -1, h, h, 0, LG_PR_BOX_THEME);
	}

	/* the perches: a deck on two containers, a ramp up to it */
	lgVlAdd(24, 34, 6, 4, 3, 0, -1, F + LG_PR_H1, F + LG_PR_H1, 0, LG_PR_PERCH_THEME);
	lgVlAdd(30, 35, 6, 2, 3, 0, -1, F, F + LG_PR_H1, LG_RAMP_XN, LG_PR_PERCH_THEME);
	lgVlAdd(53, 29, 5, 4, 3, 0, -1, F + LG_PR_H1, F + LG_PR_H1, 0, LG_PR_PERCH_THEME);
	lgVlAdd(53, 33, 2, 6, 3, 0, -1, F, F + LG_PR_H1, LG_RAMP_ZN, LG_PR_PERCH_THEME);

	/* the landing pad on the quay, the piers, the water */
	lgVlAdd(24, 44, 14, 6, 6, 0, -1, F + 16, F + 16, 0, LG_PR_PERCH_THEME);   /* (a plinth: the ship stands on it, nobody else) */
	lgVlAdd(12, 50, 3, 6, 3, 0, -1, F, F, 0, LG_DOCK_THEME);
	lgVlAdd(46, 50, 3, 5, 3, 0, -1, F, F, 0, LG_DOCK_THEME);
	if (!lgVlFill(1, 50, G - 1, G - 1, 4, 0, 0)) {
		return 0;
	}
	/* the ground: everything else */
	if (!lgVlFill(1, 1, G - 1, 50, 5, F, 0)) {
		return 0;
	}
#undef ROOM
#undef PASS
#undef WIN
#undef SOLID

	if (!lgVlPiecesToAreas(lg, top)) {
		return 0;
	}
	/* the hall: a warehouse's height */
	for (i = 0; i < lg->numareas; i++) {
		if (lg->areas[i].kind == LG_AREA_ROOM && lg->areas[i].room == 1 && !lg->areas[i].open) {
			lg->areas[i].ceil = (int16_t)(F + 480);
		}
	}
	return 1;
}

static int lgTryPier(LgLayout *lg, const LgParams *p, LgRng *r, char *err, int errlen)
{
	int i;

	(void)p;

	if (!lgPlacePier(lg)) {
		lgSetErr(err, errlen, "pier 17 did not fit (%d areas)", lg->numareas, 0);
		return 0;
	}
	if (!lgFindPortals(lg, r, err, errlen)) {
		return 0;
	}
	lgVlWaterPortals(lg);
	if (!lgAreaGraph(lg, r, err, errlen) || !lgBuildNav(lg, err, errlen) || !lgBuildSpawns(lg, r, err, errlen)) {
		return 0;
	}
	lgPlaceObstacles(lg);
	/* the jumpship on its pad, and the yard's dressing set by hand (cell x, z,
	 * the prop, its turn): barrels and crates where a lane wants cover or a
	 * corner wants something to look at; then the generator's own cover */
	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];

		if (a->theme == LG_PR_PERCH_THEME && a->special == LG_SPECIAL_RACK) {
			lgTryProp(lg, i, LG_PROP_JUMPSHIP, lgWorldX(lg, a->x) + a->w * LG_CELL / 2, lgWorldZ(lg, a->z) + a->d * LG_CELL / 2, 1, 20, 0, 0);
		}
	}
	{
		static const int16_t dress[][4] = {
			/* the quay */
			{ 16, 46, LG_PROP_BARREL, 0 }, { 17, 46, LG_PROP_BARREL, 0 }, { 16, 47, LG_PROP_BARREL, 0 }, { 20, 48, LG_PROP_DUMPSTER, 0 },
			{ 40, 46, LG_PROP_HOVERCRATE, 0 }, { 41, 47, LG_PROP_HOVERCRATE, 1 }, { 44, 45, LG_PROP_BORGCRATE, 0 }, { 51, 47, LG_PROP_BARREL, 0 }, { 52, 47, LG_PROP_BARREL, 0 },
			/* the yard's cross lane and the doors */
			{ 27, 31, LG_PROP_BORGCRATE, 0 }, { 28, 31, LG_PROP_BARREL, 0 }, { 33, 33, LG_PROP_BARREL, 0 }, { 34, 33, LG_PROP_BARREL, 0 }, { 15, 33, LG_PROP_HOVERCRATE, 0 },
			{ 43, 33, LG_PROP_HOVERCRATE, 1 }, { 14, 22, LG_PROP_BORGCRATE, 0 }, { 15, 22, LG_PROP_BARREL, 0 }, { 26, 21, LG_PROP_TROLLEY, 1 }, { 40, 22, LG_PROP_HOVBIKE, 1 },
			{ 9, 28, LG_PROP_BARREL, 0 }, { 33, 41, LG_PROP_BORGCRATE, 0 }, { 50, 37, LG_PROP_BARREL, 0 }, { 51, 37, LG_PROP_BARREL, 0 },
			/* the alleys */
			{ 38, 10, LG_PROP_DUMPSTER, 1 }, { 40, 14, LG_PROP_BARREL, 0 }, { 40, 15, LG_PROP_BARREL, 0 }, { 37, 18, LG_PROP_BORGCRATE, 0 },
			{ 20, 2, LG_PROP_DUMPSTER, 0 }, { 10, 2, LG_PROP_BORGCRATE, 0 }, { 30, 2, LG_PROP_HOVERCRATE, 0 }, { 45, 2, LG_PROP_BARREL, 0 }, { 46, 2, LG_PROP_BARREL, 0 },
			{ 2, 12, LG_PROP_BARREL, 0 }, { 3, 17, LG_PROP_BORGCRATE, 0 }, { 56, 10, LG_PROP_BORGCRATE, 0 }, { 57, 17, LG_PROP_BARREL, 0 },
			/* the perches */
			{ 26, 35, LG_PROP_BORGCRATE, 0 }, { 55, 30, LG_PROP_BARREL, 0 }, { 56, 30, LG_PROP_BARREL, 0 },
		};
		int k;

		for (k = 0; k < (int)(sizeof(dress) / sizeof(dress[0])); k++) {
			const int ai = lg->cells[dress[k][1]][dress[k][0]] - 1;

			if (ai >= 0 && lg->areas[ai].open && !LG_NOWALK(&lg->areas[ai])) {
				lgTryProp(lg, ai, dress[k][2], lgWorldX(lg, dress[k][0]) + LG_CELL / 2, lgWorldZ(lg, dress[k][1]) + LG_CELL / 2, dress[k][3], 6, 45, 62);
			}
		}
	}
	lgPlaceCover(lg, r);
	return modLevelGenValidate(lg, err, errlen);
}

/* ------------------------------------------------------------------ */
/* Claude Missions: Hollow Creek (2026-11-17)                           */
/* ------------------------------------------------------------------ */

/* The second designed level (the user: "starts in a suburban area with a
 * couple enterable houses, then a forest trail that leads to a small military
 * base and then ruins ... fun with short sightlines besides the forest
 * path"). On a 96 x 96 grid, x east, z south:
 *
 *   south-west  Hollow Creek Road: two houses you can go into (the start is
 *               the first one's living room), a road, back gardens with
 *               hedges and sheds across it
 *   north-west  the woods: the trail winds north from the back gardens
 *               through a clearing, then east to the base; a second path
 *               runs from the clearing to the base's side gate. The trees
 *               either side are a wall; lone trees and fallen logs on it
 *   north-east  the base: a command bunker, barracks, an armoury, a
 *               watchtower over the gate that covers the long last stretch of
 *               trail, a sandbagged gun emplacement, hesco walls
 *   south-east  the ruins, through the base's back wall (a gate and a
 *               breach): a colonnade, a roofless hall, broken walls, an
 *               altar on a mound, and the crypt (the far end)
 *
 * The trail is the only place a long view opens: everywhere else walls,
 * hedges and buildings break it within a few cells.
 */
#define LG_TRAIL_GRID 96
#define LG_TR_TREE   640   /* a pine's height over the ground */
/* the themes (the aesthetic's surfaces) */
#define LG_TR_LAWN   5
#define LG_TR_FOREST 6
#define LG_TR_ROAD   7
#define LG_TR_BASE   8
#define LG_TR_TREES  9     /* a lone tree or a log: its crown or bark on top */
#define LG_TR_HEDGE  10
#define LG_TR_RUINS  11

static uint8_t g_LgTrOpen[LG_TRAIL_GRID][LG_TRAIL_GRID];   /* the woods: where they are cleared */

/* a stretch of trail: x, z, w, d, in chunks of 4 along it, each nudged a cell
 * one way or the other so its sides are ragged */
static void lgTrTrail(int x, int z, int w, int d)
{
	static const int8_t nudge[8] = { 0, 1, 1, 0, -1, 0, 1, -1 };
	const int alongx = w >= d;
	const int len = alongx ? w : d;
	int i, a, b;

	for (i = 0; i < len; i += 4) {
		const int k = (x * 7 + z * 13 + i / 4) & 7;
		const int off = (i == 0 || i + 4 >= len) ? 0 : nudge[k];

		for (a = i; a < i + 4 && a < len; a++) {
			for (b = 0; b < (alongx ? d : w); b++) {
				const int cx = alongx ? x + a : x + b + off, cz = alongx ? z + b + off : z + a;

				if (cx >= 1 && cz >= 1 && cx < g_LgVl.G - 1 && cz < g_LgVl.G - 1) {
					g_LgTrOpen[cz][cx] = 1;
				}
			}
		}
	}
}

static int lgPlaceTrail(LgLayout *lg)
{
	const int F = LG_PR_F, G = LG_TRAIL_GRID;
	const int top = F + LG_TR_TREE + 260 + LG_ST_OPENTOP;
	const int HEDGE = F + 200, LOG = F + 70, HESCO = F + 120, WALL = F + 300;
	int i, x, z;

	memset(&g_LgVl, 0, sizeof(g_LgVl));
	g_LgVl.G = G;
	memset(g_LgVp.occ, 0, sizeof(g_LgVp.occ));
	memset(g_LgVp.door, 0, sizeof(g_LgVp.door));
	memset(g_LgTrOpen, 0, sizeof(g_LgTrOpen));
	g_LgVp.np = 0;

#define ROOM(x, z, w, d, type, n) lgVlAdd(x, z, w, d, 0, type, n, F, F, 0, 0)
#define PASS(x, z, w, d)          lgVlAdd(x, z, w, d, 1, 0, -1, F, F, 0, 0)
#define WIN(x, z, w, d)           lgVlAdd(x, z, w, d, 2, 0, -1, F + LG_WIN_SILL, F + LG_WIN_HEAD, 0, 0)
#define SOLID(x, z, w, d)         lgVlMark(g_LgVp.occ, x, z, w, d, G, 1)
#define BLOCK(x, z, w, d, h, th)  lgVlAdd(x, z, w, d, 6, 0, -1, h, h, 0, th)

	/* --- Hollow Creek Road --- */
	/* 0-2 the first house: living room (the start), kitchen, bedroom */
	SOLID(4, 64, 13, 11);
	ROOM(5, 65, 6, 9, LG_ROOM_OFFICE, 0);
	ROOM(13, 65, 3, 4, LG_ROOM_OFFICE, 1);
	ROOM(13, 71, 3, 3, LG_ROOM_OFFICE, 2);
	PASS(11, 66, 2, 2);
	PASS(11, 71, 2, 2);
	PASS(7, 74, 2, 1);            /* the front door, onto the road */
	PASS(9, 64, 2, 1);            /* the back door, the garden and the woods */
	WIN(16, 66, 1, 2);
	WIN(4, 67, 1, 3);
	WIN(13, 74, 2, 1);
	/* 3-4 the second house: lounge, garage */
	SOLID(22, 63, 14, 12);
	ROOM(23, 64, 6, 10, LG_ROOM_OFFICE, 3);
	ROOM(31, 64, 4, 10, LG_ROOM_HALL, 4);
	PASS(29, 67, 2, 2);
	PASS(25, 74, 2, 1);
	PASS(31, 74, 4, 1);           /* the garage door */
	PASS(27, 63, 2, 1);           /* the back door */
	WIN(22, 66, 1, 3);
	WIN(35, 68, 1, 2);
	/* the gardens across the road: hedges with gaps, two sheds */
	BLOCK(2, 83, 6, 1, HEDGE, LG_TR_HEDGE);
	BLOCK(10, 83, 4, 1, HEDGE, LG_TR_HEDGE);
	BLOCK(14, 84, 1, 6, HEDGE, LG_TR_HEDGE);
	BLOCK(4, 88, 4, 3, F + 260, 1);
	BLOCK(17, 83, 6, 1, HEDGE, LG_TR_HEDGE);
	BLOCK(26, 83, 5, 1, HEDGE, LG_TR_HEDGE);
	BLOCK(21, 87, 1, 5, HEDGE, LG_TR_HEDGE);
	BLOCK(22, 87, 5, 1, HEDGE, LG_TR_HEDGE);
	BLOCK(33, 83, 4, 1, HEDGE, LG_TR_HEDGE);
	BLOCK(40, 83, 6, 1, HEDGE, LG_TR_HEDGE);
	BLOCK(40, 84, 1, 5, HEDGE, LG_TR_HEDGE);
	BLOCK(35, 89, 5, 3, F + 260, 1);
	/* the front gardens: a hedge either side of each path */
	BLOCK(3, 75, 3, 1, HEDGE, LG_TR_HEDGE);
	BLOCK(11, 75, 5, 1, HEDGE, LG_TR_HEDGE);
	BLOCK(20, 75, 4, 1, HEDGE, LG_TR_HEDGE);
	/* the back gardens: a hedge between them */
	BLOCK(19, 63, 1, 8, HEDGE, LG_TR_HEDGE);

	/* --- the woods --- */
	lgTrTrail(37, 48, 5, 14);    /* from the back gardens, north */
	lgTrTrail(29, 44, 13, 4);    /* the bend, west */
	lgTrTrail(29, 30, 5, 14);    /* north */
	for (z = 22; z < 30; z++) {  /* the clearing */
		for (x = 24; x < 38; x++) {
			g_LgTrOpen[z][x] = 1;
		}
	}
	lgTrTrail(31, 11, 5, 11);    /* north again */
	lgTrTrail(31, 7, 25, 4);     /* east, the long stretch to the base's gate */
	lgTrTrail(38, 25, 18, 4);    /* the side path to the base's side gate */
	/* the end of each path at the base: level */
	for (z = 7; z < 11; z++) g_LgTrOpen[z][55] = 1;
	for (z = 25; z < 29; z++) g_LgTrOpen[z][55] = 1;
	for (x = 37; x < 42; x++) g_LgTrOpen[61][x] = 1;
	/* lone trees and fallen logs on the trail */
	BLOCK(27, 24, 2, 2, F + LG_TR_TREE, LG_TR_TREES);
	BLOCK(33, 26, 1, 1, F + LG_TR_TREE, LG_TR_TREES);
	BLOCK(30, 36, 1, 1, F + LG_TR_TREE, LG_TR_TREES);
	BLOCK(38, 54, 2, 1, LOG, LG_TR_TREES);
	BLOCK(33, 16, 1, 1, F + LG_TR_TREE, LG_TR_TREES);
	BLOCK(40, 8, 3, 1, LOG, LG_TR_TREES);
	BLOCK(47, 9, 1, 1, F + LG_TR_TREE, LG_TR_TREES);
	BLOCK(46, 26, 1, 1, F + LG_TR_TREE, LG_TR_TREES);
	BLOCK(50, 27, 3, 1, LOG, LG_TR_TREES);
	/* everything else in the woods: trees (nothing: the ground's edge is drawn as them) */
	for (z = 1; z < G - 1; z++) {
		for (x = 1; x < 56; x++) {
			if ((z < 62 || x >= 46) && !g_LgTrOpen[z][x]) {
				SOLID(x, z, 1, 1);
			}
		}
	}

	/* --- the base --- */
	/* 5-6 the command bunker and its radio room */
	SOLID(70, 4, 14, 10);
	ROOM(71, 5, 8, 8, LG_ROOM_LAB, 5);
	ROOM(81, 5, 2, 8, LG_ROOM_LAB, 6);
	PASS(79, 7, 2, 2);
	PASS(74, 13, 2, 1);
	PASS(83, 9, 1, 2);
	WIN(70, 7, 1, 2);
	WIN(77, 13, 1, 1);
	/* 7 the barracks */
	SOLID(60, 17, 12, 8);
	ROOM(61, 18, 10, 6, LG_ROOM_HALL, 7);
	PASS(60, 20, 1, 2);
	PASS(66, 24, 2, 1);
	WIN(65, 17, 2, 1);
	WIN(71, 19, 1, 2);
	/* 8 the armoury */
	SOLID(78, 19, 12, 9);
	ROOM(79, 20, 10, 7, LG_ROOM_HALL, 8);
	PASS(78, 22, 1, 2);
	PASS(83, 27, 2, 1);
	WIN(89, 22, 1, 2);
	/* the watchtower over the gate (it sees down the long stretch of trail) */
	lgVlAdd(58, 2, 4, 4, 3, 0, -1, F + 384, F + 384, 0, LG_TR_BASE);
	lgVlAdd(62, 3, 8, 2, 3, 0, -1, F, F + 384, LG_RAMP_XN, LG_TR_BASE);
	/* the gun emplacement */
	lgVlAdd(62, 33, 6, 4, 3, 0, -1, F + 256, F + 256, 0, LG_TR_BASE);
	lgVlAdd(68, 34, 6, 2, 3, 0, -1, F, F + 256, LG_RAMP_XN, LG_TR_BASE);
	/* hesco: low walls to crouch behind, a few tall ones to break the yard up */
	BLOCK(58, 12, 4, 1, HESCO, LG_TR_BASE);
	BLOCK(66, 29, 5, 1, HESCO, LG_TR_BASE);
	BLOCK(76, 31, 1, 5, HESCO, LG_TR_BASE);
	BLOCK(86, 12, 1, 4, HESCO, LG_TR_BASE);
	BLOCK(88, 36, 5, 1, HESCO, LG_TR_BASE);
	BLOCK(80, 40, 1, 4, HESCO, LG_TR_BASE);
	BLOCK(58, 30, 1, 4, HESCO, LG_TR_BASE);
	BLOCK(72, 39, 2, 2, WALL, LG_TR_BASE);
	BLOCK(84, 33, 3, 2, WALL, LG_TR_BASE);
	BLOCK(91, 16, 2, 3, WALL, LG_TR_BASE);
	/* the back wall, onto the ruins: a breach (60-62) and a gate (74-78) */
	BLOCK(56, 46, 4, 1, WALL, LG_TR_BASE);
	BLOCK(63, 46, 11, 1, WALL, LG_TR_BASE);
	BLOCK(79, 46, 16, 1, WALL, LG_TR_BASE);

	/* --- the ruins --- */
	/* 9 the crypt (the far end) */
	SOLID(80, 80, 12, 10);
	ROOM(81, 81, 10, 8, LG_ROOM_STORAGE, 9);
	PASS(85, 80, 2, 1);
	PASS(80, 84, 1, 2);
	g_LgVp.nrooms = 10;
	/* the colonnade */
	for (i = 0; i < 3; i++) {
		BLOCK(60 + 4 * i, 52, 1, 1, F + 480, LG_TR_RUINS);
		BLOCK(60 + 4 * i, 57, 1, 1, F + 480, LG_TR_RUINS);
	}
	/* the roofless hall */
	BLOCK(74, 51, 5, 1, F + 420, LG_TR_RUINS);
	BLOCK(81, 51, 5, 1, F + 300, LG_TR_RUINS);
	BLOCK(74, 52, 1, 8, F + 360, LG_TR_RUINS);
	BLOCK(85, 52, 1, 5, F + 420, LG_TR_RUINS);
	BLOCK(75, 59, 5, 1, F + 260, LG_TR_RUINS);
	/* the altar on its mound */
	lgVlAdd(62, 80, 6, 4, 3, 0, -1, F + 256, F + 256, 0, LG_TR_RUINS);
	lgVlAdd(68, 81, 6, 2, 3, 0, -1, F, F + 256, LG_RAMP_XN, LG_TR_RUINS);
	/* broken walls */
	BLOCK(58, 66, 6, 1, F + 300, LG_TR_RUINS);
	BLOCK(58, 67, 1, 4, F + 220, LG_TR_RUINS);
	BLOCK(66, 70, 1, 6, F + 340, LG_TR_RUINS);
	BLOCK(70, 64, 7, 1, F + 280, LG_TR_RUINS);
	BLOCK(84, 66, 1, 8, F + 360, LG_TR_RUINS);
	BLOCK(88, 72, 6, 1, F + 240, LG_TR_RUINS);
	BLOCK(76, 74, 1, 5, F + 300, LG_TR_RUINS);
	BLOCK(60, 90, 8, 1, F + 260, LG_TR_RUINS);
	BLOCK(72, 88, 1, 6, F + 320, LG_TR_RUINS);
	/* trees grown up through it */
	BLOCK(90, 56, 2, 2, F + LG_TR_TREE, LG_TR_TREES);
	BLOCK(68, 92, 2, 2, F + LG_TR_TREE, LG_TR_TREES);
	BLOCK(80, 70, 2, 2, F + LG_TR_TREE, LG_TR_TREES);

	/* the ground: the road's, the lawns', the trail's, the base's, the ruins' */
	if (!lgVlFill(1, 77, 46, 82, 5, F, LG_TR_ROAD) || !lgVlFill(1, 62, 46, 77, 5, F, LG_TR_LAWN) || !lgVlFill(1, 82, 46, G - 1, 5, F, LG_TR_LAWN)
			|| !lgVlFill(1, 1, 56, 62, 5, F, LG_TR_FOREST) || !lgVlFill(46, 62, 56, G - 1, 5, F, LG_TR_FOREST)
			|| !lgVlFill(56, 1, G - 1, 47, 5, F, LG_TR_BASE) || !lgVlFill(56, 47, G - 1, G - 1, 5, F, LG_TR_RUINS)) {
		return 0;
	}
#undef ROOM
#undef PASS
#undef WIN
#undef SOLID
#undef BLOCK

	return lgVlPiecesToAreas(lg, top);
}

static int lgTryTrail(LgLayout *lg, const LgParams *p, LgRng *r, char *err, int errlen)
{
	(void)p;

	if (!lgPlaceTrail(lg)) {
		lgSetErr(err, errlen, "hollow creek did not fit (%d areas)", lg->numareas, 0);
		return 0;
	}
	if (!lgFindPortals(lg, r, err, errlen)) {
		return 0;
	}
	lgVlWaterPortals(lg);   /* (no railings on the blocks' tops) */
	if (!lgAreaGraph(lg, r, err, errlen) || !lgBuildNav(lg, err, errlen) || !lgBuildSpawns(lg, r, err, errlen)) {
		return 0;
	}
	lgPlaceObstacles(lg);
	{
		/* dressing, by hand: cell x, z, the prop, its turn */
		static const int16_t dress[][4] = {
			/* the road: a car or two parked, bins */
			{ 10, 79, LG_PROP_HOVCAR, 1 }, { 30, 80, LG_PROP_HOVCAB, 1 }, { 32, 69, LG_PROP_HOVBIKE, 0 },
			{ 18, 76, LG_PROP_DUMPSTER, 0 }, { 42, 76, LG_PROP_BARREL, 0 },
			/* the base */
			{ 57, 9, LG_PROP_BORGCRATE, 0 }, { 66, 8, LG_PROP_HOVERCRATE, 1 }, { 67, 9, LG_PROP_BARREL, 0 },
			{ 75, 16, LG_PROP_BORGCRATE, 0 }, { 76, 16, LG_PROP_BORGCRATE, 0 }, { 85, 6, LG_PROP_BARREL, 0 }, { 86, 6, LG_PROP_BARREL, 0 },
			{ 88, 30, LG_PROP_HOVCAR, 0 }, { 79, 34, LG_PROP_HOVERCRATE, 0 }, { 70, 26, LG_PROP_TROLLEY, 1 }, { 59, 27, LG_PROP_BARREL, 0 },
			{ 66, 42, LG_PROP_BORGCRATE, 0 }, { 92, 42, LG_PROP_BARREL, 0 }, { 91, 4, LG_PROP_DISH, 0 },
			/* the ruins: fallen stone */
			{ 62, 72, LG_PROP_ROCK1, 0 }, { 79, 63, LG_PROP_ROCK2, 1 }, { 90, 92, LG_PROP_ROCK1, 2 }, { 70, 54, LG_PROP_ROCK2, 0 },
			/* the woods: a boulder in the clearing */
			{ 36, 28, LG_PROP_ROCK1, 1 },
		};
		int k;

		for (k = 0; k < (int)(sizeof(dress) / sizeof(dress[0])); k++) {
			const int ai = lg->cells[dress[k][1]][dress[k][0]] - 1;

			if (ai >= 0 && !LG_NOWALK(&lg->areas[ai]) && (lg->areas[ai].open || dress[k][2] == LG_PROP_HOVBIKE)) {
				lgTryProp(lg, ai, dress[k][2], lgWorldX(lg, dress[k][0]) + LG_CELL / 2, lgWorldZ(lg, dress[k][1]) + LG_CELL / 2, dress[k][3], 6, 45, 62);
			}
		}
	}
	lgPlaceCover(lg, r);
	return modLevelGenValidate(lg, err, errlen);
}

/* ------------------------------------------------------------------ */
/* Docks and Backwoods: the Claude Missions' levels, random (2026-11-17) */
/* ------------------------------------------------------------------ */

/* The user: "Make randomly generated versions of both these levels". Built
 * from the same pieces as Pier 17 and Hollow Creek, laid out by the seed:
 * every building a random size with its rooms split at random, things
 * scattered with a margin round each (so the ground between them is always
 * one piece and two cells wide at least), the trail a random walk; then the
 * whole of it mirrored, or not, east-west and north-south. */
static const uint8_t g_LgDocksGrid[LG_SIZE_COUNT] = { 48, 60, 72 };
static const uint8_t g_LgWoodsGrid[LG_SIZE_COUNT] = { 72, 84, 96 };

/* somewhere for a w x d thing in x0..x1, z0..z1 (exclusive) with margin
 * cells free all round it, nobody's doorstep */
static int lgRdSpot(LgRng *r, int x0, int z0, int x1, int z1, int w, int d, int margin, int *ox, int *oz)
{
	int t;

	if (x1 - x0 < w + 2 * margin || z1 - z0 < d + 2 * margin) {
		return 0;
	}
	for (t = 0; t < 80; t++) {
		const int x = lgRange(r, x0 + margin, x1 - margin - w), z = lgRange(r, z0 + margin, z1 - margin - d);

		if (lgVlFreeNoDoor(x - margin, z - margin, w + 2 * margin, d + 2 * margin, g_LgVl.G)) {
			*ox = x;
			*oz = z;
			return 1;
		}
	}
	return 0;
}

/* kept clear: x, z, w, d (clipped to the grid) */
static void lgRdKeep(int x, int z, int w, int d)
{
	lgVlMark(g_LgVp.door, x, z, w, d, g_LgVl.G, 1);
}

/* the ground outside an opening x, z, w, d on face f (0 north, 1 south, 2
 * west, 3 east), n deep: free? */
static int lgRdOutside(int x, int z, int w, int d, int f, int n)
{
	const int ox = f == 2 ? x - n : f == 3 ? x + w : x, oz = f == 0 ? z - n : f == 1 ? z + d : z;
	const int ow = f >= 2 ? n : w, od = f < 2 ? n : d;

	return lgVlFree((const uint8_t (*)[LG_GRIDMAX])g_LgVp.occ, ox, oz, ow, od, g_LgVl.G);
}

static void lgRdKeepOutside(int x, int z, int w, int d, int f, int n)
{
	const int ox = f == 2 ? x - n : f == 3 ? x + w : x - 1, oz = f == 0 ? z - n : f == 1 ? z + d : z - 1;
	const int ow = f >= 2 ? n : w + 2, od = f < 2 ? n : d + 2;

	lgRdKeep(ox, oz, ow, od);
}

/* A building at x, z, w x d (its walls the outer ring): a main room, and
 * (if wide enough) a column of one or two side rooms down one side, two-cell
 * walls between; doors from the main room out of the faces in faces (a bit
 * each, 1 north 2 south 4 west 8 east; one at least), side rooms' windows
 * out of their side. Rooms numbered from *room. Returns 0 if it could not
 * put a door anywhere. */
static int lgRdBuilding(LgRng *r, int x, int z, int w, int d, int maintype, int sidetype, int faces, int *room)
{
	const int F = LG_PR_F;
	const int ix = x + 1, iz = z + 1, iw = w - 2, id = d - 2;
	int side = -1, sw = 0, mw = iw, mx = ix, sx = 0, f, nd = 0, k, pass;

	lgVlMark(g_LgVp.occ, x, z, w, d, g_LgVl.G, 1);
	if (iw >= 9 && lgRange(r, 0, 99) < 85) {
		side = lgRange(r, 0, 1);
		sw = lgRange(r, 3, iw - 6 < 5 ? iw - 6 : 5);
		mw = iw - 2 - sw;
		mx = side ? ix : ix + sw + 2;
		sx = side ? ix + mw + 2 : ix;
	}
	lgVlAdd(mx, iz, mw, id, 0, maintype, (*room)++, F, F, 0, 0);
	if (side >= 0) {
		const int two = id >= 8 && lgRange(r, 0, 1);
		const int h1 = two ? lgRange(r, 3, id - 5) : id;

		for (k = 0; k < (two ? 2 : 1); k++) {
			const int rz = k ? iz + h1 + 2 : iz, rh = k ? id - h1 - 2 : h1;
			const int wx = side ? x + w - 1 : x;

			lgVlAdd(sx, rz, sw, rh, 0, sidetype, (*room)++, F, F, 0, 0);
			lgVlAdd(side ? ix + mw : ix + sw, lgRange(r, rz, rz + rh - 2), 2, 2, 1, 0, -1, F, F, 0, 0);
			if (rh >= 2 && lgRange(r, 0, 99) < 70) {
				const int wz = lgRange(r, rz, rz + rh - 2);

				if (lgRdOutside(wx, wz, 1, 2, side ? 3 : 2, 2)) {
					lgVlAdd(wx, wz, 1, 2, 2, 0, -1, F + LG_WIN_SILL, F + LG_WIN_HEAD, 0, 0);
					lgRdKeepOutside(wx, wz, 1, 2, side ? 3 : 2, 2);
				}
			}
		}
	}
	/* the doors: the faces asked for, then (if none would go) any */
	for (pass = 0; pass < 2 && nd == 0; pass++) {
		for (f = 0; f < 4; f++) {
			int dx, dz, dw, dd;

			if ((pass == 0 && !(faces & (1 << f))) || (f == 2 && side == 0) || (f == 3 && side == 1)) {
				continue;
			}
			if (f < 2) {
				dx = lgRange(r, mx, mx + mw - 2); dz = f == 0 ? z : z + d - 1; dw = 2; dd = 1;
			} else {
				dx = f == 2 ? x : x + w - 1; dz = lgRange(r, iz, iz + id - 2); dw = 1; dd = 2;
			}
			if (!lgRdOutside(dx, dz, dw, dd, f, 3)) {
				continue;
			}
			lgVlAdd(dx, dz, dw, dd, 1, 0, -1, F, F, 0, 0);
			lgRdKeepOutside(dx, dz, dw, dd, f, 3);
			nd++;
		}
	}
	/* a window on the main room, north or south, where there is no door */
	if (mw >= 4 && lgRange(r, 0, 99) < 75) {
		const int f = lgRange(r, 0, 1), wx = lgRange(r, mx + 1, mx + mw - 3), wz = f == 0 ? z : z + d - 1;
		int clear = 1, q;

		for (q = 0; q < g_LgVp.np && clear; q++) {
			const LgVlPiece *pc = &g_LgVp.pc[q];

			clear = !(pc->kind == 1 && pc->rc.z == wz && pc->rc.x < wx + 3 && wx - 1 < pc->rc.x + pc->rc.w);
		}
		if (clear && lgRdOutside(wx, wz, 2, 1, f, 2)) {
			lgVlAdd(wx, wz, 2, 1, 2, 0, -1, F + LG_WIN_SILL, F + LG_WIN_HEAD, 0, 0);
			lgRdKeepOutside(wx, wz, 2, 1, f, 2);
		}
	}
	return nd > 0;
}

/* a deck h up with a ramp up to it, together 12 x 4 (the ramp east of it,
 * rising west) */
static void lgRdPerch(int x, int z, int h, int theme)
{
	lgVlAdd(x, z, 6, 4, 3, 0, -1, LG_PR_F + h, LG_PR_F + h, 0, theme);
	lgVlAdd(x + 6, z + 1, 6, 2, 3, 0, -1, LG_PR_F, LG_PR_F + h, LG_RAMP_XN, theme);
	lgRdKeep(x + 12, z, 3, 4);   /* (the ramp's foot) */
}

/* the whole layout mirrored east-west (mx) and north-south (mz) */
static void lgRdMirror(int mx, int mz)
{
	const int G = g_LgVl.G;
	int i;

	for (i = 0; i < g_LgVp.np; i++) {
		LgVlPiece *q = &g_LgVp.pc[i];

		if (mx) {
			q->rc.x = (int16_t)(G - q->rc.x - q->rc.w);
			q->ramp = q->ramp == LG_RAMP_XP ? LG_RAMP_XN : q->ramp == LG_RAMP_XN ? LG_RAMP_XP : q->ramp;
		}
		if (mz) {
			q->rc.z = (int16_t)(G - q->rc.z - q->rc.d);
			q->ramp = q->ramp == LG_RAMP_ZP ? LG_RAMP_ZN : q->ramp == LG_RAMP_ZN ? LG_RAMP_ZP : q->ramp;
		}
	}
}

static void lgRdReset(int G)
{
	memset(&g_LgVl, 0, sizeof(g_LgVl));
	g_LgVl.G = G;
	memset(g_LgVp.occ, 0, sizeof(g_LgVp.occ));
	memset(g_LgVp.door, 0, sizeof(g_LgVp.door));
	memset(g_LgTrOpen, 0, sizeof(g_LgTrOpen));
	g_LgVp.np = 0;
}

/* props from pool scattered over the open ground of a theme (-1: any), n of them */
static void lgRdDress(LgLayout *lg, LgRng *r, const int8_t *pool, int npool, int theme, int n)
{
	int t;

	for (t = 0; t < n * 12 && n > 0; t++) {
		const int ai = lgRange(r, 0, lg->numareas - 1);
		const LgArea *a = &lg->areas[ai];
		const int prop = pool[lgRange(r, 0, npool - 1)];
		const LgPropDef *d = &g_LgProps[prop];
		const int turn = a->w >= a->d ? 1 : 0;
		const int hx = (turn & 1) ? d->halfl : d->halfw, hz = (turn & 1) ? d->halfw : d->halfl;

		if (!a->open || a->layer || a->ramp || LG_NOWALK(a) || a->special || ai == lg->startarea
				|| (theme >= 0 && a->theme != theme) || a->w * LG_CELL < 2 * hx + 100 || a->d * LG_CELL < 2 * hz + 100) {
			continue;
		}
		if (lgTryProp(lg, ai, prop, lgWorldX(lg, a->x) + lgRange(r, hx + 50, a->w * LG_CELL - hx - 50),
				lgWorldZ(lg, a->z) + lgRange(r, hz + 50, a->d * LG_CELL - hz - 50), turn, 30, 60, 62)) {
			n--;
		}
	}
}

/* --- Docks --- */
static int lgPlaceDocks(LgLayout *lg, LgRng *r)
{
	const int F = LG_PR_F, G = lg->gw, size = lg->size;
	const int top = F + LG_PR_H2 + 240 + LG_ST_OPENTOP;
	const int wd = lgRange(r, 5, 7), zq = G - 1 - wd;
	const int bd = lgRange(r, 10, 13), nb = size == LG_SIZE_LARGE ? 3 : 2;
	const int yz0 = 3 + bd, yz1 = zq - 8;
	int room = 0, i, k, x, z;

	lgRdReset(G);

	/* the harbour master's hut on the quay: the start (room 0) */
	{
		const int hx = lgRange(r, 3, G / 2 - 10);

		if (!lgRdBuilding(r, hx, zq - 7, 8, 6, LG_ROOM_OFFICE, LG_ROOM_OFFICE, (lgRange(r, 0, 1) ? 4 : 8) | 1, &room)) {
			return 0;
		}
	}
	/* the sheds along the north: a hall, side offices; alleys between and behind */
	{
		const int alley = 3, avail = G - 4 - (nb - 1) * alley;

		x = 2;
		for (i = 0; i < nb; i++) {
			const int w = i == nb - 1 ? G - 2 - x : lgRange(r, avail / nb - 2, avail / nb + 2);
			static const uint8_t sides[3] = { LG_ROOM_OFFICE, LG_ROOM_LAB, LG_ROOM_STORAGE };

			if (w < 9 || !lgRdBuilding(r, x, 3, w, bd, LG_ROOM_HALL, sides[lgRange(r, 0, 2)], 2 | (lgRange(r, 0, 1) ? 1 : 0) | (lgRange(r, 0, 1) ? 12 : 0), &room)) {
				return 0;
			}
			x += w + alley;
		}
	}
	/* the piers */
	k = lgRange(r, 1, size + 1);
	for (i = 0; i < k * 4 && k > 0; i++) {
		const int px = lgRange(r, 3, G - 6);

		if (lgVlFree((const uint8_t (*)[LG_GRIDMAX])g_LgVp.occ, px - 3, zq, 9, wd - 1, G)) {
			lgVlAdd(px, zq, 3, wd - 1, 3, 0, -1, F, F, 0, LG_DOCK_THEME);
			k--;
		}
	}
	/* the yard: perches, open containers, the stacks */
	for (i = lgRange(r, 1, 2); i > 0; i--) {
		if (lgRdSpot(r, 1, yz0, G - 1, yz1 + 4, 15, 4, 2, &x, &z)) {
			lgRdPerch(x, z, LG_PR_H1, LG_PR_PERCH_THEME);
		}
	}
	for (i = lgRange(r, 1, 2); i > 0; i--) {
		const int along = lgRange(r, 0, 1);

		if (lgRdSpot(r, 1, yz0, G - 1, yz1, along ? 6 : 4, along ? 4 : 6, 2, &x, &z)) {
			lgVlMark(g_LgVp.occ, x, z, along ? 6 : 4, along ? 4 : 6, G, 1);
			lgVlAdd(along ? x : x + 1, along ? z + 1 : z, along ? 6 : 2, along ? 2 : 6, 1, 0, -1, F, F, 0, 0);
			lgRdKeep(along ? x - 3 : x, along ? z : z - 3, along ? 12 : 4, along ? 4 : 12);
		}
	}
	for (i = 0, k = G * G / 100; i < 500 && k > 0; i++) {
		const int along = lgRange(r, 0, 1), dbl = lgRange(r, 0, 99) < 40;
		const int w = along ? 6 : (dbl ? 4 : 2), d = along ? (dbl ? 4 : 2) : 6;

		if (lgRdSpot(r, 1, yz0 - 1, G - 1, yz1 + 1, w, d, 2, &x, &z)) {
			const int h1 = F + (lgRange(r, 0, 99) < 45 ? LG_PR_H2 : LG_PR_H1), h2 = F + (lgRange(r, 0, 99) < 45 ? LG_PR_H2 : LG_PR_H1);

			if (!dbl) {
				lgVlAdd(x, z, w, d, 6, 0, -1, h1, h1, 0, LG_PR_BOX_THEME);
			} else if (along) {
				lgVlAdd(x, z, 6, 2, 6, 0, -1, h1, h1, 0, LG_PR_BOX_THEME);
				lgVlAdd(x, z + 2, 6, 2, 6, 0, -1, h2, h2, 0, LG_PR_BOX_THEME);
			} else {
				lgVlAdd(x, z, 2, 6, 6, 0, -1, h1, h1, 0, LG_PR_BOX_THEME);
				lgVlAdd(x + 2, z, 2, 6, 6, 0, -1, h2, h2, 0, LG_PR_BOX_THEME);
			}
			k--;
		}
	}
	/* the landing pad, if the quay has room */
	if (lgRdSpot(r, 1, zq - 8, G - 1, zq, 14, 6, 1, &x, &z)) {
		lgVlAdd(x, z, 14, 6, 6, 0, -1, F + 16, F + 16, 0, LG_PR_PERCH_THEME);
	}
	g_LgVp.nrooms = room;
	if (!lgVlFill(1, zq, G - 1, G - 1, 4, 0, 0) || !lgVlFill(1, 1, G - 1, zq, 5, F, 0)) {
		return 0;
	}
	lgRdMirror(lgRange(r, 0, 1), 0);
	if (!lgVlPiecesToAreas(lg, top)) {
		return 0;
	}
	for (i = 0; i < lg->numareas; i++) {
		if (lg->areas[i].kind == LG_AREA_ROOM && !lg->areas[i].open && lg->areas[i].roomtype == LG_ROOM_HALL) {
			lg->areas[i].ceil = (int16_t)(F + 480);
		}
	}
	return 1;
}

static int lgTryDocks(LgLayout *lg, const LgParams *p, LgRng *r, char *err, int errlen)
{
	static const int8_t yard[] = { LG_PROP_BARREL, LG_PROP_BARREL, LG_PROP_BORGCRATE, LG_PROP_HOVERCRATE, LG_PROP_DUMPSTER, LG_PROP_TROLLEY, LG_PROP_HOVBIKE };
	int i;

	(void)p;
	if (!lgPlaceDocks(lg, r)) {
		lgSetErr(err, errlen, "the docks did not fit (%d areas)", lg->numareas, 0);
		return 0;
	}
	if (!lgFindPortals(lg, r, err, errlen)) {
		return 0;
	}
	lgVlWaterPortals(lg);
	if (!lgAreaGraph(lg, r, err, errlen) || !lgBuildNav(lg, err, errlen) || !lgBuildSpawns(lg, r, err, errlen)) {
		return 0;
	}
	lgPlaceObstacles(lg);
	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];

		if (a->theme == LG_PR_PERCH_THEME && a->special == LG_SPECIAL_RACK) {
			lgTryProp(lg, i, LG_PROP_JUMPSHIP, lgWorldX(lg, a->x) + a->w * LG_CELL / 2, lgWorldZ(lg, a->z) + a->d * LG_CELL / 2, 1, 20, 0, 0);
		}
	}
	lgRdDress(lg, r, yard, (int)sizeof(yard), LG_STREET_THEME, lg->gw / 2);
	lgPlaceCover(lg, r);
	return modLevelGenValidate(lg, err, errlen);
}

/* --- Backwoods --- */
static int lgPlaceBackwoods(LgLayout *lg, LgRng *r)
{
	const int F = LG_PR_F, G = lg->gw;
	const int top = F + LG_TR_TREE + 260 + LG_ST_OPENTOP;
	const int sx = G * 46 / 96, sz = G * 62 / 96, fx = G * 56 / 96, bz = G * 46 / 96;
	const int rz = sz + (G - 1 - sz) * 45 / 100 + lgRange(r, -1, 1);
	const int tx = lgRange(r, sx - 11, sx - 7);
	int room = 0, i, k, x, z, gz, cx = -1, cz = -1, gz2 = -1;
	int corners[16][2], nc = 0;

	lgRdReset(G);

	/* --- the suburb: houses north of the road (the first's main room the
	 * start), gardens south of it --- */
	{
		const int hz = sz + 2, hd = rz - sz - 4;

		lgRdKeep(tx - 1, sz - 1, 7, rz - sz + 1);   /* (the way from the road to the trail) */
		x = 2;
		for (i = 0; i < 3 && x + 11 <= tx - 3; i++) {
			int w = lgRange(r, 11, 15);

			w = x + w > tx - 3 ? tx - 3 - x : w;
			if (hd < 6 || !lgRdBuilding(r, x, hz, w, hd, LG_ROOM_OFFICE, i == 1 ? LG_ROOM_HALL : LG_ROOM_OFFICE, 2 | 1 | (lgRange(r, 0, 1) ? 12 : 0), &room)) {
				return 0;
			}
			x += w + lgRange(r, 4, 6);
		}
		if (room == 0) {
			return 0;
		}
		for (i = G / 40; i > 0; i--) {   /* hedges between the back gardens */
			const int n = lgRange(r, 3, 6), along = lgRange(r, 0, 1);

			if (lgRdSpot(r, 1, sz, sx, rz, along ? n : 1, along ? 1 : n, 2, &x, &z)) {
				lgVlAdd(x, z, along ? n : 1, along ? 1 : n, 6, 0, -1, F + 200, F + 200, 0, LG_TR_HEDGE);
			}
		}
		for (i = G / 7; i > 0; i--) {    /* the gardens across the road */
			const int n = lgRange(r, 3, 7), along = lgRange(r, 0, 1);

			if (lgRdSpot(r, 1, rz + 5, sx, G - 1, along ? n : 1, along ? 1 : n, 2, &x, &z)) {
				lgVlAdd(x, z, along ? n : 1, along ? 1 : n, 6, 0, -1, F + 200, F + 200, 0, LG_TR_HEDGE);
			}
		}
		for (i = lgRange(r, 1, 2); i > 0; i--) {
			if (lgRdSpot(r, 1, rz + 5, sx, G - 1, 4, 3, 2, &x, &z)) {
				lgVlAdd(x, z, 4, 3, 6, 0, -1, F + 260, F + 260, 0, 1);
			}
		}
	}

	/* --- the woods: the trail a random walk north from the gardens, then
	 * east to the base's gate; a clearing at one of its corners; a side path
	 * from another corner to the base's side gate --- */
	gz = lgRange(r, 3, bz / 3);
	x = tx;
	z = sz;
	for (k = 0; k < 12 && z > gz; k++) {
		int nz = z - lgRange(r, 6, 12), nx;

		nz = nz < gz ? gz : nz;
		lgTrTrail(x, nz, 5, z - nz);
		z = nz;
		if (z <= gz + 4) {
			break;
		}
		nx = x + (lgRange(r, 0, 1) ? 1 : -1) * lgRange(r, 5, 12);
		nx = nx < 3 ? 3 : nx > fx - 12 ? fx - 12 : nx;
		lgTrTrail(x < nx ? x : nx, z, (x < nx ? nx - x : x - nx) + 5, 4);
		if (nc < 16) {
			corners[nc][0] = nx; corners[nc][1] = z; nc++;
		}
		x = nx;
	}
	if (z > gz) {
		lgTrTrail(x, gz, 5, z - gz);
	}
	lgTrTrail(x, gz, fx - x, 4);
	for (z = gz; z < gz + 4; z++) {
		g_LgTrOpen[z][fx - 1] = 1;
	}
	for (z = sz - 1, k = tx; k < tx + 5; k++) {
		g_LgTrOpen[z][k] = 1;
	}
	if (nc > 0) {   /* the clearing */
		const int c = lgRange(r, 0, nc - 1);
		int x0 = corners[c][0] - 3, z0 = corners[c][1] - 2;

		x0 = x0 < 2 ? 2 : x0 + 12 > fx - 2 ? fx - 14 : x0;
		z0 = z0 < 2 ? 2 : z0 + 8 > sz - 2 ? sz - 10 : z0;
		cx = x0; cz = z0;
		for (z = z0; z < z0 + 8; z++) {
			for (x = x0; x < x0 + 12; x++) {
				g_LgTrOpen[z][x] = 1;
			}
		}
	}
	for (i = 0; i < nc; i++) {      /* the side path, from a corner low enough */
		if (corners[i][1] > bz / 2 && corners[i][1] < bz - 6 && corners[i][1] > gz + 8) {
			gz2 = corners[i][1];
			lgTrTrail(corners[i][0], gz2, fx - corners[i][0], 4);
			for (z = gz2; z < gz2 + 4; z++) {
				g_LgTrOpen[z][fx - 1] = 1;
			}
			break;
		}
	}
	/* lone trees and fallen logs, where the trail is wide enough round them */
	for (i = 0, k = G / 6; i < 600 && k > 0; i++) {
		const int log = lgRange(r, 0, 99) < 35, along = lgRange(r, 0, 1);
		const int w = log && along ? 3 : 1, d = log && !along ? 3 : 1;
		int ok = 1, a, b;

		x = lgRange(r, 3, fx - 4);
		z = lgRange(r, 3, sz - 4);
		for (b = z - 2; b < z + d + 2 && ok; b++) {
			for (a = x - 2; a < x + w + 2 && ok; a++) {
				ok = a >= 1 && b >= 1 && a < fx && b < sz && g_LgTrOpen[b][a] && !g_LgVp.occ[b][a];
			}
		}
		if (ok) {
			lgVlAdd(x, z, w, d, 6, 0, -1, log ? F + 70 : F + LG_TR_TREE, log ? F + 70 : F + LG_TR_TREE, 0, LG_TR_TREES);
			k--;
		}
	}
	for (z = 1; z < G - 1; z++) {
		for (x = 1; x < fx; x++) {
			if ((z < sz || x >= sx) && !g_LgTrOpen[z][x]) {
				lgVlMark(g_LgVp.occ, x, z, 1, 1, G, 1);
			}
		}
	}
	(void)cx; (void)cz;

	/* --- the base: its gates kept clear, its back wall onto the ruins with
	 * a gate and a breach, three buildings, the watchtower by the gate, a gun
	 * emplacement, hesco --- */
	lgRdKeep(fx, gz - 1, 5, 6);
	if (gz2 >= 0) {
		lgRdKeep(fx, gz2 - 1, 5, 6);
	}
	{
		const int gate = lgRange(r, fx + 3, G - 9);
		int breach = lgRange(r, fx + 2, G - 6), t;

		for (t = 0; t < 20 && breach + 3 > gate - 3 && breach < gate + 8; t++) {
			breach = lgRange(r, fx + 2, G - 6);
		}
		for (x = fx; x < G - 1; ) {
			int e = x;

			while (e < G - 1 && !(e >= gate && e < gate + 5) && !(e >= breach && e < breach + 3)) {
				e++;
			}
			if (e > x) {
				lgVlAdd(x, bz, e - x, 1, 6, 0, -1, F + 300, F + 300, 0, LG_TR_BASE);
			}
			x = e;
			while (x < G - 1 && ((x >= gate && x < gate + 5) || (x >= breach && x < breach + 3))) {
				x++;
			}
		}
		lgRdKeep(gate - 1, bz - 3, 7, 7);
		lgRdKeep(breach - 1, bz - 3, 5, 7);
	}
	{
		static const uint8_t mains[3] = { LG_ROOM_LAB, LG_ROOM_HALL, LG_ROOM_HALL };

		for (i = 0; i < 3; i++) {
			const int w = lgRange(r, 10, 14), d = lgRange(r, 8, 10);

			if (lgRdSpot(r, fx, 1, G - 1, bz, w, d, 3, &x, &z)) {
				const int faces = (1 << lgRange(r, 0, 3)) | (1 << lgRange(r, 0, 3));

				if (!lgRdBuilding(r, x, z, w, d, mains[i], mains[i], faces, &room)) {
					return 0;
				}
			}
		}
	}
	if (lgRdSpot(r, fx, gz - 8 < 1 ? 1 : gz - 8, fx + 20 < G - 1 ? fx + 20 : G - 1, gz + 12 < bz ? gz + 12 : bz, 15, 4, 1, &x, &z)
			|| lgRdSpot(r, fx, 1, G - 1, bz, 15, 4, 1, &x, &z)) {
		lgVlAdd(x, z, 4, 4, 3, 0, -1, F + 384, F + 384, 0, LG_TR_BASE);
		lgVlAdd(x + 4, z + 1, 8, 2, 3, 0, -1, F, F + 384, LG_RAMP_XN, LG_TR_BASE);
		lgRdKeep(x + 12, z, 3, 4);
	}
	if (lgRdSpot(r, fx, 1, G - 1, bz, 15, 4, 2, &x, &z)) {
		lgRdPerch(x, z, 256, LG_TR_BASE);
	}
	for (i = G / 8; i > 0; i--) {
		const int n = lgRange(r, 3, 5), along = lgRange(r, 0, 1);

		if (lgRdSpot(r, fx, 1, G - 1, bz, along ? n : 1, along ? 1 : n, 2, &x, &z)) {
			lgVlAdd(x, z, along ? n : 1, along ? 1 : n, 6, 0, -1, F + 120, F + 120, 0, LG_TR_BASE);
		}
	}
	for (i = G / 24; i > 0; i--) {
		const int w = lgRange(r, 2, 3);

		if (lgRdSpot(r, fx, 1, G - 1, bz, w, 2, 2, &x, &z)) {
			lgVlAdd(x, z, w, 2, 6, 0, -1, F + 300, F + 300, 0, LG_TR_BASE);
		}
	}

	/* --- the ruins: the crypt (far from the base), a roofless hall, a
	 * colonnade, an altar on its mound, broken walls, trees --- */
	if (!lgRdSpot(r, fx, bz + (G - bz) / 2 - 2, G - 1, G - 1, 12, 10, 3, &x, &z) && !lgRdSpot(r, fx, bz + 1, G - 1, G - 1, 12, 10, 3, &x, &z)) {
		return 0;
	}
	if (!lgRdBuilding(r, x, z, 12, 10, LG_ROOM_STORAGE, LG_ROOM_STORAGE, (1 << lgRange(r, 0, 3)) | (1 << lgRange(r, 0, 3)), &room)) {
		return 0;
	}
	if (lgRdSpot(r, fx, bz + 1, G - 1, G - 1, 12, 9, 2, &x, &z)) {
		static const int8_t wl[5][4] = { { 0, 0, 5, 1 }, { 7, 0, 5, 1 }, { 0, 1, 1, 7 }, { 11, 1, 1, 4 }, { 1, 8, 5, 1 } };
		int q;

		for (q = 0; q < 5; q++) {
			const int h = F + lgRange(r, 240, 420);

			lgVlAdd(x + wl[q][0], z + wl[q][1], wl[q][2], wl[q][3], 6, 0, -1, h, h, 0, LG_TR_RUINS);
		}
	}
	if (lgRdSpot(r, fx, bz + 1, G - 1, G - 1, 9, 6, 2, &x, &z)) {
		for (i = 0; i < 3; i++) {
			lgVlAdd(x + 4 * i, z, 1, 1, 6, 0, -1, F + 480, F + 480, 0, LG_TR_RUINS);
			lgVlAdd(x + 4 * i, z + 5, 1, 1, 6, 0, -1, F + 480, F + 480, 0, LG_TR_RUINS);
		}
	}
	if (lgRdSpot(r, fx, bz + 1, G - 1, G - 1, 15, 4, 2, &x, &z)) {
		lgRdPerch(x, z, 256, LG_TR_RUINS);
	}
	for (i = G / 7; i > 0; i--) {
		const int n = lgRange(r, 4, 8), along = lgRange(r, 0, 1);

		if (lgRdSpot(r, fx, bz + 1, G - 1, G - 1, along ? n : 1, along ? 1 : n, 2, &x, &z)) {
			const int h = F + lgRange(r, 220, 420);

			lgVlAdd(x, z, along ? n : 1, along ? 1 : n, 6, 0, -1, h, h, 0, LG_TR_RUINS);
		}
	}
	for (i = G / 30; i > 0; i--) {
		if (lgRdSpot(r, fx, bz + 1, G - 1, G - 1, 2, 2, 2, &x, &z)) {
			lgVlAdd(x, z, 2, 2, 6, 0, -1, F + LG_TR_TREE, F + LG_TR_TREE, 0, LG_TR_TREES);
		}
	}
	g_LgVp.nrooms = room;

	if (!lgVlFill(1, rz, sx, rz + 5, 5, F, LG_TR_ROAD) || !lgVlFill(1, sz, sx, rz, 5, F, LG_TR_LAWN) || !lgVlFill(1, rz + 5, sx, G - 1, 5, F, LG_TR_LAWN)
			|| !lgVlFill(1, 1, fx, sz, 5, F, LG_TR_FOREST) || !lgVlFill(sx, sz, fx, G - 1, 5, F, LG_TR_FOREST)
			|| !lgVlFill(fx, 1, G - 1, bz + 1, 5, F, LG_TR_BASE) || !lgVlFill(fx, bz + 1, G - 1, G - 1, 5, F, LG_TR_RUINS)) {
		return 0;
	}
	lgRdMirror(lgRange(r, 0, 1), lgRange(r, 0, 1));
	return lgVlPiecesToAreas(lg, top);
}

static int lgTryBackwoods(LgLayout *lg, const LgParams *p, LgRng *r, char *err, int errlen)
{
	static const int8_t cars[] = { LG_PROP_HOVCAR, LG_PROP_HOVCAB, LG_PROP_TAXI, LG_PROP_POLICE };
	static const int8_t base[] = { LG_PROP_BARREL, LG_PROP_BARREL, LG_PROP_BORGCRATE, LG_PROP_HOVERCRATE, LG_PROP_TROLLEY, LG_PROP_HOVCAR };
	static const int8_t stone[] = { LG_PROP_ROCK1, LG_PROP_ROCK2 };
	static const int8_t garden[] = { LG_PROP_PLANTSPIDER, LG_PROP_PLANTRUBBER, LG_PROP_BARREL };

	(void)p;
	if (!lgPlaceBackwoods(lg, r)) {
		lgSetErr(err, errlen, "the backwoods did not fit (%d areas)", lg->numareas, 0);
		return 0;
	}
	if (!lgFindPortals(lg, r, err, errlen)) {
		return 0;
	}
	lgVlWaterPortals(lg);
	if (!lgAreaGraph(lg, r, err, errlen) || !lgBuildNav(lg, err, errlen) || !lgBuildSpawns(lg, r, err, errlen)) {
		return 0;
	}
	lgPlaceObstacles(lg);
	lgRdDress(lg, r, cars, (int)sizeof(cars), LG_TR_ROAD, lgRange(r, 1, 3));
	lgRdDress(lg, r, garden, (int)sizeof(garden), LG_TR_LAWN, lg->gw / 16);
	lgRdDress(lg, r, base, (int)sizeof(base), LG_TR_BASE, lg->gw / 5);
	lgRdDress(lg, r, stone, (int)sizeof(stone), LG_TR_RUINS, lg->gw / 16);
	lgRdDress(lg, r, stone, (int)sizeof(stone), LG_TR_FOREST, 2);
	lgPlaceCover(lg, r);
	return modLevelGenValidate(lg, err, errlen);
}

/* ------------------------------------------------------------------ */
/* Market Town (2026-11-18)                                             */
/* ------------------------------------------------------------------ */

/* The user: "This map should have a distinct landmark, and offshoot rooms
 * around it that all circle back to the large landmark. Sightlines should
 * be short." The landmark: a walled town's square, a tiered fountain in the
 * middle (a basin, a raised bowl, a tall column: it stands in the way across
 * the square) and four stone plinths round it. Round the square a ring of
 * shops and houses, each with a door on the square and one at the back onto
 * a lane; alleys three wide between them out to the lane. Round the lane, a
 * second ring of houses against the town wall, their gaps never in line
 * with the inner ring's (so no alley sees far), their doors on the lane;
 * courtyards in the town wall's corners. Every way round leads back to the
 * square: through a shop, along the lane, in by another alley. The whole
 * of it mirrored, or not, each way. */
static const uint8_t g_LgPlazaGrid[LG_SIZE_COUNT] = { 54, 64, 74 };
#define LG_PZ_PLAZA_THEME   8    /* the square: fan cobbles, rose stucco */
#define LG_PZ_STONE_THEME   9    /* the column and plinths: dressed limestone */
#define LG_PZ_FOUNT_THEME   10   /* the fountain's basin and bowl: water in wet stone */
#define LG_PZ_YARD_THEME    11   /* a courtyard: flagstones, timber-framed walls */

static struct {
	int buildings, alleys, doors;
} g_LgPz;

/* a row of buildings len long, depth deep, from x, z along x (or z), doors
 * out of faces; a 3-wide alley left after each (but not after the last if
 * the row ends there). Rows built along their length at random widths. */
static int lgPzRow(LgRng *r, int x, int z, int len, int depth, int alongx, int faces, int *room)
{
	static const uint8_t types[] = { LG_ROOM_OFFICE, LG_ROOM_OFFICE, LG_ROOM_LAB, LG_ROOM_STORAGE };
	int pos = lgRange(r, 0, 2), n = 0;

	while (pos + 6 <= len) {
		int w = lgRange(r, 6, 11);

		if (pos + w > len) {
			w = len - pos;
		}
		if (len - pos - w > 0 && len - pos - w < 9) {
			/* (too little left after it for an alley and another: run it to the end, or leave room for one more) */
			w = len - pos <= 13 ? len - pos : len - pos - 9;
		}
		if (w < 6) {
			break;
		}
		if (!lgRdBuilding(r, alongx ? x + pos : x, alongx ? z : z + pos, alongx ? w : depth, alongx ? depth : w,
				types[lgRange(r, 0, 3)], types[lgRange(r, 0, 3)], faces, room)) {
			return -1;
		}
		n++;
		g_LgPz.buildings++;
		pos += w + 3;
		g_LgPz.alleys++;
	}
	return n;
}

/* a square ring of blocks, side n, t thick, its top at h */
static void lgPzRing(int x, int z, int n, int t, int h, int theme)
{
	lgVlAdd(x, z, n, t, 6, 0, -1, h, h, 0, theme);
	lgVlAdd(x, z + n - t, n, t, 6, 0, -1, h, h, 0, theme);
	lgVlAdd(x, z + t, t, n - 2 * t, 6, 0, -1, h, h, 0, theme);
	lgVlAdd(x + n - t, z + t, t, n - 2 * t, 6, 0, -1, h, h, 0, theme);
}

static int lgPlacePlaza(LgLayout *lg, LgRng *r)
{
	const int F = LG_PR_F, G = lg->gw, size = lg->size;
	const int top = F + 420 + LG_ST_OPENTOP;
	const int Do = 6 + size, lane = 3, Di = 7 + size;
	const int a = 1 + Do + lane, p0 = a + Di, p1 = G - p0;
	const int c = G / 2;
	int room = 0, i, n;

	lgRdReset(G);
	memset(&g_LgPz, 0, sizeof(g_LgPz));

	/* the square's landmark: the fountain, its column, the plinths (kept
	 * three clear all round, so the square is one ring of ground) */
	/* (pieces may not overlap: the basin a ring round the bowl, the bowl a ring round the column) */
	lgPzRing(c - 4, c - 4, 8, 2, F + 30, LG_PZ_FOUNT_THEME);
	lgPzRing(c - 2, c - 2, 4, 1, F + 110, LG_PZ_FOUNT_THEME);
	lgVlAdd(c - 1, c - 1, 2, 2, 6, 0, -1, F + 420, F + 420, 0, LG_PZ_STONE_THEME);
	lgVlMark(g_LgVp.occ, c - 4, c - 4, 8, 8, G, 1);
	lgRdKeep(c - 7, c - 7, 14, 14);
	{
		const int q = (p1 - p0) / 4 + 1;
		static const int8_t sx[4] = { -1, 1, -1, 1 }, sz[4] = { -1, -1, 1, 1 };

		for (i = 0; i < 4; i++) {
			const int px = c + sx[i] * q - 1, pz = c + sz[i] * q - 1;

			if (lgVlFree((const uint8_t (*)[LG_GRIDMAX])g_LgVp.occ, px - 2, pz - 2, 6, 6, G)) {
				lgVlAdd(px, pz, 2, 2, 6, 0, -1, F + 260, F + 260, 0, LG_PZ_STONE_THEME);
				lgVlMark(g_LgVp.occ, px, pz, 2, 2, G, 1);
			}
		}
	}
	lgRdKeep(p0, p0, p1 - p0, 1); lgRdKeep(p0, p1 - 1, p1 - p0, 1);
	lgRdKeep(p0, p0, 1, p1 - p0); lgRdKeep(p1 - 1, p0, 1, p1 - p0);

	/* the inner ring: north and south rows the square's width and the
	 * ring's corners, west and east rows between them; doors on the square
	 * and the lane (1 north, 2 south, 4 west, 8 east) */
	if (lgPzRow(r, a, a, G - 2 * a, Di, 1, 1 | 2, &room) < 0
			|| lgPzRow(r, a, p1, G - 2 * a, Di, 1, 1 | 2, &room) < 0
			|| lgPzRow(r, a, p0 + 1, p1 - p0 - 2, Di, 0, 4 | 8, &room) < 0
			|| lgPzRow(r, p1, p0 + 1, p1 - p0 - 2, Di, 0, 4 | 8, &room) < 0) {
		return 0;
	}
	if (room < 6) {
		return 0;
	}
	/* the lane all round */
	lgRdKeep(1 + Do, 1 + Do, G - 2 - 2 * Do, lane);
	lgRdKeep(1 + Do, G - 1 - Do - lane, G - 2 - 2 * Do, lane);
	lgRdKeep(1 + Do, 1 + Do, lane, G - 2 - 2 * Do);
	lgRdKeep(G - 1 - Do - lane, 1 + Do, lane, G - 2 - 2 * Do);

	/* the outer ring against the town wall, the corners left for courtyards;
	 * doors onto the lane only */
	n = room;
	/* (three short of the corners either end: the courtyards' ways in) */
	if (lgPzRow(r, 4 + Do, 1, G - 8 - 2 * Do, Do, 1, 2, &room) < 0
			|| lgPzRow(r, 4 + Do, G - 1 - Do, G - 8 - 2 * Do, Do, 1, 1, &room) < 0
			|| lgPzRow(r, 1, 4 + Do, G - 8 - 2 * Do, Do, 0, 8, &room) < 0
			|| lgPzRow(r, G - 1 - Do, 4 + Do, G - 8 - 2 * Do, Do, 0, 4, &room) < 0) {
		return 0;
	}
	if (room - n < 4) {
		return 0;
	}
	g_LgVp.nrooms = room;

	/* the ground: the square, then the courtyards, then the lane and alleys */
	/* (each fill's ground marked taken, so the next leaves it be) */
	{
		const int fx0[5] = { p0, 1, G - 1 - Do, 1, G - 1 - Do }, fz0[5] = { p0, 1, 1, G - 1 - Do, G - 1 - Do };
		const int fx1[5] = { p1, 1 + Do, G - 1, 1 + Do, G - 1 }, fz1[5] = { p1, 1 + Do, 1 + Do, G - 1, G - 1 };

		for (i = 0; i < 5; i++) {
			if (!lgVlFill(fx0[i], fz0[i], fx1[i], fz1[i], 5, F, i ? LG_PZ_YARD_THEME : LG_PZ_PLAZA_THEME)) {
				return 0;
			}
			lgVlMark(g_LgVp.occ, fx0[i], fz0[i], fx1[i] - fx0[i], fz1[i] - fz0[i], G, 1);
		}
	}
	if (!lgVlFill(1, 1, G - 1, G - 1, 5, F, 0)) {
		return 0;
	}
	lgRdMirror(lgRange(r, 0, 1), lgRange(r, 0, 1));
	if (!lgVlPiecesToAreas(lg, top)) {
		return 0;
	}
	return 1;
}

static int lgTryPlaza(LgLayout *lg, const LgParams *p, LgRng *r, char *err, int errlen)
{
	/* the square's market: stalls (tables), barrels and crates of goods, chairs, plants */
	static const int8_t market[] = { LG_PROP_CITABLE, LG_PROP_A51TABLE, LG_PROP_BARREL, LG_PROP_BARREL, LG_PROP_VILLACHAIR, LG_PROP_PLANTSPIDER, LG_PROP_PLANTRUBBER, LG_PROP_HOVERCRATE };
	static const int8_t lanes[] = { LG_PROP_BARREL, LG_PROP_DUMPSTER, LG_PROP_TROLLEY, LG_PROP_BORGCRATE, LG_PROP_HOVBIKE };
	static const int8_t yards[] = { LG_PROP_PLANTRUBBER, LG_PROP_PLANTSPIDER, LG_PROP_VILLACHAIR, LG_PROP_CITABLE, LG_PROP_BARREL };

	(void)p;
	if (!lgPlacePlaza(lg, r)) {
		lgSetErr(err, errlen, "the market town did not fit (%d areas)", lg->numareas, 0);
		return 0;
	}
	if (!lgFindPortals(lg, r, err, errlen)) {
		return 0;
	}
	lgVlWaterPortals(lg);   /* (no railings on the fountain's and plinths' tops) */
	if (!lgAreaGraph(lg, r, err, errlen) || !lgBuildNav(lg, err, errlen) || !lgBuildSpawns(lg, r, err, errlen)) {
		return 0;
	}
	lgPlaceObstacles(lg);
	lgRdDress(lg, r, market, (int)sizeof(market), LG_PZ_PLAZA_THEME, lg->gw / 3);
	lgRdDress(lg, r, lanes, (int)sizeof(lanes), LG_STREET_THEME, lg->gw / 3);
	lgRdDress(lg, r, yards, (int)sizeof(yards), LG_PZ_YARD_THEME, 8);
	lgPlaceCover(lg, r);
	return modLevelGenValidate(lg, err, errlen);
}


/* ------------------------------------------------------------------ */
/* Themes                                                               */

/* The layouts: a name, how often the seed picks each at every size (0: never
 * at that size) and the aesthetics that suit it (bits by LG_AESTH_). Authored
 * data: a new layout is an entry here, a generator and a case in lgTry(). */
static const struct {
	const char *name;
	uint8_t weight[LG_SIZE_COUNT];
	uint32_t aesthetics;
} g_LgLayouts[LG_LAYOUT_ALL] = {
	{ "Hub", { 1, 1, 1 }, 1u << LG_AESTH_FACILITY },
	{ "Underground Lab", { 1, 1, 1 }, 1u << LG_AESTH_LABS },
	{ "Street", { 1, 1, 1 }, 1u << LG_AESTH_CITY },
	{ "Ravine", { 1, 1, 1 }, 1u << LG_AESTH_CANYON },   /* (2026-11-15: a desert canyon outpost) */
	{ "Rooftops", { 1, 1, 1 }, 1u << LG_AESTH_ROOFTOP },   /* (2026-11-16: took the warehouse's place) */
	{ "Villa", { 1, 1, 1 }, 1u << LG_AESTH_VILLA },
	{ "Docks", { 1, 1, 1 }, 1u << LG_AESTH_HARBOUR },        /* (Pier 17's parts, random: lgTryDocks) */
	{ "Backwoods", { 1, 1, 1 }, 1u << LG_AESTH_WOODLAND },   /* (Hollow Creek's, random: lgTryBackwoods) */
	{ "Penthouse", { 1, 1, 1 }, 1u << LG_AESTH_PENTHOUSE },  /* (lgTryHighrise) */
	{ "Building Site", { 1, 1, 1 }, 1u << LG_AESTH_SITE },  /* (lgTryHighrise) */
	{ "Market Town", { 0, 0, 0 }, 1u << LG_AESTH_MARKET },  /* (lgTryPlaza; 2026-11-19, the user: "terrible and should be removed": never dealt, not in the menus, its slot kept so the layout numbers stay) */
	{ "Mall", { 1, 1, 1 }, 1u << LG_AESTH_MALL },  /* (lgTryMall) */
	{ "Pipeworks", { 0, 0, 0 }, 1u << LG_AESTH_PIPEWORKS },  /* (lgTryPipeworks; 2026-11-19, the user: remove it: never dealt, not in the menus, its slot kept so the layout numbers stay) */
	{ "Pier 17", { 0, 0, 0 }, 1u << LG_AESTH_HARBOUR },   /* (designed: lgTryPier) */
	{ "Hollow Creek", { 0, 0, 0 }, 1u << LG_AESTH_WOODLAND },   /* (designed: lgTryTrail) */
	{ "Switchback Station", { 0, 0, 0 }, 1u << LG_AESTH_STATION },
	{ "Bunker 2", { 0, 0, 0 }, 1u << LG_AESTH_BUNKER2 },   /* (designed, 2026-11-26: lgTryBunker2) */
	/* (2026-11-15, the user: "all our random generated maps should use custom
	 * textures": every layout has its own look made of the generator's own
	 * textures, tools/levelgen/lgtexgen.py; Placeholder and Chicago stay in
	 * the tables, unused) */
};

/* a test or the menu can fix either theme: -1 leaves it to the seed */
static int g_LgForceLayout = -1, g_LgForceAesthetic = -1;

void modLevelGenForceTheme(int layout, int aesthetic)
{
	g_LgForceLayout = layout >= 0 && layout < LG_LAYOUT_ALL ? layout : -1;
	g_LgForceAesthetic = aesthetic >= 0 && aesthetic < LG_AESTH_ALL ? aesthetic : -1;
}

static const char *g_LgAestheticNames[LG_AESTH_ALL] = {
	"Placeholder",
	"Chicago",
	"Canyon",
	"City",
	"Rooftops",
	"Facility",
	"Labs",
	"Villa",
	"Harbour",
	"Woodland",
	"Penthouse",
	"Site",
	"Station",
	"Market",
	"Mall",
	"Pipeworks",
	"Bunker",
};

const char *modLevelGenLayoutName(int layout)
{
	return layout >= 0 && layout < LG_LAYOUT_ALL ? g_LgLayouts[layout].name : "?";
}

const char *modLevelGenAestheticName(int aesthetic)
{
	return aesthetic >= 0 && aesthetic < LG_AESTH_ALL ? g_LgAestheticNames[aesthetic] : "?";
}

const char *modLevelGenLevelName(LgTheme theme, char *buf, int buflen)
{
	snprintf(buf, (size_t)buflen, "(PR) %s, %s", modLevelGenLayoutName(theme.layout), modLevelGenAestheticName(theme.aesthetic));
	return buf;
}

/* From a stream of its own, so the themes are the same on every retry and
 * the layout generators' numbers do not move when a theme is added */
LgTheme modLevelGenPickTheme(uint32_t seed, int size)
{
	LgTheme t;
	LgRng r;
	int i, total = 0, pick;

	if (size < 0 || size >= LG_SIZE_COUNT) {
		size = LG_SIZE_MEDIUM;
	}
	r.s = seed ^ 0x7e3a11c5u ^ ((uint32_t)size << 20);
	t.layout = LG_LAYOUT_HUB;
	t.aesthetic = 0;

	for (i = 0; i < LG_LAYOUT_COUNT; i++) {
		total += g_LgLayouts[i].weight[size];
	}
	pick = total > 0 ? (int)(lgRand(&r) % (uint32_t)total) : 0;
	for (i = 0; i < LG_LAYOUT_COUNT; i++) {
		if (pick < g_LgLayouts[i].weight[size]) {
			t.layout = (uint8_t)i;
			break;
		}
		pick -= g_LgLayouts[i].weight[size];
	}
	if (g_LgForceLayout >= 0) {
		t.layout = (uint8_t)g_LgForceLayout;
	}

	total = 0;
	for (i = 0; i < LG_AESTH_ALL; i++) {
		total += (g_LgLayouts[t.layout].aesthetics >> i) & 1;
	}
	pick = total > 0 ? (int)(lgRand(&r) % (uint32_t)total) : 0;
	for (i = 0; i < LG_AESTH_ALL; i++) {
		if (((g_LgLayouts[t.layout].aesthetics >> i) & 1) && pick-- == 0) {
			t.aesthetic = (uint8_t)i;
			break;
		}
	}
	if (g_LgForceAesthetic >= 0 && ((g_LgLayouts[t.layout].aesthetics >> g_LgForceAesthetic) & 1)) {
		t.aesthetic = (uint8_t)g_LgForceAesthetic;
	}
	return t;
}

static int lgTry(LgLayout *lg, const LgParams *p, LgRng *r, char *err, int errlen)
{
	switch (lg->theme.layout) {
	case LG_LAYOUT_STATION:
		return lgTryStation(lg, r, err, errlen);
	case LG_LAYOUT_BUNKER2:
		return lgTryBunker2(lg, r, err, errlen);
	case LG_LAYOUT_LAB:
		return lgTryLab(lg, p, r, err, errlen);
	case LG_LAYOUT_STREET:
		return lgTryStreet(lg, p, r, err, errlen);
	case LG_LAYOUT_RAVINE:
		return lgTryRavine(lg, p, r, err, errlen);
	case LG_LAYOUT_ROOFTOP:
		return lgTryRooftop(lg, p, r, err, errlen);
	case LG_LAYOUT_VILLA:
		return lgTryVilla(lg, p, r, err, errlen);
	case LG_LAYOUT_PIER:
		return lgTryPier(lg, p, r, err, errlen);
	case LG_LAYOUT_TRAIL:
		return lgTryTrail(lg, p, r, err, errlen);
	case LG_LAYOUT_DOCKS:
		return lgTryDocks(lg, p, r, err, errlen);
	case LG_LAYOUT_BACKWOODS:
		return lgTryBackwoods(lg, p, r, err, errlen);
	case LG_LAYOUT_PENTHOUSE:
	case LG_LAYOUT_SITE:
		return lgTryHighrise(lg, p, r, err, errlen);
	case LG_LAYOUT_PLAZA:
		return lgTryPlaza(lg, p, r, err, errlen);
	case LG_LAYOUT_MALL:
		return lgTryMall(lg, p, r, err, errlen);
	case LG_LAYOUT_PIPEWORKS:
		return lgTryPipeworks(lg, p, r, err, errlen);
	case LG_LAYOUT_HUB:
	default:
		return lgTryHub(lg, p, r, err, errlen);
	}
}

int modLevelGenBuild(LgLayout *lg, const LgParams *p, char *err, int errlen)
{
	int attempt;
	int size = p->size < LG_SIZE_COUNT ? p->size : LG_SIZE_MEDIUM;

	for (attempt = 0; attempt < LG_RETRIES; attempt++) {
		LgRng r;

		memset(lg, 0, sizeof(*lg));
		lg->version = MODLEVELGEN_VERSION;
		lg->seed = p->seed;
		lg->size = (uint8_t)size;
		lg->attempt = (uint8_t)attempt;
		lg->theme = modLevelGenPickTheme(p->seed, size);
		lg->gw = lg->gd = lg->theme.layout == LG_LAYOUT_RAVINE ? g_LgRavine[size].grid
			: lg->theme.layout == LG_LAYOUT_STATION ? 64 + size * 6
			: lg->theme.layout == LG_LAYOUT_ROOFTOP ? g_LgRooftop[size].grid
			: lg->theme.layout == LG_LAYOUT_VILLA ? g_LgVilla[size].grid
			: lg->theme.layout == LG_LAYOUT_BUNKER2 ? LG_BK_GRID
			: lg->theme.layout == LG_LAYOUT_PIER ? LG_PIER_GRID
			: lg->theme.layout == LG_LAYOUT_TRAIL ? LG_TRAIL_GRID
			: lg->theme.layout == LG_LAYOUT_DOCKS ? g_LgDocksGrid[size]
			: lg->theme.layout == LG_LAYOUT_BACKWOODS ? g_LgWoodsGrid[size]
			: lg->theme.layout == LG_LAYOUT_PLAZA ? g_LgPlazaGrid[size]
			: lg->theme.layout == LG_LAYOUT_MALL ? g_LgMall[size].grid
			: lg->theme.layout == LG_LAYOUT_PIPEWORKS ? g_LgPipe[size].grid
			: lg->theme.layout == LG_LAYOUT_STREET ? g_LgStreet[size].grid
			: LG_LAYOUT_HIGHRISE(lg->theme.layout) ? g_LgHighrise[size].grid : g_LgSizes[size].gw;
		lg->originx = -lg->gw * LG_CELL / 2;
		lg->originz = -lg->gd * LG_CELL / 2;

		r.s = p->seed ^ (0x51ed270bu * (uint32_t)(attempt + 1)) ^ (MODLEVELGEN_VERSION << 24);

		if (lgTry(lg, p, &r, err, errlen)) {
			return 1;
		}
#ifdef LGVL_DEBUG
		fprintf(stderr, "attempt %d: %s\n", attempt, err);
#endif
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Validation                                                           */

static int lgPadInArea(const LgLayout *lg, const LgPad *pad, int margin)
{
	const LgArea *a = &lg->areas[pad->area];
	return pad->x >= lgWorldX(lg, a->x) + margin && pad->x <= lgWorldX(lg, a->x + a->w) - margin
		&& pad->z >= lgWorldZ(lg, a->z) + margin && pad->z <= lgWorldZ(lg, a->z + a->d) - margin;
}

/* a portal's edge at cell x, z is open in the grid of each of its areas'
 * layers over the ground (the ground's own if both are on it) */
static int lgPortalOpenAt(const LgLayout *lg, const LgPortal *p, int x, int z, uint8_t bit)
{
	const int la = lg->areas[p->a].layer, lb = lg->areas[p->b].layer;

	if (!la && !lb) {
		return (lg->open[z][x] & bit) != 0;
	}
	return (!la || (LG_OPENS(lg, la)[z][x] & bit)) && (!lb || (LG_OPENS(lg, lb)[z][x] & bit));
}

int modLevelGenValidate(const LgLayout *lg, char *err, int errlen)
{
	int i, j, x, z;
	int16_t queue[LG_MAXPADS];
	uint8_t seen[LG_MAXPADS];
	int head = 0, tail = 0, numnav = 0, reached = 0;
	int roomportals[LG_MAXROOMS];

	if (lg->theme.layout >= LG_LAYOUT_ALL || lg->theme.aesthetic >= LG_AESTH_ALL
			|| !((g_LgLayouts[lg->theme.layout].aesthetics >> lg->theme.aesthetic) & 1)) {
		lgSetErr(err, errlen, "bad themes: layout %d aesthetic %d", lg->theme.layout, lg->theme.aesthetic);
		return 0;
	}

	if (lg->numrooms < 3 || lg->numareas > LG_MAXAREAS || lg->numpads > LG_MAXPADS) {
		lgSetErr(err, errlen, "bad counts: rooms %d areas %d", lg->numrooms, lg->numareas);
		return 0;
	}

	/* areas: in bounds, rooms apart, every cell owned by its rectangle */
	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];

		if (a->w < 1 || a->d < 1 || a->x < 0 || a->z < 0
				|| a->x + a->w > lg->gw || a->z + a->d > lg->gd) {
			lgSetErr(err, errlen, "area %d out of bounds", i, 0);
			return 0;
		}

		for (z = a->z; z < a->z + a->d; z++) {
			for (x = a->x; x < a->x + a->w; x++) {
				if (LG_CELLS(lg, a->layer)[z][x] != i + 1) {
					lgSetErr(err, errlen, "area %d does not own cell (%d)", i, z * lg->gw + x);
					return 0;
				}
			}
		}

		if (a->kind == LG_AREA_ROOM) {
			for (j = 0; j < i; j++) {
				const LgArea *o = &lg->areas[j];
				if (o->kind != LG_AREA_ROOM || o->room == a->room || a->special == LG_SPECIAL_BALCONY || o->special == LG_SPECIAL_BALCONY
						|| a->special == LG_SPECIAL_BRIDGE || o->special == LG_SPECIAL_BRIDGE
						|| a->special == LG_SPECIAL_ROOF || o->special == LG_SPECIAL_ROOF
						|| lg->theme.layout == LG_LAYOUT_STATION
						|| ((LG_LAYOUT_SKYLIKE(lg->theme.layout) || LG_LAYOUT_FLATBUILT(lg->theme.layout)) && (a->open || o->open))) {
					/* corridors run up to rooms; a room's own wings touch it; a balcony overlooks the street;
					 * (the rooftops' roofs and streets run up to the huts' walls, one cell thick) */
					continue;
				}
				if (a->x < o->x + o->w + LG_ROOMGAP && o->x < a->x + a->w + LG_ROOMGAP
						&& a->z < o->z + o->d + LG_ROOMGAP && o->z < a->z + a->d + LG_ROOMGAP) {
					lgSetErr(err, errlen, "rooms %d and %d too close", j, i);
					return 0;
				}
			}
		}
	}

	/* every street has a bridge (user, 2026-11-14): a street without one is retried */
	if (lg->theme.layout == LG_LAYOUT_STREET) {
		int nb = 0;

		for (i = 0; i < lg->numareas; i++) {
			nb += lg->areas[i].special == LG_SPECIAL_BRIDGE;
		}
		if (nb == 0) {
			lgSetErr(err, errlen, "a street with no bridge", 0, 0);
			return 0;
		}
	}

	/* every level has a second floor */
	{
		int nm = 0;

		for (i = 0; i < lg->numareas; i++) {
			nm += (lg->areas[i].flags & LG_AREAFLAG_MEZZ) != 0;
		}
		if (nm == 0 && lg->theme.layout != LG_LAYOUT_BUNKER2 && lg->theme.layout != LG_LAYOUT_RAVINE && lg->theme.layout != LG_LAYOUT_PIPEWORKS && !LG_LAYOUT_SKYLIKE(lg->theme.layout) && lg->theme.layout != LG_LAYOUT_VILLA && !LG_LAYOUT_FLATBUILT(lg->theme.layout)) {   /* (a ravine's height is its ground; the rooftops', their towers) */
			lgSetErr(err, errlen, "no mezzanine (no room fits one)", 0, 0);
			return 0;
		}
	}

	/* a mezzanine sits exactly over an undercroft of its room: the slab between,
	 * headroom above, and a level way up (a portal with no step) */
	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];
		int under, level = 0, q;

		if (!(a->flags & LG_AREAFLAG_MEZZ) && !a->layer) {
			continue;
		}
		if (lg->theme.layout == LG_LAYOUT_PIPEWORKS && a->layer && (a->special == LG_SPECIAL_BRIDGE || a->special == LG_SPECIAL_PIPE)) {
			/* the pipeworks' pipes and catwalks (2026-11-19): over the sludge, a
			 * layer 2 one with headroom over what layer 1 has under it; level
			 * onto floor or another piece, drops onto anything lower */
			int cx, cz, level2 = 0;

			for (cz = a->z; cz < a->z + a->d; cz++) {
				for (cx = a->x; cx < a->x + a->w; cx++) {
					const int g = lg->cells[cz][cx] - 1, m = lg->cells2[cz][cx] - 1;

					if (g < 0 || lg->areas[g].special != LG_SPECIAL_PIT || a->floor <= lg->areas[g].floor || a->ramp
							|| (a->layer == 2 && m >= 0 && lg->areas[m].floor + LG_MEZZ_SLAB + LG_MEZZ_HEAD > a->floor)) {
						lgSetErr(err, errlen, "pipeworks piece %d is not over the sludge", i, 0);
						return 0;
					}
				}
			}
			for (q = 0; q < lg->numportals; q++) {
				const LgPortal *pt = &lg->portals[q];
				const int o = pt->a == i ? pt->b : pt->b == i ? pt->a : -1;

				if (o < 0) {
					continue;
				}
				if (pt->drop ? lg->areas[o].floor == a->floor : lg->areas[o].floor != a->floor) {
					lgSetErr(err, errlen, "pipeworks piece %d has a wrong opening (%d)", i, q);
					return 0;
				}
				level2 += !pt->drop;
			}
			if (level2 == 0) {
				lgSetErr(err, errlen, "pipeworks piece %d leads nowhere", i, 0);
				return 0;
			}
			continue;
		}
		if (a->special == LG_SPECIAL_BRIDGE && (lg->theme.layout == LG_LAYOUT_RAVINE || lg->theme.layout == LG_LAYOUT_ROOFTOP)) {
			/* a ravine's bridge or catwalk piece: over the pit only, above its floor;
			 * its openings level onto ground or another piece, or ledges over the pit */
			const int wh = 0;
			int cx, cz, level2 = 0;

			for (cz = a->z; cz < a->z + a->d; cz++) {
				for (cx = a->x; cx < a->x + a->w; cx++) {
					const int g = lg->cells[cz][cx] - 1;

					if (g < 0 || (wh ? lg->areas[g].room != a->room || lg->areas[g].layer : lg->areas[g].special != LG_SPECIAL_PIT)
							|| a->floor <= lg->areas[g].floor || !a->layer || (!wh && !a->open)) {
						lgSetErr(err, errlen, "ravine bridge %d is not over the pit", i, 0);
						return 0;
					}
				}
			}
			for (q = 0; q < lg->numportals; q++) {
				const LgPortal *pt = &lg->portals[q];
				const int o = pt->a == i ? pt->b : pt->b == i ? pt->a : -1;

				if (o < 0) {
					continue;
				}
				if (pt->drop ? (wh ? lg->areas[o].room != a->room || lg->areas[o].layer || lg->areas[o].floor >= a->floor
							: lg->areas[o].special != LG_SPECIAL_PIT) : lg->areas[o].floor != a->floor) {
					lgSetErr(err, errlen, "ravine bridge %d has a wrong opening (%d)", i, q);
					return 0;
				}
				level2 += !pt->drop;
			}
			if (level2 == 0) {
				lgSetErr(err, errlen, "ravine bridge %d leads nowhere", i, 0);
				return 0;
			}
			continue;
		}
		if (a->special == LG_SPECIAL_BRIDGE) {
			/* a bridge: over flat outdoor street with the porch's headroom and a
			 * slab under it, its two ends level with a balcony each */
			int cx, cz, ends = 0, sides = 0;

			if (!a->layer || !a->open || a->ramp || a->kind != LG_AREA_ROOM) {
				lgSetErr(err, errlen, "bridge %d is not an outdoor room on the upper storey", i, 0);
				return 0;
			}
			for (cz = a->z; cz < a->z + a->d; cz++) {
				for (cx = a->x; cx < a->x + a->w; cx++) {
					const int g = lg->cells[cz][cx] - 1;

					if (g < 0 || !(lg->areas[g].flags & LG_AREAFLAG_HUB) || !lg->areas[g].open || lg->areas[g].ramp
							|| a->floor != lg->areas[g].floor + LG_MEZZ_UNDER + LG_MEZZ_SLAB) {
						lgSetErr(err, errlen, "bridge %d is not over flat street with room under it", i, 0);
						return 0;
					}
				}
			}
			for (q = 0; q < lg->numportals; q++) {
				const LgPortal *pt = &lg->portals[q];
				const int o = pt->a == i ? pt->b : pt->b == i ? pt->a : -1;

				if (o < 0) {
					continue;
				}
				if (!pt->drop && lg->areas[o].special == LG_SPECIAL_BALCONY && lg->areas[o].floor == a->floor) {
					ends++;
				} else if (pt->drop && !pt->norail && (lg->areas[o].flags & LG_AREAFLAG_HUB)) {
					sides++;
				} else {
					lgSetErr(err, errlen, "bridge %d has a wrong opening (%d)", i, q);
					return 0;
				}
			}
			if (ends != 2 || sides != 2) {
				lgSetErr(err, errlen, "bridge %d has %d ends and %d sides", i, ends * 10 + sides);
				return 0;
			}
			continue;
		}
		if (a->special == LG_SPECIAL_BALCONY) {
			/* a balcony: over a porch (a low corridor) exactly, its slab between,
			 * outdoors, with a level way onto it */
			/* (2026-11-15: or the porch and one row more, out over flat street
			 * with the porch's headroom under it) */
			int cx, cz, out = 0;
			const LgArea *pc;

			under = -1;
			for (cz = a->z; cz < a->z + a->d && under < 0; cz++) {
				for (cx = a->x; cx < a->x + a->w && under < 0; cx++) {
					const int g = lg->cells[cz][cx] - 1;

					if (a->layer && !(a->flags & LG_AREAFLAG_MEZZ) && g >= 0 && lg->areas[g].kind == LG_AREA_CORRIDOR) {
						under = g;
					}
				}
			}
			pc = under >= 0 ? &lg->areas[under] : NULL;
			if (!pc || pc->special || pc->ramp || pc->x < a->x || pc->z < a->z || pc->x + pc->w > a->x + a->w || pc->z + pc->d > a->z + a->d
					|| (a->w - pc->w) + (a->d - pc->d) > 1
					|| a->floor != pc->ceil + LG_MEZZ_SLAB || !a->open || a->ramp || a->kind != LG_AREA_ROOM) {
				lgSetErr(err, errlen, "balcony %d is not over a porch", i, 0);
				return 0;
			}
			for (cz = a->z; cz < a->z + a->d; cz++) {
				for (cx = a->x; cx < a->x + a->w; cx++) {
					const int g = lg->cells[cz][cx] - 1;

					if (g == under) {
						continue;
					}
					if (g < 0 || !(lg->areas[g].flags & LG_AREAFLAG_HUB) || !lg->areas[g].open || lg->areas[g].ramp
							|| a->floor != lg->areas[g].floor + LG_MEZZ_UNDER + LG_MEZZ_SLAB) {
						lgSetErr(err, errlen, "balcony %d stands out over something not flat street", i, 0);
						return 0;
					}
					out++;
				}
			}
			(void)out;
			for (q = 0; q < lg->numportals; q++) {
				const LgPortal *pt = &lg->portals[q];

				level += (pt->a == i || pt->b == i) && !pt->drop;
			}
			if (!level) {
				lgSetErr(err, errlen, "balcony %d has no way onto it", i, 0);
				return 0;
			}
			{
				/* a drop-down and a railing; or a bridge, with drop-downs beside it */
				int drops = 0, railed = 0, bridges = 0;

				for (q = 0; q < lg->numportals; q++) {
					const LgPortal *pt = &lg->portals[q];
					const int o = pt->a == i ? pt->b : pt->b == i ? pt->a : -1;

					if (o >= 0 && pt->drop) {
						drops += pt->norail;
						railed += !pt->norail;
					}
					bridges += o >= 0 && !pt->drop && lg->areas[o].special == LG_SPECIAL_BRIDGE;
				}
				/* (standing out, its two ends are railed too) */
				if (bridges ? (bridges != 1 || railed > 2 || drops < 1) : (drops < 1 || drops > 2 || railed < 1 || railed > 2)) {
					lgSetErr(err, errlen, "balcony %d has %d drop-downs, %d railed edges and bridges", i, drops * 100 + railed * 10 + bridges);
					return 0;
				}
			}
			continue;
		}
		if (a->special == LG_SPECIAL_WINDOW) {
			/* an upper window: over a ground window exactly, as tall as one, and
			 * its two openings windows (the portal rules check them) */
			int n = 0;

			under = lg->cells[a->z][a->x] - 1;
			if (LG_LAYOUT_HIGHRISE(lg->theme.layout)) {
				/* a highrise's (2026-11-17): through the outside wall on its
				 * storey's layer, over the wall or the windows of the storeys
				 * under it, floor to head height */
				int l2;

				for (l2 = 0; l2 < a->layer; l2++) {
					const int g = LG_CELLS(lg, l2)[a->z][a->x] - 1;

					if (g >= 0 && lg->areas[g].special != LG_SPECIAL_WINDOW && lg->areas[g].kind != LG_AREA_CORRIDOR) {
						break;
					}
				}
				if (l2 < a->layer || a->ceil - a->floor != LG_HR_WINH || a->ramp) {
					lgSetErr(err, errlen, "highrise window %d is not in the outside wall", i, 0);
					return 0;
				}
			} else if (lg->theme.layout == LG_LAYOUT_ROOFTOP) {
				/* a tower's upper window (2026-11-16): through the tower's wall, on
				 * its storey's layer, over nothing (the wall) */
				if (!a->layer || under >= 0 || (a->layer == 2 && lg->cells2[a->z][a->x]) || a->ceil - a->floor != LG_WIN_HEAD - LG_WIN_SILL || a->ramp) {
					lgSetErr(err, errlen, "tower window %d is not in the tower's wall", i, 0);
					return 0;
				}
			} else if (lg->theme.layout == LG_LAYOUT_STATION) {
				if (a->layer != 1 || under < 0 || a->floor != LG_STN_PLATFORM_Y + LG_WIN_SILL
						|| a->ceil != LG_STN_PLATFORM_Y + LG_WIN_HEAD || a->ramp) {
					lgSetErr(err, errlen, "invalid train window %d", i, 0);
					return 0;
				}
			} else if (!a->layer || under < 0 || lg->areas[under].special != LG_SPECIAL_WINDOW || lg->areas[under].layer
					|| lg->areas[under].x != a->x || lg->areas[under].z != a->z || lg->areas[under].w != a->w || lg->areas[under].d != a->d
					|| a->floor <= lg->areas[under].ceil || a->ceil - a->floor != LG_WIN_HEAD - LG_WIN_SILL || a->ramp) {
				lgSetErr(err, errlen, "upper window %d is not over a window", i, 0);
				return 0;
			}
			for (q = 0; q < lg->numportals; q++) {
				n += lg->portals[q].a == i || lg->portals[q].b == i;
			}
			if (LG_LAYOUT_HIGHRISE(lg->theme.layout) ? n < 2 : n != 2) {   /* (a highrise's: onto a storey's seam, two inside) */
				lgSetErr(err, errlen, "upper window %d has %d openings", i, n);
				return 0;
			}
			continue;
		}
		if (a->special == LG_SPECIAL_ROOF) {
			/* a villa's roof slope (2026-11-15): outdoors on the upper storey, a
			 * slope, over its building (nothing under it reaches its eave); its
			 * openings level onto the other slope or a landing, or drops onto
			 * the garden */
			int cx, cz, level2 = 0;

			if (!a->layer || !a->open || !a->ramp || a->floorlo >= a->floor) {
				lgSetErr(err, errlen, "roof %d is not a slope on the upper storey", i, 0);
				return 0;
			}
			for (cz = a->z; cz < a->z + a->d; cz++) {
				for (cx = a->x; cx < a->x + a->w; cx++) {
					const int g = lg->cells[cz][cx] - 1;

					if (g >= 0 && (lg->areas[g].room == a->room || lg->areas[g].ceil > a->floorlo)) {
						lgSetErr(err, errlen, "roof %d is not over a building (cell %d)", i, cz * lg->gw + cx);
						return 0;
					}
				}
			}
			for (q = 0; q < lg->numportals; q++) {
				const LgPortal *pt = &lg->portals[q];
				const int o = pt->a == i ? pt->b : pt->b == i ? pt->a : -1;

				if (o < 0) {
					continue;
				}
				if (pt->drop ? lg->areas[o].room != a->room || lg->areas[o].layer || lg->areas[o].floor >= a->floorlo
						: !(lg->areas[o].special == LG_SPECIAL_ROOF || (!lg->areas[o].layer && lg->areas[o].floor == a->floorlo))) {
					lgSetErr(err, errlen, "roof %d has a wrong opening (%d)", i, q);
					return 0;
				}
				level2 += !pt->drop && lg->areas[o].special != LG_SPECIAL_ROOF;
			}
			(void)level2;
			continue;
		}
		if (!(a->flags & LG_AREAFLAG_MEZZ) || !a->layer) {
			lgSetErr(err, errlen, "area %d is a layer 1 area that is not a mezzanine", i, 0);
			return 0;
		}
		under = LG_CELLS(lg, a->layer - 1)[a->z][a->x] - 1;
		if (LG_LAYOUT_SKYLIKE(lg->theme.layout) || lg->theme.layout == LG_LAYOUT_STATION) {
			/* a tower's storey (2026-11-16): pieces of it over pieces of the one
			 * under, each cell over an undercroft of the tower's room, the slab
			 * between; its ramp up to the next one of them is a piece too */
			int cx, cz;

			for (cz = a->z; cz < a->z + a->d; cz++) {
				for (cx = a->x; cx < a->x + a->w; cx++) {
					const int g = LG_CELLS(lg, a->layer - 1)[cz][cx] - 1;

					/* (a penthouse storey's floor runs over the partition walls of the one under, 2026-11-17) */
					if (lg->theme.layout == LG_LAYOUT_STATION && a->layer == 2 && a->theme == LG_STN_CAR
							&& (g < 0 || lg->areas[g].special == LG_SPECIAL_WINDOW
								|| lg->areas[g].theme == LG_STN_VESTIBULE)) {
						continue;
					}
					if (g < 0 && LG_LAYOUT_HIGHRISE(lg->theme.layout) && !a->ramp) {
						continue;
					}
					if (g < 0 || !(lg->areas[g].flags & LG_AREAFLAG_UNDER)
							|| (lg->theme.layout != LG_LAYOUT_STATION && lg->areas[g].room != a->room)
							|| a->floorlo != lg->areas[g].ceil + LG_MEZZ_SLAB) {
						lgSetErr(err, errlen, "storey %d is not over an undercroft of its tower", i, 0);
						return 0;
					}
				}
			}
			if (a->ceil - a->floor < LG_MEZZ_HEAD - 20 || (a->ramp && !(a->flags & LG_AREAFLAG_BAY))) {
				lgSetErr(err, errlen, "storey %d has the wrong headroom", i, 0);
				return 0;
			}
		} else if (under < 0 || !(lg->areas[under].flags & LG_AREAFLAG_UNDER) || lg->areas[under].room != a->room
				|| lg->areas[under].x != a->x || lg->areas[under].z != a->z || lg->areas[under].w != a->w || lg->areas[under].d != a->d) {
			lgSetErr(err, errlen, "mezzanine %d is not over an undercroft of its room", i, 0);
			return 0;
		} else if (a->floor != lg->areas[under].floor + (lg->areas[under].ceil - lg->areas[under].floor) + LG_MEZZ_SLAB
				|| a->ceil - a->floor < LG_MEZZ_HEAD || a->ramp) {
			lgSetErr(err, errlen, "mezzanine %d has the wrong floor or headroom", i, 0);
			return 0;
		}
		for (q = 0; q < lg->numportals; q++) {
			const LgPortal *pt = &lg->portals[q];

			level += (pt->a == i || pt->b == i) && !pt->drop;
		}
		if (!level) {
			lgSetErr(err, errlen, "mezzanine %d has no way up", i, 0);
			return 0;
		}
	}

	/* every upper-storey cell belongs to a layer 1 area's rectangle over a ground cell */
	for (z = 0; z < lg->gd; z++) {
		for (x = 0; x < lg->gw; x++) {
			int a = lg->cells2[z][x] - 1;
			if (a >= 0) {
				const LgArea *ar = &lg->areas[a];
				if (a >= lg->numareas || !ar->layer || (lg->cells[z][x] == 0 && ar->special != LG_SPECIAL_ROOF && ar->special != LG_SPECIAL_WINDOW && !LG_LAYOUT_HIGHRISE(lg->theme.layout)) || x < ar->x || x >= ar->x + ar->w
						|| z < ar->z || z >= ar->z + ar->d) {
					lgSetErr(err, errlen, "stray upper cell %d in area %d", z * lg->gw + x, a);
					return 0;
				}
			}
		}
	}

	/* every third-storey cell belongs to a layer 2 area's rectangle over a layer 1 cell */
	for (z = 0; z < lg->gd; z++) {
		for (x = 0; x < lg->gw; x++) {
			const int a = lg->cells3[z][x] - 1;

			if (a >= 0 && (a >= lg->numareas || lg->areas[a].layer != 2 || (lg->cells2[z][x] == 0 && lg->areas[a].special != LG_SPECIAL_WINDOW && !LG_LAYOUT_HIGHRISE(lg->theme.layout) && lg->theme.layout != LG_LAYOUT_PIPEWORKS && !(lg->theme.layout == LG_LAYOUT_STATION && lg->areas[a].theme == LG_STN_CAR)) || x < lg->areas[a].x || x >= lg->areas[a].x + lg->areas[a].w
					|| z < lg->areas[a].z || z >= lg->areas[a].z + lg->areas[a].d)) {
				lgSetErr(err, errlen, "stray third-storey cell %d in area %d", z * lg->gw + x, a);
				return 0;
			}
		}
	}

	/* every grid cell belongs to an area's rectangle or is solid */
	for (z = 0; z < lg->gd; z++) {
		for (x = 0; x < lg->gw; x++) {
			int a = lg->cells[z][x] - 1;
			if (a >= 0) {
				const LgArea *ar = &lg->areas[a];
				if (a >= lg->numareas || x < ar->x || x >= ar->x + ar->w
						|| z < ar->z || z >= ar->z + ar->d) {
					lgSetErr(err, errlen, "stray cell %d in area %d", z * lg->gw + x, a);
					return 0;
				}
			}
		}
	}

	/* open edges only between two different areas */
	for (z = 0; z < lg->gd; z++) {
		for (x = 0; x < lg->gw; x++) {
			int a = lgAreaAt(lg, x, z);
			if (lg->open[z][x] & LG_OPEN_XP) {
				int b = lgAreaAt(lg, x + 1, z);
				if (a < 0 || b < 0 || a == b) {
					lgSetErr(err, errlen, "bad open x edge at cell %d", z * lg->gw + x, 0);
					return 0;
				}
			}
			if (lg->open[z][x] & LG_OPEN_ZP) {
				int b = lgAreaAt(lg, x, z + 1);
				if (a < 0 || b < 0 || a == b) {
					lgSetErr(err, errlen, "bad open z edge at cell %d", z * lg->gw + x, 0);
					return 0;
				}
			}
		}
	}

	/* portals: every cell along a portal is open, sides match its areas */
	for (i = 0; i < LG_MAXROOMS; i++) {
		roomportals[i] = 0;
	}

	for (i = 0; i < lg->numportals; i++) {
		const LgPortal *p = &lg->portals[i];

		if (p->x0 == p->x1) {
			if (p->z1 <= p->z0) {
				lgSetErr(err, errlen, "portal %d empty", i, 0);
				return 0;
			}
			for (z = p->z0; z < p->z1; z++) {
				if (!lgPortalOpenAt(lg, p, p->x0 - 1, z, LG_OPEN_XP)
						|| !lgOwns(lg, p->a, p->x0 - 1, z) || !lgOwns(lg, p->b, p->x0, z)) {
					lgSetErr(err, errlen, "portal %d mismatch at z %d", i, z);
					return 0;
				}
			}
		} else {
			if (p->x1 <= p->x0 || p->z0 != p->z1) {
				lgSetErr(err, errlen, "portal %d malformed", i, 0);
				return 0;
			}
			for (x = p->x0; x < p->x1; x++) {
				if (!lgPortalOpenAt(lg, p, x, p->z0 - 1, LG_OPEN_ZP)
						|| !lgOwns(lg, p->a, x, p->z0 - 1) || !lgOwns(lg, p->b, x, p->z0)) {
					lgSetErr(err, errlen, "portal %d mismatch at x %d", i, x);
					return 0;
				}
			}
		}

		/* two room areas only ever meet as a room and its own wing, open */
		/* (a balcony's ledge over the street is the one opening between two rooms) */
		if (lg->areas[p->a].kind == LG_AREA_ROOM && lg->areas[p->b].kind == LG_AREA_ROOM
				&& !lgStationRoofEdge(lg, p->a, p->b)
				&& !(p->drop && (lg->areas[p->a].special == LG_SPECIAL_BALCONY || lg->areas[p->b].special == LG_SPECIAL_BALCONY))
				&& lg->areas[p->a].special != LG_SPECIAL_BRIDGE && lg->areas[p->b].special != LG_SPECIAL_BRIDGE
				&& (lg->areas[p->a].room != lg->areas[p->b].room
					|| ((p->door || p->doorway) && p->door < LG_DOOR_LIFT && (p->drop || (lg->areas[p->a].special != LG_SPECIAL_BALCONY && lg->areas[p->b].special != LG_SPECIAL_BALCONY))))) {
			/* (and a balcony's doorway from its upper floor has a door) */
			lgSetErr(err, errlen, "portal %d joins rooms %d and %d", i, lg->areas[p->a].room);
			return 0;
		}

		/* the floor meets level across every opening, except a ledge: within
		 * the hub, one side never below the other anywhere along it */
		{
			const int32_t x0 = lgWorldX(lg, p->x0), x1 = lgWorldX(lg, p->x1), z0 = lgWorldZ(lg, p->z0), z1 = lgWorldZ(lg, p->z1);
			const int32_t d0 = modLevelGenFloorAt(lg, p->a, x0, z0) - modLevelGenFloorAt(lg, p->b, x0, z0);
			const int32_t d1 = modLevelGenFloorAt(lg, p->a, x1, z1) - modLevelGenFloorAt(lg, p->b, x1, z1);
			const int32_t dm = modLevelGenFloorAt(lg, p->a, (x0 + x1) / 2, (z0 + z1) / 2) - modLevelGenFloorAt(lg, p->b, (x0 + x1) / 2, (z0 + z1) / 2);

			if (!p->drop && (d0 || d1 || dm)) {
				lgSetErr(err, errlen, "portal %d is a step", i, 0);
				return 0;
			}
			/* a window's two openings are ledges by their nature (its sill), and so
			 * is a balcony's edge over the street: both are checked where they are made */
			if (p->window != (lg->areas[p->a].special == LG_SPECIAL_WINDOW || lg->areas[p->b].special == LG_SPECIAL_WINDOW)
					|| (p->window && (!p->drop || !p->norail || p->door))) {
				lgSetErr(err, errlen, "portal %d is wrong for a window", i, 0);
				return 0;
			}
			if (p->drop && !p->window && !lgStationRoofEdge(lg, p->a, p->b)
					&& lg->areas[p->a].special != LG_SPECIAL_BALCONY && lg->areas[p->b].special != LG_SPECIAL_BALCONY
					&& (lg->areas[p->a].room != lg->areas[p->b].room || lg->areas[p->a].kind != LG_AREA_ROOM || lg->areas[p->b].kind != LG_AREA_ROOM
					|| (d0 > 0 && d1 < 0) || (d0 < 0 && d1 > 0) || (dm > 0 && (d0 < 0 || d1 < 0)) || (dm < 0 && (d0 > 0 || d1 > 0)))) {
				lgSetErr(err, errlen, "portal %d is a ledge between two rooms, or its floors cross", i, 0);
				return 0;
			}
		}

		if (p->door && !p->doorway && p->door < LG_DOOR_LIFT && lg->theme.layout != LG_LAYOUT_BUNKER2) {   /* (Bunker 2's blast doors cross its ring) */   /* (a lift's: from its lobby into its shaft) */
			lgSetErr(err, errlen, "portal %d has a door but is not a doorway", i, 0);
			return 0;
		}

		if (lg->areas[p->a].room >= 0) roomportals[lg->areas[p->a].room]++;
		if (lg->areas[p->b].room >= 0) roomportals[lg->areas[p->b].room]++;
	}

	for (i = 0; i < lg->numrooms; i++) {
		if (roomportals[i] == 0) {
			lgSetErr(err, errlen, "room %d has no opening", i, 0);
			return 0;
		}
	}

	/* the hub layout's promises: a hub that climbs two levels (one on a small
	 * level), and loops */
	{
		int high = 0;

		for (i = 0; i < lg->numareas; i++) {
			if ((lg->areas[i].flags & LG_AREAFLAG_HUB) && lg->areas[i].floor > high) high = lg->areas[i].floor;
		}
		if (lg->theme.layout == LG_LAYOUT_HUB && high < (lg->size == LG_SIZE_SMALL ? 1 : 2) * LG_LEVEL) {
			lgSetErr(err, errlen, "the hub only climbs to %d", high, 0);
			return 0;
		}
		if (lg->numportals - lg->numareas + 1 < 2) {
			lgSetErr(err, errlen, "only %d loops", lg->numportals - lg->numareas + 1, 0);
			return 0;
		}
	}

	/* areas all reached, start and extraction sane */
	for (i = 0; i < lg->numareas; i++) {
		if (lg->areas[i].dist < 0 && lg->areas[i].special != LG_SPECIAL_WINDOW
				&& !(LG_LAYOUT_HIGHRISE(lg->theme.layout) && lg->areas[i].special == LG_SPECIAL_PIT)) {
			lgSetErr(err, errlen, "area %d unreached", i, 0);
			return 0;
		}
	}

	if (lg->startarea == lg->extractarea || lg->areas[lg->extractarea].kind != LG_AREA_ROOM
			|| lg->areas[lg->extractarea].dist < 1) {
		lgSetErr(err, errlen, "bad extraction room %d", lg->extractarea, 0);
		return 0;
	}

	/* pads: inside their areas, kinds counted */
	{
		int numstart = 0, numguard = 0;

		for (i = 0; i < lg->numpads; i++) {
			const LgPad *pad = &lg->pads[i];
			int margin = pad->kind == LG_PAD_GUARD
				? (lg->areas[pad->area].kind == LG_AREA_ROOM ? LG_GUARD_MARGIN : LG_GUARD_MARGIN / 2)
				: 1;

			if (pad->area < 0 || pad->area >= lg->numareas || !lgPadInArea(lg, pad, margin)) {
				lgSetErr(err, errlen, "pad %d outside its area %d", i, pad->area);
				return 0;
			}
			if (pad->y != modLevelGenFloorAt(lg, pad->area, pad->x, pad->z)) {
				lgSetErr(err, errlen, "pad %d is not on its floor (y %d)", i, pad->y);
				return 0;
			}

			if (pad->kind == LG_PAD_START) {
				numstart++;
				if (pad->area != lg->startarea) {
					lgSetErr(err, errlen, "start pad %d not in start room", i, 0);
					return 0;
				}
			} else if (pad->kind == LG_PAD_GUARD) {
				numguard++;
				if (pad->area == lg->startarea) {
					lgSetErr(err, errlen, "guard pad %d in start room", i, 0);
					return 0;
				}
			} else if (pad->kind == LG_PAD_NAV) {
				numnav++;
			} else if (pad->numneigh != 0) {
				lgSetErr(err, errlen, "non-nav pad %d has links", i, 0);
				return 0;
			}

			for (j = 0; j < pad->numneigh; j++) {
				int o = pad->neigh[j];
				if (o < 0 || o >= lg->numpads || lg->pads[o].kind != LG_PAD_NAV
						|| !lgHasNeigh(&lg->pads[o], i)) {
					lgSetErr(err, errlen, "pad %d bad link to %d", i, o);
					return 0;
				}
			}
		}

		if (numstart < 1 || numguard < 1) {
			lgSetErr(err, errlen, "start pads %d guard pads %d", numstart, numguard);
			return 0;
		}
	}

	/* nav graph connected */
	memset(seen, 0, sizeof(seen));
	for (i = 0; i < lg->numpads && tail == 0; i++) {
		if (lg->pads[i].kind == LG_PAD_NAV) {
			queue[tail++] = (int16_t)i;
			seen[i] = 1;
		}
	}

	while (head < tail) {
		const LgPad *pad = &lg->pads[queue[head++]];
		reached++;
		for (j = 0; j < pad->numneigh; j++) {
			int o = pad->neigh[j];
			if (!seen[o]) {
				seen[o] = 1;
				queue[tail++] = (int16_t)o;
			}
		}
	}

	if (reached != numnav) {
#ifdef LGVL_DEBUG
		for (i = 0; i < lg->numpads; i++) {
			if (lg->pads[i].kind == LG_PAD_NAV && !seen[i]) {
				const LgArea *ua = &lg->areas[lg->pads[i].area];
				fprintf(stderr, "unreached nav pad %d area %d (%d,%d %dx%d kind %d floor %d/%d ramp %d sp %d theme %d)\n", i, lg->pads[i].area, ua->x, ua->z, ua->w, ua->d, ua->kind, ua->floorlo, ua->floor, ua->ramp, ua->special, ua->theme);
			}
		}
#endif
		lgSetErr(err, errlen, "nav graph split: %d of %d reached", reached, numnav);
		return 0;
	}

	/* pillars: in a room, well inside it, off the pads and the walkways, apart */
	if (lg->numobst < 0 || lg->numobst > LG_MAXOBST) {
		lgSetErr(err, errlen, "bad pillar count %d", lg->numobst, 0);
		return 0;
	}
	for (i = 0; i < lg->numobst; i++) {
		const LgObstacle *o = &lg->obst[i];
		const LgArea *a;
		int32_t px, pz;

		if (o->area < 0 || o->area >= lg->numareas) {
			lgSetErr(err, errlen, "pillar %d in area %d", i, o->area);
			return 0;
		}
		a = &lg->areas[o->area];
		if (o->height > 0) {
			/* low cover: on a flat room floor, inside it, clear of pads and of
			 * every other obstacle with room to walk between */
			const int32_t x0 = lgWorldX(lg, a->x), z0 = lgWorldZ(lg, a->z);

			if (a->kind != LG_AREA_ROOM || a->ramp || o->x - o->halfx < x0 + 5 || o->x + o->halfx > x0 + a->w * LG_CELL - 5
					|| o->z - o->halfz < z0 + 5 || o->z + o->halfz > z0 + a->d * LG_CELL - 5 || (o->height > 200 && !(o->crate && o->model >= LG_CRATE_MODELS))) {
				lgSetErr(err, errlen, "cover %d is not on a room's floor", i, 0);
				return 0;
			}
			if (o->rail && (o->crate || o->height != LG_RAIL_HEIGHT || (o->halfx != 10 && o->halfz != 10))) {
				lgSetErr(err, errlen, "railing %d is not a railing's shape", i, 0);
				return 0;
			}
			if (o->crate && o->model < LG_CRATE_MODELS && (o->halfx != o->halfz || o->height != 2 * o->halfx
					|| o->scale != o->halfx * 256 / (LG_CRATE_SIZE / 2) || o->turn > 3)) {
				lgSetErr(err, errlen, "crate %d is not a crate's shape", i, 0);
				return 0;
			}
			/* a car: its own measurements, lengthways along the way it faces (turn
			 * 0 and 2: along z), on a street */
			if (o->crate && o->model >= LG_CRATE_MODELS) {
				const LgPropDef *d = o->model < LG_PROP_MODELS ? &g_LgProps[o->model] : NULL;

				if (d == NULL || o->turn > 3 || o->scale != d->scale || o->height != d->height || (!a->open && a->ceil - a->floor < d->height + 20)
						|| o->halfx != ((o->turn & 1) ? d->halfl : d->halfw) || o->halfz != ((o->turn & 1) ? d->halfw : d->halfl)) {
					lgSetErr(err, errlen, "prop %d (model %d) is not its model's shape", i, o->model);
					return 0;
				}
			}
			for (j = 0; j < lg->numpads; j++) {
				const LgPad *p = &lg->pads[j];

				/* (a washroom's toilet stands against a stall's wall, its pads close: 6) */
				const int pg = o->crate && o->model == LG_PROP_TOILET ? 6 : 30;

				if (p->area == o->area && lgAbs(p->x - o->x) < o->halfx + pg && lgAbs(p->z - o->z) < o->halfz + pg) {
					lgSetErr(err, errlen, "cover %d is on pad %d", i, j);
					return 0;
				}
			}
			for (j = 0; j < i; j++) {
				const LgObstacle *q = &lg->obst[j];

				/* (a balcony's railings meet at its corners: nobody walks between them) */
				if (q->area == o->area && lgAbs(q->x - o->x) < q->halfx + o->halfx + 60 && lgAbs(q->z - o->z) < q->halfz + o->halfz + 60
						&& !(o->rail && q->rail && lg->areas[o->area].special == LG_SPECIAL_BALCONY)) {
					lgSetErr(err, errlen, "cover %d crowds obstacle %d", i, j);
					return 0;
				}
			}
			continue;
		}
		if (a->kind != LG_AREA_ROOM || a->ramp || o->cx - a->x < 2 || a->x + a->w - 1 - o->cx < 2
				|| o->cz - a->z < 2 || a->z + a->d - 1 - o->cz < 2) {
			lgSetErr(err, errlen, "pillar %d is not well inside a room", i, 0);
			return 0;
		}
		px = lgWorldX(lg, o->cx) + LG_CELL / 2;
		pz = lgWorldZ(lg, o->cz) + LG_CELL / 2;
		for (j = 0; j < lg->numpads; j++) {
			const LgPad *p = &lg->pads[j];
			int k;

			if (p->area != o->area) {
				continue;
			}
			if (lgAbs(p->x - px) < 150 && lgAbs(p->z - pz) < 150) {
				lgSetErr(err, errlen, "pillar %d is on pad %d", i, j);
				return 0;
			}
			if (p->kind != LG_PAD_NAV) {
				continue;
			}
			for (k = j + 1; k < lg->numpads; k++) {
				const LgPad *q = &lg->pads[k];

				if (q->area == o->area && q->kind == LG_PAD_NAV && lgSegDist2(px, pz, p->x, p->z, q->x, q->z) < 130 * 130) {
					lgSetErr(err, errlen, "pillar %d is on the walkway %d-%d", i, j);
					return 0;
				}
			}
		}
		for (j = 0; j < i; j++) {
			if (lg->obst[j].height == 0 && lgAbs(lg->obst[j].cx - o->cx) < 2 && lgAbs(lg->obst[j].cz - o->cz) < 2) {
				lgSetErr(err, errlen, "pillars %d and %d touch", j, i);
				return 0;
			}
		}
	}

	return 1;
}

/* ------------------------------------------------------------------ */
/* Text map                                                             */

int modLevelGenDumpAscii(const LgLayout *lg, char *buf, int buflen)
{
	static const char roomchars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef";
	int x, z, n = 0, i;

	if (buflen < (lg->gw + 1) * lg->gd + 1) {
		return 0;
	}

	for (z = 0; z < lg->gd; z++) {
		for (x = 0; x < lg->gw; x++) {
			int a = lg->cells[z][x] - 1;
			char c = '#';

			if (a >= 0) {
				c = lg->areas[a].kind == LG_AREA_ROOM ? roomchars[lg->areas[a].room % 32] : '.';
			}

			buf[n + x] = c;
		}
		buf[n + lg->gw] = '\n';
		n += lg->gw + 1;
	}

	for (i = 0; i < lg->numpads; i++) {
		const LgPad *p = &lg->pads[i];
		int cx = (p->x - lg->originx) / LG_CELL;
		int cz = (p->z - lg->originz) / LG_CELL;
		char c = p->kind == LG_PAD_GUARD ? 'g' : p->kind == LG_PAD_START ? '@'
			: p->kind == LG_PAD_OBJECTIVE ? 'o' : 0;

		if (c && cx >= 0 && cz >= 0 && cx < lg->gw && cz < lg->gd) {
			buf[cz * (lg->gw + 1) + cx] = c;
		}
	}

	buf[n] = '\0';
	return n;
}

/* ------------------------------------------------------------------ */
/* Self-test                                                            */

/* the longest straight run of corridor cells with no door across it, in cells */
static int lgLongestHall(const LgLayout *lg)
{
	int x, z, run, best = 0, pass, i;

	for (pass = 0; pass < 2; pass++) {
		for (z = 0; z < lg->gd; z++) {
			run = 0;
			for (x = 0; x < lg->gw; x++) {
				const int ai = pass ? lg->cells[x][z] - 1 : lg->cells[z][x] - 1;

				for (i = 0; i < lg->numportals; i++) {
					const LgPortal *p = &lg->portals[i];

					if (p->door && (pass ? p->z0 == p->z1 && p->z0 == x && p->x0 <= z && z < p->x1
							: p->x0 == p->x1 && p->x0 == x && p->z0 <= z && z < p->z1)) {
						run = 0;
					}
				}
				run = ai >= 0 && lg->areas[ai].kind == LG_AREA_CORRIDOR ? run + 1 : 0;
				if (run > best) best = run;
			}
		}
	}
	return best;
}

static void lgSelfTestLayout(int layout, int numseeds, void (*print)(const char *line), int *ppassed, int *pfailed)
{
	static LgLayout a, b;
	static const char *prefix[LG_LAYOUT_COUNT] = { "levelgen", "levelgen-lab", "levelgen-street", "levelgen-ravine", "levelgen-rooftop", "levelgen-villa", "levelgen-docks", "levelgen-backwoods", "levelgen-penthouse", "levelgen-site", "levelgen-plaza", "levelgen-mall", "levelgen-pipeworks" };
	char err[160], line[480];
	int size, seed, passed = 0, failed = 0;
	int stopen = 0, stbuildings = 0, styards = 0, stcars = 0, stnocars = 0, stwindows = 0, stbalconies = 0, stnobalcony = 0, stbaldrops = 0, stbalrailed = 0, stbridges = 0, stwithbridge = 0, stupper = 0, stupper2 = 0, stupperbr = 0, stupwin = 0, stbalout = 0, stbaldoor = 0;
	int labloopsmin = 9999, labbands = 0, labbandsmin = 99, labhall = 0, labhallsum = 0, labrooms = 0, labcovered = 0, labdead = 0, lablevels = 0;
	int maxareas = 0, maxportals = 0, maxpads = 0, retries = 0, maxneigh = 0;
	int mezzlayouts = 0, mezz = 0, mezzdrops = 0, mezzramps = 0, rooms = 0, shaped = 0, wings = 0, raised = 0, layouts = 0, loops = 0, upper = 0, flat = 0, ledges = 0;
	int terraced = 0, sides = 0, cover = 0, parapets = 0, hubcover = 0;
	int rvpit = 0, rvbridges = 0, rvwithbridge = 0, rvcatwalk = 0, rvhigh = 0, rvhuts = 0;
	int rtok = 0, rttowers = 0, rtglass = 0, rtbridges = 0, rthuts = 0, rtwins = 0, rtlevels = 0, rtupwins = 0;
	int hrok = 0, hrwins = 0, hrfurn = 0, hrvoids = 0, hrdoors = 0;
	int mlok = 0, mlstores = 0, mlwins = 0, mlback = 0, mlfurn = 0, mlstalls = 0;
	int pwok = 0, pwpipes = 0, pwcats = 0, pwdrops = 0, pwgaps = 0, pwfurn = 0;
	int vlok = 0, vlrooms = 0, vlwins = 0, vlbld = 0, vlpiers = 0, vlshare = 0, vlminshare = 100, vlraised = 0, vlwallguards = 0, vlsight = 0, vlmaxsight = 0, vlstraight = 0;
	int nlayout[LG_LAYOUT_COUNT], naesth[LG_AESTH_ALL], themebad = 0;

	memset(nlayout, 0, sizeof(nlayout));
	memset(naesth, 0, sizeof(naesth));
	modLevelGenForceTheme(layout, -1);

	for (size = 0; size < LG_SIZE_COUNT; size++) {
		int sfail = 0;

		for (seed = 1; seed <= numseeds; seed++) {
			LgParams p;
			int i;

			p.seed = (uint32_t)seed * 2654435761u;
			p.size = (uint8_t)size;

			if (!modLevelGenBuild(&a, &p, err, sizeof(err))) {
				snprintf(line, sizeof(line), "SELFTEST FAIL %s-layout: size %d seed %u: %s",
						prefix[layout], size, p.seed, err);
				print(line);
				sfail++;
				continue;
			}
			if (layout == LG_LAYOUT_PIPEWORKS) {
				/* the pipeworks' (2026-11-19): the sludge a kill floor, two pipes
				 * at least over it (and one between them), catwalks low and high
				 * (the high crossing over the low), drops off them, an automatic
				 * lift, four ladders, the four rooms */
				int pit = 0, i3, l2 = 0;

				for (i3 = 0; i3 < a.numareas; i3++) {
					pit += a.areas[i3].special == LG_SPECIAL_PIT;
					l2 |= a.areas[i3].layer == 2;
				}
				pwok += pit > 0 && l2 && g_LgPw.pipes >= 2 && g_LgPw.crosses >= 1 && g_LgPw.lowcats >= 1 && g_LgPw.highcats >= 2
					&& a.numlifts == 1 && a.lifts[0].nstops == 2 && a.numladders == 4 && a.numrooms == 5 && g_LgPw.drops >= 10 && g_LgPw.gaps >= 2;
				pwpipes += g_LgPw.pipes + g_LgPw.crosses;
				pwcats += g_LgPw.lowcats + g_LgPw.highcats;
				pwdrops += g_LgPw.drops;
				pwgaps += g_LgPw.gaps;
				pwfurn += g_LgPw.furniture;
			} else if (layout == LG_LAYOUT_MALL) {
				/* a mall's (2026-11-19): two floors, the lift stopping at both,
				 * the escalators, glass shop fronts, the hallways behind the
				 * shops (a passage to each quadrant's on each floor), the
				 * washroom with its stalls, furniture */
				int l3 = 0, i3;

				for (i3 = 0; i3 < a.numareas; i3++) {
					l3 |= 1 << a.areas[i3].layer;
				}
				mlok += a.numlifts == 1 && a.lifts[0].nstops == 2 && l3 == 3 && g_LgMl.ramps == 2 && g_LgMl.windows >= 16
					&& g_LgMl.stores >= 16 && g_LgMl.passages >= 8 && g_LgMl.bathdone && g_LgMl.stalls >= 2 && g_LgMl.toilets >= 2 && g_LgMl.furniture >= 20;
				mlstores += g_LgMl.stores;
				mlwins += g_LgMl.windows;
				mlback += g_LgMl.backdoors;
				mlfurn += g_LgMl.furniture;
				mlstalls += g_LgMl.stalls;
			} else if (LG_LAYOUT_HIGHRISE(layout)) {
				int l3 = 0, i3;

				for (i3 = 0; i3 < a.numareas; i3++) {
					l3 |= 1 << a.areas[i3].layer;
				}
				hrok += a.numlifts == 1 && a.lifts[0].nstops == 3 && l3 == 7 && g_LgHr.windows >= 12 && (layout == LG_LAYOUT_SITE || (g_LgHr.furniture >= 10 && g_LgHr.doors >= 1
						&& g_LgHr.nrooms[0] >= 2 && g_LgHr.nrooms[0] <= 4 && g_LgHr.nrooms[1] >= 2 && g_LgHr.nrooms[1] <= 4 && g_LgHr.nrooms[2] >= 2 && g_LgHr.nrooms[2] <= 4));
				hrwins += g_LgHr.windows;
				hrfurn += g_LgHr.furniture;
				hrvoids += g_LgHr.nvoid;
				hrdoors += g_LgHr.doors;
			}

			/* a ravine's: a pit, bridges, the catwalk, ground up high (2026-11-15) */
			/* the rooftops': a tower of three storeys (layer 2) with windows on
			 * them, glass floors, bridges, huts with glass windows, roofs at
			 * different heights (2026-11-16) */
			if (layout == LG_LAYOUT_ROOFTOP) {
				int l2 = 0, glass = 0, br = 0, huts = 0, wins = 0, upwins = 0, hmin = 99999, hmax = -1;

				for (i = 0; i < a.numareas; i++) {
					const LgArea *ar = &a.areas[i];

					l2 += ar->layer == 2 && !ar->special;
					glass += ar->special == LG_SPECIAL_GLASS;
					br += ar->special == LG_SPECIAL_BRIDGE;
					huts += ar->kind == LG_AREA_ROOM && ar->room == i && !ar->open && ar->theme != 0;
					wins += ar->special == LG_SPECIAL_WINDOW;
					upwins += ar->special == LG_SPECIAL_WINDOW && ar->layer;
					if (ar->open && !ar->layer && !ar->special && !ar->ramp && ar->floor > 0) {
						hmin = ar->floor < hmin ? ar->floor : hmin;
						hmax = ar->floor > hmax ? ar->floor : hmax;
					}
				}
				rtok += l2 >= 5 && glass > 0 && br > 0 && wins >= 4 && upwins >= 3 && hmax > hmin;
				rttowers += l2 / 5;
				rtglass += glass;
				rtbridges += br;
				rthuts += huts;
				rtwins += wins;
				rtupwins += upwins;
				rtlevels += hmax > hmin;
			}
			/* a villa's: every building a roof of two slopes and a landing up to
			 * it, the water, the piers, windows (2026-11-15) */
			if (layout == LG_LAYOUT_VILLA) {
				int roofs = 0, landings = 0, water = 0, piers = 0, wins = 0;

				for (i = 0; i < a.numareas; i++) {
					const LgArea *ar = &a.areas[i];

					roofs += ar->special == LG_SPECIAL_ROOF;
					water += ar->special == LG_SPECIAL_PIT;
					piers += ar->theme == LG_DOCK_THEME;
					wins += ar->special == LG_SPECIAL_WINDOW;
					if (!ar->layer && !ar->ramp && (ar->floor == LG_VL_EAVE || ar->floor == LG_VL_EAVE2)) {
						int q;

						for (q = 0; q < a.numportals; q++) {
							const LgPortal *pt = &a.portals[q];
							const int o = pt->a == i ? pt->b : pt->b == i ? pt->a : -1;

							if (o >= 0 && !pt->drop && a.areas[o].special == LG_SPECIAL_ROOF) {
								landings++;
								break;
							}
						}
					}
				}
				/* (2026-11-15, the redesign) the house's share of the grounds, a
				 * raised wing and its terrace, guards on the walls' tops, the
				 * longest straight view at ground level outdoors (cells) and the
				 * longest corridor run */
				{
					int foot = 0, raised = 0, terrace = 0, wallg = 0, sight = 0, straight = 0, q, x, z;
					const int grounds = (a.gw - 2) * (a.gd - 2 - LG_VL_WATER);

					for (i = 0; i < a.numareas; i++) {
						const LgArea *ar = &a.areas[i];

						foot += ar->special == LG_SPECIAL_ROOF ? ar->w * ar->d : 0;
						raised |= ar->kind == LG_AREA_ROOM && ar->room == i && ar->floor == LG_VL_UPPER && !ar->open;
						terrace |= ar->open && !ar->layer && ar->floor == LG_VL_UPPER && ar->theme == LG_VL_TERRACE_THEME && !ar->ramp;
						if (ar->kind == LG_AREA_CORRIDOR && !ar->special && !ar->open) {
							const int run = ar->w > ar->d ? ar->w : ar->d;

							straight = run > straight ? run : straight;
						}
					}
					for (q = 0; q < a.numpads; q++) {
						const LgArea *ar = &a.areas[a.pads[q].area];

						wallg += a.pads[q].kind == LG_PAD_GUARD && ar->open && !ar->ramp && ar->floor == LG_VL_WALLTOP && ar->theme == LG_VL_WALL_THEME;
					}
					/* along each row and column of the ground-level outdoors: a run
					 * of cells each open into the next, none a step up of more than
					 * a crouch (a wall's top, a terrace, a block) */
					for (q = 0; q < 2; q++) {
						for (z = 0; z < a.gd; z++) {
							int run = 0;

							for (x = 0; x < a.gw; x++) {
								const int cx = q ? z : x, cz = q ? x : z;
								const int ci = a.cells[cz][cx] - 1;
								const LgArea *ar = ci >= 0 ? &a.areas[ci] : NULL;
								const int ground = ar && ar->open && !ar->layer && ar->floorlo <= LG_VL_FLOOR && ar->floor <= LG_VL_FLOOR + 100 && !LG_NOWALK(ar);
								int joined = 0;

								if (ground && run > 0) {
									const int px = q ? cx : cx - 1, pz = q ? cz - 1 : cz;

									joined = a.cells[pz][px] == a.cells[cz][cx] || (a.open[pz][px] & (q ? LG_OPEN_ZP : LG_OPEN_XP));
								}
								run = ground ? (joined ? run + 1 : 1) : 0;
								sight = run > sight ? run : sight;
							}
						}
					}
					vlshare += foot * 100 / grounds;
					vlminshare = foot * 100 / grounds < vlminshare ? foot * 100 / grounds : vlminshare;
					vlraised += raised && terrace;
					vlwallguards += wallg;
					vlsight += sight;
					vlmaxsight = sight > vlmaxsight ? sight : vlmaxsight;
					vlstraight = straight > vlstraight ? straight : vlstraight;
					vlok += roofs >= 6 && roofs % 2 == 0 && landings >= roofs / 2 && water > 0 && piers > 0 && wins >= 8
						&& raised && terrace && wallg > 0 && foot * 100 / grounds >= 40 && straight <= LG_VL_SEGMAX + 2;
				}
				vlrooms += a.numrooms - 1;
				vlwins += wins;
				vlbld += roofs / 2;
				vlpiers += piers;
			}
			if (layout == LG_LAYOUT_RAVINE) {
				int nbr = 0, cat = 0, pit = 0, high = 0;

				for (i = 0; i < a.numareas; i++) {
					const LgArea *ar = &a.areas[i];

					pit += ar->special == LG_SPECIAL_PIT;
					nbr += ar->special == LG_SPECIAL_BRIDGE && ar->floor > LG_RV_BASE;
					cat |= ar->special == LG_SPECIAL_BRIDGE && ar->floor == LG_RV_BASE;
					high |= !ar->layer && ar->floor >= LG_RV_BASE + 4 * LG_LEVEL;
				}
				rvpit += pit == 3;   /* the L: two legs and the corner */
				rvbridges += nbr;
				rvwithbridge += nbr > 0 || cat;
				rvcatwalk += cat;
				rvhigh += high;
				rvhuts += a.numrooms - 1;
			}

			/* the themes: the ones the seed and size give, counted */
			{
				const LgTheme t = modLevelGenPickTheme(p.seed, size);
				char name[64], want[64];

				if (t.layout != a.theme.layout || t.aesthetic != a.theme.aesthetic
						|| a.theme.layout >= LG_LAYOUT_COUNT || a.theme.aesthetic >= LG_AESTH_ALL) {
					themebad++;
				} else {
					nlayout[a.theme.layout]++;
					naesth[a.theme.aesthetic]++;
					modLevelGenLevelName(a.theme, name, sizeof(name));
					snprintf(want, sizeof(want), "(PR) %s, %s", g_LgLayouts[a.theme.layout].name, g_LgAestheticNames[a.theme.aesthetic]);
					themebad += strcmp(name, want) != 0 || strlen(name) < 9 || strncmp(name, "(PR) ", 5) != 0 || !strstr(name, ", ");
				}
			}

			if (a.numareas > maxareas) maxareas = a.numareas;
			if (a.numportals > maxportals) maxportals = a.numportals;
			if (a.numpads > maxpads) maxpads = a.numpads;
			if (a.attempt > 0) retries++;
			for (i = 0; i < a.numpads; i++) {
				if (a.pads[i].numneigh > maxneigh) maxneigh = a.pads[i].numneigh;
			}
			/* the hub: loops in the area graph, rooms off the ground, ledges */
			layouts++;
			loops += a.numportals - a.numareas + 1;
			{
				int high = 0;

				for (i = 0; i < a.numrooms; i++) upper += a.areas[i].floor > 0;
				for (i = 0; i < a.numareas; i++) {
					if ((a.areas[i].flags & LG_AREAFLAG_HUB) && a.areas[i].floor > high) high = a.areas[i].floor;
				}
				for (i = 0; i < a.numportals; i++) ledges += a.portals[i].drop;
				/* side rooms that climb (a terrace of their own), low cover */
				{
					uint8_t has[LG_MAXROOMS];

					memset(has, 0, sizeof(has));
					for (i = 0; i < a.numareas; i++) {
						if (a.areas[i].flags & LG_AREAFLAG_TERRACE) has[a.areas[i].room] = 1;
					}
					for (i = 0; i < a.numrooms - 1; i++) terraced += has[i];
					sides += a.numrooms - 1;
				}
				for (i = 0; i < a.numobst; i++) {
					if (a.obst[i].height > 0) {
						cover++;
						parapets += a.obst[i].halfx == 10 || a.obst[i].halfz == 10;
						hubcover += (a.areas[a.obst[i].area].flags & LG_AREAFLAG_HUB) != 0;
					}
				}
				flat += high < (size == LG_SIZE_SMALL ? 1 : 2) * LG_LEVEL;
			}

			{
				int nm = 0, q;

				for (q = 0; q < a.numareas; q++) nm += (a.areas[q].flags & LG_AREAFLAG_MEZZ) != 0;
				mezzlayouts += nm > 0;
			}
			/* room shapes: wings are room areas past the mains */
			rooms += a.numrooms;
			{
				uint8_t has[LG_MAXROOMS];

				memset(has, 0, sizeof(has));
				for (i = a.numrooms; i < a.numareas; i++) {
					mezz += (a.areas[i].flags & LG_AREAFLAG_MEZZ) != 0;
					raised += a.areas[i].kind == LG_AREA_ROOM && !(a.areas[i].flags & (LG_AREAFLAG_HUB | LG_AREAFLAG_TERRACE)) && a.areas[i].ramp != 0;
					if (a.areas[i].kind == LG_AREA_ROOM && !a.areas[i].floor) {
						wings++;
						has[a.areas[i].room] = 1;
					}
				}
				for (i = 0; i < a.numrooms; i++) shaped += has[i];
				for (i = 0; i < a.numportals; i++) {
					const LgPortal *q = &a.portals[i];

					if ((a.areas[q->a].flags | a.areas[q->b].flags) & LG_AREAFLAG_MEZZ) {
						mezzdrops += q->norail;
						mezzramps += !q->drop;
					}
				}
			}

			/* the lab: loops, floor heights in bands of 100, hall levels, the
			 * longest straight hall, dead ends, cover in the rooms */
			if (layout != LG_LAYOUT_HUB) {
				uint8_t band[64], lev[8];
				int nb = 0, nlev = 0, q, hall, ncov[LG_MAXROOMS];
				const int lp = a.numportals - a.numareas + 1;

				memset(band, 0, sizeof(band));
				memset(lev, 0, sizeof(lev));
				memset(ncov, 0, sizeof(ncov));
				for (q = 0; q < a.numareas; q++) {
					int np = 0, k2;

					if (a.areas[q].floor / 100 < 64) band[a.areas[q].floor / 100] = 1;
					if (a.areas[q].kind == LG_AREA_CORRIDOR && !a.areas[q].ramp && a.areas[q].floor / LG_LEVEL < 8) lev[a.areas[q].floor / LG_LEVEL] = 1;
					for (k2 = 0; k2 < a.numportals; k2++) {
						np += (a.portals[k2].a == q || a.portals[k2].b == q) && !a.portals[k2].drop;
					}
					labdead += np == 1;
				}
				for (q = 0; q < 64; q++) nb += band[q];
				for (q = 0; q < 8; q++) nlev += lev[q];
				labbands += nb;
				if (nb < labbandsmin) labbandsmin = nb;
				if (lp < labloopsmin) labloopsmin = lp;
				lablevels += nlev >= 2;
				stopen += layout == LG_LAYOUT_STREET && a.areas[a.numrooms - 1].open && (a.areas[a.numrooms - 1].flags & LG_AREAFLAG_HUB);
				{
					int nb = 0;

					for (q = 0; q < a.numareas; q++) {
						stwindows += a.areas[q].special == LG_SPECIAL_WINDOW;
						nb += a.areas[q].special == LG_SPECIAL_BALCONY;
					}
					/* bridges, and the upper floors with a second way up or down: a
					 * drop-down of their own, or a balcony (which has one, and may
					 * have a bridge to another building's upper floor) */
					{
						int nbr = 0, z2;

						for (z2 = 0; z2 < a.numareas; z2++) {
							nbr += a.areas[z2].special == LG_SPECIAL_BRIDGE;
							if (a.areas[z2].flags & LG_AREAFLAG_MEZZ) {
								int q2, two = 0, bridged = 0;

								for (q2 = 0; q2 < a.numportals; q2++) {
									const LgPortal *pt = &a.portals[q2];
									const int o = pt->a == z2 ? pt->b : pt->b == z2 ? pt->a : -1;

									if (o < 0) {
										continue;
									}
									two |= pt->drop && pt->norail;
									if (!pt->drop && a.areas[o].special == LG_SPECIAL_BALCONY) {
										int q3;

										two = 1;
										for (q3 = 0; q3 < a.numportals; q3++) {
											const LgPortal *pb = &a.portals[q3];

											bridged |= (pb->a == o || pb->b == o) && !pb->drop
												&& a.areas[pb->a == o ? pb->b : pb->a].special == LG_SPECIAL_BRIDGE;
										}
									}
								}
								stupper++;
								stupper2 += two;
								stupperbr += bridged;
							}
						}
						stbridges += nbr;
						stwithbridge += nbr > 0;
					}
					for (q = 0; q < a.numportals; q++) {
						const LgPortal *pt = &a.portals[q];
						const int bal = a.areas[pt->a].special == LG_SPECIAL_BALCONY ? pt->a : a.areas[pt->b].special == LG_SPECIAL_BALCONY ? pt->b : -1;

						if (bal >= 0 && pt->drop && pt->norail) {
							int o2, rails = 0;

							stbaldrops++;
							for (o2 = 0; o2 < a.numobst; o2++) {
								rails += a.obst[o2].area == bal && a.obst[o2].rail;
							}
							stbalrailed += rails > 0;
						}
					}
					/* (2026-11-15) upper-storey windows, balconies out over the street, doors onto balconies */
					for (q = 0; q < a.numareas; q++) {
						const LgArea *ar = &a.areas[q];

						stupwin += ar->special == LG_SPECIAL_WINDOW && ar->layer;
						if (ar->special == LG_SPECIAL_BALCONY) {
							int cx, cz, out = 0;

							for (cz = ar->z; cz < ar->z + ar->d; cz++) {
								for (cx = ar->x; cx < ar->x + ar->w; cx++) {
									out |= a.cells[cz][cx] > 0 && (a.areas[a.cells[cz][cx] - 1].flags & LG_AREAFLAG_HUB);
								}
							}
							stbalout += out;
						}
					}
					for (q = 0; q < a.numportals; q++) {
						const LgPortal *pt = &a.portals[q];

						stbaldoor += pt->door && !pt->drop
							&& (a.areas[pt->a].special == LG_SPECIAL_BALCONY || a.areas[pt->b].special == LG_SPECIAL_BALCONY);
					}
					stbalconies += nb;
					stnobalcony += layout == LG_LAYOUT_STREET && nb == 0;
				}
				{
					int had = 0;

					for (q = 0; q < a.numobst; q++) {
						had += a.obst[q].crate && a.obst[q].model >= LG_CRATE_MODELS;
					}
					stcars += had;
					stnocars += layout == LG_LAYOUT_STREET && had == 0;
				}
				for (q = 0; q < a.numrooms - 1; q++) {
					stbuildings += layout == LG_LAYOUT_STREET && !a.areas[q].open;
					styards += layout == LG_LAYOUT_STREET && a.areas[q].open;
				}
				hall = lgLongestHall(&a);
				labhallsum += hall;
				if (hall > labhall) labhall = hall;
				for (q = 0; q < a.numobst; q++) {
					if (a.obst[q].height > 0 && !a.obst[q].rail && a.areas[a.obst[q].area].room >= 0) ncov[a.areas[a.obst[q].area].room]++;
				}
				for (q = 1; q < a.numrooms; q++) {
					if (a.areas[q].w >= 6 && a.areas[q].d >= 6) {
						labrooms++;
						labcovered += ncov[q] >= 2;
					}
				}
			}

			/* same seed, same bytes */
			modLevelGenBuild(&b, &p, err, sizeof(err));
			if (memcmp(&a, &b, sizeof(a)) != 0) {
				snprintf(line, sizeof(line), "SELFTEST FAIL %s-determinism: size %d seed %u",
						prefix[layout], size, p.seed);
				print(line);
				sfail++;
				continue;
			}

			/* next seed, different level */
			p.seed++;
			modLevelGenBuild(&b, &p, err, sizeof(err));
			if (memcmp(a.cells, b.cells, sizeof(a.cells)) == 0) {
				snprintf(line, sizeof(line), "SELFTEST FAIL %s-variety: size %d seed %u",
						prefix[layout], size, p.seed - 1);
				print(line);
				sfail++;
				continue;
			}
		}

		snprintf(line, sizeof(line), "SELFTEST %s %s-size%d: %d seeds, %d failed",
				sfail ? "FAIL" : "PASS", prefix[layout], size, numseeds, sfail);
		print(line);

		if (sfail) {
			failed++;
		} else {
			passed++;
		}
	}

	snprintf(line, sizeof(line),
			"%s stats: max areas %d/%d, portals %d/%d, pads %d/%d, links %d/%d, retried %d",
			prefix[layout], maxareas, LG_MAXAREAS, maxportals, LG_MAXPORTALS, maxpads, LG_MAXPADS,
			maxneigh, LG_MAXNEIGH, retries);
	print(line);

	if (layout == LG_LAYOUT_STREET) {
		/* the street's promises: the streets themselves (one room, outdoors)
		 * in every layout, buildings along them, loops (the two streets'
		 * ring and the middle row's doors through), floors on three bands or
		 * more, two levels (the hill), cover in the buildings */
		const int ok = layouts > 0 && stopen == layouts && stbuildings >= 4 * layouts && labloopsmin >= 5 && labbandsmin >= 3
			&& lablevels == layouts && labcovered * 10 >= labrooms * 8 && stnocars * 5 <= layouts
			&& stwindows >= 4 * layouts && stnobalcony * 10 <= layouts;   /* windows in every street; a balcony in nine of ten or more */
		char more[200];

		const int frontsok = stwindows >= 4 * layouts && stnobalcony * 10 <= layouts && stbaldrops >= stbalconies
			&& stbalrailed + 2 * stbridges >= stbalconies && stwithbridge == layouts   /* every street has a bridge */
			&& stupwin * 2 >= stwindows / 2 && stbalout * 2 >= stbalconies && stbaldoor == stbalconies;   /* (2026-11-15) */

		snprintf(more, sizeof(more), "SELFTEST %s levelgen-street-fronts: %d windows, %d balconies (%d streets with none), %d with a drop-down, %d with a railing",
				frontsok ? "PASS" : "FAIL", stwindows, stbalconies, stnobalcony, stbaldrops, stbalrailed);
		print(more);
		snprintf(more, sizeof(more), "levelgen-street-upper: %d of the windows on the upper storey, %d balconies out over the street, %d with a door",
				stupwin, stbalout, stbaldoor);
		print(more);
		snprintf(more, sizeof(more), "levelgen-street-bridges: %d bridges in %d of %d streets; %d of %d upper floors with a second way (a drop-down or a balcony), %d with a bridge",
				stbridges, stwithbridge, layouts, stupper2, stupper, stupperbr);
		print(more);
		if (frontsok) {
			passed++;
		} else {
			failed++;
		}   /* cars in four streets of five or more (a short piece crowded with doorways has no kerb free) */

		snprintf(line, sizeof(line), "SELFTEST %s levelgen-street: %d layouts, %d with the street outdoors, %d.%d buildings and %d.%d yards each, %d.%d loops each (fewest %d), "
				"%d.%d floor bands each (fewest %d), %d on two levels, %d of %d rooms with two or more pieces of cover, %d cars (%d streets with none)",
				ok ? "PASS" : "FAIL", layouts, stopen, layouts ? stbuildings / layouts : 0, layouts ? stbuildings * 10 / layouts % 10 : 0,
				layouts ? styards / layouts : 0, layouts ? styards * 10 / layouts % 10 : 0,
				layouts ? loops / layouts : 0, layouts ? loops * 10 / layouts % 10 : 0, labloopsmin,
				layouts ? labbands / layouts : 0, layouts ? labbands * 10 / layouts % 10 : 0, labbandsmin, lablevels, labcovered, labrooms, stcars, stnocars);
		print(line);
		if (ok) {
			passed++;
		} else {
			failed++;
		}
	} else if (layout == LG_LAYOUT_RAVINE) {
		const int ok = layouts > 0 && rvpit == layouts && rvwithbridge == layouts;

		snprintf(line, sizeof(line), "SELFTEST %s levelgen-ravine: %d layouts, %d with the L-shaped pit, %d with a way across (%d high or mid bridges, %d with the low catwalk), "
				"%d with ground at the top height, %d.%d huts each, %d.%d loops each",
				ok ? "PASS" : "FAIL", layouts, rvpit, rvwithbridge, rvbridges, rvcatwalk, rvhigh,
				layouts ? rvhuts / layouts : 0, layouts ? rvhuts * 10 / layouts % 10 : 0, layouts ? loops / layouts : 0, layouts ? loops * 10 / layouts % 10 : 0);
		print(line);
		if (ok) {
			passed++;
		} else {
			failed++;
		}
	} else if (layout == LG_LAYOUT_PIPEWORKS) {
		const int ok = layouts > 0 && pwok == layouts;

		snprintf(line, sizeof(line), "SELFTEST %s %s: %d layouts, %d with the sludge, pipes over it, catwalks low and high, the lift, four ladders and the rooms; "
				"%d.%d pipes, %d.%d catwalks, %d.%d drops, %d.%d gaps in the rails, %d.%d pieces of furniture each",
				ok ? "PASS" : "FAIL", prefix[layout], layouts, pwok,
				layouts ? pwpipes / layouts : 0, layouts ? pwpipes * 10 / layouts % 10 : 0, layouts ? pwcats / layouts : 0, layouts ? pwcats * 10 / layouts % 10 : 0,
				layouts ? pwdrops / layouts : 0, layouts ? pwdrops * 10 / layouts % 10 : 0, layouts ? pwgaps / layouts : 0, layouts ? pwgaps * 10 / layouts % 10 : 0,
				layouts ? pwfurn / layouts : 0, layouts ? pwfurn * 10 / layouts % 10 : 0);
		print(line);
		if (ok) {
			passed++;
		} else {
			failed++;
		}
	} else if (layout == LG_LAYOUT_MALL) {
		const int ok = layouts > 0 && mlok == layouts;

		snprintf(line, sizeof(line), "SELFTEST %s %s: %d layouts, %d with two floors round the plus, the escalators, a lift stopping at both, the shops' hallways and the washroom; "
				"%d.%d shops, %d.%d panes of glass, %d.%d back doors, %d.%d pieces of furniture, %d.%d stalls each",
				ok ? "PASS" : "FAIL", prefix[layout], layouts, mlok,
				layouts ? mlstores / layouts : 0, layouts ? mlstores * 10 / layouts % 10 : 0, layouts ? mlwins / layouts : 0, layouts ? mlwins * 10 / layouts % 10 : 0,
				layouts ? mlback / layouts : 0, layouts ? mlback * 10 / layouts % 10 : 0, layouts ? mlfurn / layouts : 0, layouts ? mlfurn * 10 / layouts % 10 : 0,
				layouts ? mlstalls / layouts : 0, layouts ? mlstalls * 10 / layouts % 10 : 0);
		print(line);
		if (ok) {
			passed++;
		} else {
			failed++;
		}
	} else if (LG_LAYOUT_HIGHRISE(layout)) {
		const int ok = layouts > 0 && hrok == layouts;

		snprintf(line, sizeof(line), "SELFTEST %s %s: %d layouts, %d with three storeys, a lift stopping at each, glass (or open) outside walls%s; "
				"%d.%d windows, %d.%d pieces of furniture, %d.%d doors, %d.%d voids each",
				ok ? "PASS" : "FAIL", prefix[layout], layouts, hrok, layout == LG_LAYOUT_SITE ? "" : ", furniture, a terrace and 2 to 4 rooms a storey",
				layouts ? hrwins / layouts : 0, layouts ? hrwins * 10 / layouts % 10 : 0, layouts ? hrfurn / layouts : 0, layouts ? hrfurn * 10 / layouts % 10 : 0,
				layouts ? hrdoors / layouts : 0, layouts ? hrdoors * 10 / layouts % 10 : 0, layouts ? hrvoids / layouts : 0, layouts ? hrvoids * 10 / layouts % 10 : 0);
		print(line);
		if (ok) {
			passed++;
		} else {
			failed++;
		}
	} else if (LG_LAYOUT_FLATBUILT(layout)) {
		/* (Docks, Backwoods: built from the villa's pieces; every seed built is
		 * checked whole by the validator, its sizes above) */
		snprintf(line, sizeof(line), "SELFTEST %s %s: %d layouts built", layouts > 0 ? "PASS" : "FAIL", prefix[layout], layouts);
		print(line);
		if (layouts > 0) {
			passed++;
		} else {
			failed++;
		}
	} else if (layout == LG_LAYOUT_VILLA) {
		const int ok = layouts > 0 && vlok == layouts;

		snprintf(line, sizeof(line), "SELFTEST %s levelgen-villa: %d layouts, %d with roofs, landings, water, piers and windows; "
				"%d.%d buildings, %d.%d rooms, %d.%d windows, %d.%d piers each, %d.%d loops each; the house %d%% of the grounds (least %d%%), "
				"%d with a raised wing and terrace, %d.%d guards on the walls each, the longest corridor run %d, the longest view outdoors at ground level %d cells (%d.%d average)",
				ok ? "PASS" : "FAIL", layouts, vlok, layouts ? vlbld / layouts : 0, layouts ? vlbld * 10 / layouts % 10 : 0,
				layouts ? vlrooms / layouts : 0, layouts ? vlrooms * 10 / layouts % 10 : 0,
				layouts ? vlwins / layouts : 0, layouts ? vlwins * 10 / layouts % 10 : 0,
				layouts ? vlpiers / layouts : 0, layouts ? vlpiers * 10 / layouts % 10 : 0,
				layouts ? loops / layouts : 0, layouts ? loops * 10 / layouts % 10 : 0,
				layouts ? vlshare / layouts : 0, vlminshare, vlraised, layouts ? vlwallguards / layouts : 0, layouts ? vlwallguards * 10 / layouts % 10 : 0,
				vlstraight, vlmaxsight, layouts ? vlsight / layouts : 0, layouts ? vlsight * 10 / layouts % 10 : 0);
		print(line);
		if (ok) {
			passed++;
		} else {
			failed++;
		}
	} else if (layout == LG_LAYOUT_ROOFTOP) {
		const int ok = layouts > 0 && rtok == layouts;

		snprintf(line, sizeof(line), "SELFTEST %s levelgen-rooftop: %d layouts, %d with a tower of three storeys, glass floors, bridges, glass windows (on every storey) and roofs at different heights; "
				"%d.%d towers, %d.%d glass floors, %d.%d bridges, %d.%d huts, %d.%d windows (%d.%d up the towers) each",
				ok ? "PASS" : "FAIL", layouts, rtok, layouts ? rttowers / layouts : 0, layouts ? rttowers * 10 / layouts % 10 : 0,
				layouts ? rtglass / layouts : 0, layouts ? rtglass * 10 / layouts % 10 : 0, layouts ? rtbridges / layouts : 0, layouts ? rtbridges * 10 / layouts % 10 : 0,
				layouts ? rthuts / layouts : 0, layouts ? rthuts * 10 / layouts % 10 : 0, layouts ? rtwins / layouts : 0, layouts ? rtwins * 10 / layouts % 10 : 0,
				layouts ? rtupwins / layouts : 0, layouts ? rtupwins * 10 / layouts % 10 : 0);
		print(line);
		if (ok) {
			passed++;
		} else {
			failed++;
		}
		(void)rtlevels;
	} else if (layout == LG_LAYOUT_LAB) {
		/* the lab's promises (the Combat Simulator maps: 9-27 loops, floors in
		 * 3-12 bands): loops, floors on three bands or more, halls on two
		 * levels, cover in its rooms; and what its halls are like */
		const int ok = layouts > 0 && labloopsmin >= 5 && labbandsmin >= 3 && lablevels == layouts && labcovered * 10 >= labrooms * 8 && labhall <= 24;

		snprintf(line, sizeof(line), "SELFTEST %s levelgen-lab: %d layouts, %d.%d loops each (fewest %d), %d.%d floor bands each (fewest %d), %d with halls on two levels, "
				"%d of %d rooms with two or more pieces of cover, longest straight hall with no door across %d cells (average %d), %d.%d dead ends each",
				ok ? "PASS" : "FAIL", layouts, layouts ? loops / layouts : 0, layouts ? loops * 10 / layouts % 10 : 0, labloopsmin,
				layouts ? labbands / layouts : 0, layouts ? labbands * 10 / layouts % 10 : 0, labbandsmin, lablevels,
				labcovered, labrooms, labhall, layouts ? labhallsum / layouts : 0, layouts ? labdead / layouts : 0, layouts ? labdead * 10 / layouts % 10 : 0);
		print(line);
		if (ok) {
			passed++;
		} else {
			failed++;
		}
	} else {
	/* some rooms come out L, T, U or S shaped (wings go in round the ring's
	 * passages, so fewer than before the ring: one in twelve at least) */
	snprintf(line, sizeof(line), "SELFTEST %s levelgen-shapes: %d of %d rooms shaped (%d wings)",
			shaped * 12 >= rooms && shaped * 5 <= rooms * 3 ? "PASS" : "FAIL", shaped, rooms, wings);
	print(line);
	if (shaped * 12 >= rooms && shaped * 5 <= rooms * 3) {
		passed++;
	} else {
		failed++;
	}

	/* every hub climbs and has balconies, every level loops (the Combat
	 * Simulator maps: 9-27 loops, every one on several floors) */
	snprintf(line, sizeof(line), "SELFTEST %s levelgen-hub: %d layouts, %d.%d loops each, %d ledges, %d of %d rooms off the ground, %d flat hubs",
			flat == 0 && loops >= 2 * layouts && ledges >= layouts ? "PASS" : "FAIL", layouts, layouts ? loops / layouts : 0,
			layouts ? loops * 10 / layouts % 10 : 0, ledges, upper, rooms, flat);
	print(line);
	if (flat == 0 && loops >= 2 * layouts && ledges >= layouts) {
		passed++;
	} else {
		failed++;
	}

	/* side rooms climb too, and the hub has cover (2026-11-14) */
	snprintf(line, sizeof(line), "SELFTEST %s levelgen-cover: %d of %d side rooms with a terrace, %d pieces of low cover (%d in the hub, %d parapets)",
			terraced * 3 >= sides && hubcover >= 4 * layouts && parapets >= layouts ? "PASS" : "FAIL", terraced, sides, cover, hubcover, parapets);
	print(line);
	if (terraced * 3 >= sides && hubcover >= 4 * layouts && parapets >= layouts) {
		passed++;
	} else {
		failed++;
	}

	}

	/* mezzanines: upper floors over halls and labs, climbed by a ramp, with a drop-down */
	if (layout != LG_LAYOUT_RAVINE && layout != LG_LAYOUT_PIPEWORKS && !LG_LAYOUT_SKYLIKE(layout) && layout != LG_LAYOUT_VILLA && !LG_LAYOUT_FLATBUILT(layout)) {
		const int ok = mezzlayouts == layouts && mezzdrops > 0 && mezzramps > 0;

		snprintf(line, sizeof(line), "SELFTEST %s %s-mezzanine: %d mezzanines, every one of %d layouts has one (%d do), %d ramps up, %d drop-downs", ok ? "PASS" : "FAIL", prefix[layout], mezz, layouts, mezzlayouts, mezzramps, mezzdrops);
		print(line);
		if (ok) {
			passed++;
		} else {
			failed++;
		}
	}

	/* and some of the deep wings of halls and labs raised as galleries */
	if (layout == LG_LAYOUT_HUB) {
		snprintf(line, sizeof(line), "SELFTEST %s levelgen-splitlevels: %d raised galleries", raised > 0 ? "PASS" : "FAIL", raised);
		print(line);
		if (raised > 0) {
			passed++;
		} else {
			failed++;
		}
	}

	if (themebad) {
		snprintf(line, sizeof(line), "SELFTEST FAIL %s-themes: %d layouts with the wrong themes or name", prefix[layout], themebad);
		print(line);
		failed++;
	}
	(void)nlayout;
	(void)naesth;
	modLevelGenForceTheme(-1, -1);
	*ppassed += passed;
	*pfailed += failed;
}

int modLevelGenSelfTest(int numseeds, void (*print)(const char *line))
{
	char line[1024];
	int passed = 0, failed = 0, layout, size, seed;
	int nlayout[LG_SIZE_COUNT][LG_LAYOUT_COUNT], naesth[LG_AESTH_ALL];

	for (layout = 0; layout < LG_LAYOUT_COUNT; layout++) {
		lgSelfTestLayout(layout, numseeds, print, &passed, &failed);
	}

	/* themes: what the seeds give when nothing is fixed. Every layout turns
	 * up at every size it has a weight for, every aesthetic turns up. */
	memset(nlayout, 0, sizeof(nlayout));
	memset(naesth, 0, sizeof(naesth));
	for (size = 0; size < LG_SIZE_COUNT; size++) {
		for (seed = 1; seed <= numseeds; seed++) {
			const LgTheme t = modLevelGenPickTheme((uint32_t)seed * 2654435761u, size);

			if (t.layout < LG_LAYOUT_COUNT && t.aesthetic < LG_AESTH_ALL) {
				nlayout[size][t.layout]++;
				naesth[t.aesthetic]++;
			}
		}
	}
	{
		int i, n, missing = 0;

		n = snprintf(line, sizeof(line), "SELFTEST PASS levelgen-themes: %d seeds a size;", numseeds);
		for (i = 0; i < LG_LAYOUT_COUNT; i++) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %s", g_LgLayouts[i].name);
			for (size = 0; size < LG_SIZE_COUNT; size++) {
				missing += nlayout[size][i] == 0 && g_LgLayouts[i].weight[size] > 0 && numseeds >= 50;
				n += snprintf(line + n, sizeof(line) - (size_t)n, "%s%d", size ? "/" : " ", nlayout[size][i]);
			}
			n += snprintf(line + n, sizeof(line) - (size_t)n, ";");
		}
		for (i = 0; i < LG_AESTH_ALL; i++) {
			int used = 0, l;

			for (l = 0; l < LG_LAYOUT_COUNT; l++) {
				used |= (g_LgLayouts[l].weight[0] + g_LgLayouts[l].weight[1] + g_LgLayouts[l].weight[2] > 0) ? (g_LgLayouts[l].aesthetics >> i) & 1 : 0;
			}
			/* (an aesthetic no layout takes any more is kept in the tables, not picked) */
			missing += used && naesth[i] == 0 && numseeds >= 50;
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %s %d", g_LgAestheticNames[i], naesth[i]);
		}
		snprintf(line + n, sizeof(line) - (size_t)n, "; %d never picked", missing);
		if (missing) {
			memcpy(line + 9, "FAIL", 4);
			failed++;
		} else {
			passed++;
		}
		print(line);
	}

	snprintf(line, sizeof(line), "SELFTEST RESULT levelgen passed %d failed %d", passed, failed);
	print(line);

	return failed;
}

/* ------------------------------------------------------------------ */
/* Standalone build                                                     */

#ifdef MODLEVELGEN_MAIN

static void lgPrintLine(const char *line)
{
	puts(line);
}

int main(int argc, char **argv)
{
	static LgLayout lg;
	static char map[(LG_GRIDMAX + 1) * LG_GRIDMAX + 1];
	LgParams p;
	char err[160];
	int i, selftest = 0;

	p.seed = 1;
	p.size = LG_SIZE_MEDIUM;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--selftest") == 0) {
			selftest = i + 1 < argc ? atoi(argv[++i]) : 200;
		} else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
			p.seed = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
			p.size = (uint8_t)atoi(argv[++i]);
		} else if (strcmp(argv[i], "--layout") == 0 && i + 1 < argc) {
			modLevelGenForceTheme(atoi(argv[++i]), -1);
		}
	}

	if (selftest > 0) {
		return modLevelGenSelfTest(selftest, lgPrintLine) ? 1 : 0;
	}

	if (!modLevelGenBuild(&lg, &p, err, sizeof(err))) {
		printf("build failed: %s\n", err);
		return 1;
	}

	modLevelGenDumpAscii(&lg, map, sizeof(map));
	fputs(map, stdout);
	{
		int doors = 0;
		for (i = 0; i < lg.numportals; i++) {
			doors += lg.portals[i].door;
		}
		selftest = doors; /* reused as the door count */
	}
	printf("seed %u size %d attempt %d: %d rooms, %d areas, %d portals (%d doors), %d pads\n",
			lg.seed, lg.size, lg.attempt, lg.numrooms, lg.numareas, lg.numportals,
			selftest, lg.numpads);
	printf("start room A, extraction room %c (%d hops), objectives:",
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef"[lg.areas[lg.extractarea].room % 32], lg.maxdist);
	for (i = 0; i < lg.numobj; i++) {
		printf(" %c", "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef"[lg.areas[lg.objareas[i]].room % 32]);
	}
	printf("\nkey: letters rooms, . corridors, # solid, @ start, g guard, o objective\n");
	return 0;
}

#endif
