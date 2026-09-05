#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define AMIGA_M68K_ASM 1
#include "../decoders/flac_m68k_lpc.h"

#if !defined(MINTAMP_FLAC_TEST_030) && !defined(MINTAMP_FLAC_TEST_060)
#error Select one FLAC m68k reference-test backend
#endif

static uint32_t rng_state = 0x464c4143U;

static uint32_t next_random(void)
{
    rng_state = rng_state * 1664525U + 1013904223U;
    return rng_state;
}

static int check_madd(int32_t acc_hi, uint32_t acc_lo,
                      int32_t x, int32_t y)
{
    uint64_t expected = ((uint64_t)(uint32_t)acc_hi << 32) | acc_lo;
    expected += (uint64_t)((int64_t)x * (int64_t)y);
    MintAmpFlacMadd(&acc_hi, &acc_lo, x, y);
    if (acc_hi != (int32_t)(expected >> 32) ||
        acc_lo != (uint32_t)expected) {
        fprintf(stderr, "MADD FAIL x=%d y=%d got=%08lx:%08lx expected=%08lx:%08lx\n",
                x, y, (unsigned long)(uint32_t)acc_hi,
                (unsigned long)acc_lo,
                (unsigned long)(uint32_t)(expected >> 32),
                (unsigned long)(uint32_t)expected);
        return 0;
    }
    return 1;
}

static void restore_reference(int32_t *blk, uint32_t blk_size,
                              const int32_t *coeff, uint8_t order,
                              uint8_t shift)
{
    uint32_t i;
    for (i = order; i < blk_size; ++i) {
        uint64_t acc = 0;
        uint8_t j;
        uint32_t prediction;
        for (j = 0; j < order; ++j)
            acc += (uint64_t)((int64_t)coeff[j] *
                              (int64_t)blk[i - (uint32_t)j - 1U]);
        prediction = shift ? (uint32_t)(acc >> shift) : (uint32_t)acc;
        blk[i] = (int32_t)((uint32_t)blk[i] + prediction);
    }
}

static int check_lpc_case(uint8_t order, uint8_t shift)
{
    int32_t actual[96], expected[96], coeff[32];
    uint32_t i;

    for (i = 0; i < 32; ++i)
        coeff[i] = (int32_t)(next_random() & 31U) - 16;
    for (i = 0; i < 96; ++i)
        actual[i] = expected[i] = (int32_t)(next_random() & 0x00ffffffU) -
                                  0x00800000;

    restore_reference(expected, 96, coeff, order, shift);
    MintAmpFlacRestoreLpcSignal(actual, 96, coeff, order, (int8_t)shift);
    for (i = 0; i < 96; ++i) {
        if (actual[i] != expected[i]) {
            fprintf(stderr,
                    "LPC FAIL order=%u shift=%u sample=%lu got=%d expected=%d\n",
                    (unsigned)order, (unsigned)shift, (unsigned long)i,
                    actual[i], expected[i]);
            return 0;
        }
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
            if (!check_madd(edges[(i + 3U) % 11U], (uint32_t)edges[i],
                            edges[i], edges[j]))
                return 1;

    for (i = 0; i < 1000000U; ++i)
        if (!check_madd((int32_t)next_random(), next_random(),
                        (int32_t)next_random(), (int32_t)next_random()))
            return 1;

    for (i = 0; i < 512U; ++i)
        if (!check_lpc_case((uint8_t)(i % 33U), (uint8_t)(i % 16U)))
            return 1;

#if defined(MINTAMP_FLAC_TEST_060)
    puts("FLAC 68060 LPC partial-product math passed");
#else
    puts("FLAC 68030 LPC full-result math passed");
#endif
    return 0;
}
