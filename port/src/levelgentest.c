#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "data.h"
#include "bss.h"
#include "system.h"
#include "romdata.h"
#include "preprocess.h"
#include "levelgenstage.h"
#include "game/modlevelgenfile.h"
#include "game/modlevelgen.h"

/**
 * The generator's files through the game's own loaders.
 *
 * modLevelGenFilesCheck() (modlevelgenfile.c) walks the files the way the N64
 * loaders do. This runs the port's preprocess steps (port/src/preprocess/)
 * over them the way fileLoad() and bgLoadRoom() call them, with the same
 * buffer sizes, because those steps widen the pointers and have only a fixed
 * share of the file's size to do it in: a file that is fine for the N64 can
 * overflow here (a bg file did, the first time one was loaded).
 *
 * Run with: pd.x86_64.exe --selftest-levelgen-files
 */

#define ALIGN16_(v) (((v) + 15u) & ~15u)

static u32 lgtBe32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/** Inflates a stored-block rzip-1173 file (what the generator writes). Returns a malloc'd copy or NULL. */
static u8 *lgtUnstore(const LgBuf *f, u32 *outlen)
{
	const u8 *d = f->data;
	u32 want, o = 5, n = 0;
	u8 *out;

	if (f->size < 10 || d[0] != 0x11 || d[1] != 0x73) {
		return NULL;
	}

	want = ((u32)d[2] << 16) | ((u32)d[3] << 8) | d[4];
	out = malloc(want + 16);

	if (!out) {
		return NULL;
	}

	for (;;) {
		u32 len;
		const u32 last = d[o] & 1;

		if (o + 5 > f->size || (d[o] & 6)) {
			break;
		}

		len = d[o + 1] | ((u32)d[o + 2] << 8);

		if (o + 5 + len > f->size || n + len > want) {
			break;
		}

		memcpy(out + n, d + o + 5, len);
		n += len;
		o += 5 + len;

		if (last) {
			*outlen = n;
			return out;
		}
	}

	free(out);
	return NULL;
}

static int lgtCheck(void (*print)(const char *), int ok, const char *name, const char *detail)
{
	char line[200];

	snprintf(line, sizeof(line), ok ? "SELFTEST PASS %s" : "SELFTEST FAIL %s: %s", name, detail ? detail : "");
	print(line);
	return ok ? 0 : 1;
}

/** fileLoad(): inflate, then romdataFilePreprocess(size = the inflated size aligned up) into a buffer sized by fileLoadToNew(). */
static int lgtFile(const LgBuf *f, s32 loadtype, u8 *(*fn)(u8 *, u32, u32 *), u32 *outsize, char *err, int errlen)
{
	u32 n = 0;
	u8 *raw = lgtUnstore(f, &n);
	u32 size, est, outs = 0;
	u8 *buf;

	if (!raw) {
		snprintf(err, errlen, "not a stored rzip file");
		return 0;
	}

	size = ALIGN16_(n);
	est = romdataFileGetEstimatedSize(n, loadtype);
	buf = calloc(1, (est > size ? est : size) + 0x100);

	if (!buf) {
		free(raw);
		snprintf(err, errlen, "out of memory");
		return 0;
	}

	memcpy(buf, raw, n);
	free(raw);
	fn(buf, size, &outs);
	free(buf);

	if (outs == 0) {
		snprintf(err, errlen, "converted to nothing");
		return 0;
	}

	*outsize = outs;
	return 1;
}

/** bgLoad(): header, primary block, then the room, as bg.c reads them. */
static int lgtBg(const LgBuf *f, char *err, int errlen)
{
	u8 hdr[0x40];
	u32 inflated, primcomp, s1;
	u32 est, tab, ptr0, ptr1, roomlen, alloclen, newlen, r;
	const u8 *prim;
	u8 *buf, *room;

	if (f->size < 0x40) {
		snprintf(err, errlen, "file too small");
		return 0;
	}

	memcpy(hdr, f->data, 0x40);
	preprocessBgSection1Header(hdr, 0x40);
	inflated = *(u32 *)&hdr[0];
	s1 = *(u32 *)&hdr[4];
	primcomp = *(u32 *)&hdr[8];

	if (primcomp != inflated || 12 + s1 > f->size) {
		snprintf(err, errlen, "section 1 sizes %u/%u/%u disagree", inflated, s1, primcomp);
		return 0;
	}

	est = ALIGN16_(romdataFileGetEstimatedSize(inflated, LOADTYPE_BG));
	buf = calloc(1, est + 0x8010);

	if (!buf) {
		snprintf(err, errlen, "out of memory");
		return 0;
	}

	prim = f->data + 12;
	tab = lgtBe32(prim + 4) & 0xffffff;
	memcpy(buf, prim, inflated);
	preprocessBgSection1(buf, est, 0x0f000000);
	free(buf);

	/* each room: the data between two room table rows, in bgLoadRoom()'s
	 * buffer (the listed size in 16s plus 0x100, four times over) */
	for (r = 1; lgtBe32(prim + tab + 20 * (r + 1)) != 0; r++) {
		ptr0 = lgtBe32(prim + tab + 20 * r);
		ptr1 = lgtBe32(prim + tab + 20 * (r + 1));
		roomlen = ptr1 - ptr0;
		alloclen = ALIGN16_((roomlen / 16 + 1) * 16 + 0x100) * 4;
		room = calloc(1, alloclen);

		if (!room) {
			snprintf(err, errlen, "out of memory");
			return 0;
		}

		memcpy(room, f->data + 12 + primcomp + (ptr0 - 0x0f000000 - inflated), roomlen);
		newlen = preprocessBgRoom(room, roomlen, ptr0);
		free(room);

		if (newlen == 0 || newlen > alloclen) {
			snprintf(err, errlen, "room %u became %u bytes, buffer %u", r, newlen, alloclen);
			return 0;
		}
	}

	return 1;
}

/* The four files of one level through the four preprocessors */
static int lgtFiles(void (*print)(const char *line), const char *what, LgFiles *f, int *passed)
{
	char err[160];
	char name[96];
	u32 outsize = 0;
	int ok, failed = 0, i;

	for (i = 0; i < 4; i++) {
		err[0] = '\0';

		switch (i) {
		case 0: ok = lgtBg(&f->bg, err, sizeof(err)); break;
		case 1: ok = lgtFile(&f->tiles, LOADTYPE_TILES, preprocessTilesFile, &outsize, err, sizeof(err)); break;
		case 2: ok = lgtFile(&f->pads, LOADTYPE_PADS, preprocessPadsFile, &outsize, err, sizeof(err)); break;
		default: ok = lgtFile(&f->setup, LOADTYPE_SETUP, preprocessSetupFile, &outsize, err, sizeof(err)); break;
		}

		snprintf(name, sizeof(name), "levelgen-loaders: %s %s", what, i == 0 ? "bg" : i == 1 ? "tiles" : i == 2 ? "pads" : "setup");
		failed += lgtCheck(print, ok, name, err);
		*passed += ok;
	}

	return failed;
}

int levelgenLoadersSelfTest(void (*print)(const char *line))
{
	static const s16 sizes[3][2] = { { 350, 350 }, { 500, 500 }, { 720, 720 } };
	int failed = 0, passed = 0;
	int i;
	LgLayout *lg = malloc(sizeof(*lg));

	/* whole layouts: three seeds of each size */
	for (i = 0; lg && i < 9; i++) {
		LgParams prm;
		LgBoxParams th;
		LgFiles f;
		char err[160] = "";
		char what[64];

		prm.seed = 31337u + (u32)i * 104729u;
		prm.size = (u8)(i % 3);
		modLevelGenBoxDefaults(&th);
		snprintf(what, sizeof(what), "layout seed %u size %d", prm.seed, prm.size);

		if (!modLevelGenBuild(lg, &prm, err, sizeof(err)) || !modLevelGenLayoutFiles(lg, &th, &f, err, sizeof(err))) {
			char name[96];

			snprintf(name, sizeof(name), "levelgen-loaders: %s built", what);
			failed += lgtCheck(print, 0, name, err);
			continue;
		}

		failed += lgtFiles(print, what, &f, &passed);
		modLevelGenFilesFree(&f);
	}

	free(lg);

	for (i = 0; i < 3; i++) {
		LgBoxParams p;
		LgFiles f;
		char err[160] = "";
		char name[64];
		u32 outsize = 0;
		int ok;

		modLevelGenBoxDefaults(&p);
		p.halfx = sizes[i][0];
		p.halfz = sizes[i][1];

		if (!modLevelGenBoxFiles(&p, &f, err, sizeof(err))) {
			snprintf(name, sizeof(name), "levelgen-loaders: box %d built", i);
			failed += lgtCheck(print, 0, name, err);
			continue;
		}

		err[0] = '\0';
		snprintf(name, sizeof(name), "levelgen-loaders: box %d bg", i);
		ok = lgtBg(&f.bg, err, sizeof(err));
		failed += lgtCheck(print, ok, name, err);
		passed += ok;

		err[0] = '\0';
		snprintf(name, sizeof(name), "levelgen-loaders: box %d tiles", i);
		ok = lgtFile(&f.tiles, LOADTYPE_TILES, preprocessTilesFile, &outsize, err, sizeof(err));
		failed += lgtCheck(print, ok, name, err);
		passed += ok;

		err[0] = '\0';
		snprintf(name, sizeof(name), "levelgen-loaders: box %d pads", i);
		ok = lgtFile(&f.pads, LOADTYPE_PADS, preprocessPadsFile, &outsize, err, sizeof(err));
		failed += lgtCheck(print, ok, name, err);
		passed += ok;

		err[0] = '\0';
		snprintf(name, sizeof(name), "levelgen-loaders: box %d setup", i);
		ok = lgtFile(&f.setup, LOADTYPE_SETUP, preprocessSetupFile, &outsize, err, sizeof(err));
		failed += lgtCheck(print, ok, name, err);
		passed += ok;

		modLevelGenFilesFree(&f);
	}

	{
		char line[100];

		snprintf(line, sizeof(line), "SELFTEST RESULT levelgen-loaders passed %d failed %d", passed, failed);
		print(line);
	}

	return failed;
}
