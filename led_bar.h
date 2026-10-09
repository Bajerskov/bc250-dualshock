#ifndef LED_BAR_H
#define LED_BAR_H

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
