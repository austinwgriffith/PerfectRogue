// MOD: ragdoll physics - sounds from WAV files (data/sounds/), played at a
// body and following it as the game's own sounds do. The game can only play
// sounds from the ROM, so these are mixed into the port's output
// (audio.c: audioSetNextBuffer -> rdWavMix) with the volume and pan the game
// works out for one of its sounds at the same place (psGetTheoreticalVolPan).
#include <math.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <SDL.h>
#include "rd_internal.h"
#include "lib/snd.h"
#include "game/lv.h"
#include "game/modrandom.h"
#include "fs.h"
#include "guardvoice.h"

#ifdef PD_PHYSICS

#define RD_WAV_RATE 22020          // the game's output rate (audio.c)
// Eight simultaneous custom voices leave room for nearby guards to react
// without cutting one another off. The original four still remain the normal
// case; this only matters during busy fights.
#define RD_WAV_MAXVOICES 8
#define RD_WAV_RAMP (1.0f / 441.0f) // gain changes spread over ~20 ms, so moving doesn't click

// Loudness, measured against the game's scream (SFX_M1_SCREAM) played the same
// way from the same place (the port's output dumped): the game's sounds come
// out at about twice vol x pan x sample, and that scream's loudest 0.1 s is
// at an RMS of about 0.36 of full scale. Each WAV is brought to that level
// when it's loaded (peaks above RD_WAV_KNEE eased under full scale), so it's
// as loud as the game's cries whatever level it was saved at.
#define RD_WAV_OUTGAIN 2.0f
#define RD_WAV_LOUDNESS 0.36f
#define RD_WAV_KNEE 0.8f

struct rdwav {
	f32 *samples;                   // mono, RD_WAV_RATE
	u32 numsamples;
	s32 loaded;                     // 0 = not yet, 1 = ok, -1 = failed
};

struct rdwavvoice {
	struct rdwav *wav;
	struct guardvoicepack *pack;
	bool reaction;
	u32 pos;                        // next sample
	struct prop *prop;
	struct chrdata *chr;
	s32 chrnum;
	bool countedtalk;
	struct coord at;                // where it is (the body, or where it was when the body went)
	RoomNum rooms[8];
	s16 likesound;                  // the game sound whose distances and volume it uses
	f32 gain[4], target[4];         // front left, front right, rear left, rear right
};

#define GUARDVOICE_DIR "$E/voice packs"
#define GUARDVOICE_NAME_SIZE 96
#define GUARDVOICE_VARIANTS 8

enum guardvoicecategory {
	GUARD_ATTACK, GUARD_DAMAGE, GUARD_GRENADE, GUARD_SIGHT, GUARD_GREETING,
	GUARD_SEARCH, GUARD_QUESTION, GUARD_SURPRISE, GUARD_ACKNOWLEDGE, GUARD_TAUNT,
	GUARD_CATEGORY_COUNT,
};

static const struct {
	const char *filename;
	const char *cici;
	s32 variants;
} g_GuardVoiceCategories[GUARD_CATEGORY_COUNT] = {
	[GUARD_ATTACK]      = { "attack%d.wav",      "rpg_attack%d.wav",    GUARDVOICE_VARIANTS },
	[GUARD_DAMAGE]      = { "damage%d.wav",      "rpg_damaged%d.wav",   GUARDVOICE_VARIANTS },
	[GUARD_GRENADE]     = { "grenade.wav",       "rpg_fire.wav",        1 },
	[GUARD_SIGHT]       = { "sight%d.wav",       "bright_hello%d.wav",  GUARDVOICE_VARIANTS },
	[GUARD_GREETING]    = { "greeting%d.wav",    "bright_hey%d.wav",    GUARDVOICE_VARIANTS },
	[GUARD_SEARCH]      = { "search%d.wav",      "bright_what%d.wav",   GUARDVOICE_VARIANTS },
	[GUARD_QUESTION]    = { "question%d.wav",    "bright_why%d.wav",    GUARDVOICE_VARIANTS },
	[GUARD_SURPRISE]    = { "surprise%d.wav",    "bright_gasp%d.wav",   GUARDVOICE_VARIANTS },
	[GUARD_ACKNOWLEDGE] = { "acknowledge%d.wav", "bright_okay%d.wav",   GUARDVOICE_VARIANTS },
	[GUARD_TAUNT]       = { "taunt%d.wav",       "bright_laugh%d.wav",  GUARDVOICE_VARIANTS },
};

struct guardvoicepack {
	char name[GUARDVOICE_NAME_SIZE];
	struct rdwav clips[GUARD_CATEGORY_COUNT][GUARDVOICE_VARIANTS];
};

static const char g_GuardVoiceVanilla[] = "vanilla";
static const char g_GuardVoiceCici[] = "Cici";
static struct guardvoicepack g_GuardVoicePacks[GUARDVOICE_NUM_SEXES];
static char g_GuardVoiceChoices[GUARDVOICE_NUM_SEXES][GUARDVOICE_NAME_SIZE];
static char (*g_GuardVoiceCustomNames)[GUARDVOICE_NAME_SIZE];
static s32 g_GuardVoiceCustomCount;
static s32 g_GuardVoiceLegacyCici = 0;
static s32 g_GuardChatterNext60;
static struct rdwav g_RdScream;
static struct rdwavvoice g_RdWavVoices[RD_WAV_MAXVOICES];

static void rdWavFinish(struct rdwavvoice *v)
{
	if (v->countedtalk && v->prop && v->prop->type == PROPTYPE_CHR
			&& v->prop->chr == v->chr && v->chr->chrnum == v->chrnum
			&& v->chr->propsoundcount > 0) v->chr->propsoundcount--;
	v->countedtalk = false;
	v->wav = NULL;
}

static void rdWavLoad(struct rdwav *wav, const char *path, bool optional)
{
	u32 size = 0;
	void *data;
	SDL_RWops *rw;
	SDL_AudioSpec spec;
	Uint8 *pcm = NULL;
	Uint32 pcmlen = 0;
	SDL_AudioStream *stream;
	s32 got;

	wav->loaded = -1;
	data = fsFileSize(path) > 0 ? fsFileLoad(path, &size) : NULL;

	if (data == NULL) {
		if (!optional) sysLogPrintf(LOG_WARNING, "RAGDOLL: %s not found, that sound is left out", path);
		return;
	}

	rw = SDL_RWFromConstMem(data, size);

	if (rw == NULL || SDL_LoadWAV_RW(rw, 1, &spec, &pcm, &pcmlen) == NULL) {
		sysLogPrintf(LOG_WARNING, "RAGDOLL: can't read %s: %s", path, SDL_GetError());
		sysMemFree(data);
		return;
	}

	sysMemFree(data);

	// Down to mono at the game's rate
	stream = SDL_NewAudioStream(spec.format, spec.channels, spec.freq, AUDIO_F32SYS, 1, RD_WAV_RATE);

	if (stream && SDL_AudioStreamPut(stream, pcm, pcmlen) == 0 && SDL_AudioStreamFlush(stream) == 0) {
		got = SDL_AudioStreamAvailable(stream);
		wav->samples = malloc(got > 0 ? got : 4);

		if (wav->samples) {
			got = SDL_AudioStreamGet(stream, wav->samples, got);

			if (got > 0) {
				wav->numsamples = got / sizeof(f32);
				wav->loaded = 1;
			}
		}
	}

	if (stream) {
		SDL_FreeAudioStream(stream);
	}

	SDL_FreeWAV(pcm);

	if (wav->loaded == 1) {
		// To the game's loudness (see the top): its loudest 0.1 s
		u32 win = RD_WAV_RATE / 10, i;
		f64 sum = 0.0, loudest = 0.0;
		f32 gain;

		for (i = 0; i < wav->numsamples; i++) {
			sum += wav->samples[i] * wav->samples[i];

			if (i >= win) {
				sum -= wav->samples[i - win] * wav->samples[i - win];
			}

			if (sum > loudest) {
				loudest = sum;
			}
		}

		loudest = sqrt(loudest / MIN(win, wav->numsamples));
		gain = loudest > 0.0001 ? RD_WAV_LOUDNESS / loudest : 1.0f;

		for (i = 0; i < wav->numsamples; i++) {
			f32 v = wav->samples[i] * gain;
			f32 a = fabsf(v);

			if (a > RD_WAV_KNEE) {
				a = RD_WAV_KNEE + (1.0f - RD_WAV_KNEE) * tanhf((a - RD_WAV_KNEE) / (1.0f - RD_WAV_KNEE));
				v = v < 0.0f ? -a : a;
			}

			wav->samples[i] = v;
		}

		sysLogPrintf(LOG_NOTE, "RAGDOLL: loaded %s (%.2f s, level x%.2f)", path, wav->numsamples / (f32) RD_WAV_RATE, gain);
	} else {
		sysLogPrintf(LOG_WARNING, "RAGDOLL: can't convert %s: %s", path, SDL_GetError());
		free(wav->samples);
		wav->samples = NULL;
	}
}

bool rdWavAvailable(s32 which)
{
	if (which != RDWAV_SCREAM) return false;
	if (!g_RdScream.loaded) rdWavLoad(&g_RdScream, "sounds/scream.wav", false);
	return g_RdScream.loaded == 1;
}

/**
 * The gains for where the voice is: the game's volume and pan for likesound
 * there (distance, rooms and portals), its volume setting for that sound,
 * the Sound Effects volume, and the pan and front/rear as naudio applies them.
 */
static void rdWavVoiceAim(struct rdwavvoice *v)
{
	s32 vol = 0, pan = AL_PAN_CENTER;
	union soundnumhack ref;
	f32 amp, p, l, r;
	bool rear;
	s32 i;

	if (v->prop && v->prop->type == PROPTYPE_CHR && v->prop->chr == v->chr && v->chr->chrnum == v->chrnum) {
		v->at = v->prop->pos;
		memcpy(v->rooms, v->prop->rooms, sizeof(v->rooms));
	} else {
		v->prop = NULL; // the body is gone: it carries on where it was
	}

	psGetTheoreticalVolPan(&v->at, v->rooms, v->likesound, &vol, &pan);

	ref.packed = v->likesound;

	if (ref.hasconfig) {
		struct audioconfig *config = &g_AudioConfigs[g_AudioRussMappings[ref.confignum].audioconfig_index];

		vol = vol * config->volpercentage / 100;

		if (config->pan != -1) {
			pan = config->pan;
		}
	}

	amp = RD_WAV_OUTGAIN * vol / AL_VOL_FULL * snd0000e9dc() / AL_VOL_FULL;
	if (v->pack) amp *= 0.65f;
	rear = g_SoundMode == SOUNDMODE_SURROUND && (pan & 0x80);
	p = (pan & 0x7f) / 127.0f;
	l = amp * cosf(p * (f32) (M_PI / 2));
	r = amp * sinf(p * (f32) (M_PI / 2));

	for (i = 0; i < 4; i++) {
		v->target[i] = 0.0f;
	}

	v->target[rear ? 2 : 0] = l;
	v->target[rear ? 3 : 1] = r;
}

static struct rdwavvoice *rdWavPlayInternal(struct rdwav *wav, struct prop *prop, s16 likesound,
		struct guardvoicepack *pack, bool reaction)
{
	struct rdwavvoice *v = NULL;
	s32 i;

	// A free voice, or the one furthest through its sound
	for (i = 0; i < RD_WAV_MAXVOICES; i++) {
		struct rdwavvoice *c = &g_RdWavVoices[i];

		if (c->wav == NULL) {
			v = c;
			break;
		}

		if (v == NULL || c->pos > v->pos) {
			v = c;
		}
	}

	rdWavFinish(v);
	memset(v, 0, sizeof(*v));
	v->wav = wav;
	v->pack = pack;
	v->reaction = reaction;
	v->prop = prop;
	v->chr = prop->chr;
	v->chrnum = prop->chr->chrnum;
	v->likesound = likesound;
	v->at = prop->pos;
	memcpy(v->rooms, prop->rooms, sizeof(v->rooms));
	rdWavVoiceAim(v);

	for (i = 0; i < 4; i++) {
		v->gain[i] = v->target[i];
	}
	return v;
}

void rdWavPlay(s32 which, struct prop *prop, s16 likesound)
{
	if (rdWavAvailable(which)) rdWavPlayInternal(&g_RdScream, prop, likesound, NULL, false);
}

static bool guardVoiceCustomNameValid(const char *name)
{
	const size_t len = strlen(name);
	return len > 0 && len < GUARDVOICE_NAME_SIZE && name[0] != '.' && name[0] != ' '
		&& name[len - 1] != ' ' && strpbrk(name, "/\\\":\r\n\t") == NULL
		&& strcasecmp(name, g_GuardVoiceVanilla) && strcasecmp(name, g_GuardVoiceCici);
}

static int guardVoiceCompareNames(const void *a, const void *b)
{
	const int order = strcasecmp(a, b);
	return order ? order : strcmp(a, b);
}

static void guardVoiceUsePack(enum guardvoicesex sex, const char *name)
{
	struct guardvoicepack *pack = &g_GuardVoicePacks[sex];
	if (strcmp(pack->name, name) == 0) return;

	for (s32 i = 0; i < ARRAYCOUNT(g_RdWavVoices); i++) {
		if (g_RdWavVoices[i].wav && g_RdWavVoices[i].pack == pack) {
			rdWavFinish(&g_RdWavVoices[i]);
		}
	}
	for (s32 category = 0; category < GUARD_CATEGORY_COUNT; category++) {
		for (s32 clip = 0; clip < GUARDVOICE_VARIANTS; clip++) {
			free(pack->clips[category][clip].samples);
		}
	}
	memset(pack->clips, 0, sizeof(pack->clips));
	snprintf(pack->name, sizeof(pack->name), "%s", name);
}

void guardVoiceRefreshPacks(void)
{
	g_GuardVoiceCustomCount = 0;
	DIR *dir = opendir(fsFullPath(GUARDVOICE_DIR));
	if (dir) {
		struct dirent *entry;
		while ((entry = readdir(dir)) != NULL) {
			if (!guardVoiceCustomNameValid(entry->d_name)) continue;

			char path[FS_MAXPATH];
			struct stat st;
			snprintf(path, sizeof(path), GUARDVOICE_DIR "/%s", entry->d_name);
			if (stat(fsFullPath(path), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

			void *names = realloc(g_GuardVoiceCustomNames,
					(g_GuardVoiceCustomCount + 1) * sizeof(*g_GuardVoiceCustomNames));
			if (!names) break;
			g_GuardVoiceCustomNames = names;
			snprintf(g_GuardVoiceCustomNames[g_GuardVoiceCustomCount++], GUARDVOICE_NAME_SIZE, "%s", entry->d_name);
		}
		closedir(dir);
	}
	if (g_GuardVoiceCustomCount > 1) {
		qsort(g_GuardVoiceCustomNames, g_GuardVoiceCustomCount, sizeof(*g_GuardVoiceCustomNames), guardVoiceCompareNames);
	}

	for (enum guardvoicesex sex = GUARDVOICE_MALE; sex < GUARDVOICE_NUM_SEXES; sex++) {
		const char *saved = g_GuardVoiceChoices[sex];
		const char *name = g_GuardVoiceVanilla;
		if (sex == GUARDVOICE_FEMALE && strcasecmp(saved, g_GuardVoiceCici) == 0) {
			name = g_GuardVoiceCici;
		}
		for (s32 i = 0; i < g_GuardVoiceCustomCount; i++) {
			if (strcmp(saved, g_GuardVoiceCustomNames[i]) == 0) {
				name = g_GuardVoiceCustomNames[i];
				break;
			}
		}
		guardVoiceUsePack(sex, name);
	}
}

void guardVoiceInit(void)
{
	for (enum guardvoicesex sex = GUARDVOICE_MALE; sex < GUARDVOICE_NUM_SEXES; sex++) {
		if (!g_GuardVoiceChoices[sex][0]) {
			snprintf(g_GuardVoiceChoices[sex], sizeof(g_GuardVoiceChoices[sex]), "%s",
					sex == GUARDVOICE_FEMALE && g_GuardVoiceLegacyCici ? g_GuardVoiceCici : g_GuardVoiceVanilla);
		}
	}
	fsCreateDir(GUARDVOICE_DIR);
	guardVoiceRefreshPacks();
}

s32 guardVoicePackCount(enum guardvoicesex sex)
{
	return 1 + (sex == GUARDVOICE_FEMALE) + g_GuardVoiceCustomCount;
}

const char *guardVoicePackName(enum guardvoicesex sex, s32 index)
{
	if (index == 0) return g_GuardVoiceVanilla;
	if (sex == GUARDVOICE_FEMALE && index == 1) return g_GuardVoiceCici;

	index -= 1 + (sex == GUARDVOICE_FEMALE);
	return index >= 0 && index < g_GuardVoiceCustomCount ? g_GuardVoiceCustomNames[index] : NULL;
}

s32 guardVoicePackSelected(enum guardvoicesex sex)
{
	for (s32 i = 1; i < guardVoicePackCount(sex); i++) {
		if (strcmp(g_GuardVoicePacks[sex].name, guardVoicePackName(sex, i)) == 0) return i;
	}
	return 0;
}

void guardVoicePackSelect(enum guardvoicesex sex, s32 index)
{
	const char *name = guardVoicePackName(sex, index);
	if (!name) return;
	snprintf(g_GuardVoiceChoices[sex], sizeof(g_GuardVoiceChoices[sex]), "%s", name);
	guardVoiceUsePack(sex, name);
}

static struct guardvoicepack *guardVoiceForChr(struct chrdata *chr, bool reaction)
{
	if (!chr || !chr->prop || chr->prop->type != PROPTYPE_CHR || CHRRACE(chr) != RACE_HUMAN) return NULL;

	struct guardvoicepack *pack = &g_GuardVoicePacks[chr->voicebox == VOICEBOX_FEMALE ? GUARDVOICE_FEMALE : GUARDVOICE_MALE];
	if (!pack->name[0] || strcmp(pack->name, g_GuardVoiceVanilla) == 0) return NULL;
	if ((chr->team & (TEAM_ALLY | TEAM_NONCOMBAT))
			&& !(reaction && modRandomIsOn() && modRandomIsCivilianChr(chr))) return NULL;

	return pack;
}

static struct rdwav *guardVoiceFindClip(struct guardvoicepack *pack, enum guardvoicecategory first, s32 count, s32 start)
{
	const bool cici = strcmp(pack->name, g_GuardVoiceCici) == 0;
	const s32 variants = g_GuardVoiceCategories[first].variants;
	for (s32 i = 0; i < count * variants; i++) {
		const s32 slot = (i + start) % (count * variants);
		const s32 category = first + slot / variants;
		const s32 variant = slot % variants;
		struct rdwav *wav = &pack->clips[category][variant];
		if (!wav->loaded) {
			char file[40];
			char path[FS_MAXPATH];
			snprintf(file, sizeof(file), cici ? g_GuardVoiceCategories[category].cici
					: g_GuardVoiceCategories[category].filename, variant + 1);
			if (cici) {
				snprintf(path, sizeof(path), "sounds/guards/cici/%s", file);
			} else {
				snprintf(path, sizeof(path), GUARDVOICE_DIR "/%s/%s", pack->name, file);
			}
			rdWavLoad(wav, path, !cici);
		}
		if (wav->loaded == 1) return wav;
	}
	return NULL;
}

static bool guardVoicePlay(struct chrdata *chr, struct guardvoicepack *pack, enum guardvoicecategory first,
		s32 count, bool reaction)
{
	const s32 start = rngRandom() % (count * g_GuardVoiceCategories[first].variants);
	struct rdwav *wav = guardVoiceFindClip(pack, first, count, start);
	if (!wav) return false;

	for (s32 i = 0; i < RD_WAV_MAXVOICES; i++) {
		struct rdwavvoice *v = &g_RdWavVoices[i];
		if (v->wav && v->prop == chr->prop && v->chrnum == chr->chrnum) {
			// Pain interrupts chatter, but repeated pellet hits do not restart a cry.
			if (!reaction || (v->reaction && v->pos < RD_WAV_RATE / 4)) return true;
			rdWavFinish(v);
		}
	}
	psStopSound(chr->prop, PSTYPE_CHRTALK, 0);
	psStopSound(chr->prop, PSTYPE_CHRCHOKE, 0);
	const s16 likesound = chr->voicebox == VOICEBOX_FEMALE ? SFX_ARGH_FEMALE_000D : SFX_ARGH_MALE_0086;
	struct rdwavvoice *v = rdWavPlayInternal(wav, chr->prop, likesound, pack, reaction);
	v->countedtalk = true;
	chr->propsoundcount++;
	return true;
}

bool guardVoiceTryReaction(struct chrdata *chr)
{
	struct guardvoicepack *pack = guardVoiceForChr(chr, true);
	return pack && guardVoicePlay(chr, pack, GUARD_DAMAGE, 1, true);
}

bool guardVoiceTryQuip(struct chrdata *chr, s32 quip, s32 probability, s32 gap)
{
	struct guardvoicepack *pack = guardVoiceForChr(chr, false);
	if (!pack) return false;

	enum guardvoicecategory first = GUARD_SEARCH;
	s32 count = 2;
	switch (quip) {
	case QUIP_ATTACK1: case QUIP_ATTACK2: case QUIP_ATTACK3: case QUIP_ATTACK4:
		first = GUARD_ATTACK; count = 1; break;
	case QUIP_SEEPLAYER: case QUIP_SEARCHSUCCESS: case QUIP_GREETING:
	case QUIP_UNCOVERDISGUISE1: case QUIP_UNCOVEREDDISGUISE2:
		first = GUARD_SIGHT; break;
	case QUIP_SHOTUNALERT: case QUIP_INJURED1: case QUIP_INJURED2: case QUIP_DIE:
		first = GUARD_DAMAGE; count = 1; break;
	case QUIP_GRENADE1: case QUIP_GRENADE2: case QUIP_GOFORALARM:
		first = GUARD_GRENADE; count = 1; break;
	case QUIP_SURPRISED: case QUIP_WARNFRIENDS: case QUIP_INSPECTBODY:
		first = GUARD_SURPRISE; count = 1; break;
	case QUIP_GUNJAMMED: case QUIP_LOSTGUN: case QUIP_GOTOCOVER1: case QUIP_GOTOCOVER2:
		first = GUARD_ACKNOWLEDGE; count = 1; break;
	case QUIP_HITPLAYER: case QUIP_KILLEDPLAYER1: case QUIP_KILLEDPLAYER2:
		first = GUARD_TAUNT; count = 1; break;
	}
	if (!guardVoiceFindClip(pack, first, count, 0)) return false;
	if (g_Vars.lvframe60 < g_GuardChatterNext60
			|| (chr->soundgap && chr->soundtimer < chr->soundgap * TICKS(60))
			|| rngRandom() % 256 >= probability) return true;
	if (guardVoicePlay(chr, pack, first, count, false)) {
		chr->soundtimer = 0;
		chr->soundgap = gap;
		g_GuardChatterNext60 = g_Vars.lvframe60 + TICKS(60);
		return true;
	}
	return false;
}

PD_CONSTRUCTOR static void guardVoiceConfigInit(void)
{
	configRegisterInt("Audio.CiciGuardVoices", &g_GuardVoiceLegacyCici, 0, 1);
	configRegisterString("Audio.MaleGuardVoicePack", g_GuardVoiceChoices[GUARDVOICE_MALE], GUARDVOICE_NAME_SIZE);
	configRegisterString("Audio.FemaleGuardVoicePack", g_GuardVoiceChoices[GUARDVOICE_FEMALE], GUARDVOICE_NAME_SIZE);
}

/**
 * Each tick: follow the bodies
 */
void rdWavTick(void)
{
	s32 i;

	for (i = 0; i < RD_WAV_MAXVOICES; i++) {
		if (g_RdWavVoices[i].wav) {
			rdWavVoiceAim(&g_RdWavVoices[i]);
		}
	}
}

void rdWavStopAll(void)
{
	s32 i;
	for (i = 0; i < RD_WAV_MAXVOICES; i++) rdWavFinish(&g_RdWavVoices[i]);
	memset(g_RdWavVoices, 0, sizeof(g_RdWavVoices));
	g_GuardChatterNext60 = 0;
}

s32 rdWavActive(void)
{
	s32 i;

	for (i = 0; i < RD_WAV_MAXVOICES; i++) {
		if (g_RdWavVoices[i].wav) {
			return true;
		}
	}

	return false;
}

static s16 rdWavClamp(f32 v)
{
	return v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (s16) v;
}

/**
 * Adds the voices to the output: front (stereo) and, with 5.1, rear
 * (stereo). With stereo output the rear part goes in the front, out of phase
 * on the right for Dolby Surround as the mixer does; with plain stereo there
 * is no rear part.
 */
void rdWavMix(s16 *front, s16 *rear, u32 frames, s32 dolby)
{
	s32 i, k;
	u32 f;

	if (lvIsPaused()) {
		return; // as the game's sounds, held while paused
	}

	for (i = 0; i < RD_WAV_MAXVOICES; i++) {
		struct rdwavvoice *v = &g_RdWavVoices[i];

		if (v->wav == NULL) {
			continue;
		}

		for (f = 0; f < frames && v->pos < v->wav->numsamples; f++, v->pos++) {
			f32 s = v->wav->samples[v->pos] * 32767.0f;
			f32 rl, rr;

			for (k = 0; k < 4; k++) {
				if (v->gain[k] < v->target[k]) {
					v->gain[k] = MIN(v->gain[k] + RD_WAV_RAMP, v->target[k]);
				} else if (v->gain[k] > v->target[k]) {
					v->gain[k] = MAX(v->gain[k] - RD_WAV_RAMP, v->target[k]);
				}
			}

			rl = s * v->gain[2];
			rr = s * v->gain[3];

			front[f * 2 + 0] = rdWavClamp(front[f * 2 + 0] + s * v->gain[0]);
			front[f * 2 + 1] = rdWavClamp(front[f * 2 + 1] + s * v->gain[1]);

			if (rear) {
				rear[f * 2 + 0] = rdWavClamp(rear[f * 2 + 0] + rl);
				rear[f * 2 + 1] = rdWavClamp(rear[f * 2 + 1] + rr);
			} else if (dolby) {
				front[f * 2 + 0] = rdWavClamp(front[f * 2 + 0] + rl);
				front[f * 2 + 1] = rdWavClamp(front[f * 2 + 1] - rr);
			} else {
				front[f * 2 + 0] = rdWavClamp(front[f * 2 + 0] + rl);
				front[f * 2 + 1] = rdWavClamp(front[f * 2 + 1] + rr);
			}
		}

		if (v->pos >= v->wav->numsamples) {
			rdWavFinish(v);
		}
	}
}

#endif // PD_PHYSICS
