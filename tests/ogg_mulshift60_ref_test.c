#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#define _M68K_ASSEM_ 1
#define MINTAMP_OGG_TEST_68060 1
#include "../decoders/ogg_m68k_68060.h"

static uint32_t rng_state = 0x4f676736U;

static uint32_t next_random(void)
{
    rng_state = rng_state * 1664525U + 1013904223U;
    return rng_state;
}

static int check_pair(int32_t x, int32_t y)
{
    int64_t product = (int64_t)x * (int64_t)y;
    int32_t expected32 = (int32_t)(product >> 32);
    int32_t expected31 = (int32_t)((uint32_t)expected32 << 1);
    int32_t expected15 = (int32_t)((uint64_t)product >> 15);
    int32_t actual32 = MULT32(x, y);
    int32_t actual31 = MULT31(x, y);
    int32_t actual15 = MULT31_SHIFT15(x, y);

    if (actual32 != expected32 || actual31 != expected31 ||
        actual15 != expected15) {
        fprintf(stderr,
                "FAIL x=%d y=%d MULT32=%d/%d MULT31=%d/%d SHIFT15=%d/%d\n",
                x, y, actual32, expected32, actual31, expected31,
                actual15, expected15);
        return 0;
    }
    return 1;
}

int main(void)
{
    static const int32_t edges[] = {
        0, 1, -1, 2, -2, 0x7fff, -0x8000,
        0x10000, -0x10000, INT_MAX, INT_MIN
    };
    uint32_t i, j;

    for (i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i)
        for (j = 0; j < sizeof(edges) / sizeof(edges[0]); ++j)
            if (!check_pair(edges[i], edges[j]))
                return 1;

    for (i = 0; i < 1000000U; ++i)
        if (!check_pair((int32_t)next_random(), (int32_t)next_random()))
            return 1;

    puts("Ogg 68060 wide multiply: 1000000 random pairs plus edges passed");
    return 0;
}
