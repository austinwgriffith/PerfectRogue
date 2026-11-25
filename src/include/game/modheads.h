#ifndef _IN_GAME_MODHEADS_H
#define _IN_GAME_MODHEADS_H

#include <ultra64.h>
#include "types.h"

/**
 * Perfect Random's head bank: generated female faces, each one a stock female
 * head model wearing textures of its own.
 *
 * The bank is data, not code. tools/perfect-random-heads/ builds it into
 * perfect-random-heads/ beside the executable:
 *
 *   heads.txt                one head a line: "<base head number> <name>"
 *   textures/NN_TTTT.bin     head NN's own copy of the base model's texture
 *                            TTTT, in the ROM's compressed texture format
 *
 * Head NN is g_HeadsAndBodies row MODHEADS_FIRST + NN, a copy of its base
 * head's row. While that row's model loads (modHeadsBeginLoad()), every
 * texture it names is asked of the bank first, and keyed in the texture pool
 * under a source of the bank's own (MODHEADS_SRCMOD_BASE + NN) beside the
 * number, the way a borrowed mod's textures are (modTextureSource()). So the
 * base head's own texture number, size, format and UVs are kept exactly - a
 * texture the bank has no file for is simply the base head's - and the stock
 * head, other heads in the bank and any texture pack's art under the same
 * number never meet it. The previous scheme re-pointed the faces at spare ROM
 * texture slots, several of which were not 32x64 at all.
 */

#define MODHEADS_FIRST       512
#define MODHEADS_MAX         64
#define MODHEADS_SRCMOD_BASE 0xc0 // tex->srcmod is 8 bits; mods use 1 + their dir index
#define MODHEADS_DIR         "$E/perfect-random-heads"
#define MODHEADS_PACKED_BASE (-100) // a head in a packed setup record: -100 - slot

void modHeadsLoad(void);
void modHeadsReload(void);
s32 modHeadsCount(void);
s32 modHeadsHeadnum(s32 slot);
s32 modHeadsSlotOf(s32 headnum);
bool modHeadsIsCustom(s32 headnum);
const char *modHeadsLabel(s32 slot);
void modHeadsEnsureRows(void);
void modHeadsLogLoad(s32 headnum); // one log line a bank head: its textures found or missed

s32 modHeadsPack(s32 headnum);
s32 modHeadsUnpack(s32 packed);

s32 modHeadsBeginLoad(s32 headnum);
void modHeadsEndLoad(s32 prev);
s32 modHeadsLoadingSlot(void);
bool modHeadsIsTextureSource(u32 srcmod);
s32 modHeadsTextureLoad(u16 num, void *dst, u32 dstSize);

#endif
