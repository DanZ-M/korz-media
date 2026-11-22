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

/* move the estimate by one host step (dir +1 / -1) and tell the others if it changed */
static void step(int dir)
{
	int v = CLAMP((int)volume + dir * CONFIG_KORZ_MEDIA_VOLUME_STEP, 0, 100);
	/* the clamp is the re-sync: enough presses always land on 0 / 100 */

	if (v != volume || muted) {
		volume = (uint8_t)v;
		muted = false; /* the hosts unmute on a volume key */
		announce();
	}
}

/* The keyboard sends one press; the host repeats it by itself while the key is held.
 * Do the same here, with the host's repeat delay and rate. */
static int held_dir;
static uint32_t held_key;

static void repeat_fn(struct k_work *work)
{
	if (held_dir != 0) {
		step(held_dir);
		k_work_reschedule(k_work_delayable_from_work(work),
				  K_MSEC(CONFIG_KORZ_MEDIA_REPEAT_MS));
	}
}

static K_WORK_DELAYABLE_DEFINE(repeat_work, repeat_fn);

static int on_key(const zmk_event_t *eh)
{
	const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);

	if (ev == NULL || ev->usage_page != HID_USAGE_CONSUMER) {
		return ZMK_EV_EVENT_BUBBLE;
	}

	if (!ev->state) {
		if (ev->keycode == held_key && held_dir != 0) {
			held_dir = 0;
			k_work_cancel_delayable(&repeat_work);
		}
		return ZMK_EV_EVENT_BUBBLE;
	}

	switch (ev->keycode) {
	case HID_USAGE_CONSUMER_VOLUME_INCREMENT:
	case HID_USAGE_CONSUMER_VOLUME_DECREMENT:
		held_dir = ev->keycode == HID_USAGE_CONSUMER_VOLUME_INCREMENT ? 1 : -1;
		held_key = ev->keycode;
		step(held_dir);
		k_work_reschedule(&repeat_work, K_MSEC(CONFIG_KORZ_MEDIA_REPEAT_DELAY_MS));
		break;
	case HID_USAGE_CONSUMER_MUTE:
		muted = !muted;
		announce();
		break;
	default:
		break;
	}
	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(korz_media_volume, on_key);
ZMK_SUBSCRIPTION(korz_media_volume, zmk_keycode_state_changed);
