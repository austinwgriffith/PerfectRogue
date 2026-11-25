/*
 * modlevelgen.h: Perfect Random level generator, layout stage.
 *
 * Builds, from a seed, an abstract level: rooms, corridor areas, portals
 * (openings between areas), doors, the area graph, navigation pads with
 * waypoint links, and guard / start / objective pads.
 *
 * This stage uses no engine code and integer maths only, so the same seed
 * gives byte-identical output on every machine. Later stages (geometry,
 * collision, lighting, file output) read an LgLayout.
 *
 * Coordinates: the grid is cells of LG_CELL world units. cells[z][x].
 * World x of cell edge cx = originx + cx * LG_CELL (same for z). Floor is y 0.
 * Pad angles: degrees, 0 faces +z, 90 faces +x (verify against PD's pad look).
 */
#ifndef _IN_GAME_MODLEVELGEN_H
#define _IN_GAME_MODLEVELGEN_H

#include <stdint.h>

#define MODLEVELGEN_VERSION 3   /* 3: hub with terraces and offshoots (2026-11-14) */

#define LG_CELL        100   /* world units per grid cell */
#define LG_GRIDMAX     112
#define LG_MAXROOMS    48
#define LG_MAXAREAS     448
#define LG_MAXPORTALS   1200
#define LG_MAXPADS     2048
#define LG_MAXNEIGH    40   /* the hub's floor gathers many pads */
#define LG_MAXOBJ      3
#define LG_MAXOBST     400
#define LG_MEZZ_UNDER  220   /* an undercroft's ceiling over its floor */
#define LG_MEZZ_SLAB   20    /* the mezzanine floor's thickness */
#define LG_MEZZ_HEAD   280   /* headroom over the mezzanine floor */
#define LG_PILLAR_HALF 30    /* a pillar is this far from its cell's centre to each face */

enum { LG_SIZE_SMALL, LG_SIZE_MEDIUM, LG_SIZE_LARGE, LG_SIZE_COUNT };
enum { LG_AREA_ROOM, LG_AREA_CORRIDOR };
enum { LG_ROOM_OFFICE, LG_ROOM_HALL, LG_ROOM_STORAGE, LG_ROOM_LAB, LG_ROOM_TYPECOUNT };
/* A level's two themes, chosen from the seed and the size: its layout (the
 * shape and flow of the space; each has a generator in modlevelgen.c) and its
 * aesthetic (how it looks: the textures, lighting mood and fittings, tables
 * in modlevelgenfile.c). A layout lists the aesthetics that suit it. */
/* (2026-11-16: the warehouse was scrapped; the rooftops took its number) */
/* (LG_LAYOUT_COUNT: the layouts a seed can pick. After them the designed
 * levels, "Claude Missions", 2026-11-16: built by hand, the same every time,
 * never picked by a seed; LG_LAYOUT_ALL counts both.) */
enum { LG_LAYOUT_HUB, LG_LAYOUT_LAB, LG_LAYOUT_STREET, LG_LAYOUT_RAVINE, LG_LAYOUT_ROOFTOP, LG_LAYOUT_VILLA,
	LG_LAYOUT_DOCKS, LG_LAYOUT_BACKWOODS,   /* (2026-11-17: Pier 17's and Hollow Creek's, random) */
	LG_LAYOUT_PENTHOUSE, LG_LAYOUT_SITE,    /* (2026-11-17: a skyscraper's penthouse; a tower going up. Both with lifts) */
	LG_LAYOUT_PLAZA,                        /* (2026-11-18: a market town round its fountain square) */
	LG_LAYOUT_MALL,                         /* (2026-11-19: a two-floor 80s mall round a plus of void) */
	LG_LAYOUT_PIPEWORKS,                    /* (2026-11-19: a hall round a sump of sludge, pipes and catwalks over it) */
	LG_LAYOUT_COUNT,
	LG_LAYOUT_PIER = LG_LAYOUT_COUNT, LG_LAYOUT_TRAIL, LG_LAYOUT_STATION, LG_LAYOUT_ALL };
#define LG_LAYOUT_DESIGNED(l) ((l) == LG_LAYOUT_PIER || (l) == LG_LAYOUT_TRAIL)
/* built from the villa's pieces, flat-topped, the woods or the containers round them */
#define LG_LAYOUT_FLATBUILT(l) ((l) == LG_LAYOUT_DOCKS || (l) == LG_LAYOUT_BACKWOODS || (l) == LG_LAYOUT_PLAZA || LG_LAYOUT_DESIGNED(l))
#define LG_LAYOUT_HARBOURLIKE(l) ((l) == LG_LAYOUT_DOCKS || (l) == LG_LAYOUT_PIER)
/* a tower's storeys stacked (lgTryHighrise): the rooftops' tower machinery */
#define LG_LAYOUT_HIGHRISE(l) ((l) == LG_LAYOUT_PENTHOUSE || (l) == LG_LAYOUT_SITE || (l) == LG_LAYOUT_MALL)   /* (the mall: its floors stacked the same way) */
#define LG_LAYOUT_SKYLIKE(l) ((l) == LG_LAYOUT_ROOFTOP || LG_LAYOUT_HIGHRISE(l))
#define LG_LAYOUT_WOODSLIKE(l) ((l) == LG_LAYOUT_BACKWOODS || (l) == LG_LAYOUT_TRAIL)
enum { LG_AESTH_PLACEHOLDER, LG_AESTH_CHICAGO, LG_AESTH_CANYON, LG_AESTH_CITY, LG_AESTH_ROOFTOP, LG_AESTH_FACILITY, LG_AESTH_LABS, LG_AESTH_VILLA, LG_AESTH_COUNT,
	LG_AESTH_HARBOUR = LG_AESTH_COUNT, LG_AESTH_WOODLAND, LG_AESTH_PENTHOUSE, LG_AESTH_SITE, LG_AESTH_STATION, LG_AESTH_MARKET, LG_AESTH_MALL, LG_AESTH_PIPEWORKS, LG_AESTH_ALL };

enum { LG_STN_DEPOT, LG_STN_OFFICE, LG_STN_PLATFORM, LG_STN_SERVICE, LG_STN_ACCESS,
	LG_STN_CONCOURSE, LG_STN_CAR, LG_STN_VESTIBULE };
#define LG_STN_PLATFORM_Y 320
#define LG_STN_CONCOURSE_Y 620
#define LG_STN_TRAIN_TOP 600

typedef struct {
	uint8_t layout;         /* LG_LAYOUT_* */
	uint8_t aesthetic;      /* LG_AESTH_* */
} LgTheme;

enum { LG_PAD_NAV, LG_PAD_GUARD, LG_PAD_START, LG_PAD_OBJECTIVE };

#define LG_AREAFLAG_START     0x01
#define LG_AREAFLAG_EXTRACT   0x02
#define LG_AREAFLAG_OBJECTIVE 0x04
#define LG_AREAFLAG_HUB       0x08   /* part of the hub (its floor or a terrace) */
#define LG_AREAFLAG_TERRACE   0x10   /* a side room's terrace strip */
#define LG_AREAFLAG_UNDER     0x20   /* low-ceilinged part of a room, a mezzanine over it */
#define LG_AREAFLAG_MEZZ      0x40   /* an upper floor over part of a room (layer 1) */
#define LG_AREAFLAG_BAY       0x80   /* a mezzanine's ramp or the bay in front of it */

/* open[z][x] bits: the edge from this cell to its +x / +z neighbour is open */
#define LG_OPEN_XP 0x01
#define LG_OPEN_ZP 0x02

typedef struct {
	int16_t x, z, w, d;   /* rectangle in cells */
	uint8_t kind;         /* LG_AREA_* */
	uint8_t roomtype;     /* LG_ROOM_* (rooms only) */
	uint8_t theme;        /* texture theme index (tables come later) */
	uint8_t flags;        /* LG_AREAFLAG_* */
	int16_t room;         /* room index, -1 for corridors */
	int16_t ceil;         /* ceiling height, world units */
	int16_t dist;         /* hops from the start area, -1 unreached */
	int16_t centrepad;    /* nav pad at the area centre */
	int16_t floor;        /* floor height: a raised room's or platform's, a ramp's top end */
	int16_t floorlo;      /* a ramp's low end (its other areas: = floor) */
	uint8_t ramp;         /* LG_RAMP_*: a sloped floor rising from 0 to floor that way */
	uint8_t layer;        /* 0: ground grid (cells), 1: upper storey grid (cells2), over layer 0 cells, 2: the storey over
	                       * that (cells3, over layer 1 cells; 2026-11-16, the rooftops' towers) */
	uint8_t open;         /* outdoors: no ceiling, the sky over it (walls stand to ceil) */
	uint8_t special;      /* LG_SPECIAL_*: 0 for an ordinary area */
} LgArea;

/* A window (2026-11-14): a corridor-kind area through a wall's thickness whose
 * floor is the sill and whose ceiling is the head, with a pane of glass
 * across its middle. Its openings are ledges nobody crosses (LgPortal.window):
 * it has no pads and is not counted as reachable.
 * A balcony: a layer 1 room area, outdoors, over a doorway passage, level with
 * its building's mezzanine, a ledge on the street side. */
#define LG_SPECIAL_WINDOW   1
#define LG_SPECIAL_BALCONY  2
#define LG_SPECIAL_BRIDGE   3   /* a layer 1 area across a street between two balconies (its room is the street's), or across a ravine */
/* A layer's cell grid and its open edges: 0 the ground, 1 and 2 the storeys over it */
#define LG_MAXLAYERS 3
#define LG_CELLS(lg, l) ((l) == 2 ? (lg)->cells3 : (l) == 1 ? (lg)->cells2 : (lg)->cells)
#define LG_OPENS(lg, l) ((l) == 2 ? (lg)->open3 : (l) == 1 ? (lg)->open2 : (lg)->open)
#define LG_SPECIAL_ROOF     6   /* a villa building's roof slope: layer 1, sloped, walked on (2026-11-15) */
#define LG_SPECIAL_RACK     5   /* a raised block nobody walks on: a villa's stone hedge (2026-11-15) */
/* A glass floor over a drop to the death below (the rooftops, 2026-11-16):
 * walked on at floor, the glass breakable panes; under it the pit, open down
 * to its bottom at floorlo (a kill floor, GEOFLAG_DIE). Nobody's pads are on
 * it: the guards never walk on it. */
#define LG_SPECIAL_GLASS    7
#define LG_SPECIAL_FIREESCAPE 8
/* a big pipe walked along its top (the pipeworks', 2026-11-19): layer 1 over
 * the sludge, drawn round, its walked strip narrower than its cells */
#define LG_SPECIAL_PIPE     9
/* nobody walks there: no pads, cover, guards, objectives or tour stops */
#define LG_NOWALK(a) ((a)->special == LG_SPECIAL_PIT || (a)->special == LG_SPECIAL_RACK || (a)->special == LG_SPECIAL_GLASS)
#define LG_SPECIAL_PIT      4   /* a ravine's floor: falling onto it kills (GEOFLAG_DIE); nobody walks there */
#define LG_WIN_SILL  70    /* a window's sill above the floor */
#define LG_WIN_HEAD  210   /* and its head: under an undercroft's ceiling */

/* A ramp's floor rises from 0 at one side of its rectangle to floor at the
 * opposite side: the side it rises towards */
enum { LG_RAMP_NONE, LG_RAMP_XP, LG_RAMP_XN, LG_RAMP_ZP, LG_RAMP_ZN };

typedef struct {
	int16_t x0, z0, x1, z1; /* opening, in cell-edge coords (x0 == x1: runs along z) */
	int16_t a, b;           /* area on the low side, area on the high side */
	uint8_t doorway;        /* joins a room and a corridor */
	uint8_t door;           /* a door prop goes here (LG_DOOR_LIFT + the stop: a lift's, 2026-11-17) */
	uint8_t drop;           /* the floors differ across it: a ledge, one side over the other */
	uint8_t norail;         /* a ledge left open: a drop-down from a mezzanine */
	uint8_t window;         /* a window's opening: seen and shot through, never walked */
	int16_t pada, padb;     /* nav pads either side */
} LgPortal;

typedef struct {
	int32_t x, y, z;
	int16_t angle;
	int16_t area;
	uint8_t kind;           /* LG_PAD_* */
	uint8_t numneigh;       /* nav pads only */
	int16_t neigh[LG_MAXNEIGH];
} LgPad;

/* Something standing on a room's floor: a full-height pillar in the middle of
 * a cell, or low cover (a crate, a low wall, a balcony's parapet) */
typedef struct {
	int16_t cx, cz;         /* the cell under its centre */
	int16_t area;
	int16_t halfx, halfz;   /* half its size, world units */
	int16_t height;         /* over the floor; 0 = floor to ceiling (a pillar) */
	int32_t x, z;           /* its centre, world */
	uint8_t crate;          /* 1: one of the game's own crate props stands here (the file writers draw no faces) */
	uint8_t model;          /* crate: which of LG_CRATE_MODELS */
	uint8_t turn;           /* crate: quarter turns about its centre */
	uint8_t rail;           /* 1: a railing along a ledge: posts and rails, walked into but seen and shot through */
	int16_t scale;          /* crate: the setup's extrascale, 256 = 1.0 */
} LgObstacle;

/* The cover crates are the game's A51 crate models (MODEL_A51_CRATE1..3): a
 * cube LG_CRATE_SIZE a side about its origin at scale 1, scaled 0.9 / 1.0 /
 * 1.1 here, so a crate has halfx = halfz = height / 2 = 45, 50 or 55 */
#define LG_RAIL_HEIGHT  100   /* a railing's top, over the floor */
#define LG_CRATE_MODELS 3
#define LG_CRATE_SIZE   100

/* The props an obstacle can be (LgObstacle.crate set, .model the index): the
 * three crates, then the cars parked along a street (2026-11-14). A car is not
 * a cube and its model's origin is not its middle, so each prop has its
 * measurements here (world units at the scale given, measured in the game
 * from the model's bounding box x its model scale): half its width, half its
 * length (along the way it faces), its height, how far its origin is above
 * its underside, and how far its middle is ahead of its origin. The
 * obstacle's x, z are the middle of its box. */
#define LG_PROP_TAXI    3
#define LG_PROP_POLICE  4
/* the rest of the game's props the layouts are dressed with (2026-11-16,
 * claude/levelgen-props.md): measured in the game (LGMEASURE=1 on a tour) */
#define LG_PROP_BORGCRATE   5
#define LG_PROP_BARREL      6
#define LG_PROP_HOVERCRATE  7
#define LG_PROP_TROLLEY     8
#define LG_PROP_DDDESK      9
#define LG_PROP_REDSOFA     10
#define LG_PROP_STONEDESK   11
#define LG_PROP_A51TABLE    12
#define LG_PROP_LABTABLE    13
#define LG_PROP_CICABINET   14
#define LG_PROP_CITABLE     15
#define LG_PROP_PLANTRUBBER 16
#define LG_PROP_PLANTSPIDER 17
#define LG_PROP_DIVIDER     18
#define LG_PROP_DUMPSTER    19
#define LG_PROP_LIMO        20
#define LG_PROP_HOVCAR      21
#define LG_PROP_HOVCAB      22
#define LG_PROP_HOVBIKE     23
#define LG_PROP_JUMPSHIP    24
#define LG_PROP_SAUCER      25
#define LG_PROP_SHUTTLE     26
#define LG_PROP_ROCK1       27
#define LG_PROP_ROCK2       28
#define LG_PROP_DISH        29
#define LG_PROP_BED         30
#define LG_PROP_LAMP        31
#define LG_PROP_VILLACHAIR  32
#define LG_PROP_ARMCHAIR    33
#define LG_PROP_VILLABOT    34
#define LG_PROP_ROOFVENT    35
#define LG_PROP_G5CHAIR     36
#define LG_PROP_REDARM      37
#define LG_PROP_TOILET      38   /* (2026-11-19: the mall's washroom stalls) */
#define LG_PROP_MODELS      39

typedef struct {
	uint16_t model;        /* the game's model number */
	int16_t halfw, halfl;
	int16_t height;
	int16_t originy;
	int16_t ahead;
	int16_t scale;         /* the setup's extrascale (256 = 1); 0: by the obstacle's own */
	uint32_t flags;        /* the setup record's object flags */
} LgPropDef;

extern const LgPropDef g_LgProps[LG_PROP_MODELS];

#define LG_COVER_HEIGHT 110 /* low cover: crouch behind it, shoot over it */

typedef struct {
	uint32_t seed;
	uint8_t size;           /* LG_SIZE_* */
} LgParams;

#define LG_DOOR_LIFT 2
#define LG_LIFT_CAR_HEIGHT 250

/* A lift (2026-11-17): a platform riding up and down a shaft (a bay area open
 * from its floor to the top storey's ceiling), stopping at each storey's floor
 * level in turn and waiting there (propobj.c, liftLevelgenWait) */
#define LG_MAXLIFTS 4
typedef struct {
	int16_t area;           /* the shaft */
	int16_t side;           /* the side it is boarded from: 0 -x, 1 +x, 2 -z, 3 +z */
	int16_t nstops;
	int16_t stop[3];        /* the floor heights it stops at, lowest first */
} LgLift;

/* A ladder up a face (the pipeworks', 2026-11-19): the face on the cell edge
 * line x = line (alongz) or z = line, a cell wide at cell `at` along it; the
 * climber stands on side `facing` (0 -x, 1 +x, 2 -z, 3 +z) in area low, at
 * floor lo, and climbs to area high at hi. Its tiles are GEOFLAG_LADDER. */
#define LG_MAXLADDERS 8
typedef struct {
	uint8_t alongz, facing;
	int16_t line, at;
	int16_t low, high;
	int16_t lo, hi;
} LgLadder;

typedef struct LgLayoutTag {
	uint32_t version;
	uint32_t seed;
	uint8_t size;
	uint8_t attempt;        /* which retry produced this layout */
	LgTheme theme;          /* its layout and aesthetic, from the seed and size */
	int16_t gw, gd;
	int32_t originx, originz;
	int16_t numrooms, numareas, numportals, numpads;
	int16_t startarea, extractarea;
	int16_t numobj;
	int16_t objareas[LG_MAXOBJ];
	int16_t maxdist;
	int16_t cells[LG_GRIDMAX][LG_GRIDMAX]; /* area + 1, 0 = solid */
	int16_t cells2[LG_GRIDMAX][LG_GRIDMAX]; /* upper storey areas (layer 1) over the ground cells: area + 1, 0 = none */
	uint8_t open[LG_GRIDMAX][LG_GRIDMAX];
	uint8_t open2[LG_GRIDMAX][LG_GRIDMAX]; /* the same for edges of layer 1 areas (to their layer 1 or ground neighbours) */
	int16_t cells3[LG_GRIDMAX][LG_GRIDMAX]; /* the third storey (layer 2) over layer 1 cells */
	uint8_t open3[LG_GRIDMAX][LG_GRIDMAX]; /* edges of layer 2 areas */
	LgArea areas[LG_MAXAREAS];
	LgPortal portals[LG_MAXPORTALS];
	LgPad pads[LG_MAXPADS];
	int16_t numobst;
	LgObstacle obst[LG_MAXOBST];
	int16_t numlifts;
	LgLift lifts[LG_MAXLIFTS];
	int16_t numladders;
	LgLadder ladders[LG_MAXLADDERS];
} LgLayout;

/* The floor's height in area ai at world (x, z): its platform height, or on
 * a ramp the height of the slope there (clamped to the area) */
int32_t modLevelGenFloorAt(const LgLayout *lg, int ai, int32_t x, int32_t z);

/* Builds a layout. Returns 1 on success; on failure writes a reason to err. */
int modLevelGenBuild(LgLayout *lg, const LgParams *params, char *err, int errlen);

/* The themes a seed and size give (what modLevelGenBuild puts in the layout),
 * without building anything */
LgTheme modLevelGenPickTheme(uint32_t seed, int size);

/* Fixes the layout and / or the aesthetic of every level built from now on
 * (LG_LAYOUT_*, LG_AESTH_*; -1: from the seed again). For tests and the menu. */
void modLevelGenForceTheme(int layout, int aesthetic);

/* Names: a layout's ("Hub"), an aesthetic's, and a level's as the player sees
 * it: "(PR) <Layout>, <Aesthetic>". The last returns buf. */
const char *modLevelGenLayoutName(int layout);
const char *modLevelGenAestheticName(int aesthetic);
const char *modLevelGenLevelName(LgTheme theme, char *buf, int buflen);

/* Checks every invariant later stages rely on. Returns 1 if all hold. */
int modLevelGenValidate(const LgLayout *lg, char *err, int errlen);

/* Writes a text map of the layout. Returns characters written. */
int modLevelGenDumpAscii(const LgLayout *lg, char *buf, int buflen);

/* Runs the layout self-test over many seeds; writes SELFTEST lines through
 * the given printer. Returns the number of failures. */
int modLevelGenSelfTest(int numseeds, void (*print)(const char *line));

#endif
