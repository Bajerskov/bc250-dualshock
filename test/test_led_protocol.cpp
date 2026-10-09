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
