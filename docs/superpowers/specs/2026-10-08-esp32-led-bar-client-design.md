# ESP32 LED Bar Client — Design

**Date:** 2026-10-08
**Status:** Approved, pending implementation plan
**Target version:** 1.7.0 (from 1.6.1)

> **Amendment, 2026-10-09:** the original 17-LED cap was raised to 24 after the user's strip turned out to have 24 LEDs. `LED_SRC_COUNT` (17, the Valve bar's fixed pixel count) is now separate from `LED_MAX_COUNT` (24, the longest supported strip). The bridge cannot send more than 17 pixels, so the firmware upsamples (linear interpolation) when a frame carries fewer pixels than the configured strip length. Figures below have been updated accordingly.

## Purpose

Make the BC-250 remote PSU controller drive the Steam Machine LED bar in addition to controlling power. One ESP32 does both jobs; no second microcontroller.

The ESP32 listens for WLED-compatible DRGB packets on UDP 21324 and writes them to a WS2812B strip. When no stream is arriving, the strip mirrors the existing power-control state machine, so it reports device status whenever it is not mirroring Game Mode.

## Success criteria

1. Changing LED settings in the SteamOS Game Mode personalization menu visibly changes the strip.
2. With no stream, the strip reports power state: dark when the PSU is off, amber fade-in while powering on, Steam-blue breathing while the BC-250 boots, fade-out on shutdown.
3. The existing WiFi, Bluepad32, WebServer and OTA behaviour is unchanged. A PS5 controller still pairs and powers the PC on while the strip is active.
4. Colour order and LED count are fixable from the web UI without recompiling.
5. Strip current is bounded by a firmware-side clamp that no host can exceed.

## Context and constraints

### Host side (not built here)

The LED data source is [steamos-led-wled](https://github.com/rpf16rj/steamos-led-wled), installed on the BC-250 and pointed at the ESP32's IP. That project supersedes [steamos-led-bar-release](https://github.com/rpf16rj/steamos-led-bar-release), which its own README marks discontinued.

The host does all the work: it reads 100-byte snapshots from the `leds-valve-shim` kernel module, remaps 17 source LEDs to the strip length, renders the Game Mode effects (rainbow, breath, patrol, factory, demo), applies optional temperature / notification / audio overlays, and streams finished pixels. The ESP32 therefore needs no effect engine.

This repository ships only the firmware client plus a README section for installing the bridge. The kernel module, server and installer stay upstream.

### Protocol

WLED UDP realtime, DRGB mode, port 21324 — the same protocol HyperHDR and Hyperion speak, so the strip also works with those without firmware changes.

```
byte 0      : protocol id, 2 = DRGB
byte 1      : timeout in seconds
byte 2+n*3  : red   for LED n
byte 3+n*3  : green for LED n
byte 4+n*3  : blue  for LED n
```

24 LEDs is 74 bytes (the bridge itself sends at most 17, i.e. 53 bytes). The bridge streams at roughly 30 fps and sets timeout = 2.

The bridge's optional `wled_power_control` setting issues HTTP GETs to `http://<host>/win&T=1` and `http://<host>/win&T=0`. Note the literal `&` with no `?`: the ESP32 WebServer sees the whole thing as the request path with an empty query string, so these are registered as two exact URIs, not parsed as query parameters.

### Hardware

- WS2812B **clone** strip, not a genuine Adafruit NeoPixel. Clone batches need a ~300 us reset gap rather than the classic 50 us, and sometimes ship RGB or BRG colour order where genuine parts ship GRB. Both are accommodated below.
- Data line through a 74AHCT125 level shifter (recommended). The alternative is a 2 A+ Schottky diode (e.g. 1N5820) in series with the strip's +5 V supply rail, which lowers VDD and the logic threshold. Do not put a diode in series with the data line: it lowers the logic high further. See README "Data line".
- 1 to 24 LEDs. Above 8, power the strip from an external 5 V supply sharing ground with the ESP32.
- GPIO 18 for data. Taken already: 16, 2, 23, 22, 4, 17.

### Rejected alternatives

| Option | Why not |
|---|---|
| Legacy TCP snapshot client (port 9876, 100-byte `VLED` struct) | Upstream discontinued; ESP8266 client source was never published, only a prebuilt `.bin`; would require re-deriving five effect renderers plus reconnect logic and a 30 s watchdog |
| Dedicated FreeRTOS task | Needs a mutex around shared power state for no gain at 30 fps with a non-blocking `loop()`. Revisit only if polling visibly stutters |
| `AsyncUDP` callbacks | Callback runs in lwIP task context and cannot touch the strip, so it would buffer and hand off anyway — the task approach's complexity with less control |
| `Adafruit_NeoPixel` | Bit-bangs with interrupts disabled ~30 us per LED; with WiFi and the BT stack live this flickers and can upset Bluepad32 |
| FastLED | Works, but its clone timing variants are less explicit than NeoPixelBus's |

## Architecture

New dependency: **NeoPixelBus**, using `NeoPixelBus<NeoGrbFeature, NeoEsp32Rmt0Ws2812xMethod>`. RMT channel 0 clocks the strip in hardware, so there is no interrupt blackout. Bluepad32 does not claim RMT. The `Ws2812x` variant supplies the long reset gap clones need.

### Files

| File | Change | Notes |
|---|---|---|
| `led_bar.h` | new, ~320 lines | UDP receive, strip driver, idle animator |
| `/led_config.json` | new, LittleFS | runtime settings |
| `tools/drgb_test.py` | new | host-side DRGB generator for testing |
| `ota_pc_remote.ino` | +6 lines | include, `ledBar.begin()`, `ledBar.handle()` |
| `pc_control.h` | ~8 lines | make the 2 s pre-restart delay non-blocking |
| `web_server.h` | +5 routes | `/api/led/config` GET+POST, `/api/led/test`, `/win&T=1`, `/win&T=0` |
| `data/setup.html` | +1 section | LED bar config card |
| `pins.h` | +1 define | `LED_DATA_PIN 18` |
| `version.h` | 1.6.1 to 1.7.0 | |
| `README.md` | +1 section | bridge install, wiring, new library |

`led_bar.h` is included after `pc_control.h` in the sketch so it can read `pcIsOn` and `powerState`. The dependency is strictly one-way: the LED module reads power state and never writes it, and nothing reads LED state back except the web API. This follows the existing module pattern set by `ps5_simple.h` and `pc_control.h`.

### Module interface

```cpp
class LedBar {
public:
  void begin();                 // load config, init strip, bind UDP, dark if PC off
  void handle();                // called every loop() pass, non-blocking
  void setConfig(uint8_t count, bool enabled, uint8_t maxBrightness,
                 uint8_t colorOrder, bool idleAnimations);
  void onPowerOffImminent();    // called by pc_control.h before ESP.restart()
  bool isStreaming() const;     // for /api/status and the web UI
  void startTestPattern();      // red, green, blue, 1 s each
private:
  enum Source { SRC_STREAM, SRC_IDLE };
  ...
};
extern LedBar ledBar;
```

The effect math, UDP buffer and strip object are private. Four things are answerable from outside: is a stream live, what is configured, render a frame, we are about to reboot.

`handle()` does three things per pass:

1. Drain pending UDP packets, newest wins.
2. Decide the source.
3. Render at most one frame, and only if 20 ms have elapsed (50 fps ceiling).

Worst case per pass is about 74 bytes of copying and one RMT write.

### Colour order

NeoPixelBus sets colour order as a compile-time template parameter, which is the wrong shape for "find out what this strip does." The bus stays `NeoGrbFeature` and a runtime 3-byte permutation is applied when filling the buffer, selected by `colorOrder` in config and a dropdown in the web UI. Cost is one swap per pixel.

Supported values: `GRB` (default), `RGB`, `BRG`, `RBG`, `GBR`, `BGR`.

A **Test strip** button drives pure red, then green, then blue for 1 s each. Three observations identify the strip's true order with no guesswork.

## Configuration

`/led_config.json`, following the shape of `ps5_config.json`:

```json
{
  "enabled": true,
  "ledCount": 8,
  "maxBrightness": 200,
  "colorOrder": "GRB",
  "idleAnimations": true
}
```

No host address or port: the ESP32 listens, so it needs neither. This is a real simplification over the original ESP8266 client, which had to be told where its server lived.

A missing or corrupt file falls back to these defaults and rewrites the file, the same way `loadPS5Config()` behaves today.

`maxBrightness` is a firmware-side current limit. 24 LEDs at full white draws roughly 1.4 A, which the dev board regulator will not survive. It is applied on every frame from every source, including the test pattern, so no host misconfiguration and no malformed packet can exceed it.

It **scales proportionally** rather than capping per channel: each channel becomes `value * maxBrightness / 255`. Scaling preserves hue; capping distorts it. At `maxBrightness` 128, amber `(255, 200, 0)` scales to `(128, 100, 0)` and stays amber, but a per-channel cap would yield `(128, 128, 0)` — pure yellow. The function is named `scaleBrightness()` to keep that unambiguous.

## Web API

| Route | Method | Behaviour |
|---|---|---|
| `/api/led/config` | GET | config plus `streaming`, `lastPacketAge`, `droppedPackets` |
| `/api/led/config` | POST | validate and save; re-init the bus if `ledCount` changed |
| `/api/led/test` | POST | run the test pattern |
| `/win&T=1` | GET | bridge power-on: allow output, return 200 |
| `/win&T=0` | GET | bridge power-off: fade out, latch dark |

POST validation: `ledCount` 1 to 24, `maxBrightness` 1 to 255, `colorOrder` one of the six permutations. Out-of-range values are rejected with 400 and the config is left untouched.

## Idle state machine

Stream wins whenever it is live. Otherwise power state selects an animation.

| Condition | Strip |
|---|---|
| DRGB packet within timeout | Stream, pixels as received |
| `POWER_ON_START`, `POWER_ON_WAITING_RELAY2`, `POWER_ON_COMPLETE` | Warm amber, fading in over ~2 s then holding steady until the state changes |
| `pcIsOn`, `POWER_IDLE`, no stream yet | Steam-blue `#00AEEF` breathing, 4 s period |
| `POWER_OFF_*`, `POWER_FORCE_*` | Fade to black, ~1.5 s |
| `POWER_IDLE` and not `pcIsOn` | Dark |
| `/win&T=0` received | Dark, latched until `T=1` or any DRGB packet |
| `enabled: false` | One black frame, then all work skipped |
| `idleAnimations: false` | Dark whenever the stream is not live |

The breathing state is the most diagnostically useful: it means the PSU is on and the ESP32 is alive, but SteamOS is not streaming. That is the failure mode most likely to need diagnosing.

**Stream timeout** is `max(byte1 * 1000, 1000)` ms since the last packet. The host controls it via the header, but a zero cannot cause source thrashing.

## The `pc_control.h` change

`pc_control.h:137` currently blocks for 2 s before restarting the ESP32 to clear Bluepad32 state:

```cpp
Serial.println("PC powered off - restarting ESP32 in 2 seconds...");
delay(2000);
ESP.restart();
```

A blocking delay means the fade-out never renders. Replace it with a wait that pumps the renderer, spending the same 2 s usefully:

```cpp
Serial.println("PC powered off - restarting ESP32 in 2 seconds...");
ledBar.onPowerOffImminent();
unsigned long t0 = millis();
while (millis() - t0 < 2000) { ledBar.handle(); delay(5); }
ESP.restart();
```

The restart behaviour itself is unchanged: same delay, same `ESP.restart()`, same Bluepad32 clearing. The 2-hour off-timer restart in `loop()` is also unchanged.

`begin()` reads `PC_MONITOR_PIN` before the first render, so coming up after a restart-while-off renders dark rather than flashing the strip.

## Error handling

| Failure | Behaviour |
|---|---|
| UDP packet shorter than 2 bytes, or longer than `2 + 24*3` | Drop, increment `droppedPackets` |
| `buf[0] != 2` (WARLS, DRGBW, DNRGB) | Ignore. Only DRGB is supported; documented in the README |
| Packet carries more LEDs than `ledCount` | Render the first `ledCount`, ignore the rest |
| Packet carries fewer | Render what arrived, leave the tail untouched |
| WiFi drops | `handle()` skips UDP and keeps animating; re-bind on reconnect |
| AP mode | UDP binds on 192.168.4.1; strip is testable with no router |
| `ledCount` changed via API | Tear down and re-init the bus. One dark frame, no reboot |
| LittleFS write fails on save | Return 500, keep the in-memory config applied |

## Testing

### Host-side, no hardware

`applyColorOrder()`, `scaleBrightness()` and `parseDrgb()` are pure functions with no Arduino dependencies, kept free of hardware calls so they compile and run on the development machine. They are tested there, test-first, covering:

- each of the six colour-order permutations
- brightness scaling: channel values 0, 1, 200, 254 and 255 at `maxBrightness` of 1, 128 and 255, asserting hue is preserved and nothing exceeds `maxBrightness`
- DRGB header validation, LED-count clamping, truncated payloads, oversized payloads, and non-DRGB protocol ids

### Protocol, ESP32 on the bench

`tools/drgb_test.py` (Python stdlib only, runs on the Windows development box) sends DRGB frames to the ESP32's IP:

- solid colours for colour-order confirmation
- a rainbow sweep for smoothness
- a 30 fps soak for sustained-throughput stability
- deliberately malformed packets covering every row of the error table

### Hardware

- Colour order identified via the **Test strip** button.
- Fade-out timing verified against the serial log around the power-off restart.
- Soak test with a PS5 controller connected, confirming RMT and Bluepad32 coexist and that controller power-on still works.
- WiFi-drop and reconnect recovery.
- Current draw measured at `maxBrightness` on a full-white frame.

### Not verifiable without the BC-250

The kernel shim feeding real Game Mode data is the final manual step: install `steamos-led-wled` on the BC-250, point `wled_host` at the ESP32's IP, set `num_leds` to match, then change settings in the Game Mode personalization menu and confirm the strip follows.

## Out of scope

- The kernel module, Python server and installer. They stay upstream.
- WARLS, DRGBW and DNRGB protocol modes.
- ESP32-side effect rendering. The host renders everything.
- The Decky Loader plugin.
- Any change to power-control logic beyond making the existing 2 s delay non-blocking.
