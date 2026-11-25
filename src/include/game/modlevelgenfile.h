/*
 * modlevelgenfile.h: Perfect Random level generator, file writers.
 *
 * Turns geometry into the four files a Perfect Dark stage is made of:
 *   bg      the rooms (display lists), portals, room table
 *   tiles   floor and wall collision
 *   pads    pads, waypoints, waygroups
 *   setup   intro (spawn pads), props, paths, ai lists
 * All in the N64's big-endian layout, which the game's preprocess steps turn
 * into what the PC build wants (the same as port/src/geconvert.c writes).
 * Plain C99, no engine code, so it can be tested on its own:
 *   gcc -O1 -Wall -Wextra -std=c99 -DMODLEVELGENFILE_MAIN -I src/include \
 *       -o lgfile src/game/modlevelgenfile.c && ./lgfile outdir
 *
 * Phase 0 writes one box room (modLevelGenBoxFiles). Later phases add the
 * full layout writer next to it.
 */
#ifndef _IN_GAME_MODLEVELGENFILE_H
#define _IN_GAME_MODLEVELGENFILE_H

#include <stdint.h>

#define MODLEVELGENFILE_VERSION 1

typedef struct {
	uint8_t *data;
	uint32_t size;
} LgBuf;

typedef struct {
	LgBuf bg;
	LgBuf tiles;
	LgBuf pads;
	LgBuf setup;
} LgFiles;

typedef struct {
	int16_t halfx, halfz;   /* room half sizes, world units */
	int16_t ceil;           /* ceiling height */
	uint16_t tex[3];        /* Perfect Dark texture numbers, by LGSURF_ */
	uint32_t texword0;      /* the texture command's first word (0xc0......) */
	int16_t uvscale[3];     /* texture s/t units (1/32 texel) per 100 world units, by LGSURF_ */
	uint16_t floortype;     /* footstep sound: PD's SURFACETYPE */
	uint16_t floorcolour;   /* the floor tile's 4-4-4 shade */
	uint32_t shade;         /* RGBA colour of every vertex (prelit) */
} LgBoxParams;

/* The box's surfaces, each with its own texture */
#define LGSURF_FLOOR 0
#define LGSURF_WALL  1
#define LGSURF_CEIL  2

/* Lighting of a layout's areas (the same numbers the bg file is written with):
 * the brightest an area gets (the game's 0-255), how many lights it has, and
 * whether its light j starts broken (from the seed alone) */
struct LgLayoutTag;
int modLevelGenAreaBrightness(const struct LgLayoutTag *lg, int ai);
int modLevelGenAreaLights(const struct LgLayoutTag *lg, int ai);
int modLevelGenAreaFitting(const struct LgLayoutTag *lg, int ai, int32_t *halfx, int32_t *halfz); /* its light fittings' half sizes; returns the texture */
int modLevelGenAreaLightPos(const struct LgLayoutTag *lg, int ai, int32_t pos[][2], int max); /* world x, z of each */
int modLevelGenLightBroken(const struct LgLayoutTag *lg, int ai, int light);

/* Layout of the box's pads, so tests and the engine can name them */
#define LGBOX_PAD_START0   0
#define LGBOX_PAD_START1   1
#define LGBOX_PAD_START2   2
#define LGBOX_PAD_GUARD0   3
#define LGBOX_NUMGUARDPADS 4
#define LGBOX_NUMPADS      7

void modLevelGenBoxDefaults(LgBoxParams *p);

/* Builds the four files. Returns 1 on success, else writes a reason to err.
 * Free with modLevelGenFilesFree(). */
int modLevelGenBoxFiles(const LgBoxParams *p, LgFiles *out, char *err, int errlen);

void modLevelGenFilesFree(LgFiles *f);

/* Parses the files back the way the game's loaders walk them and checks that
 * every offset and count agrees. Returns 1 if all hold. */
int modLevelGenFilesCheck(const LgFiles *f, char *err, int errlen);

/* Phase 1: the files of a whole layout (modlevelgen.c), with the surface
 * textures and shading of th (its box sizes are not used). Returns 1 on
 * success, else writes a reason to err. */
struct LgLayoutTag;
int modLevelGenLayoutFiles(const struct LgLayoutTag *lg, const LgBoxParams *th, LgFiles *out, char *err, int errlen);

/* Checks a layout's files against the layout: rooms, portals, display lists,
 * the port's size budget, a floor tile under every pad in its own room, the
 * pads and the spawns. Returns 1 if all hold. */
int modLevelGenLayoutCheck(const struct LgLayoutTag *lg, const LgFiles *f, char *err, int errlen);

/* Builds and checks the files of many layouts of every size; SELFTEST lines
 * through print; returns the number of failures. */
int modLevelGenLayoutSelfTest(int numseeds, void (*print)(const char *line));

/* Self-test: builds boxes of several sizes, checks them, checks that the same
 * parameters give identical bytes. Prints SELFTEST lines through print;
 * returns the number of failures. */
int modLevelGenFileSelfTest(void (*print)(const char *line));

#endif
