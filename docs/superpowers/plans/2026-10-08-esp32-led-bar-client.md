# ESP32 LED Bar Client Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the BC-250 remote PSU controller also drive a WS2812B LED bar, receiving WLED-compatible DRGB packets on UDP 21324 and falling back to power-state animations when no stream arrives.

**Architecture:** Pure protocol/pixel logic lives in `led_protocol.h` with no Arduino dependencies, so it is unit-tested on the development machine with MSVC. `led_bar.h` wraps it with the Arduino concerns — a `WiFiUDP` socket polled from `loop()`, a NeoPixelBus RMT strip driver, and an idle animator that reads (never writes) the existing `pcIsOn` / `powerState` globals. The host renders all Game Mode effects; the ESP32 only displays pixels.

**Tech Stack:** Arduino C++ on `esp32-bluepad32` core 4.1.0, NeoPixelBus (new dependency), ArduinoJson (already installed), LittleFS, `WiFiUDP`, `WebServer`. Host tests compiled with MSVC `cl.exe` from VS 2022 Community. Test packet generator in Python 3 (stdlib only).

**Spec:** `docs/superpowers/specs/2026-10-08-esp32-led-bar-client-design.md`

## Global Constraints

- Target firmware version **1.7.0** (from 1.6.1) in `version.h`.
- `LED_SRC_COUNT` is **17** (the Valve LED bar's pixel count, fixed by hardware).
  `LED_MAX_COUNT` is **24** (maximum supported strip length). Default `ledCount` 8.
  Valid range 1–24 inclusive. **Amended 2026-10-09**: originally capped at 17; the
  user's strip has 24 LEDs and the bridge cannot send more than 17, so the firmware
  upsamples. See Task 7B.
- UDP listen port is **21324**. DRGB protocol id is **2**. No other WLED protocol mode is supported.
- `maxBrightness` **scales proportionally** (`value * maxBrightness / 255`), never caps per channel. Default 200. Valid range 1–255.
- LED data pin is **GPIO 18** (`LED_DATA_PIN` in `pins.h`). Pins 16, 2, 23, 22, 4, 17 are already taken.
- Strip driver is `NeoPixelBus<NeoGrbFeature, NeoEsp32Rmt0Ws2812xMethod>` — the `Ws2812x` variant for its long reset gap, required by clone WS2812B.
- Six colour orders, named exactly: `GRB` (default), `RGB`, `BRG`, `RBG`, `GBR`, `BGR`.
- Config file is `/led_config.json` on LittleFS. It contains **no host address or port** — the ESP32 listens.
- The dependency is strictly one-way: `led_bar.h` reads `pcIsOn` and `powerState` and never writes them.
- Existing WiFi, Bluepad32, WebServer and OTA behaviour must be unchanged. The only permitted power-control change is making the existing 2 s pre-restart delay non-blocking.
- `led_protocol.h` must include no Arduino headers and call no hardware functions, or the host tests stop compiling.

## Review Focus

Five conditions the spec implies but that no task's happy-path tests would otherwise exercise, most likely to bite first. Each has its test pinned to the task that owns the code.

1. **A DRGB packet arriving while `enabled: false`** must leave the strip dark — a disabled strip that lights up on a stray packet is the worst failure here. → Task 5.
2. **`ledCount` lowered via the API while a stream is live** must not leave the now-orphaned tail pixels lit from the previous frame. → Task 7.
3. **A bare 2-byte DRGB packet** (header only, no pixels) is valid per the spec's error table and must refresh stream liveness without changing or blanking any pixel. → Task 2.
4. **A DRGB packet arriving while `/win&T=0` has latched the strip dark** must unlatch it, per the idle-state table. → Task 7.
5. **`maxBrightness: 1`** must not throw, divide by zero, or wrap — mid-range channel values legitimately round to 0, and that must be a documented outcome rather than a surprise. → Task 3.

---

## File Structure

| File | Responsibility |
|---|---|
| `led_protocol.h` | **New.** Pure logic, zero Arduino deps: `parseDrgb`, `applyColorOrder`, `scaleBrightness`, colour-order name mapping, shared constants and `Rgb`/`DrgbFrame` types. Host-compilable. |
| `led_bar.h` | **New.** The `LedBar` class: config load/save, NeoPixelBus strip, `WiFiUDP` receive, source arbitration, idle animator, test pattern. |
| `test/test_led_protocol.cpp` | **New.** Host unit tests for everything in `led_protocol.h`. |
| `test/run_tests.ps1` | **New.** Compiles and runs the host tests via MSVC; exits non-zero on failure. |
| `tools/drgb_test.py` | **New.** Host-side DRGB packet generator for on-device testing. |
| `ota_pc_remote.ino` | Modify. Include `led_bar.h`, call `ledBar.begin()` and `ledBar.handle()`. |
| `pc_control.h` | Modify. Make the 2 s pre-restart delay non-blocking so the fade-out renders. |
| `web_server.h` | Modify. Five new routes. |
| `data/setup.html` | Modify. LED bar config card with colour-order dropdown and test button. |
| `pins.h` | Modify. Add `LED_DATA_PIN`. |
| `version.h` | Modify. 1.6.1 → 1.7.0. |
| `README.md` | Modify. NeoPixelBus install, wiring, bridge setup. |

**Two refinements over the spec, both deliberate:**

1. The spec described a single `led_bar.h` of ~320 lines. Splitting the pure logic into `led_protocol.h` is what makes the spec's own host-testing requirement achievable — a file that includes `WiFiUdp.h` cannot compile under `cl.exe`. This is the spec's testability requirement taken literally, not a change of design.

2. The spec sketched `setConfig(uint8_t count, bool enabled, uint8_t maxBrightness, uint8_t colorOrder, bool idleAnimations)` returning `void`. The plan uses `setConfig(const LedConfig&)` returning a `SetConfigResult` enum, because the spec's own error table requires the API to distinguish **400** (invalid value, config untouched) from **500** (valid value, LittleFS write failed, applied in memory only). A `void` return cannot carry that distinction, and five positional parameters of mostly-`uint8_t` invite silent argument transposition.

---

### Task 1: Environment and host test harness

Foundation. Produces a working `test/run_tests.ps1` that compiles and runs a real (trivial) assertion, plus the NeoPixelBus dependency the later tasks need.

**Files:**
- Create: `test/run_tests.ps1`
- Create: `test/test_led_protocol.cpp`
- Create: `led_protocol.h`
- Create: `test/.gitignore`

**Interfaces:**
- Consumes: nothing.
- Produces: `test/run_tests.ps1` as the single test command for all later tasks. `led_protocol.h` with include guard `LED_PROTOCOL_H` and the constants `LED_MAX_COUNT`, `DRGB_PROTOCOL_ID`, `DRGB_HEADER_SIZE`, `DRGB_MAX_PACKET`, plus `struct Rgb { uint8_t r, g, b; }` and `enum ColorOrder`.

- [ ] **Step 1: Install NeoPixelBus**

Open Arduino IDE → Tools → Manage Libraries, search `NeoPixelBus by Makuna`, install the latest 2.8.x or newer. 2.8.0 is the first release with `arduino-esp32` 3.x RMT support, which core 4.1.0 of `esp32-bluepad32` is based on; anything older will not compile.

Verify it landed:

```bash
ls /c/Users/Anders/Documents/Arduino/libraries
```

Expected: a `NeoPixelBus_by_Makuna` directory alongside `ArduinoJson`.

- [ ] **Step 2: Create `led_protocol.h` with types and constants only**

```cpp
#ifndef LED_PROTOCOL_H
#define LED_PROTOCOL_H

// Pure protocol and pixel logic for the LED bar client.
// This header must stay free of Arduino headers and hardware calls so the
// host unit tests in test/test_led_protocol.cpp keep compiling under MSVC.

#include <stdint.h>
#include <stddef.h>

#define LED_MAX_COUNT     17
#define DRGB_PROTOCOL_ID  2
#define DRGB_HEADER_SIZE  2
#define DRGB_MAX_PACKET   (DRGB_HEADER_SIZE + LED_MAX_COUNT * 3)

enum ColorOrder : uint8_t {
    ORDER_GRB = 0,
    ORDER_RGB,
    ORDER_BRG,
    ORDER_RBG,
    ORDER_GBR,
    ORDER_BGR,
    ORDER_COUNT
};

struct Rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

#endif
```

- [ ] **Step 3: Create the test harness with one real assertion**

`test/test_led_protocol.cpp`:

```cpp
// Host unit tests for led_protocol.h. Build and run with test/run_tests.ps1.
#include <cstdio>
#include <cstring>
#include <initializer_list>     // Task 2 iterates a braced list; do not rely on
                                // another header pulling this in transitively.
#include "led_protocol.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do {                                                   \
    g_checks++;                                                            \
    if (!(cond)) {                                                         \
        g_failures++;                                                      \
        printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
    }                                                                      \
} while (0)

#define CHECK_RGB(actual, er, eg, eb) do {                                 \
    g_checks++;                                                            \
    Rgb _a = (actual);                                                     \
    if (_a.r != (er) || _a.g != (eg) || _a.b != (eb)) {                     \
        g_failures++;                                                      \
        printf("FAIL %s:%d  got (%u,%u,%u) want (%u,%u,%u)\n",             \
               __FILE__, __LINE__,                                         \
               _a.r, _a.g, _a.b,                                           \
               (unsigned)(er), (unsigned)(eg), (unsigned)(eb));            \
    }                                                                      \
} while (0)

static void test_constants() {
    CHECK(LED_MAX_COUNT == 17);
    CHECK(DRGB_PROTOCOL_ID == 2);
    CHECK(DRGB_HEADER_SIZE == 2);
    CHECK(DRGB_MAX_PACKET == 53);
    CHECK(ORDER_COUNT == 6);
}

int main() {
    test_constants();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 4: Create the test runner**

`test/run_tests.ps1`:

```powershell
# Compiles and runs the host unit tests for led_protocol.h.
# Requires Visual Studio 2022 Community (cl.exe). Exits non-zero on failure.
$ErrorActionPreference = "Stop"

$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $PSScriptRoot "build"
New-Item -ItemType Directory -Force $out | Out-Null

$src = Join-Path $PSScriptRoot "test_led_protocol.cpp"
$exe = Join-Path $out "test_led_protocol.exe"

# Compile from inside the build directory so object files land there. Do NOT
# use /Fo:"$out\" instead -- the trailing backslash escapes the closing quote
# in cmd and the compile dies with "C1083: Cannot open compiler generated
# file: '': Invalid argument".
#
# /wd4127 silences C4127 "conditional expression is constant", which /W4 emits
# for every assertion on a compile-time constant, e.g. CHECK(LED_MAX_COUNT == 17).
cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$out`" && cl /nologo /EHsc /W4 /wd4127 /std:c++17 /I`"$root`" `"$src`" /Fe:test_led_protocol.exe"
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

& $exe
exit $LASTEXITCODE
```

Both of those flags were established by running this exact command — the first version of it failed on both counts.

- [ ] **Step 5: Create `test/.gitignore`**

```
build/
```

- [ ] **Step 6: Run the tests and verify they pass**

Run: `powershell -ExecutionPolicy Bypass -File test/run_tests.ps1`
Expected: compiles with no errors, prints `5 checks, 0 failures`, exits 0.

- [ ] **Step 7: Verify the sketch still compiles**

Open `ota_pc_remote.ino` in Arduino IDE, select the `esp32-bluepad32` ESP32 Dev Module board, and Verify. Nothing has been wired in yet, so this is a baseline — it must compile exactly as it did before.

Optional but recommended for later tasks: install `arduino-cli` so compile checks can be automated rather than clicked. Find the board's FQBN with `arduino-cli board listall | grep -i bluepad` and record it in the README; do not assume a value.

- [ ] **Step 8: Commit**

```bash
git add test/ led_protocol.h
git commit -m "test: add host test harness and led_protocol.h skeleton"
```

---

### Task 2: `parseDrgb`

**Files:**
- Modify: `led_protocol.h`
- Modify: `test/test_led_protocol.cpp`

**Interfaces:**
- Consumes: `Rgb`, `LED_MAX_COUNT`, `DRGB_PROTOCOL_ID`, `DRGB_HEADER_SIZE`, `DRGB_MAX_PACKET` from Task 1.
- Produces:
  ```cpp
  struct DrgbFrame {
      bool    valid;
      uint8_t timeoutSec;
      uint8_t ledCount;             // pixels present, already clamped to maxLeds
      Rgb     pixels[LED_MAX_COUNT];
  };
  DrgbFrame parseDrgb(const uint8_t* buf, size_t len, uint8_t maxLeds);
  ```
  `valid == false` means the packet must be dropped and the drop counter incremented. `valid == true` with `ledCount == 0` is a header-only keepalive: refresh liveness, change no pixels.

- [ ] **Step 1: Write the failing tests**

Add to `test/test_led_protocol.cpp`, and add `test_parse_drgb();` as the first line of `main()`:

```cpp
static void test_parse_drgb() {
    // A well-formed 2-LED packet.
    const uint8_t good[] = { 2, 2,
                             255, 0, 0,
                             0, 255, 0 };
    DrgbFrame f = parseDrgb(good, sizeof(good), 17);
    CHECK(f.valid);
    CHECK(f.timeoutSec == 2);
    CHECK(f.ledCount == 2);
    CHECK_RGB(f.pixels[0], 255, 0, 0);
    CHECK_RGB(f.pixels[1], 0, 255, 0);

    // Header-only keepalive: valid, no pixels. (Review Focus 3)
    const uint8_t keepalive[] = { 2, 2 };
    f = parseDrgb(keepalive, sizeof(keepalive), 17);
    CHECK(f.valid);
    CHECK(f.ledCount == 0);
    CHECK(f.timeoutSec == 2);

    // Too short.
    const uint8_t runt[] = { 2 };
    CHECK(!parseDrgb(runt, sizeof(runt), 17).valid);
    CHECK(!parseDrgb(runt, 0, 17).valid);

    // Null buffer.
    CHECK(!parseDrgb(nullptr, 8, 17).valid);

    // Wrong protocol id: WARLS(1), DRGBW(3), DNRGB(4).
    for (uint8_t proto : { (uint8_t)1, (uint8_t)3, (uint8_t)4 }) {
        uint8_t p[] = { proto, 2, 10, 20, 30 };
        CHECK(!parseDrgb(p, sizeof(p), 17).valid);
    }

    // Oversized: one LED past the 17-LED maximum.
    uint8_t big[DRGB_MAX_PACKET + 3] = { 2, 2 };
    CHECK(!parseDrgb(big, sizeof(big), 17).valid);

    // Exactly the maximum is accepted.
    uint8_t full[DRGB_MAX_PACKET] = { 2, 2 };
    for (int i = 0; i < 17; i++) { full[2 + i * 3] = (uint8_t)(i + 1); }
    f = parseDrgb(full, sizeof(full), 17);
    CHECK(f.valid);
    CHECK(f.ledCount == 17);
    CHECK(f.pixels[16].r == 17);

    // More pixels than configured: render the first maxLeds, ignore the rest.
    f = parseDrgb(full, sizeof(full), 4);
    CHECK(f.valid);
    CHECK(f.ledCount == 4);
    CHECK(f.pixels[3].r == 4);

    // maxLeds above the hard maximum is itself clamped.
    f = parseDrgb(full, sizeof(full), 200);
    CHECK(f.valid);
    CHECK(f.ledCount == 17);

    // maxLeds of 0 yields a valid frame with no pixels.
    f = parseDrgb(full, sizeof(full), 0);
    CHECK(f.valid);
    CHECK(f.ledCount == 0);

    // A trailing partial pixel is ignored, not misread.
    const uint8_t partial[] = { 2, 2, 9, 9, 9, 7, 7 };
    f = parseDrgb(partial, sizeof(partial), 17);
    CHECK(f.valid);
    CHECK(f.ledCount == 1);
    CHECK_RGB(f.pixels[0], 9, 9, 9);

    // timeoutSec passes through verbatim, including 0.
    const uint8_t zero_timeout[] = { 2, 0, 1, 2, 3 };
    CHECK(parseDrgb(zero_timeout, sizeof(zero_timeout), 17).timeoutSec == 0);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `powershell -ExecutionPolicy Bypass -File test/run_tests.ps1`
Expected: compile FAILS with errors naming `DrgbFrame` and `parseDrgb` as undeclared identifiers.

- [ ] **Step 3: Write the implementation**

Add to `led_protocol.h`, before the closing `#endif`:

```cpp
struct DrgbFrame {
    bool    valid;
    uint8_t timeoutSec;
    uint8_t ledCount;
    Rgb     pixels[LED_MAX_COUNT];
};

// Parse a WLED DRGB realtime packet.
//   byte 0      protocol id, must be 2
//   byte 1      timeout in seconds
//   byte 2+n*3  r, g, b for LED n
// Returns valid == false for any packet that must be dropped. A valid frame
// with ledCount == 0 is a header-only keepalive: refresh liveness, change
// nothing on the strip.
inline DrgbFrame parseDrgb(const uint8_t* buf, size_t len, uint8_t maxLeds) {
    DrgbFrame f;
    f.valid = false;
    f.timeoutSec = 0;
    f.ledCount = 0;
    for (uint8_t i = 0; i < LED_MAX_COUNT; i++) {
        f.pixels[i].r = 0;
        f.pixels[i].g = 0;
        f.pixels[i].b = 0;
    }

    if (buf == nullptr) { return f; }
    if (len < DRGB_HEADER_SIZE) { return f; }
    if (len > (size_t)DRGB_MAX_PACKET) { return f; }
    if (buf[0] != DRGB_PROTOCOL_ID) { return f; }

    if (maxLeds > LED_MAX_COUNT) { maxLeds = LED_MAX_COUNT; }

    size_t payload = len - DRGB_HEADER_SIZE;
    uint8_t n = (uint8_t)(payload / 3);     // a trailing partial pixel is ignored
    if (n > maxLeds) { n = maxLeds; }

    f.valid = true;
    f.timeoutSec = buf[1];
    f.ledCount = n;
    for (uint8_t i = 0; i < n; i++) {
        f.pixels[i].r = buf[DRGB_HEADER_SIZE + i * 3 + 0];
        f.pixels[i].g = buf[DRGB_HEADER_SIZE + i * 3 + 1];
        f.pixels[i].b = buf[DRGB_HEADER_SIZE + i * 3 + 2];
    }
    return f;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `powershell -ExecutionPolicy Bypass -File test/run_tests.ps1`
Expected: `0 failures`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add led_protocol.h test/test_led_protocol.cpp
git commit -m "feat: parse WLED DRGB realtime packets"
```

---

### Task 3: `applyColorOrder` and `scaleBrightness`

Two pure pixel transforms. Both are small, both are applied on the same code path, and neither is useful without the other.

**Files:**
- Modify: `led_protocol.h`
- Modify: `test/test_led_protocol.cpp`

**Interfaces:**
- Consumes: `Rgb`, `ColorOrder` from Task 1.
- Produces:
  ```cpp
  Rgb applyColorOrder(Rgb c, ColorOrder order);
  Rgb scaleBrightness(Rgb c, uint8_t maxBrightness);
  const char* colorOrderName(ColorOrder o);
  bool colorOrderFromName(const char* name, ColorOrder* out);
  ```

**Why `applyColorOrder` is not a simple swap.** The bus is compiled as `NeoGrbFeature`, so `SetPixelColor(RgbColor(r, g, b))` emits the wire bytes in the order `(g, r, b)`. To make a strip whose true order is something else display the right colour, build the wire triple that strip expects, then invert the bus's GRB emission. For `ORDER_GRB` this is the identity. Get this backwards and every non-default order is wrong, which is exactly why it is tested rather than eyeballed.

- [ ] **Step 1: Write the failing tests**

Add to `test/test_led_protocol.cpp`, and add `test_color_order();` and `test_scale_brightness();` to `main()`:

```cpp
static void test_color_order() {
    const Rgb c = { 10, 20, 30 };   // r=10, g=20, b=30

    // GRB is the bus's native order, so nothing is permuted.
    CHECK_RGB(applyColorOrder(c, ORDER_GRB), 10, 20, 30);

    // The bus emits (g, r, b). For a strip that is really RGB we want the wire
    // triple (r, g, b), so g must carry r and r must carry g.
    CHECK_RGB(applyColorOrder(c, ORDER_RGB), 20, 10, 30);

    // Strip BRG wants wire (b, r, g) -> g=b, r=r, b=g.
    CHECK_RGB(applyColorOrder(c, ORDER_BRG), 10, 30, 20);

    // Strip RBG wants wire (r, b, g) -> g=r, r=b, b=g.
    CHECK_RGB(applyColorOrder(c, ORDER_RBG), 30, 10, 20);

    // Strip GBR wants wire (g, b, r) -> g=g, r=b, b=r.
    CHECK_RGB(applyColorOrder(c, ORDER_GBR), 30, 20, 10);

    // Strip BGR wants wire (b, g, r) -> g=b, r=g, b=r.
    CHECK_RGB(applyColorOrder(c, ORDER_BGR), 20, 30, 10);

    // Every order is a permutation: the multiset of channels is preserved.
    for (uint8_t o = 0; o < ORDER_COUNT; o++) {
        Rgb out = applyColorOrder(c, (ColorOrder)o);
        int sum = out.r + out.g + out.b;
        CHECK(sum == 60);
    }

    // An out-of-range order falls back to GRB rather than reading garbage.
    CHECK_RGB(applyColorOrder(c, (ColorOrder)99), 10, 20, 30);

    // Names round-trip.
    CHECK(strcmp(colorOrderName(ORDER_GRB), "GRB") == 0);
    CHECK(strcmp(colorOrderName(ORDER_BGR), "BGR") == 0);
    for (uint8_t o = 0; o < ORDER_COUNT; o++) {
        ColorOrder back;
        CHECK(colorOrderFromName(colorOrderName((ColorOrder)o), &back));
        CHECK(back == (ColorOrder)o);
    }
    ColorOrder dummy;
    CHECK(!colorOrderFromName("WAT", &dummy));
    CHECK(!colorOrderFromName("", &dummy));
    CHECK(!colorOrderFromName(nullptr, &dummy));
    CHECK(strcmp(colorOrderName((ColorOrder)99), "GRB") == 0);
}

static void test_scale_brightness() {
    // Full brightness is the identity.
    CHECK_RGB(scaleBrightness({ 255, 200, 0 }, 255), 255, 200, 0);
    CHECK_RGB(scaleBrightness({ 0, 0, 0 }, 255), 0, 0, 0);

    // Half brightness preserves hue: amber stays amber, it does not become
    // yellow the way a per-channel cap would.
    CHECK_RGB(scaleBrightness({ 255, 200, 0 }, 128), 128, 100, 0);

    // Black stays black at any setting.
    CHECK_RGB(scaleBrightness({ 0, 0, 0 }, 1), 0, 0, 0);

    // maxBrightness of 1: must not wrap or throw. Full channels round to 1,
    // mid-range channels legitimately round to 0. (Review Focus 5)
    Rgb dim = scaleBrightness({ 255, 100, 1 }, 1);
    CHECK(dim.r == 1);
    CHECK(dim.g == 0);
    CHECK(dim.b == 0);

    // Nothing ever exceeds maxBrightness, for every channel and setting.
    for (int mb = 1; mb <= 255; mb++) {
        Rgb out = scaleBrightness({ 255, 255, 255 }, (uint8_t)mb);
        CHECK(out.r <= mb);
        CHECK(out.g <= mb);
        CHECK(out.b <= mb);
    }

    // Monotonic in the input: brighter in is never darker out.
    for (int v = 0; v < 255; v++) {
        Rgb lo = scaleBrightness({ (uint8_t)v, 0, 0 }, 200);
        Rgb hi = scaleBrightness({ (uint8_t)(v + 1), 0, 0 }, 200);
        CHECK(hi.r >= lo.r);
    }

    // maxBrightness of 0 is out of the configured range but must still be safe.
    CHECK_RGB(scaleBrightness({ 255, 255, 255 }, 0), 0, 0, 0);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `powershell -ExecutionPolicy Bypass -File test/run_tests.ps1`
Expected: compile FAILS naming `applyColorOrder`, `scaleBrightness`, `colorOrderName`, `colorOrderFromName` as undeclared.

- [ ] **Step 3: Write the implementation**

Add to `led_protocol.h`, before the closing `#endif`:

```cpp
// Permute a colour so a strip whose true channel order is `order` displays it
// correctly through a bus compiled as NeoGrbFeature.
//
// The GRB bus emits wire bytes in the order (g, r, b) from SetPixelColor(r,g,b).
// So: build the wire triple the strip expects, then invert that emission.
inline Rgb applyColorOrder(Rgb c, ColorOrder order) {
    uint8_t w0, w1, w2;             // the bytes we want on the wire, in order
    switch (order) {
        case ORDER_RGB: w0 = c.r; w1 = c.g; w2 = c.b; break;
        case ORDER_BRG: w0 = c.b; w1 = c.r; w2 = c.g; break;
        case ORDER_RBG: w0 = c.r; w1 = c.b; w2 = c.g; break;
        case ORDER_GBR: w0 = c.g; w1 = c.b; w2 = c.r; break;
        case ORDER_BGR: w0 = c.b; w1 = c.g; w2 = c.r; break;
        case ORDER_GRB:
        default:        w0 = c.g; w1 = c.r; w2 = c.b; break;
    }
    Rgb out;
    out.g = w0;                     // the bus emits g first
    out.r = w1;
    out.b = w2;
    return out;
}

// Scale a colour proportionally so no channel exceeds maxBrightness.
// Proportional scaling preserves hue; a per-channel cap would not.
// Rounds to nearest, so a full channel survives even at maxBrightness == 1.
inline Rgb scaleBrightness(Rgb c, uint8_t maxBrightness) {
    Rgb out;
    out.r = (uint8_t)(((uint16_t)c.r * maxBrightness + 127) / 255);
    out.g = (uint8_t)(((uint16_t)c.g * maxBrightness + 127) / 255);
    out.b = (uint8_t)(((uint16_t)c.b * maxBrightness + 127) / 255);
    return out;
}

inline const char* colorOrderName(ColorOrder o) {
    switch (o) {
        case ORDER_RGB: return "RGB";
        case ORDER_BRG: return "BRG";
        case ORDER_RBG: return "RBG";
        case ORDER_GBR: return "GBR";
        case ORDER_BGR: return "BGR";
        case ORDER_GRB:
        default:        return "GRB";
    }
}

inline bool colorOrderFromName(const char* name, ColorOrder* out) {
    if (name == nullptr || out == nullptr) { return false; }
    for (uint8_t o = 0; o < ORDER_COUNT; o++) {
        const char* n = colorOrderName((ColorOrder)o);
        if (n[0] == name[0] && n[1] == name[1] && n[2] == name[2] && name[3] == '\0') {
            *out = (ColorOrder)o;
            return true;
        }
    }
    return false;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `powershell -ExecutionPolicy Bypass -File test/run_tests.ps1`
Expected: `0 failures`, exit 0.

If `scaleBrightness({255,200,0}, 128)` does not give exactly `(128, 100, 0)`, the rounding term is wrong — it must be `+ 127`, not truncation.

- [ ] **Step 5: Commit**

```bash
git add led_protocol.h test/test_led_protocol.cpp
git commit -m "feat: add colour-order permutation and proportional brightness scaling"
```

---

### Task 4: `LedBar` skeleton — config, strip init, dark output

First on-device deliverable. After this task the strip is wired up, initialises, obeys `enabled`, and its config round-trips through LittleFS — but nothing lights it yet.

**Files:**
- Create: `led_bar.h`
- Modify: `pins.h`
- Modify: `ota_pc_remote.ino`

**Interfaces:**
- Consumes: everything from `led_protocol.h`. Reads the globals `pcIsOn` (bool) and `powerState` (enum `PowerState`) declared by `pc_control.h`, and `PC_MONITOR_PIN` from `pins.h`.
- Produces:
  ```cpp
  struct LedConfig {
      bool       enabled;
      uint8_t    ledCount;
      uint8_t    maxBrightness;
      ColorOrder colorOrder;
      bool       idleAnimations;
  };
  enum SetConfigResult : uint8_t { SET_OK = 0, SET_INVALID, SET_SAVE_FAILED };
  class LedBar {
  public:
      void begin();
      void handle();
      SetConfigResult setConfig(const LedConfig& cfg);
      LedConfig getConfig() const;
      bool isStreaming() const;
      uint32_t lastPacketAge() const;         // ms, UINT32_MAX if never
      uint32_t droppedPackets() const;
      void onPowerOffImminent();
      void startTestPattern();
  };
  extern LedBar ledBar;
  ```

- [ ] **Step 1: Add the LED data pin**

In `pins.h`, before the closing `#endif`:

```cpp
// WS2812B LED BAR
#define LED_DATA_PIN 18 // >> WS2812B DIN via 1N4148 diode or 74AHCT125 level shifter
```

- [ ] **Step 2: Write `led_bar.h`**

```cpp
#ifndef LED_BAR_H
#define LED_BAR_H

#include <NeoPixelBus.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include "LittleFS.h"

#include "pins.h"
#include "led_protocol.h"

#define LED_UDP_PORT        21324
#define LED_CONFIG_PATH     "/led_config.json"
#define LED_FRAME_INTERVAL  20      // ms, 50 fps ceiling
#define LED_MIN_TIMEOUT_MS  1000

struct LedConfig {
    bool       enabled;
    uint8_t    ledCount;
    uint8_t    maxBrightness;
    ColorOrder colorOrder;
    bool       idleAnimations;
};

// SET_INVALID     -> reject with 400, config untouched
// SET_SAVE_FAILED -> 500, but the new config is applied in memory
enum SetConfigResult : uint8_t { SET_OK = 0, SET_INVALID, SET_SAVE_FAILED };

class LedBar {
public:
    void begin();
    void handle();
    SetConfigResult setConfig(const LedConfig& cfg);
    LedConfig getConfig() const { return config; }
    bool isStreaming() const { return streaming; }
    uint32_t lastPacketAge() const;
    uint32_t droppedPackets() const { return dropped; }
    void onPowerOffImminent();
    void startTestPattern();

private:
    LedConfig config = { true, 8, 200, ORDER_GRB, true };
    NeoPixelBus<NeoGrbFeature, NeoEsp32Rmt0Ws2812xMethod>* strip = nullptr;
    WiFiUDP udp;
    bool udpBound = false;
    bool streaming = false;
    uint32_t lastPacketMs = 0;
    uint32_t streamTimeoutMs = 2000;
    uint32_t dropped = 0;
    uint32_t lastFrameMs = 0;

    void loadConfig();
    bool saveConfig();              // false if LittleFS could not be written
    void initStrip();
    void bindUdp();
    void writeAll(Rgb c);
    void showPixels(const Rgb* px, uint8_t count);
};

inline void LedBar::loadConfig() {
    if (!LittleFS.exists(LED_CONFIG_PATH)) { saveConfig(); return; }

    File file = LittleFS.open(LED_CONFIG_PATH, "r");
    if (!file) { return; }

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err) { saveConfig(); return; }

    LedConfig c;
    c.enabled        = doc["enabled"] | true;
    c.ledCount       = doc["ledCount"] | 8;
    c.maxBrightness  = doc["maxBrightness"] | 200;
    c.idleAnimations = doc["idleAnimations"] | true;

    ColorOrder order = ORDER_GRB;
    const char* name = doc["colorOrder"] | "GRB";
    colorOrderFromName(name, &order);
    c.colorOrder = order;

    if (c.ledCount < 1 || c.ledCount > LED_MAX_COUNT) { c.ledCount = 8; }
    if (c.maxBrightness < 1) { c.maxBrightness = 200; }

    config = c;
}

inline bool LedBar::saveConfig() {
    File file = LittleFS.open(LED_CONFIG_PATH, "w");
    if (!file) { return false; }

    StaticJsonDocument<256> doc;
    doc["enabled"]        = config.enabled;
    doc["ledCount"]       = config.ledCount;
    doc["maxBrightness"]  = config.maxBrightness;
    doc["colorOrder"]     = colorOrderName(config.colorOrder);
    doc["idleAnimations"] = config.idleAnimations;

    size_t written = serializeJson(doc, file);
    file.close();
    return written > 0;
}

inline void LedBar::initStrip() {
    if (strip != nullptr) {
        delete strip;
        strip = nullptr;
    }
    strip = new NeoPixelBus<NeoGrbFeature, NeoEsp32Rmt0Ws2812xMethod>(
        config.ledCount, LED_DATA_PIN);
    strip->Begin();
    writeAll({ 0, 0, 0 });
}

inline void LedBar::writeAll(Rgb c) {
    if (strip == nullptr) { return; }
    Rgb out = applyColorOrder(scaleBrightness(c, config.maxBrightness),
                              config.colorOrder);
    for (uint8_t i = 0; i < config.ledCount; i++) {
        strip->SetPixelColor(i, RgbColor(out.r, out.g, out.b));
    }
    strip->Show();
}

inline void LedBar::showPixels(const Rgb* px, uint8_t count) {
    if (strip == nullptr) { return; }
    if (count > config.ledCount) { count = config.ledCount; }
    for (uint8_t i = 0; i < count; i++) {
        Rgb out = applyColorOrder(scaleBrightness(px[i], config.maxBrightness),
                                  config.colorOrder);
        strip->SetPixelColor(i, RgbColor(out.r, out.g, out.b));
    }
    strip->Show();
}

inline void LedBar::begin() {
    loadConfig();
    initStrip();

    // Always come up dark. The spec only requires darkness when the PC is off
    // (so a restart-while-off does not flash the strip), but blanking
    // unconditionally achieves that without reading PC_MONITOR_PIN, and the
    // idle animator picks the right colour on the very next handle() pass.
    writeAll({ 0, 0, 0 });

    Serial.printf("LED bar: %s, %u LEDs, maxBrightness %u, order %s\n",
                  config.enabled ? "enabled" : "disabled",
                  config.ledCount, config.maxBrightness,
                  colorOrderName(config.colorOrder));
}

inline SetConfigResult LedBar::setConfig(const LedConfig& cfg) {
    if (cfg.ledCount < 1 || cfg.ledCount > LED_MAX_COUNT) { return SET_INVALID; }
    if (cfg.maxBrightness < 1) { return SET_INVALID; }
    if (cfg.colorOrder >= ORDER_COUNT) { return SET_INVALID; }

    bool countChanged   = (cfg.ledCount != config.ledCount);
    bool becameDisabled = (config.enabled && !cfg.enabled);

    config = cfg;                       // in-memory config applies either way

    if (countChanged) {
        initStrip();                    // re-init blanks the strip, so no
                                        // orphaned tail pixels stay lit
    }
    if (becameDisabled) {
        writeAll({ 0, 0, 0 });          // one black frame, then handle() skips
    }

    return saveConfig() ? SET_OK : SET_SAVE_FAILED;
}

inline uint32_t LedBar::lastPacketAge() const {
    if (lastPacketMs == 0) { return UINT32_MAX; }
    return millis() - lastPacketMs;
}

inline void LedBar::onPowerOffImminent() { /* Task 6 */ }
inline void LedBar::startTestPattern()   { /* Task 8 */ }

inline void LedBar::handle() {
    if (!config.enabled) { return; }

    uint32_t now = millis();
    if (now - lastFrameMs < LED_FRAME_INTERVAL) { return; }
    lastFrameMs = now;
    // Stream handling: Task 5. Idle animation: Task 6.
}

LedBar ledBar;

#endif
```

- [ ] **Step 3: Wire it into the sketch**

In `ota_pc_remote.ino`, add immediately after the `#include "pc_control.h"` line (line 13):

```cpp
#include "led_bar.h"
```

**This position is load-bearing, in both directions.** `pc_control.h:9-19` defines
the `PowerState` enum and `pc_control.h:21,33` declare `extern bool pcIsOn;` and
`extern PowerState powerState;`, so everything `led_bar.h` reads is visible from
there — the definitions appear later in the sketch (`ota_pc_remote.ino:18,60`),
which is fine for a header that only reads them at runtime. And `web_server.h` is
included much later, at line 86, so Task 7's routes can see `LedConfig`,
`SetConfigResult` and the `ledBar` object. Moving this include earlier breaks the
first half; moving it after `web_server.h` breaks the second.

In `setup()`, immediately after the `setupWebServer();` call:

```cpp
    Serial.println("Setting up LED bar...");
    ledBar.begin();
```

At the very end of `loop()`:

```cpp
    ledBar.handle();
```

- [ ] **Step 4: Verify it compiles and runs**

Verify in Arduino IDE, then upload. On the serial monitor at 115200 expect a line like:

```
LED bar: enabled, 8 LEDs, maxBrightness 200, order GRB
```

The strip must be completely dark, and the existing boot sequence must be unchanged — WiFi connects, the web UI loads, a PS5 controller still pairs.

- [ ] **Step 5: Confirm the config file round-trips**

```bash
curl http://<esp-ip>/api/led/config
```

This will 404 until Task 7. For now confirm persistence by power-cycling the ESP32 and checking the serial line reports the same values.

- [ ] **Step 6: Commit**

```bash
git add led_bar.h pins.h ota_pc_remote.ino
git commit -m "feat: add LedBar module with config persistence and strip init"
```

---

### Task 5: DRGB stream rendering over UDP

**Files:**
- Modify: `led_bar.h`
- Create: `tools/drgb_test.py`

**Interfaces:**
- Consumes: `parseDrgb`, `DrgbFrame` from Task 2; `showPixels`, `writeAll`, `config`, `streaming`, `lastPacketMs`, `streamTimeoutMs`, `dropped` from Task 4.
- Produces: `isStreaming()` returns true while packets are arriving; `handle()` renders stream pixels.

- [ ] **Step 1: Add UDP binding and stream handling**

Replace the `bindUdp` declaration's body and the `handle()` stub in `led_bar.h`:

```cpp
inline void LedBar::bindUdp() {
    // In STA mode the socket must be re-bound after a reconnect, because the
    // interface goes down and takes the binding with it. In AP mode the softAP
    // interface is always up, so treat that as connected.
    bool linkUp = (WiFi.status() == WL_CONNECTED) ||
                  (WiFi.getMode() & WIFI_AP);

    if (!linkUp) {
        if (udpBound) {
            udp.stop();
            udpBound = false;
            Serial.println("LED bar: WiFi down, UDP unbound");
        }
        return;
    }

    if (udpBound) { return; }

    if (udp.begin(LED_UDP_PORT)) {
        udpBound = true;
        Serial.printf("LED bar: listening on UDP %d\n", LED_UDP_PORT);
    }
}

inline void LedBar::handle() {
    uint32_t now = millis();

    if (!config.enabled) {
        // A stray packet must never light a disabled strip, so do not even read.
        return;
    }

    bindUdp();

    // Drain the socket; the newest complete frame wins.
    DrgbFrame latest;
    latest.valid = false;
    bool got = false;
    if (udpBound) {
        int size;
        while ((size = udp.parsePacket()) > 0) {
            uint8_t buf[DRGB_MAX_PACKET + 16];
            int n = udp.read(buf, sizeof(buf));
            if (n <= 0) { continue; }
            DrgbFrame f = parseDrgb(buf, (size_t)n, config.ledCount);
            if (!f.valid) { dropped++; continue; }
            latest = f;
            got = true;
        }
    }

    if (got) {
        lastPacketMs = now == 0 ? 1 : now;
        uint32_t t = (uint32_t)latest.timeoutSec * 1000;
        streamTimeoutMs = t > LED_MIN_TIMEOUT_MS ? t : LED_MIN_TIMEOUT_MS;
        streaming = true;
    } else if (streaming && (now - lastPacketMs) > streamTimeoutMs) {
        streaming = false;
    }

    if (now - lastFrameMs < LED_FRAME_INTERVAL) { return; }
    lastFrameMs = now;

    if (streaming) {
        // A header-only keepalive has ledCount 0: refresh liveness, change
        // nothing on the strip.
        if (got && latest.ledCount > 0) {
            showPixels(latest.pixels, latest.ledCount);
        }
        return;
    }

    // Idle animation: Task 6.
    writeAll({ 0, 0, 0 });
}
```

Add `#include <WiFi.h>` to the includes at the top of `led_bar.h`.

- [ ] **Step 2: Write the test packet generator**

`tools/drgb_test.py`:

```python
#!/usr/bin/env python3
"""Send WLED DRGB realtime packets to the ESP32 LED bar client.

Usage:
  python tools/drgb_test.py <esp-ip> solid <r> <g> <b>
  python tools/drgb_test.py <esp-ip> rainbow [seconds]
  python tools/drgb_test.py <esp-ip> soak [seconds]
  python tools/drgb_test.py <esp-ip> malformed
"""
import colorsys
import socket
import sys
import time

PORT = 21324
DRGB = 2
TIMEOUT_SEC = 2
NUM_LEDS = 8


def packet(pixels):
    buf = bytearray(2 + len(pixels) * 3)
    buf[0] = DRGB
    buf[1] = TIMEOUT_SEC
    for i, (r, g, b) in enumerate(pixels):
        buf[2 + i * 3] = r & 0xFF
        buf[3 + i * 3] = g & 0xFF
        buf[4 + i * 3] = b & 0xFF
    return bytes(buf)


def send(sock, host, data):
    sock.sendto(data, (host, PORT))


def solid(sock, host, r, g, b, seconds=5):
    data = packet([(r, g, b)] * NUM_LEDS)
    end = time.time() + seconds
    while time.time() < end:
        send(sock, host, data)
        time.sleep(1 / 30)


def rainbow(sock, host, seconds=10):
    end = time.time() + seconds
    phase = 0.0
    while time.time() < end:
        px = []
        for i in range(NUM_LEDS):
            h = (phase + i / NUM_LEDS) % 1.0
            r, g, b = colorsys.hsv_to_rgb(h, 1.0, 1.0)
            px.append((int(r * 255), int(g * 255), int(b * 255)))
        send(sock, host, packet(px))
        phase = (phase + 0.01) % 1.0
        time.sleep(1 / 30)


def soak(sock, host, seconds=60):
    end = time.time() + seconds
    frames = 0
    while time.time() < end:
        v = frames % 256
        send(sock, host, packet([(v, 255 - v, 128)] * NUM_LEDS))
        frames += 1
        time.sleep(1 / 30)
    print(f"sent {frames} frames in {seconds}s")


def malformed(sock, host):
    cases = [
        ("empty", b""),
        ("one byte", bytes([2])),
        ("header only keepalive", bytes([2, 2])),
        ("wrong protocol WARLS", bytes([1, 2, 255, 0, 0])),
        ("wrong protocol DRGBW", bytes([3, 2, 255, 0, 0])),
        ("wrong protocol DNRGB", bytes([4, 2, 0, 0, 255, 0, 0])),
        ("oversized", bytes([2, 2]) + bytes(18 * 3)),
        ("partial pixel", bytes([2, 2, 9, 9, 9, 7, 7])),
        ("zero timeout", bytes([2, 0, 0, 255, 0])),
    ]
    for name, data in cases:
        print(f"sending {name} ({len(data)} bytes)")
        send(sock, host, data)
        time.sleep(0.5)
    print("strip must still be responsive; check droppedPackets via "
          "GET /api/led/config")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    host = sys.argv[1]
    mode = sys.argv[2]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if mode == "solid":
        r, g, b = (int(x) for x in sys.argv[3:6])
        solid(sock, host, r, g, b)
    elif mode == "rainbow":
        rainbow(sock, host, int(sys.argv[3]) if len(sys.argv) > 3 else 10)
    elif mode == "soak":
        soak(sock, host, int(sys.argv[3]) if len(sys.argv) > 3 else 60)
    elif mode == "malformed":
        malformed(sock, host)
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Verify stream rendering on the device**

Upload, then from this machine:

```bash
python tools/drgb_test.py <esp-ip> solid 255 0 0
```

Expected: the strip lights for 5 s and goes dark ~2 s after the script stops. If the colour is not red, the strip's true colour order is not GRB — that is Task 8's dropdown, not a bug here. Note which colour appeared.

```bash
python tools/drgb_test.py <esp-ip> rainbow 10
```

Expected: a smooth sweep with no flicker and no stutter.

- [ ] **Step 4: Verify the `enabled: false` guard (Review Focus 1)**

Edit `/led_config.json` to `"enabled": false` (via Task 7's API once it exists; for now reflash with the default changed), reboot, then:

```bash
python tools/drgb_test.py <esp-ip> solid 255 255 255
```

Expected: the strip stays **completely dark** for the whole run.

- [ ] **Step 5: Verify malformed packets are survived**

```bash
python tools/drgb_test.py <esp-ip> malformed
```

Expected: no crash, no reboot (watch the serial monitor for a boot banner), and the strip still responds to a following `solid` run.

- [ ] **Step 6: Commit**

```bash
git add led_bar.h tools/drgb_test.py
git commit -m "feat: render DRGB packets received over UDP 21324"
```

---

### Task 6: Idle animator and the non-blocking power-off fade

**Files:**
- Modify: `led_bar.h`
- Modify: `pc_control.h:130-140`

**Interfaces:**
- Consumes: `pcIsOn`, `powerState` and the `PowerState` enum from `pc_control.h`; `writeAll`, `config` from Task 4; the `streaming` flag from Task 5.
- Produces: `onPowerOffImminent()` starts the fade-out. `handle()` renders a power-state animation whenever the stream is not live.

- [ ] **Step 1: Implement the idle animator**

In `led_bar.h`, add to the private section:

```cpp
    bool fadingOut = false;
    uint32_t fadeOutStartMs = 0;
    bool darkLatched = false;       // set by /win&T=0, cleared by any DRGB packet
    PowerState lastSeenPowerState = POWER_IDLE;
    uint32_t stateEnteredMs = 0;    // when the current power state began

    Rgb idleColor(uint32_t now);
```

`stateEnteredMs` is a member, not a function-local `static`. A `static` inside the animation branch would be initialised once for the lifetime of the program, so the second power-on of a session would see a huge elapsed time and snap straight to full amber with no fade.

Then add the implementation, and replace the `onPowerOffImminent` stub:

```cpp
// Steam blue, used for the "powered on, waiting for the bridge" breath.
#define LED_STEAM_R 0x00
#define LED_STEAM_G 0xAE
#define LED_STEAM_B 0xEF

static inline bool isPoweringOn(PowerState s) {
    return s == POWER_ON_START || s == POWER_ON_WAITING_RELAY2 ||
           s == POWER_ON_COMPLETE;
}

static inline bool isShuttingDown(PowerState s) {
    return s == POWER_OFF_START || s == POWER_OFF_WAITING ||
           s == POWER_OFF_WAITING_POWEROFF || s == POWER_FORCE_START ||
           s == POWER_FORCE_WAITING;
}

inline Rgb LedBar::idleColor(uint32_t now) {
    const Rgb steam = { LED_STEAM_R, LED_STEAM_G, LED_STEAM_B };

    // Restart animations on every power-state transition.
    if (powerState != lastSeenPowerState) {
        // POWER_ON_* has three sub-states. Treat the whole power-on sequence as
        // one animation so the amber fade-in is not restarted two extra times
        // as the relay sequence advances.
        bool sameGroup = isPoweringOn(powerState) && isPoweringOn(lastSeenPowerState);
        lastSeenPowerState = powerState;
        if (!sameGroup) { stateEnteredMs = now; }

        if (isShuttingDown(powerState)) {
            fadingOut = true;
            fadeOutStartMs = now;
        } else if (isPoweringOn(powerState)) {
            fadingOut = false;
        }
        // On a transition to POWER_IDLE, leave fadingOut alone: pc_control.h
        // calls ledBarPowerOffImminent() while already in POWER_IDLE, and
        // clearing the flag here would cancel that fade before it renders.
    }

    if (darkLatched) { return { 0, 0, 0 }; }
    if (!config.idleAnimations) { return { 0, 0, 0 }; }

    if (fadingOut) {
        uint32_t elapsed = now - fadeOutStartMs;
        if (elapsed >= 1500) { return { 0, 0, 0 }; }
        uint8_t k = (uint8_t)(255 - (elapsed * 255) / 1500);
        return scaleBrightness(steam, k);
    }

    if (isPoweringOn(powerState)) {
        // Warm amber fading in over 2 s, then holding until the state changes.
        uint32_t elapsed = now - stateEnteredMs;
        uint8_t k = elapsed >= 2000 ? 255 : (uint8_t)((elapsed * 255) / 2000);
        return scaleBrightness({ 255, 140, 20 }, k);
    }

    if (!pcIsOn) { return { 0, 0, 0 }; }

    // PSU on, ESP32 alive, bridge not streaming: slow Steam-blue breath,
    // 4 s period. This is the state you will be diagnosing most often.
    uint32_t t = now % 4000;
    uint32_t tri = t < 2000 ? t : (4000 - t);           // 0..2000..0
    uint8_t k = (uint8_t)(16 + (tri * (255 - 16)) / 2000);
    return scaleBrightness(steam, k);
}

inline void LedBar::onPowerOffImminent() {
    fadingOut = true;
    fadeOutStartMs = millis();
}
```

In `handle()`, replace the final `writeAll({ 0, 0, 0 });` with:

```cpp
    writeAll(idleColor(now));
```

And in the `if (got)` block, clear the latch, since any DRGB packet unlatches a `/win&T=0` dark hold:

```cpp
        darkLatched = false;
        fadingOut = false;
```

- [ ] **Step 2: Make the pre-restart delay non-blocking**

`pc_control.h` is included *before* `led_bar.h` in the sketch, so the `LedBar`
type does not exist yet at that point and `ledBar.handle()` cannot be called
there — a forward declaration of the class is not enough to call methods on an
incomplete type. Two free functions bridge the gap instead.

First, declare them near the top of `pc_control.h`, just after the include guard:

```cpp
// Defined in led_bar.h, which is included after this header.
void ledBarPowerOffImminent();
void ledBarPump();
```

Then replace these lines in `pc_control.h` (around line 134-138):

```cpp
                Serial.println("PC powered off - restarting ESP32 in 2 seconds...");
                delay(2000);
                ESP.restart();
```

with:

```cpp
                Serial.println("PC powered off - restarting ESP32 in 2 seconds...");
                ledBarPowerOffImminent();
                unsigned long fadeStart = millis();
                while (millis() - fadeStart < 2000) {
                    ledBarPump();
                    delay(5);
                }
                ESP.restart();
```

Finally, define them at the bottom of `led_bar.h`, after the `ledBar` object:

```cpp
void ledBarPowerOffImminent() { ledBar.onPowerOffImminent(); }
void ledBarPump()             { ledBar.handle(); }
```

- [ ] **Step 3: Verify the compile**

Verify in Arduino IDE. Expected: compiles clean. A "use of undefined type 'LedBar'" error means the free-function indirection above was not applied.

- [ ] **Step 4: Verify the animations on the device**

With the bridge not running:

| Action | Expected strip behaviour |
|---|---|
| PC off, idle | dark |
| Press the power button to power on | amber fading in over ~2 s, then holding |
| BC-250 booted, bridge not installed | Steam-blue breathing, 4 s period |
| Run `tools/drgb_test.py <ip> rainbow 10` | rainbow takes over immediately, breathing returns ~2 s after it stops |
| Power off | fade to black over ~1.5 s, **then** the reboot banner on serial |

That last row is the point of the `pc_control.h` change — before it, the fade never rendered. Confirm on the serial monitor that `PC powered off - restarting ESP32 in 2 seconds...` appears while the strip is still visibly fading.

- [ ] **Step 5: Commit**

```bash
git add led_bar.h pc_control.h
git commit -m "feat: mirror power state on the strip when no stream is live"
```

---

### Task 7: Web API routes

**Files:**
- Modify: `web_server.h`

**Interfaces:**
- Consumes: `ledBar.getConfig()`, `setConfig()`, `isStreaming()`, `lastPacketAge()`, `droppedPackets()`, `startTestPattern()` from Tasks 4–6; `colorOrderName`, `colorOrderFromName` from Task 3.
- Produces: the five routes below, consumed by Task 8's UI.

- [ ] **Step 1: Add the routes**

In `web_server.h`, inside `setupWebServer()` alongside the existing `/api/ps5/config` handlers:

```cpp
    server.on("/api/led/config", HTTP_GET, []() {
        LedConfig c = ledBar.getConfig();
        StaticJsonDocument<384> doc;
        doc["enabled"]        = c.enabled;
        doc["ledCount"]       = c.ledCount;
        doc["maxBrightness"]  = c.maxBrightness;
        doc["colorOrder"]     = colorOrderName(c.colorOrder);
        doc["idleAnimations"] = c.idleAnimations;
        doc["streaming"]      = ledBar.isStreaming();
        uint32_t age = ledBar.lastPacketAge();
        doc["lastPacketAge"]  = (age == UINT32_MAX) ? -1 : (int32_t)age;
        doc["droppedPackets"] = ledBar.droppedPackets();
        doc["maxLedCount"]    = LED_MAX_COUNT;

        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
    });

    server.on("/api/led/config", HTTP_POST, []() {
        if (!server.hasArg("plain")) {
            server.send(400, "application/json", "{\"error\":\"no body\"}");
            return;
        }
        StaticJsonDocument<384> doc;
        if (deserializeJson(doc, server.arg("plain"))) {
            server.send(400, "application/json", "{\"error\":\"bad json\"}");
            return;
        }

        LedConfig c = ledBar.getConfig();
        if (doc.containsKey("enabled"))        { c.enabled = doc["enabled"]; }
        if (doc.containsKey("ledCount"))       { c.ledCount = doc["ledCount"]; }
        if (doc.containsKey("maxBrightness"))  { c.maxBrightness = doc["maxBrightness"]; }
        if (doc.containsKey("idleAnimations")) { c.idleAnimations = doc["idleAnimations"]; }
        if (doc.containsKey("colorOrder")) {
            ColorOrder o;
            // Cast explicitly: colorOrderFromName takes const char*, and a bare
            // JsonVariant will not convert on its own.
            const char* name = doc["colorOrder"].as<const char*>();
            if (!colorOrderFromName(name, &o)) {
                server.send(400, "application/json",
                            "{\"error\":\"colorOrder must be one of "
                            "GRB RGB BRG RBG GBR BGR\"}");
                return;
            }
            c.colorOrder = o;
        }

        SetConfigResult res = ledBar.setConfig(c);
        if (res == SET_INVALID) {
            server.send(400, "application/json",
                        "{\"error\":\"ledCount must be 1-17, "
                        "maxBrightness 1-255\"}");
            return;
        }
        if (res == SET_SAVE_FAILED) {
            // Applied in memory, but it will not survive a reboot.
            server.send(500, "application/json",
                        "{\"error\":\"could not write /led_config.json\","
                        "\"appliedInMemory\":true}");
            return;
        }
        server.send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/led/test", HTTP_POST, []() {
        ledBar.startTestPattern();
        server.send(200, "application/json", "{\"ok\":true}");
    });

    // The steamos-led-wled bridge's power control calls these literally, with
    // an "&" and no "?", so the whole string is the request path and there is
    // no query to parse.
    server.on("/win&T=1", HTTP_GET, []() {
        ledBar.setDarkLatched(false);
        server.send(200, "text/plain", "OK");
    });

    server.on("/win&T=0", HTTP_GET, []() {
        ledBar.setDarkLatched(true);
        server.send(200, "text/plain", "OK");
    });
```

- [ ] **Step 2: Add the latch setter**

In `led_bar.h`, add to the public section:

```cpp
    void setDarkLatched(bool latched) {
        darkLatched = latched;
        if (latched) {
            fadingOut = true;
            fadeOutStartMs = millis();
        } else {
            fadingOut = false;
        }
    }
```

- [ ] **Step 3: Verify the happy path**

```bash
curl http://<esp-ip>/api/led/config
curl -X POST -H "Content-Type: application/json" \
     -d '{"ledCount":8,"maxBrightness":150,"colorOrder":"RGB"}' \
     http://<esp-ip>/api/led/config
curl http://<esp-ip>/api/led/config
```

Expected: the second GET reflects the new values. Power-cycle and GET again — the values must persist.

- [ ] **Step 4: Verify validation rejects bad input**

```bash
curl -i -X POST -H "Content-Type: application/json" -d '{"ledCount":0}'   http://<esp-ip>/api/led/config
curl -i -X POST -H "Content-Type: application/json" -d '{"ledCount":18}'  http://<esp-ip>/api/led/config
curl -i -X POST -H "Content-Type: application/json" -d '{"maxBrightness":0}' http://<esp-ip>/api/led/config
curl -i -X POST -H "Content-Type: application/json" -d '{"colorOrder":"WAT"}' http://<esp-ip>/api/led/config
curl -i -X POST -H "Content-Type: application/json" -d 'not json'         http://<esp-ip>/api/led/config
```

Expected: all five return **400**, and a following GET shows the config unchanged.

- [ ] **Step 5: Verify the orphaned-tail case (Review Focus 2)**

```bash
python tools/drgb_test.py <esp-ip> solid 255 255 255 &
curl -X POST -H "Content-Type: application/json" -d '{"ledCount":3}' http://<esp-ip>/api/led/config
```

Expected: only the first 3 LEDs stay lit. LEDs 4 onward must go dark, not stay frozen on white from the previous frame. If they stay lit, `initStrip()` is not being called on a count change, or it is not blanking.

- [ ] **Step 6: Verify the dark latch and its release (Review Focus 4)**

```bash
curl "http://<esp-ip>/win&T=0"
python tools/drgb_test.py <esp-ip> solid 0 255 0
```

Expected: `T=0` fades the strip out and holds it dark; the following DRGB run lights it green anyway, because any DRGB packet unlatches. Then:

```bash
curl "http://<esp-ip>/win&T=0"
curl "http://<esp-ip>/win&T=1"
```

Expected: dark, then the idle animation resumes.

- [ ] **Step 7: Commit**

```bash
git add web_server.h led_bar.h
git commit -m "feat: add LED bar config API and WLED power-control shim"
```

---

### Task 7B: Upsample 17 source pixels to a 24-LED strip

**Added mid-execution on 2026-10-09**, after the user reported their physical strip has 24 LEDs. The full task text lives in the execution workspace at `.superpowers/sdd/2026-10-08-esp32-led-bar-client/task-7b-brief.md` and is summarised here so the plan remains the authority.

The Valve LED bar has exactly 17 pixels and nothing upstream upsamples: the bridge's `remap_snapshot()` only downsamples, and its `extract_pixels()` would index past the end of the 100-byte snapshot at `i = 17`, raising `IndexError`. So `num_leds = 24` crashes the bridge rather than working. The host must stay at 17 and the stretch has to happen in our firmware.

Changes: `LED_SRC_COUNT` (17) is split from `LED_MAX_COUNT` (now 24); `DRGB_MAX_PACKET` becomes 74 so a 24-LED packet from HyperHDR or Hyperion is accepted rather than dropped; a new pure `upsamplePixels()` in `led_protocol.h` stretches the received pixels with 8.8 fixed-point linear interpolation, preserving both endpoints exactly; `handle()` applies it when the frame carries fewer pixels than the configured strip length; the API's error messages move from 1-17 to 1-24.

Defaults stay at `ledCount` 8 and `maxBrightness` 200 deliberately — 24 LEDs at full white draws roughly 1.4 A, beyond the dev board regulator, so first boot stays conservative and the user sets 24 once in the web UI.

Idle animations need no change: they already render through `writeAll()` over `config.ledCount`.

---

### Task 8: Web UI card and the test pattern

**Files:**
- Modify: `data/setup.html`
- Modify: `led_bar.h`

**Interfaces:**
- Consumes: the routes from Task 7.
- Produces: the user-facing way to fix colour order without recompiling.

- [ ] **Step 1: Implement the test pattern**

In `led_bar.h`, add to the private section:

```cpp
    uint32_t testStartMs = 0;       // 0 = not running
```

Replace the `startTestPattern` stub:

```cpp
inline void LedBar::startTestPattern() {
    testStartMs = millis();
    if (testStartMs == 0) { testStartMs = 1; }
}
```

In `handle()`, immediately after `lastFrameMs = now;`, add:

```cpp
    if (testStartMs != 0) {
        uint32_t elapsed = now - testStartMs;
        if (elapsed < 1000)      { writeAll({ 255, 0, 0 }); return; }
        else if (elapsed < 2000) { writeAll({ 0, 255, 0 }); return; }
        else if (elapsed < 3000) { writeAll({ 0, 0, 255 }); return; }
        testStartMs = 0;
    }
```

The test pattern goes through `writeAll`, so `maxBrightness` still clamps it — it cannot be used to command full white.

- [ ] **Step 2: Add the UI card**

(The LED-count field's `max` is **24** per Task 7B, not 17.)

In `data/setup.html`, following the existing PS5 configuration card and matching its markup and class names:

```html
<div class="card">
  <h2>LED Bar</h2>
  <p class="hint">
    Mirrors the Steam Machine LED bar from a WS2812B strip on GPIO 18.
    Install <code>steamos-led-wled</code> on the BC-250 and point it at this
    device's IP.
  </p>

  <label><input type="checkbox" id="ledEnabled"> Enable LED bar</label>

  <label>LED count
    <input type="number" id="ledCount" min="1" max="24" step="1">
  </label>

  <label>Max brightness
    <input type="range" id="ledBrightness" min="1" max="255">
    <span id="ledBrightnessVal"></span>
  </label>

  <label>Colour order
    <select id="ledColorOrder">
      <option value="GRB">GRB (default)</option>
      <option value="RGB">RGB</option>
      <option value="BRG">BRG</option>
      <option value="RBG">RBG</option>
      <option value="GBR">GBR</option>
      <option value="BGR">BGR</option>
    </select>
  </label>

  <label><input type="checkbox" id="ledIdle"> Power-state animations when idle</label>

  <div class="row">
    <button id="ledSave">Save</button>
    <button id="ledTest">Test strip</button>
  </div>

  <p class="hint" id="ledStatus"></p>
  <p class="hint">
    Test strip shows red, then green, then blue for one second each. If the
    colours come out in a different order, pick the order you actually saw.
  </p>
</div>

<script>
async function ledLoad() {
  const r = await fetch('/api/led/config');
  const c = await r.json();
  document.getElementById('ledEnabled').checked = c.enabled;
  document.getElementById('ledCount').value = c.ledCount;
  document.getElementById('ledBrightness').value = c.maxBrightness;
  document.getElementById('ledBrightnessVal').textContent = c.maxBrightness;
  document.getElementById('ledColorOrder').value = c.colorOrder;
  document.getElementById('ledIdle').checked = c.idleAnimations;
  document.getElementById('ledStatus').textContent =
    (c.streaming ? 'Streaming from host' : 'No stream') +
    ' | dropped packets: ' + c.droppedPackets;
}

document.getElementById('ledBrightness').addEventListener('input', e => {
  document.getElementById('ledBrightnessVal').textContent = e.target.value;
});

document.getElementById('ledSave').addEventListener('click', async () => {
  const body = {
    enabled: document.getElementById('ledEnabled').checked,
    ledCount: parseInt(document.getElementById('ledCount').value, 10),
    maxBrightness: parseInt(document.getElementById('ledBrightness').value, 10),
    colorOrder: document.getElementById('ledColorOrder').value,
    idleAnimations: document.getElementById('ledIdle').checked
  };
  const r = await fetch('/api/led/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body)
  });
  const res = await r.json();
  document.getElementById('ledStatus').textContent =
    r.ok ? 'Saved' : ('Error: ' + (res.error || r.status));
  if (r.ok) { ledLoad(); }
});

document.getElementById('ledTest').addEventListener('click', () => {
  fetch('/api/led/test', { method: 'POST' });
});

ledLoad();
setInterval(ledLoad, 5000);
</script>
```

- [ ] **Step 3: Upload the filesystem**

The sketch serves `setup.html` from LittleFS, so changing it requires a filesystem upload, not just a sketch upload. Use the existing `/update-fs` endpoint in `web_server.h:327`, or the Arduino IDE's LittleFS upload tool.

- [ ] **Step 4: Verify the UI end to end**

Open `http://<esp-ip>/setup`. Then:

1. Click **Test strip**. Note the actual colour sequence.
2. If it was not red → green → blue, pick the order matching what you saw, Save, and click Test strip again. It must now be red → green → blue.
3. Change LED count, Save, reload the page — the value persists.
4. Drag brightness to 10, click Test strip — visibly dim.
5. Set LED count to 0 in the field and Save — the browser's `min="1"` should block it; if it submits, the status line must show the 400's error text and the config must not change.

- [ ] **Step 5: Commit**

```bash
git add data/setup.html led_bar.h
git commit -m "feat: add LED bar card with colour-order picker and test pattern"
```

---

### Task 9: Documentation and version bump

**Files:**
- Modify: `version.h`
- Modify: `README.md`

- [ ] **Step 1: Bump the version**

In `version.h`:

```cpp
const String VERSION = "1.7.0";
```

- [ ] **Step 2: Document the feature**

Add to `README.md`, after the existing "Install libraries" section:

```markdown
# LED Bar (optional)

Mirrors the Steam Machine LED bar on a WS2812B strip driven by this ESP32, so
one device handles both power control and the LED bar.

## Extra library

- [NeoPixelBus by Makuna](https://github.com/Makuna/NeoPixelBus) — 2.8.0 or
  newer (earlier versions do not support the ESP32 Arduino 3.x RMT driver)

## Wiring

| ESP32 pin | Connection |
|---|---|
| GPIO 18 | WS2812B DIN, via a 1N4148 diode (anode to GPIO) or a 74AHCT125 level shifter |
| GND | strip GND |
| 5V | strip VCC — for more than 8 LEDs use an external 5 V supply sharing GND |

A 1000 uF capacitor across the strip's VCC/GND near the first LED is recommended
for longer strips.

## Host side

Install [steamos-led-wled](https://github.com/rpf16rj/steamos-led-wled) on the
BC-250 and point it at this device:

```bash
git clone https://github.com/rpf16rj/steamos-led-wled.git
cd steamos-led-wled
sudo ./install.sh
```

Set `wled_host` to the ESP32's IP in `/etc/steamos-led-wled.conf`, and
`num_leds` to match the LED count configured in the web UI.

Only the **DRGB** protocol mode is supported (WARLS, DRGBW and DNRGB packets are
ignored). Because DRGB is the standard WLED realtime protocol, HyperHDR,
Hyperion and Prismatik can also drive the strip.

## Configure

Open `http://<esp-ip>/setup` and use the **LED Bar** card to set the LED count,
maximum brightness and colour order.

Cheap clone WS2812B strips do not always use GRB order. Click **Test strip**: it
shows red, then green, then blue for one second each. If the colours come out in
a different order, select the order you actually saw and save.

## Troubleshooting

| Problem | Cause |
|---|---|
| Strip breathes blue and never shows Game Mode colours | The bridge is not streaming. Check `sudo systemctl status steamos-led-wled` on the BC-250 |
| Colours wrong (red shows as green, etc.) | Clone colour order. Use Test strip and the colour-order dropdown |
| First LED flickers or colours are unstable | Missing diode or level shifter on the data line, or no shared ground |
| Strip dim but correct | `Max brightness` is low, or the strip is underpowered — use an external 5 V supply |
| Nothing at all | Confirm the strip is enabled in the web UI and `droppedPackets` is not climbing (wrong protocol mode) |
```

- [ ] **Step 3: Verify the documented numbers match the code**

```bash
grep -n "1.7.0" version.h
grep -n "LED_DATA_PIN" pins.h
grep -n "LED_UDP_PORT" led_bar.h
```

Expected: version 1.7.0, pin 18, port 21324 — matching the README.

- [ ] **Step 4: Full regression on hardware**

1. Power on via the web UI — amber fade, then blue breathing.
2. Start the bridge on the BC-250, open Game Mode → Personalization → LED settings, change colour and effect. The strip follows.
3. Connect a PS5 controller and power the PC on with the PS button while the strip is streaming. Both still work.
4. `python tools/drgb_test.py <esp-ip> soak 300` — five minutes, no flicker, no reboot, controller still responsive.
5. Pull the router's power for 30 s, restore it. The ESP32 reconnects and the strip resumes.
6. Power off from the web UI. The strip fades out, then the reboot banner appears on serial.

- [ ] **Step 5: Commit**

```bash
git add version.h README.md
git commit -m "docs: document the LED bar feature and bump to 1.7.0"
```
