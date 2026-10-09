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

    // Boundary: 54 and 55 bytes are just past the 53-byte maximum and must be
    // DROPPED, not truncated down to 17 pixels. A regression to "truncate"
    // would pass every other oversize test in this file.
    uint8_t over54[54] = { 2, 2 };
    CHECK(!parseDrgb(over54, sizeof(over54), 17).valid);
    uint8_t over55[55] = { 2, 2 };
    CHECK(!parseDrgb(over55, sizeof(over55), 17).valid);

    // The receive buffer in the UDP path is DRGB_MAX_PACKET + 16 = 69 bytes, so
    // pin that length too: a read that fills the buffer must still be dropped.
    uint8_t over69[69] = { 2, 2 };
    CHECK(!parseDrgb(over69, sizeof(over69), 17).valid);
}

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

int main() {
    test_parse_drgb();
    test_constants();
    test_color_order();
    test_scale_brightness();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
