#include <ultra64.h>
#include <string.h>
#include "constants.h"
#include "game/smoke.h"
#include "game/sparks.h"
#include "game/bg.h"
#include "game/propobj.h"
#include "game/splat.h"
#include "game/utils.h"
#include "game/wallhit.h"
#include "bss.h"
#include "lib/collision.h"
#include "lib/lib_17ce0.h"
#include "lib/model.h"
#include "lib/rng.h"
#include "lib/mtx.h"
#include "data.h"
#include "types.h"
#ifdef PD_PHYSICS
#include "ragdoll.h"
#endif

struct coord *g_SplatPuddleAt = NULL; // MOD: gore - a puddle (not a shot's splat) goes under this point instead of the chr's middle (set round a splatsCreate())

#define SPLATTYPE_PUDDLE 1
#define SPLATTYPE_DROP   2
#define SPLATTYPE_HIT_FLOOR 3
#define SPLATTYPE_SPRAY 4
#define SPLAT_HIT_SCALE 0.8f

struct splatdata {
	struct coord relpos;
	struct coord unk0c;
	struct coord unk18;
	struct coord gunpos;
	s32 splattype;
	struct prop *objprop;
	struct prop *chrprop;
	struct chrdata *chr;
	s32 mtxindex;
	s32 room;
	s32 isskedar;
	s32 translucent;
	f32 unk50;
	s32 timermax;
	s32 timerspeed;
};

#if VERSION < VERSION_NTSC_1_0
u32 var80082100nb = 0;
#endif

f32 var8007f8a0 = 0.15;
f32 var8007f8a4 = 3;
f32 var8007f8a8 = 12;
s32 var8007f8ac = 8;
f32 g_SplatMaxDistance = 180;
f32 var8007f8b4 = 5;
f32 var8007f8b8 = 50;

#if VERSION == VERSION_JPN_FINAL
#else
bool splat0f149274(f32 arg0, struct prop *prop, struct shotdata *shotdata, f32 arg3, bool isskedar, s32 arg5, s32 arg6, struct chrdata *chr, s32 arg8);
void splat0f14986c(struct splatdata *splatdata);
#endif

/**
 * MOD: gore - a corpse bleeds under the parts it was shot in. Stock, a dead
 * chr that had taken a bullet pooled up to six puddles under its middle, one
 * every half second from a moment after it landed. Now there is a pool under
 * each part in the chr's wound list (chrDamage() sets woundmask), three
 * puddles each, in turn, at the same pace: a chr shot in the leg and the head
 * pools under the leg and the head, and one never shot in the body has
 * nothing under its middle. A body shot in the torso or the pelvis pools
 * there. Puddles are bigger under the head (1.4) than under the body (1.1)
 * or a limb (0.9). On a body that is not a ragdoll the parts' places are not
 * known, and the puddles go under the middle as before.
 */
static void splatCorpsePool(struct prop *prop, struct chrdata *chr, bool isskedar, struct chrdata *attacker)
{
	s32 sites[16];
	s32 numsites = 0;
	s32 cap;
	s32 site;
	f32 size;
	struct coord at;
	s32 i;

	for (i = 1; i <= HITPART_TAIL; i++) {
		if (chr->woundmask & (1u << i)) {
			sites[numsites++] = i;
		}
	}

	cap = numsites * 3;

	if (cap > 15) {
		cap = 15;
	}

	if (numsites == 0 || chr->deaddropsplatsadded >= cap) {
		return;
	}

	site = sites[chr->deaddropsplatsadded % numsites];
	size = site == HITPART_HEAD ? 1.4f : (site == HITPART_TORSO || site == HITPART_PELVIS) ? 1.1f : 0.9f;

#ifdef PD_PHYSICS
	{
		if (ragdollGetHitpartPos(chr, site, &at)) {
			g_SplatPuddleAt = &at;
		}
	}
#endif

	chr->deaddropsplatsadded += splatsCreate(1, size, prop, NULL, 0, 0, isskedar, SPLATTYPE_PUDDLE, TICKS(150), attacker, rngRandom() & 8);
	g_SplatPuddleAt = NULL;
}

#if !defined(PLATFORM_N64) && VERSION != VERSION_JPN_FINAL
static void splatTickHeadshotDeath(struct prop *prop, bool isskedar);
#endif

void splatTickChr(struct prop *prop)
{
	struct chrdata *chr = prop->chr;
	struct chrdata *attacker = chr->lastattacker;
	s32 race;

	if (chr->noblood || (chr->chrflags & CHRCFLAG_HIDDEN) || (chr->bulletstaken == 0 && chr->woundmask == 0)) {
		return;
	}

	race = CHRRACE(chr);

	if (race != RACE_DRCAROLL && race != RACE_ROBOT) {
		u8 isskedar = false;

		if (race == RACE_SKEDAR || chr->bodynum == BODY_MRBLONDE) {
			isskedar = true;
		}

		osSyncPrintf("Splat Tick - P=%x, B=%d, T=%d, S=%d, W=%d, D=%d, H=%d\n");

		if (chr->actiontype == ACT_DEAD || chr->actiontype == ACT_DIE) {
			u32 stack;
			f32 thudframe = -1.0f;

			if (chr->actiontype == ACT_DIE) {
#if !defined(PLATFORM_N64) && VERSION != VERSION_JPN_FINAL
				if (prop->type == PROPTYPE_CHR) {
					splatTickHeadshotDeath(prop, isskedar);
				}
#endif
				if (chr->act_die.thudframe2 != -1) {
					thudframe = chr->act_die.thudframe2;
				} else if (chr->act_die.thudframe1 != -1) {
					thudframe = chr->act_die.thudframe1;
				}
			}

			if (thudframe != -1.0f && modelGetCurAnimFrame(chr->model) < thudframe) {
				osSyncPrintf("SPLAT : Not Dead Enough %s%s%f", "", "", modelGetCurAnimFrame(chr->model));
			} else if (chr->tickssincesplat > TICKS(30)) {
				splatCorpsePool(prop, chr, isskedar, attacker);
			}
		} else {
			// Consider creating a wounded drop
			u32 value = chr->bulletstaken * chr->tickssincesplat;

			if (value > TICKS(240)) {
				f32 dist = coordsGetDistance(&chr->lastdroppos, &prop->pos);
				s32 addmore = false;

				if (dist > 40) {
					addmore = true;
					chr->splatsdroppedhere = 0;
				} else if (chr->splatsdroppedhere < 8) {
					addmore = true;
					chr->splatsdroppedhere++;
				}

				if (addmore) {
					chr->woundedsplatsadded += splatsCreate(1, 0.3f, prop, NULL, 0, 0, isskedar, SPLATTYPE_DROP, TICKS(80), attacker, 0);
				}
			}

			if (chr->woundedsplatsadded >= 40) {
				wallhitRemoveOldestWoundedSplatByChr(prop);
				chr->woundedsplatsadded--;
			}

			chr->deaddropsplatsadded = 0;
		}
	}

	chr->tickssincesplat += g_Vars.lvupdate60;
}

#if VERSION != VERSION_JPN_FINAL
static bool splatCreateHitFloor(struct prop *prop, struct coord direction, f32 range, bool isskedar,
		s32 timermax, struct chrdata *attacker)
{
	const f32 extra = range * 0.5f * RANDOMFRAC();
	// Start anywhere from wall-splat range to half again as far. If the floor
	// there is blocked or missing, step back toward the target.
	const f32 offsets[] = {
		range + extra, range + extra * 0.5f, range,
		range * 0.75f, range * 0.5f, range * 0.3f, range * 0.18f, range * -0.18f, 0.0f,
	};
	struct coord from = prop->pos;
	struct shotdata down = {0};
	const f32 horizontal = sqrtf(direction.x * direction.x + direction.z * direction.z);
	const f32 dx = horizontal > 0.001f ? direction.x / horizontal : 1.0f;
	const f32 dz = horizontal > 0.001f ? direction.z / horizontal : 0.0f;
	const f32 height = from.y - prop->chr->manground;

	// Work near the feet so the decal stays on the local floor at ledges.
	from.y = height >= -30.0f && height <= prop->chr->height + 45.0f
		? prop->chr->manground + 40.0f : from.y + 40.0f;
	down.gundir3d.y = down.gundir2d.y = -1.0f;

	for (s32 i = 0; i < ARRAYCOUNT(offsets); i++) {
		RoomNum rooms[8];
		const f32 offset = offsets[i];
		struct coord origin = from;
		origin.x += dx * offset;
		origin.z += dz * offset;
		if (offset && !cdTestLos11(&from, prop->rooms, &origin, rooms,
				CDTYPE_BG | CDTYPE_DOORS | CDTYPE_OBJS)) continue;

		down.gunpos3d = down.gunpos2d = origin;
		if (splat0f149274(SPLAT_HIT_SCALE, prop, &down, 0.0f, isskedar, SPLATTYPE_HIT_FLOOR, timermax, attacker, 0)) {
			return true;
		}
	}
	return false;
}

#ifndef PLATFORM_N64
static void splatTickHeadshotDeath(struct prop *prop, bool isskedar)
{
	struct chrdata *chr = prop->chr;
	struct model *model = chr->model;

	if (!model || chr->act_die.nextheadshotsprayframe < 0.0f || prop->rooms[0] < 0
			|| g_Vars.lvupdate240 <= 0) {
		return;
	}

	switch (modelGetAnimNum(model)) {
	case ANIM_0038:
	case ANIM_0251:
	case ANIM_0252:
	case ANIM_0256:
	case ANIM_0338: // Skedar
		break;
	default:
		return;
	}

	const f32 frame = modelGetCurAnimFrame(model);
	if (frame < chr->act_die.nextheadshotsprayframe || frame >= modelGetAnimEndFrame(model)) {
		return;
	}

	struct coord origin = prop->pos;
	origin.y += chr->height * 0.7f;
#ifdef PD_PHYSICS
	if (ragdollIsChrRagdolled(chr)) {
		if (!ragdollIsChrBlending(chr)) return;
		ragdollGetHitpartPos(chr, HITPART_HEAD, &origin);
	}
#endif

	// Run at the animation's pace, including when slow motion is active.
	chr->act_die.nextheadshotsprayframe = frame + 6.0f;
	if (chr->lastattacker && chrIsUsingPaintball(chr->lastattacker)) return;

	const f32 angle = RANDOMFRAC() * 6.2831853f;
	struct coord dir = { .x = cosf(angle), .y = -0.10f, .z = sinf(angle) };
	sparksCreate(prop->rooms[0], prop, &origin, &dir, NULL, SPARKTYPE_BLOOD);

	if (g_FreeWallhits) {
		struct shotdata spray = {0};
		spray.gunpos3d = spray.gunpos2d = origin;
		spray.gundir3d = spray.gundir2d = dir;
		splat0f149274(SPLAT_HIT_SCALE, prop, &spray, 0.0f, isskedar, SPLATTYPE_SPRAY,
				TICKS(75), chr->lastattacker, 0);
	}
	if (g_FreeWallhits) {
		// Put the floor stain on the other side of the body from the wall ray.
		dir.x = -dir.x;
		dir.z = -dir.z;
		splatCreateHitFloor(prop, dir, 85.0f, isskedar, TICKS(75), chr->lastattacker);
	}
}
#endif
#endif

static void splatsCreateForChrHitSized(struct prop *prop, struct shotdata *shotdata, struct coord *arg2, struct coord *arg3, bool isskedar, s32 splattype, struct chrdata *chr2, bool headshot)
{
#if VERSION != VERSION_JPN_FINAL
	struct chrdata *chr = prop->chr;

	if (chr->bulletstaken < 7) {
		chr->bulletstaken++;
	}

	if (splattype == 0) {
		// MOD: gore - a headshot throws 3-5 splats (stock 0-2), at the
		// stock size (0.8), that last half again as long.
		const u32 qty = headshot ? 3 + rngRandom() % 3 : rngRandom() % 3;
		const s32 timermax = headshot ? TICKS(75) : TICKS(50);
		const struct coord direction = shotdata->gundir3d;
		s32 dropped = 0;

		if (qty) {
			dropped = splatsCreate(qty, SPLAT_HIT_SCALE, prop, shotdata, arg2, arg3,
					isskedar, splattype, timermax, chr2, 0);
		}
		if (dropped == 0 && g_FreeWallhits && splatCreateHitFloor(prop, direction, g_SplatMaxDistance, isskedar, timermax, chr2)) {
			dropped = 1;
			chr->tickssincesplat = 0;
			chr->lastdroppos = prop->pos;
		}
		chr->stdsplatsadded += dropped;
	}
#endif
}

void splatsCreateForChrHit(struct prop *prop, struct shotdata *shotdata, struct coord *arg2, struct coord *arg3, bool isskedar, s32 splattype, struct chrdata *chr2)
{
	splatsCreateForChrHitSized(prop, shotdata, arg2, arg3, isskedar, splattype, chr2, false);
}

void splatsCreateForChrHeadshot(struct prop *prop, struct shotdata *shotdata, struct coord *arg2, struct coord *arg3, bool isskedar, s32 splattype, struct chrdata *chr2)
{
	splatsCreateForChrHitSized(prop, shotdata, arg2, arg3, isskedar, splattype, chr2, true);
}

s32 splatsCreate(s32 qty, f32 arg1, struct prop *prop, struct shotdata *shotdataarg,
		struct coord *arg4, struct coord *arg5, bool isskedar, s32 splattype,
		s32 timermax, struct chrdata *chr, s32 timerspeed)
{
#if VERSION == VERSION_JPN_FINAL
	return 0;
#else
	struct shotdata stackshotdata;
	struct shotdata *shotdata = splattype == 0 ? shotdataarg : &stackshotdata;
	struct coord spfc;
	struct coord spf0;
	struct coord spe4;
	Mtxf spa4;
	s32 numdropped = 0;
	f32 dist;
	s32 i;
	s32 j;

	if (splattype == 0) {
		dist = coordsGetDistance(&shotdata->gunpos3d, arg5);

		for (i = 0; i < 3; i++) {
			spfc.f[i] = shotdata->gundir3d.f[i];
			spf0.f[i] = shotdata->gundir2d.f[i];
			shotdata->gunpos3d.f[i] = arg5->f[i];
			shotdata->gunpos2d.f[i] = arg4->f[i];
		}
	} else {
		f32 extraheight;

		if (prop->type == PROPTYPE_CHR) {
			extraheight = 50;
		} else {
			extraheight = 0;
		}

		dist = 0.7f;

		spfc.x = shotdata->gundir3d.x = 0;
		spfc.y = shotdata->gundir3d.y = -1;
		spfc.z = shotdata->gundir3d.z = 0;

		spf0.x = shotdata->gundir2d.x = 0;
		spf0.y = shotdata->gundir2d.y = -1;
		spf0.z = shotdata->gundir2d.z = 0;

		struct coord *base = g_SplatPuddleAt ? g_SplatPuddleAt : &prop->pos;

		shotdata->gunpos3d.x = base->x;
		shotdata->gunpos3d.y = base->y + extraheight;
		shotdata->gunpos3d.z = base->z;

		shotdata->gunpos2d.x = base->x;
		shotdata->gunpos2d.y = base->y + extraheight;
		shotdata->gunpos2d.z = base->z;
	}

	for (i = 0; i < qty; i++) {
		for (j = 0; j < 3; j++) {
			spe4.f[j] = (RANDOMFRAC() * var8007f8a8 * 2.0f - var8007f8a8) * 0.017453292384744f;
		}

		mtx4LoadRotation(&spe4, &spa4);
		mtx4RotateVec(&spa4, &spfc, &shotdata->gundir3d);
		mtx4RotateVec(&spa4, &spf0, &shotdata->gundir2d);

#if VERSION >= VERSION_NTSC_1_0
		func0f177164(&shotdata->gundir3d, &shotdata->gundir3d, 403, "splat.c");
		func0f177164(&shotdata->gundir2d, &shotdata->gundir2d, 404, "splat.c");
#else
		func0f177164(&shotdata->gundir3d, &shotdata->gundir3d, 405, "splat.c");
		func0f177164(&shotdata->gundir2d, &shotdata->gundir2d, 406, "splat.c");
#endif

		if (splat0f149274(arg1, prop, shotdata, /*reused var*/ dist, isskedar, splattype, timermax, chr, timerspeed)) {
			numdropped++;
		}
	}

	if (numdropped) {
		struct chrdata *chr = prop->chr;

		chr->tickssincesplat = 0;
		chr->lastdroppos.x = prop->pos.x;
		chr->lastdroppos.y = prop->pos.y;
		chr->lastdroppos.z = prop->pos.z;
	}

	return numdropped;
#endif
}

#if VERSION == VERSION_JPN_FINAL
void splat0f149274(void)
{
	osSyncPrintf("Splat : Out of range\n");
}
#else
bool splat0f149274(f32 arg0, struct prop *chrprop, struct shotdata *shotdata, f32 arg3, bool isskedar, s32 splattype, s32 timermax, struct chrdata *chr, s32 timerspeed)
{
	struct prop **propptr;
	struct prop *objprop;
	struct hitthing hitthing;
	struct hitthing besthitthing;
	RoomNum rooms[32];
	RoomNum gunrooms[8];
	RoomNum endrooms[8];
	struct coord endpos;
	s32 i;
	struct coord *sp50c;
	struct coord *hitpos;
	struct coord *sp504;
	s32 bestroom = 0;
	s32 mtxindex;
	s32 room;
	f32 spraydistance;
	bool translucent;
	bool hasresult = false;
	struct shotdata stackshotdata;
	const bool hitfloor = splattype == SPLATTYPE_HIT_FLOOR;
	const f32 maxdistance = hitfloor ? 100.0f : g_SplatMaxDistance;

	for (i = 0; i < 3; i++) {
		stackshotdata.gunpos2d.f[i] = shotdata->gunpos2d.f[i];
		stackshotdata.gundir2d.f[i] = shotdata->gundir2d.f[i];
		stackshotdata.gunpos3d.f[i] = shotdata->gunpos3d.f[i];
		stackshotdata.gundir3d.f[i] = shotdata->gundir3d.f[i];
	}

	stackshotdata.penetration = 1;

	for (i = 0; i < 3; i++) {
		endpos.f[i] = stackshotdata.gunpos3d.f[i] + stackshotdata.gundir3d.f[i] * maxdistance;
	}

	portal00018148(&chrprop->pos, &stackshotdata.gunpos3d, chrprop->rooms, gunrooms, NULL, 0);
	portal00018148(&stackshotdata.gunpos3d, &endpos, gunrooms, endrooms, rooms, ARRAYCOUNT(rooms) - 1);

	for (i = 0; rooms[i] != -1; i++) {
		if (bgTestHitInRoom(&stackshotdata.gunpos3d, &endpos, rooms[i], &hitthing)
				&& (!hitfloor || hitthing.unk0c.y > 0.0f)
				&& ((stackshotdata.gunpos3d.x <= endpos.x && hitthing.pos.x <= endpos.x && stackshotdata.gunpos3d.x <= hitthing.pos.x) || (endpos.x <= stackshotdata.gunpos3d.x && endpos.x <= hitthing.pos.x && hitthing.pos.x <= stackshotdata.gunpos3d.x))
					&& ((stackshotdata.gunpos3d.y <= endpos.y && hitthing.pos.y <= endpos.y && stackshotdata.gunpos3d.y <= hitthing.pos.y) || (endpos.y <= stackshotdata.gunpos3d.y && endpos.y <= hitthing.pos.y && hitthing.pos.y <= stackshotdata.gunpos3d.y))
					&& ((stackshotdata.gunpos3d.z <= endpos.z && hitthing.pos.z <= endpos.z && stackshotdata.gunpos3d.z <= hitthing.pos.z) || (endpos.z <= stackshotdata.gunpos3d.z && endpos.z <= hitthing.pos.z && hitthing.pos.z <= stackshotdata.gunpos3d.z))) {
			if (stackshotdata.gunpos3d.x != hitthing.pos.x || stackshotdata.gunpos3d.y != hitthing.pos.y || stackshotdata.gunpos3d.z != hitthing.pos.z) {
				bestroom = rooms[i];
				besthitthing = hitthing;

				endpos.x = hitthing.pos.x;
				endpos.y = hitthing.pos.y;
				endpos.z = hitthing.pos.z;

				hasresult = true;
				break;
			}
		}
	}

	if (hasresult) {
		spraydistance = coordsGetDistance(&stackshotdata.gunpos3d, &besthitthing.pos);

		if (spraydistance < maxdistance) {
			sp50c = &hitthing.pos;
			hitpos = &hitthing.pos;
			sp504 = &hitthing.unk0c;
			objprop = NULL;
			mtxindex = 0;
			room = bestroom;
			translucent = hitthing.unk2c == 2;
		} else {
			osSyncPrintf("Splat : Out of range\n");
			hasresult = false;
		}
	} else {
		spraydistance = 999999.0f;
	}

	if (!hasresult && (splattype == SPLATTYPE_PUDDLE || hitfloor)) {
		// Use the collision floor when the short shot fallback missed a visual
		// triangle, or when a corpse's room geometry is still unloaded.
		struct prop *floorprop = NULL;
		struct coord floor = stackshotdata.gunpos3d;
		RoomNum floorroom = cdFindFloorRoomYColourNormalPropAtPos(
				&stackshotdata.gunpos3d, rooms, &floor.y, NULL, &hitthing.unk0c, &floorprop);

		if (floorroom > 0 && floorroom < g_Vars.roomcount && !floorprop
				&& (hitfloor || !g_Rooms[floorroom].vtxbatches)
				&& floor.y < stackshotdata.gunpos3d.y && floor.y >= endpos.y
				&& (!hitfloor || hitthing.unk0c.y != 0.0f)) {
			// Collision tile winding can point down.
			if (hitfloor && hitthing.unk0c.y < 0.0f) {
				for (i = 0; i < 3; i++) hitthing.unk0c.f[i] = -hitthing.unk0c.f[i];
			}
			hitthing.pos = floor;
			sp50c = hitpos = &hitthing.pos;
			sp504 = &hitthing.unk0c;
			objprop = NULL;
			mtxindex = 0;
			room = floorroom;
			translucent = false;
			hasresult = true;
		}
	}

	if (splattype == 0) {
		stackshotdata.distance = (spraydistance < g_SplatMaxDistance ? spraydistance : g_SplatMaxDistance);
		stackshotdata.distance += arg3;

		for (i = 0; i < ARRAYCOUNT(stackshotdata.hits); i++) {
			stackshotdata.hits[i].prop = NULL;
			stackshotdata.hits[i].hitpart = 0;
			stackshotdata.hits[i].bboxnode = NULL;
		}

		propptr = g_Vars.endonscreenprops - 1;

		while (propptr >= g_Vars.onscreenprops) {
			struct prop *prop = *propptr;

			if (prop) {
				if (prop->type == PROPTYPE_OBJ || prop->type == PROPTYPE_DOOR || prop->type == PROPTYPE_WEAPON) {
					objTestHit(prop, &stackshotdata);
					if (1);
				}
			}

			propptr--;
		}

		for (i = 0; i < ARRAYCOUNT(stackshotdata.hits); i++) {
			struct hit *hit = &stackshotdata.hits[i];

			if (hit->prop && (hit->hitthing.texturenum < 0
						|| hit->hitthing.texturenum >= NUM_TEXTURES
						|| g_SurfaceTypes[g_Textures[hit->hitthing.texturenum].surfacetype]->numwallhittexes != 0)) {
				sp50c = &hit->hitthing.pos;
				hitpos = &hit->pos;
				sp504 = &hit->hitthing.unk0c;
				objprop = hit->prop;
				mtxindex = (s8)hit->mtxindex;
				room = 1;
				translucent = false;
				hasresult = true;
				break;
			}
		}
	}

	if (hasresult) {
		struct splatdata splatdata;
		u32 stack;

		for (i = 0; i < 3; i++) {
			splatdata.relpos.f[i] = sp50c->f[i];
			splatdata.unk0c.f[i] = hitpos->f[i];
			splatdata.gunpos.f[i] = stackshotdata.gunpos3d.f[i];
			splatdata.unk18.f[i] = sp504->f[i];
		}

		splatdata.chrprop = chrprop;
		splatdata.objprop = objprop;
		splatdata.chr = chr;
		splatdata.mtxindex = mtxindex;
		splatdata.room = room;
		splatdata.isskedar = isskedar;
		splatdata.timermax = timermax;
		splatdata.unk50 = arg0;
		splatdata.splattype = splattype;
		splatdata.timerspeed = timerspeed;
		splatdata.translucent = translucent;

		splat0f14986c(&splatdata);

		return true;
	}

	if (hitthing.pos.x);
	if (hitthing.pos.y);
	if (hitthing.pos.z);

	return false;
}
#endif

void splatsTick(void)
{
	// empty
}

#if VERSION == VERSION_JPN_FINAL
void splat0f14986c(void)
{
	// empty
}

#else
void splat0f14986c(struct splatdata *splat)
{
	f32 spac;
	f32 spa8;
	struct defaultobj *obj;
	f32 spa0;
	f32 sp9c;
	f32 height;
	f32 width;
	u8 maxalpha = 0xff;
	u8 minalpha = 0xc0;
	s32 texnum;
	bool sp88 = splat->isskedar & 1;
	bool sp84 = splat->isskedar & 1;
	bool translucent = splat->translucent;
	f32 distance;
	RoomNum smokerooms[2];

	texnum = WALLHITTEX_BLOOD1 + (rngRandom() % 3);

	if (splat->objprop != NULL && splat->objprop->type == PROPTYPE_OBJ) {
		obj = splat->objprop->obj;

		if (obj && (obj->type == OBJTYPE_GLASS || obj->type == OBJTYPE_TINTEDGLASS)) {
			minalpha = 0x40;
			translucent = true;
		}
	}

	switch (splat->splattype) {
	case SPLATTYPE_PUDDLE:
		texnum = WALLHITTEX_BLOOD1 + (rngRandom() % 3);
		break;
	case SPLATTYPE_DROP:
		texnum = WALLHITTEX_BLOOD4 + (rngRandom() % 1);
		break;
	}

	switch (rngRandom() % 6) {
	case 0:
	case 1:
	case 2:
		sp9c = 1.5f;
		break;
	case 3:
	case 4:
		sp9c = 5.0f;
		break;
	case 5:
		sp9c = 3.0f;
		break;
	}

	// Size the floor fallback like a wall hit at full range. Its downward
	// probe measures the height above the floor instead of the spray distance.
	distance = splat->splattype == SPLATTYPE_HIT_FLOOR
		? g_SplatMaxDistance : coordsGetDistance(&splat->gunpos, &splat->unk0c);
	spa0 = var8007f8a0 * distance * sp9c;

	if (var8007f8b8 < spa0) {
		spa0 = var8007f8b8;
	}

	if (var8007f8b4 > spa0) {
		spa0 = var8007f8b4;
	}

	spac = 0.5f * spa0;
	spa8 = 0.5f * spa0;

	if (spac < 1.0f) {
		spac = 1.0f;
	}

	if (spa8 < 1.0f) {
		spa8 = 1.0f;
	}

	width = RANDOMFRAC() * spac * 2.0f - spac + spa0;
	height = RANDOMFRAC() * spa8 * 2.0f - spa8 + spa0;

	if (width > var8007f8b8) {
		width = var8007f8b8;
	}

	if (height > var8007f8b8) {
		height = var8007f8b8;
	}

	width *= splat->unk50;
	height *= splat->unk50;

	wallhitChooseBloodColour(splat->chrprop);

	wallhitCreateWith20Args(&splat->relpos, &splat->unk18, splat->chr ? &splat->chr->prop->pos : NULL, NULL,
			NULL, texnum, splat->room, splat->objprop,
			splat->chrprop, splat->mtxindex, 0, splat->chr,
			width, height, minalpha, maxalpha,
			rngRandom() % 360, (u16)splat->timermax, splat->timerspeed, translucent);

#ifdef PD_PHYSICS
	// MOD: blood wrap - on the level, the splat carries on round corners (port/src/bloodwrap.c)
	if (splat->objprop == NULL) {
		extern void bloodWrapSplat(struct coord *pos, struct coord *normal, f32 width, f32 height, s16 texnum, RoomNum room,
				struct prop *chrprop, struct chrdata *chr, u8 minalpha, u8 maxalpha, u32 timermax, u32 timerspeed, bool xlu);

		bloodWrapSplat(&splat->relpos, &splat->unk18, width, height, texnum, splat->room,
				splat->chrprop, splat->chr, minalpha, maxalpha, (u16)splat->timermax, splat->timerspeed, translucent);
	}

#endif
	if (sp88 || sp84) {
		smokerooms[0] = splat->room;
		smokerooms[1] = -1;

		smokeCreateSimple(&splat->unk0c, smokerooms, sp88 ? SMOKETYPE_SKCORPSE : SMOKETYPE_14);
	}
}
#endif

/**
 * MOD: gore - one puddle of a blood trail, under a point (a ragdoll sliding
 * along the floor, dismember.c ragdollBloodTrail()). The game's own puddle:
 * the same texture and spread as a corpse's, a ray down from above the point.
 */
void splatCreateTrail(struct prop *prop, struct chrdata *chr, struct coord *at, f32 size)
{
#if VERSION != VERSION_JPN_FINAL
	u8 isskedar = CHRRACE(chr) == RACE_SKEDAR || chr->bodynum == BODY_MRBLONDE;

	g_SplatPuddleAt = at;
	splatsCreate(1, size, prop, NULL, 0, 0, isskedar, SPLATTYPE_PUDDLE, TICKS(100), chr->lastattacker, rngRandom() & 8);
	g_SplatPuddleAt = NULL;
#endif
}

/**
 * MOD: gore - one splat of a blood trail on a wall next to a point (a ragdoll
 * sliding down or along a wall, dismember.c ragdollBloodTrail()). Looks for a
 * wall within reach in eight directions round the point, level with it, and
 * puts a splat on the nearest, by the game's own shot splat: a ray from the
 * point towards that wall. Returns 1 if there was a wall and the splat went
 * up, 0 if there was no wall in reach.
 */
s32 splatCreateTrailWall(struct prop *prop, struct chrdata *chr, struct coord *at, f32 size, f32 reach)
{
#if VERSION == VERSION_JPN_FINAL
	return 0;
#else
	struct hitthing hitthing;
	struct shotdata sd;
	struct coord end;
	struct coord dir;
	struct coord bestdir;
	f32 bestdist = 1e30f;
	s32 i;
	s32 j;
	u8 isskedar;

	for (i = 0; i < 8; i++) {
		f32 a = i * 0.785398f;

		dir.x = cosf(a);
		dir.y = 0.0f;
		dir.z = sinf(a);

		end.x = at->x + dir.x * reach;
		end.y = at->y;
		end.z = at->z + dir.z * reach;

		for (j = 0; prop->rooms[j] != -1; j++) {
			if (bgTestHitInRoom(at, &end, prop->rooms[j], &hitthing)
					&& hitthing.unk0c.y > -0.5f && hitthing.unk0c.y < 0.5f) {
				f32 dist = coordsGetDistance(at, &hitthing.pos);

				if (dist < bestdist) {
					bestdist = dist;
					bestdir = dir;
				}

				break;
			}
		}
	}

	if (bestdist > reach) {
		return 0;
	}

	isskedar = CHRRACE(chr) == RACE_SKEDAR || chr->bodynum == BODY_MRBLONDE;

	memset(&sd, 0, sizeof(sd));
	sd.gunpos3d = *at;
	sd.gunpos2d = *at;
	sd.gundir3d = bestdir;
	sd.gundir2d = bestdir;

	return splatsCreate(1, size, prop, &sd, at, at, isskedar, 0, TICKS(100), chr->lastattacker, 0) ? 1 : 0;
#endif
}

void splatResetChr(struct chrdata *chr)
{
	osSyncPrintf("Splat_ResetChr : Reset One Char : chrdata = %x\n", (uintptr_t) chr);

	chr->woundmask = 0;
	chr->bulletstaken = 0;
	chr->tickssincesplat = 0;
	chr->stdsplatsadded = 0;
	chr->woundedsplatsadded = 0;
	chr->deaddropsplatsadded = 0;
	chr->splatsdroppedhere = 0;
	chr->lastdroppos.x = 0;
	chr->lastdroppos.y = 0;
	chr->lastdroppos.z = 0;
}
