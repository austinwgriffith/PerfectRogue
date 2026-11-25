#include <ultra64.h>
#include <PR/libaudio.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "fs.h"
#include "gelevels.h"
#include "gesfxdoor.h"
#include "prstubs.h"
#include "preprocess.h"
#include "system.h"
#include "lib/snd.h"
#include "game/propsnd.h"

#define GESFX_MAX 512
#define GESFX_DIR "mods/goldeneye/menu/"
#define GE_SOUND_DMA_BLOCK 0x400

struct sfxbank {
	u8 *ctl;
	u8 *tbl;
	ALInstrument *inst;
	s16 map[GESFX_MAX];
	uintptr_t *rebased;
	s32 numrebased;
	s32 maxrebased;
};

static struct sfxbank g_DoorBank;
static bool g_DoorBankChecked;

static bool geDoorBankLoad(void)
{
	u32 len, ctllen;
	u8 *raw, *ctl, *tbl;
	ALBankFile *file;
	ALBank *bank;

	if (g_DoorBankChecked) return g_DoorBank.inst != NULL;
	g_DoorBankChecked = true;

	if (fsFileSize(GESFX_DIR "sfxctl") <= 0 || fsFileSize(GESFX_DIR "sfxtbl") <= 0) return false;
	raw = fsFileLoad(GESFX_DIR "sfxctl", &len);
	if (!raw) return false;
	ctl = preprocessALBankFile(raw, len, &ctllen);
	if (ctl != raw) sysMemFree(raw);
	tbl = fsFileLoadPadded(GESFX_DIR "sfxtbl", &len, GE_SOUND_DMA_BLOCK);
	if (!ctl || !tbl) {
		sysMemFree(ctl);
		sysMemFree(tbl);
		return false;
	}

	g_DoorBank.ctl = ctl;
	g_DoorBank.tbl = tbl;
	file = (ALBankFile *)ctl;
	bank = (ALBank *)(ctl + (uintptr_t)file->bankArray[0]);
	g_DoorBank.inst = (ALInstrument *)(ctl + (uintptr_t)bank->instArray[0]);
	return true;
}

static s32 sfxRebaseOnce(struct sfxbank *b, uintptr_t off)
{
	for (s32 i = 0; i < b->numrebased; i++) {
		if (b->rebased[i] == off) {
			return 0;
		}
	}

	if (b->numrebased == b->maxrebased) {
		const s32 max = b->maxrebased ? b->maxrebased * 2 : 64;
		uintptr_t *grown = sysMemRealloc(b->rebased, max * sizeof(uintptr_t));

		if (!grown) {
			return 0;
		}

		b->rebased = grown;
		b->maxrebased = max;
	}

	b->rebased[b->numrebased++] = off;

	return 1;
}

#define GESFX_CTL_DELTA(b) ((uintptr_t)(b)->ctl - sndGetCtlStart())

static s32 sfxGetIn(struct sfxbank *b, s32 id)
{
	ALSound *sound;
	uintptr_t off;
	s32 ours;

	if (!b || id <= 0 || id >= GESFX_MAX) {
		return 0;
	}

	if (b->map[id]) {
		return b->map[id] > 0 ? b->map[id] : 0;
	}

	if (id > b->inst->soundCount) {
		b->map[id] = -1;
		return 0;
	}

	off = (uintptr_t)b->inst->soundArray[id - 1];
	sound = (ALSound *)(b->ctl + off);

	if (sfxRebaseOnce(b, off)) {
		if (sound->envelope) {
			sound->envelope = (ALEnvelope *)((uintptr_t)sound->envelope + GESFX_CTL_DELTA(b));
		}

		if (sound->keyMap) {
			const uintptr_t koff = (uintptr_t)sound->keyMap;
			ALKeyMap *keymap = (ALKeyMap *)(b->ctl + koff);

			sound->keyMap = (ALKeyMap *)(koff + GESFX_CTL_DELTA(b));

			if (sfxRebaseOnce(b, koff)) {
				// Clear GoldenEye's sound-chain metadata from the key map.
				keymap->velocityMin = 0;
				keymap->keyMin &= ~0xc0;
			}
		}

		if (sound->wavetable) {
			const uintptr_t woff = (uintptr_t)sound->wavetable;
			ALWaveTable *wave = (ALWaveTable *)(b->ctl + woff);

			sound->wavetable = (ALWaveTable *)(woff + GESFX_CTL_DELTA(b));

			if (sfxRebaseOnce(b, woff)) {
				wave->base = (u8 *)((uintptr_t)wave->base + (uintptr_t)b->tbl - sndGetTblStart());

				if (wave->type == AL_ADPCM_WAVE) {
					if (wave->waveInfo.adpcmWave.book) {
						wave->waveInfo.adpcmWave.book = (ALADPCMBook *)((uintptr_t)wave->waveInfo.adpcmWave.book + GESFX_CTL_DELTA(b));
					}

					if (wave->waveInfo.adpcmWave.loop) {
						wave->waveInfo.adpcmWave.loop = (ALADPCMloop *)((uintptr_t)wave->waveInfo.adpcmWave.loop + GESFX_CTL_DELTA(b));
					}
				} else if (wave->waveInfo.rawWave.loop) {
					wave->waveInfo.rawWave.loop = (ALRawLoop *)((uintptr_t)wave->waveInfo.rawWave.loop + GESFX_CTL_DELTA(b));
				}
			}
		}
	}

	ours = sndAppendSound(off + GESFX_CTL_DELTA(b));

	if (ours <= 0) {
		// no bank loaded (--no-sound) or no ids left
		b->map[id] = -1;
		return 0;
	}


	return b->map[id] = ours;
}

#define SFX_TRAIN_SLIDE        7
#define SFX_WOOD_CLOSE         187
#define SFX_WOOD_OPEN          188
#define SFX_WOOD_SLIDE         191
#define SFX_TRAIN_CATCH        192
#define SFX_SHUTTER_OPEN       194
#define SFX_SHUTTER_CLOSE      195
#define SFX_METAL_OPEN         196
#define SFX_METAL_CLOSE        197
#define SFX_METAL_CLOSE2       199
#define SFX_METAL_OPEN3        200
#define SFX_METAL_CLOSE3       201
#define SFX_METAL_SLIDE_OPEN   202
#define SFX_METAL_SLIDE_CLOSE  203
#define SFX_METAL_SLIDE_LOOP   204
#define SFX_SMART_CATCH        210
#define SFX_SMART_SLIDE        211
#define SFX_HEAVY_SLIDE_OPEN   214
#define SFX_HEAVY_SLIDE_CLOSE  215
#define SFX_HEAVY_SLIDE_LOOP   216
#define SFX_HYDRAL_CLOSE       218
#define SFX_HYDRAL_OPEN        219
#define SFX_STONE_OPEN         225
#define SFX_STONE_CLOSE        226

#define GESFX_NUM_DOOR_TYPES 18

/**
 * GoldenEye's propobj.c, doorPlayOpenSound0/1() and doorPlayCloseSound0/1(),
 * by DOOR_OPEN_SOUND. A sound is started one of two ways there. `once` is
 * given its volume where the door is and let go: nothing stops it, and it
 * rings out over whatever the door does next. `held` goes in one of the door's
 * two sound states: its volume follows the player, and it is what the door's
 * next sound stops - the slide under a moving door, which is a looped wave
 * for the metal and the heavy doors.
 */
static const struct {
	u8 once[2];
	u8 held;
} g_SfxDoors[4][GESFX_NUM_DOOR_TYPES] = {
	[GESFX_DOOR_OPENING] = {
		[1]  = { { SFX_SMART_CATCH }, SFX_SMART_SLIDE },
		[2]  = { { SFX_SMART_CATCH }, SFX_TRAIN_SLIDE },
		[3]  = { { SFX_METAL_SLIDE_OPEN }, SFX_METAL_SLIDE_LOOP },
		[4]  = { { SFX_HEAVY_SLIDE_OPEN }, SFX_HEAVY_SLIDE_LOOP },
		[5]  = { { SFX_WOOD_OPEN } },
		[6]  = { { SFX_TRAIN_SLIDE } },
		[7]  = { { SFX_TRAIN_CATCH }, SFX_WOOD_SLIDE },
		[8]  = { { SFX_WOOD_OPEN }, SFX_TRAIN_SLIDE },
		[9]  = { { 0 }, SFX_SHUTTER_OPEN },
		[10] = { { SFX_METAL_OPEN } },
		[11] = { { SFX_TRAIN_SLIDE } },
		[12] = { { SFX_METAL_OPEN3 } },
		[13] = { { SFX_TRAIN_SLIDE }, SFX_TRAIN_SLIDE },
		[14] = { { 0 }, SFX_HYDRAL_CLOSE },
		[15] = { { 0 }, SFX_STONE_OPEN },
		[16] = { { SFX_HEAVY_SLIDE_OPEN } },
		[17] = { { SFX_TRAIN_SLIDE, SFX_METAL_SLIDE_OPEN }, SFX_METAL_SLIDE_LOOP },
	},
	// the opening's, less the five that swing: a door that swings shut is
	// not heard until it closes
	[GESFX_DOOR_CLOSING] = {
		[1]  = { { SFX_SMART_CATCH }, SFX_SMART_SLIDE },
		[2]  = { { SFX_SMART_CATCH }, SFX_TRAIN_SLIDE },
		[3]  = { { SFX_METAL_SLIDE_OPEN }, SFX_METAL_SLIDE_LOOP },
		[4]  = { { SFX_HEAVY_SLIDE_OPEN }, SFX_HEAVY_SLIDE_LOOP },
		[7]  = { { SFX_TRAIN_CATCH }, SFX_WOOD_SLIDE },
		[8]  = { { SFX_WOOD_OPEN }, SFX_TRAIN_SLIDE },
		[9]  = { { 0 }, SFX_SHUTTER_OPEN },
		[13] = { { SFX_TRAIN_SLIDE }, SFX_TRAIN_SLIDE },
		[14] = { { 0 }, SFX_HYDRAL_CLOSE },
		[15] = { { 0 }, SFX_STONE_OPEN },
		[16] = { { SFX_HEAVY_SLIDE_OPEN } },
		[17] = { { SFX_TRAIN_SLIDE, SFX_METAL_SLIDE_OPEN }, SFX_METAL_SLIDE_LOOP },
	},
	[GESFX_DOOR_OPENED] = {
		[1]  = { { SFX_SMART_CATCH } },
		[2]  = { { SFX_SMART_CATCH } },
		[3]  = { { SFX_METAL_SLIDE_CLOSE } },
		[4]  = { { SFX_HEAVY_SLIDE_CLOSE } },
		[7]  = { { SFX_SMART_CATCH } },
		[8]  = { { SFX_WOOD_CLOSE } },
		[9]  = { { SFX_SHUTTER_CLOSE } },
		[13] = { { SFX_TRAIN_SLIDE } },
		[14] = { { SFX_HYDRAL_OPEN } },
		[15] = { { SFX_STONE_CLOSE } },
		[16] = { { SFX_HEAVY_SLIDE_CLOSE } },
		[17] = { { SFX_METAL_SLIDE_CLOSE } },
	},
	[GESFX_DOOR_CLOSED] = {
		[1]  = { { SFX_SMART_CATCH } },
		[2]  = { { SFX_SMART_CATCH } },
		[3]  = { { SFX_METAL_SLIDE_CLOSE } },
		[4]  = { { SFX_HEAVY_SLIDE_CLOSE } },
		[5]  = { { SFX_WOOD_CLOSE } },
		[6]  = { { SFX_TRAIN_SLIDE } },
		[7]  = { { SFX_SMART_CATCH } },
		[8]  = { { SFX_WOOD_CLOSE } },
		[9]  = { { SFX_SHUTTER_CLOSE } },
		[10] = { { SFX_METAL_CLOSE } },
		[11] = { { SFX_METAL_CLOSE2 } },
		[12] = { { SFX_METAL_CLOSE3 } },
		[13] = { { SFX_TRAIN_SLIDE } },
		[14] = { { SFX_HYDRAL_OPEN } },
		[15] = { { SFX_STONE_CLOSE } },
		[16] = { { SFX_HEAVY_SLIDE_CLOSE } },
		[17] = { { SFX_METAL_SLIDE_CLOSE } },
	},
};

static void geDoorPlaySample(s32 id, struct prop *prop, s32 type, u16 flags)
{
	s32 sound = sfxGetIn(&g_DoorBank, id);

	if (sound > 0) {
		psCreate(NULL, prop, sound, -1, -1, flags, 0, type, NULL, -1, NULL, -1, -1, -1, -1);
	}
}

bool geSfxDoor(s32 moment, s32 soundtype, struct prop *prop)
{
	if (!gelevelsIsStage(g_Vars.stagenum) || prop->type != PROPTYPE_DOOR || !geDoorBankLoad()) {
		return false;
	}

	if (soundtype > 0 && soundtype < GESFX_NUM_DOOR_TYPES) {
		for (s32 i = 0; i < 2; i++) {
			geDoorPlaySample(g_SfxDoors[moment][soundtype].once[i], prop, PSTYPE_GENERAL, PSFLAG_0400);
		}
		if (!g_Vars.in_cutscene) {
			geDoorPlaySample(g_SfxDoors[moment][soundtype].held, prop, PSTYPE_DOOR, 0);
		}
	}

	return true;
}
