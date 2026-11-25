/*
 * modlevelgenfile.c: Perfect Random level generator, file writers.
 * See modlevelgenfile.h. Formats follow port/src/geconvert.c (writeBg,
 * roomPack, writeTiles, writePads) and the loaders in src/game/bg.c,
 * tilesreset.c, pad.c and setup.c.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game/modlevelgenfile.h"
#include "game/modlevelgen.h"
#include "game/levelgentexnums.h"

#define SEG_BG 0x0f000000u

/* tile flags (geconvert.c): FLOOR1 FLOOR2 SIGHT SHOOT / WALL SIGHT SHOOT */
#define LGF_FLOOR_FLAGS 0x001b
#define LGF_WALL_FLAGS  0x001c
#define LGF_WALL_BELOW  50
#define LGF_WALL_ABOVE  400
#define LGF_DIE         0x4000   /* GEOFLAG_DIE: whoever lands on it dies */
#define LGF_LADDER      0x0040   /* GEOFLAG_LADDER: climbed (the pipeworks', 2026-11-19) */
#define LG_PW_PIT_DRAWN 0        /* the pipeworks' sludge (modlevelgen.c's LG_PW_PIT) */

#define LGF_OBJTYPE_END 0x34
#define LGF_INTROCMD_SPAWN 0
#define LGF_INTROCMD_END   12
#define LGF_AICMD_END      0x0004
#define LGF_AILIST_ID      0x0401

/* ------------------------------------------------------------------ */
/* Growing big-endian buffer                                            */

typedef struct {
	uint8_t *v;
	uint32_t n, cap;
	int err;
} Buf;

static void bufPut(Buf *b, const void *p, uint32_t n)
{
	if (b->err) {
		return;
	}
	if (b->n + n > b->cap) {
		uint32_t cap = b->cap ? b->cap : 256;
		uint8_t *nv;

		while (cap < b->n + n) {
			cap *= 2;
		}
		nv = (uint8_t *)realloc(b->v, cap);
		if (!nv) {
			b->err = 1;
			return;
		}
		b->v = nv;
		b->cap = cap;
	}
	memcpy(b->v + b->n, p, n);
	b->n += n;
}

static void bufU8(Buf *b, uint32_t x)
{
	uint8_t c = (uint8_t)x;
	bufPut(b, &c, 1);
}

static void bufU16(Buf *b, uint32_t x)
{
	uint8_t c[2];
	c[0] = (uint8_t)(x >> 8);
	c[1] = (uint8_t)x;
	bufPut(b, c, 2);
}

static void bufU32(Buf *b, uint32_t x)
{
	uint8_t c[4];
	c[0] = (uint8_t)(x >> 24);
	c[1] = (uint8_t)(x >> 16);
	c[2] = (uint8_t)(x >> 8);
	c[3] = (uint8_t)x;
	bufPut(b, c, 4);
}

static void bufF32(Buf *b, float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	bufU32(b, u);
}

static void bufZeros(Buf *b, uint32_t n)
{
	while (n--) {
		bufU8(b, 0);
	}
}

static void bufPad(Buf *b, uint32_t align)
{
	while (b->n % align) {
		bufU8(b, 0);
	}
}

static void bufSet32(Buf *b, uint32_t at, uint32_t x)
{
	if (b->err || at + 4 > b->n) {
		b->err = 1;
		return;
	}
	b->v[at] = (uint8_t)(x >> 24);
	b->v[at + 1] = (uint8_t)(x >> 16);
	b->v[at + 2] = (uint8_t)(x >> 8);
	b->v[at + 3] = (uint8_t)x;
}

static void bufSet16(Buf *b, uint32_t at, uint32_t x)
{
	if (b->err || at + 2 > b->n) {
		b->err = 1;
		return;
	}
	b->v[at] = (uint8_t)(x >> 8);
	b->v[at + 1] = (uint8_t)x;
}

static uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t rd16(const uint8_t *p)
{
	return ((uint32_t)p[0] << 8) | p[1];
}

/*
 * rzip 1173 made of "stored" deflate blocks: 0x11 0x73, the inflated length
 * in three bytes, then a raw deflate stream. The game inflates it with zlib
 * like any other; no compressor is needed to make one.
 */
static int rzipStored(const Buf *in, LgBuf *out)
{
	const uint32_t nblocks = in->n ? (in->n + 65534) / 65535 : 1;
	const uint32_t size = 5 + nblocks * 5 + in->n;
	uint8_t *d = (uint8_t *)malloc(size);
	uint32_t o = 5, i = 0;

	if (!d || in->err) {
		free(d);
		return 0;
	}
	d[0] = 0x11;
	d[1] = 0x73;
	d[2] = (uint8_t)(in->n >> 16);
	d[3] = (uint8_t)(in->n >> 8);
	d[4] = (uint8_t)in->n;

	do {
		const uint32_t len = in->n - i > 65535 ? 65535 : in->n - i;
		const uint32_t last = (i + len >= in->n);

		d[o++] = (uint8_t)last;            /* BFINAL, BTYPE 00 */
		d[o++] = (uint8_t)len;
		d[o++] = (uint8_t)(len >> 8);
		d[o++] = (uint8_t)~len;
		d[o++] = (uint8_t)(~len >> 8);
		if (len) {
			memcpy(d + o, in->v + i, len);
		}
		o += len;
		i += len;
	} while (i < in->n);

	out->data = d;
	out->size = o;
	return 1;
}

static int rzipUnstore(const uint8_t *d, uint32_t size, Buf *out)
{
	uint32_t o = 5, want;

	if (size < 10 || d[0] != 0x11 || d[1] != 0x73) {
		return 0;
	}
	want = ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 8) | d[4];
	for (;;) {
		uint32_t last, len, nlen;

		if (o + 5 > size) {
			return 0;
		}
		last = d[o] & 1;
		if ((d[o] & 6) != 0) {
			return 0;
		}
		len = d[o + 1] | ((uint32_t)d[o + 2] << 8);
		nlen = d[o + 3] | ((uint32_t)d[o + 4] << 8);
		if ((len ^ nlen) != 0xffff || o + 5 + len > size) {
			return 0;
		}
		bufPut(out, d + o + 5, len);
		o += 5 + len;
		if (last) {
			break;
		}
	}
	return out->n == want && !out->err;
}

/* ------------------------------------------------------------------ */
/* Defaults                                                              */

void modLevelGenBoxDefaults(LgBoxParams *p)
{
	memset(p, 0, sizeof(*p));
	p->halfx = 500;
	p->halfz = 500;
	p->ceil = 300;
	/* A texture per surface, each at the scale Perfect Dark's own levels give
	 * it and each one they use only on that kind of surface (see
	 * tools/levelgen/pdtex.py and the project's levelgen-textures notes):
	 *   floor   0x11e  1816 floor triangles,   7.6 s/t units per world unit
	 *   wall    0x109  3351 wall triangles,   10.0
	 *   ceiling 0x116   767 ceiling triangles, 21.6
	 * 0x109 on every surface looked like a stone wall all round.
	 *
	 * The command word: 0xc0, wrap in s and t (bits 22-23 and 20-21 zero),
	 * subcommand 2 - the commonest word in Perfect Dark's and the converted
	 * GoldenEye rooms. 0xc0980002 clamps s and mirrors t: the walls showed
	 * the texture's edge pixels smeared across them. */
	p->tex[LGSURF_FLOOR] = 0x011e;
	p->tex[LGSURF_WALL] = 0x0109;
	p->tex[LGSURF_CEIL] = 0x0116;
	p->texword0 = 0xc0080002u;
	p->uvscale[LGSURF_FLOOR] = 760;
	p->uvscale[LGSURF_WALL] = 1000;
	p->uvscale[LGSURF_CEIL] = 2160;
	p->floortype = 2;          /* SURFACETYPE wood/stone, footsteps */
	p->floorcolour = 0x0ccc;
	p->shade = 0xc8c8c8ffu;
}

/* ------------------------------------------------------------------ */
/* Room geometry                                                         */

typedef struct {
	int32_t x, y, z;
	int32_t s, t;
} LgVert;

typedef struct {
	LgVert v[16];
	int n;
	uint8_t tri[16][3];
	int ntri;
} LgBatch;

/* One quad, its corners in any order, drawn facing the way normal points.
 * Triangles use the F3DEX rule: counter-clockwise as seen by the viewer is
 * the front, with the normal (b - a) x (c - a). */
static int addQuad(LgBatch *bt, const int32_t c[4][3], const int32_t uv[4][2], const int32_t nrm[3])
{
	static const int fwd[4] = { 0, 1, 2, 3 };
	static const int rev[4] = { 0, 3, 2, 1 };
	int64_t e1[3], e2[3], n[3], dot;
	const int *ord;
	int base = bt->n, i;

	if (bt->n + 4 > 16) {
		return 0;
	}
	for (i = 0; i < 3; i++) {
		e1[i] = c[1][i] - c[0][i];
		e2[i] = c[2][i] - c[0][i];
	}
	n[0] = e1[1] * e2[2] - e1[2] * e2[1];
	n[1] = e1[2] * e2[0] - e1[0] * e2[2];
	n[2] = e1[0] * e2[1] - e1[1] * e2[0];
	dot = n[0] * nrm[0] + n[1] * nrm[1] + n[2] * nrm[2];
	ord = dot >= 0 ? fwd : rev;

	for (i = 0; i < 4; i++) {
		const int k = ord[i];
		bt->v[base + i].x = c[k][0];
		bt->v[base + i].y = c[k][1];
		bt->v[base + i].z = c[k][2];
		bt->v[base + i].s = uv[k][0];
		bt->v[base + i].t = uv[k][1];
	}
	bt->n += 4;
	bt->tri[bt->ntri][0] = (uint8_t)base;
	bt->tri[bt->ntri][1] = (uint8_t)(base + 1);
	bt->tri[bt->ntri][2] = (uint8_t)(base + 2);
	bt->ntri++;
	bt->tri[bt->ntri][0] = (uint8_t)base;
	bt->tri[bt->ntri][1] = (uint8_t)(base + 2);
	bt->tri[bt->ntri][2] = (uint8_t)(base + 3);
	bt->ntri++;
	return 1;
}

static int32_t uvOf(int32_t units, int uvscale)
{
	return (int32_t)(((int64_t)units * uvscale) / 100);
}

/* The display list of one batch: colours, vertices, then triangles four to a
 * command (G_TRI4, which Perfect Dark's rooms use for everything). */
static void emitBatch(Buf *gdl, const LgBatch *bt, uint32_t start)
{
	const uint32_t n = (uint32_t)bt->n;
	int i;

	bufU32(gdl, (0x07u << 24) | ((((n - 1) << 2) & 0xff) << 16) | (n * 4));
	bufU32(gdl, 0x0d000000u | (start * 4));
	bufU32(gdl, (0x04u << 24) | ((n - 1) << 20) | (0u << 16) | (n * 12));
	bufU32(gdl, 0x0e000000u | (start * 12));

	for (i = 0; i < bt->ntri; i += 4) {
		uint32_t w0 = 0xb1u << 24, w1 = 0;
		int j;

		for (j = 0; j < 4 && i + j < bt->ntri; j++) {
			const uint32_t a = bt->tri[i + j][0], b = bt->tri[i + j][1], c = bt->tri[i + j][2];

			w0 |= c << (4 * j);
			w1 |= ((b << 4) | a) << (8 * j);
		}
		bufU32(gdl, w0);
		bufU32(gdl, w1);
	}
}

/* Baked shading: a vertex is brighter near the room's lights and darker away
 * from them, which gives a room bright patches under each fixture and dim
 * corners. The game does not scale these by the room's brightness: it only
 * caps them (roomHighlight() in dlights.c: a vertex whose brightest channel
 * is over the room's brightness is scaled down to it, nothing is brightened).
 * So the colours here run right up to full, and the room's brightness (its
 * ceiling by room type, less what broken or shot-out lights take) is what
 * darkens them. Colours at most 200 were never touched by any room
 * brightness above 200, which is how the first version had no effect. */
typedef struct {
	const int32_t (*pos)[3];   /* light x, z and the height it hangs at */
	int n;
	const uint8_t *tint;       /* per channel, 255 = none */
	int32_t factor;            /* per mille: the room type's overall brightness */
	int32_t reach;             /* a light's reach, world units */
	int32_t ambient;           /* per mille: the least any vertex gets (outdoors: the sky's light; indoors 0) */
} LgLightCtx;

#define LGL_SHADEBASE 0xffffffffu  /* a vertex right under a light */
#define LGL_SKYLIGHT  600          /* per mille of a light's: what the open sky gives every vertex outdoors */

static uint32_t lglVertShade(const LgLightCtx *lc, uint32_t base, int32_t x, int32_t y, int32_t z)
{
	const int64_t r2 = (int64_t)lc->reach * lc->reach;
	int64_t ill = 0, scale;
	uint32_t out = base & 0xff;
	int i, c;

	for (i = 0; i < lc->n; i++) {
		/* height counts for half: a fitting lights the floor under it more than
		 * the ceiling beside it */
		const int64_t dx = x - lc->pos[i][0], dy = (y - lc->pos[i][2]) / 2, dz = z - lc->pos[i][1];

		const int64_t f = 1000 * r2 / (r2 + dx * dx + dy * dy + dz * dz);   /* the nearest light's, not a sum: a grid of them would light everything */

		if (f > ill) {
			ill = f;
		}
	}
	if (ill < lc->ambient) {
		ill = lc->ambient;
	}
	if (ill > 1000) {
		ill = 1000;
	}
	/* per mille: Perfect Dark's own rooms run from about 48 (a fifth) in the
	 * dark to 255 at the fittings (tools/levelgen/pdlight.py) */
	scale = (250 + 750 * ill / 1000) * lc->factor / 1000;
	for (c = 0; c < 3; c++) {
		const int64_t v = (int64_t)((base >> (24 - 8 * c)) & 0xff) * scale / 1000 * lc->tint[c] / 255;

		out |= (uint32_t)v << (24 - 8 * c);
	}
	return out;
}

static void emitVerts(Buf *body, uint32_t at, const LgBatch *bt, uint32_t first, const int32_t centre[3],
		uint32_t colat, uint32_t shade, const LgLightCtx *lc)
{
	int i;

	for (i = 0; i < bt->n; i++) {
		const uint32_t o = at + 12 * (first + (uint32_t)i);

		bufSet16(body, o, (uint32_t)(bt->v[i].x - centre[0]) & 0xffff);
		bufSet16(body, o + 2, (uint32_t)(bt->v[i].y - centre[1]) & 0xffff);
		bufSet16(body, o + 4, (uint32_t)(bt->v[i].z - centre[2]) & 0xffff);
		body->v[o + 6] = 0;                     /* flags: not lit */
		body->v[o + 7] = (uint8_t)(i << 2);     /* its colour, within the batch */
		bufSet16(body, o + 8, (uint32_t)bt->v[i].s & 0xffff);
		bufSet16(body, o + 10, (uint32_t)bt->v[i].t & 0xffff);
		bufSet32(body, colat + 4 * (first + (uint32_t)i),
				lc ? lglVertShade(lc, shade, bt->v[i].x, bt->v[i].y, bt->v[i].z) : shade);
	}
}

/* ------------------------------------------------------------------ */
/* The box                                                               */

static int boxBg(const LgBoxParams *p, Buf *out, int32_t bbox[6], int32_t centre[3])
{
	LgBatch bt[3];          /* by LGSURF_: one texture command each */
	Buf prim = { 0 }, room = { 0 }, gdl = { 0 }, s2 = { 0 }, s3 = { 0 };
	const int32_t hx = p->halfx, hz = p->halfz, h = p->ceil;
	const int32_t up[3] = { 0, 1, 0 }, down[3] = { 0, -1, 0 };
	const int32_t nzp[3] = { 0, 0, 1 }, nzn[3] = { 0, 0, -1 }, nxp[3] = { 1, 0, 0 }, nxn[3] = { -1, 0, 0 };
	int32_t c[4][3], uv[4][2];
	uint32_t inf, roomsize, ptr, vtxat, colat, gdlat, nv, tableat = 0x18, lightsat, cmdsat, portalsat, groupsat;
	uint32_t s1comp;
	int32_t mn[3], mx[3];
	int i, k;

	memset(bt, 0, sizeof(bt));

	/* floor and ceiling */
	{
		const int32_t xs[4] = { -hx, hx, hx, -hx }, zs[4] = { -hz, -hz, hz, hz };

		for (i = 0; i < 4; i++) {
			c[i][0] = xs[i]; c[i][1] = 0; c[i][2] = zs[i];
			uv[i][0] = uvOf(xs[i], p->uvscale[LGSURF_FLOOR]); uv[i][1] = uvOf(zs[i], p->uvscale[LGSURF_FLOOR]);
		}
		if (!addQuad(&bt[LGSURF_FLOOR], c, uv, up)) {
			return 0;
		}
		for (i = 0; i < 4; i++) {
			c[i][1] = h;
			uv[i][0] = uvOf(xs[i], p->uvscale[LGSURF_CEIL]); uv[i][1] = uvOf(zs[i], p->uvscale[LGSURF_CEIL]);
		}
		if (!addQuad(&bt[LGSURF_CEIL], c, uv, down)) {
			return 0;
		}
	}

	/* four walls; (x0,z0)-(x1,z1) along the wall, facing into the room */
	{
		const int32_t wx0[4] = { -hx, -hx, -hx, hx }, wz0[4] = { -hz, hz, -hz, -hz };
		const int32_t wx1[4] = { hx, hx, -hx, hx }, wz1[4] = { -hz, hz, hz, hz };
		const int32_t *wn[4] = { nzp, nzn, nxp, nxn };

		for (k = 0; k < 4; k++) {
			const int alongx = (wx0[k] != wx1[k]);
			const int32_t u0 = alongx ? wx0[k] : wz0[k], u1 = alongx ? wx1[k] : wz1[k];
			const int32_t ux[4] = { u0, u1, u1, u0 }, yy[4] = { 0, 0, h, h };

			for (i = 0; i < 4; i++) {
				c[i][0] = alongx ? ux[i] : wx0[k];
				c[i][1] = yy[i];
				c[i][2] = alongx ? wz0[k] : ux[i];
				uv[i][0] = uvOf(ux[i], p->uvscale[LGSURF_WALL]);
				uv[i][1] = uvOf(-yy[i], p->uvscale[LGSURF_WALL]);
			}
			if (!addQuad(&bt[LGSURF_WALL], c, uv, wn[k])) {
				return 0;
			}
		}
	}

	nv = (uint32_t)(bt[0].n + bt[1].n + bt[2].n);

	/* the room's own centre and the box of its vertices about it */
	mn[0] = mn[1] = mn[2] = 0x7fffffff;
	mx[0] = mx[1] = mx[2] = -0x7fffffff;
	for (k = 0; k < 3; k++) {
		for (i = 0; i < bt[k].n; i++) {
			const LgVert *v = &bt[k].v[i];
			const int32_t q[3] = { v->x, v->y, v->z };
			int m;

			for (m = 0; m < 3; m++) {
				if (q[m] < mn[m]) mn[m] = q[m];
				if (q[m] > mx[m]) mx[m] = q[m];
			}
		}
	}
	for (k = 0; k < 3; k++) {
		centre[k] = (mn[k] + mx[k]) / 2;
		bbox[k] = mn[k] - centre[k];
		bbox[3 + k] = mx[k] - centre[k];
	}

	/* primary block: header, room table, lights (none), vis commands (just
	 * the end), portals (just the end) and portal vertices (just the end) */
	lightsat = tableat + 20 * (1 + 3);
	cmdsat = lightsat;
	portalsat = cmdsat + 8;
	groupsat = portalsat + 8;
	/* The port widens this block's pointers when it loads the file
	 * (preprocessBgSection1) and makes room for that by growing the declared
	 * size by a tenth. A block this small grows by more than a tenth, so it
	 * ends in zeros the loader never reads. */
	bufZeros(&prim, groupsat + 4 + 256);
	bufPad(&prim, 4);
	bufSet32(&prim, 4, SEG_BG + tableat);
	bufSet32(&prim, 8, SEG_BG + portalsat);
	bufSet32(&prim, 12, SEG_BG + cmdsat);
	prim.v[cmdsat + 1] = 1;          /* the end command: type 0, length 1 */
	inf = prim.n;

	/* the room: header, one leaf, vertices, colours, display list */
	vtxat = (0x18 + 20 + 7) & ~7u;
	colat = vtxat + 12 * nv;
	gdlat = (colat + 4 * nv + 7) & ~7u;
	ptr = SEG_BG + inf;
	bufZeros(&room, gdlat);
	bufSet32(&room, 0, ptr + vtxat);
	bufSet32(&room, 4, ptr + colat);
	bufSet32(&room, 8, ptr + 0x18);
	bufSet32(&room, 12, 0);
	bufSet16(&room, 0x10, 0xffff);
	bufSet16(&room, 0x12, 0);
	bufSet16(&room, 0x14, nv);
	bufSet16(&room, 0x16, nv);
	room.v[0x18] = 0;                           /* a leaf */
	bufSet32(&room, 0x18 + 4, 0);
	bufSet32(&room, 0x18 + 8, ptr + gdlat);
	bufSet32(&room, 0x18 + 12, ptr + vtxat);
	bufSet32(&room, 0x18 + 16, ptr + colat);
	{
		uint32_t first = 0;

		for (k = 0; k < 3; k++) {
			emitVerts(&room, vtxat, &bt[k], first, centre, colat, p->shade, NULL);
			first += (uint32_t)bt[k].n;
		}
	}

	/* the display list: Perfect Dark's own opaque room preamble (render
	 * mode, combiner, texture command), then the batches */
	bufU32(&gdl, 0xe7000000u); bufU32(&gdl, 0);
	bufU32(&gdl, 0xba001402u); bufU32(&gdl, 0x00100000u);
	bufU32(&gdl, 0xb900031du); bufU32(&gdl, 0x0c182078u);
	bufU32(&gdl, 0xfc26a004u); bufU32(&gdl, 0x1f1093ffu);
	bufU32(&gdl, 0xba001001u); bufU32(&gdl, 0x00010000u);
	bufU32(&gdl, 0xbb003001u); bufU32(&gdl, 0xffffffffu);
	bufU32(&gdl, 0xba001102u); bufU32(&gdl, 0);
	bufU32(&gdl, 0xba000c02u); bufU32(&gdl, 0x00002000u);
	bufU32(&gdl, 0xb7000000u); bufU32(&gdl, 0x00002000u);
	{
		uint32_t first = 0;

		/* each surface: its texture command, then its triangles */
		for (k = 0; k < 3; k++) {
			bufU32(&gdl, p->texword0); bufU32(&gdl, p->tex[k] & 0xfffu);
			emitBatch(&gdl, &bt[k], first);
			first += (uint32_t)bt[k].n;
		}
	}
	bufU32(&gdl, 0xb8000000u); bufU32(&gdl, 0);
	bufPut(&room, gdl.v, gdl.n);
	roomsize = room.n;

	/* the room table: row 1 is the room, row 2 closes the list at the end of
	 * the room data */
	bufSet32(&prim, tableat + 20 * 1, ptr);
	{
		/* centre as floats */
		const float cf[3] = { (float)centre[0], (float)centre[1], (float)centre[2] };
		uint32_t u;

		for (k = 0; k < 3; k++) {
			memcpy(&u, &cf[k], 4);
			bufSet32(&prim, tableat + 20 * 1 + 4 + 4 * (uint32_t)k, u);
		}
	}
	prim.v[tableat + 20 * 1 + 16] = 128;     /* brightness range */
	prim.v[tableat + 20 * 1 + 17] = 255;
	bufSet32(&prim, tableat + 20 * 2, ptr + roomsize);

	/* section 2: the textures the room names */
	for (k = 0; k < 3; k++) {
		bufU16(&s2, p->tex[k]);
	}
	/* section 3: the room's box, its list size in 16s, its light count */
	bufU16(&s3, (uint32_t)bbox[0] & 0xffff); bufU16(&s3, (uint32_t)bbox[1] & 0xffff);
	bufU16(&s3, (uint32_t)bbox[2] & 0xffff); bufU16(&s3, (uint32_t)bbox[3] & 0xffff);
	bufU16(&s3, (uint32_t)bbox[4] & 0xffff); bufU16(&s3, (uint32_t)bbox[5] & 0xffff);
	bufU16(&s3, room.n / 16 + 1);
	bufU8(&s3, 0);

	/* the file: [inflated primary size][size of section 1 after these 12][stored
	 * primary size] primary rooms, section 2, section 3. Nothing here is
	 * compressed; the loader copies what is not rzip. */
	s1comp = prim.n + room.n;
	bufU32(out, inf);
	bufU32(out, s1comp);
	bufU32(out, prim.n);
	bufPut(out, prim.v, prim.n);
	bufPut(out, room.v, room.n);
	bufU16(out, 0x8000u | s2.n);
	bufU16(out, s2.n);
	bufPut(out, s2.v, s2.n);
	bufU16(out, 0x8000u | s3.n);
	bufU16(out, s3.n);
	bufPut(out, s3.v, s3.n);
	bufPad(out, 16);

	{
		const int err = prim.err | room.err | gdl.err | s2.err | s3.err | out->err;

		free(prim.v);
		free(room.v);
		free(gdl.v);
		free(s2.v);
		free(s3.v);
		return !err;
	}
}

/* One tile: type 0 (s16 points) */
static void emitTile(Buf *b, int n, const int32_t pts[][3], uint32_t flags, uint32_t floortype, uint32_t colour)
{
	int mn[3] = { 0, 0, 0 }, mx[3] = { 0, 0, 0 };
	int c, k;

	for (c = 0; c < 3; c++) {
		for (k = 1; k < n; k++) {
			if (pts[k][c] < pts[mn[c]][c]) mn[c] = k;
			if (pts[k][c] > pts[mx[c]][c]) mx[c] = k;
		}
	}
	bufU8(b, 0);
	bufU8(b, (uint32_t)n);
	bufU16(b, flags);
	bufU16(b, floortype);
	for (c = 0; c < 3; c++) bufU8(b, (uint32_t)mn[c]);
	for (c = 0; c < 3; c++) bufU8(b, (uint32_t)mx[c]);
	bufU16(b, colour);
	for (k = 0; k < n; k++) {
		for (c = 0; c < 3; c++) {
			bufU16(b, (uint32_t)pts[k][c] & 0xffff);
		}
	}
}

static int boxTiles(const LgBoxParams *p, LgBuf *out)
{
	const int32_t hx = p->halfx, hz = p->halfz;
	/* floors wind so that x*z' - x'*z sums negative (geconvert.c's tiles) */
	const int32_t fl[4][3] = { { -hx, 0, -hz }, { -hx, 0, hz }, { hx, 0, hz }, { hx, 0, -hz } };
	Buf body = { 0 }, f = { 0 };
	int k, ok;

	emitTile(&body, 4, fl, LGF_FLOOR_FLAGS, p->floortype, p->floorcolour);
	for (k = 0; k < 4; k++) {
		const int32_t *a = fl[k], *b = fl[(k + 1) % 4];
		int32_t q[4][3];

		q[0][0] = a[0]; q[0][1] = a[1] - LGF_WALL_BELOW; q[0][2] = a[2];
		q[1][0] = b[0]; q[1][1] = b[1] - LGF_WALL_BELOW; q[1][2] = b[2];
		q[2][0] = b[0]; q[2][1] = b[1] + LGF_WALL_ABOVE; q[2][2] = b[2];
		q[3][0] = a[0]; q[3][1] = a[1] + LGF_WALL_ABOVE; q[3][2] = a[2];
		emitTile(&body, 4, q, LGF_WALL_FLAGS, 0, 0x0fff);
	}

	/* [number of rooms + 1][offset of each room 0..n][offset of the end][bodies] */
	{
		const uint32_t numrooms = 1;
		uint32_t pos = 4 + 4 * (numrooms + 1) + 4;

		bufU32(&f, numrooms + 1);
		bufU32(&f, pos);            /* room 0: none */
		bufU32(&f, pos);            /* room 1 */
		bufU32(&f, pos + body.n);   /* the end */
		bufPut(&f, body.v, body.n);
		bufPad(&f, 16);
	}
	ok = rzipStored(&f, out);
	free(body.v);
	free(f.v);
	return ok;
}

typedef struct {
	float x, y, z, lx, lz;
} LgBoxPad;

static int boxPads(const LgBoxParams *p, LgBuf *out)
{
	const float hx = (float)p->halfx, hz = (float)p->halfz;
	LgBoxPad pads[LGBOX_NUMPADS];
	Buf head = { 0 }, body = { 0 }, pb = { 0 }, f = { 0 };
	uint32_t start, wpstart, wgstart, coverstart;
	int i, ok;

	/* three start pads on the west side facing east, four guard pads in the
	 * corners facing the middle; one unit above the floor (the game looks
	 * for a floor strictly below a pad) */
	pads[0].x = -hx * 0.5f; pads[0].z = 0.0f;    pads[0].lx = 1.0f;  pads[0].lz = 0.0f;
	pads[1].x = -hx * 0.5f; pads[1].z = 100.0f;  pads[1].lx = 1.0f;  pads[1].lz = 0.0f;
	pads[2].x = -hx * 0.5f; pads[2].z = -100.0f; pads[2].lx = 1.0f;  pads[2].lz = 0.0f;
	pads[3].x = hx - 150.0f; pads[3].z = hz - 150.0f;  pads[3].lx = -0.7071f; pads[3].lz = -0.7071f;
	pads[4].x = hx - 150.0f; pads[4].z = -hz + 150.0f; pads[4].lx = -0.7071f; pads[4].lz = 0.7071f;
	pads[5].x = -hx + 150.0f; pads[5].z = hz - 150.0f; pads[5].lx = 0.7071f;  pads[5].lz = -0.7071f;
	pads[6].x = -hx + 150.0f; pads[6].z = -hz + 150.0f; pads[6].lx = 0.7071f; pads[6].lz = 0.7071f;
	for (i = 0; i < LGBOX_NUMPADS; i++) {
		pads[i].y = 1.0f;
	}

	start = (0x14 + 2 * LGBOX_NUMPADS + 3) & ~3u;
	for (i = 0; i < LGBOX_NUMPADS; i++) {
		bufU16(&head, start + body.n);
		bufU32(&body, (0u << 14) | (1u << 4));      /* no flags, room 1, no lift */
		bufF32(&body, pads[i].x); bufF32(&body, pads[i].y); bufF32(&body, pads[i].z);
		bufF32(&body, 0.0f); bufF32(&body, 1.0f); bufF32(&body, 0.0f);          /* up */
		bufF32(&body, pads[i].lx); bufF32(&body, 0.0f); bufF32(&body, pads[i].lz); /* look */
		bufPad(&body, 4);
	}
	bufPad(&head, 4);
	bufPut(&pb, head.v, head.n);
	bufPut(&pb, body.v, body.n);

	/* no waypoints and no groups; the end of each list is all there is. The
	 * game builds a graph from the pads when a stage has none (modalarm.c) */
	wpstart = 0x14 + pb.n;
	wgstart = wpstart + 16;
	coverstart = wgstart + 12;

	bufU32(&f, LGBOX_NUMPADS);
	bufU32(&f, 0);
	bufU32(&f, wpstart);
	bufU32(&f, wgstart);
	bufU32(&f, coverstart);
	bufPut(&f, pb.v, pb.n);
	bufU32(&f, 0xffffffffu);
	bufZeros(&f, 12);
	bufZeros(&f, 12);
	bufPad(&f, 16);
	ok = rzipStored(&f, out);
	free(head.v);
	free(body.v);
	free(pb.v);
	free(f.v);
	return ok;
}

static int boxSetup(LgBuf *out)
{
	Buf f = { 0 };
	uint32_t intro = 0x20, props, paths, lists, listcode;
	int i, ok;

	bufZeros(&f, 0x20);
	for (i = 0; i < 3; i++) {                       /* spawn on the start pads */
		bufU32(&f, LGF_INTROCMD_SPAWN);
		bufU32(&f, (uint32_t)(LGBOX_PAD_START0 + i));
		bufU32(&f, 0);
	}
	bufU32(&f, LGF_INTROCMD_END);
	props = f.n;
	bufU32(&f, LGF_OBJTYPE_END);                    /* no props */
	paths = f.n;
	bufZeros(&f, 8);                                /* no paths */
	/* One AI list that does nothing, and the table that names it. The table
	 * may not be empty: setup.c sorts it by reading entry [i + 1] before it
	 * checks [i], so an empty table has it read, and swap, 16 bytes at a time,
	 * whatever the stage pool put after the setup (the pads file: the first
	 * generated level's pads came out scrambled). The id is a stage list's
	 * (0x401 up) and under 0x1000, which would make a background chr of it. */
	listcode = f.n;
	bufU16(&f, LGF_AICMD_END);
	bufPad(&f, 4);
	lists = f.n;
	bufU32(&f, listcode);
	bufU32(&f, LGF_AILIST_ID);
	bufZeros(&f, 8);
	bufPad(&f, 16);
	bufSet32(&f, 0x0c, intro);
	bufSet32(&f, 0x10, props);
	bufSet32(&f, 0x14, paths);
	bufSet32(&f, 0x18, lists);
	ok = rzipStored(&f, out);
	free(f.v);
	return ok;
}

int modLevelGenBoxFiles(const LgBoxParams *p, LgFiles *out, char *err, int errlen)
{
	Buf bg = { 0 };
	int32_t bbox[6], centre[3];

	memset(out, 0, sizeof(*out));
	if (p->halfx < 200 || p->halfz < 200 || p->halfx > 3000 || p->halfz > 3000 || p->ceil < 200 || p->ceil > 1000) {
		snprintf(err, (size_t)errlen, "box size out of range");
		return 0;
	}
	if (!boxBg(p, &bg, bbox, centre)) {
		free(bg.v);
		snprintf(err, (size_t)errlen, "could not build the bg file");
		return 0;
	}
	out->bg.data = bg.v;
	out->bg.size = bg.n;
	if (!boxTiles(p, &out->tiles) || !boxPads(p, &out->pads) || !boxSetup(&out->setup)) {
		modLevelGenFilesFree(out);
		snprintf(err, (size_t)errlen, "could not build the tiles, pads or setup file");
		return 0;
	}
	return 1;
}

void modLevelGenFilesFree(LgFiles *f)
{
	free(f->bg.data);
	free(f->tiles.data);
	free(f->pads.data);
	free(f->setup.data);
	memset(f, 0, sizeof(*f));
}

/* ------------------------------------------------------------------ */
/* Checker: walks the files as the loaders do                             */

#define CHK(cond, ...) do { if (!(cond)) { snprintf(err, (size_t)errlen, __VA_ARGS__); return 0; } } while (0)

static int checkBg(const LgBuf *bgf, char *err, int errlen)
{
	const uint8_t *d = bgf->data;
	uint32_t inf, s1, comp, tab, rows, o, nroom;
	uint32_t s2, s2n, s3, s3n, nv, nc, opa, gdl, vtx, col, tri;
	uint32_t texs[8], ntex = 0, ti, tj;
	const uint8_t *prim, *room;
	uint32_t ptr0, ptr1;

	CHK(bgf->size >= 0x40, "bg: file too small");
	inf = rd32(d); s1 = rd32(d + 4); comp = rd32(d + 8);
	CHK(comp == inf, "bg: stored primary size %u differs from %u", comp, inf);
	CHK(12 + s1 <= bgf->size, "bg: section 1 runs off the file");
	CHK(!(d[12] == 0x11 && d[13] == 0x73), "bg: primary looks rzip");
	prim = d + 12;
	CHK(rd32(prim) == 0, "bg: first primary word is not 0");
	tab = rd32(prim + 4) - SEG_BG;
	CHK(tab == 0x18, "bg: room table not at 0x18");
	rows = 0;
	for (o = tab + 20; rd32(prim + o) != 0; o += 20) {
		rows++;
		CHK(o + 20 <= inf, "bg: room table runs off the primary block");
	}
	nroom = rows - 1;               /* the last row is the end pointer */
	CHK(nroom == 1, "bg: %u rooms, expected 1", nroom);
	CHK(rd32(prim + 8) - SEG_BG + 8 <= inf, "bg: portal table out of range");
	/* the port grows this block when it widens the pointers and has a tenth of
	 * the declared size to do it in: the block must be at least twice what it uses */
	CHK(inf >= 2 * (rd32(prim + 12) - SEG_BG + 16), "bg: primary block %u has too little room to be widened", inf);
	CHK(rd32(prim + rd32(prim + 8) - SEG_BG) == 0, "bg: portal table not empty");
	ptr0 = rd32(prim + tab + 20);
	ptr1 = rd32(prim + tab + 40);
	CHK(ptr0 == SEG_BG + inf, "bg: room 1 not straight after the primary block");
	CHK(12 + comp + (ptr1 - ptr0) <= bgf->size, "bg: room data runs off the file");
	CHK(12 + comp + (ptr1 - ptr0) == 12 + s1, "bg: section 1 size %u does not end at the rooms", s1);

	room = d + 12 + comp;
	CHK(rd32(room) > ptr0 && rd32(room) < ptr1, "bg: vertex pointer outside the room");
	nv = rd16(room + 0x14); nc = rd16(room + 0x16);
	CHK(nv == nc && nv > 0 && (nv & 1) == 0, "bg: %u vertices, %u colours", nv, nc);
	opa = rd32(room + 8) - ptr0;
	CHK(opa == 0x18 && room[opa] == 0, "bg: no opaque leaf");
	gdl = rd32(room + opa + 8) - ptr0;
	vtx = rd32(room) - ptr0;
	col = rd32(room + 4) - ptr0;
	CHK(vtx == rd32(room + opa + 12) - ptr0 && col == rd32(room + opa + 16) - ptr0, "bg: leaf pointers disagree");
	CHK(col == vtx + 12 * nv && gdl >= col + 4 * nv, "bg: arrays overlap");

	/* the display list */
	{
		uint32_t cur = 0, ended = 0;

		for (tri = gdl; tri + 8 <= ptr1 - ptr0; tri += 8) {
			const uint32_t w0 = rd32(room + tri), w1 = rd32(room + tri + 4), op = w0 >> 24;

			if (op == 0x04) {
				const uint32_t n = ((w0 >> 20) & 0xf) + 1, v0 = (w0 >> 16) & 0xf;

				CHK(v0 == 0 && (w0 & 0xffff) == n * 12, "bg: odd vertex load %08x", w0);
				CHK((w1 >> 24) == 0x0e && (w1 & 0xffffff) / 12 + n <= nv, "bg: vertex load %08x past the vertices", w1);
				cur = n;
			} else if (op == 0xb1) {
				uint32_t q;

				for (q = 0; q < 4; q++) {
					const uint32_t a = (w1 >> (8 * q)) & 0xf, b = (w1 >> (8 * q + 4)) & 0xf, c = (w0 >> (4 * q)) & 0xf;

					if (a == 0 && b == 0 && c == 0) continue;
					CHK(a < cur && b < cur && c < cur, "bg: triangle %u,%u,%u past the %u loaded vertices", a, b, c, cur);
					CHK(a != b && b != c && a != c, "bg: degenerate triangle");
				}
			} else if (op == 0xc0) {
				/* tiled, not clamped or mirrored: the walls are bigger than one copy */
				CHK(((w0 >> 20) & 0xf) == 0, "bg: texture command %08x does not wrap in s and t", w0);
				CHK(ntex < 8, "bg: more than 8 texture commands");
				texs[ntex++] = w1 & 0xfff;
			} else if (op == 0xb8) {
				ended = 1;
				break;
			}
		}
		CHK(ended, "bg: display list has no end");
	}

	/* sections 2 and 3 */
	s2 = 12 + s1;
	CHK(s2 + 4 <= bgf->size, "bg: section 2 missing");
	s2n = rd16(d + s2 + 2);
	CHK((rd16(d + s2) & 0x7fff) == s2n && s2n >= 2 && (s2n & 1) == 0, "bg: section 2 header");
	CHK(rd16(d + s2 + 4) != 0x1173, "bg: first texture number looks like rzip");
	/* every texture the display list names is in the list the loader loads */
	for (ti = 0; ti < ntex; ti++) {
		for (tj = 0; tj < s2n / 2 && rd16(d + s2 + 4 + 2 * tj) != texs[ti]; tj++);
		CHK(tj < s2n / 2, "bg: texture %#x is used but not listed in section 2", texs[ti]);
	}
	CHK(ntex == 3, "bg: %u texture commands, expected one per surface", ntex);
	s3 = s2 + 4 + s2n;
	CHK(s3 + 4 <= bgf->size, "bg: section 3 missing");
	s3n = rd16(d + s3 + 2);
	CHK((rd16(d + s3) & 0x7fff) == s3n && s3n == nroom * (12 + 2 + 1), "bg: section 3 is %u bytes", s3n);
	CHK(s3 + 4 + s3n <= bgf->size, "bg: section 3 runs off the file");
	return 1;
}

static int checkTiles(const LgBuf *f, char *err, int errlen)
{
	Buf d = { 0 };
	uint32_t count, i, o, end, floors = 0, walls = 0;

	CHK(rzipUnstore(f->data, f->size, &d), "tiles: not a valid stored rzip");
	count = rd32(d.v);
	CHK(count == 2, "tiles: %u rooms", count);
	end = rd32(d.v + 4 + 4 * count);
	CHK(end <= d.n, "tiles: end offset past the file");
	CHK(rd32(d.v + 4) == rd32(d.v + 8), "tiles: room 0 is not empty");
	for (o = rd32(d.v + 4), i = 0; o < end; i++) {
		const uint8_t *t = d.v + o;
		const uint32_t n = t[1], flags = rd16(t + 2);

		CHK(t[0] == 0 && n >= 3 && n <= 15, "tiles: tile %u has type %u, %u points", i, t[0], n);
		if (flags == LGF_FLOOR_FLAGS) floors++;
		else if (flags == LGF_WALL_FLAGS) walls++;
		else CHK(0, "tiles: tile %u has flags %04x", i, flags);
		o += 14 + 6 * n;
	}
	CHK(o == end, "tiles: bodies end at %u, not %u", o, end);
	CHK(floors == 1 && walls == 4, "tiles: %u floors, %u walls", floors, walls);
	free(d.v);
	return 1;
}

static int checkPads(const LgBuf *f, char *err, int errlen)
{
	Buf d = { 0 };
	uint32_t n, wp, wg, cover, i;

	CHK(rzipUnstore(f->data, f->size, &d), "pads: not a valid stored rzip");
	n = rd32(d.v); wp = rd32(d.v + 8); wg = rd32(d.v + 12); cover = rd32(d.v + 16);
	CHK(n == LGBOX_NUMPADS, "pads: %u pads", n);
	CHK(wp < wg && wg < cover && cover <= d.n, "pads: section offsets %u %u %u of %u", wp, wg, cover, d.n);
	for (i = 0; i < n; i++) {
		const uint32_t off = rd16(d.v + 0x14 + 2 * i);
		const uint32_t hdr = rd32(d.v + off);

		CHK(off >= 0x14 + 2 * n && off + 40 <= wp, "pads: pad %u at %u", i, off);
		CHK(((hdr >> 4) & 0x3ff) == 1 && (hdr >> 14) == 0, "pads: pad %u header %08x", i, hdr);
	}
	CHK(rd32(d.v + wp) == 0xffffffffu, "pads: waypoint list not closed");
	free(d.v);
	return 1;
}

static int checkSetup(const LgBuf *f, char *err, int errlen)
{
	Buf d = { 0 };
	uint32_t intro, props, paths, lists, o;
	int spawns = 0;

	CHK(rzipUnstore(f->data, f->size, &d), "setup: not a valid stored rzip");
	intro = rd32(d.v + 0x0c); props = rd32(d.v + 0x10); paths = rd32(d.v + 0x14); lists = rd32(d.v + 0x18);
	CHK(intro == 0x20 && intro < props && props < paths && paths < lists && lists + 8 <= d.n, "setup: header offsets");
	for (o = intro; rd32(d.v + o) != LGF_INTROCMD_END; o += 12) {
		CHK(o < props && rd32(d.v + o) == LGF_INTROCMD_SPAWN, "setup: bad intro command at %u", o);
		CHK(rd32(d.v + o + 4) < LGBOX_NUMPADS, "setup: spawn on pad %u", rd32(d.v + o + 4));
		spawns++;
	}
	CHK(spawns == 3 && o + 4 == props, "setup: %u spawns, intro ends at %u", spawns, o);
	CHK((rd32(d.v + props) & 0xff) == LGF_OBJTYPE_END, "setup: props not closed");
	CHK(rd32(d.v + paths) == 0 && rd32(d.v + paths + 4) == 0, "setup: paths not closed");
	CHK(lists + 16 <= d.n, "setup: ai list table runs off the file");
	CHK(rd32(d.v + lists) != 0 && rd32(d.v + lists) < lists && rd16(d.v + rd32(d.v + lists)) == LGF_AICMD_END,
			"setup: the AI list table's first list is not an end command");
	CHK(rd32(d.v + lists + 4) == LGF_AILIST_ID, "setup: AI list id %u", rd32(d.v + lists + 4));
	CHK(rd32(d.v + lists + 8) == 0 && rd32(d.v + lists + 12) == 0, "setup: ai lists not closed");
	free(d.v);
	return 1;
}

int modLevelGenFilesCheck(const LgFiles *f, char *err, int errlen)
{
	return checkBg(&f->bg, err, errlen) && checkTiles(&f->tiles, err, errlen)
		&& checkPads(&f->pads, err, errlen) && checkSetup(&f->setup, err, errlen);
}

/* ------------------------------------------------------------------ */
/* Phase 1: the files of a whole layout (modlevelgen.c)                   */
/*
 * Every area of the layout (a room or a convex piece of corridor) is one bg
 * room, numbered area + 1. Per area:
 *   - floor and ceiling in quads of at most LGL_QUAD cells a side;
 *   - a wall along every cell edge of its rectangle that is not open, and
 *     above an open edge, from the opening's top (the lower of the two
 *     ceilings) to its own ceiling;
 *   - floor tiles from the floor quads and wall tiles from the closed edges,
 *     in its own tile room, so a position's tile room is its bg room;
 * and every LgPortal is one bg portal: a quad in the opening, the lower area
 * as room1 and the higher (in front of the quad's normal) as room2.
 * Pads are the layout's own, in their area's room; the intro spawns the
 * player on its start pads. Waypoints come in Phase 2: the game builds a
 * graph from the pads when a stage has none (modAlarmBuildPadWaypoints).
 */

#define LGL_QUAD     2                  /* wall runs and floor tiles: cells */
#define LGL_FQUAD    1                  /* floor and ceiling quads: cells a side (one, so the baked light has a vertex every cell) */
#define LGL_UVWRAP   8192               /* s/t units: a multiple of every texture's size (256 texels x 32) */
#define LGL_PADY     1                  /* pads stand this far over the floor */
#define LGL_PADFLAGS (0x0001 | 0x0004)  /* PADFLAG_INTPOS | PADFLAG_UPALIGNTOY */
#define LGL_LIGHTSIZE 0x22              /* struct light */
#define LGL_LIGHTHALF 40                /* a room light's square, half its side */

typedef struct {
	LgBatch *v;
	int n, cap;
	int err;
} LgBatchList;

static LgBatch *lglBatchFor(LgBatchList *l)
{
	if (l->err) {
		return NULL;
	}
	if (l->n == 0 || l->v[l->n - 1].n + 4 > 16) {
		if (l->n == l->cap) {
			int cap = l->cap ? l->cap * 2 : 8;
			LgBatch *nv = (LgBatch *)realloc(l->v, (size_t)cap * sizeof(*nv));

			if (!nv) {
				l->err = 1;
				return NULL;
			}
			l->v = nv;
			l->cap = cap;
		}
		memset(&l->v[l->n], 0, sizeof(l->v[l->n]));
		l->n++;
	}
	return &l->v[l->n - 1];
}

static int32_t lglFloorDiv(int32_t a, int32_t b)
{
	return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/* Aesthetics: how a level looks. One entry per LG_AESTH_ (modlevelgen.h), the
 * level's is lg->theme.aesthetic. Each holds, per room type, the textures of
 * the floor, walls and ceiling (texture, command word, s/t units per 100
 * world units) and the lighting mood, then the railings' and the light
 * fittings' textures. This table is authored data: a new aesthetic is a new
 * entry, and the checker reads the same table.
 * The surface sets are indexed by the area's theme number: 0 lab, 1 office,
 * 2 hall, 3 corridor, 4 storage, 5 outdoors (street, alleys, yards: these
 * have no ceiling, so their ceiling texture is never drawn).
 * Mood: the brightest the room gets (its lights all on; the game's room
 * brightness runs 0-255) and a tint on the baked shading. */
#define LGL_NUMTHEMES 12   /* (9-11 a villa's gardens: hedge, pool, flower bed, 2026-11-16) */ //  /* (6 a villa's roofs, 7 water, 8 a pier's deck: 2026-11-15) */

typedef struct {
	uint16_t tex[3];       /* by LGSURF_ */
	uint32_t word[3];
	int uv[3];
	/* 1: a greyscale (intensity) texture, whose own alpha is its brightness:
	 * drawn with the combine mode the game's own levels give it (Chicago's
	 * asphalt and paving: fc26a004 1ffc93fc), not the usual one (1f1093ff),
	 * which made its dark texels see-through (the sky showed) (2026-11-15) */
	uint8_t grey[3];
	/* walls in height bands (2026-11-15, the canyon's cliffs): above bandy[0]
	 * (world height) band[0] instead of tex[LGSURF_WALL], above bandy[1]
	 * band[1]; 0: none. A tall wall shows layers, not one tile stacked */
	uint16_t band[2];
	int bandy[2];
} LgTexSet;

typedef struct {
	uint8_t bright;
	uint8_t tint[3];
} LgMood;

typedef struct {
	LgTexSet surf[LGL_NUMTHEMES];
	LgMood mood[LGL_NUMTHEMES];
	uint16_t railtex;      /* railings: a see-through panel texture, cut out by its alpha */
	uint32_t railword;
	int railuv;            /* unused since the panels (their s/t are set per tile) */
	uint16_t paneltex;     /* a room's square light panel */
	uint16_t striptex;     /* a corridor's strip light */
	uint16_t racktex;      /* a warehouse rack's faces (0: as the walls), at rackuv */
	int rackuv;
	/* (2026-11-17, Hollow Creek) a second kind of block, its theme, its faces;
	 * and the edge's wall scale (0: the theme's wall) */
	uint8_t rack2theme;
	uint16_t racktex2;
	int rackuv2;
	int edgeuv;
} LgAesthetic;

static const LgAesthetic g_LglAesthetics[LG_AESTH_ALL] = {
	/* LG_AESTH_PLACEHOLDER: the first texture picks (2026-11-12), from several
	 * Combat Simulator maps mixed; kept so that old levels do not change */
	{
		{
			{ { 0x028a, 0x02ef, 0x02ef }, { 0xc0080002u, 0xc0180002u, 0xc0180002u }, { 860, 980, 620 } },   /* lab: mp9, sho */
			{ { 0x011e, 0x0109, 0x0116 }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 760, 1000, 2160 } }, /* office: the commonest of each surface */
			{ { 0x0069, 0x0068, 0x005c }, { 0xc0080202u, 0xc0080002u, 0xc0180202u }, { 840, 1000, 660 } },  /* hall: ame */
			{ { 0x086f, 0x00c0, 0x0064 }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 860, 470, 700 } },   /* corridor: mp5 */
			{ { 0x00c1, 0x00c0, 0x00c0 }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1030, 470, 540 } },  /* storage: crad */
			{ { 0x00c1, 0x0068, 0x00c0 }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1030, 1000, 540 } }, /* outdoors: a stand-in */
		},
		{
			{ 255, { 236, 246, 255 } },   /* lab: cool, brightest */
			{ 250, { 255, 255, 255 } },   /* office */
			{ 245, { 255, 248, 236 } },   /* hall: warm */
			{ 235, { 244, 250, 255 } },   /* corridor */
			{ 225, { 255, 246, 232 } },   /* storage: dimmer, warm */
			{ 215, { 222, 232, 255 } },   /* outdoors: night */
		},
		0x0190, 0xc0180002u, 100,
		0x019c, 0x005d,
	},
	/* LG_AESTH_CHICAGO (2026-11-15, the street's; the user approved the picks
	 * from a picture): all Chicago's own textures, at the scale Chicago gives
	 * each (its median s/t units per world unit) and with its command words
	 * (bar a clamp or a mirror: everything wraps; a mirrored 48-high texture
	 * smeared up a tall wall, seen on the ravine's cliffs) */
	{
		{
			{ { 0x0bac, 0x0172, 0x0191 }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1590, 1090, 1330 } }, /* lab: maroon tile, concrete panels, panel with a red pipe */
			{ { 0x063f, 0x01aa, 0x0174 }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1490, 1160, 980 } },  /* office: wood boards, olive panelling, concrete */
			{ { 0x029a, 0x018f, 0x01aa }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 3670, 1330, 1170 } }, /* hall (a bar): dark wood, olive wainscot, panelling */
			{ { 0x0197, 0x01c0, 0x0174 }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1060, 1020, 980 } },  /* corridor: worn concrete, concrete with a red band */
			{ { 0x01c8, 0x0174, 0x01aa }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 2060, 1020, 1170 }, { 1, 0, 0 } }, /* storage: paving stones, concrete, panelling */
			{ { 0x017f, 0x017d, 0x0174 }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1050, 1800, 980 }, { 1, 0, 0 } },  /* outdoors: asphalt, red brick (no ceiling) */
		},
		{
			{ 250, { 240, 246, 255 } },   /* lab */
			{ 245, { 255, 250, 238 } },   /* office: warm */
			{ 240, { 255, 244, 228 } },   /* hall: a bar, warmer */
			{ 232, { 248, 250, 255 } },   /* corridor */
			{ 225, { 255, 246, 232 } },   /* storage */
			{ 215, { 222, 232, 255 } },   /* outdoors: night */
		},
		0x0190, 0xc0180002u, 100,
		0x019c, 0x005d,
	},
	/* LG_AESTH_CANYON (2026-11-15, the ravine's): a desert canyon outpost, all
	 * the generator's own textures (tools/levelgen/lgtexgen.py): sandstone
	 * cliffs in three layers by height (dark at the foot, pale at the top: the
	 * user found one texture repeated up a tall cliff too much), a rocky desert
	 * floor, huts of rusting tin, weathered boards and riveted steel */
	{
		{
			{ { LGTEX_BOARDS_WEATHERED, LGTEX_TIN_RUST, LGTEX_STEEL_RUST }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 1024, 800 } },
			{ { LGTEX_BOARDS_WEATHERED, LGTEX_TIN_RUST, LGTEX_STEEL_RUST }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 1024, 800 } },
			{ { LGTEX_CONCRETE, LGTEX_STEEL_RUST, LGTEX_TIN_RUST }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 800, 1024 } },
			{ { LGTEX_TREAD_PLATE, LGTEX_STEEL_RUST, LGTEX_STEEL_RUST }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1600, 800, 800 } },
			{ { LGTEX_CONCRETE, LGTEX_STEEL_RUST, LGTEX_TIN_RUST }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 800, 1024 } },
			{ { LGTEX_DESERT, LGTEX_CLIFF_BASE, LGTEX_CLIFF_MID }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 420, 512, 512 }, { 0, 0, 0 },
				{ LGTEX_CLIFF_MID, LGTEX_CLIFF_TOP }, { 280, 760 } },
		},
		{
			{ 245, { 255, 248, 240 } },   /* lab (unused): as an office hut */
			{ 245, { 255, 248, 240 } },   /* office hut */
			{ 240, { 255, 244, 232 } },   /* hall (unused) */
			{ 235, { 255, 246, 236 } },   /* corridor */
			{ 235, { 255, 244, 230 } },   /* storage hut */
			{ 240, { 255, 242, 222 } },   /* outdoors: warm daylight */
		},
		0x0190, 0xc0180002u, 100,
		0x019c, 0x005d,
	},
	/* LG_AESTH_CITY (the street): brick and asphalt outside; inside oak floors
	 * and plaster offices, a dark wood bar, tiled labs, block storerooms. A
	 * texture's scale (s/t units per world unit x 100) is 204800 / the world
	 * units one tile covers: bricks a course every 25, floorboards 64 wide */
	{
		{
			{ { LGTEX_CHECKER_TILES, LGTEX_WALL_TILES, LGTEX_CEILING_TILES }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1280, 1280, 1024 } },
			{ { LGTEX_OAK_FLOOR, LGTEX_PLASTER_BEIGE, LGTEX_CEILING_TILES }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 1024 } },
			{ { LGTEX_DARK_PLANKS, LGTEX_WOOD_PANELLING, LGTEX_DARK_PLANKS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_CONCRETE, LGTEX_BRICK_PAINTED, LGTEX_CONCRETE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 1024, 533 } },
			{ { LGTEX_CONCRETE, LGTEX_BLOCKWORK, LGTEX_CONCRETE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 1280, 533 } },
			{ { LGTEX_ASPHALT, LGTEX_BRICK_RED, LGTEX_CONCRETE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 1024, 533 } },
		},
		{
			{ 252, { 244, 248, 255 } },   /* lab */
			{ 245, { 255, 248, 236 } },   /* office: warm */
			{ 235, { 255, 238, 218 } },   /* hall: a bar, warmer */
			{ 232, { 250, 250, 255 } },   /* corridor */
			{ 225, { 255, 246, 232 } },   /* storage */
			{ 215, { 222, 232, 255 } },   /* outdoors: night */
		},
		0x0190, 0xc0180002u, 100,
		0x019c, 0x005d,
	},
	/* LG_AESTH_ROOFTOP (the rooftops, 2026-11-16): flat roofs of tarred felt,
	 * huts of blockwork, the streets far below asphalt between the faces of
	 * the buildings (brick to the first floors, office glass above), the
	 * towers' storeys steel-panelled, the glass houses marble-floored */
	{
		{
			{ { LGTEX_FLOOR_CONCRETE, LGTEX_PANEL_GREY, LGTEX_CEIL_METAL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 800, 800 } },
			{ { LGTEX_FLOOR_TILES_GREY, LGTEX_PLASTER_BEIGE, LGTEX_CEILING_TILES }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1024, 800, 1024 } },
			{ { LGTEX_MARBLE, LGTEX_PLASTER_BEIGE, LGTEX_LIGHT_PANELS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1024, 800, 800 } },
			{ { LGTEX_CONCRETE, LGTEX_BLOCKWORK, LGTEX_CONCRETE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 1280, 533 } },
			{ { LGTEX_CONCRETE, LGTEX_BLOCKWORK, LGTEX_ROOF_STEEL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 1280, 800 } },
			{ { LGTEX_ROOF_FELT, LGTEX_BLOCKWORK, LGTEX_CONCRETE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 1280, 533 } },
			{ { LGTEX_TREAD_PLATE, LGTEX_BLOCKWORK, LGTEX_CONCRETE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1600, 1280, 533 } },
			{ { LGTEX_ASPHALT, LGTEX_FACADE_BRICK, LGTEX_CONCRETE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 512, 533 }, { 0, 0, 0 },
				{ LGTEX_FACADE_OFFICE, LGTEX_FACADE_OFFICE }, { 800, 3000 } },
			{ { LGTEX_ASPHALT, LGTEX_FACADE_BRICK, LGTEX_CEILING_TILES }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 512, 1024 }, { 0, 0, 0 },
				{ LGTEX_FACADE_OFFICE, LGTEX_FACADE_OFFICE }, { 800, 3000 } },
			{ { LGTEX_ROOF_GRAVEL, LGTEX_FACADE_GLASS, LGTEX_ROOF_STEEL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_TREAD_PLATE, LGTEX_RELAY_CLADDING, LGTEX_ROOF_STEEL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1024, 800, 800 } },
			{ { LGTEX_BAY_FLOOR, LGTEX_BLOCKWORK, LGTEX_ROOF_STEEL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 720, 1280, 800 } },
		},
		{
			{ 245, { 250, 252, 255 } },   /* lab: a tower's storeys */
			{ 248, { 255, 250, 240 } },   /* office: huts */
			{ 252, { 250, 252, 255 } },   /* hall: glass houses */
			{ 238, { 250, 250, 252 } },   /* corridor */
			{ 230, { 252, 248, 240 } },   /* storage */
			{ 250, { 250, 250, 255 } },   /* outdoors: the roofs, daylight */
			{ 250, { 250, 250, 255 } },   /* bridges */
			{ 215, { 230, 236, 250 } },   /* the streets below: in shadow */
			{ 230, { 236, 240, 250 } },   /* under the glass */
			{ 242, { 231, 240, 255 } },
			{ 250, { 255, 241, 224 } },
			{ 248, { 243, 248, 255 } },
		},
		0x0190, 0xc0180002u, 100,
		0x019c, 0x005d,
		LGTEX_FACADE_GLASS, 1300, 10, LGTEX_RELAY_CLADDING, 850,
	},
	/* LG_AESTH_FACILITY (the hub): a steel facility, grey-blue panels, dark
	 * floor plates, white labs, red-brown storerooms */
	{
		{
			{ { LGTEX_LAB_FLOOR, LGTEX_LAB_WHITE_TEAL, LGTEX_LIGHT_PANELS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_FLOOR_TILES_GREY, LGTEX_LAB_GREY_BLUE, LGTEX_CEILING_TILES }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1024, 800, 1024 } },
			{ { LGTEX_PANEL_DARK, LGTEX_PANEL_GREY, LGTEX_CEIL_METAL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_TREAD_PLATE, LGTEX_PANEL_OLIVE, LGTEX_CEIL_METAL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1600, 800, 800 } },
			{ { LGTEX_PANEL_DARK, LGTEX_PANEL_RED, LGTEX_CEIL_METAL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_CONCRETE, LGTEX_BLOCKWORK, LGTEX_CONCRETE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 1280, 533 } },
		},
		{
			{ 255, { 236, 246, 255 } },   /* lab: cool, brightest */
			{ 250, { 248, 250, 255 } },   /* office */
			{ 245, { 240, 246, 255 } },   /* hall: cool */
			{ 235, { 250, 252, 244 } },   /* corridor */
			{ 225, { 255, 244, 236 } },   /* storage: warmer */
			{ 215, { 222, 232, 255 } },   /* outdoors */
		},
		0x0190, 0xc0180002u, 100,
		0x019c, 0x005d,
	},
	/* LG_AESTH_LABS (the underground lab): white labs with a teal stripe, grey
	 * halls with a blue one, light panels overhead, carpeted offices */
	{
		{
			{ { LGTEX_LAB_FLOOR, LGTEX_LAB_WHITE_TEAL, LGTEX_LIGHT_PANELS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_CARPET_BLUE, LGTEX_LAB_GREY_BLUE, LGTEX_CEILING_TILES }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 1024 } },
			{ { LGTEX_LAB_FLOOR, LGTEX_PANEL_GREY, LGTEX_LIGHT_PANELS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_FLOOR_TILES_GREY, LGTEX_LAB_GREY_BLUE, LGTEX_LIGHT_PANELS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1024, 800, 800 } },
			{ { LGTEX_CONCRETE, LGTEX_BLOCKWORK, LGTEX_CEIL_METAL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 1280, 800 } },
			{ { LGTEX_CONCRETE, LGTEX_BLOCKWORK, LGTEX_CONCRETE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 1280, 533 } },
		},
		{
			{ 255, { 236, 248, 255 } },   /* lab */
			{ 250, { 250, 250, 255 } },   /* office */
			{ 250, { 244, 250, 255 } },   /* hall: the atrium */
			{ 245, { 244, 250, 255 } },   /* corridor */
			{ 228, { 255, 248, 238 } },   /* storage */
			{ 215, { 222, 232, 255 } },   /* outdoors */
		},
		0x0190, 0xc0180002u, 100,
		0x019c, 0x005d,
	},
	/* LG_AESTH_VILLA (the villa, 2026-11-15: "textures like the Carrington
	 * villa"): terracotta and marble floors, cream stucco, dark beams, flagstone
	 * gardens, clay roof tiles, stone quay walls, water, weathered pier boards */
	{
		{
			{ { LGTEX_MARBLE, LGTEX_STUCCO, LGTEX_BEAMS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1024, 800, 800 } },
			{ { LGTEX_TERRACOTTA, LGTEX_STUCCO, LGTEX_BEAMS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1280, 800, 800 } },
			{ { LGTEX_MARBLE, LGTEX_STUCCO, LGTEX_BEAMS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1024, 800, 800 } },
			{ { LGTEX_TERRACOTTA, LGTEX_STUCCO, LGTEX_BEAMS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1280, 800, 800 } },
			{ { LGTEX_FLAGSTONE, LGTEX_STONE_WALL, LGTEX_BEAMS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_FLAGSTONE, LGTEX_STUCCO, LGTEX_STUCCO }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_ROOF_TILES, LGTEX_STUCCO, LGTEX_STUCCO }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_WATER, LGTEX_STONE_WALL, LGTEX_STONE_WALL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 400, 800, 800 } },
			{ { LGTEX_BOARDS_WEATHERED, LGTEX_STONE_WALL, LGTEX_STONE_WALL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_HEDGE, LGTEX_HEDGE, LGTEX_HEDGE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 640, 640 } },
			{ { LGTEX_POOL_WATER, LGTEX_STUCCO, LGTEX_STUCCO }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 800, 800 } },
			{ { LGTEX_FLOWER_BED, LGTEX_STONE_WALL, LGTEX_STONE_WALL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 800, 800 } },
		},
		{
			{ 250, { 255, 250, 240 } },   /* lab (unused) */
			{ 248, { 255, 246, 232 } },   /* office: rooms, warm */
			{ 252, { 255, 248, 236 } },   /* hall */
			{ 240, { 255, 246, 232 } },   /* corridor */
			{ 232, { 255, 242, 226 } },   /* storage */
			{ 235, { 255, 246, 228 } },   /* outdoors: warm evening */
			{ 235, { 255, 246, 228 } },   /* roofs */
			{ 230, { 240, 248, 255 } },   /* water */
			{ 235, { 255, 246, 228 } },   /* piers */
			{ 235, { 255, 246, 228 } },   /* hedges */
			{ 240, { 244, 250, 255 } },   /* pools */
			{ 240, { 255, 250, 240 } },   /* flower beds */
		},
		0x0190, 0xc0180002u, 100,
		0x019c, 0x005d,
		LGTEX_HEDGE, 640,   /* the hedges' faces (2026-11-16; a pool's or a bed's kerb is the grounds' wall) */
	},
	/* LG_AESTH_HARBOUR (Claude Missions: Pier 17, 2026-11-16): a container
	 * terminal at night. Its own textures: containers blue (the stacks), red
	 * (the stacks round the terminal's edge) and green (the perches and the
	 * open ones), the terminal's concrete, the sheds' sheet walls and bay
	 * floor, the quay's face, black water, the landing pad. */
	{
		{
			{ { LGTEX_FLOOR_TILES_GREY, LGTEX_OFFICE_NAVY, LGTEX_LIGHT_PANELS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1024, 800, 800 } },
			{ { LGTEX_CARPET_BLUE, LGTEX_OFFICE_NAVY, LGTEX_CEILING_TILES }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 1024 } },
			{ { LGTEX_BAY_FLOOR, LGTEX_SHED_WALL, LGTEX_ROOF_STEEL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 400, 800, 800 } },
			{ { LGTEX_DARK_PLANKS, LGTEX_CONTAINER_GREEN, LGTEX_ROOF_STEEL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_FLOOR_CONCRETE, LGTEX_BLOCKWORK, LGTEX_ROOF_STEEL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 1280, 800 } },
			{ { LGTEX_DOCK_CONCRETE, LGTEX_SHED_WALL, LGTEX_ROOF_STEEL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 400, 800, 800 }, { 0, 0, 0 },
				{ LGTEX_CONTAINER_RED, LGTEX_CONTAINER_GREEN }, { 0x3ffffff, 0x3ffffff } },   /* (the bands: put there by hand, lglRoom) */
			{ { LGTEX_ROOF_STEEL, LGTEX_SHED_WALL, LGTEX_ROOF_STEEL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_HARBOUR_WATER, LGTEX_QUAY_WALL, LGTEX_QUAY_WALL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 300, 800, 800 } },
			{ { LGTEX_BOARDS_WEATHERED, LGTEX_QUAY_WALL, LGTEX_QUAY_WALL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_CONTAINER_TOP, LGTEX_CONTAINER_BLUE, LGTEX_CONTAINER_BLUE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_PAD_DECK, LGTEX_CONTAINER_GREEN, LGTEX_CONTAINER_GREEN }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 400, 800, 800 },
				{ 0, 0, 0 }, { LGTEX_CONTAINER_RED, LGTEX_CONTAINER_GREEN }, { 0x3ffffff, 0x3ffffff } },
			{ { LGTEX_DOCK_CONCRETE, LGTEX_SHED_WALL, LGTEX_ROOF_STEEL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 400, 800, 800 } },
		},
		{
			{ 250, { 236, 246, 255 } },   /* the control room: cold light */
			{ 240, { 255, 244, 224 } },   /* offices: warm */
			{ 225, { 255, 240, 214 } },   /* the hall: sodium lamps */
			{ 215, { 240, 248, 240 } },   /* the open containers, the doorways */
			{ 215, { 255, 240, 220 } },   /* the back room */
			{ 190, { 206, 220, 255 } },   /* the yard: night, floodlit */
			{ 190, { 206, 220, 255 } },
			{ 150, { 190, 210, 255 } },   /* the water */
			{ 185, { 206, 220, 255 } },   /* the piers */
			{ 195, { 206, 220, 255 } },   /* the containers' tops */
			{ 200, { 214, 226, 255 } },   /* the perches, the pad */
			{ 190, { 206, 220, 255 } },
		},
		0x0190, 0xc0180002u, 100,
		0x019c, 0x005d,
		LGTEX_CONTAINER_BLUE, 800,   /* the stacks' sides */
	},
	/* LG_AESTH_WOODLAND (Claude Missions: Hollow Creek, 2026-11-17): a
	 * suburb, the woods, an army post, old ruins, by day. The woods are the
	 * edge of every open area (a wall of pines ground to treetop, one tile of
	 * TREE_WALL at 320) and the lone trees (blocks of theme 9); hedges are
	 * blocks of theme 10. A block's faces otherwise: its ground's band[1]
	 * (weatherboard sheds, hesco, old stone); a building's wall the ground's. */
	{
		{
			{ { LGTEX_FLOOR_CONCRETE, LGTEX_BUNKER_OLIVE, LGTEX_CEIL_METAL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 800, 800 } },
			{ { LGTEX_OAK_FLOOR, LGTEX_WALLPAPER, LGTEX_PLASTER_BEIGE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 1024 } },
			{ { LGTEX_FLOOR_CONCRETE, LGTEX_PANEL_OLIVE, LGTEX_ROOF_STEEL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 800, 800 } },
			{ { LGTEX_DARK_PLANKS, LGTEX_PLASTER_BEIGE, LGTEX_PLASTER_BEIGE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 1024 } },
			{ { LGTEX_FLAGSTONES, LGTEX_CRYPT_STONE, LGTEX_CRYPT_STONE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 800, 800 } },
			{ { LGTEX_LAWN, LGTEX_SIDING_CREAM, LGTEX_ROOF_STEEL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 800, 800 }, { 0, 0, 0 },
				{ LGTEX_TREE_WALL, LGTEX_SIDING_CREAM }, { 0x3ffffff, 0x3ffffff } },
			{ { LGTEX_FOREST_FLOOR, LGTEX_TREE_WALL, LGTEX_ROOF_STEEL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 320, 800 }, { 0, 0, 0 },
				{ LGTEX_TREE_WALL, LGTEX_TREE_WALL }, { 0x3ffffff, 0x3ffffff } },
			{ { LGTEX_ASPHALT, LGTEX_SIDING_CREAM, LGTEX_ROOF_STEEL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 800, 800 }, { 0, 0, 0 },
				{ LGTEX_TREE_WALL, LGTEX_SIDING_CREAM }, { 0x3ffffff, 0x3ffffff } },
			{ { LGTEX_GRAVEL_YARD, LGTEX_BUNKER_OLIVE, LGTEX_ROOF_STEEL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 800, 800 }, { 0, 0, 0 },
				{ LGTEX_TREE_WALL, LGTEX_HESCO }, { 0x3ffffff, 0x3ffffff } },
			{ { LGTEX_TREE_TOP, LGTEX_TREE_WALL, LGTEX_TREE_WALL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 320, 320 } },
			{ { LGTEX_HEDGE, LGTEX_HEDGE, LGTEX_HEDGE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 640, 640 } },
			{ { LGTEX_FLAGSTONES, LGTEX_RUIN_STONE, LGTEX_RUIN_STONE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 800, 800 }, { 0, 0, 0 },
				{ LGTEX_TREE_WALL, LGTEX_RUIN_STONE }, { 0x3ffffff, 0x3ffffff } },
		},
		{
			{ 238, { 236, 248, 236 } },   /* the bunker: cold strip lights */
			{ 245, { 255, 244, 224 } },   /* the houses: warm */
			{ 232, { 244, 244, 232 } },   /* barracks, armoury, garage */
			{ 240, { 255, 246, 232 } },   /* doorways */
			{ 190, { 220, 226, 255 } },   /* the crypt: dim */
			{ 250, { 255, 252, 240 } },   /* outdoors: daylight */
			{ 215, { 236, 248, 228 } },   /* under the trees: green shade */
			{ 250, { 255, 252, 240 } },
			{ 250, { 255, 252, 240 } },
			{ 225, { 236, 248, 228 } },
			{ 245, { 255, 252, 240 } },
			{ 245, { 255, 250, 236 } },
		},
		0x0190, 0xc0180002u, 100,
		0x019c, 0x005d,
		LGTEX_TREE_WALL, 320,         /* the lone trees and logs (a tile 640 high) */
		10, LGTEX_HEDGE, 640,         /* the hedges */
		320,                          /* the woods round every open area */
	},
	/* LG_AESTH_PENTHOUSE (2026-11-17): a skyscraper's top three floors by
	 * night. Black marble and walnut below, charcoal carpet in the study
	 * floor, oak and silk in the suites; the stairs cream marble; the lift's
	 * shaft brushed steel; a teak terrace and landing pad against the glass
	 * curtain wall; the city's towers round it all, far below */
	{
		{
			{ { LGTEX_MARBLE_BLACK, LGTEX_WALNUT, LGTEX_CEIL_COFFER }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_CARPET_CHARCOAL, LGTEX_WALNUT, LGTEX_CEIL_COFFER }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_OAK_FLOOR, LGTEX_SILK_WALL, LGTEX_CEIL_COFFER }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_MARBLE, LGTEX_WALNUT, LGTEX_PLASTER_BEIGE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1024, 800, 1024 } },
			{ { LGTEX_TREAD_PLATE, LGTEX_LIFT_STEEL, LGTEX_CEIL_METAL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1600, 800, 800 } },
			{ { LGTEX_TEAK_DECK, LGTEX_FACADE_GLASS, LGTEX_CONCRETE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 400, 533 } },
			{ { LGTEX_PAD_DECK, LGTEX_FACADE_GLASS, LGTEX_CONCRETE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 290, 400, 533 } },
			{ { LGTEX_ASPHALT, LGTEX_FACADE_GLASS, LGTEX_CONCRETE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 400, 533 }, { 0, 0, 0 },
				{ LGTEX_FACADE_OFFICE, LGTEX_FACADE_GLASS }, { 800, 1800 } },
			{ { LGTEX_MARBLE_BLACK, LGTEX_WALNUT, LGTEX_CEIL_COFFER }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_TREAD_PLATE, LGTEX_FACADE_GLASS, LGTEX_CEIL_METAL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1024, 800, 800 } },
		},
		{
			{ 235, { 255, 240, 214 } },   /* storey 1: warm lamplight */
			{ 228, { 255, 236, 208 } },   /* storey 2: the study */
			{ 240, { 255, 244, 226 } },   /* storey 3: the suites */
			{ 240, { 255, 246, 230 } },   /* the stairs */
			{ 230, { 236, 244, 255 } },   /* the shaft: cold */
			{ 175, { 196, 210, 255 } },   /* the terrace: night */
			{ 185, { 206, 218, 255 } },   /* the pad: floodlit */
			{ 140, { 170, 186, 240 } },   /* the city far below */
			{ 235, { 255, 240, 214 } },
			{ 205, { 206, 221, 255 } },   /* exterior fire escape */
		},
		0x0190, 0xc0180002u, 100,
		0x019c, 0x005d,
	},
	/* LG_AESTH_SITE (2026-11-17): a tower going up, by day. Fresh slabs
	 * over ply formwork, block and red-oxide steel where walls will be,
	 * scaffold-board stairs, a hoist's yellow cage; the finished towers of
	 * the city round it */
	{
		{
			{ { LGTEX_SLAB_FORM, LGTEX_BLOCKWORK, LGTEX_FORMWORK }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 1280, 800 } },
			{ { LGTEX_SLAB_FORM, LGTEX_GIRDER, LGTEX_FORMWORK }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_SLAB_FORM, LGTEX_GIRDER, LGTEX_FORMWORK }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_SCAFFOLD_BOARDS, LGTEX_GIRDER, LGTEX_FORMWORK }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_TREAD_PLATE, LGTEX_HOIST_MESH, LGTEX_CEIL_METAL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1600, 640, 800 } },
			{ { LGTEX_SLAB_FORM, LGTEX_FACADE_GLASS, LGTEX_CONCRETE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 400, 533 } },
			{ { LGTEX_PAD_DECK, LGTEX_FACADE_GLASS, LGTEX_CONCRETE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 290, 400, 533 } },
			{ { LGTEX_ASPHALT, LGTEX_FACADE_OFFICE, LGTEX_CONCRETE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 512, 533 }, { 0, 0, 0 },
				{ LGTEX_FACADE_GLASS, LGTEX_FACADE_OFFICE }, { 800, 1800 } },
			{ { LGTEX_SLAB_FORM, LGTEX_GIRDER, LGTEX_FORMWORK }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
		},
		{
			{ 250, { 255, 250, 240 } },   /* storey 1 */
			{ 252, { 255, 252, 244 } },
			{ 255, { 255, 252, 246 } },   /* storey 3: under the sky's light */
			{ 245, { 255, 248, 236 } },
			{ 238, { 250, 248, 236 } },   /* the hoist */
			{ 250, { 250, 250, 255 } },
			{ 250, { 250, 250, 255 } },
			{ 215, { 230, 236, 250 } },   /* the street below: in shadow */
			{ 245, { 255, 250, 240 } },   /* the voids */
		},
		0x0190, 0xc0180002u, 100,
		0x019c, 0x005d,
	},
	/* Switchback Station */
	{
		{
			{ { LGTEX_FLOOR_CONCRETE, LGTEX_STEEL_RUST, LGTEX_CEIL_METAL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 800, 800 } },
			{ { LGTEX_FLOOR_TILES_GREY, LGTEX_WALL_TILES, LGTEX_LIGHT_PANELS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1024, 1280, 800 } },
			{ { LGTEX_PAVING, LGTEX_PANEL_GREY, LGTEX_ROOF_STEEL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1024, 800, 800 }, { 0, 0, 0 },
				{ LGTEX_PANEL_RED, LGTEX_PANEL_GREY }, { 354, 398 } },
			{ { LGTEX_TREAD_PLATE, LGTEX_BLOCKWORK, LGTEX_CEIL_METAL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1600, 1280, 800 } },
			{ { LGTEX_TREAD_PLATE, LGTEX_LAB_GREY_BLUE, LGTEX_CEIL_METAL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1600, 800, 800 } },
			{ { LGTEX_FLOOR_TILES_GREY, LGTEX_LAB_WHITE_TEAL, LGTEX_LIGHT_PANELS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1024, 800, 800 } },
			{ { LGTEX_PANEL_DARK, LGTEX_LAB_GREY_BLUE, LGTEX_LIGHT_PANELS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_TREAD_PLATE, LGTEX_PANEL_DARK, LGTEX_CEIL_METAL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 1600, 800, 800 } },
		},
		{
			{ 232, { 255, 232, 195 } }, { 250, { 252, 250, 239 } },
			{ 249, { 231, 242, 255 } }, { 228, { 253, 229, 190 } },
			{ 243, { 230, 244, 255 } }, { 252, { 223, 246, 255 } },
			{ 250, { 255, 244, 226 } }, { 236, { 245, 240, 231 } },
		},
		0x0190, 0xc0180002u, 100, 0x019c, 0x005d,
	},
	/* LG_AESTH_MARKET (2026-11-18): a walled market town by day. Hexagonal
	 * terracotta and limewash in the shops, a timber-framed tavern, stone
	 * cellars; granite setts in the lanes between saffron houses, fan-laid
	 * cobbles in the square between rose ones; a tiered fountain of wet stone
	 * and teal water, a limestone column and plinths; flagged courtyards in
	 * the corners; the town wall of rubble stone round it all, red barrel
	 * tiles on the roofs */
	{
		{
			{ { LGTEX_HEX_TERRACOTTA, LGTEX_LIMEWASH, LGTEX_TIMBER_JOISTS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 800, 800 } },
			{ { LGTEX_DARK_PLANKS, LGTEX_HALF_TIMBER, LGTEX_TIMBER_JOISTS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 640, 800 } },
			{ { LGTEX_FLAGSTONES, LGTEX_ASHLAR_LIME, LGTEX_TIMBER_JOISTS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 800, 800 } },
			{ { LGTEX_FLAGSTONES, LGTEX_ASHLAR_LIME, LGTEX_TIMBER_JOISTS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 800, 800 } },
			{ { LGTEX_FLAGSTONES, LGTEX_TOWN_WALL, LGTEX_TIMBER_JOISTS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 800, 800 } },
			{ { LGTEX_ALLEY_SETTS, LGTEX_STUCCO_SAFFRON, LGTEX_CLAY_ROOF_DEEP }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 800, 800 }, { 0, 0, 0 },
				{ LGTEX_TOWN_WALL, LGTEX_SHUTTERS_BLUE }, { 0x3ffffff, 0x3ffffff } },
			{ { LGTEX_CLAY_ROOF_DEEP, LGTEX_STUCCO_SAFFRON, LGTEX_CLAY_ROOF_DEEP }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 800, 800 } },
			{ { LGTEX_FOUNTAIN_TEAL, LGTEX_BASIN_STONE, LGTEX_BASIN_STONE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 400, 640, 640 } },
			{ { LGTEX_PLAZA_FAN, LGTEX_STUCCO_ROSE, LGTEX_CLAY_ROOF_DEEP }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 533, 800, 800 }, { 0, 0, 0 },
				{ LGTEX_TOWN_WALL, LGTEX_SHUTTERS_BLUE }, { 0x3ffffff, 0x3ffffff } },
			{ { LGTEX_ASHLAR_LIME, LGTEX_ASHLAR_LIME, LGTEX_ASHLAR_LIME }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 640, 640 } },
			{ { LGTEX_FOUNTAIN_TEAL, LGTEX_BASIN_STONE, LGTEX_BASIN_STONE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 400, 640, 640 } },
			{ { LGTEX_FLAGSTONES, LGTEX_HALF_TIMBER, LGTEX_CLAY_ROOF_DEEP }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 640, 800 }, { 0, 0, 0 },
				{ LGTEX_TOWN_WALL, LGTEX_SHUTTERS_BLUE }, { 0x3ffffff, 0x3ffffff } },
		},
		{
			{ 245, { 255, 240, 214 } },   /* the shops: warm */
			{ 230, { 255, 228, 196 } },   /* the tavern: firelight */
			{ 240, { 255, 246, 230 } },
			{ 240, { 255, 246, 230 } },   /* doorways */
			{ 205, { 230, 230, 240 } },   /* the cellars: dim */
			{ 245, { 255, 246, 226 } },   /* the lanes: afternoon sun, shaded */
			{ 250, { 255, 250, 236 } },
			{ 250, { 240, 255, 252 } },   /* the water */
			{ 255, { 255, 250, 236 } },   /* the square: full sun */
			{ 255, { 255, 250, 236 } },
			{ 255, { 245, 255, 252 } },   /* the fountain */
			{ 240, { 255, 248, 230 } },   /* the courtyards */
		},
		0x0190, 0xc0180002u, 100,
		0x019c, 0x005d,
		LGTEX_ASHLAR_LIME, 640,       /* the column's and plinths' faces */
		10, LGTEX_BASIN_STONE, 640,   /* the fountain's basin and bowl */
		0,
	},
	/* LG_AESTH_MALL (2026-11-19): an 80s shopping mall. Cream terrazzo with
	 * pastel chips in the concourse and on the galleries, white tiles with
	 * magenta and teal stripes on its walls, a dropped ceiling of acoustic
	 * tiles and lights; the plus open to a skylight; grooved escalator steps
	 * between chrome; the shops in a squiggle carpet and a pink neon grid,
	 * black and white checks and coral stucco, a plum confetti carpet and
	 * teal glass blocks; the department stores in rose terrazzo and pinstripe
	 * wallpaper over a teal dado;
	 * vinyl tiles in the hallways behind; mint and pink tiles in the
	 * washroom; outside, a car park's bays before tan block with pink and
	 * teal stripes */
	{
		{
			{ { LGTEX_TERRAZZO_80S, LGTEX_MALL_WALL_NEON, LGTEX_ACOUSTIC_80S }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 600, 600 } },
			{ { LGTEX_TERRAZZO_80S, LGTEX_MALL_WALL_NEON, LGTEX_ACOUSTIC_80S }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 600, 600 } },
			{ { LGTEX_TERRAZZO_80S, LGTEX_GLASS_BLOCK, LGTEX_SKYLIGHT_GRID }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 400, 800 } },
			{ { LGTEX_ESCALATOR_STEPS, LGTEX_CHROME_PANEL, LGTEX_SKYLIGHT_GRID }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 200, 400, 800 } },
			{ { LGTEX_CHROME_PANEL, LGTEX_CHROME_PANEL, LGTEX_CHROME_PANEL }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 400, 400, 400 } },
			{ { LGTEX_PARKING_LOT, LGTEX_MALL_FACADE, LGTEX_MALL_FACADE }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 800, 800 } },
			{ { LGTEX_CARPET_MEMPHIS, LGTEX_NEON_GRID, LGTEX_ACOUSTIC_80S }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 600, 600 } },
			{ { LGTEX_CHECKER_BW, LGTEX_CORAL_STUCCO, LGTEX_ACOUSTIC_80S }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 400, 800, 600 } },
			{ { LGTEX_VCT_SPECKLE, LGTEX_BLOCKWORK, LGTEX_ACOUSTIC_80S }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 400, 800, 600 } },
			{ { LGTEX_BATH_TILE, LGTEX_BATH_TILE, LGTEX_ACOUSTIC_80S }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 400, 400, 600 } },
			{ { LGTEX_CARPET_CONFETTI, LGTEX_GLASS_BLOCK, LGTEX_ACOUSTIC_80S }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 640, 400, 600 } },
			{ { LGTEX_TERRAZZO_ROSE, LGTEX_DEPT_WALLPAPER, LGTEX_ACOUSTIC_80S }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 600, 600 } },
		},
		{
			{ 250, { 255, 246, 240 } },   /* the concourse: bright */
			{ 250, { 255, 246, 240 } },   /* the galleries */
			{ 255, { 248, 250, 255 } },   /* under the skylight */
			{ 250, { 246, 248, 255 } },   /* the escalators */
			{ 235, { 240, 240, 255 } },   /* the lift */
			{ 245, { 255, 240, 226 } },   /* the car park */
			{ 240, { 255, 228, 250 } },   /* shops: neon pink */
			{ 245, { 255, 244, 236 } },
			{ 210, { 236, 240, 230 } },   /* the hallways behind: fluorescent, dim */
			{ 245, { 240, 255, 248 } },   /* the washroom */
			{ 240, { 236, 244, 255 } },   /* shops: cool */
			{ 250, { 255, 248, 244 } },   /* the department stores */
		},
		0x0190, 0xc0180002u, 100,
		0x019c, 0x005d,
		0, 0,
		0, 0, 0,
		0,
	},
	/* LG_AESTH_PIPEWORKS (2026-11-19): a works hall round a sump. Diamond
	 * tread plate on the low walkways, riveted steel walls; concrete decks
	 * with a yellow safety line and sooty brick up the middle; tread plate
	 * again up top; steel stairs with yellow nosings; the pipes in green
	 * paint with flange bands; the sump's sludge green and foaming; the
	 * catwalks a see-through bar grating; the lift's shaft in hazard
	 * stripes; a roof of dark sheeting and trusses over it all; the pump
	 * room's boiler plate, the control room's green tiles and lino. Its
	 * ladders yellow steel (the rack faces), the pipes' collars in chevrons */
	{
		{
			{ { LGTEX_PW_TREAD, LGTEX_PW_RIVET, LGTEX_PW_TRUSS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 400, 600, 800 } },
			{ { LGTEX_PW_DECK, LGTEX_PW_BRICK, LGTEX_PW_TRUSS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 640, 800 } },
			{ { LGTEX_PW_TREAD, LGTEX_PW_BRICK, LGTEX_PW_TRUSS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 400, 640, 800 } },
			{ { LGTEX_PW_STAIRS, LGTEX_PW_RIVET, LGTEX_PW_TRUSS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 160, 600, 800 } },
			{ { LGTEX_PW_PIPE, LGTEX_PW_PIPE, LGTEX_PW_TRUSS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 400, 800 } },
			{ { LGTEX_PW_SLUDGE, LGTEX_PW_BRICK, LGTEX_PW_TRUSS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 600, 640, 800 } },
			{ { LGTEX_PW_GRATE, LGTEX_PW_RIVET, LGTEX_PW_TRUSS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 400, 600, 800 } },
			{ { LGTEX_PW_TREAD, LGTEX_PW_HAZARD, LGTEX_PW_TRUSS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 400, 300, 800 } },
			{ { LGTEX_PW_DECK, LGTEX_PW_BOILER, LGTEX_PW_TRUSS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 600, 800 } },
			{ { LGTEX_PW_LINO, LGTEX_PW_TILE, LGTEX_CEILING_TILES }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 400, 400, 400 } },
			{ { LGTEX_PW_TREAD, LGTEX_PW_BOILER, LGTEX_PW_TRUSS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 400, 600, 800 } },
			{ { LGTEX_PW_DECK, LGTEX_PW_BRICK, LGTEX_PW_TRUSS }, { 0xc0080002u, 0xc0080002u, 0xc0080002u }, { 800, 640, 800 } },
		},
		{
			{ 215, { 255, 236, 206 } },   /* the low walkways: sodium lamps */
			{ 220, { 255, 238, 212 } },
			{ 225, { 250, 240, 222 } },
			{ 215, { 255, 236, 206 } },
			{ 225, { 236, 255, 226 } },   /* the pipes over the sludge's glow */
			{ 235, { 200, 255, 170 } },   /* the sump: green */
			{ 220, { 240, 255, 228 } },
			{ 215, { 255, 232, 196 } },
			{ 210, { 255, 224, 190 } },   /* the pump room */
			{ 245, { 226, 244, 255 } },   /* the control room: fluorescent */
			{ 205, { 255, 214, 170 } },   /* the boiler gallery: hot */
			{ 210, { 250, 236, 214 } },
		},
		0x0190, 0xc0180002u, 100,
		0x019c, 0x005d,
		LGTEX_PW_LADDER, 200,
		0, LGTEX_PW_HAZARD, 200,
		0,
	},
};

static const LgAesthetic *lglAesthetic(const LgLayout *lg);

static int lglEdgeUv(const LgLayout *lg, const LgTexSet *T)
{
	return lglAesthetic(lg)->edgeuv ? lglAesthetic(lg)->edgeuv : T->uv[LGSURF_WALL];
}

static const LgAesthetic *lglAesthetic(const LgLayout *lg)
{
	return &g_LglAesthetics[lg->theme.aesthetic < LG_AESTH_ALL ? lg->theme.aesthetic : 0];
}

/* the surfaces of an area's theme (th: unused now that every set is in the table) */
static LgTexSet lglTheme(const LgLayout *lg, const LgBoxParams *th, int theme)
{
	(void)th;
	return lglAesthetic(lg)->surf[theme >= 0 && theme < LGL_NUMTHEMES ? theme : 1];
}

#define LGL_MAXLIGHTS  12
#define LGL_MAXNEAR    96       /* lights an area's shading looks at: its own and its neighbours' */
#define LGL_LIGHTSPACE 400      /* about one light per this many world units each way */

static int lglMoodTheme(const LgLayout *lg, int ai)
{
	const int t = lg->areas[ai].theme;

	return t >= 0 && t < LGL_NUMTHEMES ? t : 1;
}

/* Every room's brightness range is the game's usual 128..255. A room's
 * brightness only ever caps its vertex colours (roomHighlight()), and it is a
 * whole-room number with a hard edge at every opening, so a lower one showed
 * as rooms changing brightness as the player crossed from one to the next
 * (tester 2026-11-14). How bright a room type is now goes into the baked
 * colours instead (lglAreaFactor), which run on smoothly through openings. */
int modLevelGenAreaBrightness(const LgLayout *lg, int ai)
{
	(void)lg;
	(void)ai;
	return 255;
}

static int32_t lglAreaFactor(const LgLayout *lg, int ai)
{
	return lglAesthetic(lg)->mood[lglMoodTheme(lg, ai)].bright * 1000 / 255;
}

/* the lights of one area: a grid of fittings in its ceiling, each on a corner
 * of the floor and ceiling quads (one cell each) so a vertex sits right under
 * it and the pool of light is centred on it */
static int lglLights(const LgLayout *lg, int ai, int32_t pos[][2])
{
	const LgArea *a = &lg->areas[ai];
	const int32_t C = LG_CELL, W = a->w * C, D = a->d * C;
	const int32_t X0 = lg->originx + a->x * C, Z0 = lg->originz + a->z * C;
	int nx = (W + LGL_LIGHTSPACE / 2) / LGL_LIGHTSPACE, nz = (D + LGL_LIGHTSPACE / 2) / LGL_LIGHTSPACE, ix, iz, n = 0;

	/* Outdoors there is no ceiling to hang a light from, so there are none: the
	 * sky lights it evenly (LGL_SKYLIGHT in the baked shading), and the game
	 * gives a room with no lights four fifths of its brightness range, fixed.
	 * (Lights with no fitting were tried first: stray shots put them out and
	 * the alleys went dark, 2026-11-14.) */
	if (a->open || a->special == LG_SPECIAL_WINDOW || (lg->theme.layout == LG_LAYOUT_PIPEWORKS && a->layer)) {   /* nor in a window: the rooms either side light it; nor on a pipe or catwalk: the hall's roof lights them */
		return 0;
	}

	nx = nx < 1 ? 1 : (nx > 4 ? 4 : nx);
	nz = nz < 1 ? 1 : (nz > 3 ? 3 : nz);
	for (iz = 0; iz < nz; iz++) {
		for (ix = 0; ix < nx; ix++) {
			if (pos) {
				int cx = ((2 * ix + 1) * a->w + nx) / (2 * nx);
				int cz = ((2 * iz + 1) * a->d + nz) / (2 * nz);

				if (a->w >= 2) cx = cx < 1 ? 1 : (cx > a->w - 1 ? a->w - 1 : cx);
				if (a->d >= 2) cz = cz < 1 ? 1 : (cz > a->d - 1 ? a->d - 1 : cz);
				pos[n][0] = a->w >= 2 ? X0 + cx * C : X0 + C / 2;
				pos[n][1] = a->d >= 2 ? Z0 + cz * C : Z0 + C / 2;
				/* a corridor's strips sit evenly along it, not on the grid, so
				 * each strip has the same room either side (lglFitting) */
				if (a->kind == LG_AREA_CORRIDOR) {
					if (a->w >= a->d) pos[n][0] = X0 + (2 * ix + 1) * W / (2 * nx);
					else pos[n][1] = Z0 + (2 * iz + 1) * D / (2 * nz);
				}
			}
			n++;
		}
	}
	return n;
}

/* The fittings: what a light looks like in the ceiling. Perfect Dark draws its
 * lights as textured panels drawn at full brightness (vertex colour 255); these
 * two are the commonest ceiling textures that its rooms only ever draw that
 * way (tools/levelgen/pdlight.py), each mapped once across the panel as the
 * game maps it:
 *   0x19c  a square panel, s and t 0..1024 (depo, mp10, pete)
 *   0x05d  a strip light, s 0..4096 along it, t 0..512 across (ame, mp5)
 * Rooms get panels, corridors strips along their length. The light's own box
 * (the part a shot has to hit) is the fitting. */
#define LGL_PANELHALF  50
#define LGL_STRIPHALFL 150
#define LGL_STRIPHALFW 20

static void lglFitting(const LgLayout *lg, int ai, int32_t *texnum, int32_t *halfx, int32_t *halfz, int32_t *slen, int32_t *tlen, int *alongx)
{
	const LgArea *a = &lg->areas[ai];

	if (a->kind == LG_AREA_CORRIDOR) {
		/* no longer than the corridor piece less a margin, or the strip would
		 * hang out through its end wall (seen on the map, 2026-11-14) */
		const int32_t len = (a->w >= a->d ? a->w : a->d) * LG_CELL;
		const int nmax = a->w >= a->d ? 4 : 3;      /* as lglLights() */
		int nl = (len + LGL_LIGHTSPACE / 2) / LGL_LIGHTSPACE;
		int32_t hl;

		nl = nl < 1 ? 1 : (nl > nmax ? nmax : nl);
		hl = len / (2 * nl) - 30 < LGL_STRIPHALFL ? len / (2 * nl) - 30 : LGL_STRIPHALFL;

		if (hl < 30) {
			hl = 30;
		}
		*alongx = a->w >= a->d;
		*texnum = lglAesthetic(lg)->striptex;
		*halfx = *alongx ? hl : LGL_STRIPHALFW;
		*halfz = *alongx ? LGL_STRIPHALFW : hl;
		*slen = 4096 * hl / LGL_STRIPHALFL;   /* the texture at the scale the game draws it */
		*tlen = 512;
	} else {
		*alongx = 1;
		*texnum = lglAesthetic(lg)->paneltex;
		*halfx = *halfz = LGL_PANELHALF;
		*slen = *tlen = 1024;
	}
}

int modLevelGenAreaFitting(const LgLayout *lg, int ai, int32_t *halfx, int32_t *halfz)
{
	int32_t tex, sl, tl;
	int alongx;

	lglFitting(lg, ai, &tex, halfx, halfz, &sl, &tl, &alongx);
	return (int)tex;
}

int modLevelGenAreaLights(const LgLayout *lg, int ai)
{
	return lglLights(lg, ai, NULL);
}

int modLevelGenAreaLightPos(const LgLayout *lg, int ai, int32_t pos[][2], int max)
{
	int32_t tmp[LGL_MAXLIGHTS][2];
	const int n = lglLights(lg, ai, tmp);
	int i;

	for (i = 0; i < n && i < max; i++) {
		pos[i][0] = tmp[i][0];
		pos[i][1] = tmp[i][1];
	}
	return n < max ? n : max;
}

static uint32_t lglTotalLights(const LgLayout *lg)
{
	uint32_t n = 0;
	int i;

	for (i = 0; i < lg->numareas; i++) {
		n += (uint32_t)lglLights(lg, i, NULL);
	}
	return n;
}

/* Which lights start broken: none. Seeded broken lights dimmed whole rooms
 * (the room's brightness caps every vertex in it), and the player can spawn
 * on any pad, so a dim room could be where the level starts and the
 * brightness jumped at its openings (tester 2026-11-14). Lights can still be
 * shot out. */
int modLevelGenLightBroken(const LgLayout *lg, int ai, int light)
{
	(void)lg;
	(void)ai;
	(void)light;
	return 0;
}

/* A quad whose texture runs with the world: s along sa, t along ta (world
 * coordinates of each corner), shifted by whole repeats so s and t stay small */
static void lglQuad(LgBatchList *l, const int32_t c[4][3], const int32_t sa[4], const int32_t ta[4],
		int uvscale, const int32_t nrm[3])
{
	int32_t uv[4][2], smin, tmin, sb, tb;
	LgBatch *bt;
	int i;

	for (i = 0; i < 4; i++) {
		uv[i][0] = uvOf(sa[i], uvscale);
		uv[i][1] = uvOf(ta[i], uvscale);
	}
	smin = uv[0][0];
	tmin = uv[0][1];
	for (i = 1; i < 4; i++) {
		if (uv[i][0] < smin) smin = uv[i][0];
		if (uv[i][1] < tmin) tmin = uv[i][1];
	}
	sb = lglFloorDiv(smin, LGL_UVWRAP) * LGL_UVWRAP;
	tb = lglFloorDiv(tmin, LGL_UVWRAP) * LGL_UVWRAP;
	for (i = 0; i < 4; i++) {
		uv[i][0] -= sb;
		uv[i][1] -= tb;
	}

	bt = lglBatchFor(l);
	if (!bt || !addQuad(bt, c, uv, nrm)) {
		l->err = 1;
	}
}

#define LGL_LISTS 10   /* a room's batch lists: 0-2 by LGSURF_, 3 fittings, 4 railings, 5 grating, 6-7 wall bands, 8 rack faces, 9 the second kind's */

/* A wall quad (corners: 0 a's foot, 1 b's foot, 2 b's top, 3 a's top; t from
 * height), cut at the texture set's band heights into the wall list and the
 * band lists 6 and 7 */
static void lglWallQuad(LgBatchList *lists, const LgTexSet *T, const int32_t c[4][3], const int32_t sa[4], const int32_t nrm[3])
{
	const int32_t big = 0x3fffffff;
	const int32_t lim[4] = { -big, T->band[0] ? T->bandy[0] : big, T->band[1] ? T->bandy[1] : big, big };
	int b, k;

	for (b = 0; b < 3; b++) {
		int32_t cc[4][3], tt[4];
		int32_t al = c[0][1] > lim[b] ? c[0][1] : lim[b], ah = c[3][1] < lim[b + 1] ? c[3][1] : lim[b + 1];
		int32_t bl = c[1][1] > lim[b] ? c[1][1] : lim[b], bh = c[2][1] < lim[b + 1] ? c[2][1] : lim[b + 1];

		if (al >= ah && bl >= bh) {
			continue;
		}
		if (al > ah) al = ah;   /* (a sloped edge crossing the line: that side shrinks to a point) */
		if (bl > bh) bl = bh;
		memcpy(cc, c, sizeof(cc));
		cc[0][1] = al; cc[1][1] = bl; cc[2][1] = bh; cc[3][1] = ah;
		for (k = 0; k < 4; k++) {
			tt[k] = -cc[k][1];
		}
		lglQuad(b == 0 ? &lists[LGSURF_WALL] : &lists[5 + b], cc, sa, tt, T->uv[LGSURF_WALL], nrm);
	}
}

/* An edge of an area's rectangle, one cell long */
typedef struct {
	int32_t ax, az, bx, bz;   /* world ends, in the floor's winding order */
	int32_t bottom;           /* 0 for a wall (a mezzanine's floor), the opening's top above an open edge */
	int32_t top;              /* the wall's top: the ceiling, or over a mezzanine's undercroft just its slab */
	int open;
	int side;                 /* 0-3, as lglEdges() walks them; a run never turns a corner */
	int32_t holelo, holehi;   /* a hole in the wall (an upper window looking out), or 0 0 */
	int nb;                   /* the area across an open edge, or -1 */
	int32_t topa, topb;       /* a sloping top (under a villa's roof): at a and at b; LGL_FLATTOP: top */
} LgEdge;

#define LGL_FLATTOP (-0x7fffffff)
#define LGL_VL_GROUNDWALL 640   /* a villa's garden walls over their floor */
#define LGL_VL_PARAPET    150   /* and along the top of the grounds' wall, outside */

/* The area's boundary, cell by cell, in the floor tiles' winding: -x side
 * (z rising), +z side (x rising), +x side (z falling), -z side (x falling). */
/* The floor drawn: a glass floor's is the bottom of the drop under it (the
 * glass is panes over it, walked on; 2026-11-16) */
static int32_t lglFloorDrawn(const LgLayout *lg, int ai, int32_t x, int32_t z)
{
	return lg->areas[ai].special == LG_SPECIAL_GLASS ? lg->areas[ai].floorlo : modLevelGenFloorAt(lg, ai, x, z);
}

/* The rooftops' buildings (2026-11-16): how high the solid cell x z (a
 * building's wall) stands: the highest ceiling of the building's areas round
 * it, any layer, and its roof's slab; -1 if none */
#define LGL_RT_SKYLINE 2100   /* the buildings round the map's edge */
#define LGL_PIER_EDGE (160 + 512)   /* Pier 17's edge: two containers on the ground */
#define LGL_TRAIL_EDGE (160 + 640)  /* Hollow Creek's: the woods, ground to treetop */
static int32_t lglRoomTop(const LgLayout *lg, int room)
{
	int32_t top = -1;
	int i;

	for (i = 0; i < lg->numareas; i++) {
		if (lg->areas[i].room == room && !lg->areas[i].open && lg->areas[i].ceil + LG_MEZZ_SLAB > top) {
			top = lg->areas[i].ceil + LG_MEZZ_SLAB;
		}
	}
	return top;
}

static int32_t lglStructTopAny(const LgLayout *lg, int x, int z);

static int32_t lglStructTop(const LgLayout *lg, int x, int z)
{
	/* (a rooftop tower's upper windows stand in its wall over the roof: the
	 * wall is still drawn to the top, with their holes cut in it, 2026-11-16) */
	if (lg->cells[z][x] || (!LG_LAYOUT_SKYLIKE(lg->theme.layout) && !LG_LAYOUT_FLATBUILT(lg->theme.layout) && (lg->cells2[z][x] || lg->cells3[z][x]))) {
		return -1;
	}
	return lglStructTopAny(lg, x, z);
}

/* (the same, whatever is in the cell: a door or a window through the wall) */
static int32_t lglStructTopAny(const LgLayout *lg, int x, int z)
{
	int32_t top = -1;
	int i;

	for (i = 0; i < lg->numareas; i++) {
		const LgArea *b = &lg->areas[i];

		if (!b->open && x >= b->x - 1 && x <= b->x + b->w && z >= b->z - 1 && z <= b->z + b->d) {
			const int32_t t = b->room >= 0 ? lglRoomTop(lg, b->room) : b->ceil + LG_MEZZ_SLAB;

			top = t > top ? t : top;
		}
	}
	return top;
}

/* A rooftop building's footprint (its room's areas' box, its walls round it)
 * and the outdoor area that draws its top: the biggest one on the ground
 * beside it (the game draws a room only through its openings, so the top is
 * the roof's, seen whenever the roof round it is) */
static int lglRoomBox(const LgLayout *lg, int room, int *x0, int *z0, int *x1, int *z1)
{
	int i, any = 0;

	for (i = 0; i < lg->numareas; i++) {
		const LgArea *b = &lg->areas[i];

		if (b->room != room || b->open) {
			continue;
		}
		if (!any) {
			*x0 = b->x; *z0 = b->z; *x1 = b->x + b->w; *z1 = b->z + b->d;
			any = 1;
		} else {
			if (b->x < *x0) *x0 = b->x;
			if (b->z < *z0) *z0 = b->z;
			if (b->x + b->w > *x1) *x1 = b->x + b->w;
			if (b->z + b->d > *z1) *z1 = b->z + b->d;
		}
	}
	(*x0)--; (*z0)--; (*x1)++; (*z1)++;
	return any;
}

static int lglCapOwner(const LgLayout *lg, int x0, int z0, int x1, int z1)
{
	int i, best = -1;

	for (i = 0; i < lg->numareas; i++) {
		const LgArea *b = &lg->areas[i];

		if (!b->open || b->layer || b->special == LG_SPECIAL_PIT || b->special == LG_SPECIAL_GLASS
				|| b->x > x1 || b->x + b->w < x0 || b->z > z1 || b->z + b->d < z0) {
			continue;
		}
		if (best < 0 || b->w * b->d > lg->areas[best].w * lg->areas[best].d) {
			best = i;
		}
	}
	return best;
}

static int lglStationTrainSide(const LgLayout *lg, const LgArea *a, int side, int nx, int nz)
{
	const int center = lg->gw / 2;
	return lg->theme.layout == LG_LAYOUT_STATION && a->layer == 1
		&& a->theme == LG_STN_PLATFORM && (a->flags & LG_AREAFLAG_BAY)
		&& (a->w > a->d ? ((side & 1) && nz >= center - 3 && nz < center + 3)
			: (!(side & 1) && nx >= center - 3 && nx < center + 3));
}

static int lglCrossLayerNeighbour(const LgLayout *lg, int ai, int side, int x, int z)
{
	const LgArea *a = &lg->areas[ai];
	if (!a->ramp && !a->layer && a->special != LG_SPECIAL_FIREESCAPE) {
		return -1;
	}

	for (int i = 0; i < lg->numportals; i++) {
		const LgPortal *p = &lg->portals[i];
		const int other = p->a == ai ? p->b : p->b == ai ? p->a : -1;
		if (other < 0 || p->drop || a->layer == lg->areas[other].layer) {
			continue;
		}
		const LgArea *b = &lg->areas[other];
		const int route = lg->theme.layout == LG_LAYOUT_PENTHOUSE
			? a->special == LG_SPECIAL_FIREESCAPE || b->special == LG_SPECIAL_FIREESCAPE
			: a->ramp || b->ramp || a->special == LG_SPECIAL_BRIDGE || b->special == LG_SPECIAL_BRIDGE;
		if (!route) {
			continue;
		}

		if ((side & 1) ? p->z0 == p->z1 && p->z0 == z + (side == 1) && x >= p->x0 && x < p->x1
				: p->x0 == p->x1 && p->x0 == x + (side == 2) && z >= p->z0 && z < p->z1) {
			return other;
		}
	}
	return -1;
}

static int lglEdges(const LgLayout *lg, int ai, LgEdge *out, int max)
{
	const LgArea *a = &lg->areas[ai];
	const int32_t C = LG_CELL, ox = lg->originx, oz = lg->originz;
	const uint8_t (*opens)[LG_GRIDMAX] = LG_OPENS(lg, a->layer);
	int n = 0, side, i;

	for (side = 0; side < 4; side++) {
		const int len = (side & 1) ? a->w : a->d;

		for (i = 0; i < len; i++) {
			int cx, cz, nx, nz, open = 0;
			LgEdge *e;

			if (n >= max) {
				return -1;
			}
			e = &out[n++];

			switch (side) {
			case 0: /* -x: x = a->x, z rising */
				cx = a->x; cz = a->z + i; nx = cx - 1; nz = cz;
				e->ax = ox + a->x * C; e->az = oz + cz * C; e->bx = e->ax; e->bz = e->az + C;
				open = nx >= 0 && (opens[cz][nx] & LG_OPEN_XP);
				break;
			case 1: /* +z: z = a->z + a->d, x rising */
				cx = a->x + i; cz = a->z + a->d - 1; nx = cx; nz = cz + 1;
				e->ax = ox + cx * C; e->az = oz + (a->z + a->d) * C; e->bx = e->ax + C; e->bz = e->az;
				open = (opens[cz][cx] & LG_OPEN_ZP) != 0;
				break;
			case 2: /* +x: x = a->x + a->w, z falling */
				cx = a->x + a->w - 1; cz = a->z + a->d - 1 - i; nx = cx + 1; nz = cz;
				e->ax = ox + (a->x + a->w) * C; e->az = oz + (cz + 1) * C; e->bx = e->ax; e->bz = e->az - C;
				open = (opens[cz][cx] & LG_OPEN_XP) != 0;
				break;
			default: /* -z: z = a->z, x falling */
				cx = a->x + a->w - 1 - i; cz = a->z; nx = cx; nz = cz - 1;
				e->ax = ox + (cx + 1) * C; e->az = oz + a->z * C; e->bx = e->ax - C; e->bz = e->az;
				open = nz >= 0 && (opens[nz][cx] & LG_OPEN_ZP);
				break;
			}

			e->open = 0;
			/* (a pit's floor is below 0: its walls go down to it) */
			e->bottom = a->layer ? a->floorlo : (a->floor < 0 ? a->floor : 0);
			e->top = a->ceil;
			e->side = side;
			e->nb = -1;
			e->holelo = e->holehi = 0;
			e->topa = e->topb = LGL_FLATTOP;
			if (lg->theme.layout == LG_LAYOUT_STATION || lg->theme.layout == LG_LAYOUT_PENTHOUSE
					|| lg->theme.layout == LG_LAYOUT_ROOFTOP) {
				const int landing = lglCrossLayerNeighbour(lg, ai, side, cx, cz);
				if (landing >= 0) {
					/* The cell below the landing is a different room. Use the
					 * actual portal neighbour for the wall and riser geometry. */
					const int32_t ceiling = lg->areas[landing].ceil;
					e->open = 1;
					e->nb = landing;
					e->bottom = ceiling < a->ceil ? ceiling : a->ceil;
					if (a->special == LG_SPECIAL_FIREESCAPE) {
						e->top = lglStructTopAny(lg, nx, nz);
					}
					continue;
				}
			}
			if (lglStationTrainSide(lg, a, side, nx, nz)) {
				e->top = LG_STN_CONCOURSE_Y;
			}
			if (!open && a->special == LG_SPECIAL_FIREESCAPE) {
				e->bottom = a->floorlo - LG_MEZZ_SLAB;
				e->top = side == 3 ? lglStructTopAny(lg, nx, nz) : e->bottom;
				continue;
			}
			/* a balcony looks out over this ground area from the next cell's upper
			 * storey: this area's wall there stops at the balcony's floor */
			if (!a->layer && nx >= 0 && nz >= 0 && nx < lg->gw && nz < lg->gd && lg->cells2[nz][nx] > 0) {
				const LgArea *up = &lg->areas[lg->cells2[nz][nx] - 1];
				int open2 = 0;

				switch (side) {
				case 0: open2 = lg->open2[cz][nx] & LG_OPEN_XP; break;
				case 1: open2 = lg->open2[cz][cx] & LG_OPEN_ZP; break;
				case 2: open2 = lg->open2[cz][cx] & LG_OPEN_XP; break;
				default: open2 = lg->open2[nz][cx] & LG_OPEN_ZP; break;
				}
				/* (or over this area's own edge cell too: a balcony out over the
				 * street, whose wall above the porch would cut it in two) */
				if ((open2 || lg->cells2[cz][cx] - 1 == lg->cells2[nz][nx] - 1) && up->special == LG_SPECIAL_BALCONY && up->floor < e->top) {
					e->top = up->floor;
				}
				/* an upper window looks out through this wall: a hole in it from its
				 * sill to its head (2026-11-15) */
				if (open2 && up->special == LG_SPECIAL_WINDOW && up->ceil <= e->top) {
					e->holelo = up->floor;
					e->holehi = up->ceil;
				}
			}
			/* (a tower's third-storey window too, 2026-11-16) */
			if (!a->layer && nx >= 0 && nz >= 0 && nx < lg->gw && nz < lg->gd && lg->cells3[nz][nx] > 0 && !e->holehi) {
				const LgArea *up = &lg->areas[lg->cells3[nz][nx] - 1];
				int open3 = 0;

				switch (side) {
				case 0: open3 = lg->open3[cz][nx] & LG_OPEN_XP; break;
				case 1: open3 = lg->open3[cz][cx] & LG_OPEN_ZP; break;
				case 2: open3 = lg->open3[cz][cx] & LG_OPEN_XP; break;
				default: open3 = lg->open3[nz][cx] & LG_OPEN_ZP; break;
				}
				if (open3 && up->special == LG_SPECIAL_WINDOW && up->ceil <= e->top) {
					e->holelo = up->floor;
					e->holehi = up->ceil;
				}
			}
			/* the rooftops (2026-11-16): outdoors, a wall against a building
			 * stands as high as the building (its top drawn over it, lglRoom);
			 * at the edge of the map the street's walls are the next buildings,
			 * as high as the highest roofs */
			if (!open && !a->layer && a->open && (LG_LAYOUT_SKYLIKE(lg->theme.layout) || LG_LAYOUT_FLATBUILT(lg->theme.layout))) {
				const int32_t st = (nx < 0 || nz < 0 || nx >= lg->gw || nz >= lg->gd) ? -1 : lglStructTop(lg, nx, nz);
				/* (Pier 17: the terminal's edge is containers two high, the water's a harbour wall as high) */
				const int32_t want = st >= 0 ? st : LG_LAYOUT_HARBOURLIKE(lg->theme.layout) ? LGL_PIER_EDGE
					: LG_LAYOUT_WOODSLIKE(lg->theme.layout) ? LGL_TRAIL_EDGE
					: a->special == LG_SPECIAL_PIT ? LGL_RT_SKYLINE : a->floor + 120;

				if (want < e->top) {
					e->top = want;
				}
			}
			/* a ravine bridge lands across this edge, over this cell or the next:
			 * the wall here stops under its deck (2026-11-15) */
			if (!a->layer && nx >= 0 && nz >= 0 && nx < lg->gw && nz < lg->gd) {
				const int bi = lg->cells2[cz][cx] > 0 ? lg->cells2[cz][cx] - 1 : lg->cells2[nz][nx] - 1;
				int open2 = 0;

				switch (side) {
				case 0: open2 = lg->open2[cz][nx] & LG_OPEN_XP; break;
				case 1: open2 = lg->open2[cz][cx] & LG_OPEN_ZP; break;
				case 2: open2 = lg->open2[cz][cx] & LG_OPEN_XP; break;
				default: open2 = lg->open2[nz][cx] & LG_OPEN_ZP; break;
				}
				if (open2 && bi >= 0 && (lg->areas[bi].special == LG_SPECIAL_BRIDGE || lg->areas[bi].special == LG_SPECIAL_PIPE) && lg->areas[bi].floor < e->top) {
					e->top = lg->areas[bi].floor;
				}
				/* (a pipeworks' high catwalk, layer 2, landing the same way) */
				if (lg->theme.layout == LG_LAYOUT_PIPEWORKS) {
					const int b3 = lg->cells3[cz][cx] > 0 ? lg->cells3[cz][cx] - 1 : lg->cells3[nz][nx] - 1;
					int open3 = 0;

					switch (side) {
					case 0: open3 = lg->open3[cz][nx] & LG_OPEN_XP; break;
					case 1: open3 = lg->open3[cz][cx] & LG_OPEN_ZP; break;
					case 2: open3 = lg->open3[cz][cx] & LG_OPEN_XP; break;
					default: open3 = lg->open3[nz][cx] & LG_OPEN_ZP; break;
					}
					if (open3 && b3 >= 0 && lg->areas[b3].special == LG_SPECIAL_BRIDGE && lg->areas[b3].floor < e->top) {
						e->top = lg->areas[b3].floor;
					}
				}
				/* a villa's roof slope over the next cell (2026-11-15): the wall
				 * here follows the roof's line, eave or gable (whether or not
				 * anything opens onto the roof there: a terrace against a wing,
				 * the wall over a door or a window) */
				if (bi >= 0 && lg->areas[bi].special == LG_SPECIAL_ROOF) {
					const int32_t ta = modLevelGenFloorAt(lg, bi, e->ax, e->az), tb = modLevelGenFloorAt(lg, bi, e->bx, e->bz);

					if (ta < e->top && tb < e->top) {
						e->topa = ta;
						e->topb = tb;
						e->top = ta > tb ? ta : tb;
					}
				}
			}
			/* a villa's grounds (2026-11-15): their ceiling stands over the roof
			 * ridges, but their outer walls stand only a storey and a half, so
			 * the sky shows over them; and only where nothing is over the next
			 * cell (a building's walls follow its roof, above) */
			if (!open && !a->layer && a->open && lg->theme.layout == LG_LAYOUT_VILLA && e->topa == LGL_FLATTOP
					&& (nx < 0 || nz < 0 || nx >= lg->gw || nz >= lg->gd || lg->cells2[nz][nx] == 0)
					&& a->floor + (a->floor > 260 ? LGL_VL_PARAPET : LGL_VL_GROUNDWALL) < e->top) {
				e->top = a->floor + (a->floor > 260 ? LGL_VL_PARAPET : LGL_VL_GROUNDWALL);   /* (the grounds' wall's top: a parapet) */
			}

			/* a villa roof's edge with nothing across it (over a ramp up to it,
			 * beside the landing): no wall stands on a roof */
			if (!open && a->special == LG_SPECIAL_ROOF) {
				e->top = e->bottom;
			}
			if (open && nx >= 0 && nz >= 0 && nx < lg->gw && nz < lg->gd) {
				/* a layer 1 area's neighbour is one of layer 1, else the ground's */
				int nbi = -1, l;

				for (l = a->layer; l >= 0 && nbi < 0; l--) {
					nbi = LG_CELLS(lg, l)[nz][nx] - 1;
				}

				if (nbi >= 0) {
					const LgArea *b = &lg->areas[nbi];

					e->open = 1;
					e->nb = nbi;
					e->bottom = b->ceil < a->ceil ? b->ceil : a->ceil;
					if (a->special == LG_SPECIAL_FIREESCAPE && side == 3) {
						e->top = lglStructTopAny(lg, nx, nz);
					}
					/* The three lift doors share this vertical strip in the
					 * shaft. The lobby at ground level must not wall off
					 * the two higher doorways as seen from inside the car. */
					if ((lg->theme.layout == LG_LAYOUT_PENTHOUSE || lg->theme.layout == LG_LAYOUT_MALL) && lg->numlifts && ai == lg->lifts[0].area) {
						e->top = e->bottom;
					}
					if (b->layer == a->layer && (b->flags & LG_AREAFLAG_UNDER) && b->ceil + LG_MEZZ_SLAB < a->ceil) {
						e->top = b->ceil + LG_MEZZ_SLAB;   /* above the slab the mezzanine looks out */
					}
					/* (the rooftops: over a building's door or window, its wall up
					 * to its top, not to the sky's ceiling, 2026-11-16) */
					if ((LG_LAYOUT_SKYLIKE(lg->theme.layout) || LG_LAYOUT_FLATBUILT(lg->theme.layout)) && a->open && !a->layer && !b->open) {
						const int32_t t = b->room >= 0 ? lglRoomTop(lg, b->room) : lglStructTopAny(lg, nx, nz);

						if (t > e->bottom && t < e->top) {
							e->top = t;
						}
					}
				}
			}
		}
	}
#ifdef LGEDGEDBG
	if (getenv("LGEDGEDBG")) {
		int q;

		for (q = 0; q < n; q++) {
			if (out[q].top > 1000 && out[q].bottom < out[q].top && out[q].topa == LGL_FLATTOP) {
				fprintf(stderr, "tall wall: area %d (%d,%d %dx%d f%d L%d sp%d th%d k%d) side %d at %d,%d open %d nb %d bottom %d top %d\n", ai, a->x, a->z, a->w, a->d, a->floor, a->layer, a->special, a->theme, a->kind,
						out[q].side, out[q].ax, out[q].az, out[q].open, out[q].nb, out[q].bottom, out[q].top);
			}
		}
	}
#endif
	return n;
}

/* A ledge's face: across an open edge whose far side's floor stands higher
 * than this area's, the wall from this floor up to that one (a terrace's edge
 * seen from the hub floor below). Gives the four heights; returns 0 if there
 * is none. */
static int lglRiser(const LgLayout *lg, int ai, const LgEdge *e, int32_t *lowa, int32_t *lowb, int32_t *higha, int32_t *highb)
{
	if (!e->open || e->nb < 0) {
		return 0;
	}
	*lowa = lglFloorDrawn(lg, ai, e->ax, e->az);
	*lowb = lglFloorDrawn(lg, ai, e->bx, e->bz);
	*higha = lglFloorDrawn(lg, e->nb, e->ax, e->az);
	*highb = lglFloorDrawn(lg, e->nb, e->bx, e->bz);
	if (*higha < *lowa) *higha = *lowa;
	if (*highb < *lowb) *highb = *lowb;
	if (*higha > lg->areas[ai].ceil) *higha = lg->areas[ai].ceil;   /* a low undercroft: up to its ceiling */
	if (*highb > lg->areas[ai].ceil) *highb = lg->areas[ai].ceil;
	return *higha > *lowa || *highb > *lowb;
}

static int lglEdgeMax(const LgArea *a)
{
	return 2 * (a->w + a->d);
}

/* The four faces of a pillar or a piece of low cover, wound the way an area's
 * walls are (a hole in the floor plan runs the other way round), with the
 * normal each faces out of it. obstTop gives how high it stands. */
static int32_t lglObstTop(const LgLayout *lg, const LgObstacle *o)
{
	return o->height ? modLevelGenFloorAt(lg, o->area, o->x, o->z) + o->height : lg->areas[o->area].ceil;
}

static void lglPillarFace(const LgLayout *lg, const LgObstacle *o, int face, int32_t *ax, int32_t *az, int32_t *bx, int32_t *bz,
		int32_t nrm[3])
{
	const int32_t x0 = o->x - o->halfx, x1 = o->x + o->halfx, z0 = o->z - o->halfz, z1 = o->z + o->halfz;

	(void)lg;
	nrm[0] = nrm[1] = nrm[2] = 0;
	switch (face) {
	case 0: *ax = x0; *az = z0; *bx = x1; *bz = z0; nrm[2] = -1; break;
	case 1: *ax = x1; *az = z0; *bx = x1; *bz = z1; nrm[0] = 1; break;
	case 2: *ax = x1; *az = z1; *bx = x0; *bz = z1; nrm[2] = 1; break;
	default: *ax = x0; *az = z1; *bx = x0; *bz = z0; nrm[0] = -1; break;
	}
}

/*
 * Railings (2026-11-14): a ledge's parapet is a railing: a top rail, a middle
 * rail and posts, thin boxes of a metal texture of their own (the fifth batch
 * of a room, after the light fittings). The tiles under it are a wall that
 * stops walking and nothing else (GEOFLAG_WALL, not BLOCK_SIGHT or
 * BLOCK_SHOOT), so a railing is seen and shot through as the game's own are.
 */
/* A railing (2026-11-15, the user's request: "There already exist sprite
 * railings that look better than the 3d model you've made"): one flat panel
 * along the ledge, from the floor to the top rail, with the game's own railing
 * texture (0x0190: a top rail, bars, a post at each side, a kick plate; 32x48
 * texels, alpha in the gaps; mp5, Crash Site, Depot, Defection), both faces,
 * cut out by its alpha (lglRoom). A tile is about
 * 67 units wide (the texels square at a 100 high railing), stretched so that
 * a whole number of tiles fill the railing and a post stands at each end. */
#define LGL_RAILTILE    67      /* world units a tile of the railing texture spans */
#define LGL_RAILTEXW    32      /* the texture's size in texels */
#define LGL_RAILTEXH    48
#define LGF_RAIL_FLAGS  0x0004  /* collision: GEOFLAG_WALL only, so walked into but seen and shot through */

static int lglIsGlassRail(const LgLayout *lg, const LgObstacle *o)
{
	/* (and the ravine's, 2026-11-15: "Railings should be glass") */
	return (lg->theme.layout == LG_LAYOUT_STREET || lg->theme.layout == LG_LAYOUT_RAVINE || lg->theme.layout == LG_LAYOUT_ROOFTOP || lg->theme.layout == LG_LAYOUT_PENTHOUSE
			|| lg->theme.layout == LG_LAYOUT_MALL) && o->rail;   /* (the mall's galleries, 2026-11-19) */
}

/* A ravine's bridges and catwalk are a see-through grating (2026-11-15: "catwalks
 * should use a seethrough texture, specifically the one already used in the
 * Combat Simulator map Ravine"): texture 0x0243 (16x16 bars with gaps, bg_arec's
 * only translucent texture), cut out by its alpha, seen from both sides, with no
 * slab under it */
#define LGL_GRATETEX   0x0243
#define LGL_GRATEUV    1000    /* a tile about every 50 units */
static int lglIsGrating(const LgLayout *lg, const LgArea *a)
{
	return (lg->theme.layout == LG_LAYOUT_RAVINE || lg->theme.layout == LG_LAYOUT_ROOFTOP || lg->theme.layout == LG_LAYOUT_PIPEWORKS) && a->special == LG_SPECIAL_BRIDGE;   /* (the rooftops' bridges, 2026-11-16; the pipeworks' catwalks, 2026-11-19) */
}

/* ---- the pipeworks' pipes and ladders (2026-11-19) ---- */

/* the pipeworks' own grating, else the ravine's (bg_arec's) */
static uint32_t lglGrateTex(const LgLayout *lg)
{
	return lg->theme.layout == LG_LAYOUT_PIPEWORKS ? (uint32_t)LGTEX_PW_GRATE : (uint32_t)LGL_GRATETEX;
}

/* a pipe's walked strip either side of its axis (its drawn top face is 83
 * wide, the slope beside it 20 under the strip's edge) */
#define LGL_PIPE_WALK  55
#define LGL_PIPE_POSTS 6        /* cells between the posts down into the sludge */
#define LGL_PIPE_COLLAR 20      /* a collar's reach past the joining pipe */

/* whether the main pipe ai has a pipe joining its side at cell column k (along it) */
static int lglPipeJoin(const LgLayout *lg, int ai, int k)
{
	const LgArea *a = &lg->areas[ai];
	const int alongx = a->w >= a->d;
	int s;

	for (s = 0; s < 2; s++) {
		const int x = alongx ? a->x + k : (s ? a->x + a->w : a->x - 1);
		const int z = alongx ? (s ? a->z + a->d : a->z - 1) : a->z + k;
		int o;

		if (x < 0 || z < 0 || x >= lg->gw || z >= lg->gd) {
			continue;
		}
		o = lg->cells2[z][x] - 1;
		if (o >= 0 && o != ai && lg->areas[o].special == LG_SPECIAL_PIPE) {
			return 1;
		}
	}
	return 0;
}

/* whether pipe ai's end (0 low, 1 high along it) meets another pipe */
static int lglPipeEndJoins(const LgLayout *lg, int ai, int end)
{
	const LgArea *a = &lg->areas[ai];
	const int alongx = a->w >= a->d;
	const int x = alongx ? (end ? a->x + a->w : a->x - 1) : a->x;
	const int z = alongx ? a->z : (end ? a->z + a->d : a->z - 1);
	int o;

	if (x < 0 || z < 0 || x >= lg->gw || z >= lg->gd) {
		return 0;
	}
	o = lg->cells2[z][x] - 1;
	return o >= 0 && lg->areas[o].special == LG_SPECIAL_PIPE;
}

/* a box's faces (not its bottom unless bottom) into list l */
static void lglBox(LgBatchList *l, int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1, int32_t z1, int uv, int bottom)
{
	int32_t c[4][3], sa[4], ta[4], nrm[3];
	int f, k;

	for (f = 0; f < 6; f++) {
		if (f == 5 && !bottom) {
			continue;
		}
		switch (f) {
		case 0: /* -x */
			c[0][0] = x0; c[0][1] = y0; c[0][2] = z0; c[1][0] = x0; c[1][1] = y0; c[1][2] = z1;
			c[2][0] = x0; c[2][1] = y1; c[2][2] = z1; c[3][0] = x0; c[3][1] = y1; c[3][2] = z0;
			nrm[0] = -1; nrm[1] = 0; nrm[2] = 0;
			break;
		case 1: /* +x */
			c[0][0] = x1; c[0][1] = y0; c[0][2] = z0; c[1][0] = x1; c[1][1] = y0; c[1][2] = z1;
			c[2][0] = x1; c[2][1] = y1; c[2][2] = z1; c[3][0] = x1; c[3][1] = y1; c[3][2] = z0;
			nrm[0] = 1; nrm[1] = 0; nrm[2] = 0;
			break;
		case 2: /* -z */
			c[0][0] = x0; c[0][1] = y0; c[0][2] = z0; c[1][0] = x1; c[1][1] = y0; c[1][2] = z0;
			c[2][0] = x1; c[2][1] = y1; c[2][2] = z0; c[3][0] = x0; c[3][1] = y1; c[3][2] = z0;
			nrm[0] = 0; nrm[1] = 0; nrm[2] = -1;
			break;
		case 3: /* +z */
			c[0][0] = x0; c[0][1] = y0; c[0][2] = z1; c[1][0] = x1; c[1][1] = y0; c[1][2] = z1;
			c[2][0] = x1; c[2][1] = y1; c[2][2] = z1; c[3][0] = x0; c[3][1] = y1; c[3][2] = z1;
			nrm[0] = 0; nrm[1] = 0; nrm[2] = 1;
			break;
		default: /* top, bottom */
			c[0][0] = x0; c[0][1] = f == 4 ? y1 : y0; c[0][2] = z0; c[1][0] = x1; c[1][1] = c[0][1]; c[1][2] = z0;
			c[2][0] = x1; c[2][1] = c[0][1]; c[2][2] = z1; c[3][0] = x0; c[3][1] = c[0][1]; c[3][2] = z1;
			nrm[0] = 0; nrm[1] = f == 4 ? 1 : -1; nrm[2] = 0;
			break;
		}
		for (k = 0; k < 4; k++) {
			sa[k] = f < 2 ? c[k][2] : c[k][0];
			ta[k] = f < 4 ? -c[k][1] : c[k][2];
		}
		lglQuad(l, (const int32_t (*)[3])c, sa, ta, uv, nrm);
	}
}

/* A pipe: an octagon round its long axis, its top face the walked floor, a
 * cell's length a piece (the baked light has a vertex every cell); a joining
 * pipe's ends reach into the pipes they join; posts down into the sludge;
 * a collar where a pipe joins from the side */
static void lglPipe(const LgLayout *lg, int ai, LgBatchList *lists, const LgTexSet *T)
{
	const LgArea *a = &lg->areas[ai];
	const int32_t C = LG_CELL;
	const int alongx = a->w >= a->d;
	const int len = alongx ? a->w : a->d;
	const int32_t X0 = lg->originx + a->x * C, Z0 = lg->originz + a->z * C;
	const int32_t X1 = X0 + a->w * C, Z1 = Z0 + a->d * C;
	const int32_t R = (alongx ? a->d : a->w) * C / 2;
	const int32_t cy = a->floor - R;
	const int32_t cc = alongx ? (Z0 + Z1) / 2 : (X0 + X1) / 2;
	const int32_t L0 = (alongx ? X0 : Z0) - (lglPipeEndJoins(lg, ai, 0) ? R / 2 : 0);
	const int32_t L1 = (alongx ? X1 : Z1) + (lglPipeEndJoins(lg, ai, 1) ? R / 2 : 0);
	int32_t ox[9], oy[9], arc[9];
	int k, f;

	/* the octagon round the axis (integers: the same everywhere): across, up,
	 * from just right of the bottom round; its faces' arc length for t */
	{
		const int32_t q8 = R * 4142 / 10000;   /* R tan 22.5 */
		const int32_t vx[9] = { q8, R, R, q8, -q8, -R, -R, -q8, q8 };
		const int32_t vy[9] = { -R, -q8, q8, R, R, q8, -q8, -R, -R };

		for (k = 0; k <= 8; k++) {
			ox[k] = vx[k];
			oy[k] = vy[k];
			arc[k] = 2 * q8 * k;
		}
	}
	for (f = 0; f < 8; f++) {
		static const int32_t nx[8] = { 707, 1000, 707, 0, -707, -1000, -707, 0 };
		static const int32_t ny[8] = { -707, 0, 707, 1000, 707, 0, -707, -1000 };
		const int32_t nrm[3] = { alongx ? 0 : nx[f], ny[f], alongx ? nx[f] : 0 };
		int32_t l0;

		for (l0 = L0; l0 < L1; ) {
			const int32_t l1 = l0 + C - ((l0 - L0) % C) < L1 ? l0 + C - ((l0 - L0) % C) : L1;
			int32_t c[4][3], sa[4], ta[4];
			const int32_t ls[4] = { l0, l1, l1, l0 };
			const int ks[4] = { f, f, f + 1, f + 1 };
			int q;

			for (q = 0; q < 4; q++) {
				const int32_t across = cc + ox[ks[q]], up = cy + oy[ks[q]];

				c[q][0] = alongx ? ls[q] : across;
				c[q][1] = up;
				c[q][2] = alongx ? across : ls[q];
				sa[q] = ls[q];
				ta[q] = arc[ks[q]];
			}
			lglQuad(&lists[LGSURF_FLOOR], (const int32_t (*)[3])c, sa, ta, T->uv[LGSURF_FLOOR], nrm);
			l0 = l1;
		}
	}
	/* posts: a main pipe's, down into the sludge */
	if (len >= 2 * LGL_PIPE_POSTS) {
		for (k = LGL_PIPE_POSTS / 2; k < len - 1; k += LGL_PIPE_POSTS) {
			const int32_t at = (alongx ? X0 : Z0) + k * C + C / 2;

			if (lglPipeJoin(lg, ai, k)) {
				continue;
			}
			if (alongx) {
				lglBox(&lists[LGSURF_WALL], at - 20, LG_PW_PIT_DRAWN, cc - 20, at + 20, cy - R / 2, cc + 20, T->uv[LGSURF_WALL], 0);
			} else {
				lglBox(&lists[LGSURF_WALL], cc - 20, LG_PW_PIT_DRAWN, at - 20, cc + 20, cy - R / 2, at + 20, T->uv[LGSURF_WALL], 0);
			}
		}
	}
	/* collars where pipes join it from the side */
	for (k = 0; k < len; ) {
		int e;

		if (!lglPipeJoin(lg, ai, k)) {
			k++;
			continue;
		}
		for (e = k + 1; e < len && lglPipeJoin(lg, ai, e); e++);
		{
			const int32_t s0 = (alongx ? X0 : Z0) + k * C - LGL_PIPE_COLLAR, s1 = (alongx ? X0 : Z0) + e * C + LGL_PIPE_COLLAR;
			const int32_t c0 = cc - R - 8, c1 = cc + R + 8;

			if (alongx) {
				lglBox(&lists[9], s0, cy - R - 8, c0, s1, a->floor + 3, c1, 200, 1);
			} else {
				lglBox(&lists[9], c0, cy - R - 8, s0, c1, a->floor + 3, s1, 200, 1);
			}
		}
		k = e;
	}
}

/* the ladder's foot on its face: the face's x or z, the cell's middle along it, and which way out to the climber */
static void lglLadderAt(const LgLayout *lg, const LgLadder *ld, int32_t *face, int32_t *mid, int *sgn)
{
	const int32_t C = LG_CELL;

	*face = (ld->alongz ? lg->originx : lg->originz) + ld->line * C;
	*mid = (ld->alongz ? lg->originz : lg->originx) + ld->at * C + C / 2;
	*sgn = ld->facing == 0 || ld->facing == 2 ? -1 : 1;
}

/* a ladder's rails and rungs, drawn by the room at its foot */
static void lglLadder(const LgLayout *lg, const LgLadder *ld, LgBatchList *l)
{
	int32_t face, mid, y;
	int sgn, s;

	lglLadderAt(lg, ld, &face, &mid, &sgn);
	for (s = -1; s <= 1; s += 2) {
		const int32_t a0 = mid + s * 30 - 4, a1 = mid + s * 30 + 4;
		const int32_t o0 = face + sgn * 4, o1 = face + sgn * 14;
		const int32_t lo = ld->lo, hi = ld->hi + 50;

		if (ld->alongz) {
			lglBox(l, o0 < o1 ? o0 : o1, lo, a0, o0 < o1 ? o1 : o0, hi, a1, 200, 0);
		} else {
			lglBox(l, a0, lo, o0 < o1 ? o0 : o1, a1, hi, o0 < o1 ? o1 : o0, 200, 0);
		}
	}
	for (y = ld->lo + 28; y < ld->hi + 20; y += 32) {
		const int32_t a0 = mid - 26, a1 = mid + 26;
		const int32_t o0 = face + sgn * 6, o1 = face + sgn * 12;

		if (ld->alongz) {
			lglBox(l, o0 < o1 ? o0 : o1, y, a0, o0 < o1 ? o1 : o0, y + 5, a1, 200, 1);
		} else {
			lglBox(l, a0, y, o0 < o1 ? o0 : o1, a1, y + 5, o0 < o1 ? o1 : o0, 200, 1);
		}
	}
}

/* a ladder's tile: a wall the climber holds, GEOFLAG_LADDER, a little out
 * from the face, wound as the foot's edge on that side (lglEdges) */
static void lglLadderTile(const LgLayout *lg, const LgLadder *ld, Buf *body)
{
	int32_t face, mid, q[4][3];
	int sgn;
	const int32_t out = 0;

	lglLadderAt(lg, ld, &face, &mid, &sgn);
	{
		const int32_t f = face + sgn * (4 + out);
		/* the foot's edge runs: climber on -x (its +x edge) -z; on +x (its -x edge) +z; on -z (its +z edge) +x; on +z (its -z edge) -x */
		const int dir = ld->facing == 0 ? -1 : ld->facing == 1 ? 1 : ld->facing == 2 ? 1 : -1;
		const int32_t a0 = mid - dir * 40, a1 = mid + dir * 40;

		if (ld->alongz) {
			q[0][0] = f; q[0][1] = ld->lo - LGF_WALL_BELOW; q[0][2] = a0;
			q[1][0] = f; q[1][1] = ld->lo - LGF_WALL_BELOW; q[1][2] = a1;
			q[2][0] = f; q[2][1] = ld->hi; q[2][2] = a1;
			q[3][0] = f; q[3][1] = ld->hi; q[3][2] = a0;
		} else {
			q[0][0] = a0; q[0][1] = ld->lo - LGF_WALL_BELOW; q[0][2] = f;
			q[1][0] = a1; q[1][1] = ld->lo - LGF_WALL_BELOW; q[1][2] = f;
			q[2][0] = a1; q[2][1] = ld->hi; q[2][2] = f;
			q[3][0] = a0; q[3][1] = ld->hi; q[3][2] = f;
		}
	}
	emitTile(body, 4, (const int32_t (*)[3])q, LGF_WALL_FLAGS | LGF_LADDER, 0, 0x0fff);
}


/* The railing of obstacle o: along its long side, on the floor at its foot */
static void lglRailing(const LgLayout *lg, const LgObstacle *o, LgBatchList *l)
{
	const int alongx = o->halfx >= o->halfz;
	const int32_t foot = modLevelGenFloorAt(lg, o->area, o->x, o->z), top = foot + o->height;
	const int32_t len = 2 * (alongx ? o->halfx : o->halfz);
	const int32_t a0 = (alongx ? o->x : o->z) - len / 2;
	int32_t tiles = (len + LGL_RAILTILE / 2) / LGL_RAILTILE;
	int32_t c[4][3], sa[4], ta[4], nrm[3] = { 0, 0, 0 };
	int k;

	if (tiles < 1) {
		tiles = 1;
	}
	for (k = 0; k < 4; k++) {
		const int32_t along = (k == 1 || k == 2) ? a0 + len : a0;

		c[k][0] = alongx ? along : o->x;
		c[k][1] = (k < 2) ? foot : top;
		c[k][2] = alongx ? o->z : along;
		/* s along it, a tile at a time; t down from the top rail (row 0) */
		sa[k] = (k == 1 || k == 2) ? tiles * LGL_RAILTEXW * 32 : 0;
		ta[k] = (k < 2) ? LGL_RAILTEXH * 32 : 0;
	}
	nrm[alongx ? 2 : 0] = 1;   /* either face: the list is drawn with no culling */
	lglQuad(l, c, sa, ta, 100, nrm);
}

static void lglFireEscapeRail(const LgLayout *lg, int ai, const LgEdge *e, int32_t q[4][3])
{
	const int32_t a = lglFloorDrawn(lg, ai, e->ax, e->az);
	const int32_t b = lglFloorDrawn(lg, ai, e->bx, e->bz);
	const int32_t corners[4][3] = {
		{ e->ax, a, e->az }, { e->bx, b, e->bz },
		{ e->bx, b + LG_RAIL_HEIGHT, e->bz }, { e->ax, a + LG_RAIL_HEIGHT, e->az },
	};
	memcpy(q, corners, sizeof(corners));
}

/* One area's room: geometry relative to its centre, its box about it */
static int lglRoom(const LgLayout *lg, int ai, const LgBoxParams *th, uint32_t ptr, Buf *room,
		int32_t centre[3], int32_t bbox[6])
{
	const LgArea *a = &lg->areas[ai];
	const int32_t C = LG_CELL, h = a->ceil;
	const int32_t X0 = lg->originx + a->x * C, Z0 = lg->originz + a->z * C;
	const int32_t X1 = X0 + a->w * C, Z1 = Z0 + a->d * C;
	const int32_t up[3] = { 0, 1, 0 }, down[3] = { 0, -1, 0 };
	LgBatchList lists[LGL_LISTS];     /* by LGSURF_, then the light fittings, the railings, a grating floor, the wall bands */
	const LgTexSet T = lglTheme(lg, th, a->theme);
	int32_t lpos[LGL_MAXLIGHTS][2];
	int32_t near[LGL_MAXNEAR][3];
	int nown;
	LgLightCtx lc;
	LgEdge *edges;
	Buf gdl = { 0 };
	int32_t c[4][3], sa[4], ta[4];
	uint32_t nv = 0, vtxat, colat, gdlat;
	int nedges, i, k, x, z, ok = 1;

	memset(lists, 0, sizeof(lists));
	/* the shading takes the lights of this area and of every area it opens
	 * on to, so the two sides of an opening are lit the same and the light
	 * runs on through it without a seam */
	nown = lglLights(lg, ai, lpos);
	lc.n = 0;
	for (i = 0; i < nown; i++) {
		near[lc.n][0] = lpos[i][0]; near[lc.n][1] = lpos[i][1]; near[lc.n][2] = h;
		lc.n++;
	}
	for (i = 0; i < lg->numportals; i++) {
		const LgPortal *pt = &lg->portals[i];
		const int other = pt->a == ai ? pt->b : (pt->b == ai ? pt->a : -1);
		int32_t opos[LGL_MAXLIGHTS][2];
		int on, j, dup = 0;

		if (other < 0) {
			continue;
		}
		for (j = 0; j < i; j++) {
			dup |= (lg->portals[j].a == ai && lg->portals[j].b == other) || (lg->portals[j].b == ai && lg->portals[j].a == other);
		}
		if (dup) {
			continue;
		}
		on = lglLights(lg, other, opos);
		for (j = 0; j < on && lc.n < LGL_MAXNEAR; j++) {
			near[lc.n][0] = opos[j][0]; near[lc.n][1] = opos[j][1]; near[lc.n][2] = lg->areas[other].ceil;
			lc.n++;
		}
	}
	lc.pos = (const int32_t (*)[3])near;
	lc.tint = lglAesthetic(lg)->mood[lglMoodTheme(lg, ai)].tint;
	lc.factor = lglAreaFactor(lg, ai);
	lc.ambient = a->open ? LGL_SKYLIGHT : a->special == LG_SPECIAL_WINDOW ? LGL_SKYLIGHT / 2 : 0;
	/* a tall space's lights reach further down (the hub's ceiling is 900 up) */
	lc.reach = 220 + (h - a->floor > 300 ? (h - a->floor - 300) / 2 : 0);
	edges = (LgEdge *)malloc((size_t)lglEdgeMax(a) * sizeof(*edges));
	if (!edges) {
		return 0;
	}
	nedges = lglEdges(lg, ai, edges, lglEdgeMax(a));

	/* floor and ceiling */
	for (z = 0; z < a->d; z += LGL_FQUAD) {
		for (x = 0; x < a->w; x += LGL_FQUAD) {
			const int32_t x0 = X0 + x * C, z0 = Z0 + z * C;
			const int32_t x1 = X0 + (x + LGL_FQUAD < a->w ? x + LGL_FQUAD : a->w) * C;
			const int32_t z1 = Z0 + (z + LGL_FQUAD < a->d ? z + LGL_FQUAD : a->d) * C;
			const int32_t xs[4] = { x0, x1, x1, x0 }, zs[4] = { z0, z0, z1, z1 };

			for (i = 0; i < 4; i++) {
				/* (a roof sloping along x has its ridge along z: its tiles' rows turn with it) */
				const int turn = a->special == LG_SPECIAL_ROOF && (a->ramp == LG_RAMP_XP || a->ramp == LG_RAMP_XN);

				c[i][0] = xs[i]; c[i][1] = lglFloorDrawn(lg, ai, xs[i], zs[i]); c[i][2] = zs[i];
				sa[i] = turn ? zs[i] : xs[i]; ta[i] = turn ? xs[i] : zs[i];
			}
			if (a->special == LG_SPECIAL_PIPE) {
				/* (round: lglPipe) */
			} else if (lglIsGrating(lg, a)) {
				lglQuad(&lists[5], c, sa, ta, LGL_GRATEUV, up);
			} else {
				lglQuad(&lists[LGSURF_FLOOR], c, sa, ta, T.uv[LGSURF_FLOOR], up);
			}
			for (i = 0; i < 4; i++) {
				c[i][1] = h;
			}
			if (!a->open && !(lg->theme.layout == LG_LAYOUT_PIPEWORKS && a->layer)) {   /* outdoors: the sky; (a pipe or catwalk: under the hall's roof) */
				lglQuad(&lists[LGSURF_CEIL], c, sa, ta, T.uv[LGSURF_CEIL], down);
			}
		}
	}

	/* the pipeworks' pipes, and the ladders up from this floor (2026-11-19) */
	if (a->special == LG_SPECIAL_PIPE) {
		lglPipe(lg, ai, lists, &T);
	}
	for (i = 0; i < lg->numladders; i++) {
		if (lg->ladders[i].low == ai) {
			lglLadder(lg, &lg->ladders[i], &lists[8]);
		}
	}

	/* The rooftops' buildings beside this roof (2026-11-16): their tops,
	 * flat, over their walls (no room of theirs is drawn from outside, so the
	 * roof draws them; the one beside each that is biggest) */
	if (!a->layer && a->open && (LG_LAYOUT_SKYLIKE(lg->theme.layout) || LG_LAYOUT_FLATBUILT(lg->theme.layout)) && a->special != LG_SPECIAL_PIT && a->special != LG_SPECIAL_GLASS) {
		int rm;

		for (rm = 0; rm < lg->numrooms; rm++) {
			int bx0, bz0, bx1, bz1;

			if (rm == a->room || !lglRoomBox(lg, rm, &bx0, &bz0, &bx1, &bz1) || lglCapOwner(lg, bx0, bz0, bx1, bz1) != ai) {
				continue;
			}
			{
				const int32_t y = lglRoomTop(lg, rm);
				const int32_t x0 = lg->originx + bx0 * C, z0 = lg->originz + bz0 * C, x1 = lg->originx + bx1 * C, z1 = lg->originz + bz1 * C;
				const int32_t xs[4] = { x0, x1, x1, x0 }, zs[4] = { z0, z0, z1, z1 };

				for (i = 0; i < 4; i++) {
					c[i][0] = xs[i]; c[i][1] = y; c[i][2] = zs[i];
					sa[i] = xs[i]; ta[i] = zs[i];
				}
				lglQuad(&lists[LGSURF_FLOOR], c, sa, ta, T.uv[LGSURF_FLOOR], up);
			}
		}
	}

	/* Bridges over this ground area (a street): the street draws their undersides
	 * and the edges of their decks, so they are there whatever room the game is
	 * drawing (looking up from under one, no opening of the bridge's own room is
	 * in view). The underside is the slab's, LG_MEZZ_SLAB under the deck. */
	if (!a->layer && a->open) {
		int cx, cz;

		for (cz = a->z; cz < a->z + a->d; cz++) {
			for (cx = a->x; cx < a->x + a->w; cx++) {
				const int gi = lg->cells2[cz][cx] - 1;
				const int32_t x0 = lg->originx + cx * C, z0 = lg->originz + cz * C, x1 = x0 + C, z1 = z0 + C;
				int f;

				/* (and a balcony's row out over the street, 2026-11-15) */
				if (gi < 0 || (lg->areas[gi].special != LG_SPECIAL_BRIDGE && lg->areas[gi].special != LG_SPECIAL_BALCONY)
						|| lglIsGrating(lg, &lg->areas[gi])) {   /* (a grating has no slab) */
					continue;
				}
				{
					const int32_t y = lg->areas[gi].floor - LG_MEZZ_SLAB;
					const int32_t xs[4] = { x0, x1, x1, x0 }, zs[4] = { z0, z0, z1, z1 };

					for (i = 0; i < 4; i++) {
						c[i][0] = xs[i]; c[i][1] = y; c[i][2] = zs[i];
						sa[i] = xs[i]; ta[i] = zs[i];
					}
					lglQuad(&lists[LGSURF_CEIL], c, sa, ta, T.uv[LGSURF_CEIL], down);
				}
				/* the deck's edge where the next cell has no bridge over it (and is
				 * still this street: at the ends the balconies' slabs carry on) */
				for (f = 0; f < 4; f++) {
					const int nx = cx + (f == 0 ? -1 : f == 1 ? 1 : 0), nz = cz + (f == 2 ? -1 : f == 3 ? 1 : 0);
					const int32_t ylo = lg->areas[gi].floor - LG_MEZZ_SLAB, yhi = lg->areas[gi].floor;
					int32_t ax, az, bx, bz;
					int32_t nrm[3] = { 0, 0, 0 };

					if (nx < 0 || nz < 0 || nx >= lg->gw || nz >= lg->gd || lg->cells2[nz][nx] - 1 == gi
							|| lg->cells[nz][nx] - 1 != ai) {
						continue;
					}
					/* wound so the face looks out of the bridge */
					switch (f) {
					case 0: ax = x0; az = z1; bx = x0; bz = z0; nrm[0] = -1; break;
					case 1: ax = x1; az = z0; bx = x1; bz = z1; nrm[0] = 1; break;
					case 2: ax = x0; az = z0; bx = x1; bz = z0; nrm[2] = -1; break;
					default: ax = x1; az = z1; bx = x0; bz = z1; nrm[2] = 1; break;
					}
					c[0][0] = ax; c[0][1] = ylo; c[0][2] = az;
					c[1][0] = bx; c[1][1] = ylo; c[1][2] = bz;
					c[2][0] = bx; c[2][1] = yhi; c[2][2] = bz;
					c[3][0] = ax; c[3][1] = yhi; c[3][2] = az;
					for (i = 0; i < 4; i++) {
						sa[i] = az == bz ? c[i][0] : c[i][2]; ta[i] = -c[i][1];
					}
					lglQuad(&lists[LGSURF_WALL], c, sa, ta, T.uv[LGSURF_WALL], nrm);
				}
			}
		}
	}

	/* walls: runs of up to LGL_QUAD edges with the same bottom */
	for (i = 0; i < nedges; ) {
		int j = i + 1;
		int32_t nrm[3];

		while (j < nedges && j - i < LGL_QUAD && edges[j].bottom == edges[i].bottom && edges[j].top == edges[i].top && edges[j].side == edges[i].side
				&& edges[i].topa == LGL_FLATTOP && edges[j].topa == LGL_FLATTOP
				&& edges[j].holelo == edges[i].holelo && edges[j].holehi == edges[i].holehi
				&& edges[j].ax == edges[j - 1].bx && edges[j].az == edges[j - 1].bz) {
			j++;
		}

		if (edges[i].bottom < edges[i].top) {
			const int32_t ax = edges[i].ax, az = edges[i].az, bx = edges[j - 1].bx, bz = edges[j - 1].bz;
			const int alongx = (az == bz);
			/* one piece, or two either side of an upper window's hole */
			const int holed = edges[i].holehi > edges[i].holelo && edges[i].holelo > edges[i].bottom && edges[i].holehi < edges[i].top;
			const int32_t ylo[2] = { edges[i].bottom, holed ? edges[i].holehi : 0 };
			const int32_t yhi[2] = { holed ? edges[i].holelo : edges[i].top, edges[i].top };
			int piece;

			/* into the area: the rectangle's centre side */
			nrm[0] = alongx ? 0 : (ax == X0 ? 1 : -1);
			nrm[1] = 0;
			nrm[2] = alongx ? (az == Z0 ? 1 : -1) : 0;

			for (piece = 0; piece < (holed ? 2 : 1); piece++) {
				const int sloped = !holed && edges[i].topa != LGL_FLATTOP;

				c[0][0] = ax; c[0][1] = ylo[piece]; c[0][2] = az;
				c[1][0] = bx; c[1][1] = ylo[piece]; c[1][2] = bz;
				c[2][0] = bx; c[2][1] = sloped ? edges[i].topb : yhi[piece]; c[2][2] = bz;
				c[3][0] = ax; c[3][1] = sloped ? edges[i].topa : yhi[piece]; c[3][2] = az;
				for (k = 0; k < 4; k++) {
					sa[k] = alongx ? c[k][0] : c[k][2];
					ta[k] = -c[k][1];
				}
				/* Pier 17's yard: the terminal's edge (nothing built beyond it) is
				 * red containers, a building's wall the shed's sheet */
				if (LG_LAYOUT_FLATBUILT(lg->theme.layout) && a->open && !a->layer && a->special != LG_SPECIAL_PIT && T.band[0]) {
					const int ocx = ((edges[i].ax + edges[i].bx) / 2 - nrm[0] * (C / 2) - lg->originx) / C;
					const int ocz = ((edges[i].az + edges[i].bz) / 2 - nrm[2] * (C / 2) - lg->originz) / C;
					const int inside = ocx >= 1 && ocz >= 1 && ocx < lg->gw - 1 && ocz < lg->gd - 1;

					if (!inside || lglStructTopAny(lg, ocx, ocz) < 0) {
						lglQuad(&lists[6], c, sa, ta, lglEdgeUv(lg, &T), nrm);
						continue;
					}
				}
				lglWallQuad(lists, &T, c, sa, nrm);
			}
		}
		i = j;
	}

	/* ledges: the face of every terrace edge that stands over this floor */
	for (i = 0; i < nedges; i++) {
		int32_t la, lb, ha, hb, nrm[3];
		const LgEdge *e = &edges[i];
		const int alongx = e->az == e->bz;

		if (a->special == LG_SPECIAL_FIREESCAPE && !e->open && e->side != 3) {
			const int32_t s[4] = { 0, LGL_RAILTEXW * 32, LGL_RAILTEXW * 32, 0 };
			const int32_t t[4] = { LGL_RAILTEXH * 32, LGL_RAILTEXH * 32, 0, 0 };
			const int32_t normal[3] = { !alongx, 0, alongx };
			lglFireEscapeRail(lg, ai, e, c);
			lglQuad(&lists[4], c, s, t, 100, normal);
		}
		if (!lglRiser(lg, ai, e, &la, &lb, &ha, &hb)) {
			continue;
		}
		nrm[0] = alongx ? 0 : (e->ax == X0 ? 1 : -1);
		nrm[1] = 0;
		nrm[2] = alongx ? (e->az == Z0 ? 1 : -1) : 0;
		c[0][0] = e->ax; c[0][1] = la; c[0][2] = e->az;
		c[1][0] = e->bx; c[1][1] = lb; c[1][2] = e->bz;
		c[2][0] = e->bx; c[2][1] = hb; c[2][2] = e->bz;
		c[3][0] = e->ax; c[3][1] = ha; c[3][2] = e->az;
		for (k = 0; k < 4; k++) {
			sa[k] = alongx ? c[k][0] : c[k][2];
			ta[k] = -c[k][1];
		}
		if (lglAesthetic(lg)->racktex2 && e->nb >= 0 && lg->areas[e->nb].special == LG_SPECIAL_RACK
				&& lg->areas[e->nb].theme == lglAesthetic(lg)->rack2theme) {
			/* a second kind of block (Hollow Creek's hedges, 2026-11-17) */
			lglQuad(&lists[9], c, sa, ta, lglAesthetic(lg)->rackuv2, nrm);
		} else if (lglAesthetic(lg)->racktex && e->nb >= 0 && lg->areas[e->nb].special == LG_SPECIAL_RACK
				&& ((lg->theme.layout != LG_LAYOUT_VILLA && !LG_LAYOUT_WOODSLIKE(lg->theme.layout)) || lg->areas[e->nb].theme == 9)) {   /* (a villa: its hedges; the woods: their trees) */
			/* a warehouse rack's face: shelves of boxes (2026-11-15) */
			lglQuad(&lists[8], c, sa, ta, lglAesthetic(lg)->rackuv, nrm);
		} else if (LG_LAYOUT_FLATBUILT(lg->theme.layout) && a->open && !a->layer && a->special != LG_SPECIAL_PIT && T.band[0]) {
			/* Pier 17's yard: a perch's side is a green container, the terminal's
			 * edge red ones, a building's wall the shed's sheet */
			const int ocx = ((e->ax + e->bx) / 2 - nrm[0] * (C / 2) - lg->originx) / C, ocz = ((e->az + e->bz) / 2 - nrm[2] * (C / 2) - lg->originz) / C;
			const int inside = ocx >= 1 && ocz >= 1 && ocx < lg->gw - 1 && ocz < lg->gd - 1;

			if (e->nb >= 0 && lg->areas[e->nb].open) {
				lglQuad(&lists[7], c, sa, ta, T.uv[LGSURF_WALL], nrm);
			} else if (e->nb < 0 && (!inside || lglStructTopAny(lg, ocx, ocz) < 0)) {
				lglQuad(&lists[6], c, sa, ta, lglEdgeUv(lg, &T), nrm);
			} else {
				lglQuad(&lists[LGSURF_WALL], c, sa, ta, T.uv[LGSURF_WALL], nrm);
			}
		} else {
			lglWallQuad(lists, &T, c, sa, nrm);
		}
	}
	free(edges);

	/* pillars and low cover: four faces, and low cover a top */
	for (i = 0; i < lg->numobst; i++) {
		const LgObstacle *o = &lg->obst[i];
		const int32_t top = lglObstTop(lg, o), foot = modLevelGenFloorAt(lg, ai, o->x, o->z);
		int face;

		if (o->area != ai || o->crate) {   /* a crate is the game's own prop: only its collision is ours (the tiles) */
			continue;
		}
		if (o->rail) {                     /* posts and rails, not a wall */
			if (!lglIsGlassRail(lg, o)) {
				lglRailing(lg, o, &lists[4]);
			}
			continue;
		}
		for (face = 0; face < 4; face++) {
			int32_t ax, az, bx, bz, nrm[3];

			lglPillarFace(lg, o, face, &ax, &az, &bx, &bz, nrm);
			c[0][0] = ax; c[0][1] = foot; c[0][2] = az;
			c[1][0] = bx; c[1][1] = foot; c[1][2] = bz;
			c[2][0] = bx; c[2][1] = top; c[2][2] = bz;
			c[3][0] = ax; c[3][1] = top; c[3][2] = az;
			for (k = 0; k < 4; k++) {
				sa[k] = (az == bz) ? c[k][0] : c[k][2];
				ta[k] = -c[k][1];
			}
			lglQuad(&lists[LGSURF_WALL], c, sa, ta, T.uv[LGSURF_WALL], nrm);
		}
		if (o->height) {
			const int32_t xs[4] = { o->x - o->halfx, o->x + o->halfx, o->x + o->halfx, o->x - o->halfx };
			const int32_t zs[4] = { o->z - o->halfz, o->z - o->halfz, o->z + o->halfz, o->z + o->halfz };

			for (k = 0; k < 4; k++) {
				c[k][0] = xs[k]; c[k][1] = top; c[k][2] = zs[k];
				sa[k] = xs[k]; ta[k] = zs[k];
			}
			lglQuad(&lists[LGSURF_WALL], c, sa, ta, T.uv[LGSURF_WALL], up);
		}
	}

	/* the light fittings, a hair under the ceiling, facing down */
	{
		int32_t ftex, fhx, fhz, fs, ft;
		int alongx;

		lglFitting(lg, ai, &ftex, &fhx, &fhz, &fs, &ft, &alongx);
		for (i = 0; i < nown; i++) {
			const int32_t fx0 = lpos[i][0] - fhx, fx1 = lpos[i][0] + fhx;
			const int32_t fz0 = lpos[i][1] - fhz, fz1 = lpos[i][1] + fhz;
			const int32_t xs[4] = { fx0, fx1, fx1, fx0 }, zs[4] = { fz0, fz0, fz1, fz1 };
			/* s along the fitting's length, t across, each running once */
			const int32_t ls[4] = { 0, fs, fs, 0 }, lt[4] = { 0, 0, ft, ft };

			for (k = 0; k < 4; k++) {
				c[k][0] = xs[k]; c[k][1] = h - 1; c[k][2] = zs[k];
				sa[k] = alongx ? ls[k] : lt[k];
				ta[k] = alongx ? lt[k] : ls[k];
			}
			lglQuad(&lists[3], c, sa, ta, 100, down);
		}
	}

	for (k = 0; k < LGL_LISTS; k++) {
		ok &= !lists[k].err;
		for (i = 0; i < lists[k].n; i++) {
			nv += (uint32_t)lists[k].v[i].n;
		}
	}
	if (!ok || nedges < 0 || nv == 0 || nv > 0x7fff) {
		for (k = 0; k < LGL_LISTS; k++) free(lists[k].v);
		return 0;
	}

	centre[0] = (X0 + X1) / 2;
	centre[1] = h / 2;
	centre[2] = (Z0 + Z1) / 2;
	/* (a layer 1 area's box from its lowest floor: a villa's roof slopes) */
	centre[1] = ((a->layer ? a->floorlo : 0) + h) / 2;
	bbox[0] = X0 - centre[0]; bbox[1] = (a->layer ? a->floorlo : 0) - centre[1]; bbox[2] = Z0 - centre[2];
	bbox[3] = X1 - centre[0]; bbox[4] = h - centre[1]; bbox[5] = Z1 - centre[2];

	vtxat = (0x18 + 20 + 7) & ~7u;
	colat = vtxat + 12 * nv;
	gdlat = (colat + 4 * nv + 7) & ~7u;
	bufZeros(room, gdlat);
	bufSet32(room, 0, ptr + vtxat);
	bufSet32(room, 4, ptr + colat);
	bufSet32(room, 8, ptr + 0x18);
	bufSet32(room, 12, 0);
	bufSet16(room, 0x10, 0xffff);
	bufSet16(room, 0x12, 0);
	bufSet16(room, 0x14, nv);
	bufSet16(room, 0x16, nv);
	room->v[0x18] = 0;                           /* a leaf */
	bufSet32(room, 0x18 + 4, 0);
	bufSet32(room, 0x18 + 8, ptr + gdlat);
	bufSet32(room, 0x18 + 12, ptr + vtxat);
	bufSet32(room, 0x18 + 16, ptr + colat);

	/* the room preamble (boxBg), then each surface's texture and batches */
	bufU32(&gdl, 0xe7000000u); bufU32(&gdl, 0);
	bufU32(&gdl, 0xba001402u); bufU32(&gdl, 0x00100000u);
	bufU32(&gdl, 0xb900031du); bufU32(&gdl, 0x0c182078u);
	bufU32(&gdl, 0xfc26a004u); bufU32(&gdl, 0x1f1093ffu);
	bufU32(&gdl, 0xba001001u); bufU32(&gdl, 0x00010000u);
	bufU32(&gdl, 0xbb003001u); bufU32(&gdl, 0xffffffffu);
	bufU32(&gdl, 0xba001102u); bufU32(&gdl, 0);
	bufU32(&gdl, 0xba000c02u); bufU32(&gdl, 0x00002000u);
	bufU32(&gdl, 0xb7000000u); bufU32(&gdl, 0x00002000u);
	/* the environment colour's alpha full, and no alpha compare: on a level
	 * with no transparency the game turns the rooms' combine modes to take
	 * alpha from the environment colour (gfxreplace.c, groups 6 and 7), and
	 * the game's own lists set it (Crash Site's: fb000000 000000ff). Left
	 * unset it was whatever was drawn before: a railing cut out by its alpha
	 * vanished, and a bridge's deck seen from the next room too (2026-11-15) */
	bufU32(&gdl, 0xfb000000u); bufU32(&gdl, 0x000000ffu);
	bufU32(&gdl, 0xb9000002u); bufU32(&gdl, 0x00000000u);
	{
		uint32_t first = 0;

		int32_t ftex, fhx, fhz, fs, ft;
		int alongx;

		lglFitting(lg, ai, &ftex, &fhx, &fhz, &fs, &ft, &alongx);
		for (k = 0; k < 4; k++) {
			if (lists[k].n == 0) {
				continue;
			}
			if (k < 3) {
				if (T.grey[k]) {   /* a greyscale texture: its own combine mode, the usual one back after */
					bufU32(&gdl, 0xfc26a004u); bufU32(&gdl, 0x1ffc93fcu);
				}
				bufU32(&gdl, T.word[k]); bufU32(&gdl, T.tex[k] & 0xfffu);
			} else {
				bufU32(&gdl, 0xc0080002u); bufU32(&gdl, (uint32_t)ftex & 0xfffu);
			}
			for (i = 0; i < lists[k].n; i++) {
				/* a fitting is drawn at full brightness, as the game draws its own */
				emitVerts(room, vtxat, &lists[k].v[i], first, centre, colat, LGL_SHADEBASE, k < 3 ? &lc : NULL);
				emitBatch(&gdl, &lists[k].v[i], first);
				first += (uint32_t)lists[k].v[i].n;
			}
			if (k < 3 && T.grey[k]) {
				bufU32(&gdl, 0xfc26a004u); bufU32(&gdl, 0x1f1093ffu);
			}
		}
		/* the wall bands and the rack faces (opaque, lit as the walls) */
		for (k = 6; k < 10; k++) {
			if (lists[k].n == 0) {
				continue;
			}
			bufU32(&gdl, T.word[LGSURF_WALL]); bufU32(&gdl, (k < 8 ? T.band[k - 6] : k == 8 ? lglAesthetic(lg)->racktex : lglAesthetic(lg)->racktex2) & 0xfffu);
			for (i = 0; i < lists[k].n; i++) {
				emitVerts(room, vtxat, &lists[k].v[i], first, centre, colat, LGL_SHADEBASE, &lc);
				emitBatch(&gdl, &lists[k].v[i], first);
				first += (uint32_t)lists[k].v[i].n;
			}
		}
		/* the railings, cut out by the texture's alpha (its palette's 1-bit
		 * alpha: a pixel is solid or not there), both faces: "texture edge"
		 * (0x0c183078: the opaque surface's mode with coverage times alpha),
		 * culling off; then the room's own mode back. */
		if (lists[4].n) {
			bufU32(&gdl, 0xb900031du); bufU32(&gdl, 0x0c183078u);
			bufU32(&gdl, 0xb6000000u); bufU32(&gdl, 0x00003000u);
			bufU32(&gdl, lglAesthetic(lg)->railword); bufU32(&gdl, (uint32_t)lglAesthetic(lg)->railtex & 0xfffu);
			for (i = 0; i < lists[4].n; i++) {
				emitVerts(room, vtxat, &lists[4].v[i], first, centre, colat, LGL_SHADEBASE, &lc);
				emitBatch(&gdl, &lists[4].v[i], first);
				first += (uint32_t)lists[4].v[i].n;
			}
			bufU32(&gdl, 0xb900031du); bufU32(&gdl, 0x0c182078u);
			bufU32(&gdl, 0xb7000000u); bufU32(&gdl, 0x00002000u);
		}
		/* a grating floor (a ravine's bridges): the same, with the grating */
		if (lists[5].n) {
			bufU32(&gdl, 0xb900031du); bufU32(&gdl, 0x0c183078u);
			bufU32(&gdl, 0xb6000000u); bufU32(&gdl, 0x00003000u);
			bufU32(&gdl, 0xc0080002u); bufU32(&gdl, lglGrateTex(lg));
			for (i = 0; i < lists[5].n; i++) {
				emitVerts(room, vtxat, &lists[5].v[i], first, centre, colat, LGL_SHADEBASE, &lc);
				emitBatch(&gdl, &lists[5].v[i], first);
				first += (uint32_t)lists[5].v[i].n;
			}
			bufU32(&gdl, 0xb900031du); bufU32(&gdl, 0x0c182078u);
			bufU32(&gdl, 0xb7000000u); bufU32(&gdl, 0x00002000u);
		}
		bufU32(&gdl, 0xb8000000u); bufU32(&gdl, 0);
	}
	bufPut(room, gdl.v, gdl.n);
	bufPad(room, 8);

	ok = !gdl.err && !room->err;
	free(gdl.v);
	for (k = 0; k < LGL_LISTS; k++) free(lists[k].v);
	return ok;
}

/* A portal's quad: its opening from the floor to the lower ceiling, wound so
 * that the game's normal for it (bgInitPortal, bgConsumeSnakeItem) points at
 * area b (the high side), which is room2: the corners run up the low end first.
 * (Wound the other way until 2026-11-14, bgInitPortal swapped the rooms back by
 * where room1's centre is, which a street's centre beside its bridge gets
 * wrong: from the bridge, the street on one side was never drawn.) */
static void lglPortalQuad(const LgLayout *lg, const LgPortal *p, float q[4][3])
{
	const int32_t C = LG_CELL;
	const int32_t ha = lg->areas[p->a].ceil, hb = lg->areas[p->b].ceil;
	const float oh = (float)(ha < hb ? ha : hb);
	/* a mezzanine's opening starts at its floor */
	const float lo = (lg->areas[p->a].layer || lg->areas[p->b].layer)
			? (float)(lg->areas[p->a].floorlo > lg->areas[p->b].floorlo ? lg->areas[p->a].floorlo : lg->areas[p->b].floorlo) : 0.0f;

	if (p->z0 == p->z1) {               /* along x, normal +z */
		const float z = (float)(lg->originz + p->z0 * C);
		const float xa = (float)(lg->originx + p->x0 * C), xb = (float)(lg->originx + p->x1 * C);

		q[0][0] = xa; q[0][1] = lo; q[0][2] = z;
		q[1][0] = xa; q[1][1] = oh; q[1][2] = z;
		q[2][0] = xb; q[2][1] = oh; q[2][2] = z;
		q[3][0] = xb; q[3][1] = lo; q[3][2] = z;
	} else {                            /* along z, normal +x */
		const float x = (float)(lg->originx + p->x0 * C);
		const float za = (float)(lg->originz + p->z0 * C), zb = (float)(lg->originz + p->z1 * C);

		q[0][0] = x; q[0][1] = lo; q[0][2] = zb;
		q[1][0] = x; q[1][1] = oh; q[1][2] = zb;
		q[2][0] = x; q[2][1] = oh; q[2][2] = za;
		q[3][0] = x; q[3][1] = lo; q[3][2] = za;
	}
}

static int layoutBg(const LgLayout *lg, const LgBoxParams *th, Buf *out)
{
	const uint32_t n = (uint32_t)lg->numareas, np = (uint32_t)lg->numportals;
	const uint32_t tableat = 0x18;
	Buf prim = { 0 }, rooms = { 0 }, s2 = { 0 }, s3 = { 0 }, lens = { 0 };
	int32_t (*bboxes)[6];
	uint32_t lightsat, cmdsat, portalsat, groupsat, endat, pad, inf, ptr, r, i;
	int k, ok = 1;

	if (n == 0 || n > 1000) {
		return 0;
	}
	bboxes = (int32_t (*)[6])calloc(n, sizeof(*bboxes));
	if (!bboxes) {
		return 0;
	}

	/* the primary block: header, room table (row 0, a row per room, the end
	 * pointer, a zero row), the visibility commands (just the end), the
	 * portals and their vertices, and room for the port to widen it in (its
	 * pointers grow from 4 bytes to 8, inside a tenth of the declared size) */
	lightsat = tableat + 20 * (n + 3);
	cmdsat = lightsat + ((LGL_LIGHTSIZE * lglTotalLights(lg) + 3) & ~3u);
	portalsat = cmdsat + 8;
	groupsat = portalsat + 8 * (np + 1);
	endat = groupsat + (4 + 48) * np + 4;
	pad = 64 + 8 * (n + 3);
	bufZeros(&prim, endat + pad);
	bufPad(&prim, 4);
	bufSet32(&prim, 4, SEG_BG + tableat);
	bufSet32(&prim, 8, SEG_BG + portalsat);
	bufSet32(&prim, 12, SEG_BG + cmdsat);
	bufSet32(&prim, 16, SEG_BG + lightsat);
	prim.v[cmdsat + 1] = 1;                     /* the end command */

	/* The lights: a grid of fixtures in each room's ceiling (lglLights), in
	 * room order, since the game finds a room's lights by counting the
	 * numbers in section 3. A room the game sees no light in is drawn at full
	 * brightness until its brightness has been worked out and then settles
	 * to about four fifths of it, so every room darkened a little as the
	 * player came into it. With lights, the settled brightness is the room's
	 * maximum (roomInitLights() gives each light an equal share of what is
	 * left above the minimum), and each light can be shot out. */
	{
		uint32_t li = 0;
		int ai;

		for (ai = 0; ai < (int)n; ai++) {
			const LgArea *a = &lg->areas[ai];
			int32_t lpos[LGL_MAXLIGHTS][2];
			const int nl = lglLights(lg, ai, lpos);
			const int32_t y = a->ceil - 2;
			int32_t ftex, fhx, fhz, fs, ft;
			int alongx, j;

			lglFitting(lg, ai, &ftex, &fhx, &fhz, &fs, &ft, &alongx);

			for (j = 0; j < nl; j++, li++) {
				const uint32_t at = lightsat + LGL_LIGHTSIZE * li;
				const int32_t cx = lpos[j][0], cz = lpos[j][1];
				const int32_t corners[4][2] = { { cx - fhx, cz - fhz }, { cx + fhx, cz - fhz }, { cx + fhx, cz + fhz }, { cx - fhx, cz + fhz } };

				bufSet16(&prim, at, (uint32_t)ai + 1);  /* its room */
				bufSet16(&prim, at + 2, 0xffff);        /* colour */
				/* brightness, the on/healthy bits and the multiplier are set at the load */
				prim.v[at + 7] = 0;                     /* pointing down */
				prim.v[at + 8] = (uint8_t)-127;
				prim.v[at + 9] = 0;
				for (k = 0; k < 4; k++) {
					bufSet16(&prim, at + 10 + 6 * (uint32_t)k, (uint32_t)corners[k][0] & 0xffff);
					bufSet16(&prim, at + 12 + 6 * (uint32_t)k, (uint32_t)y & 0xffff);
					bufSet16(&prim, at + 14 + 6 * (uint32_t)k, (uint32_t)corners[k][1] & 0xffff);
				}
			}
		}
	}
	for (i = 0; i < np; i++) {
		const LgPortal *p = &lg->portals[i];
		const uint32_t at = portalsat + 8 * i, gat = groupsat + 52 * i;
		float q[4][3];
		int c;

		bufSet16(&prim, at, i + 1);             /* bg.c makes it an offset */
		bufSet16(&prim, at + 2, (uint32_t)(p->a + 1));
		bufSet16(&prim, at + 4, (uint32_t)(p->b + 1));
		/* a bridge's side over the ground below it (2026-11-16): the ground is
		 * seen down through the bridge's grating and past its edge, below the
		 * opening, not only through it, so whatever of the screen the bridge
		 * has, the ground under it has too (the game's portal flag 2) */
		{
			const LgArea *pa = &lg->areas[p->a], *pb = &lg->areas[p->b];

			if (p->drop && pa->open && pb->open && ((pa->layer && pa->special == LG_SPECIAL_BRIDGE && !pb->layer)
					|| (pb->layer && pb->special == LG_SPECIAL_BRIDGE && !pa->layer))) {
				prim.v[at + 6] = 0x02;
			}
			/* the pipeworks' hall (2026-11-19): one big space of floors, pipes and
			 * catwalks, each a room; looking along a pipe or catwalk its side
			 * openings run past the camera, and the game boxes a portal by its
			 * corners in front of the camera only (no near-plane clipping), so
			 * the sludge and the far sides were cut to a sliver. Every opening
			 * between two of the hall's pieces passes the whole box on. */
			if (lg->theme.layout == LG_LAYOUT_PIPEWORKS && pa->room == 0 && pb->room == 0
					&& pa->kind == LG_AREA_ROOM && pb->kind == LG_AREA_ROOM) {
				prim.v[at + 6] = 0x02;
			}
		}
		lglPortalQuad(lg, p, q);
		prim.v[gat] = 4;
		for (k = 0; k < 4; k++) {
			for (c = 0; c < 3; c++) {
				uint32_t u;

				memcpy(&u, &q[k][c], 4);
				bufSet32(&prim, gat + 4 + 12 * (uint32_t)k + 4 * (uint32_t)c, u);
			}
		}
	}
	inf = prim.n;

	/* the rooms, each straight after the last */
	ptr = SEG_BG + inf;
	for (r = 1; r <= n && ok; r++) {
		Buf room = { 0 };
		int32_t centre[3];
		const uint32_t e = tableat + 20 * r;

		if (!lglRoom(lg, (int)r - 1, th, ptr, &room, centre, bboxes[r - 1])) {
			ok = 0;
			free(room.v);
			break;
		}
		bufSet32(&prim, e, ptr);
		for (k = 0; k < 3; k++) {
			const float f = (float)centre[k];
			uint32_t u;

			memcpy(&u, &f, 4);
			bufSet32(&prim, e + 4 + 4 * (uint32_t)k, u);
		}
		prim.v[e + 16] = 128;                   /* brightness range; 0 draws the room black */
		prim.v[e + 17] = (uint8_t)modLevelGenAreaBrightness(lg, (int)r - 1);
		bufU16(&lens, room.n / 16 + 1);
		bufPut(&rooms, room.v, room.n);
		ptr += room.n;
		free(room.v);
	}
	if (ok) {
		bufSet32(&prim, tableat + 20 * (n + 1), ptr);

		/* section 2: every texture any room names */
		{
			uint16_t listed[5 * LGL_NUMTHEMES + 4];
			int nl = 0;

			for (r = 0; r < n; r++) {
				const LgTexSet T = lglTheme(lg, th, lg->areas[r].theme);
				int32_t ftex, fhx, fhz, fs, ft;
				int alongx;

				lglFitting(lg, (int)r, &ftex, &fhx, &fhz, &fs, &ft, &alongx);
				for (k = 0; k < 8; k++) {
					const uint16_t tn = k < 3 ? T.tex[k] : k == 3 ? (uint16_t)ftex : k < 6 ? T.band[k - 4] : k == 6 ? lglAesthetic(lg)->racktex : lglAesthetic(lg)->racktex2;
					int seen = 0, m;

					if (tn == 0) {
						continue;   /* (no band) */
					}
					for (m = 0; m < nl; m++) seen |= listed[m] == tn;
					if (!seen) {
						listed[nl++] = tn;
						bufU16(&s2, tn);
					}
				}
			}
			/* the railings' texture, where no room's surfaces name it already */
			{
				const uint16_t tn = lglAesthetic(lg)->railtex;
				int seen = 0, m;

				for (m = 0; m < nl; m++) seen |= listed[m] == tn;
				if (!seen && lg->numobst > 0) {
					listed[nl++] = tn;
					bufU16(&s2, tn);
				}
			}
			/* and a ravine's grating */
			if (lg->theme.layout == LG_LAYOUT_RAVINE || lg->theme.layout == LG_LAYOUT_ROOFTOP || lg->theme.layout == LG_LAYOUT_PIPEWORKS) {   /* (and the rooftops' bridges, the pipeworks' catwalks) */
				int seen = 0, m;

				for (m = 0; m < nl; m++) seen |= listed[m] == lglGrateTex(lg);
				if (!seen) {
					listed[nl++] = (uint16_t)lglGrateTex(lg);
					bufU16(&s2, (uint16_t)lglGrateTex(lg));
				}
			}
		}
		/* section 3: the boxes, the list sizes, the light counts */
		for (r = 0; r < n; r++) {
			for (k = 0; k < 6; k++) {
				bufU16(&s3, (uint32_t)bboxes[r][k] & 0xffff);
			}
		}
		bufPut(&s3, lens.v, lens.n);
		for (r = 0; r < n; r++) {
			bufU8(&s3, (uint32_t)lglLights(lg, (int)r, NULL));   /* its lights */
		}

		bufU32(out, inf);
		bufU32(out, prim.n + rooms.n);
		bufU32(out, prim.n);
		bufPut(out, prim.v, prim.n);
		bufPut(out, rooms.v, rooms.n);
		bufU16(out, 0x8000u | s2.n);
		bufU16(out, s2.n);
		bufPut(out, s2.v, s2.n);
		bufU16(out, 0x8000u | s3.n);
		bufU16(out, s3.n);
		bufPut(out, s3.v, s3.n);
		bufPad(out, 16);
		ok = !(prim.err | rooms.err | s2.err | s3.err | lens.err | out->err) && s3.n < 0x8000 && s2.n < 0x8000;
	}

	free(bboxes);
	free(prim.v);
	free(rooms.v);
	free(s2.v);
	free(s3.v);
	free(lens.v);
	return ok;
}

static int layoutTiles(const LgLayout *lg, const LgBoxParams *th, LgBuf *out)
{
	const uint32_t n = (uint32_t)lg->numareas;
	Buf body = { 0 }, f = { 0 };
	uint32_t *offs;
	int ai, ok;

	offs = (uint32_t *)calloc(n + 2, sizeof(*offs));
	if (!offs) {
		return 0;
	}

	offs[0] = 0;                                /* room 0: none */
	for (ai = 0; ai < (int)n; ai++) {
		const LgArea *a = &lg->areas[ai];
		const int32_t C = LG_CELL, h = a->ceil;
		const int32_t X0 = lg->originx + a->x * C, Z0 = lg->originz + a->z * C;
		LgEdge *edges = (LgEdge *)malloc((size_t)lglEdgeMax(a) * sizeof(*edges));
		int nedges, x, z, i;

		offs[ai + 1] = body.n;
		if (!edges) {
			body.err = 1;
			break;
		}
		nedges = lglEdges(lg, ai, edges, lglEdgeMax(a));

		/* a pipe's floor (2026-11-19): a strip along its top, but full width
		 * where another pipe joins its side */
		if (a->special == LG_SPECIAL_PIPE) {
			const int alongx = a->w >= a->d, len = alongx ? a->w : a->d;
			const int32_t mid = alongx ? Z0 + a->d * C / 2 : X0 + a->w * C / 2;
			int k;

			for (k = 0; k < len; k++) {
				const int full = lglPipeJoin(lg, ai, k);
				const int32_t s0 = (alongx ? X0 : Z0) + k * C, s1 = s0 + C;
				const int32_t c0 = full ? (alongx ? Z0 : X0) : mid - LGL_PIPE_WALK;
				const int32_t c1 = full ? (alongx ? Z0 + a->d * C : X0 + a->w * C) : mid + LGL_PIPE_WALK;
				const int32_t x0 = alongx ? s0 : c0, x1 = alongx ? s1 : c1, z0 = alongx ? c0 : s0, z1 = alongx ? c1 : s1;
				const int32_t fl[4][3] = { { x0, a->floor, z0 }, { x0, a->floor, z1 }, { x1, a->floor, z1 }, { x1, a->floor, z0 } };

				emitTile(&body, 4, fl, LGF_FLOOR_FLAGS, th->floortype, th->floorcolour);
			}
		}
		for (i = 0; i < lg->numladders; i++) {
			if (lg->ladders[i].low == ai || lg->ladders[i].high == ai) {
				lglLadderTile(lg, &lg->ladders[i], &body);
			}
		}

		/* floors, wound as the box's: (x0,z0) (x0,z1) (x1,z1) (x1,z0) */
		for (z = 0; z < a->d && a->special != LG_SPECIAL_PIPE; z += LGL_QUAD) {
			for (x = 0; x < a->w; x += LGL_QUAD) {
				const int32_t x0 = X0 + x * C, z0 = Z0 + z * C;
				const int32_t x1 = X0 + (x + LGL_QUAD < a->w ? x + LGL_QUAD : a->w) * C;
				const int32_t z1 = Z0 + (z + LGL_QUAD < a->d ? z + LGL_QUAD : a->d) * C;
				/* a platform's floor is raised, a ramp's slopes (modLevelGenFloorAt) */
				const int32_t fl[4][3] = {
					{ x0, lglFloorDrawn(lg, ai, x0, z0), z0 }, { x0, lglFloorDrawn(lg, ai, x0, z1), z1 },
					{ x1, lglFloorDrawn(lg, ai, x1, z1), z1 }, { x1, lglFloorDrawn(lg, ai, x1, z0), z0 },
				};

				/* a ravine's floor kills (GEOFLAG_DIE, as the game's own pits); and
				 * the bottom under a glass floor (the glass itself is its panes) */
				emitTile(&body, 4, fl, LGF_FLOOR_FLAGS | (a->special == LG_SPECIAL_PIT || a->special == LG_SPECIAL_GLASS ? LGF_DIE : 0), th->floortype, th->floorcolour);
			}
		}

		/* walls along the closed edges, runs of up to LGL_QUAD cells */
		for (i = 0; i < nedges; ) {
			int j = i + 1;

			while (j < nedges && j - i < LGL_QUAD && edges[j].open == edges[i].open && edges[j].side == edges[i].side
					&& edges[j].top == edges[i].top && edges[j].holelo == edges[i].holelo && edges[j].holehi == edges[i].holehi
					&& edges[j].ax == edges[j - 1].bx && edges[j].az == edges[j - 1].bz) {
				j++;
			}
			if (a->special == LG_SPECIAL_FIREESCAPE && !edges[i].open && edges[i].side != 3) {
				for (int k = i; k < j; k++) {
					int32_t q[4][3];
					lglFireEscapeRail(lg, ai, &edges[k], q);
					emitTile(&body, 4, q, LGF_RAIL_FLAGS, 0, 0x0fff);
				}
			} else if (!edges[i].open) {
				/* up to the edge's own top, as drawn: below a balcony that is its floor
				 * (2026-11-14: these walls went to the ceiling, an invisible wall
				 * over the street's edge kept the player on the balcony) */
				const int32_t top = edges[i].top < h ? edges[i].top : h;
				/* an upper window's hole: the wall below its sill and above its head */
				const int holed = edges[i].holehi > edges[i].holelo && edges[i].holelo > edges[i].bottom && edges[i].holehi < top;
				const int32_t ylo[2] = { edges[i].bottom - LGF_WALL_BELOW, holed ? edges[i].holehi : 0 };
				const int32_t yhi[2] = { holed ? edges[i].holelo : top, top };
				int piece;

				for (piece = 0; piece < (holed ? 2 : 1); piece++) {
					const int32_t q[4][3] = {
						{ edges[i].ax, ylo[piece], edges[i].az },
						{ edges[j - 1].bx, ylo[piece], edges[j - 1].bz },
						{ edges[j - 1].bx, yhi[piece], edges[j - 1].bz },
						{ edges[i].ax, yhi[piece], edges[i].az },
					};

					emitTile(&body, 4, q, LGF_WALL_FLAGS, 0, 0x0fff);
				}
			}
			i = j;
		}
		/* ledges: their faces stop whoever is on the floor below */
		for (i = 0; i < nedges; i++) {
			int32_t la, lb, ha, hb;

			if (lglRiser(lg, ai, &edges[i], &la, &lb, &ha, &hb)) {
				const int32_t q[4][3] = {
					{ edges[i].ax, la - LGF_WALL_BELOW, edges[i].az },
					{ edges[i].bx, lb - LGF_WALL_BELOW, edges[i].bz },
					{ edges[i].bx, hb, edges[i].bz },
					{ edges[i].ax, ha, edges[i].az },
				};

				emitTile(&body, 4, q, LGF_WALL_FLAGS, 0, 0x0fff);
			}
		}
		free(edges);
		if (nedges < 0) {
			body.err = 1;
			break;
		}

		/* the pillars' and the cover's faces: walls too; low cover's top a
		 * floor, for whoever drops onto it */
		for (i = 0; i < lg->numobst; i++) {
			const LgObstacle *o = &lg->obst[i];
			const int32_t top = lglObstTop(lg, o), foot = modLevelGenFloorAt(lg, ai, o->x, o->z);
			int face;

			if (o->area != ai || lglIsGlassRail(lg, o)) {
				continue;
			}
			for (face = 0; face < 4; face++) {
				int32_t ax, az, bx, bz, nrm[3];
				int32_t q[4][3];

				lglPillarFace(lg, o, face, &ax, &az, &bx, &bz, nrm);
				q[0][0] = ax; q[0][1] = foot - LGF_WALL_BELOW; q[0][2] = az;
				q[1][0] = bx; q[1][1] = foot - LGF_WALL_BELOW; q[1][2] = bz;
				q[2][0] = bx; q[2][1] = top; q[2][2] = bz;
				q[3][0] = ax; q[3][1] = top; q[3][2] = az;
				emitTile(&body, 4, (const int32_t (*)[3])q, o->rail ? LGF_RAIL_FLAGS : LGF_WALL_FLAGS, 0, 0x0fff);
			}
			/* a crate has no floor on its top: the game rests a prop on the highest floor at
			 * or under the top of its model, and a floor exactly there lifts the crate onto
			 * its own collision box (the prop has its own top to stand on) */
			if (o->height && !o->crate && !o->rail) {   /* nor a railing: nobody stands on one */
				const int32_t fl[4][3] = {
					{ o->x - o->halfx, top, o->z - o->halfz }, { o->x - o->halfx, top, o->z + o->halfz },
					{ o->x + o->halfx, top, o->z + o->halfz }, { o->x + o->halfx, top, o->z - o->halfz },
				};

				emitTile(&body, 4, fl, LGF_FLOOR_FLAGS, th->floortype, th->floorcolour);
			}
		}
	}
	offs[n + 1] = body.n;

	/* [rooms + 1][offset of each room 0..n][offset of the end][bodies] */
	{
		const uint32_t base = 4 + 4 * (n + 2);
		uint32_t i;

		bufU32(&f, n + 1);
		for (i = 0; i <= n + 1; i++) {
			bufU32(&f, base + offs[i]);
		}
		bufPut(&f, body.v, body.n);
		bufPad(&f, 16);
	}
	ok = !body.err && !f.err && rzipStored(&f, out);
	free(offs);
	free(body.v);
	free(f.v);
	return ok;
}

/* look from a layout angle: 0 faces +z, 90 faces +x */
static void lglLook(int angle, float *lx, float *lz)
{
	static const float s[8] = { 0.0f, 0.70710678f, 1.0f, 0.70710678f, 0.0f, -0.70710678f, -1.0f, -0.70710678f };
	const int a = ((angle % 360) + 360) % 360;

	if (a % 45 == 0) {
		*lx = s[(a / 45) % 8];
		*lz = s[((a / 45) + 2) % 8];
	} else {
		/* the layout only makes multiples of 45; anything else faces +z */
		*lx = 0.0f;
		*lz = 1.0f;
	}
}

/*
 * Doors. A door is a setup object (OBJTYPE_DOOR, 55 words on the N64) standing
 * on its own pad, which carries the door's box (PADFLAG_HASBBOXDATA). The game
 * scales the model so it fills that box (setupCreateDoor()) and finds the
 * portal it belongs to by looking along the pad's normal through the box's
 * centre (setupGetPortalByDoorPad()), so a door pad sits on its portal's quad.
 *
 * The model is MODEL_DD_OFFICEDOOR (0x3b), a single sliding leaf. Its box is
 * the box of the door in the Air Base setup, which is the model's own size at
 * scale 1; a door here has that box stretched to the portal's width and height.
 * The door pads come after the layout's own pads: the door k of the layout is
 * pad numpads + k.
 */
#define LGL_DOORMODEL   0x003b
#define LGL_DOORPADFLAGS (0x0001 | 0x0040 | 0x0200) /* INTPOS | LOOKALIGNTOY | HASBBOXDATA */
#define LGL_DOORBOX_X   4.0f
#define LGL_DOORBOX_Y0  (-18.003361f)
#define LGL_DOORBOX_Y1  170.019424f
#define LGL_DOORBOX_Z0  (-32.016659f)
#define LGL_DOORBOX_Z1  185.001770f
#define LGL_DOORWORDS   55

static int lglNumDoors(const LgLayout *lg)
{
	int i, n = 0;

	for (i = 0; i < lg->numportals; i++) {
		n += lg->portals[i].door != 0;
	}
	return n;
}

/* The portal of door number k, or -1 */
static int lglDoorPortal(const LgLayout *lg, int k)
{
	int i;

	for (i = 0; i < lg->numportals; i++) {
		if (lg->portals[i].door && k-- == 0) {
			return i;
		}
	}
	return -1;
}

/*
 * Crates. The cover crates are the game's own props (OBJTYPE_BASIC, models
 * MODEL_A51_CRATE1..3, a cube about its origin: the obstacle's centre, half
 * its height over the floor). Each stands on a pad of its own, after the door
 * pads: crate k of the layout is pad numpads + doors + k. Invincible and not
 * pushable, so they are scenery; the obstacle's tiles stay as their
 * collision, so that a crate stops a bullet and a guard's line of sight
 * exactly as the box it replaced did.
 */
#define LGL_CRATEWORDS 23
/* models and flags: g_LgProps (modlevelgen.c), which has the cars too */

static int lglNumCrates(const LgLayout *lg)
{
	int i, n = 0;

	for (i = 0; i < lg->numobst; i++) {
		n += lg->obst[i].crate != 0;
	}
	return n;
}

/* Where a prop's pad goes: its model's origin. A crate's is its middle. A
 * car's is under its middle and behind it (g_LgProps: originy above the
 * floor, the box's middle `ahead` of it along the way it faces: turn 0 faces
 * +z, 1 -x, 2 -z, 3 +x, as lglLook has it). */
static void lglPropPad(const LgLayout *lg, const LgObstacle *o, int32_t *x, int32_t *y, int32_t *z)
{
	const int32_t floor = modLevelGenFloorAt(lg, o->area, o->x, o->z);
	static const int8_t dirx[4] = { 0, -1, 0, 1 }, dirz[4] = { 1, 0, -1, 0 };

	if (o->model < LG_CRATE_MODELS || o->model >= LG_PROP_MODELS) {
		*x = o->x;
		*y = floor + o->height / 2;
		*z = o->z;
	} else {
		const LgPropDef *d = &g_LgProps[o->model];

		*x = o->x - d->ahead * dirx[o->turn & 3];
		*y = floor + d->originy;
		*z = o->z - d->ahead * dirz[o->turn & 3];
	}
}

/* The obstacle of crate number k, or NULL */
static const LgObstacle *lglCrate(const LgLayout *lg, int k)
{
	int i;

	for (i = 0; i < lg->numobst; i++) {
		if (lg->obst[i].crate && k-- == 0) {
			return &lg->obst[i];
		}
	}
	return NULL;
}

/* One pane across each window opening and each Street railing, using the
 * game's own window model (MODEL_WINDOW 4) set up as the Combat Simulator
 * map mp15 has its panes: OBJTYPE_GLASS, flags 0x04000b62 (0x2, X and Y to
 * the pad's bounds, 0x100, orthogonal, path blocker, AI see through), on a pad
 * with a box: up along the wall's normal (aligned to x or z), look straight
 * up, the box's x the pane's width, y nothing (the pane's plane), z its
 * height. Pane k is pad numpads + doors + crates + k; its record follows the
 * crates'. It breaks when shot, as any of the game's glass does. */
#define LGL_GLASSWORDS 24
#define LGL_GLASSMODEL 0x0004
#define LGL_GLASSFLAGS 0x04000b62u
#define LGL_GLASSPADFLAGS_X (0x0001 | 0x0002 | 0x0200) /* INTPOS | UPALIGNTOX | HASBBOXDATA: the pane faces along x */
#define LGL_GLASSPADFLAGS_Z (0x0001 | 0x0008 | 0x0200) /* INTPOS | UPALIGNTOZ | HASBBOXDATA */
/* A glass floor (2026-11-16, the rooftops): the same pane lying flat, up
 * along y, over the whole of its area (the box's x and z), with no box of its
 * own to bump into (not 0x100): the game makes it a floor (propobj.c, its
 * MOD) until it breaks */
#define LGL_GLASSPADFLAGS_Y (0x0001 | 0x0004 | 0x0200) /* INTPOS | UPALIGNTOY | HASBBOXDATA */
#define LGL_GLASSFLOORFLAGS (LGL_GLASSFLAGS & ~0x00000100u)

/* (the building site's openings have no glass yet, 2026-11-17) */
#define LGL_GLAZED(lg, a) ((a)->special == LG_SPECIAL_GLASS || ((a)->special == LG_SPECIAL_WINDOW && (lg)->theme.layout != LG_LAYOUT_SITE))

static int lglNumGlass(const LgLayout *lg)
{
	int i, n = 0;

	for (i = 0; i < lg->numareas; i++) {
		n += LGL_GLAZED(lg, &lg->areas[i]);
	}
	for (i = 0; i < lg->numobst; i++) {
		n += lglIsGlassRail(lg, &lg->obst[i]);
	}
	return n;
}

/* Lifts (2026-11-17, the penthouse and the building site): OBJTYPE_LIFT
 * (37 words on the N64) with no doors, the model MODEL_CI_LIFT scaled to the
 * shaft's floor (X, Y and Z to its pad's bounds) LGL_LIFTT thick. Each stop is a
 * pad at its floor level less half the platform (the model is centred on its
 * origin); the first stop's pad carries the box. A lift's pads follow the panes';
 * its record the panes'. Stops: up and back down (0 1 2 1: the game goes round
 * the four in turn), waiting at each (propobj.c, liftLevelgenWait). */
#define LGL_LIFTWORDS 37
/* (2026-11-17, second go: a car with sliding doors on each storey, as
 * dataDyne Central's lifts are. Their records as the game holds them: the
 * car model 0x37, flags 035204e8 and 00304300, lift sound 22, accel .0278
 * and top speed 16.7 a tick; the doors flags 04000500, door flags
 * 0080, sound 28, sliding to .9 open; a link per door to the stop it calls
 * the lift to. Ours stay open until used (they close by themselves only after
 * a day): press activate at an open door and it shuts, beeps and the lift
 * sets off; at a shut one and the lift is called. The car's door opens at
 * the start (propobj.c, liftTick).) */
#define LGL_LIFTMODEL 0x0037
#define LGL_LIFTFLAGS  0x035204e8u    /* X Y Z to the pad's bounds, invincible, ... */
#define LGL_LIFTFLAGS2 0x00324300u   /* keep the car ticking while another storey calls it */
/* (the door: dataDyne's own lift door, 0x44, does not show in a generated
 * level: the penthouse has dataDyne's service door, a dark metal slider; the
 * building site the G5 car park's freight lift door, rust and hazard stripes) */
#define LGL_LIFTDOORMODEL(lg) ((lg)->theme.layout == LG_LAYOUT_SITE ? 0x00ad : 0x0042)
#define LGL_LIFTDOORFLAGS (0x04000500u | 0x10000000u)   /* dataDyne's, HASPORTAL */
#define LGL_LINKWORDS 5
#define LGL_LIFTPADFLAGS_BOX (0x0001 | 0x0004 | 0x0200)   /* INTPOS | UPALIGNTOY | HASBBOXDATA */
#define LGL_LIFTPADFLAGS (0x0001 | 0x0004)

/* the door (its number among the doors: its record's) of a lift's stop, or -1 */
static int lglLiftDoor(const LgLayout *lg, int lift, int stop)
{
	int i, k = 0;

	for (i = 0; i < lg->numportals; i++) {
		const LgPortal *p = &lg->portals[i];

		if (!p->door) {
			continue;
		}
		if (p->door == LG_DOOR_LIFT + stop && (p->a == lg->lifts[lift].area || p->b == lg->lifts[lift].area)) {
			return k;
		}
		k++;
	}
	return -1;
}

static int lglNumDoors(const LgLayout *lg);
static int lglNumCrates(const LgLayout *lg);
static int lglNumGlass(const LgLayout *lg);

/* a lift's record's number among the props' (lift numlifts: where the links start) */
static int lglLiftCmd(const LgLayout *lg, int lift)
{
	return lglNumDoors(lg) + lglNumCrates(lg) + lglNumGlass(lg) + lift;
}

/* the links before lift's stop's (the doorless stops have none) */
static int lglLinkBefore(const LgLayout *lg, int lift, int stop)
{
	int i, st, n = 0;

	for (i = 0; i <= lift; i++) {
		for (st = 0; st < (i == lift ? stop : lg->lifts[i].nstops); st++) {
			n += lglLiftDoor(lg, i, st) >= 0;
		}
	}
	return n;
}

static int lglNumLinks(const LgLayout *lg)
{
	return lg->numlifts ? lglLinkBefore(lg, lg->numlifts - 1, lg->lifts[lg->numlifts - 1].nstops) : 0;
}

static int lglNumLiftPads(const LgLayout *lg)
{
	int i, n = 0;

	for (i = 0; i < lg->numlifts; i++) {
		n += lg->lifts[i].nstops;
	}
	return n;
}

/* the lift and stop of lift pad k */
static int lglLiftPad(const LgLayout *lg, int k, int *lift, int *stop)
{
	int i;

	for (i = 0; i < lg->numlifts; i++) {
		if (k < lg->lifts[i].nstops) {
			*lift = i;
			*stop = k;
			return 1;
		}
		k -= lg->lifts[i].nstops;
	}
	return 0;
}

/* A window looks through the wall along x if its openings are on its x sides */
static int lglGlassAlongX(const LgLayout *lg, int ai)
{
	int i;

	for (i = 0; i < lg->numportals; i++) {
		const LgPortal *p = &lg->portals[i];

		if (p->a == ai || p->b == ai) {
			return p->x0 == p->x1;
		}
	}
	return 0;
}

typedef struct {
	int32_t x, y, z;
	int area, alongx;
	float halfwidth, halfheight;
	int flat;               /* a glass floor: halfwidth along x, halfheight along z */
} LgGlassPad;

static int lglGlassPad(const LgLayout *lg, int k, LgGlassPad *pad)
{
	int i;

	for (i = 0; i < lg->numareas; i++) {
		const LgArea *a = &lg->areas[i];

		if (a->special == LG_SPECIAL_GLASS && k-- == 0) {
			pad->area = i;
			pad->alongx = 0;
			pad->flat = 1;
			pad->x = lg->originx + a->x * LG_CELL + a->w * LG_CELL / 2;
			pad->y = a->floor;
			pad->z = lg->originz + a->z * LG_CELL + a->d * LG_CELL / 2;
			pad->halfwidth = a->w * LG_CELL * 0.5f;
			pad->halfheight = a->d * LG_CELL * 0.5f;
			return 1;
		}
		if (LGL_GLAZED(lg, a) && a->special == LG_SPECIAL_WINDOW && k-- == 0) {
			pad->flat = 0;
			pad->area = i;
			pad->alongx = lglGlassAlongX(lg, i);
			pad->x = lg->originx + a->x * LG_CELL + a->w * LG_CELL / 2;
			pad->y = (a->floor + a->ceil) / 2;
			pad->z = lg->originz + a->z * LG_CELL + a->d * LG_CELL / 2;
			pad->halfwidth = (pad->alongx ? a->d : a->w) * LG_CELL * 0.5f;
			pad->halfheight = (a->ceil - a->floor) * 0.5f;
			return 1;
		}
	}

	for (i = 0; i < lg->numobst; i++) {
		const LgObstacle *o = &lg->obst[i];

		if (lglIsGlassRail(lg, o) && k-- == 0) {
			int32_t foot = modLevelGenFloorAt(lg, o->area, o->x, o->z);

			pad->flat = 0;
			pad->area = o->area;
			pad->alongx = o->halfz > o->halfx;
			pad->x = o->x;
			pad->y = foot + o->height / 2;
			pad->z = o->z;
			pad->halfwidth = pad->alongx ? o->halfz : o->halfx;
			pad->halfheight = o->height * 0.5f;
			return 1;
		}
	}

	return 0;
}

typedef struct {
	int32_t x, y, z;        /* the pad's position (integers in the file) */
	float up[3];            /* along the opening */
	float box[6];           /* xmin xmax ymin ymax zmin zmax, pad axes (x = normal, y = up, z = look) */
	int room;               /* the room (area + 1) of the portal's low side */
} LgDoorPad;

static int32_t lglRound(float v)
{
	return (int32_t)(v < 0 ? v - 0.5f : v + 0.5f);
}

/* the side a door stands on: its low side, or a lift's lobby (over the shaft's floor) */
static int lglDoorSide(const LgLayout *lg, const LgPortal *p)
{
	return p->door >= LG_DOOR_LIFT && lg->areas[p->b].floor > lg->areas[p->a].floor ? p->b : p->a;
}

static void lglDoorPad(const LgLayout *lg, const LgPortal *p, LgDoorPad *d)
{
	const int32_t C = LG_CELL;
	const int ha = lg->areas[p->a].ceil, hb = lg->areas[p->b].ceil;
	const float oh = (float)(ha < hb ? ha : hb);
	float width, centre, sy, sz, fy;

	if (p->z0 == p->z1) {               /* runs along x */
		width = (float)((p->x1 - p->x0) * C);
		centre = (float)(lg->originx + ((p->x0 + p->x1) * C) / 2);
		d->up[0] = 1.0f; d->up[1] = 0.0f; d->up[2] = 0.0f;
	} else {                            /* runs along z */
		width = (float)((p->z1 - p->z0) * C);
		centre = (float)(lg->originz + ((p->z0 + p->z1) * C) / 2);
		d->up[0] = 0.0f; d->up[1] = 0.0f; d->up[2] = 1.0f;
	}
	if (width < 0) {
		width = -width;
	}
	/* the opening runs from its floor (level on both sides) to the lower ceiling */
	{
		const int32_t mx = lg->originx + (p->x0 + p->x1) * C / 2, mz = lg->originz + (p->z0 + p->z1) * C / 2;

		fy = (float)modLevelGenFloorAt(lg, lglDoorSide(lg, p), mx, mz);
	}
	sy = width / (LGL_DOORBOX_Y1 - LGL_DOORBOX_Y0);
	sz = (oh - fy) / (LGL_DOORBOX_Z1 - LGL_DOORBOX_Z0);
	d->box[0] = -LGL_DOORBOX_X;
	d->box[1] = LGL_DOORBOX_X;
	d->box[2] = LGL_DOORBOX_Y0 * sy;
	d->box[3] = LGL_DOORBOX_Y1 * sy;
	d->box[4] = LGL_DOORBOX_Z0 * sz;
	d->box[5] = LGL_DOORBOX_Z1 * sz;
	d->y = (int32_t)(fy - d->box[4] + 0.5f);
	d->room = lglDoorSide(lg, p) + 1;
	if (p->z0 == p->z1) {
		d->x = lglRound(centre - (d->box[2] + d->box[3]) * 0.5f);
		d->z = (int32_t)(lg->originz + p->z0 * C);
	} else {
		d->x = (int32_t)(lg->originx + p->x0 * C);
		d->z = lglRound(centre - (d->box[2] + d->box[3]) * 0.5f);
	}
}

static int layoutPads(const LgLayout *lg, LgBuf *out)
{
	const uint32_t nd = (uint32_t)lglNumDoors(lg);
	const uint32_t nc = (uint32_t)lglNumCrates(lg);
	const uint32_t ng = (uint32_t)lglNumGlass(lg);
	const uint32_t n = (uint32_t)lg->numpads + nd + nc + ng + (uint32_t)lglNumLiftPads(lg);
	Buf head = { 0 }, body = { 0 }, f = { 0 };
	uint32_t start, wpstart, wgstart, coverstart, i;
	int ok;

	start = (0x14 + 2 * n + 3) & ~3u;
	for (i = 0; i < n; i++) {
		if (i < (uint32_t)lg->numpads) {
			const LgPad *p = &lg->pads[i];
			float lx, lz;

			lglLook(p->angle, &lx, &lz);
			bufU16(&head, start + body.n);
			bufU32(&body, ((uint32_t)LGL_PADFLAGS << 14) | ((uint32_t)(p->area + 1) << 4));
			bufU16(&body, (uint32_t)p->x & 0xffff);
			bufU16(&body, (uint32_t)(p->y + LGL_PADY) & 0xffff);
			bufU16(&body, (uint32_t)p->z & 0xffff);
			bufU16(&body, 0);
			bufF32(&body, lx); bufF32(&body, 0.0f); bufF32(&body, lz);
		} else if (i >= (uint32_t)lg->numpads + nd + nc + ng) {
			int li, st;
			const LgArea *sh;

			if (!lglLiftPad(lg, (int)(i - (uint32_t)lg->numpads - nd - nc - ng), &li, &st)) {
				body.err = 1;
				break;
			}
			sh = &lg->areas[lg->lifts[li].area];
			bufU16(&head, start + body.n);
			bufU32(&body, ((uint32_t)(st == 0 ? LGL_LIFTPADFLAGS_BOX : LGL_LIFTPADFLAGS) << 14) | ((uint32_t)(lg->lifts[li].area + 1) << 4));
			bufU16(&body, (uint32_t)(lg->originx + sh->x * LG_CELL + sh->w * LG_CELL / 2) & 0xffff);
			bufU16(&body, (uint32_t)(lg->lifts[li].stop[st] + LG_LIFT_CAR_HEIGHT / 2) & 0xffff);
			bufU16(&body, (uint32_t)(lg->originz + sh->z * LG_CELL + sh->d * LG_CELL / 2) & 0xffff);
			bufU16(&body, 0);
			{
				/* the car's open side to the boarding side (the model's open on its +x) */
				const char *e = getenv("LGLIFTLOOK");
				const int k = (lg->lifts[li].side + (e ? atoi(e) : 0)) & 3;
				const float lx[4] = { 0.0f, 0.0f, 1.0f, -1.0f }, lz[4] = { 1.0f, -1.0f, 0.0f, 0.0f };

				bufF32(&body, lx[k]); bufF32(&body, 0.0f); bufF32(&body, lz[k]);
			}
			if (st == 0) {
				/* (the platform: the shaft's floor less a hand's gap all round) */
				const float hx = sh->w * LG_CELL * 0.5f - 8.0f, hz = sh->d * LG_CELL * 0.5f - 8.0f;

				bufF32(&body, -hx); bufF32(&body, hx);
				bufF32(&body, -LG_LIFT_CAR_HEIGHT * 0.5f); bufF32(&body, LG_LIFT_CAR_HEIGHT * 0.5f);
				bufF32(&body, -hz); bufF32(&body, hz);
			}
		} else if (i >= (uint32_t)lg->numpads + nd + nc) {
			LgGlassPad pane;

			if (!lglGlassPad(lg, (int)(i - (uint32_t)lg->numpads - nd - nc), &pane)) {
				body.err = 1;
				break;
			}
			bufU16(&head, start + body.n);
			bufU32(&body, ((uint32_t)(pane.flat ? LGL_GLASSPADFLAGS_Y : pane.alongx ? LGL_GLASSPADFLAGS_X : LGL_GLASSPADFLAGS_Z) << 14) | ((uint32_t)(pane.area + 1) << 4));
			bufU16(&body, (uint32_t)pane.x & 0xffff);
			bufU16(&body, (uint32_t)pane.y & 0xffff);
			bufU16(&body, (uint32_t)pane.z & 0xffff);
			bufU16(&body, 0);
			if (pane.flat) {
				/* look along z; the box x along x, z along z, nothing up */
				bufF32(&body, 0.0f); bufF32(&body, 0.0f); bufF32(&body, 1.0f);
				bufF32(&body, -pane.halfwidth); bufF32(&body, pane.halfwidth);
				bufF32(&body, 0.0f); bufF32(&body, 0.0f);
				bufF32(&body, -pane.halfheight); bufF32(&body, pane.halfheight);
				continue;
			}
			bufF32(&body, 0.0f); bufF32(&body, 1.0f); bufF32(&body, 0.0f);
			bufF32(&body, -pane.halfwidth); bufF32(&body, pane.halfwidth);
			bufF32(&body, 0.0f); bufF32(&body, 0.0f);
			bufF32(&body, -pane.halfheight); bufF32(&body, pane.halfheight);
		} else if (i >= (uint32_t)lg->numpads + nd) {
			const LgObstacle *o = lglCrate(lg, (int)(i - (uint32_t)lg->numpads - nd));
			float lx, lz;

			if (o == NULL) {
				body.err = 1;
				break;
			}
			int32_t px, py, pz;

			lglLook(o->turn * 90, &lx, &lz);
			lglPropPad(lg, o, &px, &py, &pz);
			bufU16(&head, start + body.n);
			bufU32(&body, ((uint32_t)LGL_PADFLAGS << 14) | ((uint32_t)(o->area + 1) << 4));
			bufU16(&body, (uint32_t)px & 0xffff);
			bufU16(&body, (uint32_t)py & 0xffff);
			bufU16(&body, (uint32_t)pz & 0xffff);
			bufU16(&body, 0);
			bufF32(&body, lx); bufF32(&body, 0.0f); bufF32(&body, lz);
		} else {
			LgDoorPad d;
			int k;

			lglDoorPad(lg, &lg->portals[lglDoorPortal(lg, (int)(i - (uint32_t)lg->numpads))], &d);
			bufU16(&head, start + body.n);
			bufU32(&body, ((uint32_t)LGL_DOORPADFLAGS << 14) | ((uint32_t)d.room << 4));
			bufU16(&body, (uint32_t)d.x & 0xffff);
			bufU16(&body, (uint32_t)d.y & 0xffff);
			bufU16(&body, (uint32_t)d.z & 0xffff);
			bufU16(&body, 0);
			bufF32(&body, d.up[0]); bufF32(&body, d.up[1]); bufF32(&body, d.up[2]);
			for (k = 0; k < 6; k++) {
				bufF32(&body, d.box[k]);
			}
		}
	}
	bufPad(&head, 4);
	if (0x14 + head.n != start || start + body.n > 0xffff) {
		free(head.v);
		free(body.v);
		return 0;
	}

	wpstart = start + body.n;
	wgstart = wpstart + 16;
	coverstart = wgstart + 12;

	bufU32(&f, n);
	bufU32(&f, 0);
	bufU32(&f, wpstart);
	bufU32(&f, wgstart);
	bufU32(&f, coverstart);
	bufPut(&f, head.v, head.n);
	bufPut(&f, body.v, body.n);
	bufU32(&f, 0xffffffffu);                    /* no waypoints yet */
	bufZeros(&f, 12);
	bufZeros(&f, 12);                           /* no waygroups */
	bufPad(&f, 16);
	ok = !head.err && !body.err && !f.err && rzipStored(&f, out);
	free(head.v);
	free(body.v);
	free(f.v);
	return ok;
}

static int layoutSetup(const LgLayout *lg, LgBuf *out)
{
	Buf f = { 0 };
	uint32_t intro = 0x20, props, paths, lists, listcode;
	int i, ok, spawns = 0;

	bufZeros(&f, 0x20);
	for (i = 0; i < lg->numpads; i++) {
		if (lg->pads[i].kind == LG_PAD_START) {
			bufU32(&f, LGF_INTROCMD_SPAWN);
			bufU32(&f, (uint32_t)i);
			bufU32(&f, 0);
			spawns++;
		}
	}
	bufU32(&f, LGF_INTROCMD_END);
	props = f.n;
	for (i = 0; i < lglNumDoors(lg); i++) {
		/* the record of a dataDyne office door, as the Air Base setup has it: the
		 * 16.16 numbers are maxfrac .95, perimfrac 1, accel/decel, maxspeed */
		const uint32_t at = f.n;

		const int lift = lg->portals[lglDoorPortal(lg, i)].door >= LG_DOOR_LIFT;

		bufZeros(&f, 4 * LGL_DOORWORDS);
		bufSet32(&f, at + 0x00, 0x01000001);                  /* extrascale 1.0, type DOOR */
		bufSet32(&f, at + 0x04, ((uint32_t)(lift ? LGL_LIFTDOORMODEL(lg) : LGL_DOORMODEL) << 16) | (uint32_t)(lg->numpads + i));
		bufSet32(&f, at + 0x08, lift ? LGL_LIFTDOORFLAGS : 0x10000400);   /* OBJFLAG_DOOR_HASPORTAL | 0x400 */
		if (lift) {
			bufSet32(&f, at + 0x0c, 0x00020000u);  /* OBJFLAG2_DONTPAUSE: close the door on an offscreen floor */
		}
		bufSet32(&f, at + 0x4c, 0x000003e8);                  /* maxdamage */
		bufSet32(&f, at + 0x58, 0x0fff0000);
		bufSet32(&f, at + 0x5c, 0x0000f333);
		bufSet32(&f, at + 0x60, 0x00010000);
		bufSet32(&f, at + 0x64, 0x00007fff);
		bufSet32(&f, at + 0x68, 0x00014000);
		bufSet32(&f, at + 0x6c, 0x00000666);
		bufSet32(&f, at + 0x70, 0x00040000);                  /* doorflags 4, sliding */
		bufSet32(&f, at + 0x78, 0x00000384);                  /* closes by itself after 15 s */
		bufSet32(&f, at + 0xc4, 0x00000800);                  /* sound type 8 */
		if (lift) {
			bufSet32(&f, at + 0x5c, 0x0000e666);              /* open to .9 */
			bufSet32(&f, at + 0x70, 0x00800000);              /* door flags 0080, sliding */
			bufSet32(&f, at + 0x78, 0x00100000);              /* (closes by itself after a day: only when used) */
			bufSet32(&f, at + 0xc4, 0x00001c00);              /* sound type 28: the lift door's */
		}
		bufSet32(&f, at + 0xcc, 0xff000000);                  /* laserfade */
	}
	for (i = 0; i < lglNumCrates(lg); i++) {
		/* a plain object as the A51 crates are in the game's own setups: invincible,
		 * not falling, not pushable; the 16.16 word at 0x58 is floorcol and geocount */
		const LgObstacle *o = lglCrate(lg, i);
		const uint32_t at = f.n;

		bufZeros(&f, 4 * LGL_CRATEWORDS);
		bufSet32(&f, at + 0x00, ((uint32_t)(uint16_t)o->scale << 16) | 0x03);   /* extrascale, type BASIC */
		bufSet32(&f, at + 0x04, ((uint32_t)g_LgProps[o->model].model << 16) | (uint32_t)(lg->numpads + lglNumDoors(lg) + i));
		bufSet32(&f, at + 0x08, g_LgProps[o->model].flags);
		bufSet32(&f, at + 0x4c, 0x000003e8);                  /* maxdamage */
		bufSet32(&f, at + 0x58, 0x0fff0000);
	}
	for (i = 0; i < lglNumGlass(lg); i++) {
		const uint32_t at = f.n;
		LgGlassPad pane;

		pane.flat = 0;
		lglGlassPad(lg, (int)i, &pane);
		bufZeros(&f, 4 * LGL_GLASSWORDS);
		bufSet32(&f, at + 0x00, 0x0100002a);                  /* extrascale 1.0, type GLASS */
		bufSet32(&f, at + 0x04, ((uint32_t)LGL_GLASSMODEL << 16) | (uint32_t)(lg->numpads + lglNumDoors(lg) + lglNumCrates(lg) + i));
		bufSet32(&f, at + 0x08, pane.flat ? LGL_GLASSFLOORFLAGS : LGL_GLASSFLAGS);
		bufSet32(&f, at + 0x4c, 0x000003e8);                  /* maxdamage */
		bufSet32(&f, at + 0x58, 0x0fff0000);
	}
	{
		int pad = lg->numpads + lglNumDoors(lg) + lglNumCrates(lg) + lglNumGlass(lg);

		for (i = 0; i < lg->numlifts; i++) {
			const LgLift *l = &lg->lifts[i];
			const uint32_t at = f.n;
			int s;

			bufZeros(&f, 4 * LGL_LIFTWORDS);
			bufSet32(&f, at + 0x00, 0x01000030);              /* extrascale 1.0, type LIFT */
			bufSet32(&f, at + 0x04, ((uint32_t)LGL_LIFTMODEL << 16) | (uint32_t)pad);
			bufSet32(&f, at + 0x08, LGL_LIFTFLAGS);
			bufSet32(&f, at + 0x0c, LGL_LIFTFLAGS2);
			bufSet32(&f, at + 0x4c, 0x000003e8);              /* maxdamage */
			bufSet32(&f, at + 0x58, 0x0fff0000);
			/* stops 0 1 2 1 (two stops: 0 1 -1 -1) */
			for (s = 0; s < 4; s++) {
				const int st = s < l->nstops ? s : l->nstops >= 3 && s == 3 ? 1 : -1;
				const uint32_t v = st < 0 ? 0xffffu : (uint32_t)(pad + st);

				if (s & 1) {
					bufSet32(&f, at + 0x5c + (s / 2) * 4, (rd32(f.v + at + 0x5c + (s / 2) * 4) & 0xffff0000u) | v);
				} else {
					bufSet32(&f, at + 0x5c + (s / 2) * 4, (rd32(f.v + at + 0x5c + (s / 2) * 4) & 0xffffu) | (v << 16));
				}
			}
			/* its doors, a stop each (stop 3 is stop 1 again): as offsets from its own record */
			for (s = 0; s < 4; s++) {
				const int st = s < l->nstops ? s : l->nstops >= 3 && s == 3 ? 1 : -1;
				const int d = st < 0 ? -1 : lglLiftDoor(lg, i, st);

				if (d >= 0) {
					bufSet32(&f, at + 0x64 + 4 * s, (uint32_t)(d - lglLiftCmd(lg, i)));
				}
			}
			bufSet32(&f, at + 0x7c, 0x0000071c);              /* accel .0278 a tick (dataDyne's) */
			bufSet32(&f, at + 0x80, 0x0010aaaa);              /* top speed 16.7 a tick */
			bufSet32(&f, at + 0x84, 0x16000000);              /* sound type 22 */
			pad += l->nstops;
		}
		/* the links: each door to its lift and the stop it calls it to */
		for (i = 0; i < lg->numlifts; i++) {
			int st;

			for (st = 0; st < lg->lifts[i].nstops; st++) {
				const int d = lglLiftDoor(lg, i, st), here = lglLiftCmd(lg, lg->numlifts) + lglLinkBefore(lg, i, st);
				const uint32_t at = f.n;

				if (d < 0) {
					continue;
				}
				bufZeros(&f, 4 * LGL_LINKWORDS);
				bufSet32(&f, at + 0x00, 0x00000013);          /* type LINKLIFTDOOR */
				bufSet32(&f, at + 0x04, (uint32_t)(d - here));
				bufSet32(&f, at + 0x08, (uint32_t)(lglLiftCmd(lg, i) - here));
				bufSet32(&f, at + 0x10, (uint32_t)st);
			}
		}
	}
	bufU32(&f, LGF_OBJTYPE_END);
	paths = f.n;
	bufZeros(&f, 8);
	/* one AI list that does nothing; see boxSetup() for why there must be one */
	listcode = f.n;
	bufU16(&f, LGF_AICMD_END);
	bufPad(&f, 4);
	lists = f.n;
	bufU32(&f, listcode);
	bufU32(&f, LGF_AILIST_ID);
	bufZeros(&f, 8);
	bufPad(&f, 16);
	bufSet32(&f, 0x0c, intro);
	bufSet32(&f, 0x10, props);
	bufSet32(&f, 0x14, paths);
	bufSet32(&f, 0x18, lists);
	ok = spawns > 0 && !f.err && rzipStored(&f, out);
	free(f.v);
	return ok;
}

int modLevelGenLayoutFiles(const LgLayout *lg, const LgBoxParams *th, LgFiles *out, char *err, int errlen)
{
	Buf bg = { 0 };

	memset(out, 0, sizeof(*out));
	if (!layoutBg(lg, th, &bg)) {
		snprintf(err, (size_t)errlen, "layout bg failed (%d areas, %d portals)", lg->numareas, lg->numportals);
		free(bg.v);
		return 0;
	}
	out->bg.data = bg.v;
	out->bg.size = bg.n;
	if (!layoutTiles(lg, th, &out->tiles)) {
		snprintf(err, (size_t)errlen, "layout tiles failed");
		modLevelGenFilesFree(out);
		return 0;
	}
	if (!layoutPads(lg, &out->pads)) {
		snprintf(err, (size_t)errlen, "layout pads failed (%d pads)", lg->numpads);
		modLevelGenFilesFree(out);
		return 0;
	}
	if (!layoutSetup(lg, &out->setup)) {
		snprintf(err, (size_t)errlen, "layout setup failed (no start pad?)");
		modLevelGenFilesFree(out);
		return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Checker for a layout's files                                           */

/* One room's data: header, leaf, arrays, display list */
static int checkRoomData(const uint8_t *room, uint32_t len, uint32_t ptr0, uint32_t *texs, uint32_t *ntex,
		char *err, int errlen)
{
	uint32_t nv, nc, opa, gdl, vtx, col, tri, cur = 0, ended = 0;

	CHK(len >= 0x30, "bg: room data of %u bytes", len);
	nv = rd16(room + 0x14); nc = rd16(room + 0x16);
	CHK(nv == nc && nv > 0, "bg: %u vertices, %u colours", nv, nc);
	opa = rd32(room + 8) - ptr0;
	CHK(opa == 0x18 && room[opa] == 0, "bg: no opaque leaf");
	gdl = rd32(room + opa + 8) - ptr0;
	vtx = rd32(room) - ptr0;
	col = rd32(room + 4) - ptr0;
	CHK(vtx == rd32(room + opa + 12) - ptr0 && col == rd32(room + opa + 16) - ptr0, "bg: leaf pointers disagree");
	CHK(col == vtx + 12 * nv && gdl >= col + 4 * nv && gdl < len, "bg: arrays overlap");

	for (tri = gdl; tri + 8 <= len; tri += 8) {
		const uint32_t w0 = rd32(room + tri), w1 = rd32(room + tri + 4), op = w0 >> 24;

		if (op == 0x04) {
			const uint32_t n = ((w0 >> 20) & 0xf) + 1, v0 = (w0 >> 16) & 0xf;

			CHK(v0 == 0 && (w0 & 0xffff) == n * 12, "bg: odd vertex load %08x", w0);
			CHK((w1 >> 24) == 0x0e && (w1 & 0xffffff) % 12 == 0 && (w1 & 0xffffff) / 12 + n <= nv,
					"bg: vertex load %08x %08x past the %u vertices", w0, w1, nv);
			cur = n;
		} else if (op == 0x07) {
			CHK((w1 >> 24) == 0x0d && (w1 & 0xffffff) / 4 + (w0 & 0xffff) / 4 <= nc, "bg: colour load %08x past the colours", w1);
		} else if (op == 0xb1) {
			uint32_t q;

			for (q = 0; q < 4; q++) {
				const uint32_t a = (w1 >> (8 * q)) & 0xf, b = (w1 >> (8 * q + 4)) & 0xf, c = (w0 >> (4 * q)) & 0xf;

				if (a == 0 && b == 0 && c == 0) continue;
				CHK(a < cur && b < cur && c < cur, "bg: triangle %u,%u,%u past the %u loaded vertices", a, b, c, cur);
				CHK(a != b && b != c && a != c, "bg: degenerate triangle");
			}
		} else if (op == 0xc0) {
			CHK(((w0 >> 20) & 3) != 2 && ((w0 >> 22) & 3) != 2, "bg: texture command %08x clamps in s or t", w0);
			{
				uint32_t m;

				for (m = 0; m < *ntex && texs[m] != (w1 & 0xfff); m++);
				if (m == *ntex) {
					CHK(*ntex < 64, "bg: more than 64 different textures");
					texs[(*ntex)++] = w1 & 0xfff;
				}
			}
		} else if (op == 0xb8) {
			ended = 1;
			break;
		}
	}
	CHK(ended, "bg: display list has no end");
	return 1;
}

int modLevelGenLayoutCheck(const LgLayout *lg, const LgFiles *f, char *err, int errlen)
{
	const uint8_t *d = f->bg.data;
	const uint32_t n = (uint32_t)lg->numareas;
	uint32_t inf, s1, comp, r, o, np, ncmd, gbytes, host, s2, s2n, s3, s3n, ti, tj, ntex = 0, lbytes;
	uint32_t texs[64];
	const uint8_t *prim;
	Buf t = { 0 }, pd = { 0 }, sd = { 0 };
	LgBoxParams dflt;
	uint32_t shmin = 255, shmax = 0;
	int i;

	modLevelGenBoxDefaults(&dflt);

	/* bg: header and room table */
	CHK(f->bg.size >= 0x40, "bg: file too small");
	inf = rd32(d); s1 = rd32(d + 4); comp = rd32(d + 8);
	CHK(comp == inf && 12 + s1 <= f->bg.size, "bg: section 1 sizes");
	prim = d + 12;
	CHK(rd32(prim + 4) - SEG_BG == 0x18, "bg: room table not at 0x18");
	for (r = 1; r <= n + 1; r++) {
		CHK(0x18 + 20 * r + 20 <= inf, "bg: room table runs off the primary block");
		CHK(rd32(prim + 0x18 + 20 * r) != 0, "bg: room table ends at %u, %u areas", r, n);
	}
	CHK(rd32(prim + 0x18 + 20 * (n + 2)) == 0, "bg: room table does not end after %u rooms", n);
	CHK(rd32(prim + 0x18 + 20) == SEG_BG + inf, "bg: room 1 not straight after the primary block");

	/* bg: portals, as the layout has them */
	o = rd32(prim + 8) - SEG_BG;
	for (np = 0; rd16(prim + o + 8 * np) != 0; np++) {
		const uint32_t ra = rd16(prim + o + 8 * np + 2), rb = rd16(prim + o + 8 * np + 4);

		CHK(np < (uint32_t)lg->numportals, "bg: more portals than the layout's %d", lg->numportals);
		CHK(ra == (uint32_t)lg->portals[np].a + 1 && rb == (uint32_t)lg->portals[np].b + 1,
				"bg: portal %u joins rooms %u and %u", np, ra, rb);
	}
	CHK(np == (uint32_t)lg->numportals, "bg: %u portals, layout has %d", np, lg->numportals);
	o += 8 * (np + 1);
	gbytes = 0;
	for (r = 0; r < np; r++) {
		CHK(prim[o + gbytes] == 4, "bg: portal %u has %u points", r, prim[o + gbytes]);
		gbytes += 4 + 48;
	}
	CHK(prim[o + gbytes] == 0, "bg: portal vertex list not closed");
	gbytes += 4;
	for (ncmd = 1, o = rd32(prim + 12) - SEG_BG; prim[o] != 0; o += 8) ncmd++;

	/* lights: each room's grid, by room, straight before the commands */
	CHK(rd32(prim + 16) == SEG_BG + 0x18 + 20 * (n + 3), "bg: lights not after the room table");
	lbytes = rd32(prim + 12) - rd32(prim + 16);
	CHK(lbytes == ((LGL_LIGHTSIZE * lglTotalLights(lg) + 3) & ~3u), "bg: %u bytes of lights for %u lights", lbytes, lglTotalLights(lg));
	{
		uint32_t li = 0;

		for (r = 0; r < n; r++) {
			int32_t lpos[LGL_MAXLIGHTS][2];
			const int nl = lglLights(lg, (int)r, lpos);
			int j;

			/* outdoors has none (the sky lights it), every room indoors has some */
			CHK((lg->areas[r].open || lg->areas[r].special == LG_SPECIAL_WINDOW || (lg->theme.layout == LG_LAYOUT_PIPEWORKS && lg->areas[r].layer)) ? nl == 0 : (nl >= 1 && nl <= LGL_MAXLIGHTS), "bg: room %u has %d lights", r + 1, nl);
			for (j = 0; j < nl; j++, li++) {
				const uint32_t at = 0x18 + 20 * (n + 3) + LGL_LIGHTSIZE * li;
				int32_t mx = 0;
				int k2;

				CHK(rd16(prim + at) == r + 1, "bg: light %u not in room %u", li, r + 1);
				/* the fixture's square is centred on its position, in the ceiling */
				for (k2 = 0; k2 < 4; k2++) {
					const int32_t cx = (int16_t)rd16(prim + at + 10 + 6 * k2), cz = (int16_t)rd16(prim + at + 14 + 6 * k2);
					const LgArea *la = &lg->areas[r];

					mx += cx;
					/* the fitting (the light's box) is inside its own area: a strip
					 * longer than its corridor hung out through the end wall */
					CHK(cx >= lg->originx + la->x * LG_CELL && cx <= lg->originx + (la->x + la->w) * LG_CELL
							&& cz >= lg->originz + la->z * LG_CELL && cz <= lg->originz + (la->z + la->d) * LG_CELL,
							"bg: light %u of room %u reaches outside its area", j, r + 1);
				}
				CHK(mx / 4 == lpos[j][0], "bg: light %u of room %u is not at its grid position", j, r + 1);
				CHK((int16_t)rd16(prim + at + 12) == lg->areas[r].ceil - 2, "bg: light %u of room %u is not at the ceiling", j, r + 1);
			}
			/* the room's brightness range: a dim floor of 128, its own ceiling */
			CHK(prim[0x18 + 20 * (r + 1) + 16] == 128 && prim[0x18 + 20 * (r + 1) + 17] == (uint32_t)modLevelGenAreaBrightness(lg, (int)r),
					"bg: room %u brightness range %u..%u", r + 1, prim[0x18 + 20 * (r + 1) + 16], prim[0x18 + 20 * (r + 1) + 17]);
		}
	}

	/* the port widens the block in place and has a tenth more to do it in */
	host = 48 + 24 * (n + 3) + lbytes + 8 * ncmd + 8 * (np + 1) + gbytes;
	CHK(host <= (uint32_t)((float)inf * 1.1f), "bg: primary block of %u widens to %u, more than a tenth", inf, host);

	/* bg: each room */
	for (r = 1; r <= n; r++) {
		const uint32_t p0 = rd32(prim + 0x18 + 20 * r), p1 = rd32(prim + 0x18 + 20 * (r + 1));

		CHK(p1 > p0 && 12 + comp + (p1 - SEG_BG - inf) <= 12 + s1, "bg: room %u data out of order", r);
		uint32_t rtex[64], rn = 0, m, q;
		const LgTexSet T = lglTheme(lg, &dflt, lg->areas[r - 1].theme);
		int32_t ftex, fhx, fhz, fs, ft;
		int alongx;

		lglFitting(lg, (int)r - 1, &ftex, &fhx, &fhz, &fs, &ft, &alongx);

		if (!checkRoomData(d + 12 + comp + (p0 - SEG_BG - inf), p1 - p0, p0, rtex, &rn, err, errlen)) {
			size_t l = strlen(err);

			snprintf(err + l, (size_t)errlen - l, " (room %u)", r);
			return 0;
		}
		/* the room uses its theme's textures, its light fitting's and the railings', no others
		 * (and a grating's: its own, or a gantry's over a warehouse's floor) */
		for (m = 0; m < rn; m++) {
			CHK(rtex[m] == T.tex[0] || rtex[m] == T.tex[1] || rtex[m] == T.tex[2] || rtex[m] == (uint32_t)ftex
					|| rtex[m] == (uint32_t)lglAesthetic(lg)->railtex
					|| (T.band[0] && rtex[m] == T.band[0]) || (T.band[1] && rtex[m] == T.band[1])
					|| (lglAesthetic(lg)->racktex && rtex[m] == lglAesthetic(lg)->racktex)
					|| (lglAesthetic(lg)->racktex2 && rtex[m] == lglAesthetic(lg)->racktex2)
					|| (lglIsGrating(lg, &lg->areas[r - 1]) && rtex[m] == lglGrateTex(lg)),
					"bg: room %u uses texture %#x outside theme %u, its light fitting and the railings'", r, rtex[m], lg->areas[r - 1].theme);
			for (q = 0; q < ntex && texs[q] != rtex[m]; q++);
			if (q == ntex) {
				CHK(ntex < 64, "bg: more than 64 different textures");
				texs[ntex++] = rtex[m];
			}
		}
		/* the baked shading runs from near full to dim: the game only caps vertex
		 * colours at a room's brightness, so colours that all sat under 200 were
		 * never changed by the room's light */
		{
			const uint32_t nvr = rd16(d + 12 + comp + (p0 - SEG_BG - inf) + 0x14);
			const uint8_t *col = d + 12 + comp + (p0 - SEG_BG - inf) + (rd32(d + 12 + comp + (p0 - SEG_BG - inf) + 4) - p0);
			uint32_t vi;

			for (vi = 0; vi < nvr; vi++) {
				const uint32_t m3 = col[4 * vi] > col[4 * vi + 1] ? col[4 * vi] : col[4 * vi + 1];
				const uint32_t mx = m3 > col[4 * vi + 2] ? m3 : col[4 * vi + 2];

				if (mx > shmax) shmax = mx;
				if (mx < shmin) shmin = mx;
			}
		}
		/* every room has a floor; a wall texture may be unused where a room's
		 * edges are all openings */
		for (q = 0; q < rn && rtex[q] != (lglIsGrating(lg, &lg->areas[r - 1]) ? lglGrateTex(lg) : T.tex[LGSURF_FLOOR]); q++);
		CHK(q < rn, "bg: room %u lacks its theme's floor texture %#x (or a grating)", r, T.tex[LGSURF_FLOOR]);
	}

	CHK(shmax >= 0xf0 && shmin <= 0x90 && shmin >= 0x30, "bg: vertex shading runs %02x..%02x", shmin, shmax);

	/* bg: sections 2 and 3 */
	s2 = 12 + s1;
	CHK(s2 + 4 <= f->bg.size, "bg: section 2 missing");
	s2n = rd16(d + s2 + 2);
	CHK((rd16(d + s2) & 0x7fff) == s2n && s2n >= 2 && (s2n & 1) == 0, "bg: section 2 header");
	for (ti = 0; ti < ntex; ti++) {
		for (tj = 0; tj < s2n / 2 && rd16(d + s2 + 4 + 2 * tj) != texs[ti]; tj++);
		CHK(tj < s2n / 2, "bg: texture %#x is used but not listed in section 2", texs[ti]);
	}
	s3 = s2 + 4 + s2n;
	CHK(s3 + 4 <= f->bg.size, "bg: section 3 missing");
	s3n = rd16(d + s3 + 2);
	CHK((rd16(d + s3) & 0x7fff) == s3n && s3n == n * 15, "bg: section 3 is %u bytes for %u rooms", s3n, n);
	CHK(s3 + 4 + s3n <= f->bg.size, "bg: section 3 runs off the file");
	for (r = 0; r < n; r++) {
		CHK(d[s3 + 4 + 14 * n + r] == (uint32_t)lglLights(lg, (int)r, NULL), "bg: room %u has %u lights in section 3", r + 1, d[s3 + 4 + 14 * n + r]);
	}

	/* tiles: a list per room; every pad stands on a floor tile of its own room */
	CHK(rzipUnstore(f->tiles.data, f->tiles.size, &t), "tiles: not a valid stored rzip");
	CHK(rd32(t.v) == n + 1, "tiles: %u rooms, expected %u", rd32(t.v), n + 1);
	CHK(rd32(t.v + 4) == rd32(t.v + 8), "tiles: room 0 is not empty");
	CHK(rd32(t.v + 4 + 4 * (n + 1)) <= t.n, "tiles: end past the file");
	for (r = 1; r <= n; r++) {
		const uint32_t start = rd32(t.v + 4 + 4 * r), end = rd32(t.v + 4 + 4 * (r + 1));
		uint32_t floors = 0;

		CHK(start <= end, "tiles: room %u's list out of order", r);
		for (o = start; o < end; ) {
			const uint8_t *tl = t.v + o;
			const uint32_t nvx = tl[1], flags = rd16(tl + 2);

			CHK(tl[0] == 0 && nvx >= 3 && nvx <= 15, "tiles: room %u tile type %u, %u points", r, tl[0], nvx);
			CHK(flags == LGF_FLOOR_FLAGS || flags == (LGF_FLOOR_FLAGS | LGF_DIE) || flags == LGF_WALL_FLAGS || flags == LGF_RAIL_FLAGS
					|| (flags == (LGF_WALL_FLAGS | LGF_LADDER) && lg->numladders), "tiles: flags %04x", flags);
			floors += (flags & ~LGF_DIE) == LGF_FLOOR_FLAGS;
			o += 14 + 6 * nvx;
		}
		CHK(o == end, "tiles: room %u's tiles run past its list", r);
		CHK(floors > 0, "tiles: room %u has no floor", r);
	}
	/* every ledge has a solid face on its low side, standing up to the high floor */
	for (i = 0; i < lg->numportals; i++) {
		const LgPortal *pt = &lg->portals[i];
		const int32_t lx = lg->originx + pt->x0 * LG_CELL, lz = lg->originz + pt->z0 * LG_CELL;
		const int32_t mx = lg->originx + (pt->x0 + pt->x1) * LG_CELL / 2, mz = lg->originz + (pt->z0 + pt->z1) * LG_CELL / 2;
		int low, faces = 0;
		uint32_t rm, start, end;

		if (!pt->drop || lg->areas[pt->a].layer || lg->areas[pt->b].layer) {
			continue;   /* a mezzanine's edge is open: railings, no wall */
		}
		if (lg->areas[pt->a].special == LG_SPECIAL_GLASS || lg->areas[pt->b].special == LG_SPECIAL_GLASS) {
			continue;   /* a glass floor's edge: the panes over the drop, its floor down in it */
		}
		low = modLevelGenFloorAt(lg, pt->a, mx, mz) < modLevelGenFloorAt(lg, pt->b, mx, mz) ? pt->a : pt->b;
		rm = (uint32_t)low + 1;
		start = rd32(t.v + 4 + 4 * rm);
		end = rd32(t.v + 4 + 4 * (rm + 1));
		for (o = start; o < end; ) {
			const uint8_t *tl = t.v + o;
			const uint32_t nvx = tl[1];

			if (rd16(tl + 2) == LGF_WALL_FLAGS) {
				int32_t top = -0x7fffffff, onx = 1, onz = 1;
				uint32_t k;

				for (k = 0; k < nvx; k++) {
					const int32_t x = (int16_t)rd16(tl + 14 + 6 * k), y = (int16_t)rd16(tl + 14 + 6 * k + 2), z = (int16_t)rd16(tl + 14 + 6 * k + 4);

					onx &= x == lx;
					onz &= z == lz;
					if (y > top) top = y;
				}
				faces += (pt->x0 == pt->x1 ? onx : onz) && top > modLevelGenFloorAt(lg, low, mx, mz) && top <= lg->areas[low].ceil;
			}
			o += 14 + 6 * nvx;
		}
		CHK(faces > 0, "tiles: ledge %d has no face in area %d", i, low);
	}

	for (i = 0; i < lg->numpads; i++) {
		const LgPad *p = &lg->pads[i];
		const uint32_t rm = (uint32_t)p->area + 1;
		const uint32_t start = rd32(t.v + 4 + 4 * rm), end = rd32(t.v + 4 + 4 * (rm + 1));
		int found = 0;

		for (o = start; o < end && !found; ) {
			const uint8_t *tl = t.v + o;
			const uint32_t nvx = tl[1];

			if (rd16(tl + 2) == LGF_FLOOR_FLAGS) {
				int32_t mnx = 0x7fffffff, mxx = -0x7fffffff, mnz = 0x7fffffff, mxz = -0x7fffffff;
				int32_t mny = 0x7fffffff, mxy = -0x7fffffff;
				uint32_t k;

				for (k = 0; k < nvx; k++) {
					const int32_t x = (int16_t)rd16(tl + 14 + 6 * k), z = (int16_t)rd16(tl + 14 + 6 * k + 4);
					const int32_t y = (int16_t)rd16(tl + 14 + 6 * k + 2);

					if (x < mnx) mnx = x;
					if (x > mxx) mxx = x;
					if (z < mnz) mnz = z;
					if (z > mxz) mxz = z;
					if (y < mny) mny = y;
					if (y > mxy) mxy = y;
				}
				/* and at the pad's height: a gallery's pads on its raised floor,
				 * a ramp's within its slope */
				found = p->x >= mnx && p->x <= mxx && p->z >= mnz && p->z <= mxz && p->y >= mny && p->y <= mxy;
			}
			o += 14 + 6 * nvx;
		}
		CHK(found, "tiles: pad %d at (%d %d) has no floor in its room %u", i, (int)p->x, (int)p->z, rm);
	}

	/* pads: the layout's, in their areas' rooms */
	CHK(rzipUnstore(f->pads.data, f->pads.size, &pd), "pads: not a valid stored rzip");
	CHK(rd32(pd.v) == (uint32_t)(lg->numpads + lglNumDoors(lg) + lglNumCrates(lg) + lglNumGlass(lg) + lglNumLiftPads(lg)), "pads: %u pads, layout has %d, %d doors and %d crates and panes",
			rd32(pd.v), lg->numpads, lglNumDoors(lg), lglNumCrates(lg) + lglNumGlass(lg));
	/* Validate pane centers and orientation for both windows and railings. */
	for (i = 0; i < lglNumGlass(lg); i++) {
		LgGlassPad pane;
		const uint32_t off = rd16(pd.v + 0x14 + 2 * (uint32_t)(lg->numpads + lglNumDoors(lg) + lglNumCrates(lg) + i));
		uint32_t hdr, want;

		CHK(off + 4 + 8 + 12 + 24 <= rd32(pd.v + 8), "pads: glass pad %d at %u", i, off);
		CHK(lglGlassPad(lg, i, &pane), "pads: missing glass pane %d", i);
		hdr = rd32(pd.v + off);
		want = pane.flat ? LGL_GLASSPADFLAGS_Y : pane.alongx ? LGL_GLASSPADFLAGS_X : LGL_GLASSPADFLAGS_Z;
		CHK((hdr >> 14) == want && ((hdr >> 4) & 0x3ff) == (uint32_t)pane.area + 1,
				"pads: glass pad %d flags %x room %u", i, hdr >> 14, hdr >> 4 & 0x3ff);
		CHK((int16_t)rd16(pd.v + off + 4) == pane.x
				&& (int16_t)rd16(pd.v + off + 6) == pane.y
				&& (int16_t)rd16(pd.v + off + 8) == pane.z, "pads: glass pad %d position", i);
	}
	for (i = 0; i < lg->numpads; i++) {
		const uint32_t off = rd16(pd.v + 0x14 + 2 * i), hdr = rd32(pd.v + off);

		CHK(off + 24 <= rd32(pd.v + 8), "pads: pad %d at %u", i, off);
		CHK(((hdr >> 4) & 0x3ff) == (uint32_t)lg->pads[i].area + 1, "pads: pad %d in room %u", i, (hdr >> 4) & 0x3ff);
		CHK((int16_t)rd16(pd.v + off + 4) == lg->pads[i].x && (int16_t)rd16(pd.v + off + 8) == lg->pads[i].z,
				"pads: pad %d position", i);
	}
	/* the door pads: a box on the portal's quad, along the opening */
	for (i = 0; i < lglNumDoors(lg); i++) {
		const LgPortal *pt = &lg->portals[lglDoorPortal(lg, i)];
		const uint32_t off = rd16(pd.v + 0x14 + 2 * (lg->numpads + i)), hdr = rd32(pd.v + off);
		const int32_t px = (int16_t)rd16(pd.v + off + 4), py = (int16_t)rd16(pd.v + off + 6), pz = (int16_t)rd16(pd.v + off + 8);
		LgDoorPad d;
		uint32_t k;
		float box[6], up[3];

		CHK(off + 48 <= rd32(pd.v + 8), "pads: door pad %d at %u", i, off);
		CHK((hdr >> 14) == LGL_DOORPADFLAGS, "pads: door pad %d flags %x", i, hdr >> 14);
		CHK(((hdr >> 4) & 0x3ff) == (uint32_t)lglDoorSide(lg, pt) + 1, "pads: door pad %d in room %u", i, (hdr >> 4) & 0x3ff);
		for (k = 0; k < 3; k++) { uint32_t u = rd32(pd.v + off + 12 + 4 * k); memcpy(&up[k], &u, 4); }
		for (k = 0; k < 6; k++) { uint32_t u = rd32(pd.v + off + 24 + 4 * k); memcpy(&box[k], &u, 4); }
		lglDoorPad(lg, pt, &d);
		CHK(px == d.x && py == d.y && pz == d.z, "pads: door pad %d position", i);
		CHK(box[1] > box[0] && box[3] > box[2] && box[5] > box[4], "pads: door pad %d box is empty", i);
		CHK(up[0] * up[0] + up[1] * up[1] + up[2] * up[2] == 1.0f && up[1] == 0.0f, "pads: door pad %d direction", i);
		if (pt->z0 == pt->z1) {
			/* along x: the box (x of the pad is the normal, z world) must cross the portal's centre */
			const float cx = (float)(lg->originx + ((pt->x0 + pt->x1) * LG_CELL) / 2);
			const float mid = (float)px + (box[2] + box[3]) * 0.5f;

			CHK(mid > cx - 1.0f && mid < cx + 1.0f && pz == lg->originz + pt->z0 * LG_CELL && up[0] == 1.0f,
					"pads: door pad %d is off its portal (%d %d %d), portal x %d..%d z %d, mid %.1f centre %.1f up %.1f",
					i, px, py, pz, pt->x0, pt->x1, pt->z0, mid, cx, up[0]);
		} else {
			const float cz = (float)(lg->originz + ((pt->z0 + pt->z1) * LG_CELL) / 2);
			const float mid = (float)pz + (box[2] + box[3]) * 0.5f;

			CHK(mid > cz - 1.0f && mid < cz + 1.0f && px == lg->originx + pt->x0 * LG_CELL && up[2] == 1.0f,
					"pads: door pad %d is off its portal (%d %d %d)", i, px, py, pz);
		}
		/* the leaf's top must not stand above the ceiling, nor its foot under the floor */
		CHK(py + (int)box[4] >= modLevelGenFloorAt(lg, pt->a, lg->originx + (pt->x0 + pt->x1) * LG_CELL / 2, lg->originz + (pt->z0 + pt->z1) * LG_CELL / 2) - 1
				&& py + (int)box[5] <= lg->areas[pt->a].ceil + 1 && py + (int)box[5] <= lg->areas[pt->b].ceil + 1,
				"pads: door pad %d leaf spans %d..%d", i, py + (int)box[4], py + (int)box[5]);
	}
	/* the crate pads: at the crate's centre, in its room, no box */
	for (i = 0; i < lglNumCrates(lg); i++) {
		const LgObstacle *ob = lglCrate(lg, i);
		const uint32_t off = rd16(pd.v + 0x14 + 2 * (lg->numpads + lglNumDoors(lg) + i)), hdr = rd32(pd.v + off);

		CHK(off + 24 <= rd32(pd.v + 8), "pads: crate pad %d at %u", i, off);
		CHK((hdr >> 14) == LGL_PADFLAGS && ((hdr >> 4) & 0x3ff) == (uint32_t)ob->area + 1, "pads: crate pad %d flags %x room %u", i, hdr >> 14, (hdr >> 4) & 0x3ff);
		{
			/* a crate's pad is its middle; a car's its model's origin, inside its box
			 * above the floor, within its `ahead` of the middle */
			const LgPropDef *d = &g_LgProps[ob->model < LG_PROP_MODELS ? ob->model : 0];
			const int dx = (int16_t)rd16(pd.v + off + 4) - ob->x, dz = (int16_t)rd16(pd.v + off + 8) - ob->z;
			const int dy = (int)(int16_t)rd16(pd.v + off + 6) - modLevelGenFloorAt(lg, ob->area, ob->x, ob->z);

			CHK(ob->model < LG_PROP_MODELS, "pads: crate %d has model %d", i, ob->model);
			CHK((dx == 0 || dz == 0) && (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) == (d->ahead < 0 ? -d->ahead : d->ahead), "pads: crate pad %d position (%d %d)", i, dx, dz);
			CHK(dy == (ob->model < LG_CRATE_MODELS ? ob->height / 2 : d->originy), "pads: crate pad %d height %d", i, dy);
		}
	}
	CHK(rd32(pd.v + rd32(pd.v + 8)) == 0xffffffffu, "pads: waypoint list not closed");

	/* setup: a spawn per start pad, then the AI list */
	CHK(rzipUnstore(f->setup.data, f->setup.size, &sd), "setup: not a valid stored rzip");
	{
		uint32_t lists = rd32(sd.v + 0x18), starts = 0, spawns = 0;

		for (i = 0; i < lg->numpads; i++) starts += lg->pads[i].kind == LG_PAD_START;
		for (o = 0x20; rd32(sd.v + o) == LGF_INTROCMD_SPAWN; o += 12) {
			CHK(lg->pads[rd32(sd.v + o + 4)].kind == LG_PAD_START, "setup: spawn on pad %u, not a start pad", rd32(sd.v + o + 4));
			spawns++;
		}
		CHK(rd32(sd.v + o) == LGF_INTROCMD_END && spawns == starts && spawns > 0, "setup: %u spawns, %u start pads", spawns, starts);
		CHK(rd32(sd.v + lists) != 0 && rd32(sd.v + lists + 8) == 0, "setup: AI list table");
		{
			const uint32_t props = rd32(sd.v + 0x10);
			int nd = lglNumDoors(lg);

			for (i = 0; i < nd; i++) {
				const uint32_t at = props + 4 * LGL_DOORWORDS * (uint32_t)i;

				CHK(at + 4 * LGL_DOORWORDS <= sd.n && rd32(sd.v + at) == 0x01000001, "setup: door %d record", i);
				CHK((rd32(sd.v + at + 4) & 0xffff) == (uint32_t)(lg->numpads + i), "setup: door %d on pad %u", i, rd32(sd.v + at + 4) & 0xffff);
				CHK((rd32(sd.v + at + 4) >> 16) == (uint32_t)(lg->portals[lglDoorPortal(lg, i)].door >= LG_DOOR_LIFT ? LGL_LIFTDOORMODEL(lg) : LGL_DOORMODEL) && (rd32(sd.v + at + 8) & 0x10000000), "setup: door %d model or flags", i);
			}
			{
				const int nc = lglNumCrates(lg);
				const uint32_t cs = props + 4 * LGL_DOORWORDS * (uint32_t)nd;

				for (i = 0; i < nc; i++) {
					const LgObstacle *ob = lglCrate(lg, i);
					const uint32_t at = cs + 4 * LGL_CRATEWORDS * (uint32_t)i;

					CHK(at + 4 * LGL_CRATEWORDS <= sd.n && rd32(sd.v + at) == (((uint32_t)(uint16_t)ob->scale << 16) | 0x03), "setup: crate %d record", i);
					CHK((rd32(sd.v + at + 4) & 0xffff) == (uint32_t)(lg->numpads + nd + i), "setup: crate %d on pad %u", i, rd32(sd.v + at + 4) & 0xffff);
					CHK((rd32(sd.v + at + 4) >> 16) == g_LgProps[ob->model].model && rd32(sd.v + at + 8) == g_LgProps[ob->model].flags, "setup: crate %d model or flags", i);
				}
				{
					const int ng = lglNumGlass(lg);
					const uint32_t gs = cs + 4 * LGL_CRATEWORDS * (uint32_t)nc;

					for (i = 0; i < ng; i++) {
						const uint32_t at = gs + 4 * LGL_GLASSWORDS * (uint32_t)i;

						CHK(at + 4 * LGL_GLASSWORDS <= sd.n && rd32(sd.v + at) == 0x0100002au, "setup: pane %d record", i, 0);
						CHK(rd32(sd.v + at + 4) == (((uint32_t)LGL_GLASSMODEL << 16) | (uint32_t)(lg->numpads + nd + nc + i)) && (rd32(sd.v + at + 8) == LGL_GLASSFLAGS || rd32(sd.v + at + 8) == LGL_GLASSFLOORFLAGS),
								"setup: pane %d model, pad or flags", i, 0);
					}
					{
						const uint32_t ls = gs + 4 * LGL_GLASSWORDS * (uint32_t)ng;

						for (i = 0; i < lg->numlifts; i++) {
							const uint32_t at = ls + 4 * LGL_LIFTWORDS * (uint32_t)i;

							CHK(at + 4 * LGL_LIFTWORDS <= sd.n && rd32(sd.v + at) == 0x01000030u && (rd32(sd.v + at + 4) >> 16) == LGL_LIFTMODEL, "setup: lift %d record", i, 0);
						}
						for (i = 0; i < lglNumLinks(lg); i++) {
							const uint32_t at = ls + 4 * LGL_LIFTWORDS * (uint32_t)lg->numlifts + 4 * LGL_LINKWORDS * (uint32_t)i;

							CHK(at + 4 * LGL_LINKWORDS <= sd.n && rd32(sd.v + at) == 0x13u, "setup: lift door link %d", i, 0);
						}
						CHK((rd32(sd.v + ls + 4 * LGL_LIFTWORDS * (uint32_t)lg->numlifts + 4 * LGL_LINKWORDS * (uint32_t)lglNumLinks(lg)) & 0xff) == LGF_OBJTYPE_END,
								"setup: props not closed after %d doors and %d crates, panes, lifts and links", nd, nc + ng + lg->numlifts);
					}
				}
			}
		}
	}

	free(t.v);
	free(pd.v);
	free(sd.v);
	return 1;
}

/* by LG_LAYOUT_ (2026-11-15: every layout's since the railings became the
 * game's own texture panels and every room sets the environment colour; the
 * street's also for its upper windows, balconies out over the street and the
 * balconies' doors and glass railings) */
static const uint32_t g_LglFingerprint100[LG_LAYOUT_ALL] = { 0x3d2e3fb3u, 0xb375df8bu, 0xc4c770afu, 0, 0 };

static uint32_t lglFingerprint(uint32_t h, const LgBuf *f)
{
	uint32_t i;

	for (i = 0; i < f->size; i++) {
		h = (h ^ f->data[i]) * 16777619u;
	}
	return (h ^ f->size) * 16777619u;
}

static int lglLayoutSelfTestOne(int layout, int numseeds, void (*print)(const char *line))
{
	const char *tag = layout == LG_LAYOUT_LAB ? "-lab" : layout == LG_LAYOUT_STREET ? "-street" : layout == LG_LAYOUT_RAVINE ? "-ravine" : layout == LG_LAYOUT_ROOFTOP ? "-rooftop" : layout == LG_LAYOUT_PENTHOUSE ? "-penthouse" : layout == LG_LAYOUT_SITE ? "-site" : layout == LG_LAYOUT_PLAZA ? "-plaza" : layout == LG_LAYOUT_MALL ? "-mall" : layout == LG_LAYOUT_PIPEWORKS ? "-pipeworks" : layout == LG_LAYOUT_VILLA ? "-villa" : layout == LG_LAYOUT_STATION ? "-station" : "";
	char line[300], err[200];
	int failed = 0, passed = 0, size, s;
	LgLayout *lg = (LgLayout *)malloc(sizeof(*lg));
	uint32_t maxbg = 0, maxtiles = 0, maxpads = 0;
	uint32_t print32 = 2166136261u;   /* FNV-1a over every file of every layout */

	if (!lg) {
		print("SELFTEST FAIL levelgen-layoutfiles: out of memory");
		print("SELFTEST RESULT levelgen-layoutfiles passed 0 failed 1");
		return 1;
	}
	for (size = 0; size < LG_SIZE_COUNT; size++) {
		int sizefailed = 0, sizen = 0;
		uint32_t sizeminbg = 0xffffffffu, sizemaxbg = 0;
		uint64_t sizesum = 0;

		for (s = 0; s < numseeds; s++) {
			LgParams prm;
			LgBoxParams th;
			LgFiles a, b;

			prm.seed = 1000u + (uint32_t)s * 7919u;
			prm.size = (uint8_t)size;
			modLevelGenBoxDefaults(&th);
			err[0] = 0;
			if (!modLevelGenBuild(lg, &prm, err, sizeof(err))) {
				continue;   /* the layout's own test covers a layout that fails */
			}
			if (!modLevelGenLayoutFiles(lg, &th, &a, err, sizeof(err)) || !modLevelGenLayoutCheck(lg, &a, err, sizeof(err))) {
				if (sizefailed++ < 3) {
					snprintf(line, sizeof(line), "SELFTEST FAIL levelgen-layoutfiles%s: seed %u size %d: %s", tag, prm.seed, size, err);
					print(line);
				}
				failed++;
				if (a.bg.data) modLevelGenFilesFree(&a);
				continue;
			}
			if (a.bg.size > maxbg) maxbg = a.bg.size;
			if (a.bg.size > sizemaxbg) sizemaxbg = a.bg.size;
			if (a.bg.size < sizeminbg) sizeminbg = a.bg.size;
			sizesum += a.bg.size;
			sizen++;
			if (a.tiles.size > maxtiles) maxtiles = a.tiles.size;
			if (a.pads.size > maxpads) maxpads = a.pads.size;
			print32 = lglFingerprint(print32, &a.bg);
			print32 = lglFingerprint(print32, &a.tiles);
			print32 = lglFingerprint(print32, &a.pads);
			print32 = lglFingerprint(print32, &a.setup);
			if (s < 3) {
				/* the same layout twice gives the same bytes */
				if (!modLevelGenLayoutFiles(lg, &th, &b, err, sizeof(err))
						|| a.bg.size != b.bg.size || memcmp(a.bg.data, b.bg.data, a.bg.size)
						|| a.tiles.size != b.tiles.size || memcmp(a.tiles.data, b.tiles.data, a.tiles.size)
						|| a.pads.size != b.pads.size || memcmp(a.pads.data, b.pads.data, a.pads.size)) {
					snprintf(line, sizeof(line), "SELFTEST FAIL levelgen-layoutfiles%s: seed %u size %d not deterministic", tag, prm.seed, size);
					print(line);
					failed++;
					modLevelGenFilesFree(&a);
					continue;
				}
				modLevelGenFilesFree(&b);
			}
			passed++;
			modLevelGenFilesFree(&a);
		}
		snprintf(line, sizeof(line), "SELFTEST %s levelgen-layoutfiles%s: size %d, %d seeds, bg %u to %u bytes (average %u)", sizefailed ? "FAIL" : "PASS", tag, size, numseeds,
				sizeminbg, sizemaxbg, sizen ? (uint32_t)(sizesum / sizen) : 0);
		print(line);
	}
	snprintf(line, sizeof(line), "levelgen-layoutfiles%s: largest bg %u, tiles %u, pads %u bytes", tag, maxbg, maxtiles, maxpads);
	print(line);
	/* the same bytes as before, on every machine: a change to the generator
	 * that is meant to change its levels changes this number too (then put
	 * the new one in g_LglFingerprint100 and say so in the progress notes) */
	/* (2026-11-15, light testing: a different fingerprint is a note, not a
	 * failure; the files themselves are checked above) */
	if (numseeds == 100) {
		snprintf(line, sizeof(line), "levelgen-layoutfiles%s: fingerprint %08x of every file of %d seeds a size (%s %08x)",
				tag, print32, numseeds, print32 == g_LglFingerprint100[layout] ? "as before," : "CHANGED, was", g_LglFingerprint100[layout]);
	} else {
		snprintf(line, sizeof(line), "levelgen-layoutfiles%s: fingerprint %08x over %d seeds a size", tag, print32, numseeds);
	}
	print(line);
	snprintf(line, sizeof(line), "SELFTEST RESULT levelgen-layoutfiles%s passed %d failed %d", tag, passed, failed);
	print(line);
	free(lg);
	return failed;
}

/* every layout in turn, each fixed for its run whatever the seeds would pick */
int modLevelGenLayoutSelfTest(int numseeds, void (*print)(const char *line))
{
	int layout, failed = 0;

	for (layout = 0; layout < LG_LAYOUT_ALL; layout++) {
		if (LG_LAYOUT_DESIGNED(layout)) continue;
		modLevelGenForceTheme(layout, -1);
		failed += lglLayoutSelfTestOne(layout, numseeds, print);
	}
	modLevelGenForceTheme(-1, -1);
	return failed;
}

/* ------------------------------------------------------------------ */
/* Self-test                                                             */

int modLevelGenFileSelfTest(void (*print)(const char *line))
{
	static const int16_t sizes[][3] = { { 300, 300, 300 }, { 500, 500, 300 }, { 900, 400, 400 }, { 2000, 2000, 500 } };
	char line[256], err[160];
	int failed = 0, passed = 0;
	size_t i;

	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		LgBoxParams p;
		LgFiles a, b;

		modLevelGenBoxDefaults(&p);
		p.halfx = sizes[i][0];
		p.halfz = sizes[i][1];
		p.ceil = sizes[i][2];
		err[0] = 0;

		if (!modLevelGenBoxFiles(&p, &a, err, sizeof(err))) {
			snprintf(line, sizeof(line), "SELFTEST FAIL levelgen-files: box %d: %s", (int)i, err);
			print(line);
			failed++;
			continue;
		}
		if (!modLevelGenFilesCheck(&a, err, sizeof(err))) {
			snprintf(line, sizeof(line), "SELFTEST FAIL levelgen-files: box %d (%d x %d): %s", (int)i, p.halfx, p.halfz, err);
			print(line);
			failed++;
			modLevelGenFilesFree(&a);
			continue;
		}
		if (!modLevelGenBoxFiles(&p, &b, err, sizeof(err))
				|| a.bg.size != b.bg.size || memcmp(a.bg.data, b.bg.data, a.bg.size)
				|| a.tiles.size != b.tiles.size || memcmp(a.tiles.data, b.tiles.data, a.tiles.size)
				|| a.pads.size != b.pads.size || memcmp(a.pads.data, b.pads.data, a.pads.size)
				|| a.setup.size != b.setup.size || memcmp(a.setup.data, b.setup.data, a.setup.size)) {
			snprintf(line, sizeof(line), "SELFTEST FAIL levelgen-files: box %d is not deterministic", (int)i);
			print(line);
			failed++;
		} else {
			passed++;
		}
		modLevelGenFilesFree(&a);
		modLevelGenFilesFree(&b);
	}
	snprintf(line, sizeof(line), "SELFTEST %s levelgen-files: %d boxes", failed ? "FAIL" : "PASS", passed + failed);
	print(line);
	snprintf(line, sizeof(line), "SELFTEST RESULT levelgen-files passed %d failed %d", passed, failed);
	print(line);
	return failed;
}

#ifdef MODLEVELGENFILE_MAIN
static void printLine(const char *line)
{
	puts(line);
}

int main(int argc, char **argv)
{
	LgBoxParams p;
	LgFiles f;
	char err[160];
	static const char *names[4] = { "bg_lgen.seg", "bg_lgen_tilesZ", "bg_lgen_padsZ", "Ump_setuplgenZ" };

	if (argc < 2) {
		return modLevelGenFileSelfTest(printLine) ? 1 : 0;
	}
	modLevelGenBoxDefaults(&p);
	if (!modLevelGenBoxFiles(&p, &f, err, sizeof(err)) || !modLevelGenFilesCheck(&f, err, sizeof(err))) {
		fprintf(stderr, "failed: %s\n", err);
		return 1;
	}
	{
		const LgBuf *bufs[4] = { &f.bg, &f.tiles, &f.pads, &f.setup };
		int i;

		for (i = 0; i < 4; i++) {
			char path[512];
			FILE *fp;

			snprintf(path, sizeof(path), "%s/%s", argv[1], names[i]);
			fp = fopen(path, "wb");
			if (!fp || fwrite(bufs[i]->data, 1, bufs[i]->size, fp) != bufs[i]->size) {
				fprintf(stderr, "cannot write %s\n", path);
				return 1;
			}
			fclose(fp);
			printf("%s %u bytes\n", path, bufs[i]->size);
		}
	}
	modLevelGenFilesFree(&f);
	return 0;
}
#endif
