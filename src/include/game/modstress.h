#ifndef IN_GAME_MODSTRESS_H
#define IN_GAME_MODSTRESS_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

// Guard-cap stress test (modstress.c), run with --selftest-stress N
extern s32 g_StressGuards;
extern s32 g_StressSeconds;
extern s32 g_StressStage;
void modStressTestTick(void);

#endif
