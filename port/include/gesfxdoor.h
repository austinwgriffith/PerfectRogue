#ifndef IN_GESFXDOOR_H
#define IN_GESFXDOOR_H

#include "types.h"

enum {
	GESFX_DOOR_OPENING,
	GESFX_DOOR_CLOSING,
	GESFX_DOOR_OPENED,
	GESFX_DOOR_CLOSED,
};

bool geSfxDoor(s32 moment, s32 soundtype, struct prop *prop);

#endif
