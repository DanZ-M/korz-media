/* korz-media: estimated host volume. The value is counted, not read: see README. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <zmk/event_manager.h>

enum korz_media_learn {
	KORZ_MEDIA_LEARN_OFF = 0,
	KORZ_MEDIA_LEARN_DOWN = 1,  /* going down to 0 % */
	KORZ_MEDIA_LEARN_UP = 2,    /* counting the steps up to 100 % */
	KORZ_MEDIA_LEARN_SAVED = 3, /* just saved (for about 2 s) */
};

struct korz_media_changed {
	uint8_t volume; /* 0..100 */
	bool muted;
	uint8_t learn; /* enum korz_media_learn */
};

ZMK_EVENT_DECLARE(korz_media_changed);

uint8_t korz_media_volume(void);
bool korz_media_muted(void);
enum korz_media_learn korz_media_learn_stage(void);
