#ifndef _IN_GAME_PORTALCONV_H
#define _IN_GAME_PORTALCONV_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

s32 portalConvertCoordinates(s32 portalnum, s32 *start, struct portalthing2 *things);
s32 portalConvertCoordinatesOffset(s32 portalnum, s32 *start, struct portalthing2 *things, f32 offset);

#endif
