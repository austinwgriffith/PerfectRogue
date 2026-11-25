/**
 * HD text: a texture pack's pictures of the game's font glyphs (hdtext.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO_FAILURE_CALLBACKS
#include "external/stb_image.h"

#include <PR/ultratypes.h>
#include "hdtext.h"
#include "platform.h"
#include "config.h"
#include "system.h"
void gfx_texture_cache_clear(void); // port/fast3d/gfx_pc.cpp

#define HDTEXT_NUM_FONTS 5
#define HDTEXT_FONT_CHARS 135
#define HDTEXT_DIR "texture-packs"

static const char *g_FontDirs[HDTEXT_NUM_FONTS] = {
	"fonthandelgothicsm",
	"fonthandelgothicmd",
	"fonthandelgothicxs",
	"fonthandelgothiclg",
	"fontnumeric",
};

static s32 g_Enabled = 1;
static char g_PackName[128];

static s32 g_Scanned;
static char g_FontPath[256]; // the folder that holds the font folders, "" for none
static s32 g_BottomUp;       // images are already in the game's row order

struct glyphimage {
	u8 state; // 0 not tried, 1 loaded, 2 none
	u8 *rgba;
	s32 width;
	s32 height;
};

static struct glyphimage g_Glyphs[2][HDTEXT_NUM_FONTS][HDTEXT_FONT_CHARS];

static int isDir(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int isFile(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/** Looks for a folder holding the font folders, in dir and one level below. */
static int findFontRoot(const char *dir, char *out, size_t outsize)
{
	char probe[512];
	DIR *d;
	struct dirent *e;

	snprintf(probe, sizeof(probe), "%s/%s", dir, g_FontDirs[0]);

	if (isDir(probe)) {
		snprintf(out, outsize, "%s", dir);
		return 1;
	}

	d = opendir(dir);

	if (!d) {
		return 0;
	}

	while ((e = readdir(d)) != NULL) {
		char sub[512];

		if (e->d_name[0] == '.') {
			continue;
		}

		snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name);
		snprintf(probe, sizeof(probe), "%s/%s", sub, g_FontDirs[0]);

		if (isDir(sub) && isDir(probe)) {
			snprintf(out, outsize, "%s", sub);
			closedir(d);
			return 1;
		}
	}

	closedir(d);
	return 0;
}

static void scan(void)
{
	char packdir[512];
	char root[512];
	char marker[600];

	g_Scanned = 1;
	g_FontPath[0] = '\0';

	if (!isDir(HDTEXT_DIR)) {
		return;
	}

	if (g_PackName[0]) {
		snprintf(packdir, sizeof(packdir), "%s/%s", HDTEXT_DIR, g_PackName);

		if (!findFontRoot(packdir, root, sizeof(root))) {
			sysLogPrintf(LOG_WARNING, "hdtext: no font folders under %s", packdir);
			return;
		}
	} else {
		DIR *d = opendir(HDTEXT_DIR);
		struct dirent *e;
		int found = 0;

		if (!d) {
			return;
		}

		while (!found && (e = readdir(d)) != NULL) {
			if (e->d_name[0] == '.') {
				continue;
			}

			snprintf(packdir, sizeof(packdir), "%s/%s", HDTEXT_DIR, e->d_name);
			found = isDir(packdir) && findFontRoot(packdir, root, sizeof(root));
		}

		closedir(d);

		if (!found) {
			return;
		}
	}

	snprintf(g_FontPath, sizeof(g_FontPath), "%s", root);

	// A pack saying its images are in the N64's row order has a bottomup.txt
	// at its top (or beside the fonts); otherwise they are written the right
	// way up and are turned over on load.
	g_BottomUp = 0;
	snprintf(marker, sizeof(marker), "%s/bottomup.txt", root);

	if (isFile(marker)) {
		g_BottomUp = 1;
	} else {
		snprintf(marker, sizeof(marker), "%s/%s/bottomup.txt", HDTEXT_DIR, g_PackName[0] ? g_PackName : "");

		if (g_PackName[0] && isFile(marker)) {
			g_BottomUp = 1;
		} else if (!g_PackName[0]) {
			// the pack folder is the parent of root, or root itself
			char parent[512];
			char *slash;

			snprintf(parent, sizeof(parent), "%s", root);
			slash = strrchr(parent, '/');

			if (slash) {
				*slash = '\0';
				snprintf(marker, sizeof(marker), "%s/bottomup.txt", parent);
				g_BottomUp = isFile(marker);
			}
		}
	}

	sysLogPrintf(LOG_NOTE, "hdtext: font pictures from %s (%s row order)", root, g_BottomUp ? "N64" : "top-down");
}

s32 hdtextGetEnabled(void)
{
	return g_Enabled;
}

void hdtextSetEnabled(s32 enabled)
{
	enabled = enabled ? 1 : 0;

	if (enabled != g_Enabled) {
		g_Enabled = enabled;
		gfx_texture_cache_clear(); // the glyphs already uploaded are the other kind
	}
}

s32 hdtextHaveGlyphs(void)
{
	if (!g_Enabled) {
		return 0;
	}

	if (!g_Scanned) {
		scan();
	}

	return g_FontPath[0] != '\0';
}

static u8 *loadFile(const char *path, s32 *w, s32 *h)
{
	int comp;
	u8 *px = stbi_load(path, w, h, &comp, 4);

	if (px && !g_BottomUp) {
		const size_t row = (size_t)*w * 4;
		u8 *tmp = malloc(row);

		if (tmp) {
			s32 y;

			for (y = 0; y < *h / 2; y++) {
				u8 *a = px + y * row;
				u8 *b = px + (*h - 1 - y) * row;

				memcpy(tmp, a, row);
				memcpy(a, b, row);
				memcpy(b, tmp, row);
			}

			free(tmp);
		}
	}

	return px;
}

/** The rows of an image that hold ink: [*top, *bottom). */
static void inkRows(const u8 *rgba, s32 w, s32 h, s32 *top, s32 *bottom)
{
	s32 y, x;

	*top = h;
	*bottom = 0;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			if (rgba[(y * w + x) * 4 + 3] > 16) {
				if (y < *top) *top = y;
				if (y + 1 > *bottom) *bottom = y + 1;
				break;
			}
		}
	}
}

static u8 *glyphImage(s32 outline, s32 font, s32 index, s32 *w, s32 *h);

/**
 * The pack's lowercase z is drawn a row taller than its x, so it hangs below
 * the line (and stands above the other small letters). Squeeze its ink into
 * x's rows. Done in the order the image is stored, which is row 0 first.
 */
static void fitZToX(struct glyphimage *z, s32 outline, s32 font)
{
	s32 xw, xh, zt, zb, xt, xb, y, x, dh, sh;
	u8 *xi = glyphImage(outline, font, 0x57, &xw, &xh); // 'x'
	u8 *out;
	s32 scale;

	if (!xi || xw != z->width || xh != z->height) {
		return;
	}

	scale = z->width / 16;
	inkRows(z->rgba, z->width, z->height, &zt, &zb);
	inkRows(xi, xw, xh, &xt, &xb);
	sh = zb - zt;
	dh = xb - xt;

	if (scale < 2 || dh <= 0 || sh - dh < scale / 2) {
		return;
	}

	out = calloc((size_t)z->width * z->height, 4);

	if (!out) {
		return;
	}

	for (y = 0; y < dh; y++) {
		const f32 srcy = zt + (y + 0.5f) * sh / dh - 0.5f;
		s32 y0 = (s32)srcy;
		const f32 fr = srcy - y0;
		s32 y1 = y0 + 1;

		if (y0 < zt) { y0 = zt; }
		if (y1 > zb - 1) { y1 = zb - 1; }

		for (x = 0; x < z->width * 4; x++) {
			const f32 a = z->rgba[y0 * z->width * 4 + x];
			const f32 b = z->rgba[y1 * z->width * 4 + x];

			out[(xt + y) * z->width * 4 + x] = (u8)(a + (b - a) * fr + 0.5f);
		}
	}

	free(z->rgba);
	z->rgba = out;
}

static u8 *glyphImage(s32 outline, s32 font, s32 index, s32 *w, s32 *h)
{
	struct glyphimage *g = &g_Glyphs[outline][font][index];
	char path[640];
	static const char *exts[] = { "png", "PNG", "jpg" };
	s32 i;

	if (g->state == 0) {
		g->state = 2;

		for (i = 0; i < 3 && g->state == 2; i++) {
			char name[8];
			s32 upper;

			for (upper = 0; upper < 2 && g->state == 2; upper++) {
				snprintf(name, sizeof(name), upper ? "%02X" : "%02x", index);

				if (outline) {
					snprintf(path, sizeof(path), "%s/%s/outlines/%s.%s", g_FontPath, g_FontDirs[font], name, exts[i]);
				} else {
					snprintf(path, sizeof(path), "%s/%s/%s.%s", g_FontPath, g_FontDirs[font], name, exts[i]);
				}

				if (isFile(path)) {
					g->rgba = loadFile(path, &g->width, &g->height);

					if (g->rgba) {
						g->state = 1;
					}
				}
			}
		}

		if (g->state == 1 && index == 0x59 && font != 4) { // 'z'
			fitZToX(g, outline, font);
		}
	}

	if (g->state != 1) {
		return NULL;
	}

	*w = g->width;
	*h = g->height;

	return g->rgba;
}

u8 *hdtextLoadGlyph(u32 glyph, s32 *outWidth, s32 *outHeight)
{
	s32 outline;
	s32 font;
	s32 index;
	if (!(glyph & HDTEXT_GLYPH_SET) || !hdtextHaveGlyphs()) {
		return NULL;
	}

	outline = HDTEXT_GLYPH_IS_OUTLINE(glyph) ? 1 : 0;
	font = HDTEXT_GLYPH_FONT(glyph);
	index = HDTEXT_GLYPH_INDEX(glyph);

	if (font >= HDTEXT_NUM_FONTS || index >= HDTEXT_FONT_CHARS) {
		return NULL;
	}

	return glyphImage(outline, font, index, outWidth, outHeight);
}

PD_CONSTRUCTOR static void hdtextConfigInit(void)
{
	configRegisterInt("Mod.HDText", &g_Enabled, 0, 1);
	configRegisterString("Mod.HDTextPack", g_PackName, sizeof(g_PackName));
}
