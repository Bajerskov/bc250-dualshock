#ifndef LED_BAR_H
#define LED_BAR_H

#include <WiFi.h>
#include <NeoPixelBus.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include "LittleFS.h"

#include "pc_control.h"
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
    DrgbFrame pendingFrame;         // newest frame WITH pixels, awaiting render
    bool pendingValid = false;      // gates pendingFrame; it is otherwise unread
    bool fadingOut = false;
    uint32_t fadeOutStartMs = 0;
    bool darkLatched = false;       // set by /win&T=0, cleared by any DRGB packet
    PowerState lastSeenPowerState = POWER_IDLE;
    uint32_t stateEnteredMs = 0;    // when the current power state began

    Rgb idleColor(uint32_t now);

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
    c.idleAnimations = doc["idleAnimations"] | true;

    int rawCount      = doc["ledCount"] | 8;
    int rawBrightness = doc["maxBrightness"] | 200;

    // Range-check BEFORE narrowing to uint8_t: a hand-edited file containing
    // 257 would otherwise wrap to 1 and pass validation as a one-LED strip.
    if (rawCount < 1 || rawCount > LED_MAX_COUNT) { rawCount = 8; }
    if (rawBrightness < 1 || rawBrightness > 255) { rawBrightness = 200; }

    c.ledCount      = (uint8_t)rawCount;
    c.maxBrightness = (uint8_t)rawBrightness;

    ColorOrder order = ORDER_GRB;
    const char* name = doc["colorOrder"] | "GRB";
    colorOrderFromName(name, &order);
    c.colorOrder = order;

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
    // Idempotent: a POWER_OFF_* / POWER_FORCE_* transition has usually already
    // started (and often finished) this fade by the time pc_control.h calls us
    // at POWER_IDLE. Resetting the timer here would jump the strip from black
    // back to full brightness and fade a second time, a visible flash right
    // before the restart.
    if (!fadingOut) {
        fadingOut = true;
        fadeOutStartMs = millis();
    }
}
inline void LedBar::startTestPattern()   { /* Task 8 */ }

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

    // Drain the socket. The newest PIXEL-BEARING frame wins and is held in a
    // member, because handle() runs far more often than it renders: draining
    // into a local and returning early at the frame-interval check below would
    // consume and discard most frames.
    bool gotAny = false;            // any valid packet, keepalives included
    uint8_t latestTimeout = 0;
    if (udpBound) {
        int size;
        uint8_t drained = 0;
        while (drained < 16 && (size = udp.parsePacket()) > 0) {
            drained++;
            uint8_t buf[DRGB_MAX_PACKET + 16];
            int n = udp.read(buf, sizeof(buf));
            if (n <= 0) { continue; }
            DrgbFrame f = parseDrgb(buf, (size_t)n, config.ledCount);
            if (!f.valid) { dropped++; continue; }

            gotAny = true;              // liveness, even for a keepalive
            latestTimeout = f.timeoutSec;

            if (f.ledCount > 0) {       // a keepalive must not suppress a frame
                pendingFrame = f;
                pendingValid = true;
            }
        }
    }

    if (gotAny) {
        lastPacketMs = now == 0 ? 1 : now;
        uint32_t t = (uint32_t)latestTimeout * 1000;
        streamTimeoutMs = t > LED_MIN_TIMEOUT_MS ? t : LED_MIN_TIMEOUT_MS;
        streaming = true;
        darkLatched = false;
        fadingOut = false;
    } else if (streaming && (now - lastPacketMs) > streamTimeoutMs) {
        streaming = false;
    }

    if (now - lastFrameMs < LED_FRAME_INTERVAL) { return; }
    lastFrameMs = now;

    if (streaming) {
        // Render the newest frame we have been holding. A keepalive-only
        // interval leaves pendingValid false, so the strip holds its last
        // frame rather than being rewritten or blanked.
        if (pendingValid) {
            showPixels(pendingFrame.pixels, pendingFrame.ledCount);
            pendingValid = false;
        }
        return;
    }

    writeAll(idleColor(now));
}

extern LedBar ledBar;

inline void ledBarPowerOffImminent() { ledBar.onPowerOffImminent(); }
inline void ledBarPump()             { ledBar.handle(); }

#endif
