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

int main() {
    test_parse_drgb();
    test_constants();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
