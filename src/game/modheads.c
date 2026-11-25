#include <ultra64.h>
#include "constants.h"
#include "prcompat.h"
#include "bss.h"
#include "data.h"
#include "game/modheads.h"
#ifndef PLATFORM_N64
#include <stdio.h>
#include <string.h>
#include "fs.h"
#include "system.h"
#endif
#include "types.h"

/**
 * Perfect Random's head bank - see modheads.h. Loaded once, the first time
 * anything asks; modHeadsReload() reads heads.txt again.
 */

struct modhead {
	s16 base;
	char name[24];
	char label[40];
};

static struct modhead g_ModHeads[MODHEADS_MAX];
static s32 g_ModHeadsCount = -1;
static s32 g_ModHeadsLoadingSlot = -1;
static s32 g_ModHeadsTexHit[MODHEADS_MAX];
static s32 g_ModHeadsTexMiss[MODHEADS_MAX];

static bool modHeadsBaseIsUsable(s32 base)
{
	return base > 0 && base < 151
		&& g_HeadsAndBodies[base].filenum != 0
		&& !g_HeadsAndBodies[base].ismale;
}

void modHeadsLoad(void)
{
#ifndef PLATFORM_N64
	u32 size = 0;
	char *text;
	char *line;
	s32 lineno = 0;
#endif

	if (g_ModHeadsCount >= 0) {
		return;
	}

#ifndef PLATFORM_N64
	// The config is registered before the exe's folder is known, and "$E"
	// then stays unexpanded: ask again later rather than keep an empty bank
	if (fsFullPath(MODHEADS_DIR "/heads.txt")[0] == '$') {
		return;
	}
#endif

	g_ModHeadsCount = 0;

#ifndef PLATFORM_N64
	text = fsFileLoadPadded(MODHEADS_DIR "/heads.txt", &size, 1);

	if (!text) {
		sysLogPrintf(LOG_NOTE, "modheads: no %s/heads.txt, the head bank is empty", MODHEADS_DIR);
		return;
	}

	line = text;

	while (line && *line && g_ModHeadsCount < MODHEADS_MAX) {
		char *next = strpbrk(line, "\r\n");
		char name[64];
		s32 base;

		if (next) {
			*next++ = '\0';
		}

		lineno++;

		while (*line == ' ' || *line == '\t') {
			line++;
		}

		if (*line && *line != '#') {
			name[0] = '\0';

			if (sscanf(line, "%i %63[^\r\n]", &base, name) == 2 && modHeadsBaseIsUsable(base)) {
				struct modhead *head = &g_ModHeads[g_ModHeadsCount];

				head->base = base;
				snprintf(head->name, sizeof(head->name), "%s", name);
				snprintf(head->label, sizeof(head->label), "%s: Full Dress", head->name);
				g_ModHeadsCount++;
			} else {
				// A bad line still takes its slot, so the slots after it keep
				// their texture files; it is a copy of Anka until fixed
				struct modhead *head = &g_ModHeads[g_ModHeadsCount];

				sysLogPrintf(LOG_WARNING, "modheads: heads.txt line %d is not \"<female head number> <name>\": %s", lineno, line);
				head->base = HEAD_ANKA;
				snprintf(head->name, sizeof(head->name), "Head %d", g_ModHeadsCount);
				snprintf(head->label, sizeof(head->label), "%s: Full Dress", head->name);
				g_ModHeadsCount++;
			}
		}

		line = next;
	}

	sysMemFree(text);
	sysLogPrintf(LOG_NOTE, "modheads: %d head(s) in the bank", g_ModHeadsCount);
#endif
}

void modHeadsReload(void)
{
	s32 i;

	for (i = 0; i < MODHEADS_MAX; i++) {
		g_HeadsAndBodies[MODHEADS_FIRST + i].filenum = 0;
		g_HeadsAndBodies[MODHEADS_FIRST + i].modeldef = NULL;
	}

	g_ModHeadsCount = -1;
	modHeadsLoad();
}

s32 modHeadsCount(void)
{
	modHeadsLoad();

	return g_ModHeadsCount > 0 ? g_ModHeadsCount : 0;
}

s32 modHeadsHeadnum(s32 slot)
{
	return MODHEADS_FIRST + slot;
}

s32 modHeadsSlotOf(s32 headnum)
{
	const s32 slot = headnum - MODHEADS_FIRST;

	return slot >= 0 && slot < modHeadsCount() ? slot : -1;
}

bool modHeadsIsCustom(s32 headnum)
{
	return modHeadsSlotOf(headnum) >= 0;
}

const char *modHeadsLabel(s32 slot)
{
	return slot >= 0 && slot < modHeadsCount() ? g_ModHeads[slot].label : "";
}

/**
 * Each bank head's row is its base head's, made when first wanted; the
 * modeldef is its own, since its textures are.
 */
void modHeadsEnsureRows(void)
{
	const s32 count = modHeadsCount();
	s32 i;

	for (i = 0; i < count; i++) {
		struct headorbody *row = &g_HeadsAndBodies[MODHEADS_FIRST + i];

		if (row->filenum == 0) {
			*row = g_HeadsAndBodies[g_ModHeads[i].base];
			row->modeldef = NULL;
		}
	}
}

s32 modHeadsPack(s32 headnum)
{
	const s32 slot = modHeadsSlotOf(headnum);

	return slot >= 0 ? MODHEADS_PACKED_BASE - slot : headnum;
}

/**
 * The head a packed setup record's number stands for, or the number itself
 * when it is not a bank head's.
 */
s32 modHeadsUnpack(s32 packed)
{
	if (packed <= MODHEADS_PACKED_BASE && packed > MODHEADS_PACKED_BASE - modHeadsCount()) {
		modHeadsEnsureRows();
		return MODHEADS_FIRST + (MODHEADS_PACKED_BASE - packed);
	}

	return packed;
}

void modHeadsLogLoad(s32 headnum)
{
#ifndef PLATFORM_N64
	const s32 slot = modHeadsSlotOf(headnum);

	if (slot >= 0) {
		sysLogPrintf(g_ModHeadsTexMiss[slot] ? LOG_WARNING : LOG_NOTE,
				"modheads: head %d (%s, base file %d) loaded: %d texture file(s) from the bank, %d missing",
				slot, g_ModHeads[slot].name, g_HeadsAndBodies[headnum].filenum,
				g_ModHeadsTexHit[slot], g_ModHeadsTexMiss[slot]);
		g_ModHeadsTexHit[slot] = 0;
		g_ModHeadsTexMiss[slot] = 0;
	}
#endif
}

s32 modHeadsBeginLoad(s32 headnum)
{
	const s32 prev = g_ModHeadsLoadingSlot;

	g_ModHeadsLoadingSlot = modHeadsSlotOf(headnum);

	if (g_ModHeadsLoadingSlot >= 0) {
		modHeadsEnsureRows();
	}

	return prev;
}

void modHeadsEndLoad(s32 prev)
{
	g_ModHeadsLoadingSlot = prev;
}

s32 modHeadsLoadingSlot(void)
{
	return g_ModHeadsLoadingSlot;
}

bool modHeadsIsTextureSource(u32 srcmod)
{
	return srcmod >= MODHEADS_SRCMOD_BASE && srcmod < MODHEADS_SRCMOD_BASE + MODHEADS_MAX;
}

/**
 * The loading head's own copy of texture num, or -1 to have the base head's
 * from the ROM.
 */
s32 modHeadsTextureLoad(u16 num, void *dst, u32 dstSize)
{
#ifndef PLATFORM_N64
	char path[256];
	s32 ret;

	if (g_ModHeadsLoadingSlot < 0) {
		return -1;
	}

	snprintf(path, sizeof(path), MODHEADS_DIR "/textures/%02d_%04x.bin", g_ModHeadsLoadingSlot, num);
	ret = fsFileLoadTo(path, dst, dstSize);

	if (ret > 0) {
		g_ModHeadsTexHit[g_ModHeadsLoadingSlot]++;
	} else {
		g_ModHeadsTexMiss[g_ModHeadsLoadingSlot]++;
		sysLogPrintf(LOG_WARNING, "modheads: no %s", path);
	}

	return ret > 0 ? ret : -1;
#else
	return -1;
#endif
}
