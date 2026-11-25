#ifndef _IN_HDTEXT_H
#define _IN_HDTEXT_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * HD text: a texture pack's pictures of the game's font glyphs.
 *
 * The game draws text one glyph at a time from 16 texel wide bitmaps. A pack
 * such as PD Plus HD ships a larger picture for each, one file per character
 * (named by its index in hex) in a folder named after the font:
 *
 *   texture-packs/<pack>/.../fonthandelgothicsm/00.png
 *   texture-packs/<pack>/.../fonthandelgothicsm/outlines/00.png
 *
 * The renderer is told which glyph it is about to draw (gDPSetFontGlyphEXT,
 * from textSetFontGlyph() in game_1531a0.c) and swaps the picture in on its way
 * to the GPU. The game's layout, kerning and widths are untouched; only the
 * ink changes. A glyph the pack has no file for is drawn the game's own way.
 *
 * Settings (pd.ini, [Mod]): HDText=1 turns it on; HDTextPack names a folder
 * under texture-packs/ (empty: the first one that has font folders).
 */

#define HDTEXT_GLYPH_SET            0x80000000u
#define HDTEXT_GLYPH_IS_OUTLINE(g)  (((g) >> 24) & 0x7f)
#define HDTEXT_GLYPH_FONT(g)        (((g) >> 16) & 0xff)
#define HDTEXT_GLYPH_INDEX(g)       ((g) & 0xffff)

/** The "HD Text" option (Video menu). Turning it off or on redraws the text. */
s32 hdtextGetEnabled(void);
void hdtextSetEnabled(s32 enabled);

/** Whether a pack with font pictures is selected and the setting is on. */
s32 hdtextHaveGlyphs(void);

/**
 * The picture for one glyph named by gDPSetFontGlyphEXT, as RGBA32 in the
 * game's row order, or NULL when the pack has none. Owned by hdtext: do not
 * free it.
 */
u8 *hdtextLoadGlyph(u32 glyph, s32 *outWidth, s32 *outHeight);

#ifdef __cplusplus
}
#endif

#endif
