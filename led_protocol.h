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

// The 100-byte snapshot the host bridge reads from the kernel module carries 17
// pixels, and the DRGB packet we receive must match that on the wire. These are
// a contract with the host, so pin them at compile time -- on the ESP32 build
// as well as the host tests, where a runtime CHECK would never run.
static_assert(LED_MAX_COUNT == 17, "LED_MAX_COUNT is fixed by the Valve LED bar");
static_assert(DRGB_PROTOCOL_ID == 2, "DRGB is WLED realtime protocol 2");
static_assert(DRGB_HEADER_SIZE == 2, "DRGB header is protocol id + timeout");
static_assert(DRGB_MAX_PACKET == 53, "2 header bytes + 17 pixels * 3 bytes");

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

#endif
