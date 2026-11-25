/*
 * modlevelgenmap.c: Perfect Random level generator, top-down map pictures.
 * See modlevelgenmap.h. Plain C99, no engine code. Standalone:
 *   gcc -O1 -Wall -std=gnu99 -DMODLEVELGENMAP_MAIN -I src/include src/game/modlevelgen.c \
 *       src/game/modlevelgenfile.c src/game/modlevelgenmap.c -o lgmap && ./lgmap SEED [SIZE] [COUNT]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game/modlevelgen.h"
#include "game/modlevelgenfile.h"
#include "game/modlevelgenmap.h"

/* ------------------------------------------------------------------ */
/* Drawing                                                               */

static void imgPut(LgImage *im, int x, int y, uint32_t rgb)
{
	if (x >= 0 && y >= 0 && x < im->w && y < im->h) {
		uint8_t *p = im->px + 3 * ((size_t)y * im->w + x);

		p[0] = (uint8_t)(rgb >> 16);
		p[1] = (uint8_t)(rgb >> 8);
		p[2] = (uint8_t)rgb;
	}
}

static uint32_t imgGet(const LgImage *im, int x, int y)
{
	const uint8_t *p;

	if (x < 0 || y < 0 || x >= im->w || y >= im->h) {
		return 0xffffffffu;
	}
	p = im->px + 3 * ((size_t)y * im->w + x);
	return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static void imgFill(LgImage *im, int x0, int y0, int x1, int y1, uint32_t rgb)
{
	int x, y;

	for (y = y0; y < y1; y++) {
		for (x = x0; x < x1; x++) {
			imgPut(im, x, y, rgb);
		}
	}
}

static void imgBox(LgImage *im, int x0, int y0, int x1, int y1, int t, uint32_t rgb)
{
	imgFill(im, x0, y0, x1, y0 + t, rgb);
	imgFill(im, x0, y1 - t, x1, y1, rgb);
	imgFill(im, x0, y0, x0 + t, y1, rgb);
	imgFill(im, x1 - t, y0, x1, y1, rgb);
}

static void imgLine(LgImage *im, int x0, int y0, int x1, int y1, uint32_t rgb)
{
	const int dx = abs(x1 - x0), dy = -abs(y1 - y0);
	const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;

	for (;;) {
		int e2;

		imgPut(im, x0, y0, rgb);
		if (x0 == x1 && y0 == y1) {
			break;
		}
		e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

/* A 3x5 font: each glyph is five rows of three bits, one octal digit a row */
static const char *lgmGlyph(char c)
{
	static const char *letters[26] = {
		"25755", "65656", "34443", "65556", "74647", "74644", "34553", "55755", "72227", "11152",
		"55655", "44447", "57755", "65555", "25552", "65644", "25563", "65655", "34216", "72222",
		"55557", "55552", "55775", "55255", "55222", "71247",
	};
	static const char *digits[10] = {
		"75557", "26227", "61247", "61216", "55711", "74616", "34757", "71222", "75757", "75716",
	};

	if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
	if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
	if (c >= '0' && c <= '9') return digits[c - '0'];
	switch (c) {
	case ':': return "02020";
	case '-': return "00700";
	case '.': return "00002";
	case '/': return "11244";
	case '(': return "24442";
	case ')': return "21112";
	case '#': return "57575";
	case '%': return "51245";
	case '+': return "02720";
	case ',': return "00024";
	case '=': return "07070";
	case '>': return "42124";
	case '<': return "12421";
	default: return "00000";
	}
}

/* Writes text at (x, y), each font pixel scale x scale; returns the x after it */
static int imgText(LgImage *im, int x, int y, int scale, uint32_t rgb, const char *s)
{
	for (; *s; s++) {
		const char *g = lgmGlyph(*s);
		int row, col;

		for (row = 0; row < 5; row++) {
			const int bits = g[row] - '0';

			for (col = 0; col < 3; col++) {
				if (bits & (4 >> col)) {
					imgFill(im, x + col * scale, y + row * scale, x + (col + 1) * scale, y + (row + 1) * scale, rgb);
				}
			}
		}
		x += 4 * scale;
	}
	return x;
}

/* ------------------------------------------------------------------ */
/* The map                                                               */

static const uint32_t g_LgmThemeRgb[6] = {
	0x3f8fa3,   /* 0 lab */
	0xa88a5a,   /* 1 office */
	0x7a5fa0,   /* 2 hall */
	0x5a5a5a,   /* 3 corridor */
	0x8a6038,   /* 4 storage */
	0x4f6b4a,   /* 5 street: outdoors */
};
static const char *g_LgmThemeName[6] = { "LAB", "OFFICE", "HALL", "CORRIDOR", "STORAGE", "OUTDOORS" };
static const char *g_LgmSizeName[LG_SIZE_COUNT] = { "SMALL", "MEDIUM", "LARGE" };

#define LGM_GRID   0x262626
#define LGM_NAV    0xb0b0b0
#define LGM_LINK   0x3c5c3c
#define LGM_LIGHT  0xfff2a0
#define LGM_TEXT   0xdddddd
#define LGM_LEDGE  0xffe060
#define LGM_DIM    0x8a8a8a

static uint32_t lgmAreaRgb(const LgLayout *lg, int ai)
{
	const int t = lg->areas[ai].theme;

	return g_LgmThemeRgb[t >= 0 && t < 6 ? t : 1];
}

typedef struct {
	const LgLayout *lg;
	LgImage *im;
	int s;              /* pixels a cell */
	int mx, my;         /* the map's top left in the picture */
} LgMapCtx;

static int lgmX(const LgMapCtx *c, int32_t wx)
{
	return c->mx + (int)(((int64_t)(wx - c->lg->originx) * c->s) / LG_CELL);
}

static int lgmZ(const LgMapCtx *c, int32_t wz)
{
	return c->my + (int)(((int64_t)(wz - c->lg->originz) * c->s) / LG_CELL);
}

static int lgmAreaAt(const LgLayout *lg, int x, int z)
{
	if (x < 0 || z < 0 || x >= lg->gw || z >= lg->gd) {
		return -1;
	}
	return lg->cells[z][x] - 1;
}

/* is the edge from cell (x, z) to its neighbour dx, dz open? */
static int lgmOpen(const LgLayout *lg, int x, int z, int dx, int dz)
{
	if (dx == 1) return x + 1 < lg->gw && (lg->open[z][x] & LG_OPEN_XP);
	if (dx == -1) return x > 0 && (lg->open[z][x - 1] & LG_OPEN_XP);
	if (dz == 1) return z + 1 < lg->gd && (lg->open[z][x] & LG_OPEN_ZP);
	return z > 0 && (lg->open[z - 1][x] & LG_OPEN_ZP);
}

static void lgmDrawMap(const LgMapCtx *c)
{
	const LgLayout *lg = c->lg;
	LgImage *im = c->im;
	const int s = c->s;
	const int wallt = s >= 12 ? 2 : 1;
	int i, j, x, z;

	/* rock and a grid line every ten cells */
	imgFill(im, c->mx, c->my, c->mx + lg->gw * s, c->my + lg->gd * s, LGMAP_RGB_ROCK);
	for (x = 0; x <= lg->gw; x += 10) imgFill(im, c->mx + x * s, c->my, c->mx + x * s + 1, c->my + lg->gd * s, LGM_GRID);
	for (z = 0; z <= lg->gd; z += 10) imgFill(im, c->mx, c->my + z * s, c->mx + lg->gw * s, c->my + z * s + 1, LGM_GRID);

	/* floors: a raised gallery lighter, a ramp striped across its slope, the
	 * stripes closer together towards the top */
	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];
		const int x0 = c->mx + a->x * s, z0 = c->my + a->z * s, x1 = c->mx + (a->x + a->w) * s, z1 = c->my + (a->z + a->d) * s;
		uint32_t rgb = lgmAreaRgb(lg, i);

		if (a->floor && !a->ramp) {
			rgb = (((rgb >> 16 & 0xff) + 0xff) / 2 << 16) | (((rgb >> 8 & 0xff) + 0xff) / 2 << 8) | ((rgb & 0xff) + 0xff) / 2;
		}
		imgFill(im, x0, z0, x1, z1, rgb);
		if (a->ramp) {
			const uint32_t dark = (rgb >> 1) & 0x7f7f7f;
			const int step = s / 2 > 2 ? s / 2 : 2;
			int k;

			if (a->ramp == LG_RAMP_XP || a->ramp == LG_RAMP_XN) {
				for (k = x0 + step / 2; k < x1; k += step) imgFill(im, k, z0, k + 1, z1, dark);
			} else {
				for (k = z0 + step / 2; k < z1; k += step) imgFill(im, x0, k, x1, k + 1, dark);
			}
		}
	}

	/* navigation links */
	for (i = 0; i < lg->numpads; i++) {
		const LgPad *p = &lg->pads[i];

		for (j = 0; j < p->numneigh; j++) {
			const LgPad *q = &lg->pads[p->neigh[j]];

			if (p->neigh[j] > i) {
				imgLine(im, lgmX(c, p->x), lgmZ(c, p->z), lgmX(c, q->x), lgmZ(c, q->z), LGM_LINK);
			}
		}
	}

	/* light fittings, as the bg writer draws them: panels in rooms, strips
	 * along corridors (modlevelgenfile.c, lglFitting) */
	for (i = 0; i < lg->numareas; i++) {
		int32_t pos[16][2];
		const int n = modLevelGenAreaLightPos(lg, i, pos, 16);
		int32_t hx, hz;

		modLevelGenAreaFitting(lg, i, &hx, &hz);

		for (j = 0; j < n; j++) {
			imgFill(im, lgmX(c, pos[j][0] - hx), lgmZ(c, pos[j][1] - hz), lgmX(c, pos[j][0] + hx), lgmZ(c, pos[j][1] + hz), LGM_LIGHT);
		}
	}

	/* walls: every closed cell edge of an area, drawn inside the area */
	for (z = 0; z < lg->gd; z++) {
		for (x = 0; x < lg->gw; x++) {
			const int a = lgmAreaAt(lg, x, z);
			static const int dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
			int d;

			if (a < 0) {
				continue;
			}
			for (d = 0; d < 4; d++) {
				const int b = lgmAreaAt(lg, x + dirs[d][0], z + dirs[d][1]);
				const int px0 = c->mx + x * s, pz0 = c->my + z * s;

				if (b >= 0 && b != a && lgmOpen(lg, x, z, dirs[d][0], dirs[d][1])) {
					/* a ledge: dashed along the high side's edge */
					const int32_t ex = lg->originx + x * LG_CELL + LG_CELL / 2 + dirs[d][0] * LG_CELL / 2;
					const int32_t ez = lg->originz + z * LG_CELL + LG_CELL / 2 + dirs[d][1] * LG_CELL / 2;

					if (modLevelGenFloorAt(lg, a, ex, ez) > modLevelGenFloorAt(lg, b, ex, ez)) {
						int k;

						for (k = 0; k < s; k++) {
							if ((k / 3) & 1) continue;
							switch (d) {
							case 0: imgFill(im, px0 + s - 2, pz0 + k, px0 + s, pz0 + k + 1, LGM_LEDGE); break;
							case 1: imgFill(im, px0, pz0 + k, px0 + 2, pz0 + k + 1, LGM_LEDGE); break;
							case 2: imgFill(im, px0 + k, pz0 + s - 2, px0 + k + 1, pz0 + s, LGM_LEDGE); break;
							default: imgFill(im, px0 + k, pz0, px0 + k + 1, pz0 + 2, LGM_LEDGE); break;
							}
						}
					}
					continue;
				}
				if (b == a) {
					continue;
				}
				switch (d) {
				case 0: imgFill(im, px0 + s - wallt, pz0, px0 + s, pz0 + s, LGMAP_RGB_WALL); break;
				case 1: imgFill(im, px0, pz0, px0 + wallt, pz0 + s, LGMAP_RGB_WALL); break;
				case 2: imgFill(im, px0, pz0 + s - wallt, px0 + s, pz0 + s, LGMAP_RGB_WALL); break;
				default: imgFill(im, px0, pz0, px0 + s, pz0 + wallt, LGMAP_RGB_WALL); break;
				}
			}
		}
	}

	/* pillars (dark) and low cover (grey) */
	for (i = 0; i < lg->numobst; i++) {
		const LgObstacle *o = &lg->obst[i];
		const int x0 = lgmX(c, o->x - o->halfx), z0 = lgmZ(c, o->z - o->halfz);
		const int x1 = lgmX(c, o->x + o->halfx), z1 = lgmZ(c, o->z + o->halfz);
		const int ex = x1 > x0 + 1 ? x1 : x0 + 2, ez = z1 > z0 + 1 ? z1 : z0 + 2;

		imgFill(im, x0, z0, ex, ez, o->height ? LGMAP_RGB_COVER : LGMAP_RGB_PILLAR);
		if (ex - x0 >= 4 && ez - z0 >= 4) {
			imgBox(im, x0, z0, ex, ez, 1, o->height ? 0x303030 : LGMAP_RGB_WALL);
		}
	}

	/* doors: a bar across the opening */
	for (i = 0; i < lg->numportals; i++) {
		const LgPortal *p = &lg->portals[i];
		const int t = s >= 8 ? s / 4 : 2;

		if (!p->door) {
			continue;
		}
		if (p->x0 == p->x1) {
			const int px = c->mx + p->x0 * s;
			imgFill(im, px - t / 2 - 1, c->my + p->z0 * s, px + (t + 1) / 2 + 1, c->my + p->z1 * s, LGMAP_RGB_DOOR);
		} else {
			const int pz = c->my + p->z0 * s;
			imgFill(im, c->mx + p->x0 * s, pz - t / 2 - 1, c->mx + p->x1 * s, pz + (t + 1) / 2 + 1, LGMAP_RGB_DOOR);
		}
	}

	/* start, extraction and objective rooms: outlined round the whole room,
	 * wings and all (a room's flags are on its main area, area = room number) */
	for (z = 0; z < lg->gd; z++) {
		for (x = 0; x < lg->gw; x++) {
			const int a = lgmAreaAt(lg, x, z);
			static const int dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
			const int t = s >= 10 ? 3 : 2;
			int room, flags, d;
			uint32_t rgb;

			if (a < 0) {
				continue;
			}
			room = lg->areas[a].kind == LG_AREA_ROOM ? lg->areas[a].room : -1;
			flags = room >= 0 ? lg->areas[room].flags : lg->areas[a].flags;
			rgb = (flags & LG_AREAFLAG_START) ? LGMAP_RGB_START : (flags & LG_AREAFLAG_EXTRACT) ? LGMAP_RGB_EXTRACT
				: (flags & LG_AREAFLAG_OBJECTIVE) ? LGMAP_RGB_OBJECTIVE : 0;
			if (!rgb) {
				continue;
			}
			for (d = 0; d < 4; d++) {
				const int b = lgmAreaAt(lg, x + dirs[d][0], z + dirs[d][1]);
				const int px0 = c->mx + x * s, pz0 = c->my + z * s;

				if (b == a || (room >= 0 && b >= 0 && lg->areas[b].kind == LG_AREA_ROOM && lg->areas[b].room == room)) {
					continue;
				}
				switch (d) {
				case 0: imgFill(im, px0 + s - wallt - t, pz0, px0 + s - wallt, pz0 + s, rgb); break;
				case 1: imgFill(im, px0 + wallt, pz0, px0 + wallt + t, pz0 + s, rgb); break;
				case 2: imgFill(im, px0, pz0 + s - wallt - t, px0 + s, pz0 + s - wallt, rgb); break;
				default: imgFill(im, px0, pz0 + wallt, px0 + s, pz0 + wallt + t, rgb); break;
				}
			}
		}
	}

	/* pads: navigation, then guards, objectives and starts on top */
	{
		static const int order[4] = { LG_PAD_NAV, LG_PAD_GUARD, LG_PAD_OBJECTIVE, LG_PAD_START };
		static const uint32_t rgbs[4] = { LGM_NAV, LGMAP_RGB_GUARD, LGMAP_RGB_OBJECTIVE, LGMAP_RGB_START };
		int k;

		for (k = 0; k < 4; k++) {
			const int r = k == 0 ? (s >= 10 ? 1 : 0) : (s >= 10 ? s / 5 : 1);

			for (i = 0; i < lg->numpads; i++) {
				const LgPad *p = &lg->pads[i];

				if (p->kind == order[k]) {
					const int px = lgmX(c, p->x), pz = lgmZ(c, p->z);
					imgFill(im, px - r, pz - r, px + r + 1, pz + r + 1, rgbs[k]);
				}
			}
		}
	}

	/* galleries: their height, and an arrow up each ramp */
	if (s >= 8) {
		for (i = 0; i < lg->numareas; i++) {
			const LgArea *a = &lg->areas[i];
			char t[16];

			if (a->floor && !a->ramp && (a->kind == LG_AREA_ROOM ? i == a->room || a->floor != lg->areas[a->room].floor : 0)) {
				/* a room's floor level, on its main area and on a raised gallery */
				snprintf(t, sizeof(t), "+%d", a->floor);
				imgText(im, c->mx + (a->x + a->w) * s - 4 * (int)strlen(t) * (s >= 14 ? 2 : 1) - wallt - 3,
						c->my + (a->z + a->d) * s - 5 * (s >= 14 ? 2 : 1) - wallt - 3, s >= 14 ? 2 : 1, 0x000000, t);
			} else if (a->ramp) {
				const int cx = c->mx + a->x * s + a->w * s / 2, cz = c->my + a->z * s + a->d * s / 2;
				const int dx = a->ramp == LG_RAMP_XP ? 1 : a->ramp == LG_RAMP_XN ? -1 : 0;
				const int dz = a->ramp == LG_RAMP_ZP ? 1 : a->ramp == LG_RAMP_ZN ? -1 : 0;
				const int len = s, hd = s / 2;

				imgLine(im, cx - dx * len, cz - dz * len, cx + dx * len, cz + dz * len, 0xffffff);
				imgLine(im, cx + dx * len, cz + dz * len, cx + dx * (len - hd) + dz * hd, cz + dz * (len - hd) + dx * hd, 0xffffff);
				imgLine(im, cx + dx * len, cz + dz * len, cx + dx * (len - hd) - dz * hd, cz + dz * (len - hd) - dx * hd, 0xffffff);
			}
		}
	}

	/* room numbers, top left of each room's first area */
	if (s >= 8) {
		for (i = 0; i < lg->numrooms; i++) {
			const LgArea *a = &lg->areas[i];
			char t[16];

			const int hgt = a->ceil - a->floor;

			snprintf(t, sizeof(t), "R%d %s", a->room, s >= 10 ? (hgt >= 600 ? "H6" : hgt >= 500 ? "H5" : hgt >= 400 ? "H4" : hgt >= 350 ? "H3.5" : "H3") : "");
			imgText(im, c->mx + a->x * s + wallt + 2, c->my + a->z * s + wallt + 2, s >= 14 ? 2 : 1, 0xffffff, t);
		}
	}
}

static int lgmCount(const LgLayout *lg, int what)
{
	int i, n = 0;

	for (i = 0; what == 0 && i < lg->numportals; i++) n += lg->portals[i].door != 0;
	for (i = 0; what == 1 && i < lg->numareas; i++) n += modLevelGenAreaLights(lg, i);
	for (i = 0; what == 2 && i < lg->numpads; i++) n += lg->pads[i].kind == LG_PAD_GUARD;
	return n;
}

int modLevelGenMapRender(const LgLayout *lg, int cellpx, int header, LgImage *out)
{
	LgMapCtx c;
	const int margin = 8, hdr = header ? 76 : 0;
	int w, h;

	memset(out, 0, sizeof(*out));
	if (cellpx <= 0) {
		cellpx = 960 / (lg->gw > lg->gd ? lg->gw : lg->gd);
		if (cellpx < 4) cellpx = 4;
		if (cellpx > 24) cellpx = 24;
	}
	w = lg->gw * cellpx + 2 * margin;
	h = lg->gd * cellpx + 2 * margin + hdr;
	if (header && w < 980) {
		w = 980;
	}
	out->px = (uint8_t *)malloc((size_t)w * h * 3);
	if (!out->px) {
		return 0;
	}
	out->w = w;
	out->h = h;
	imgFill(out, 0, 0, w, h, 0x101010);

	c.lg = lg;
	c.im = out;
	c.s = cellpx;
	c.mx = margin;
	c.my = margin + hdr;
	lgmDrawMap(&c);

	if (header) {
		char t[260], name[64];
		int x, k;

		snprintf(t, sizeof(t), "SEED %u  %s  ROOMS %d  AREAS %d  PORTALS %d  DOORS %d  OBSTACLES %d  GUARD PADS %d  LIGHTS %d  TRY %d",
				lg->seed, lg->size < LG_SIZE_COUNT ? g_LgmSizeName[lg->size] : "?", lg->numrooms, lg->numareas, lg->numportals,
				lgmCount(lg, 0), lg->numobst, lgmCount(lg, 2), lgmCount(lg, 1), lg->attempt);
		imgText(out, margin, margin, 2, LGM_TEXT, t);

		/* the key */
		x = margin;
		for (k = 0; k < 6; k++) {
			imgFill(out, x, 34, x + 12, 46, g_LgmThemeRgb[k]);
			x = imgText(out, x + 16, 35, 2, LGM_DIM, g_LgmThemeName[k]) + 12;
		}
		/* the level's name, with both its themes */
		imgText(out, x + 12, 35, 2, LGM_TEXT, modLevelGenLevelName(lg->theme, name, sizeof(name)));
		{
			static const char *names[8] = { "START", "EXTRACT", "OBJECTIVE", "GUARD", "DOOR", "LIGHT", "NAV", "COVER" };
			static const uint32_t rgbs[8] = { LGMAP_RGB_START, LGMAP_RGB_EXTRACT, LGMAP_RGB_OBJECTIVE, LGMAP_RGB_GUARD, LGMAP_RGB_DOOR, LGM_LIGHT, LGM_NAV, LGMAP_RGB_COVER };

			x = margin;
			for (k = 0; k < 8; k++) {
				imgFill(out, x, 54, x + 12, 66, rgbs[k]);
				x = imgText(out, x + 16, 55, 2, LGM_DIM, names[k]) + 12;
			}
			imgText(out, x + 8, 55, 2, LGM_DIM, "STRIPES RAMP +N RAISED DASHES LEDGE");
		}
	}
	return 1;
}

void modLevelGenMapMark(const LgLayout *lg, LgImage *img, int cellpx, int header, int32_t x, int32_t z, uint32_t rgb, int ring)
{
	LgMapCtx c;
	int px, pz, r;

	if (cellpx <= 0) {
		cellpx = 960 / (lg->gw > lg->gd ? lg->gw : lg->gd);
		if (cellpx < 4) cellpx = 4;
		if (cellpx > 24) cellpx = 24;
	}
	c.lg = lg;
	c.im = img;
	c.s = cellpx;
	c.mx = 8;
	c.my = 8 + (header ? 76 : 0);
	px = lgmX(&c, x);
	pz = lgmZ(&c, z);
	r = cellpx / 2 + 2;
	if (ring) {
		imgBox(img, px - r, pz - r, px + r + 1, pz + r + 1, 2, rgb);
	} else {
		imgLine(img, px - r, pz - r, px + r, pz + r, rgb);
		imgLine(img, px - r, pz + r, px + r, pz - r, rgb);
		imgLine(img, px - r + 1, pz - r, px + r + 1, pz + r, rgb);
		imgLine(img, px - r + 1, pz + r, px + r + 1, pz - r, rgb);
	}
}

void modLevelGenImageFree(LgImage *img)
{
	free(img->px);
	memset(img, 0, sizeof(*img));
}

int modLevelGenImageSheet(const LgImage *imgs, const uint32_t *seeds, int n, int cols, LgImage *out)
{
	const int gap = 10, label = 18;
	int cw = 0, ch = 0, rows, i;

	memset(out, 0, sizeof(*out));
	if (n <= 0 || cols <= 0) {
		return 0;
	}
	for (i = 0; i < n; i++) {
		if (imgs[i].w > cw) cw = imgs[i].w;
		if (imgs[i].h > ch) ch = imgs[i].h;
	}
	rows = (n + cols - 1) / cols;
	out->w = cols * (cw + gap) + gap;
	out->h = rows * (ch + label + gap) + gap;
	out->px = (uint8_t *)malloc((size_t)out->w * out->h * 3);
	if (!out->px) {
		return 0;
	}
	imgFill(out, 0, 0, out->w, out->h, 0x080808);
	for (i = 0; i < n; i++) {
		const int ox = gap + (i % cols) * (cw + gap), oy = gap + (i / cols) * (ch + label + gap);
		char t[32];
		int y;

		for (y = 0; y < imgs[i].h; y++) {
			memcpy(out->px + 3 * ((size_t)(oy + y) * out->w + ox), imgs[i].px + 3 * (size_t)y * imgs[i].w, 3 * (size_t)imgs[i].w);
		}
		snprintf(t, sizeof(t), "SEED %u", seeds ? seeds[i] : (uint32_t)i);
		imgText(out, ox, oy + imgs[i].h + 3, 2, LGM_TEXT, t);
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* PNG                                                                   */

static uint32_t g_LgmCrc[256];

static uint32_t lgmCrc(const uint8_t *d, size_t n, uint32_t crc)
{
	size_t i;

	if (g_LgmCrc[1] == 0) {
		uint32_t k, b, v;

		for (k = 0; k < 256; k++) {
			for (v = k, b = 0; b < 8; b++) {
				v = (v & 1) ? 0xedb88320u ^ (v >> 1) : v >> 1;
			}
			g_LgmCrc[k] = v;
		}
	}
	crc = ~crc;
	for (i = 0; i < n; i++) {
		crc = g_LgmCrc[(crc ^ d[i]) & 0xff] ^ (crc >> 8);
	}
	return ~crc;
}

static void lgmBe32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* a chunk at *at: length, type, data, CRC of type and data */
static void lgmChunk(uint8_t *buf, uint32_t *at, const char *type, const uint8_t *data, uint32_t len)
{
	uint8_t *p = buf + *at;

	lgmBe32(p, len);
	memcpy(p + 4, type, 4);
	if (len) memcpy(p + 8, data, len);
	lgmBe32(p + 8 + len, lgmCrc(p + 4, len + 4, 0));
	*at += 12 + len;
}

int modLevelGenImagePng(const LgImage *img, uint8_t **png, uint32_t *size)
{
	const uint32_t rowlen = 1 + 3 * (uint32_t)img->w;
	const uint32_t raw = rowlen * (uint32_t)img->h;
	const uint32_t nblocks = (raw + 65534) / 65535;
	const uint32_t zlen = 2 + raw + 5 * nblocks + 4;
	uint8_t *buf, *z, ihdr[13];
	uint32_t at = 0, i, a = 1, b = 0, done = 0, zat = 2;

	*png = NULL;
	*size = 0;
	if (img->w <= 0 || img->h <= 0) {
		return 0;
	}
	buf = (uint8_t *)malloc(8 + 25 + 12 + zlen + 12);
	z = (uint8_t *)malloc(zlen);
	if (!buf || !z) {
		free(buf);
		free(z);
		return 0;
	}

	/* the zlib stream: stored deflate blocks of the rows, each row led by
	 * filter type 0, then the Adler-32 of the rows */
	z[0] = 0x78;
	z[1] = 0x01;
	while (done < raw) {
		const uint32_t n = raw - done < 65535 ? raw - done : 65535;

		z[zat++] = (uint8_t)(done + n == raw);
		z[zat++] = (uint8_t)n; z[zat++] = (uint8_t)(n >> 8);
		z[zat++] = (uint8_t)~n; z[zat++] = (uint8_t)(~n >> 8);
		for (i = 0; i < n; i++) {
			const uint32_t o = done + i, row = o / rowlen, col = o % rowlen;
			const uint8_t v = col == 0 ? 0 : img->px[(size_t)row * img->w * 3 + col - 1];

			z[zat++] = v;
			a = (a + v) % 65521;
			b = (b + a) % 65521;
		}
		done += n;
	}
	lgmBe32(z + zat, (b << 16) | a);
	zat += 4;

	memcpy(buf, "\x89PNG\r\n\x1a\n", 8);
	at = 8;
	lgmBe32(ihdr, (uint32_t)img->w);
	lgmBe32(ihdr + 4, (uint32_t)img->h);
	ihdr[8] = 8;    /* bits */
	ihdr[9] = 2;    /* RGB */
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	lgmChunk(buf, &at, "IHDR", ihdr, 13);
	lgmChunk(buf, &at, "IDAT", z, zat);
	lgmChunk(buf, &at, "IEND", NULL, 0);
	free(z);
	*png = buf;
	*size = at;
	return 1;
}

int modLevelGenImageWritePng(const LgImage *img, const char *path)
{
	uint8_t *png;
	uint32_t size;
	FILE *fp;
	int ok;

	if (!modLevelGenImagePng(img, &png, &size)) {
		return 0;
	}
	fp = fopen(path, "wb");
	ok = fp && fwrite(png, 1, size, fp) == size;
	if (fp) {
		ok &= fclose(fp) == 0;
	}
	free(png);
	return ok;
}

/* ------------------------------------------------------------------ */
/* Self-test                                                             */

/* Reads back a PNG this file wrote: chunks and CRCs, the stored stream, the
 * Adler-32, and every pixel against the picture */
static int lgmPngCheck(const uint8_t *png, uint32_t size, const LgImage *img, char *err, int errlen)
{
	uint32_t at = 8, a = 1, b = 0, zlen = 0, w = 0, h = 0;
	const uint8_t *zd = NULL;
	int ended = 0;

#define RD32(p) (((uint32_t)(p)[0] << 24) | ((uint32_t)(p)[1] << 16) | ((uint32_t)(p)[2] << 8) | (p)[3])
	if (size < 8 || memcmp(png, "\x89PNG\r\n\x1a\n", 8) != 0) {
		snprintf(err, errlen, "no PNG signature");
		return 0;
	}
	while (at + 12 <= size && !ended) {
		const uint32_t len = RD32(png + at);

		if (at + 12 + len > size || lgmCrc(png + at + 4, len + 4, 0) != RD32(png + at + 8 + len)) {
			snprintf(err, errlen, "chunk at %u: bad length or CRC", at);
			return 0;
		}
		if (!memcmp(png + at + 4, "IHDR", 4)) { w = RD32(png + at + 8); h = RD32(png + at + 12); }
		if (!memcmp(png + at + 4, "IDAT", 4)) { zd = png + at + 8; zlen = len; }
		if (!memcmp(png + at + 4, "IEND", 4)) ended = 1;
		at += 12 + len;
	}
	if (!ended || !zd || w != (uint32_t)img->w || h != (uint32_t)img->h || zlen < 6 || zd[0] != 0x78) {
		snprintf(err, errlen, "chunks: end %d, data %d, %ux%u", ended, zd != NULL, w, h);
		return 0;
	}
	{
		const uint32_t rowlen = 1 + 3 * w;
		uint32_t zat = 2, o = 0, last = 0;

		while (!last) {
			uint32_t n, i;

			if (zat + 5 > zlen) { snprintf(err, errlen, "stream runs out"); return 0; }
			last = zd[zat] & 1;
			n = zd[zat + 1] | ((uint32_t)zd[zat + 2] << 8);
			if ((n ^ (zd[zat + 3] | ((uint32_t)zd[zat + 4] << 8))) != 0xffff || zat + 5 + n > zlen) {
				snprintf(err, errlen, "bad stored block at %u", zat);
				return 0;
			}
			zat += 5;
			for (i = 0; i < n; i++, o++) {
				const uint32_t row = o / rowlen, col = o % rowlen;
				const uint8_t want = col == 0 ? 0 : img->px[(size_t)row * w * 3 + col - 1];

				if (zd[zat + i] != want) { snprintf(err, errlen, "byte %u differs", o); return 0; }
				a = (a + zd[zat + i]) % 65521;
				b = (b + a) % 65521;
			}
			zat += n;
		}
		if (o != rowlen * h || zat + 4 > zlen || RD32(zd + zat) != ((b << 16) | a)) {
			snprintf(err, errlen, "stream size %u or Adler-32", o);
			return 0;
		}
	}
#undef RD32
	return 1;
}

int modLevelGenMapSelfTest(int numseeds, void (*print)(const char *line))
{
	LgLayout *lg = (LgLayout *)malloc(sizeof(*lg));
	char line[300], err[160];
	int passed = 0, failed = 0, size, s, pngchecked = 0;

	if (!lg) {
		print("SELFTEST FAIL levelgen-map: out of memory");
		print("SELFTEST RESULT levelgen-map passed 0 failed 1");
		return 1;
	}
	for (size = 0; size < LG_SIZE_COUNT; size++) {
		int bad = 0;

		for (s = 0; s < numseeds; s++) {
			LgParams prm;
			LgMapCtx c;
			LgImage im;
			int i, ok = 1;

			prm.seed = 5000u + (uint32_t)s * 104729u;
			prm.size = (uint8_t)size;
			if (!modLevelGenBuild(lg, &prm, err, sizeof(err))) {
				continue;   /* the layout's own test covers that */
			}
			if (!modLevelGenMapRender(lg, 0, 1, &im)) {
				snprintf(err, sizeof(err), "render failed");
				ok = 0;
			}
			c.lg = lg;
			c.s = 960 / (lg->gw > lg->gd ? lg->gw : lg->gd);   /* as modLevelGenMapRender() picks it */
			if (c.s < 4) c.s = 4;
			if (c.s > 24) c.s = 24;
			c.mx = 8;
			c.my = 8 + 76;
			c.im = &im;

			/* what is drawn where: every start pad green, every door's middle
			 * orange, every pillar's middle dark, a wall on every room's
			 * outside edge somewhere, the start room outlined */
			for (i = 0; ok && i < lg->numpads; i++) {
				const LgPad *p = &lg->pads[i];

				if (p->kind == LG_PAD_START && imgGet(&im, lgmX(&c, p->x), lgmZ(&c, p->z)) != LGMAP_RGB_START) {
					snprintf(err, sizeof(err), "start pad %d not drawn", i);
					ok = 0;
				}
			}
			for (i = 0; ok && i < lg->numportals; i++) {
				const LgPortal *p = &lg->portals[i];
				const int px = c.mx + (p->x0 + p->x1) * c.s / 2, pz = c.my + (p->z0 + p->z1) * c.s / 2;

				if (p->door && imgGet(&im, px, pz) != LGMAP_RGB_DOOR) {
					snprintf(err, sizeof(err), "door %d not drawn", i);
					ok = 0;
				}
			}
			for (i = 0; ok && i < lg->numobst; i++) {
				const LgObstacle *o = &lg->obst[i];

				const uint32_t want = o->height ? LGMAP_RGB_COVER : LGMAP_RGB_PILLAR;
				int seen = 0, px, pz;

				/* somewhere in its box (a label or an outline may cross its middle) */
				for (pz = lgmZ(&c, o->z - o->halfz); pz <= lgmZ(&c, o->z + o->halfz) && !seen; pz++) {
					for (px = lgmX(&c, o->x - o->halfx); px <= lgmX(&c, o->x + o->halfx) && !seen; px++) {
						seen = imgGet(&im, px, pz) == want;
					}
				}
				if (!seen) {
					snprintf(err, sizeof(err), "pillar %d not drawn", i);
					ok = 0;
				}
			}
			if (ok) {
				const LgArea *a = &lg->areas[lg->startarea];
				int found = 0, x;

				for (x = c.mx + a->x * c.s; x < c.mx + (a->x + a->w) * c.s; x++) {
					found |= imgGet(&im, x, c.my + a->z * c.s + 2) == LGMAP_RGB_START || imgGet(&im, x, c.my + a->z * c.s + 3) == LGMAP_RGB_START;
				}
				if (!found) {
					snprintf(err, sizeof(err), "start room not outlined");
					ok = 0;
				}
			}
			/* the PNG reads back to the same picture (a few, it is slow) */
			if (ok && pngchecked < 3) {
				uint8_t *png;
				uint32_t psize;

				pngchecked++;
				if (!modLevelGenImagePng(&im, &png, &psize) || !lgmPngCheck(png, psize, &im, err, sizeof(err))) {
					ok = 0;
				}
				free(png);
			}
			if (!ok && bad++ < 3) {
				snprintf(line, sizeof(line), "SELFTEST FAIL levelgen-map: seed %u size %d: %s", prm.seed, size, err);
				print(line);
			}
			failed += !ok;
			passed += ok;
			modLevelGenImageFree(&im);
		}
		snprintf(line, sizeof(line), "SELFTEST %s levelgen-map: size %d, %d seeds", bad ? "FAIL" : "PASS", size, numseeds);
		print(line);
	}
	snprintf(line, sizeof(line), "SELFTEST RESULT levelgen-map passed %d failed %d", passed, failed);
	print(line);
	free(lg);
	return failed;
}

#ifdef MODLEVELGENMAP_MAIN
static void lgmPrint(const char *line)
{
	puts(line);
}

int main(int argc, char **argv)
{
	static LgLayout lg;
	char err[160], path[256];
	uint32_t seed;
	int size, count, i;

	if (argc < 2) {
		return modLevelGenMapSelfTest(20, lgmPrint) ? 1 : 0;
	}
	seed = (uint32_t)strtoul(argv[1], NULL, 10);
	size = argc > 2 ? atoi(argv[2]) : 1;
	count = argc > 3 ? atoi(argv[3]) : 1;
	for (i = 0; i < count; i++) {
		LgParams prm;
		LgImage im;

		prm.seed = seed + (uint32_t)i;
		prm.size = (uint8_t)size;
		if (!modLevelGenBuild(&lg, &prm, err, sizeof(err))) {
			printf("seed %u: %s\n", prm.seed, err);
			continue;
		}
		modLevelGenMapRender(&lg, 0, 1, &im);
		snprintf(path, sizeof(path), "map-%u-%d.png", prm.seed, size);
		printf("%s %s\n", path, modLevelGenImageWritePng(&im, path) ? "written" : "FAILED");
		modLevelGenImageFree(&im);
	}
	return 0;
}
#endif
