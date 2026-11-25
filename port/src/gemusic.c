#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include <PR/libaudio.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "fs.h"
#include "system.h"
#include "prstubs.h"
#include "preprocess.h"
#include "gemusic.h"
#include "gelevels.h"
#include "lib/rng.h"
#include "lib/snd.h"

/**
 * GoldenEye's own music, out of the player's ROM, on GoldenEye's levels.
 *
 * GoldenEye picks a level's music from one table - a main theme, a background
 * and an X theme a level - and a level the table does not name (the
 * multiplayer-only ones) draws its theme from a list of random tracks. The
 * converter puts each level's row on its line of modconfig.txt and writes the
 * rest beside it in mods/goldeneye/menu/. Perfect Dark's three questions
 * (stagemusic.c) are answered from those: the main theme is the primary track,
 * the background the ambient one.
 *
 * Each sequence plays at GoldenEye's own volume for it, on the scale an
 * appended sequence has here (three quarters of GoldenEye's, as the sound
 * effects are).
 */

#define GEMUSIC_DIR "mods/goldeneye/menu"
#define MAX_SEQUENCES 128
#define MAX_RANDOM    64
#define ADMA_ITEM_SIZE 0x400 // lib/audiodma.c: the sound DMA reads a whole item

static struct {
	bool loaded;     // the files were looked for
	bool have;       // and found
	u8 *seqs;        // the sequence table as the ROM has it
	u32 seqlen;
	s32 count;
	ALBank *bank;
	s16 seqnums[MAX_SEQUENCES]; // 0 not asked for yet, -1 cannot be had
	s16 volumes[MAX_SEQUENCES];
	s16 random[MAX_RANDOM];
	s32 numrandom;
	s32 drawnstage;
	s32 drawn;
} g_GeMusic = { .drawnstage = -1 };

static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u8 *geMusicLoadFile(const char *name, u32 *len, u32 pad)
{
	char path[FS_MAXPATH + 1];

	snprintf(path, sizeof(path), GEMUSIC_DIR "/%s", name);
	*len = 0;

	// asked for first: a load of a file that is not there is an error line of
	// its own
	return fsFileSize(path) > 0 ? fsFileLoadPadded(path, len, pad) : NULL;
}

// Reads the bank and the tables, once
static bool geMusicLoad(void)
{
	u32 len = 0;
	u32 ctllen = 0;
	u32 seqlen = 0;
	u8 *raw;
	u8 *ctl;
	u8 *tbl;
	u8 *seqs;
	u8 *list;

	if (g_GeMusic.loaded) {
		return g_GeMusic.have;
	}

	g_GeMusic.loaded = true;

	if (g_SndDisabled || !(raw = geMusicLoadFile("instrumentsctl", &len, 0))) {
		return false;
	}

	ctl = preprocessALBankFile(raw, len, &ctllen);
	sysMemFree(raw);
	// padded: the sound DMA reads a whole item from where a sample starts
	tbl = geMusicLoadFile("instrumentstbl", &len, ADMA_ITEM_SIZE);
	seqs = geMusicLoadFile("sequences", &seqlen, 0);

	if (!ctl || !tbl || !seqs || seqlen < 4) {
		sysMemFree(ctl);
		sysMemFree(tbl);
		sysMemFree(seqs);
		return false;
	}

	alBnkfNew((ALBankFile *)ctl, tbl);
	g_GeMusic.bank = ((ALBankFile *)ctl)->bankArray[0];
	g_GeMusic.seqs = seqs;
	g_GeMusic.seqlen = seqlen;
	g_GeMusic.count = (seqs[0] << 8) | seqs[1];

	if (g_GeMusic.count > MAX_SEQUENCES) {
		g_GeMusic.count = MAX_SEQUENCES;
	}

	if (seqlen < 4 + (u32)g_GeMusic.count * 8) {
		g_GeMusic.count = (seqlen - 4) / 8;
	}

	if ((list = geMusicLoadFile("musicvolumes.bin", &len, 0))) {
		for (u32 n = 0; n < len / 2 && n < MAX_SEQUENCES; n++) {
			g_GeMusic.volumes[n] = (s16)((list[n * 2] << 8) | list[n * 2 + 1]);
		}

		sysMemFree(list);
	}

	if ((list = geMusicLoadFile("musicrandom.bin", &len, 0))) {
		for (u32 n = 0; n < len / 2 && g_GeMusic.numrandom < MAX_RANDOM; n++) {
			const s16 geseq = (s16)((list[n * 2] << 8) | list[n * 2 + 1]);

			if (geseq <= 0) {
				break;
			}

			g_GeMusic.random[g_GeMusic.numrandom++] = geseq;
		}

		sysMemFree(list);
	}

	g_GeMusic.have = true;
	sysLogPrintf(LOG_NOTE, "gemusic: %d sequences from " GEMUSIC_DIR, g_GeMusic.count);

	return true;
}

// This game's sequence number for GoldenEye's geseq, appended the first time; -1 when it cannot be had
static s32 geMusicSequence(s32 geseq)
{
	const u8 *e;
	s32 seqnum = -1;

	if (!geMusicLoad() || geseq <= 0 || geseq >= MAX_SEQUENCES || geseq >= g_GeMusic.count) {
		return -1;
	}

	if (g_GeMusic.seqnums[geseq]) {
		return g_GeMusic.seqnums[geseq];
	}

	e = g_GeMusic.seqs + 4 + geseq * 8;

	if (be32(e) < g_GeMusic.seqlen && (u32)((e[6] << 8) | e[7]) <= g_GeMusic.seqlen - be32(e)) {
		seqnum = seqAppend(g_GeMusic.seqs + be32(e), (e[4] << 8) | e[5], (e[6] << 8) | e[7], g_GeMusic.bank);
	}

	if (seqnum >= 0 && g_GeMusic.volumes[geseq] > 0) {
		seqAppendSetScale(seqnum, g_GeMusic.volumes[geseq] * 3 / 4);
	}

	g_GeMusic.seqnums[geseq] = seqnum >= 0 ? seqnum : -1;

	return g_GeMusic.seqnums[geseq];
}

void geMusicStageReset(void)
{
	g_GeMusic.drawnstage = -1;
}

s32 geMusicStageTrack(s32 stagenum, s32 which)
{
	s32 tracks[3];
	s32 seqnum;

	if (which < 0 || which > GEMUSIC_X || !gelevelsIsStage(stagenum) || !gelevelsGetMusic(stagenum, tracks)) {
		return GEMUSIC_NOTOURS;
	}

	if (!geMusicLoad()) {
		return GEMUSIC_NOTOURS;
	}

	if (which == GEMUSIC_MAIN && tracks[GEMUSIC_MAIN] < 0) {
		// asked more than once as a level starts: the draw is the level's
		if (g_GeMusic.drawnstage != stagenum) {
			if (g_GeMusic.numrandom <= 0) {
				return GEMUSIC_NOTOURS;
			}

			g_GeMusic.drawn = g_GeMusic.random[rngRandom() % (u32)g_GeMusic.numrandom];
			g_GeMusic.drawnstage = stagenum;
		}

		tracks[GEMUSIC_MAIN] = g_GeMusic.drawn;
	}

	if (tracks[which] < 0) {
		return -1;
	}

	seqnum = geMusicSequence(tracks[which]);

	if (seqnum < 0) {
		return which == GEMUSIC_MAIN ? GEMUSIC_NOTOURS : -1;
	}

	return seqnum;
}
