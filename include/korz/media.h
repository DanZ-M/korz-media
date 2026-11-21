/* korz-media: estimated host volume. The value is counted, not read: see README. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <zmk/event_manager.h>

struct korz_media_changed {
	uint8_t volume; /* 0..100 */
	bool muted;
};

ZMK_EVENT_DECLARE(korz_media_changed);

uint8_t korz_media_volume(void);
bool korz_media_muted(void);
