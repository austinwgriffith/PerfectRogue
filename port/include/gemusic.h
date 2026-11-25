#ifndef _IN_GEMUSIC_H
#define _IN_GEMUSIC_H

#include <PR/ultratypes.h>

/**
 * GoldenEye's own music for GoldenEye's levels (port/src/gemusic.c).
 *
 * The conversion keeps GoldenEye's instrument bank and sequence table
 * (mods/goldeneye/menu/instrumentsctl, instrumentstbl, sequences), how loud
 * each sequence plays (musicvolumes.bin) and the sequences a level with no
 * theme of its own draws from (musicrandom.bin); the level's own row comes from
 * its line of modconfig.txt (gelevelsGetMusic()). A sequence is appended after
 * the game's own the first time a level asks for it (seqAppend()), and plays on
 * GoldenEye's instruments.
 */
#define GEMUSIC_MAIN       0
#define GEMUSIC_BACKGROUND 1
#define GEMUSIC_X          2

// what geMusicStageTrack() answers for a stage whose music is not GoldenEye's
#define GEMUSIC_NOTOURS (-2)

// A stage's main theme, background or X theme as this game's sequence number,
// -1 where GoldenEye gives the level none, and GEMUSIC_NOTOURS for a stage that
// is not one of GoldenEye's, or when no conversion music is there. A level with
// no main theme draws one of GoldenEye's random tracks and keeps it until the
// next level starts.
s32 geMusicStageTrack(s32 stagenum, s32 which);

// A level is starting: the next one with no theme of its own draws again
void geMusicStageReset(void);

#endif
