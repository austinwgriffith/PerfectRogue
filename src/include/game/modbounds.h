#ifndef IN_GAME_MODBOUNDS_H
#define IN_GAME_MODBOUNDS_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

// Bounds-fix regression test (modbounds.c), run with --selftest-bounds
extern bool g_BoundsTest;
void modBoundsTestTick(void);

#endif
