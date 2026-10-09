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
