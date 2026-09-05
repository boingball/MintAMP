/***************************************************************************
 * MintAMP CPU-specific wide math for the Rockbox WMA decoder.
 *
 * The 68020/030/040 implementation uses the hardware 32x32->64 register-pair
 * MULS.L instruction.  The 68060 implementation deliberately avoids that
 * emulated form and reconstructs the exact product from four hardware
 * two-operand 16x16 partial products.
 ***************************************************************************/

#ifndef MINTAMP_WMA_ASM_M68K_H
#define MINTAMP_WMA_ASM_M68K_H

#include <stdint.h>

#if defined(AMIGA_M68K_WMA_ASM) && defined(__GNUC__) && ( \
        defined(__mc68020__) || defined(__mc68030__) || \
        defined(__mc68040__) || defined(__mc68060__) || \
        defined(mc68020) || defined(mc68030) || \
        defined(mc68040) || defined(mc68060) || \
        defined(MINTAMP_WMA_TEST_030) || \
        defined(MINTAMP_WMA_TEST_060))

#define MINTAMP_WMA_M68K_ACTIVE 1
#define MINTAMP_WMA_INLINE static __inline __attribute__((always_inline))

typedef struct WmaM68kProduct {
    uint32_t lo;
    int32_t hi;
} WmaM68kProduct;

#if defined(__mc68060__) || defined(mc68060) || \
        defined(MINTAMP_WMA_TEST_060)

#define MINTAMP_WMA_M68K_060 1

MINTAMP_WMA_INLINE uint32_t WmaM68kMulu32Lo(uint32_t x, uint32_t y)
{
#if defined(MINTAMP_WMA_TEST_060)
    return x * y;
#else
    __asm__ ("mulu.l %1,%0"
             : "+d" (x)
             : "d"  (y)
             : "cc");
    return x;
#endif
}

MINTAMP_WMA_INLINE int32_t WmaM68kMuls32Lo(int32_t x, int32_t y)
{
#if defined(MINTAMP_WMA_TEST_060)
    return x * y;
#else
    __asm__ ("muls.l %1,%0"
             : "+d" (x)
             : "d"  (y)
             : "cc");
    return x;
#endif
}

/* Hacker's Delight 8-2 signed mulhs construction.  Keeping both result
 * halves costs the same four hardware partial multiplies as keeping hi alone.
 */
MINTAMP_WMA_INLINE WmaM68kProduct WmaM68kMultiply(int32_t x, int32_t y)
{
    uint32_t x0 = (uint32_t)x & 0xffffU;
    int32_t x1 = x >> 16;
    uint32_t y0 = (uint32_t)y & 0xffffU;
    int32_t y1 = y >> 16;
    uint32_t w0 = WmaM68kMulu32Lo(x0, y0);
    int32_t t = WmaM68kMuls32Lo(x1, (int32_t)y0) +
                (int32_t)(w0 >> 16);
    int32_t w1 = t & 0xffff;
    int32_t w2 = t >> 16;
    WmaM68kProduct product;

    w1 = WmaM68kMuls32Lo((int32_t)x0, y1) + w1;
    product.hi = WmaM68kMuls32Lo(x1, y1) + w2 + (w1 >> 16);
    product.lo = ((uint32_t)w1 << 16) | (w0 & 0xffffU);
    return product;
}

#else /* 68020/030/040 */

#define MINTAMP_WMA_M68K_030 1

MINTAMP_WMA_INLINE WmaM68kProduct WmaM68kMultiply(int32_t x, int32_t y)
{
    WmaM68kProduct product;

#if defined(MINTAMP_WMA_TEST_030)
    int64_t reference = (int64_t)x * (int64_t)y;
    product.hi = (int32_t)(reference >> 32);
    product.lo = (uint32_t)reference;
#else
    int32_t hi;
    uint32_t lo = (uint32_t)x;
    __asm__ ("muls.l %2,%0:%1"
             : "=d" (hi), "+d" (lo)
             : "dmi" (y)
             : "cc");
    product.hi = hi;
    product.lo = lo;
#endif
    return product;
}

#endif

MINTAMP_WMA_INLINE int32_t WmaM68kMulHigh32(int32_t x, int32_t y)
{
    return WmaM68kMultiply(x, y).hi;
}

MINTAMP_WMA_INLINE int32_t WmaM68kMulShift31(int32_t x, int32_t y)
{
    WmaM68kProduct product = WmaM68kMultiply(x, y);
    return (int32_t)((product.lo >> 31) | ((uint32_t)product.hi << 1));
}

MINTAMP_WMA_INLINE int32_t WmaM68kMulShift16(int32_t x, int32_t y)
{
    WmaM68kProduct product = WmaM68kMultiply(x, y);
    return (int32_t)((product.lo >> 16) | ((uint32_t)product.hi << 16));
}

MINTAMP_WMA_INLINE int32_t WmaM68kMulShift15(int32_t x, int32_t y)
{
    WmaM68kProduct product = WmaM68kMultiply(x, y);
    return (int32_t)((product.lo >> 15) | ((uint32_t)product.hi << 17));
}

/* Pre-empt codeclib_misc.h's int64_t fallbacks. */
#define INCL_OPTIMIZED_MULT32
MINTAMP_WMA_INLINE int32_t MULT32(int32_t x, int32_t y)
{
    return WmaM68kMulHigh32(x, y);
}

#define INCL_OPTIMIZED_MULT31
MINTAMP_WMA_INLINE int32_t MULT31(int32_t x, int32_t y)
{
    return WmaM68kMulShift31(x, y);
}

#define INCL_OPTIMIZED_MULT31_SHIFT15
MINTAMP_WMA_INLINE int32_t MULT31_SHIFT15(int32_t x, int32_t y)
{
    return WmaM68kMulShift15(x, y);
}

#define INCL_OPTIMIZED_MULT31_SHIFT16
MINTAMP_WMA_INLINE int32_t MULT31_SHIFT16(int32_t x, int32_t y)
{
    return WmaM68kMulShift16(x, y);
}

MINTAMP_WMA_INLINE void WmaM68kComplexMultiply(int32_t *real, int32_t *imag,
                                             int32_t are, int32_t aim,
                                             int32_t bre, int32_t bim)
{
    int32_t r1 = WmaM68kMulShift31(bre, are);
    int32_t r2 = WmaM68kMulShift31(bim, aim);
    int32_t r3 = WmaM68kMulShift31(bre, aim);
    int32_t r4 = WmaM68kMulShift31(bim, are);

    *real = (int32_t)((uint32_t)r1 - (uint32_t)r2);
    *imag = (int32_t)((uint32_t)r3 + (uint32_t)r4);
}

/* Two independent products per iteration reduce loop overhead on 030 and give
 * the 060 scheduler useful work from the other chain while one result settles.
 */
MINTAMP_WMA_INLINE void WmaM68kVectorFmulAdd(int32_t *dst,
                                          const int32_t *data,
                                          const int32_t *window, int n)
{
    while (n >= 2) {
        int32_t p0 = WmaM68kMulShift31(data[0], window[0]);
        int32_t p1 = WmaM68kMulShift31(data[1], window[1]);
        dst[0] = (int32_t)((uint32_t)dst[0] + (uint32_t)p0);
        dst[1] = (int32_t)((uint32_t)dst[1] + (uint32_t)p1);
        dst += 2;
        data += 2;
        window += 2;
        n -= 2;
    }
    if (n)
        dst[0] = (int32_t)((uint32_t)dst[0] +
                           (uint32_t)WmaM68kMulShift31(data[0], window[0]));
}

MINTAMP_WMA_INLINE void WmaM68kVectorFmulReverse(int32_t *dst,
                                              const int32_t *data,
                                              const int32_t *window, int n)
{
    window += n - 1;
    while (n >= 2) {
        int32_t p0 = WmaM68kMulShift31(data[0], window[0]);
        int32_t p1 = WmaM68kMulShift31(data[1], window[-1]);
        dst[0] = p0;
        dst[1] = p1;
        dst += 2;
        data += 2;
        window -= 2;
        n -= 2;
    }
    if (n)
        dst[0] = WmaM68kMulShift31(data[0], window[0]);
}

#endif /* supported Amiga m68k target */
#endif /* MINTAMP_WMA_ASM_M68K_H */
