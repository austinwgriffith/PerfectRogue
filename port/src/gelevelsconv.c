#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <SDL.h>
#include <ultra64.h>
#include "types.h"
#include "fs.h"
#include "system.h"
#include "geconvert.h"
#include "gelevels.h"

/**
 * First-run conversion: if the player has put their GoldenEye 007 (USA) ROM
 * beside Perfect Dark's and mods/goldeneye has not been made from it yet, make
 * it now, so nothing else needs running. The converter (geconvert.c, also built
 * alone by tools/geconvert/convert.sh) writes a full mod into a scratch folder
 * and only what the levels use is moved out of it.
 *
 * It runs on a thread of its own while this one shows a small "Converting
 * GoldenEye" window with a progress bar, so the game's own black window is not
 * all a player sees.
 */

#define GELEVELS_STAMP_FILE "geconvert.version"

struct gelevelsconvjob {
	u8 *rom;
	size_t romlen;
	char outdir[FS_MAXPATH + 1];
	char tmpdir[FS_MAXPATH + 1];
	char err[256];
	volatile int phase; // 0 converting, 1 moving files into place, 2 cleaning up
	volatile int done;
	int ok;
};

static struct gelevelsconvjob g_Job;

// This runs on the worker thread; the log is not thread safe, so it is mute.
static void gelevelsConvLog(const char *msg)
{
}

static bool gelevelsConvIsDir(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void gelevelsConvMakeDirs(const char *path)
{
	char tmp[FS_MAXPATH + 1];
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", path);

	for (p = tmp + 1; *p; p++) {
		if (*p == '/' || *p == '\\') {
			char c = *p;
			*p = '\0';
			if (!gelevelsConvIsDir(tmp)) {
				fsCreateDir(tmp);
			}
			*p = c;
		}
	}

	if (!gelevelsConvIsDir(tmp)) {
		fsCreateDir(tmp);
	}
}

// Deletes dir and everything in it
static void gelevelsConvRemoveTree(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char path[FS_MAXPATH + 1];

	if (!d) {
		return;
	}

	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
			continue;
		}

		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);

		if (gelevelsConvIsDir(path)) {
			gelevelsConvRemoveTree(path);
		} else {
			remove(path);
		}
	}

	closedir(d);
	rmdir(dir);
}

static bool gelevelsConvCopyFile(const char *from, const char *to)
{
	FILE *in = fopen(from, "rb");
	FILE *out;
	char buf[65536];
	size_t n;
	bool ok = true;

	if (!in) {
		return false;
	}

	out = fopen(to, "wb");

	if (!out) {
		fclose(in);
		return false;
	}

	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			ok = false;
			break;
		}
	}

	fclose(in);

	if (fclose(out) != 0) {
		ok = false;
	}

	return ok;
}

// Moves a file (a rename, which is instant on one drive; else a copy)
static bool gelevelsConvMoveFile(const char *from, const char *to)
{
	if (rename(from, to) == 0) {
		return true;
	}

	if (gelevelsConvCopyFile(from, to)) {
		remove(from);
		return true;
	}

	return false;
}

// Moves the files in fromdir whose names start with prefix into todir
static bool gelevelsConvMoveFiles(const char *fromdir, const char *todir, const char *prefix)
{
	DIR *d = opendir(fromdir);
	struct dirent *e;
	char from[FS_MAXPATH + 1];
	char to[FS_MAXPATH + 1];
	size_t plen = strlen(prefix);
	bool ok = true;

	if (!d) {
		return false;
	}

	gelevelsConvMakeDirs(todir);

	while ((e = readdir(d)) != NULL) {
		if (strncmp(e->d_name, prefix, plen) != 0) {
			continue;
		}

		snprintf(from, sizeof(from), "%s/%s", fromdir, e->d_name);

		if (gelevelsConvIsDir(from)) {
			continue;
		}

		snprintf(to, sizeof(to), "%s/%s", todir, e->d_name);

		if (!gelevelsConvMoveFile(from, to)) {
			ok = false;
		}
	}

	closedir(d);
	return ok;
}

// A GoldenEye 007 (USA) ROM, in any dump byte order, in dir: its bytes, or NULL
static u8 *gelevelsConvFindRomIn(const char *dir, size_t *len)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char path[FS_MAXPATH + 1];
	u8 *rom = NULL;

	if (!d) {
		return NULL;
	}

	while (!rom && (e = readdir(d)) != NULL) {
		struct stat st;
		u8 head[0x40];
		FILE *f;

		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);

		if (stat(path, &st) != 0 || S_ISDIR(st.st_mode) || (size_t)st.st_size != GECONVERT_ROM_SIZE) {
			continue;
		}

		f = fopen(path, "rb");

		if (!f) {
			continue;
		}

		if (fread(head, 1, sizeof(head), f) == sizeof(head) && geconvertHeaderIsGoldenEyeUs(head, sizeof(head))) {
			rom = malloc(GECONVERT_ROM_SIZE);

			fseek(f, 0, SEEK_SET);

			if (rom && fread(rom, 1, GECONVERT_ROM_SIZE, f) == GECONVERT_ROM_SIZE) {
				*len = GECONVERT_ROM_SIZE;
				sysLogPrintf(LOG_NOTE, "gelevels: GoldenEye ROM found: %s", path);
			} else {
				free(rom);
				rom = NULL;
			}
		}

		fclose(f);
	}

	closedir(d);
	return rom;
}

static bool gelevelsConvStampMatches(const char *outdir)
{
	char path[FS_MAXPATH + 1];
	char buf[16] = {0};
	FILE *f;

	snprintf(path, sizeof(path), "%s/" GELEVELS_STAMP_FILE, outdir);
	f = fopen(path, "rb");

	if (!f) {
		return false;
	}

	fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);

	return strcmp(buf, GECONVERT_VERSION_STR) == 0;
}

// The conversion itself, all of it, on the worker thread
static int gelevelsConvWork(void *arg)
{
	struct gelevelsconvjob *job = arg;
	char path[FS_MAXPATH + 1];
	char path2[FS_MAXPATH + 1];
	bool ok;

	job->ok = 0;
	job->phase = 0;

	geconvertSetLog(gelevelsConvLog);
	gelevelsConvRemoveTree(job->tmpdir);
	gelevelsConvMakeDirs(job->tmpdir);

	if (!geconvertRun(job->rom, job->romlen, job->tmpdir, job->err, sizeof(job->err))) {
		gelevelsConvRemoveTree(job->tmpdir);
		job->done = 1;
		return 0;
	}

	// keep only what the levels use: the level files, prop models, textures
	job->phase = 1;

	gelevelsConvRemoveTree(job->outdir);
	snprintf(path, sizeof(path), "%s/files", job->outdir);
	gelevelsConvMakeDirs(path);

	snprintf(path, sizeof(path), "%s/modconfig.txt", job->tmpdir);
	snprintf(path2, sizeof(path2), "%s/modconfig.txt", job->outdir);
	ok = gelevelsConvMoveFile(path, path2);

	snprintf(path, sizeof(path), "%s/files/bgdata", job->tmpdir);
	snprintf(path2, sizeof(path2), "%s/files/bgdata", job->outdir);
	ok = gelevelsConvMoveFile(path, path2) && ok;

	snprintf(path, sizeof(path), "%s/textures", job->tmpdir);
	snprintf(path2, sizeof(path2), "%s/textures", job->outdir);
	ok = gelevelsConvMoveFile(path, path2) && ok;

	// GoldenEye's music: the instrument bank, the sequences and their tables
	// (gemusic.c). A missing one is not fatal: the levels play without it.
	snprintf(path, sizeof(path), "%s/menu", job->tmpdir);
	snprintf(path2, sizeof(path2), "%s/menu", job->outdir);
	gelevelsConvMoveFiles(path, path2, "instruments");
	gelevelsConvMoveFiles(path, path2, "sequences");
	gelevelsConvMoveFiles(path, path2, "music");
	gelevelsConvMoveFiles(path, path2, "sfx");

	snprintf(path, sizeof(path), "%s/files", job->tmpdir);
	snprintf(path2, sizeof(path2), "%s/files", job->outdir);
	ok = gelevelsConvMoveFiles(path, path2, "Pgx") && ok;
	ok = gelevelsConvMoveFiles(path, path2, "Ump_setupgx") && ok;

	job->phase = 2;
	gelevelsConvRemoveTree(job->tmpdir);

	if (!ok) {
		snprintf(job->err, sizeof(job->err), "could not write %s (disk full or read-only?)", job->outdir);
		job->done = 1;
		return 0;
	}

	// the stamp last, so an interrupted run is made again next start
	snprintf(path, sizeof(path), "%s/" GELEVELS_STAMP_FILE, job->outdir);

	{
		FILE *f = fopen(path, "wb");

		if (f) {
			fputs(GECONVERT_VERSION_STR, f);
			fclose(f);
		}
	}

	job->ok = 1;
	job->done = 1;
	return 0;
}

/* The loading window: a 5x7 font for the few letters it needs, and a bar */

static const char g_GlyphChars[] = "CGEonvertigldy0123456789% ";
static const u8 g_Glyphs[][7] = {
	{ 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e }, // 'C',
	{ 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f }, // 'G',
	{ 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f }, // 'E',
	{ 0x00, 0x00, 0x0e, 0x11, 0x11, 0x11, 0x0e }, // 'o',
	{ 0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11 }, // 'n',
	{ 0x00, 0x00, 0x11, 0x11, 0x11, 0x0a, 0x04 }, // 'v',
	{ 0x00, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0e }, // 'e',
	{ 0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10 }, // 'r',
	{ 0x08, 0x08, 0x1c, 0x08, 0x08, 0x09, 0x06 }, // 't',
	{ 0x04, 0x00, 0x0c, 0x04, 0x04, 0x04, 0x0e }, // 'i',
	{ 0x00, 0x0f, 0x11, 0x11, 0x0f, 0x01, 0x0e }, // 'g',
	{ 0x0c, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e }, // 'l',
	{ 0x01, 0x01, 0x0d, 0x13, 0x11, 0x11, 0x0f }, // 'd',
	{ 0x00, 0x11, 0x11, 0x11, 0x0f, 0x01, 0x0e }, // 'y',
	{ 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e }, // '0',
	{ 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e }, // '1',
	{ 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f }, // '2',
	{ 0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e }, // '3',
	{ 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 }, // '4',
	{ 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e }, // '5',
	{ 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e }, // '6',
	{ 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, // '7',
	{ 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e }, // '8',
	{ 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c }, // '9',
	{ 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 }, // '%',
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // ' '
};

static void gelevelsConvDrawText(SDL_Renderer *ren, const char *text, int x, int y, int scale)
{
	for (; *text; text++) {
		const char *at = strchr(g_GlyphChars, *text);

		if (at && *text) {
			const u8 *rows = g_Glyphs[at - g_GlyphChars];

			for (int r = 0; r < 7; r++) {
				for (int c = 0; c < 5; c++) {
					if (rows[r] & (0x10 >> c)) {
						SDL_Rect px = { x + c * scale, y + r * scale, scale, scale };
						SDL_RenderFillRect(ren, &px);
					}
				}
			}
		}

		x += 6 * scale;
	}
}

static int gelevelsConvTextWidth(const char *text, int scale)
{
	return (int)strlen(text) * 6 * scale - scale;
}

static int gelevelsConvPercent(void)
{
	int total = geconvertTotal();
	int p = geconvertProgress();
	int pct;

	if (g_Job.done) {
		return 100;
	}

	if (g_Job.phase == 1) {
		return 85;
	}

	if (g_Job.phase == 2) {
		return 95;
	}

	if (total <= 0) {
		return 0;
	}

	// the conversion is most of the work; it does not count past 80
	if (p > total) {
		p = total;
	}

	pct = p * 80 / total;
	return pct < 0 ? 0 : pct;
}

static void gelevelsConvDraw(SDL_Renderer *ren, int w, int h, int pct)
{
	char label[8];
	const char *title = "Converting GoldenEye";
	SDL_Rect bar = { 30, h - 56, w - 60, 22 };
	SDL_Rect fill = { bar.x + 3, bar.y + 3, (bar.w - 6) * pct / 100, bar.h - 6 };
	int titlew = gelevelsConvTextWidth(title, 3);
	int labelw;

	snprintf(label, sizeof(label), "%d%%", pct);
	labelw = gelevelsConvTextWidth(label, 2);

	SDL_SetRenderDrawColor(ren, 18, 20, 28, 255);
	SDL_RenderClear(ren);

	SDL_SetRenderDrawColor(ren, 235, 235, 240, 255);
	gelevelsConvDrawText(ren, title, (w - titlew) / 2, 26, 3);

	SDL_SetRenderDrawColor(ren, 90, 96, 112, 255);
	SDL_RenderDrawRect(ren, &bar);

	SDL_SetRenderDrawColor(ren, 70, 170, 255, 255);
	SDL_RenderFillRect(ren, &fill);

	SDL_SetRenderDrawColor(ren, 160, 165, 180, 255);
	gelevelsConvDrawText(ren, label, (w - labelw) / 2, bar.y - 22, 2);

	SDL_RenderPresent(ren);
}

// Runs the conversion on a thread and shows the bar until it finishes
static void gelevelsConvRunWithBar(void)
{
	const int w = 460;
	const int h = 130;
	SDL_Window *win;
	SDL_Renderer *ren = NULL;
	SDL_Thread *thread;

	win = SDL_CreateWindow("Perfect Dark", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
			SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP);

	if (win) {
		ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
	}

	thread = ren ? SDL_CreateThread(gelevelsConvWork, "geconvert", &g_Job) : NULL;

	if (!thread) {
		// no window or no thread: do it here, without the bar
		if (ren) {
			SDL_DestroyRenderer(ren);
		}

		if (win) {
			SDL_DestroyWindow(win);
		}

		gelevelsConvWork(&g_Job);
		return;
	}

	while (!g_Job.done) {
		SDL_Event ev;

		while (SDL_PollEvent(&ev)) {
			// the window cannot be closed mid-way; events are only drained
		}

		gelevelsConvDraw(ren, w, h, gelevelsConvPercent());
		SDL_Delay(16);
	}

	SDL_WaitThread(thread, NULL);

	gelevelsConvDraw(ren, w, h, 100);
	SDL_Delay(150);

	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
}

void gelevelsAutoConvert(void)
{
	char base[FS_MAXPATH + 1];
	char path[FS_MAXPATH + 1];
	size_t romlen = 0;
	u8 *rom;
	bool haveLevels;

	snprintf(base, sizeof(base), "%s", fsFullPath("$B"));
	snprintf(path, sizeof(path), "%s/mods/goldeneye/modconfig.txt", base);
	haveLevels = fsFileSize(path) > 0;

	snprintf(g_Job.outdir, sizeof(g_Job.outdir), "%s/mods/goldeneye", base);
	snprintf(g_Job.tmpdir, sizeof(g_Job.tmpdir), "%s/mods/.geconvert-tmp", base);

	// already made by this converter: nothing to do, and no ROM needed
	if (haveLevels && gelevelsConvStampMatches(g_Job.outdir)) {
		return;
	}

	// the ROM sits beside Perfect Dark's: in the base folder (data/ by default),
	// else next to the exe, else in the working folder
	rom = gelevelsConvFindRomIn(base, &romlen);

	if (!rom) {
		snprintf(path, sizeof(path), "%s", fsFullPath("$E"));
		rom = gelevelsConvFindRomIn(path, &romlen);
	}

	if (!rom) {
		rom = gelevelsConvFindRomIn(".", &romlen);
	}

	if (!rom) {
		if (!haveLevels) {
			sysLogPrintf(LOG_NOTE, "gelevels: no GoldenEye 007 (USA) ROM beside the game, so no GoldenEye levels");
		}

		return;
	}

	sysLogPrintf(LOG_NOTE, "gelevels: converting GoldenEye's levels (first run)");

	g_Job.rom = rom;
	g_Job.romlen = romlen;
	g_Job.done = 0;
	g_Job.err[0] = '\0';

	gelevelsConvRunWithBar();

	free(rom);
	g_Job.rom = NULL;

	if (g_Job.ok) {
		sysLogPrintf(LOG_NOTE, "gelevels: GoldenEye's levels written to %s", g_Job.outdir);
	} else {
		sysLogPrintf(LOG_ERROR, "gelevels: conversion failed: %s", g_Job.err);
	}
}
