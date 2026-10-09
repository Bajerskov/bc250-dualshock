# LED bar — on-device verification checklist

Everything in the LED bar feature that cannot be verified without hardware.
It was implemented and reviewed entirely without a board attached, so **nothing
below has been observed working** — this list is what remains to confirm.

Firmware v1.7.0. See `README.md` for wiring and host setup.

> ## READ THIS BEFORE FLASHING
>
> **Keep the serial monitor (115200) attached through the first boot after
> flashing.** NeoPixelBus initialises the LED driver with a bare
> `ESP_ERROR_CHECK(rmt_config(...))`, which *aborts* the chip rather than
> returning an error. If RMT channel 0 is ever unavailable, the device panics
> during `setup()` and boot-loops: no web UI, no OTA, and recovery only over USB
> serial. This is unlikely (nothing in Bluepad32 or this sketch should claim
> RMT), but it cannot be caught in code.
>
> - Confirm the banner appears: `LED bar: enabled, N LEDs, maxBrightness M, order X`
> - A **repeating boot banner or a panic** means you must reflash over USB.
> - Also upload the filesystem (`data/`) separately, or the web UI has no LED card.

## Prerequisite

- [ ] **Install NeoPixelBus** — Arduino IDE → Tools → Manage Libraries →
      `NeoPixelBus by Makuna` → 2.8.x or newer.
      Older versions lack ESP32 Arduino 3.x RMT support, which the
      `esp32-bluepad32` core 4.1.0 is built on, so they will not compile.

## Wiring — before any strip test

Your strip is **24 LEDs**, which changes the power situation.

- [ ] WS2812B DIN ← **GPIO 18**, through a **74AHCT125 level shifter**
      (recommended). Alternative: a Schottky diode rated 2 A or more (e.g.
      1N5820) in series with the strip's **+5 V supply**, which lowers VDD and
      the logic threshold. **Do not put a diode on the data line**: it lowers
      the logic high and makes things worse. A 1N4148 is far too small for the
      supply rail (~1.4 A). Clone strips are fussy about 3.3 V data.
- [ ] Strip GND ↔ ESP32 GND (shared ground is mandatory).
- [ ] **External 5 V supply for the strip — not optional at 24 LEDs.** Full white
      on 24 is roughly 1.4 A, which the dev board's regulator cannot supply.
      Feed the strip directly and share ground with the ESP32.
- [ ] Recommended: 1000 uF across strip VCC/GND near the first LED.

## 24-LED specifics

- [ ] Set **ledCount = 24** in the web UI (`/setup`). The firmware default is
      deliberately **8**, so a first boot cannot pull 1.4 A before you have
      wired the external supply. Set it once; it persists.
- [ ] **Set the bridge's `num_leds = min(strip length, 17)`** in
      `/etc/steamos-led-wled.conf`. For your 24-LED strip that is **17**.
      Do NOT set 24 there. (For a strip shorter than 17, use the strip length so
      the host downsamples; a larger packet is cropped, not downsampled.) The Valve bar has only 17 pixels and the bridge's
      `extract_pixels()` reads past the end of its 100-byte snapshot at pixel
      17, so `num_leds = 24` makes the service die with `IndexError`. The
      ESP32 stretches 17 → 24 itself.
- [ ] Expect the Game Mode bar to look slightly softer than a native 24-pixel
      image: 17 source pixels are interpolated across 24 LEDs. Both ends map
      exactly onto the first and last source pixel, so no LED is dark at the
      edges.
- [ ] `tools/drgb_test.py` sends 8-pixel frames by default. That still lights
      all 24, because the firmware upsamples anything shorter than your strip.
      Pass the LED count if you want native 24-pixel frames.

## Boot and config persistence

- [ ] Arduino IDE Verify, then Upload.
- [ ] Serial monitor at 115200 shows a line like:
      `LED bar: enabled, 8 LEDs, maxBrightness 200, order GRB`
- [ ] Strip is completely dark.
- [ ] Existing behaviour unchanged: WiFi connects, web UI loads, PS5 controller
      still pairs.
- [ ] Power-cycle: the serial line reports the same values (config persisted).

## DRGB streaming

- [ ] `python tools/drgb_test.py <esp-ip> solid 255 0 0`
      Strip lights for 5 s, then goes dark ~2 s after the script stops.
      **Note which colour actually appeared** — if not red, your strip's colour
      order is not GRB. That is expected and Task 8 fixes it from the web UI.
- [ ] `python tools/drgb_test.py <esp-ip> rainbow 10` — smooth, no flicker, no stutter.
- [ ] With `enabled: false` in config: `solid 255 255 255` leaves the strip
      **completely dark** for the whole run.
- [ ] `python tools/drgb_test.py <esp-ip> malformed` — no crash, no reboot
      (watch serial for a boot banner), and the strip still responds to a
      following `solid` run.
      **Caveat, not a bug:** the `empty` (0-byte) case will NOT increment
      `droppedPackets`. `parsePacket()` returns 0 for an empty datagram, which
      is indistinguishable from "no packet", so the drain loop exits without
      counting it. Known and deliberately deferred. Of the other cases, `header only
      keepalive`, `partial pixel` and `zero timeout` are valid by design, so
      they do NOT increment the counter either. The rest do (`oversized` is
      77 bytes, over the 74-byte maximum). Do not chase a counter that is right
      not to move.

> The generator itself is already verified. I bound UDP 21324 on localhost and
> asserted the bytes `drgb_test.py` emits: protocol id, timeout byte, frame
> length, exact solid colour, rainbow variation and animation, and all the
> malformed shapes. 16/16 passed. So if a bench test misbehaves, suspect the
> firmware or the wiring, not the tool.

## Idle animations and the power-off fade

With the host bridge NOT running:

- [ ] PC off, idle → dark
- [ ] Press the power button → amber fading in over ~2 s, then holding
- [ ] BC-250 booted, bridge not installed → Steam-blue breathing, 4 s period
- [ ] `rainbow 10` → takes over immediately; breathing returns ~2 s after it stops
- [ ] Power off → fade to black over ~1.5 s, **then** the reboot banner on serial.
      This is the whole point of the `pc_control.h` change: confirm
      `PC powered off - restarting ESP32 in 2 seconds...` appears on serial
      while the strip is still visibly fading.

## Web API

- [ ] `curl http://<esp-ip>/api/led/config` returns the config
- [ ] POST a change, GET again, power-cycle, GET again → persisted
- [ ] All five malformed POSTs return **400** and leave config unchanged
- [ ] Orphaned tail: start `solid 255 255 255`, then POST `{"ledCount":3}` →
      only the first 3 LEDs stay lit; LEDs 4+ go dark, not frozen white
- [ ] `curl "http://<esp-ip>/win&T=0"` → strip goes dark and holds (it snaps
      to black; there is no fade, which is correct per the spec);
      a following DRGB run lights it anyway (any packet unlatches)
- [ ] `T=0` then `T=1` → dark, then idle animation resumes

## Web UI and colour order

- [ ] Upload the filesystem (LittleFS), not just the sketch — `setup.html` is
      served from flash. Use the existing `/update-fs` endpoint or the IDE's
      LittleFS upload tool.
- [ ] Open `http://<esp-ip>/setup`, click **Test strip**, note the colour sequence
- [ ] If it was not red → green → blue, select the order you actually saw, Save,
      Test strip again → must now be red → green → blue
- [ ] Change LED count (24 for your strip), Save, reload → persists
- [ ] Brightness to 10, Test strip → visibly dim
- [ ] LED count 0 → blocked by the field's `min="1"`; if it submits anyway, the
      status line shows the 400 error and config does not change

## Full regression

- [ ] Power on via web UI → amber fade, then blue breathing
- [ ] Install `steamos-led-wled` on the BC-250, set `wled_host` to the ESP32's
      IP and `num_leds` to `min(strip length, 17)` (17 for your 24 LEDs). Game Mode → Personalization → LED settings:
      change colour and effect, strip follows. **This is the one thing that
      cannot be verified any other way.**
- [ ] PS5 controller connected; power the PC on with the PS button while the
      strip is streaming. Both still work.
- [ ] `python tools/drgb_test.py <esp-ip> soak 300` — five minutes, no flicker,
      no reboot, controller still responsive
- [ ] Pull router power 30 s, restore → ESP32 reconnects, strip resumes
      (this exercises the UDP re-bind fix)
- [ ] Power off from the web UI → strip fades, then the reboot banner
- [ ] Measure current draw at `maxBrightness` on a full-white frame across all
      24 LEDs, and confirm the external supply handles it
