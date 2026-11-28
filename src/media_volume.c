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
#include <zmk/endpoints.h>
#include <zephyr/settings/settings.h>
#include <stdlib.h>
#include <string.h>
#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <korz/media.h>

ZMK_EVENT_IMPL(korz_media_changed);

#define DEFAULT_STEPS (100 / CONFIG_KORZ_MEDIA_VOLUME_STEP)

static uint8_t volume = CONFIG_KORZ_MEDIA_VOLUME_START;
static bool muted;
/* host steps from 0 % to 100 %, one value per endpoint (USB, each Bluetooth profile) */
static uint8_t steps[ZMK_ENDPOINT_COUNT] = {[0 ... ZMK_ENDPOINT_COUNT - 1] = DEFAULT_STEPS};
/* host key repeat (delay before it starts, ms between repeats), per endpoint */
static uint16_t rep_delay[ZMK_ENDPOINT_COUNT] = {
	[0 ... ZMK_ENDPOINT_COUNT - 1] = CONFIG_KORZ_MEDIA_REPEAT_DELAY_MS};
static uint16_t rep_period[ZMK_ENDPOINT_COUNT] = {
	[0 ... ZMK_ENDPOINT_COUNT - 1] = CONFIG_KORZ_MEDIA_REPEAT_MS};
static enum korz_media_learn learn_stage = KORZ_MEDIA_LEARN_OFF;
static int learn_count;

uint8_t korz_media_volume(void) { return volume; }
bool korz_media_muted(void) { return muted; }
enum korz_media_learn korz_media_learn_stage(void) { return learn_stage; }

static void announce(void)
{
	LOG_INF("korz-media: volume %d%% muted %d learn %d", volume, muted, learn_stage);
	raise_korz_media_changed((struct korz_media_changed){
		.volume = volume, .muted = muted, .learn = learn_stage});
}

static int ep_index(void) { return zmk_endpoint_instance_to_index(zmk_endpoint_get_selected()); }

/* move the estimate by one host step (dir +1 / -1) and tell the others if it changed */
static void step(int dir)
{
	int n = steps[ep_index()];
	int pos = CLAMP(((int)volume * n + 50) / 100 + dir, 0, n);
	int v = pos * 100 / n;
	/* the clamp is the re-sync: enough presses always land on 0 / 100 */

	if (v != volume || muted) {
		volume = (uint8_t)v;
		muted = false; /* the hosts unmute on a volume key */
		announce();
	}
}

#if IS_ENABLED(CONFIG_KORZ_MEDIA_LEARN)
/* ---- learning: how many host steps from 0 % to 100 % (this device)  ----
 * Hold mute to start. Stage 1: press volume down until the computer / phone is at 0 %.
 * Stage 2: press volume up, one press per step, until it is at 100 %.
 * Then press volume down once: that ends it and the count is saved for this connection. */
static void learn_set_stage(enum korz_media_learn st)
{
	learn_stage = st;
	LOG_INF("korz-media: learning stage %d", st);
	announce();
}

static void learn_timeout_fn(struct k_work *work)
{
	if (learn_stage != KORZ_MEDIA_LEARN_OFF) {
		learn_set_stage(KORZ_MEDIA_LEARN_OFF);
	}
}

static K_WORK_DELAYABLE_DEFINE(learn_timeout_work, learn_timeout_fn);

static void learn_hold_fn(struct k_work *work)
{
	if (learn_stage == KORZ_MEDIA_LEARN_OFF) {
		volume = 100; /* the lights and the bar start full and shrink as it is tapped down */
		muted = false;
		learn_set_stage(KORZ_MEDIA_LEARN_DOWN);
		k_work_reschedule(&learn_timeout_work, K_SECONDS(CONFIG_KORZ_MEDIA_LEARN_TIMEOUT_S));
	} else {
		learn_set_stage(KORZ_MEDIA_LEARN_OFF);
	}
}

static K_WORK_DELAYABLE_DEFINE(learn_hold_work, learn_hold_fn);

static void learn_name(char *buf, size_t len, const char *suffix)
{
	struct zmk_endpoint_instance e = zmk_endpoint_get_selected();

	if (e.transport == ZMK_TRANSPORT_BLE) {
		snprintk(buf, len, "korz_media/ble%d%s", e.ble.profile_index, suffix);
	} else {
		snprintk(buf, len, "korz_media/usb%s", suffix);
	}
}

static int learn_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	struct zmk_endpoint_instance e = {.transport = ZMK_TRANSPORT_USB};
	bool rep = strstr(name, "_rep") != NULL;

	if (strncmp(name, "ble", 3) == 0) {
		e.transport = ZMK_TRANSPORT_BLE;
		e.ble.profile_index = atoi(name + 3);
		if (e.ble.profile_index < 0 || e.ble.profile_index >= ZMK_BLE_PROFILE_COUNT) {
			return -EINVAL;
		}
	} else if (strncmp(name, "usb", 3) != 0) {
		return -ENOENT;
	}
	int idx = zmk_endpoint_instance_to_index(e);

	if (rep) {
		uint16_t v[2];

		if (len != sizeof(v) || read_cb(cb_arg, v, sizeof(v)) <= 0 || v[0] > 2000 ||
		    v[1] < 10 || v[1] > 500) {
			return -EINVAL;
		}
		rep_delay[idx] = v[0];
		rep_period[idx] = v[1];
		return 0;
	}
	uint8_t n;

	if (len != sizeof(n) || read_cb(cb_arg, &n, sizeof(n)) <= 0 || n < 2 || n > 100) {
		return -EINVAL;
	}
	steps[idx] = n;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(korz_media, "korz_media", NULL, learn_set, NULL, NULL);

static void learn_done_fn(struct k_work *work)
{
	learn_set_stage(KORZ_MEDIA_LEARN_OFF);
}

static K_WORK_DELAYABLE_DEFINE(learn_done_work, learn_done_fn);

/* a volume key arrives while learning */
static void learn_key(int dir)
{
	k_work_reschedule(&learn_timeout_work, K_SECONDS(CONFIG_KORZ_MEDIA_LEARN_TIMEOUT_S));

	if (learn_stage == KORZ_MEDIA_LEARN_DOWN && dir > 0) {
		volume = 0; /* the computer is at 0 % now */
		learn_count = 0;
		learn_set_stage(KORZ_MEDIA_LEARN_UP);
	}
	if (learn_stage == KORZ_MEDIA_LEARN_UP) {
		if (dir > 0) {
			learn_count++;
			volume = (uint8_t)MIN(100, learn_count * 100 / DEFAULT_STEPS);
			muted = false;
			announce();
		} else if (learn_count >= 2) {
			char name[24];
			uint8_t n = (uint8_t)MIN(learn_count, 100);

			steps[ep_index()] = n;
			learn_name(name, sizeof(name), "");
			settings_save_one(name, &n, sizeof(n));
			LOG_INF("korz-media: %s = %d steps from 0 to 100 %%", name, n);
			volume = 100; /* then the volume-down that ended it moves one step */
			step(-1);
			/* next: how fast the host repeats a held key (hold mute 3 s to skip it) */
			learn_set_stage(KORZ_MEDIA_LEARN_HOLD1);
		} else {
			learn_set_stage(KORZ_MEDIA_LEARN_OFF); /* too few presses: nothing saved */
		}
		return;
	}
	step(dir);
}
/* ---- learning the host's key repeat (two holds of different length) ----
 * Host at 0 %: hold volume up for a while, release (the keyboard times it, T), tap volume down
 * until the host is at 0 again counting the taps (S, press mute when it is there). Do it twice
 * with a different hold time: S = 1 + (T - delay) / period, so two of them give both. */
static int64_t hold_t0;
static int32_t rep_t1, rep_s1, rep_t2;

static bool rep_hold_stage(void)
{
	return learn_stage == KORZ_MEDIA_LEARN_HOLD1 || learn_stage == KORZ_MEDIA_LEARN_HOLD2;
}

static bool rep_count_stage(void)
{
	return learn_stage == KORZ_MEDIA_LEARN_COUNT1 || learn_stage == KORZ_MEDIA_LEARN_COUNT2;
}

static void rep_finish(int32_t t2, int32_t s2)
{
	int32_t dt = t2 - rep_t1, ds = s2 - rep_s1;
	int idx = ep_index();

	if (dt < 500 || ds < 2 || s2 >= steps[idx] - 1) {
		LOG_WRN("korz-media: repeat not learned (T %d/%d ms, steps %d/%d of %d): hold longer the "
			"second time, but stop before 100 %%",
			rep_t1, t2, rep_s1, s2, steps[idx]);
		learn_set_stage(KORZ_MEDIA_LEARN_OFF);
		return;
	}
	int32_t period = CLAMP(dt / ds, 10, 500);
	int32_t delay = CLAMP(rep_t1 - (rep_s1 - 1) * period, 0, 2000);
	uint16_t v[2] = {(uint16_t)delay, (uint16_t)period};
	char name[24];

	rep_delay[idx] = v[0];
	rep_period[idx] = v[1];
	learn_name(name, sizeof(name), "_rep");
	settings_save_one(name, v, sizeof(v));
	LOG_INF("korz-media: %s = repeat starts after %d ms, then every %d ms", name, delay, period);
	volume = 0; /* the host is at 0 % */
	announce();
	learn_set_stage(KORZ_MEDIA_LEARN_SAVED);
	k_work_reschedule(&learn_done_work, K_SECONDS(2));
}

static void rep_press(const struct zmk_keycode_state_changed *ev)
{
	k_work_reschedule(&learn_timeout_work, K_SECONDS(CONFIG_KORZ_MEDIA_LEARN_TIMEOUT_S));

	switch (ev->keycode) {
	case HID_USAGE_CONSUMER_VOLUME_INCREMENT:
		if (rep_hold_stage()) {
			hold_t0 = k_uptime_get();
		}
		break;
	case HID_USAGE_CONSUMER_VOLUME_DECREMENT:
		if (rep_count_stage()) {
			learn_count++;
			volume = (uint8_t)MIN(100, learn_count * 100 / steps[ep_index()]);
			announce();
		}
		break;
	case HID_USAGE_CONSUMER_MUTE:
		k_work_reschedule(&learn_hold_work, K_MSEC(CONFIG_KORZ_MEDIA_LEARN_HOLD_MS));
		if (learn_stage == KORZ_MEDIA_LEARN_COUNT1) {
			rep_s1 = learn_count;
			if (rep_s1 < 2) {
				LOG_WRN("korz-media: repeat not learned (too few taps)");
				learn_set_stage(KORZ_MEDIA_LEARN_OFF);
				return;
			}
			learn_count = 0;
			volume = 0;
			learn_set_stage(KORZ_MEDIA_LEARN_HOLD2);
		} else if (learn_stage == KORZ_MEDIA_LEARN_COUNT2) {
			rep_finish(rep_t2, learn_count);
		}
		break;
	default:
		break;
	}
}

static void rep_release(const struct zmk_keycode_state_changed *ev)
{
	if (ev->keycode == HID_USAGE_CONSUMER_MUTE) {
		k_work_cancel_delayable(&learn_hold_work); /* a short press: not a cancel */
	}
	if (ev->keycode == HID_USAGE_CONSUMER_VOLUME_INCREMENT && rep_hold_stage() && hold_t0) {
		int32_t t = (int32_t)(k_uptime_get() - hold_t0);

		hold_t0 = 0;
		if (t >= 700) { /* a real hold, not a tap */
			if (learn_stage == KORZ_MEDIA_LEARN_HOLD1) {
				rep_t1 = t;
			} else {
				rep_t2 = t;
			}
			learn_count = 0;
			volume = 0;
			learn_set_stage(learn_stage == KORZ_MEDIA_LEARN_HOLD1 ? KORZ_MEDIA_LEARN_COUNT1
									      : KORZ_MEDIA_LEARN_COUNT2);
		}
	}
}
#endif

static void volume_key(int dir)
{
#if IS_ENABLED(CONFIG_KORZ_MEDIA_LEARN)
	if (learn_stage == KORZ_MEDIA_LEARN_DOWN || learn_stage == KORZ_MEDIA_LEARN_UP) {
		learn_key(dir);
		return;
	}
#endif
	step(dir);
}

/* The keyboard sends one press; the host repeats it by itself while the key is held.
 * Do the same here, with the host's repeat delay and rate. */
static int held_dir;
static uint32_t held_key;

static void repeat_fn(struct k_work *work)
{
	if (held_dir != 0) {
#if IS_ENABLED(CONFIG_KORZ_MEDIA_LEARN)
		if (learn_stage >= KORZ_MEDIA_LEARN_HOLD1) {
			held_dir = 0; /* the repeat is being measured, not counted */
			return;
		}
#endif
		volume_key(held_dir);
		k_work_reschedule(k_work_delayable_from_work(work),
				  K_MSEC(rep_period[ep_index()]));
	}
}

static K_WORK_DELAYABLE_DEFINE(repeat_work, repeat_fn);

static int on_key(const zmk_event_t *eh)
{
	const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);

	if (ev == NULL || ev->usage_page != HID_USAGE_CONSUMER) {
		return ZMK_EV_EVENT_BUBBLE;
	}

#if IS_ENABLED(CONFIG_KORZ_MEDIA_LEARN)
	if (ev->state && learn_stage >= KORZ_MEDIA_LEARN_HOLD1) {
		rep_press(ev);
		return ZMK_EV_EVENT_BUBBLE; /* the keys reach the host, but are not counted as volume */
	}
#endif

	if (!ev->state) {
		if (ev->keycode == held_key && held_dir != 0) {
			held_dir = 0;
			k_work_cancel_delayable(&repeat_work);
		}
#if IS_ENABLED(CONFIG_KORZ_MEDIA_LEARN)
		if (learn_stage >= KORZ_MEDIA_LEARN_HOLD1) {
			rep_release(ev); /* it also covers the mute key */
		} else if (ev->keycode == HID_USAGE_CONSUMER_MUTE) {
			k_work_cancel_delayable(&learn_hold_work); /* a short press: just a mute */
		}
#endif
		return ZMK_EV_EVENT_BUBBLE;
	}

	switch (ev->keycode) {
	case HID_USAGE_CONSUMER_VOLUME_INCREMENT:
	case HID_USAGE_CONSUMER_VOLUME_DECREMENT:
		held_dir = ev->keycode == HID_USAGE_CONSUMER_VOLUME_INCREMENT ? 1 : -1;
		held_key = ev->keycode;
		volume_key(held_dir);
		k_work_reschedule(&repeat_work, K_MSEC(rep_delay[ep_index()]));
		break;
	case HID_USAGE_CONSUMER_MUTE:
		muted = !muted;
		announce();
#if IS_ENABLED(CONFIG_KORZ_MEDIA_LEARN)
		k_work_reschedule(&learn_hold_work, K_MSEC(CONFIG_KORZ_MEDIA_LEARN_HOLD_MS));
#endif
		break;
	default:
		break;
	}
	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(korz_media_volume, on_key);
ZMK_SUBSCRIPTION(korz_media_volume, zmk_keycode_state_changed);
