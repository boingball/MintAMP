/***************************************************************************
 * MintAMP CPU-specific LPC restoration for libfoxenflac.
 *
 * 68020/030/040 use the hardware 32x32->64 register-pair MULS.L form.
 * 68060 reconstructs the identical product with four hardware two-operand
 * partial multiplies, avoiding the emulated register-pair instruction.
 ***************************************************************************/

#ifndef MINTAMP_FLAC_M68K_LPC_H
#define MINTAMP_FLAC_M68K_LPC_H

#include <stdint.h>

#if defined(AMIGA_M68K_ASM) && defined(__GNUC__) && ( \
        defined(__mc68020__) || defined(__mc68030__) || \
        defined(__mc68040__) || defined(__mc68060__) || \
        defined(mc68020) || defined(mc68030) || \
        defined(mc68040) || defined(mc68060) || \
        defined(MINTAMP_FLAC_TEST_030) || \
        defined(MINTAMP_FLAC_TEST_060))

#define MINTAMP_FLAC_LPC_ASM 1
#define MINTAMP_FLAC_INLINE static __inline __attribute__((always_inline))

#if defined(__mc68060__) || defined(mc68060) || \
        defined(MINTAMP_FLAC_TEST_060)

MINTAMP_FLAC_INLINE uint32_t MintAmpFlacMulu32Lo(uint32_t x, uint32_t y)
{
#if defined(MINTAMP_FLAC_TEST_060)
    return x * y;
#else
    __asm__ ("mulu.l %1,%0"
             : "+d" (x)
             : "d"  (y)
             : "cc");
    return x;
#endif
}

MINTAMP_FLAC_INLINE int32_t MintAmpFlacMuls32Lo(int32_t x, int32_t y)
{
#if defined(MINTAMP_FLAC_TEST_060)
    return x * y;
#else
    __asm__ ("muls.l %1,%0"
             : "+d" (x)
             : "d"  (y)
             : "cc");
    return x;
#endif
}

MINTAMP_FLAC_INLINE void MintAmpFlacMultiply(int32_t x, int32_t y,
                                              int32_t *hi, uint32_t *lo)
{
    uint32_t x0 = (uint32_t)x & 0xffffU;
    int32_t x1 = x >> 16;
    uint32_t y0 = (uint32_t)y & 0xffffU;
    int32_t y1 = y >> 16;
    uint32_t w0 = MintAmpFlacMulu32Lo(x0, y0);
    int32_t t = MintAmpFlacMuls32Lo(x1, (int32_t)y0) +
                (int32_t)(w0 >> 16);
    int32_t w1 = t & 0xffff;
    int32_t w2 = t >> 16;

    w1 = MintAmpFlacMuls32Lo((int32_t)x0, y1) + w1;
    *hi = MintAmpFlacMuls32Lo(x1, y1) + w2 + (w1 >> 16);
    *lo = ((uint32_t)w1 << 16) | (w0 & 0xffffU);
}

MINTAMP_FLAC_INLINE void MintAmpFlacMadd(int32_t *acc_hi,
                                          uint32_t *acc_lo,
                                          int32_t x, int32_t y)
{
    int32_t product_hi;
    uint32_t product_lo;
    int32_t hi = *acc_hi;
    uint32_t lo = *acc_lo;

    MintAmpFlacMultiply(x, y, &product_hi, &product_lo);
#if defined(MINTAMP_FLAC_TEST_060)
    {
        uint32_t previous = lo;
        lo += product_lo;
        hi = (int32_t)((uint32_t)hi + (uint32_t)product_hi + (lo < previous));
    }
#else
    __asm__ ("add.l %3,%1\n\t"
             "addx.l %2,%0"
             : "+d" (hi), "+d" (lo)
             : "d" (product_hi), "d" (product_lo)
             : "cc");
#endif
    *acc_hi = hi;
    *acc_lo = lo;
}

#else /* 68020/030/040 */

MINTAMP_FLAC_INLINE void MintAmpFlacMadd(int32_t *acc_hi,
                                          uint32_t *acc_lo,
                                          int32_t x, int32_t y)
{
#if defined(MINTAMP_FLAC_TEST_030)
    uint64_t acc = ((uint64_t)(uint32_t)*acc_hi << 32) | *acc_lo;
    int64_t product = (int64_t)x * (int64_t)y;
    acc += (uint64_t)product;
    *acc_hi = (int32_t)(acc >> 32);
    *acc_lo = (uint32_t)acc;
#else
    int32_t hi = *acc_hi;
    uint32_t lo = *acc_lo;
    int32_t product_hi;
    uint32_t product_lo = (uint32_t)x;

    __asm__ ("muls.l %4,%2:%3\n\t"
             "add.l %3,%1\n\t"
             "addx.l %2,%0"
             : "+d" (hi), "+d" (lo), "=d" (product_hi),
               "+d" (product_lo)
             : "dmi" (y)
             : "cc");
    *acc_hi = hi;
    *acc_lo = lo;
#endif
}

#endif

MINTAMP_FLAC_INLINE void MintAmpFlacRestoreLpcSignal(
    int32_t *blk, uint32_t blk_size, const int32_t *lpc_coeffs,
    uint8_t lpc_order, int8_t lpc_shift)
{
    uint32_t i;

    for (i = lpc_order; i < blk_size; ++i) {
        int32_t acc_hi = 0;
        uint32_t acc_lo = 0;
        uint8_t j;
        uint32_t prediction;

        for (j = 0; j < lpc_order; ++j)
            MintAmpFlacMadd(&acc_hi, &acc_lo, lpc_coeffs[j],
                            blk[i - (uint32_t)j - 1U]);

        if (lpc_shift == 0)
            prediction = acc_lo;
        else
            prediction = (acc_lo >> (uint8_t)lpc_shift) |
                         ((uint32_t)acc_hi << (32U - (uint8_t)lpc_shift));
        blk[i] = (int32_t)((uint32_t)blk[i] + prediction);
    }
}

#endif /* supported Amiga m68k target */
#endif /* MINTAMP_FLAC_M68K_LPC_H */
