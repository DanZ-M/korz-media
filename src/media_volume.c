/*
 * korz-media: estimated host volume, counted from the volume keys the keyboard sends.
 * Every key that reaches the host passes through ZMK's keycode event, so keys from the
 * keymap, macros, and later the gesture engine are all counted the same way.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <korz/media.h>

ZMK_EVENT_IMPL(korz_media_changed);

static uint8_t volume = CONFIG_KORZ_MEDIA_VOLUME_START;
static bool muted;

uint8_t korz_media_volume(void) { return volume; }
bool korz_media_muted(void) { return muted; }

static void announce(void)
{
	raise_korz_media_changed((struct korz_media_changed){.volume = volume, .muted = muted});
}

static int on_key(const zmk_event_t *eh)
{
	const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
	int v = volume;
	bool m = muted;

	if (ev == NULL || !ev->state || ev->usage_page != HID_USAGE_CONSUMER) {
		return ZMK_EV_EVENT_BUBBLE;
	}

	switch (ev->keycode) {
	case HID_USAGE_CONSUMER_VOLUME_INCREMENT:
		v += CONFIG_KORZ_MEDIA_VOLUME_STEP;
		m = false; /* the hosts unmute on a volume key */
		break;
	case HID_USAGE_CONSUMER_VOLUME_DECREMENT:
		v -= CONFIG_KORZ_MEDIA_VOLUME_STEP;
		m = false;
		break;
	case HID_USAGE_CONSUMER_MUTE:
		m = !m;
		break;
	default:
		return ZMK_EV_EVENT_BUBBLE;
	}

	v = CLAMP(v, 0, 100); /* the clamp is the re-sync: enough presses always land on 0 / 100 */
	if (v != volume || m != muted) {
		volume = (uint8_t)v;
		muted = m;
		announce();
	}
	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(korz_media_volume, on_key);
ZMK_SUBSCRIPTION(korz_media_volume, zmk_keycode_state_changed);
