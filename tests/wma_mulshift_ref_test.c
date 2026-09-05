#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#define AMIGA_M68K_WMA_ASM 1
#include "../decoders/wma/asm_m68k.h"

#if !defined(MINTAMP_WMA_TEST_030) && !defined(MINTAMP_WMA_TEST_060)
#error Select one WMA m68k reference-test backend
#endif

static uint32_t rng_state = 0x574d4131U;

static uint32_t next_random(void)
{
    rng_state = rng_state * 1664525U + 1013904223U;
    return rng_state;
}

static int32_t reference_shift(int32_t x, int32_t y, unsigned int shift)
{
    int64_t product = (int64_t)x * (int64_t)y;
    return (int32_t)((uint64_t)product >> shift);
}

static int check_pair(int32_t x, int32_t y)
{
    int64_t product = (int64_t)x * (int64_t)y;
    WmaM68kProduct actual = WmaM68kMultiply(x, y);
    int32_t expected_hi = (int32_t)(product >> 32);

    if (actual.hi != expected_hi || actual.lo != (uint32_t)product ||
        MULT32(x, y) != expected_hi ||
        MULT31(x, y) != reference_shift(x, y, 31) ||
        MULT31_SHIFT15(x, y) != reference_shift(x, y, 15) ||
        MULT31_SHIFT16(x, y) != reference_shift(x, y, 16) ||
        WmaM68kMulShift16(x, y) != reference_shift(x, y, 16)) {
        fprintf(stderr, "FAIL x=%d y=%d hi=%d/%d lo=%08lx/%08lx\n",
                x, y, actual.hi, expected_hi,
                (unsigned long)actual.lo, (unsigned long)(uint32_t)product);
        return 0;
    }
    return 1;
}

static int check_complex(int32_t a, int32_t b, int32_t t, int32_t v)
{
    int32_t real, imag;
    int32_t r1 = reference_shift(t, a, 31);
    int32_t r2 = reference_shift(v, b, 31);
    int32_t r3 = reference_shift(t, b, 31);
    int32_t r4 = reference_shift(v, a, 31);
    int32_t expected_real = (int32_t)((uint32_t)r1 - (uint32_t)r2);
    int32_t expected_imag = (int32_t)((uint32_t)r3 + (uint32_t)r4);

    WmaM68kComplexMultiply(&real, &imag, a, b, t, v);
    if (real != expected_real || imag != expected_imag) {
        fprintf(stderr, "CMUL FAIL real=%d/%d imag=%d/%d\n",
                real, expected_real, imag, expected_imag);
        return 0;
    }
    return 1;
}

static int check_vectors(void)
{
    int32_t dst[17], expected[17], reverse[17];
    int32_t data[17], window[17];
    unsigned int i;

    for (i = 0; i < 17; ++i) {
        dst[i] = expected[i] = (int32_t)next_random();
        data[i] = (int32_t)next_random();
        window[i] = (int32_t)next_random();
    }
    for (i = 0; i < 17; ++i) {
        expected[i] = (int32_t)((uint32_t)expected[i] +
                      (uint32_t)reference_shift(data[i], window[i], 31));
        reverse[i] = reference_shift(data[i], window[16 - i], 31);
    }

    WmaM68kVectorFmulAdd(dst, data, window, 17);
    for (i = 0; i < 17; ++i)
        if (dst[i] != expected[i])
            return 0;

    WmaM68kVectorFmulReverse(dst, data, window, 17);
    for (i = 0; i < 17; ++i)
        if (dst[i] != reverse[i])
            return 0;
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

    for (i = 0; i < 1000000U; ++i) {
        int32_t a = (int32_t)next_random();
        int32_t b = (int32_t)next_random();
        int32_t t = (int32_t)next_random();
        int32_t v = (int32_t)next_random();
        if (!check_pair(a, b) || !check_complex(a, b, t, v))
            return 1;
    }

    if (!check_vectors()) {
        fputs("vector helper mismatch\n", stderr);
        return 1;
    }

#if defined(MINTAMP_WMA_TEST_060)
    puts("WMA 68060 partial-product math: 1000000 pairs plus edges passed");
#else
    puts("WMA 68030 full-result math: 1000000 pairs plus edges passed");
#endif
    return 0;
}
