#ifndef PD_GUARDVOICE_H
#define PD_GUARDVOICE_H
#include <PR/ultratypes.h>
#include "types.h"
struct chrdata;
enum guardvoicesex {
	GUARDVOICE_MALE, GUARDVOICE_FEMALE, GUARDVOICE_NUM_SEXES
};
void guardVoiceInit(void);
void guardVoiceRefreshPacks(void);
s32 guardVoicePackCount(enum guardvoicesex sex);
const char *guardVoicePackName(enum guardvoicesex sex, s32 index);
s32 guardVoicePackSelected(enum guardvoicesex sex);
void guardVoicePackSelect(enum guardvoicesex sex, s32 index);
enum guardvoiceevent {
	GUARDVOICE_SIGHT, GUARDVOICE_ATTACK, GUARDVOICE_PAIN, GUARDVOICE_DEATH,
	GUARDVOICE_SURPRISE, GUARDVOICE_SEARCH, GUARDVOICE_ALLYDOWN,
	GUARDVOICE_RELOAD, GUARDVOICE_TAUNT
};
bool guardVoiceTryQuip(struct chrdata *chr, s32 quip, s32 probability, s32 gap);
bool guardVoiceTryReaction(struct chrdata *chr);
#endif
