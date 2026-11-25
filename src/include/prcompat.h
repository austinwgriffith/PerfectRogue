#ifndef _IN_PRCOMPAT_H
#define _IN_PRCOMPAT_H

#include <ultra64.h>
#include "prstubs.h"

/**
 * Perfect Random on Carrington Evolved: the seams left by porting the mode off
 * the Dabs fork.
 *
 * Perfect Random was written on the Dabs fork, which adds GoldenEye (GE Plus,
 * GoldenEye X, the XBLA release), netplay, Ghost Trials, a mod loader and many
 * engine limits that CE does not have. This build carries the mode itself and
 * leaves those out. Everything that still talks to them goes through this
 * header and port/src/prstubs.c, so bringing GoldenEye back later is a matter
 * of replacing the stubs with the real modules and turning PR_GOLDENEYE on.
 */

// GoldenEye levels, guns and characters (Dabs: gexplus, gebean, geguns, ...)
#define PR_GOLDENEYE 0

// Engine values Dabs defines and CE does not. Values match Dabs so that a
// seed or a pd.ini written there means the same thing here.
#ifndef MPFEATURE_NEVER
#define MPFEATURE_NEVER 0x7f // an mpweapon row that is never offered
#endif

#ifndef MP_NUM_STOCK_ARENAS
#define MP_NUM_STOCK_ARENAS 17
#endif

#ifndef STAGE_MAX_ID
#define STAGE_MAX_ID 0xff
#endif

#ifndef MAX_ROOMPROPS
#define MAX_ROOMPROPS 1024 // the size of a roomGetProps() list
#endif

// Dabs gameplay options carried in g_ModOptions. Their settings exist in
// pd.ini (so the file round-trips), but CE has no code behind them.
#ifndef THIRDPERSON_CAMDIST
#define THIRDPERSON_CAMDIST      200.0f
#define THIRDPERSON_CAMCLEARANCE 30.0f
#define THIRDPERSON_CAMSIDE      0.0f
#define THIRDPERSON_CAMFWD       0.0f
#define THIRDPERSON_CAMHEIGHT    0.0f
#define THIRDPERSON_CAMFADE      40.0f
#endif

#ifndef JUMP_IMPULSE
#define JUMP_IMPULSE   5.75f
#define JUMP_APEX      60.0f
#define JUMPHEIGHT_MIN 1
#define JUMPHEIGHT_MAX 5
#endif

#ifndef SPAWNWEAPON_OFF
#define SPAWNWEAPON_OFF    0
#define SPAWNWEAPON_FIRST  1
#define SPAWNWEAPON_RANDOM 2
#endif


// Netplay (Dabs: port/src/net/). There is no online play here: every
// "is this a client/server" question answers no.
#ifndef NETMODE_NONE
#define NETMODE_NONE   0
#define NETMODE_SERVER 1
#define NETMODE_CLIENT 2
#endif
extern s32 g_NetMode; // always NETMODE_NONE (prstubs.c)
#ifndef NET_CLIENT
#define NET_CLIENT 0
#endif

// GoldenEye's guns stand on Perfect Dark weapons (Dabs: weaponHost()); here
// every weapon is its own
#ifndef weaponHost
#define weaponHost(weaponnum) (weaponnum)
#endif

// One past the last real weapon number (Dabs adds GoldenEye's after it)
#ifndef NUM_WEAPONS
#define NUM_WEAPONS (WEAPON_TARGETLOCATOR + 1)
#endif

#ifndef WEAPON_IS_GE
#define WEAPON_IS_GE(weaponnum) (false)
#endif

#endif
