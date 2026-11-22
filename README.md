# korz-media

Host media state for the korz keyboard, **without any program on the computer**.

## Volume (estimate)

A keyboard cannot read the computer's volume: USB HID only goes keyboard -> computer, and
Bluetooth AVRCP is a Bluetooth Classic profile (the nRF52840 is BLE only). So the module
**counts**: each volume up / down key the dongle sends moves the value by
`CONFIG_KORZ_MEDIA_VOLUME_STEP` percent, mute toggles it. Keys from the keymap, macros and
the future gesture engine are all counted, because they all pass through ZMK's keycode event.

- Windows changes 2 % per press (default). macOS ~6 %, Android ~7 %: set the step.
- A held key repeats at the host rate (`KORZ_MEDIA_REPEAT_DELAY_MS`, `KORZ_MEDIA_REPEAT_MS`; tune by holding a key 3 s and comparing).
- It drifts if the volume is changed elsewhere (mouse, laptop keys, an app). Pressing up or
  down enough times pins it at 100 / 0, which re-syncs it.
- Other modules use it: `#include <korz/media.h>`, `korz_media_volume()`, `korz_media_muted()`,
  and the `korz_media_changed` event. `korz-epaper` draws it as a bar when `CONFIG_KORZ_MEDIA=y`.

## Not possible without a helper program on the host

Real volume, song title / artist. (iPhone only: Apple's AMS service over BLE could give
title and volume; not built.)

## Planned

Media-control layer for the round display (CrowPanel 1.28" knob), gesture-driven volume.
