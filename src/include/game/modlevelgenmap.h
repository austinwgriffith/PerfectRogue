/*
 * modlevelgenmap.h: Perfect Random level generator, top-down map pictures.
 *
 * Draws a layout (modlevelgen.h) from above as an RGB picture and writes it
 * as a PNG: rooms coloured by theme, walls, openings, doors, pillars, light
 * fittings, the navigation pads and their links, guard / start / objective
 * pads, and the start, extraction and objective rooms. For iterating on the
 * generator: pd.x86_64.exe --levelgen-map SEED [--levelgen-size 0|1|2]
 * [--levelgen-count N] writes one picture per seed and a sheet of them all.
 * Plain C99, no engine code (like modlevelgen.c), so it can be tested alone.
 */
#ifndef _IN_GAME_MODLEVELGENMAP_H
#define _IN_GAME_MODLEVELGENMAP_H

#include <stdint.h>

struct LgLayoutTag;

typedef struct {
	uint8_t *px;            /* RGB, rows top down */
	int w, h;
} LgImage;

/* Draws the layout, cellpx pixels per grid cell (0 picks a size about 960
 * pixels across). With header, a title and a key above the map. Returns 1 on
 * success; free the picture with modLevelGenImageFree(). */
int modLevelGenMapRender(const struct LgLayoutTag *lg, int cellpx, int header, LgImage *out);

/* Pictures side by side, cols to a row, each with its seed written under it */
int modLevelGenImageSheet(const LgImage *imgs, const uint32_t *seeds, int n, int cols, LgImage *out);

/* A PNG file of the picture (stored, not compressed). Free *png with free(). */
int modLevelGenImagePng(const LgImage *img, uint8_t **png, uint32_t *size);

void modLevelGenImageFree(LgImage *img);

/* Marks a world position on a picture modLevelGenMapRender() drew with the
 * same cellpx and header: a square ring (ring) or a cross */
void modLevelGenMapMark(const struct LgLayoutTag *lg, LgImage *img, int cellpx, int header,
		int32_t x, int32_t z, uint32_t rgb, int ring);

/* Writes the picture as a PNG file; returns 1 if it was written */
int modLevelGenImageWritePng(const LgImage *img, const char *path);

/* Colours the map uses, so tests can look for them */
#define LGMAP_RGB_START     0x40ff60
#define LGMAP_RGB_EXTRACT   0xff40ff
#define LGMAP_RGB_OBJECTIVE 0xffd040
#define LGMAP_RGB_GUARD     0xff4040
#define LGMAP_RGB_DOOR      0xff8c1a
#define LGMAP_RGB_PILLAR    0x101010
#define LGMAP_RGB_COVER     0x9aa4b0
#define LGMAP_RGB_WALL      0xe8e8e8
#define LGMAP_RGB_ROCK      0x1c1c1c

/* The picture's own self-test: draws many layouts and checks what is where.
 * Returns the number of failures. */
int modLevelGenMapSelfTest(int numseeds, void (*print)(const char *line));

#endif
