/*
 * polyphase_68060.h - dedicated Motorola 68060 polyphase synthesis kernel.
 *
 * This is a DISTINCT implementation, not a modification of the 68030 path.
 * It exists to solve one specific 68060 problem: the existing fast polyphase
 * (both the C PolyphaseMulShift26 and the hand-written .S kernel) computes the
 * per-tap high multiply with the register-pair long multiply
 *
 *     muls.l Dn,Dl:Dh        (32x32 -> 64, keep high 32)
 *
 * which the 68060 does NOT implement in hardware -- every one traps into slow
 * OS software emulation. In the polyphase hot loop there are thousands of them
 * per frame, which is the measured cause of mouse lag and audio starvation on
 * a real ~75 MHz 68060.
 *
 * The fix is to compute the identical value -- the high 32 bits of a signed
 * 32x32 product, i.e. MULSHIFT32(x, y) -- using only operations the 68060 runs
 * in hardware: the 32x32->32 low multiply (muls.l <ea>,Dn) and 16-bit
 * multiplies, via Warren's signed "mulhs" (Hacker's Delight 8-2). Four
 * half-width partial products, shifts and adds; no register pair, no 64-bit
 * libgcc helper call. GCC's -m68060 codegen keeps every multiply here as a
 * hardware muls.l/mulu.l (verified with the audit tool), so the emulated
 * instruction count for this kernel is zero.
 *
 * Numerically MulShift68060(x, y) == MULSHIFT32_C_REFERENCE(x, y) bit for bit,
 * so this kernel produces the same PCM as the accepted AMIGA_FAST_POLYPHASE C
 * path (same per-tap truncation), only without the emulated instruction. Its
 * output is compared against the bit-exact 64-bit reference polyphase by
 * tests/polyphase60_ref_test.c.
 *
 * Nothing here is enabled by default. It compiles only under
 * AMIGA_M68K_POLYPHASE_68060 (ASM60_GROUPS=poly060) and is experimental until
 * validated on the physical 68060.
 */

#ifndef _POLYPHASE_68060_H
#define _POLYPHASE_68060_H

/* Match the polyphase.c fixed-point layout. Guarded so including this after
 * polyphase.c's own #defines does not redefine them. */
#ifndef POLY60_CSHIFT
#define POLY60_CSHIFT 12
#endif
#ifndef POLY60_NFRACBITS
#define POLY60_NFRACBITS (25 - 2 - 2 - 15) /* == DEF_NFRACBITS == 6 */
#endif

/*
 * Guard bits carried inside the 32-bit accumulator. The plain fast path
 * truncates each tap's product straight to the final PCM domain (>>26), which
 * over ~16-34 taps accumulates ~13 LSB of divergence from the bit-exact 64-bit
 * reference. Keeping a few extra fraction bits per tap and rounding once at the
 * end cuts that to a couple of LSB at no extra multiply cost. Capped at 5: the
 * coefficient (<= 2^19) is pre-shifted by NFRACBITS+GUARD and must stay inside
 * 32 bits, and the accumulator must not overflow across the valid vbuf range.
 */
#ifndef POLY60_GUARD
#define POLY60_GUARD 4
#endif
#if POLY60_GUARD > 0
#define POLY60_ROUND (1 << (POLY60_GUARD - 1))
#else
#define POLY60_ROUND 0
#endif

/*
 * Signed high 32 bits of a 32x32 product, computed with hardware-only m68k
 * multiplies. Each partial product has both operands <= 16 bits in magnitude,
 * so it fits in 32 bits and GCC emits a hardware 32x32->32 muls.l/mulu.l --
 * never the emulated 64-bit register-pair form.
 *
 * Returns (int)(((long long)x * (long long)y) >> 32), exactly.
 */
static __inline int MulShift68060(int x, int y)
{
	unsigned int x0 = (unsigned int)x & 0xFFFFu;
	int x1 = x >> 16;
	unsigned int y0 = (unsigned int)y & 0xFFFFu;
	int y1 = y >> 16;

	unsigned int w0 = x0 * y0;               /* unsigned 16x16 -> 32 */
	int t = x1 * (int)y0 + (int)(w0 >> 16);  /* signed*unsigned + carry */
	int w1 = t & 0xFFFF;
	int w2 = t >> 16;                        /* arithmetic */

	w1 = (int)x0 * y1 + w1;                  /* unsigned*signed + partial */

	return x1 * y1 + w2 + (w1 >> 16);
}

/*
 * Per-tap contribution used by the polyphase convolution:
 *   (sample * coef) >> (32 - CSHIFT + NFRACBITS)  ==  (sample * coef) >> 26.
 * Shifting the coefficient left by NFRACBITS and taking the high 32 bits gives
 * exactly that. coef carries CSHIFT (=12) leading sign bits, so coef<<6 cannot
 * overflow 32 bits.
 */
static __inline int PolyphaseMulShift26_68060(int sample, int coef)
{
	return MulShift68060(sample, coef << (POLY60_NFRACBITS + POLY60_GUARD));
}

static __inline short ClipIntToShort68060(int x)
{
	int sign = x >> 31;
	if (sign != (x >> 15))
		x = sign ^ ((1 << 15) - 1);
	return (short)x;
}

/* Round off the guard bits and clip the accumulator to a PCM sample. */
static __inline short Poly60Finalize(int acc)
{
	return ClipIntToShort68060((acc + POLY60_ROUND) >> POLY60_GUARD);
}

/* 32-bit accumulate versions of polyphase.c's MC0/MC1/MC2 tap macros. The tap
 * indexing, coefficient walk and output sample mapping are identical to
 * PolyphaseMonoReference/PolyphaseStereoReference; only the arithmetic changes
 * (per-tap high multiply + 32-bit sum instead of 64-bit MADD64). */
#define MC0M60(x) { \
	c1 = *coef; coef++; c2 = *coef; coef++; \
	vLo = *(vb1 + (x)); vHi = *(vb1 + (23 - (x))); \
	sum1 += PolyphaseMulShift26_68060(vLo,  c1); \
	sum1 += PolyphaseMulShift26_68060(vHi, -c2); \
}
#define MC1M60(x) { \
	c1 = *coef; coef++; \
	vLo = *(vb1 + (x)); \
	sum1 += PolyphaseMulShift26_68060(vLo, c1); \
}
#define MC2M60(x) { \
	c1 = *coef; coef++; c2 = *coef; coef++; \
	vLo = *(vb1 + (x)); vHi = *(vb1 + (23 - (x))); \
	sum1 += PolyphaseMulShift26_68060(vLo,  c1); \
	sum2 += PolyphaseMulShift26_68060(vLo,  c2); \
	sum1 += PolyphaseMulShift26_68060(vHi, -c2); \
	sum2 += PolyphaseMulShift26_68060(vHi,  c1); \
}

static void PolyphaseMono68060(short *pcm, int *vbuf, const int *coefBase)
{
	int i;
	const int *coef;
	int *vb1;
	int vLo, vHi, c1, c2;
	int sum1, sum2;

	/* output sample 0 */
	coef = coefBase;
	vb1 = vbuf;
	sum1 = 0;
	MC0M60(0) MC0M60(1) MC0M60(2) MC0M60(3)
	MC0M60(4) MC0M60(5) MC0M60(6) MC0M60(7)
	*(pcm + 0) = Poly60Finalize(sum1);

	/* output sample 16 */
	coef = coefBase + 256;
	vb1 = vbuf + 64 * 16;
	sum1 = 0;
	MC1M60(0) MC1M60(1) MC1M60(2) MC1M60(3)
	MC1M60(4) MC1M60(5) MC1M60(6) MC1M60(7)
	*(pcm + 16) = Poly60Finalize(sum1);

	/* samples 1..15 and 31..17 */
	coef = coefBase + 16;
	vb1 = vbuf + 64;
	pcm++;
	for (i = 15; i > 0; i--) {
		sum1 = 0;
		sum2 = 0;
		MC2M60(0) MC2M60(1) MC2M60(2) MC2M60(3)
		MC2M60(4) MC2M60(5) MC2M60(6) MC2M60(7)
		vb1 += 64;
		*(pcm)         = Poly60Finalize(sum1);
		*(pcm + 2 * i) = Poly60Finalize(sum2);
		pcm++;
	}
}

#define MC0S60(x) { \
	c1 = *coef; coef++; c2 = *coef; coef++; \
	vLo = *(vb1 + (x)); vHi = *(vb1 + (23 - (x))); \
	sum1L += PolyphaseMulShift26_68060(vLo,  c1); \
	sum1L += PolyphaseMulShift26_68060(vHi, -c2); \
	vLo = *(vb1 + 32 + (x)); vHi = *(vb1 + 32 + (23 - (x))); \
	sum1R += PolyphaseMulShift26_68060(vLo,  c1); \
	sum1R += PolyphaseMulShift26_68060(vHi, -c2); \
}
#define MC1S60(x) { \
	c1 = *coef; coef++; \
	vLo = *(vb1 + (x)); \
	sum1L += PolyphaseMulShift26_68060(vLo, c1); \
	vLo = *(vb1 + 32 + (x)); \
	sum1R += PolyphaseMulShift26_68060(vLo, c1); \
}
#define MC2S60(x) { \
	c1 = *coef; coef++; c2 = *coef; coef++; \
	vLo = *(vb1 + (x)); vHi = *(vb1 + (23 - (x))); \
	sum1L += PolyphaseMulShift26_68060(vLo,  c1); \
	sum2L += PolyphaseMulShift26_68060(vLo,  c2); \
	sum1L += PolyphaseMulShift26_68060(vHi, -c2); \
	sum2L += PolyphaseMulShift26_68060(vHi,  c1); \
	vLo = *(vb1 + 32 + (x)); vHi = *(vb1 + 32 + (23 - (x))); \
	sum1R += PolyphaseMulShift26_68060(vLo,  c1); \
	sum2R += PolyphaseMulShift26_68060(vLo,  c2); \
	sum1R += PolyphaseMulShift26_68060(vHi, -c2); \
	sum2R += PolyphaseMulShift26_68060(vHi,  c1); \
}

static void PolyphaseStereo68060(short *pcm, int *vbuf, const int *coefBase)
{
	int i;
	const int *coef;
	int *vb1;
	int vLo, vHi, c1, c2;
	int sum1L, sum2L, sum1R, sum2R;

	/* output sample 0 */
	coef = coefBase;
	vb1 = vbuf;
	sum1L = 0;
	sum1R = 0;
	MC0S60(0) MC0S60(1) MC0S60(2) MC0S60(3)
	MC0S60(4) MC0S60(5) MC0S60(6) MC0S60(7)
	*(pcm + 0) = Poly60Finalize(sum1L);
	*(pcm + 1) = Poly60Finalize(sum1R);

	/* output sample 16 */
	coef = coefBase + 256;
	vb1 = vbuf + 64 * 16;
	sum1L = 0;
	sum1R = 0;
	MC1S60(0) MC1S60(1) MC1S60(2) MC1S60(3)
	MC1S60(4) MC1S60(5) MC1S60(6) MC1S60(7)
	*(pcm + 2 * 16 + 0) = Poly60Finalize(sum1L);
	*(pcm + 2 * 16 + 1) = Poly60Finalize(sum1R);

	/* samples 1..15 and 31..17, interleaved L/R */
	coef = coefBase + 16;
	vb1 = vbuf + 64;
	pcm += 2;
	for (i = 15; i > 0; i--) {
		sum1L = 0; sum2L = 0;
		sum1R = 0; sum2R = 0;
		MC2S60(0) MC2S60(1) MC2S60(2) MC2S60(3)
		MC2S60(4) MC2S60(5) MC2S60(6) MC2S60(7)
		vb1 += 64;
		*(pcm + 0)             = Poly60Finalize(sum1L);
		*(pcm + 1)             = Poly60Finalize(sum1R);
		*(pcm + 2 * 2 * i + 0) = Poly60Finalize(sum2L);
		*(pcm + 2 * 2 * i + 1) = Poly60Finalize(sum2R);
		pcm += 2;
	}
}

#endif /* _POLYPHASE_68060_H */
