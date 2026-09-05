/********************************************************************
 * MintAMP 68060 wide-math helpers for the Tremor Vorbis decoder.
 *
 * Tremor's 68020/030/040 helper uses the full-result register-pair
 * MULS.L form. The 68060 does not implement that form in hardware, so
 * this forced-include header supplies the same results using Warren's
 * signed high-multiply construction and hardware two-operand multiplies.
 ********************************************************************/

#ifndef MINTAMP_OGG_M68K_68060_H
#define MINTAMP_OGG_M68K_68060_H

#if defined(_M68K_ASSEM_) && (defined(__mc68060__) || defined(mc68060) || \
                              defined(MINTAMP_OGG_TEST_68060))

#include <stdint.h>

static __inline uint32_t MINTAMP_OGG_MULU32_LO(uint32_t x, uint32_t y)
{
#if defined(MINTAMP_OGG_TEST_68060)
    return x * y;
#else
    __asm__ volatile ("mulu.l %1,%0"
                      : "+d" (x)
                      : "d"  (y)
                      : "cc");
    return x;
#endif
}

static __inline int32_t MINTAMP_OGG_MULS32_LO(int32_t x, int32_t y)
{
#if defined(MINTAMP_OGG_TEST_68060)
    return x * y;
#else
    __asm__ volatile ("muls.l %1,%0"
                      : "+d" (x)
                      : "d"  (y)
                      : "cc");
    return x;
#endif
}

/* Exact signed high 32 bits of x*y, using four hardware low-result products. */
static __inline int32_t MINTAMP_OGG_MULSHIFT32(int32_t x, int32_t y)
{
    uint32_t x0 = (uint32_t)x & 0xffffU;
    int32_t x1 = x >> 16;
    uint32_t y0 = (uint32_t)y & 0xffffU;
    int32_t y1 = y >> 16;
    uint32_t w0 = MINTAMP_OGG_MULU32_LO(x0, y0);
    int32_t t = MINTAMP_OGG_MULS32_LO(x1, (int32_t)y0) +
                (int32_t)(w0 >> 16);
    int32_t w1 = t & 0xffff;
    int32_t w2 = t >> 16;

    w1 = MINTAMP_OGG_MULS32_LO((int32_t)x0, y1) + w1;
    return MINTAMP_OGG_MULS32_LO(x1, y1) + w2 + (w1 >> 16);
}

static __inline int32_t MULT32(int32_t x, int32_t y)
{
    return MINTAMP_OGG_MULSHIFT32(x, y);
}

static __inline int32_t MULT31(int32_t x, int32_t y)
{
    return (int32_t)((uint32_t)MINTAMP_OGG_MULSHIFT32(x, y) << 1);
}

static __inline int32_t MULT31_SHIFT15(int32_t x, int32_t y)
{
    uint32_t lo = MINTAMP_OGG_MULU32_LO((uint32_t)x, (uint32_t)y);
    uint32_t hi = (uint32_t)MINTAMP_OGG_MULSHIFT32(x, y);

    return (int32_t)((lo >> 15) | (hi << 17));
}

#define MB() __asm__ volatile ("" : : : "memory")

static __inline void XPROD32(int32_t a, int32_t b, int32_t t, int32_t v,
                             int32_t *x, int32_t *y)
{
    *x = MULT32(a, t) + MULT32(b, v);
    *y = MULT32(b, t) - MULT32(a, v);
}

static __inline void XPROD31(int32_t a, int32_t b, int32_t t, int32_t v,
                             int32_t *x, int32_t *y)
{
    *x = MULT31(a, t) + MULT31(b, v);
    *y = MULT31(b, t) - MULT31(a, v);
}

static __inline void XNPROD31(int32_t a, int32_t b, int32_t t, int32_t v,
                              int32_t *x, int32_t *y)
{
    *x = MULT31(a, t) - MULT31(b, v);
    *y = MULT31(b, t) + MULT31(a, v);
}

/* Prevent misc.h and the patched 020/030/040 header defining wide math again. */
#define _V_WIDE_MATH

#endif /* 68060 OGG asm or host test */
#endif /* MINTAMP_OGG_M68K_68060_H */
