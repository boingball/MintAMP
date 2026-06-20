/* ***** BEGIN LICENSE BLOCK ***** 
 * Version: RCSL 1.0/RPSL 1.0 
 *  
 * Portions Copyright (c) 1995-2002 RealNetworks, Inc. All Rights Reserved. 
 *      
 * The contents of this file, and the files included with this file, are 
 * subject to the current version of the RealNetworks Public Source License 
 * Version 1.0 (the "RPSL") available at 
 * http://www.helixcommunity.org/content/rpsl unless you have licensed 
 * the file under the RealNetworks Community Source License Version 1.0 
 * (the "RCSL") available at http://www.helixcommunity.org/content/rcsl, 
 * in which case the RCSL will apply. You may also obtain the license terms 
 * directly from RealNetworks.  You may not use this file except in 
 * compliance with the RPSL or, if you have a valid RCSL with RealNetworks 
 * applicable to this file, the RCSL.  Please see the applicable RPSL or 
 * RCSL for the rights, obligations and limitations governing use of the 
 * contents of the file.  
 *  
 * This file is part of the Helix DNA Technology. RealNetworks is the 
 * developer of the Original Code and owns the copyrights in the portions 
 * it created. 
 *  
 * This file, and the files included with this file, is distributed and made 
 * available on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER 
 * EXPRESS OR IMPLIED, AND REALNETWORKS HEREBY DISCLAIMS ALL SUCH WARRANTIES, 
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS 
 * FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT. 
 * 
 * Technology Compatibility Kit Test Suite(s) Location: 
 *    http://www.helixcommunity.org/content/tck 
 * 
 * Contributor(s): 
 *  
 * ***** END LICENSE BLOCK ***** */ 

/**************************************************************************************
 * Fixed-point MP3 decoder
 * Jon Recker (jrecker@real.com), Ken Cooke (kenc@real.com)
 * June 2003
 *
 * imdct.c - antialias, inverse transform (short/long/mixed), windowing, 
 *             overlap-add, frequency inversion
 **************************************************************************************/

#include "coder.h"
#include "assembly.h"
#include <stdio.h>
#include <string.h>

/**************************************************************************************
 * Function:    IMDCT36
 *
 * Description: 36-point modified DCT, with windowing and overlap-add (50% overlap)
 *
 * Inputs:      vector of 18 coefficients (N/2 inputs produces N outputs, by symmetry)
 *              overlap part of last IMDCT (9 samples - see output comments)
 *              window type (0,1,2,3) of current and previous block
 *              current block index (for deciding whether to do frequency inversion)
 *              number of guard bits in input vector
 *
 * Outputs:     18 output samples, after windowing and overlap-add with last frame
 *              second half of (unwindowed) 36-point IMDCT - save for next time
 *                only save 9 xPrev samples, using symmetry (see WinPrevious())
 *
 * Notes:       this is Ken's hyper-fast algorithm, including symmetric sin window
 *                optimization, if applicable
 *              total number of multiplies, general case: 
 *                2*10 (idct9) + 9 (last stage imdct) + 36 (for windowing) = 65
 *              total number of multiplies, btCurr == 0 && btPrev == 0:
 *                2*10 (idct9) + 9 (last stage imdct) + 18 (for windowing) = 47
 *
 *              blockType == 0 is by far the most common case, so it should be
 *                possible to use the fast path most of the time
 *              this is the fastest known algorithm for performing 
 *                long IMDCT + windowing + overlap-add in MP3
 *
 * Return:      mOut (OR of abs(y) for all y calculated here)
 *
 * TODO:        optimize for ARM (reorder window coefs, ARM-style pointers in C, 
 *                inline asm may or may not be helpful)
 **************************************************************************************/
// barely faster in RAM
int IMDCT36_C_REFERENCE(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb)
{
	int i, es, xBuf[18], xPrevWin[18];
	int acc1, acc2, s, d, t, mOut;
	int xo, xe, c, *xp, *xpwLo, *xpwHi, *ypLo, *ypHi, yLo, yHi;
	const int *cp, *wp, *wpLo, *wpHi;

	acc1 = acc2 = 0;
	xCurr += 17;

	/* 7 gb is always adequate for antialias + accumulator loop + idct9 */
	if (gb < 7) {
		/* rarely triggered - 5% to 10% of the time on normal clips (with Q25 input) */
		es = 7 - gb;
		for (i = 8; i >= 0; i--) {	
			acc1 = ((*xCurr--) >> es) - acc1;
			acc2 = acc1 - acc2;
			acc1 = ((*xCurr--) >> es) - acc1;
			xBuf[i+9] = acc2;	/* odd */
			xBuf[i+0] = acc1;	/* even */
			xPrev[i] >>= es;
		}
	} else {
		es = 0;
		/* max gain = 18, assume adequate guard bits */
		for (i = 8; i >= 0; i--) {	
			acc1 = (*xCurr--) - acc1;
			acc2 = acc1 - acc2;
			acc1 = (*xCurr--) - acc1;
			xBuf[i+9] = acc2;	/* odd */
			xBuf[i+0] = acc1;	/* even */
		}
	}
	/* xEven[0] and xOdd[0] scaled by 0.5 */
	xBuf[9] >>= 1;
	xBuf[0] >>= 1;

	/* do 9-point IDCT on even and odd */
	idct9(xBuf+0);	/* even */
	idct9(xBuf+9);	/* odd */

	xp = xBuf + 8;
	cp = c18 + 8;
	mOut = 0;
	if (btPrev == 0 && btCurr == 0) {
		/* fast path - use symmetry of sin window to reduce windowing multiplies to 18 (N/2) */
		wp = fastWin36;
		ypLo = y;
		ypHi = y + 17*NBANDS;
		for (i = 9; i > 0; i--) {
			c = *cp--;	xo = *(xp + 9);		xe = *xp--;
			/* gain 2 int bits here */
			xo = MULSHIFT32(c, xo);			/* 2*c18*xOdd (mul by 2 implicit in scaling)  */
			xe >>= 2;

			s = -(*xPrev);		/* sum from last block (always at least 2 guard bits) */
			d = -(xe - xo);		/* gain 2 int bits, don't shift xo (effective << 1 to eat sign bit, << 1 for mul by 2) */
			(*xPrev++) = xe + xo;			/* symmetry - xPrev[i] = xPrev[17-i] for long blocks */
			t = s - d;

			yLo = (d + (MULSHIFT32(t, *wp++) << 2));
			yHi = (s + (MULSHIFT32(t, *wp++) << 2));
			*ypLo = yLo;		ypLo += NBANDS;
			*ypHi = yHi;		ypHi -= NBANDS;
			mOut |= FASTABS(yLo);
			mOut |= FASTABS(yHi);
		}
	} else {
		/* slower method - either prev or curr is using window type != 0 so do full 36-point window 
		 * output xPrevWin has at least 3 guard bits (xPrev has 2, gain 1 in WinPrevious)
		 */
		WinPrevious(xPrev, xPrevWin, btPrev);

		wpLo = imdctWin[btCurr];
		wpHi = wpLo + 17;
		xpwLo = xPrevWin;
		xpwHi = xPrevWin + 17;
		ypLo = y;
		ypHi = y + 17*NBANDS;
		for (i = 9; i > 0; i--) {
			c = *cp--;	xo = *(xp + 9);		xe = *xp--;
			/* gain 2 int bits here */
			xo = MULSHIFT32(c, xo);			/* 2*c18*xOdd (mul by 2 implicit in scaling)  */
			xe >>= 2;

			d = xe - xo;
			(*xPrev++) = xe + xo;	/* symmetry - xPrev[i] = xPrev[17-i] for long blocks */
			
			yLo = (*xpwLo++ + MULSHIFT32(d, *wpLo++)) << 2;
			yHi = (*xpwHi-- + MULSHIFT32(d, *wpHi--)) << 2;
			*ypLo = yLo;		ypLo += NBANDS;
			*ypHi = yHi;		ypHi -= NBANDS;
			mOut |= FASTABS(yLo);
			mOut |= FASTABS(yHi);
		}
	}

	xPrev -= 9;
	if (es)
		mOut |= FreqInvertRescale(y, xPrev, blockIdx, es);
	else if (blockIdx & 0x01)
		FreqInvertOdd(y);

	return mOut;
}

#if IMDCT36_HAS_AMIGA_M68K_ASM
static __inline void idct9_amiga_m68k_asm(int *x)
{
	int a1, a2, a3, a4, a5, a6, a7, a8, a9;
	int a10, a11, a12, a13, a14, a15, a16, a17, a18;
	int a19, a20, a21, a22, a23, a24, a25, a26, a27;
	int m1, m3, m5, m6, m7, m8, m9, m10, m11, m12;
	int x0, x1, x2, x3, x4, x5, x6, x7, x8;

	x0 = x[0]; x1 = x[1]; x2 = x[2]; x3 = x[3]; x4 = x[4];
	x5 = x[5]; x6 = x[6]; x7 = x[7]; x8 = x[8];

	a1 = x0 - x6;
	a2 = x1 - x5;
	a3 = x1 + x5;
	a4 = x2 - x4;
	a5 = x2 + x4;
	a6 = x2 + x8;
	a7 = x1 + x7;

	a8 = a6 - a5;
	a9 = a3 - a7;
	a10 = a2 - x7;
	a11 = a4 - x8;

	m1 =  IMDCT36_AMIGA_M68K_MULSHIFT32(c9_0, x3);
	m3 =  IMDCT36_AMIGA_M68K_MULSHIFT32(c9_0, a10);
	m5 =  IMDCT36_AMIGA_M68K_MULSHIFT32(c9_1, a5);
	m6 =  IMDCT36_AMIGA_M68K_MULSHIFT32(c9_2, a6);
	m7 =  IMDCT36_AMIGA_M68K_MULSHIFT32(c9_1, a8);
	m8 =  IMDCT36_AMIGA_M68K_MULSHIFT32(c9_2, a5);
	m9 =  IMDCT36_AMIGA_M68K_MULSHIFT32(c9_3, a9);
	m10 = IMDCT36_AMIGA_M68K_MULSHIFT32(c9_4, a7);
	m11 = IMDCT36_AMIGA_M68K_MULSHIFT32(c9_3, a3);
	m12 = IMDCT36_AMIGA_M68K_MULSHIFT32(c9_4, a9);

	a12 = x[0] +  (x[6] >> 1);
	a13 = a12  +  (  m1 << 1);
	a14 = a12  -  (  m1 << 1);
	a15 = a1   +  ( a11 >> 1);
	a16 = ( m5 << 1) + (m6 << 1);
	a17 = ( m7 << 1) - (m8 << 1);
	a18 = a16 + a17;
	a19 = ( m9 << 1) + (m10 << 1);
	a20 = (m11 << 1) - (m12 << 1);

	a21 = a20 - a19;
	a22 = a13 + a16;
	a23 = a14 + a16;
	a24 = a14 + a17;
	a25 = a13 + a17;
	a26 = a14 - a18;
	a27 = a13 - a18;

	x0 = a22 + a19;			x[0] = x0;
	x1 = a15 + (m3 << 1);	x[1] = x1;
	x2 = a24 + a20;			x[2] = x2;
	x3 = a26 - a21;			x[3] = x3;
	x4 = a1 - a11;			x[4] = x4;
	x5 = a27 + a21;			x[5] = x5;
	x6 = a25 - a20;			x[6] = x6;
	x7 = a15 - (m3 << 1);	x[7] = x7;
	x8 = a23 - a19;			x[8] = x8;
}

/*
 * Compact common-long-window kernel.  Keep the nine iterations in one asm
 * region so the 68030 does not pay C spill/reload overhead around the three
 * high-word multiplies in every iteration.  The instruction order mirrors the
 * C reference exactly; in particular, stores happen before FASTABS so INT_MIN
 * retains the reference result.
 */
static __inline int IMDCT36_AMIGA_M68K_LONG_WINDOW(int *xp, const int *cp,
	const int *wp, int *xPrev, int *ypLo, int *ypHi)
{
	int mOut;

	/*
	 * The loop below is the btCurr == 0 && btPrev == 0 C fast path:
	 *
	 *   c = *cp--; xo = *(xp + 9); xe = *xp--;
	 *   xo = MULSHIFT32(c, xo); xe >>= 2;
	 *   s = -*xPrev; d = -(xe - xo); *xPrev++ = xe + xo;
	 *   t = s - d;
	 *   yLo = d + (MULSHIFT32(t, *wp++) << 2);
	 *   yHi = s + (MULSHIFT32(t, *wp++) << 2);
	 *
	 * Instruction-group map for each of the nine DBRA iterations:
	 *   - xo: load xp[9] into d1, load *cp into d0, cp -= 1 int,
	 *     then muls.l d0,d2:d1 so d2 is MULSHIFT32(*old_cp, xp[9]).
	 *   - xe: load *xp into d0, xp -= 1 int, then asr.l #2 to match
	 *     xe >>= 2 exactly.
	 *   - s/d/xPrev: d3 = -*xPrev, d4 = xe + xo stored to
	 *     *xPrev++, and d5 = xo - xe (the C d = -(xe - xo)).
	 *   - yLo: recompute t = s - d in d1, multiply by *wp++, shift
	 *     the high word left by 2, add d, store through ypLo, advance
	 *     ypLo by NBANDS ints (128 bytes), and OR FASTABS(yLo) into mOut.
	 *   - yHi: recompute the same t, multiply by the next *wp++, shift
	 *     left by 2, add s, store through ypHi, retreat ypHi by NBANDS
	 *     ints (128 bytes), and OR FASTABS(yHi) into mOut.
	 */
	__asm__ volatile (
		"\tmoveq #0,%0\n\t"
		"moveq #8,%%d7\n"
		"1:\n\t"
		/* xo = MULSHIFT32(*cp--, xp[9]); xe = *xp-- >> 2 */
		"move.l 36(%1),%%d1\n\t"
		"move.l (%2),%%d0\n\t"
		"subq.l #4,%2\n\t"
		"muls.l %%d0,%%d2:%%d1\n\t"
		"move.l (%1),%%d0\n\t"
		"subq.l #4,%1\n\t"
		"asr.l #2,%%d0\n\t"
		/* s = -*xPrev; d = xo - xe; *xPrev++ = xe + xo */
		"move.l (%4),%%d3\n\t"
		"neg.l %%d3\n\t"
		"move.l %%d0,%%d4\n\t"
		"add.l %%d2,%%d4\n\t"
		"move.l %%d4,(%4)+\n\t"
		"move.l %%d2,%%d5\n\t"
		"sub.l %%d0,%%d5\n\t"
		/* yLo = d + (MULSHIFT32(s - d, *wp++) << 2) */
		"move.l %%d3,%%d1\n\t"
		"sub.l %%d5,%%d1\n\t"
		"move.l (%3)+,%%d0\n\t"
		"muls.l %%d0,%%d4:%%d1\n\t"
		"lsl.l #2,%%d4\n\t"
		"add.l %%d5,%%d4\n\t"
		"move.l %%d4,(%5)\n\t"
		"adda.l #128,%5\n\t"
		/* 68k immediate shifts only encode counts 1..8; ADD/SUBX makes the sign mask. */
		"move.l %%d4,%%d0\n\t"
		"add.l %%d0,%%d0\n\t"
		"subx.l %%d0,%%d0\n\t"
		"eor.l %%d0,%%d4\n\t"
		"sub.l %%d0,%%d4\n\t"
		"or.l %%d4,%0\n\t"
		/* yHi = s + (MULSHIFT32(s - d, *wp++) << 2) */
		"move.l %%d3,%%d1\n\t"
		"sub.l %%d5,%%d1\n\t"
		"move.l (%3)+,%%d0\n\t"
		"muls.l %%d0,%%d4:%%d1\n\t"
		"lsl.l #2,%%d4\n\t"
		"add.l %%d3,%%d4\n\t"
		"move.l %%d4,(%6)\n\t"
		"suba.l #128,%6\n\t"
		/* 68k immediate shifts only encode counts 1..8; ADD/SUBX makes the sign mask. */
		"move.l %%d4,%%d0\n\t"
		"add.l %%d0,%%d0\n\t"
		"subx.l %%d0,%%d0\n\t"
		"eor.l %%d0,%%d4\n\t"
		"sub.l %%d0,%%d4\n\t"
		"or.l %%d4,%0\n\t"
		"dbra %%d7,1b"
		: "=&d" (mOut), "+a" (xp), "+a" (cp), "+a" (wp),
		  "+a" (xPrev), "+a" (ypLo), "+a" (ypHi)
		:
		: "d0", "d1", "d2", "d3", "d4", "d5", "d7", "cc", "memory");

	return mOut;
}

static int IMDCT36_AMIGA_M68K_ASM(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb)
{
	int i, es, xBuf[18];
	int acc1, acc2, mOut;
	int *xp, *ypLo, *ypHi;
	const int *cp, *wp;

	if (btCurr != 0 || btPrev != 0)
		return IMDCT36_C_REFERENCE(xCurr, xPrev, y, btCurr, btPrev, blockIdx, gb);

	acc1 = acc2 = 0;
	xCurr += 17;
	if (gb < 7) {
		es = 7 - gb;
		for (i = 8; i >= 0; i--) {
			acc1 = ((*xCurr--) >> es) - acc1;
			acc2 = acc1 - acc2;
			acc1 = ((*xCurr--) >> es) - acc1;
			xBuf[i+9] = acc2;
			xBuf[i+0] = acc1;
			xPrev[i] >>= es;
		}
	} else {
		es = 0;
		for (i = 8; i >= 0; i--) {
			acc1 = (*xCurr--) - acc1;
			acc2 = acc1 - acc2;
			acc1 = (*xCurr--) - acc1;
			xBuf[i+9] = acc2;
			xBuf[i+0] = acc1;
		}
	}
	xBuf[9] >>= 1;
	xBuf[0] >>= 1;

	idct9_amiga_m68k_asm(xBuf+0);
	idct9_amiga_m68k_asm(xBuf+9);

	xp = xBuf + 8;
	cp = c18 + 8;
	wp = fastWin36;
	ypLo = y;
	ypHi = y + 17*NBANDS;
	mOut = IMDCT36_AMIGA_M68K_LONG_WINDOW(xp, cp, wp, xPrev, ypLo, ypHi);

	if (es)
		mOut |= FreqInvertRescale(y, xPrev, blockIdx, es);
	else if (blockIdx & 0x01)
		FreqInvertOdd(y);

	return mOut;
}
#endif

int IMDCT36_HAS_AMIGA_M68K_ASM_RUNTIME(void)
{
	return IMDCT36_HAS_AMIGA_M68K_ASM;
}

int IMDCT36_TEST_ACTIVE(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb)
{
#if IMDCT36_HAS_AMIGA_M68K_ASM
	if (btCurr == 0 && btPrev == 0)
		return IMDCT36_AMIGA_M68K_ASM(xCurr, xPrev, y, btCurr, btPrev, blockIdx, gb);
#endif
	return IMDCT36_C_REFERENCE(xCurr, xPrev, y, btCurr, btPrev, blockIdx, gb);
}

static int IMDCT36(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb)
{
	return IMDCT36_TEST_ACTIVE(xCurr, xPrev, y, btCurr, btPrev, blockIdx, gb);
}

#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_IMDCT_THIN_OUTPUT)
static __inline int IMDCTThinBlockSelected(const BlockCount *bc, int i)
{
	if (!bc->imdctThinActive)
		return 1;
	/* stride-4: polyphase reads every 4th subband starting at the phase offset */
	return ((i % bc->imdctThinStride) == (bc->imdctThinPhase % bc->imdctThinStride));
}

static int IMDCT36_ThinSkip(int *xCurr, int *xPrev, int gb)
{
	int i, es, xBuf[18];
	int acc1, acc2;
	int xo, xe, c, *xp;
	const int *cp;

	acc1 = acc2 = 0;
	xCurr += 17;

	/* 7 gb is always adequate for antialias + accumulator loop + idct9 */
	if (gb < 7) {
		/* preserve the reference guard-bit path's in-place xPrev scaling */
		es = 7 - gb;
		for (i = 8; i >= 0; i--) {
			acc1 = ((*xCurr--) >> es) - acc1;
			acc2 = acc1 - acc2;
			acc1 = ((*xCurr--) >> es) - acc1;
			xBuf[i+9] = acc2;	/* odd */
			xBuf[i+0] = acc1;	/* even */
			xPrev[i] >>= es;
		}
	} else {
		es = 0;
		(void)es;
		/* max gain = 18, assume adequate guard bits */
		for (i = 8; i >= 0; i--) {
			acc1 = (*xCurr--) - acc1;
			acc2 = acc1 - acc2;
			acc1 = (*xCurr--) - acc1;
			xBuf[i+9] = acc2;	/* odd */
			xBuf[i+0] = acc1;	/* even */
		}
	}
	/* xEven[0] and xOdd[0] scaled by 0.5 */
	xBuf[9] >>= 1;
	xBuf[0] >>= 1;

	/* always run both 9-point IDCTs so xPrev matches IMDCT36 */
	idct9(xBuf+0);	/* even */
	idct9(xBuf+9);	/* odd */

	xp = xBuf + 8;
	cp = c18 + 8;
	for (i = 9; i > 0; i--) {
		c = *cp--;	xo = *(xp + 9);		xe = *xp--;
		/* gain 2 int bits here */
		xo = MULSHIFT32(c, xo);			/* 2*c18*xOdd (mul by 2 implicit in scaling) */
		xe >>= 2;

		*xPrev++ = xe + xo;			/* symmetry - xPrev[i] = xPrev[17-i] for long blocks */
	}

	return 0;
}
#else
static __inline int IMDCTThinBlockSelected(const BlockCount *bc, int i)
{
	(void)bc; (void)i;
	return 1;
}
#endif

static int c3_0 = 0x6ed9eba1;	/* format = Q31, cos(pi/6) */
static int c6[3] = { 0x7ba3751d, 0x5a82799a, 0x2120fb83 };	/* format = Q31, cos(((0:2) + 0.5) * (pi/6)) */

/* 12-point inverse DCT, used in IMDCT12x3() 
 * 4 input guard bits will ensure no overflow
 */
static __inline void imdct12 (int *x, int *out)
{
	int a0, a1, a2;
	int x0, x1, x2, x3, x4, x5;

	x0 = *x;	x+=3;	x1 = *x;	x+=3;
	x2 = *x;	x+=3;	x3 = *x;	x+=3;
	x4 = *x;	x+=3;	x5 = *x;	x+=3;

	x4 -= x5;
	x3 -= x4;
	x2 -= x3;
	x3 -= x5;
	x1 -= x2;
	x0 -= x1;
	x1 -= x3;

	x0 >>= 1;
	x1 >>= 1;

	a0 = MULSHIFT32(c3_0, x2) << 1;
	a1 = x0 + (x4 >> 1);
	a2 = x0 - x4;
	x0 = a1 + a0;
	x2 = a2;
	x4 = a1 - a0;

	a0 = MULSHIFT32(c3_0, x3) << 1;
	a1 = x1 + (x5 >> 1);
	a2 = x1 - x5;

	/* cos window odd samples, mul by 2, eat sign bit */
	x1 = MULSHIFT32(c6[0], a1 + a0) << 2;			
	x3 = MULSHIFT32(c6[1], a2) << 2;
	x5 = MULSHIFT32(c6[2], a1 - a0) << 2;

	*out = x0 + x1;	out++;
	*out = x2 + x3;	out++;
	*out = x4 + x5;	out++;
	*out = x4 - x5;	out++;
	*out = x2 - x3;	out++;
	*out = x0 - x1;
}

/**************************************************************************************
 * Function:    IMDCT12x3
 *
 * Description: three 12-point modified DCT's for short blocks, with windowing,
 *                short block concatenation, and overlap-add
 *
 * Inputs:      3 interleaved vectors of 6 samples each 
 *                (block0[0], block1[0], block2[0], block0[1], block1[1]....)
 *              overlap part of last IMDCT (9 samples - see output comments)
 *              window type (0,1,2,3) of previous block
 *              current block index (for deciding whether to do frequency inversion)
 *              number of guard bits in input vector
 *
 * Outputs:     updated sample vector x, net gain of 1 integer bit
 *              second half of (unwindowed) IMDCT's - save for next time
 *                only save 9 xPrev samples, using symmetry (see WinPrevious())
 *
 * Return:      mOut (OR of abs(y) for all y calculated here)
 *
 * TODO:        optimize for ARM
 **************************************************************************************/
 // barely faster in RAM
static int IMDCT12x3(int *xCurr, int *xPrev, int *y, int btPrev, int blockIdx, int gb)
{
	int i, es, mOut, yLo, xBuf[18], xPrevWin[18];	/* need temp buffer for reordering short blocks */
	const int *wp;

	es = 0;
	/* 7 gb is always adequate for accumulator loop + idct12 + window + overlap */
	if (gb < 7) {
		es = 7 - gb;
		for (i = 0; i < 18; i+=2) {
			xCurr[i+0] >>= es;
			xCurr[i+1] >>= es;
			*xPrev++ >>= es;
		}
		xPrev -= 9;
	}

	/* requires 4 input guard bits for each imdct12 */
	imdct12(xCurr + 0, xBuf + 0);
	imdct12(xCurr + 1, xBuf + 6);
	imdct12(xCurr + 2, xBuf + 12);

	/* window previous from last time */
	WinPrevious(xPrev, xPrevWin, btPrev);

	/* could unroll this for speed, minimum loads (short blocks usually rare, so doesn't make much overall difference) 
	 * xPrevWin[i] << 2 still has 1 gb always, max gain of windowed xBuf stuff also < 1.0 and gain the sign bit
	 * so y calculations won't overflow
	 */
	wp = imdctWin[2];
	mOut = 0;
	for (i = 0; i < 3; i++) {
		yLo = (xPrevWin[ 0+i] << 2);
		mOut |= FASTABS(yLo);	y[( 0+i)*NBANDS] = yLo;
		yLo = (xPrevWin[ 3+i] << 2);
		mOut |= FASTABS(yLo);	y[( 3+i)*NBANDS] = yLo;
		yLo = (xPrevWin[ 6+i] << 2) + (MULSHIFT32(wp[0+i], xBuf[3+i]));	
		mOut |= FASTABS(yLo);	y[( 6+i)*NBANDS] = yLo;
		yLo = (xPrevWin[ 9+i] << 2) + (MULSHIFT32(wp[3+i], xBuf[5-i]));	
		mOut |= FASTABS(yLo);	y[( 9+i)*NBANDS] = yLo;
		yLo = (xPrevWin[12+i] << 2) + (MULSHIFT32(wp[6+i], xBuf[2-i]) + MULSHIFT32(wp[0+i], xBuf[(6+3)+i]));	
		mOut |= FASTABS(yLo);	y[(12+i)*NBANDS] = yLo;
		yLo = (xPrevWin[15+i] << 2) + (MULSHIFT32(wp[9+i], xBuf[0+i]) + MULSHIFT32(wp[3+i], xBuf[(6+5)-i]));	
		mOut |= FASTABS(yLo);	y[(15+i)*NBANDS] = yLo;
	}

	/* save previous (unwindowed) for overlap - only need samples 6-8, 12-17 */
	for (i = 6; i < 9; i++)
		*xPrev++ = xBuf[i] >> 2;
	for (i = 12; i < 18; i++)
		*xPrev++ = xBuf[i] >> 2;

	xPrev -= 9;
	if (es)
		mOut |= FreqInvertRescale(y, xPrev, blockIdx, es);
	else if (blockIdx & 0x01)
		FreqInvertOdd(y);

	return mOut;
}

/**************************************************************************************
 * Function:    HybridTransform
 *
 * Description: IMDCT's, windowing, and overlap-add on long/short/mixed blocks
 *
 * Inputs:      vector of input coefficients, length = nBlocksTotal * 18)
 *              vector of overlap samples from last time, length = nBlocksPrev * 9)
 *              buffer for output samples, length = MAXNSAMP
 *              SideInfoSub struct for this granule/channel
 *              BlockCount struct with necessary info
 *                number of non-zero input and overlap blocks
 *                number of long blocks in input vector (rest assumed to be short blocks)
 *                number of blocks which use long window (type) 0 in case of mixed block
 *                  (bc->currWinSwitch, 0 for non-mixed blocks)
 *
 * Outputs:     transformed, windowed, and overlapped sample buffer
 *              does frequency inversion on odd blocks
 *              updated buffer of samples for overlap
 *
 * Return:      number of non-zero IMDCT blocks calculated in this call
 *                (including overlap-add)
 *
 * TODO:        examine mixedBlock/winSwitch logic carefully (test he_mode.bit)
 **************************************************************************************/
static int HybridTransform(int *xCurr, int *xPrev, int y[BLOCK_SIZE][NBANDS], SideInfoSub *sis, BlockCount *bc)
{
	int xPrevWin[18], currWinIdx, prevWinIdx;
	int i, j, nBlocksOut, nonZero, mOut;
	int fiBit, xp;
	int blockType, mixedBlock, prevType, prevWinSwitch, currWinSwitch;

	ASSERT(bc->nBlocksLong  <= NBANDS);
	ASSERT(bc->nBlocksTotal <= NBANDS);
	ASSERT(bc->nBlocksPrev  <= NBANDS);

	mOut = 0;
	blockType = sis->blockType;
	mixedBlock = sis->mixedBlock;
	prevType = bc->prevType;
	prevWinSwitch = bc->prevWinSwitch;
	currWinSwitch = bc->currWinSwitch;

	/* do long blocks, if any */
	if (!mixedBlock && prevWinSwitch == 0) {
		currWinIdx = blockType;
		prevWinIdx = prevType;
		for(i = 0; i < bc->nBlocksLong; i++) {
			/* do 36-point IMDCT, including windowing and overlap-add */
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_IMDCT_THIN_OUTPUT)
			if (IMDCTThinBlockSelected(bc, i))
				mOut |= IMDCT36(xCurr, xPrev, &(y[0][i]), currWinIdx, prevWinIdx, i, bc->gbIn);
			else {
				int row;
				mOut |= IMDCT36_ThinSkip(xCurr, xPrev, bc->gbIn);
				for (row = 0; row < BLOCK_SIZE; row++)
					y[row][i] = 0;
			}
#else
			mOut |= IMDCT36(xCurr, xPrev, &(y[0][i]), currWinIdx, prevWinIdx, i, bc->gbIn);
#endif
			xCurr += 18;
			xPrev += 9;
		}
	} else {
		for(i = 0; i < bc->nBlocksLong; i++) {
			/* currWinIdx picks the right window for long blocks (if mixed, long blocks use window type 0) */
			currWinIdx = blockType;
			if (mixedBlock && i < currWinSwitch)
				currWinIdx = 0;

			prevWinIdx = prevType;
			if (i < prevWinSwitch)
				 prevWinIdx = 0;

			/* do 36-point IMDCT, including windowing and overlap-add.
			 * Mixed/transition-window long blocks deliberately stay on the C reference path.
			 */
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_IMDCT_THIN_OUTPUT)
			if (IMDCTThinBlockSelected(bc, i))
				mOut |= IMDCT36_C_REFERENCE(xCurr, xPrev, &(y[0][i]), currWinIdx, prevWinIdx, i, bc->gbIn);
			else {
				int row;
				mOut |= IMDCT36_ThinSkip(xCurr, xPrev, bc->gbIn);
				for (row = 0; row < BLOCK_SIZE; row++)
					y[row][i] = 0;
			}
#else
			mOut |= IMDCT36_C_REFERENCE(xCurr, xPrev, &(y[0][i]), currWinIdx, prevWinIdx, i, bc->gbIn);
#endif
			xCurr += 18;
			xPrev += 9;
		}
	}

	/* do short blocks (if any) */
	for (   ; i < bc->nBlocksTotal; i++) {
		ASSERT(blockType == 2);

		prevWinIdx = prevType;
		if (i < prevWinSwitch)
			 prevWinIdx = 0;
		
		mOut |= IMDCT12x3(xCurr, xPrev, &(y[0][i]), prevWinIdx, i, bc->gbIn);
		xCurr += 18;
		xPrev += 9;
	}
	nBlocksOut = i;
	
	/* window and overlap prev if prev longer that current */
	for (   ; i < bc->nBlocksPrev; i++) {
		prevWinIdx = prevType;
		if (i < prevWinSwitch)
			 prevWinIdx = 0;
		WinPrevious(xPrev, xPrevWin, prevWinIdx);

		nonZero = 0;
		fiBit = i << 31;
		for (j = 0; j < 9; j++) {
			xp = xPrevWin[2*j+0] << 2;	/* << 2 temp for scaling */
			nonZero |= xp;
			y[2*j+0][i] = xp;
			mOut |= FASTABS(xp);

			/* frequency inversion on odd blocks/odd samples (flip sign if i odd, j odd) */
			xp = xPrevWin[2*j+1] << 2;
			xp = (xp ^ (fiBit >> 31)) + (i & 0x01);	
			nonZero |= xp;
			y[2*j+1][i] = xp;
			mOut |= FASTABS(xp);

			xPrev[j] = 0;
		}
		xPrev += 9;
		if (nonZero)
			nBlocksOut = i;
	}
	
	/* clear rest of blocks */
	for (   ; i < 32; i++) {
		int *yp;

		yp = &(y[0][i]);
		for (j = 18; j > 0; j--) {
			*yp = 0;
			yp += NBANDS;
		}
	}

	if (bc->subbandCapActive && bc->activeSubbands < NBANDS) {
		int row, band;
		for (row = 0; row < BLOCK_SIZE; row++)
			for (band = bc->activeSubbands; band < NBANDS; band++)
				y[row][band] = 0;
		MP3AddDecodeCoreIMDCTSubbands((unsigned long)nBlocksOut,
			(unsigned long)(NBANDS - bc->activeSubbands));
	} else {
		MP3AddDecodeCoreIMDCTSubbands((unsigned long)nBlocksOut, 0);
	}

	bc->gbOut = CLZ(mOut) - 1;

	return nBlocksOut;
}


static unsigned int IMDCTThinChecksumPCM(const int y[BLOCK_SIZE][NBANDS], int gb)
{
	int vbuf[MAX_NCHAN * VBUF_LENGTH];
	short pcm[BLOCK_SIZE * NBANDS];
	int tmp[NBANDS];
	int phase;
	int vindex;
	int out;
	int b;
	int i;
	unsigned int sum;

	memset(vbuf, 0, sizeof(vbuf));
	memset(pcm, 0, sizeof(pcm));
	phase = 0;
	vindex = 0;
	out = 0;
	for (b = 0; b < BLOCK_SIZE; b += 2) {
		memcpy(tmp, y[b], sizeof(tmp));
		FDCT32(tmp, vbuf, vindex, 0, gb);
		out += PolyphaseMonoFastLowrate(pcm + out, vbuf + vindex, polyCoef, 4, &phase);
		memcpy(tmp, y[b + 1], sizeof(tmp));
		FDCT32(tmp, vbuf, vindex, 1, gb);
		out += PolyphaseMonoFastLowrate(pcm + out, vbuf + vindex + VBUF_LENGTH, polyCoef, 4, &phase);
		vindex = (vindex - 1) & 7;
	}

	sum = 2166136261UL;
	for (i = 0; i < out; i++) {
		sum ^= (unsigned short)pcm[i];
		sum *= 16777619U;
	}
	return sum;
}

static int IMDCTCountHighSubbandNonZero(const int y[BLOCK_SIZE][NBANDS])
{
	int t;
	int sb;
	int count;

	count = 0;
	for (t = 0; t < BLOCK_SIZE; t++) {
		for (sb = 16; sb < NBANDS; sb++) {
			if (y[t][sb] != 0)
				count++;
		}
	}
	return count;
}

int IMDCTSubbandCapSelftest(void)
{
	int xUncap[MAX_NSAMP];
	int xCap[MAX_NSAMP];
	int prevUncap[MAX_NSAMP / 2];
	int prevCap[MAX_NSAMP / 2];
	int yUncap[BLOCK_SIZE][NBANDS];
	int yCap[BLOCK_SIZE][NBANDS];
	SideInfoSub sis;
	BlockCount baseBc;
	BlockCount uncapBc;
	BlockCount capBc;
	BlockCount helperBc;
	MP3DecInfo decInfo;
	int i;
	int outUncap;
	int outCap;
	int highNonZero;
	int helperApplied;
	unsigned int pcmUncap;
	unsigned int pcmCap;
	int failures;

	for (i = 0; i < MAX_NSAMP; i++) {
		xUncap[i] = ((i * 1103515245UL + 12345UL) & 0x001fffff) - 0x00100000;
		xCap[i] = xUncap[i];
	}
	for (i = 0; i < MAX_NSAMP / 2; i++) {
		prevUncap[i] = ((i * 69069UL + 1UL) & 0x0007ffff) - 0x00040000;
		prevCap[i] = prevUncap[i];
	}
	memset(yUncap, 0xa5, sizeof(yUncap));
	memset(yCap, 0xa5, sizeof(yCap));
	memset(&sis, 0, sizeof(sis));
	memset(&baseBc, 0, sizeof(baseBc));
	memset(&decInfo, 0, sizeof(decInfo));

	sis.blockType = 0;
	sis.mixedBlock = 0;
	baseBc.nBlocksLong = 32;
	baseBc.nBlocksTotal = 32;
	baseBc.nBlocksPrev = 32;
	baseBc.prevType = 0;
	baseBc.prevWinSwitch = 0;
	baseBc.currWinSwitch = 0;
	baseBc.gbIn = 8;
	uncapBc = baseBc;
	capBc = baseBc;
	capBc.nBlocksLong = 16;
	capBc.nBlocksTotal = 16;
	capBc.nBlocksPrev = 16;
	capBc.subbandCapActive = 1;

	outUncap = HybridTransform(xUncap, prevUncap, yUncap, &sis, &uncapBc);
	outCap = HybridTransform(xCap, prevCap, yCap, &sis, &capBc);
	pcmUncap = IMDCTThinChecksumPCM(yUncap, uncapBc.gbOut);
	pcmCap = IMDCTThinChecksumPCM(yCap, capBc.gbOut);
	highNonZero = IMDCTCountHighSubbandNonZero(yUncap);

	failures = 0;
	if (pcmUncap == pcmCap) {
		printf("subband cap selftest checksum did not change: uncapped=%lu capped=%lu\n",
			(unsigned long)pcmUncap, (unsigned long)pcmCap);
		failures++;
	}
	if (highNonZero == 0) {
		printf("subband cap selftest found no non-zero uncapped subband 16-31 samples\n");
		failures++;
	}
	if (outUncap <= outCap) {
		printf("subband cap selftest nBlocksOut unexpected: uncapped=%d capped=%d\n",
			outUncap, outCap);
		failures++;
	}

	decInfo.superfastLowrate = 1;
	decInfo.fastLowrateStride = 2;
	decInfo.fastLowrateActiveSubbands = 16;
	helperBc = baseBc;
	helperApplied = IMDCTApplySubbandCap(&decInfo, &helperBc);
	if (!helperApplied || helperBc.nBlocksTotal != 16 || helperBc.nBlocksLong != 16 ||
		helperBc.nBlocksPrev != 16 || helperBc.activeSubbands != 16 || !helperBc.subbandCapActive) {
		printf("subband cap selftest superfast stride-2 helper failed: applied=%d total=%d long=%d prev=%d activeBands=%d active=%d\n",
			helperApplied, helperBc.nBlocksTotal, helperBc.nBlocksLong, helperBc.nBlocksPrev,
			helperBc.activeSubbands, helperBc.subbandCapActive);
		failures++;
	}

	decInfo.fastLowrateStride = 4;
	decInfo.fastLowrateActiveSubbands = 8;
	decInfo.outputMono = 1;
	helperBc = baseBc;
	helperApplied = IMDCTApplySubbandCap(&decInfo, &helperBc);
	if (!helperApplied || helperBc.nBlocksTotal != 8 || helperBc.nBlocksLong != 8 ||
		helperBc.nBlocksPrev != 8 || helperBc.activeSubbands != 8 || !helperBc.subbandCapActive) {
		printf("subband cap selftest superfast stride-4 helper failed: applied=%d total=%d long=%d prev=%d activeBands=%d active=%d\n",
			helperApplied, helperBc.nBlocksTotal, helperBc.nBlocksLong, helperBc.nBlocksPrev,
			helperBc.activeSubbands, helperBc.subbandCapActive);
		failures++;
	}

	decInfo.fastLowrateStride = 5;
	decInfo.fastLowrateActiveSubbands = 6;
	decInfo.outputMono = 0;
	helperBc = baseBc;
	helperApplied = IMDCTApplySubbandCap(&decInfo, &helperBc);
	if (!helperApplied || helperBc.nBlocksTotal != 6 || helperBc.nBlocksLong != 6 ||
		helperBc.nBlocksPrev != 6 || helperBc.activeSubbands != 6 || !helperBc.subbandCapActive) {
		printf("subband cap selftest superfast stride-5 helper failed: applied=%d total=%d long=%d prev=%d activeBands=%d active=%d\n",
			helperApplied, helperBc.nBlocksTotal, helperBc.nBlocksLong, helperBc.nBlocksPrev,
			helperBc.activeSubbands, helperBc.subbandCapActive);
		failures++;
	}

	memset(&decInfo, 0, sizeof(decInfo));
	decInfo.fastLowrateStride = 4;
	decInfo.outputMono = 1;
	helperBc = baseBc;
	helperApplied = IMDCTApplySubbandCap(&decInfo, &helperBc);
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_SUBBAND_CAP)
	if (!helperApplied || helperBc.nBlocksTotal != 16 || helperBc.nBlocksLong != 16 ||
		helperBc.nBlocksPrev != 16 || !helperBc.subbandCapActive) {
		printf("subband cap selftest stride-4 helper failed: applied=%d total=%d long=%d prev=%d active=%d\n",
			helperApplied, helperBc.nBlocksTotal, helperBc.nBlocksLong, helperBc.nBlocksPrev,
			helperBc.subbandCapActive);
		failures++;
	}
#else
	if (helperApplied || helperBc.nBlocksTotal != 32 || helperBc.nBlocksLong != 32 ||
		helperBc.nBlocksPrev != 32) {
		printf("subband cap selftest compile-gated helper changed state while unavailable\n");
		failures++;
	}
#endif

	decInfo.fastLowrateStride = 1;
	decInfo.outputMono = 1;
	helperBc = baseBc;
	helperApplied = IMDCTApplySubbandCap(&decInfo, &helperBc);
	if (helperApplied || helperBc.nBlocksTotal != 32 || helperBc.nBlocksLong != 32 ||
		helperBc.nBlocksPrev != 32 || helperBc.subbandCapActive) {
		printf("subband cap selftest full-rate helper unexpectedly applied: applied=%d total=%d long=%d prev=%d active=%d\n",
			helperApplied, helperBc.nBlocksTotal, helperBc.nBlocksLong, helperBc.nBlocksPrev,
			helperBc.subbandCapActive);
		failures++;
	}

	printf("subband cap compile gate: %s\n",
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_SUBBAND_CAP)
		"available"
#else
		"unavailable"
#endif
	);
	printf("subband cap uncapped PCM checksum: %lu\n", (unsigned long)pcmUncap);
	printf("subband cap capped PCM checksum: %lu\n", (unsigned long)pcmCap);
	printf("subband cap uncapped non-zero y samples in subbands 16-31: %d\n", highNonZero);
	printf("subband cap full-rate stride-1 helper: not applied\n");
	printf("subband cap selftest: %s\n", failures ? "FAIL" : "PASS");
	return failures ? -1 : 0;
}

int IMDCTThinOutputSelftest(void)
{
	int xFull[MAX_NSAMP];
	int xThin[MAX_NSAMP];
	int prevFull[MAX_NSAMP / 2];
	int prevThin[MAX_NSAMP / 2];
	int yFull[BLOCK_SIZE][NBANDS];
	int yThin[BLOCK_SIZE][NBANDS];
	int yExpected[BLOCK_SIZE][NBANDS];
	SideInfoSub sis;
	BlockCount fullBc;
	BlockCount thinBc;
	int i;
	int outFull;
	int outThin;
	unsigned int pcmFull;
	unsigned int pcmThin;
	unsigned int pcmExpected;
	int failures;

	for (i = 0; i < MAX_NSAMP; i++) {
		xFull[i] = ((i * 1103515245UL + 12345UL) & 0x001fffff) - 0x00100000;
		xThin[i] = xFull[i];
	}
	for (i = 0; i < MAX_NSAMP / 2; i++) {
		prevFull[i] = ((i * 69069UL + 1UL) & 0x0007ffff) - 0x00040000;
		prevThin[i] = prevFull[i];
	}
	memset(yFull, 0xa5, sizeof(yFull));
	memset(yThin, 0xa5, sizeof(yThin));
	memset(yExpected, 0, sizeof(yExpected));
	memset(&sis, 0, sizeof(sis));
	memset(&fullBc, 0, sizeof(fullBc));
	memset(&thinBc, 0, sizeof(thinBc));

	sis.blockType = 0;
	sis.mixedBlock = 0;
	fullBc.nBlocksLong = 32;
	fullBc.nBlocksTotal = 32;
	fullBc.nBlocksPrev = 32;
	fullBc.prevType = 0;
	fullBc.prevWinSwitch = 0;
	fullBc.currWinSwitch = 0;
	fullBc.gbIn = 8;
	thinBc = fullBc;
	thinBc.imdctThinActive = 1;
	thinBc.imdctThinStride = 4;
	thinBc.imdctThinPhase = 0;

	outFull = HybridTransform(xFull, prevFull, yFull, &sis, &fullBc);
	outThin = HybridTransform(xThin, prevThin, yThin, &sis, &thinBc);
	for (i = 0; i < NBANDS; i++) {
		int t;
		if ((i % thinBc.imdctThinStride) == thinBc.imdctThinPhase)
			for (t = 0; t < BLOCK_SIZE; t++)
				yExpected[t][i] = yFull[t][i];
	}
	pcmFull = IMDCTThinChecksumPCM(yFull, fullBc.gbOut);
	pcmThin = IMDCTThinChecksumPCM(yThin, thinBc.gbOut);
	pcmExpected = IMDCTThinChecksumPCM(yExpected, fullBc.gbOut);

	failures = 0;
	if (outFull != outThin) {
		printf("IMDCT thin nBlocksOut mismatch: full=%d thin=%d\n", outFull, outThin);
		failures++;
	}
	if (memcmp(prevFull, prevThin, sizeof(prevFull)) != 0) {
		printf("IMDCT thin xPrev mismatch\n");
		failures++;
	}
	if (memcmp(yExpected, yThin, sizeof(yExpected)) != 0) {
		int t, sb;
		for (t = 0; t < BLOCK_SIZE; t++) {
			for (sb = 0; sb < NBANDS; sb++) {
				if (yExpected[t][sb] != yThin[t][sb]) {
					printf("IMDCT thin sparse y[] mismatch: index=%d subband=%d time=%d blockType=%d stride=%d full=%ld thin=%ld\n",
						t * NBANDS + sb, sb, t, sis.blockType, thinBc.imdctThinStride,
						(long)yExpected[t][sb], (long)yThin[t][sb]);
					t = BLOCK_SIZE;
					break;
				}
			}
		}
		failures++;
	}
	if (pcmThin != pcmExpected) {
		printf("IMDCT thin deterministic sparse PCM checksum mismatch: expected=%lu thin=%lu full=%lu\n",
			(unsigned long)pcmExpected, (unsigned long)pcmThin, (unsigned long)pcmFull);
		failures++;
	}

	printf("IMDCT thin compile gate: %s\n",
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_IMDCT_THIN_OUTPUT)
		"available"
#else
		"unavailable"
#endif
	);
	printf("IMDCT thin runtime activation: %s\n",
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_IMDCT_THIN_OUTPUT)
		"enabled for --exp-imdct-thin stride-4 mono fast-lowrate"
#else
		"disabled (compile gate unavailable)"
#endif
	);
	printf("IMDCT thin full PCM checksum: %lu\n", (unsigned long)pcmFull);
	printf("IMDCT thin deterministic sparse PCM checksum: %lu\n", (unsigned long)pcmExpected);
	printf("IMDCT thin selftest failures: %d\n", failures);
	return failures ? -1 : 0;
}

/**************************************************************************************
 * Function:    IMDCT
 *
 * Description: do alias reduction, inverse MDCT, overlap-add, and frequency inversion
 *
 * Inputs:      MP3DecInfo structure filled by UnpackFrameHeader(), UnpackSideInfo(),
 *                UnpackScaleFactors(), and DecodeHuffman() (for this granule, channel)
 *                includes PCM samples in overBuf (from last call to IMDCT) for OLA
 *              index of current granule and channel
 *
 * Outputs:     PCM samples in outBuf, for input to subband transform
 *              PCM samples in overBuf, for OLA next time
 *              updated hi->nonZeroBound index for this channel
 *
 * Return:      0 on success,  -1 if null input pointers
 **************************************************************************************/
 // a bit faster in RAM
int IMDCT(MP3DecInfo *mp3DecInfo, int gr, int ch)
{
	int nBfly, blockCutoff;
	FrameHeader *fh;
	SideInfo *si;
	HuffmanInfo *hi;
	IMDCTInfo *mi;
	BlockCount bc;

	/* validate pointers */
	if (!mp3DecInfo || !mp3DecInfo->FrameHeaderPS || !mp3DecInfo->SideInfoPS || 
		!mp3DecInfo->HuffmanInfoPS || !mp3DecInfo->IMDCTInfoPS)
		return -1;

	/* si is an array of up to 4 structs, stored as gr0ch0, gr0ch1, gr1ch0, gr1ch1 */
	fh = (FrameHeader *)(mp3DecInfo->FrameHeaderPS);
	si = (SideInfo *)(mp3DecInfo->SideInfoPS);
	hi = (HuffmanInfo*)(mp3DecInfo->HuffmanInfoPS);
	mi = (IMDCTInfo *)(mp3DecInfo->IMDCTInfoPS);

	/* anti-aliasing done on whole long blocks only
	 * for mixed blocks, nBfly always 1, except 3 for 8 kHz MPEG 2.5 (see sfBandTab) 
     *   nLongBlocks = number of blocks with (possibly) non-zero power 
	 *   nBfly = number of butterflies to do (nLongBlocks - 1, unless no long blocks)
	 */
	blockCutoff = fh->sfBand->l[(fh->ver == MPEG1 ? 8 : 6)] / 18;	/* same as 3* num short sfb's in spec */
	if (si->sis[gr][ch].blockType != 2) {
		/* all long transforms */
		bc.nBlocksLong = MIN((hi->nonZeroBound[ch] + 7) / 18 + 1, 32);	
		nBfly = bc.nBlocksLong - 1;
	} else if (si->sis[gr][ch].blockType == 2 && si->sis[gr][ch].mixedBlock) {
		/* mixed block - long transforms until cutoff, then short transforms */
		bc.nBlocksLong = blockCutoff;	
		nBfly = bc.nBlocksLong - 1;
	} else {
		/* all short transforms */
		bc.nBlocksLong = 0;
		nBfly = 0;
	}
 
	AntiAlias(hi->huffDecBuf[ch], nBfly);
	hi->nonZeroBound[ch] = MAX(hi->nonZeroBound[ch], (nBfly * 18) + 8);

	ASSERT(hi->nonZeroBound[ch] <= MAX_NSAMP);

	/* for readability, use a struct instead of passing a million parameters to HybridTransform() */
	bc.nBlocksTotal = (hi->nonZeroBound[ch] + 17) / 18;
	bc.nBlocksPrev = mi->numPrevIMDCT[ch];
	IMDCTApplySubbandCap(mp3DecInfo, &bc);
	bc.prevType = mi->prevType[ch];
	bc.prevWinSwitch = mi->prevWinSwitch[ch];
	bc.currWinSwitch = (si->sis[gr][ch].mixedBlock ? blockCutoff : 0);	/* where WINDOW switches (not nec. transform) */
	bc.gbIn = hi->gb[ch];
	bc.imdctThinStride = mp3DecInfo->fastLowrateStride;
	bc.imdctThinPhase = mp3DecInfo->fastLowratePhase;
	bc.imdctThinActive = IMDCTThinOutputCanActivate(mp3DecInfo);
	mp3DecInfo->imdctThinActive = bc.imdctThinActive;

	mi->numPrevIMDCT[ch] = HybridTransform(hi->huffDecBuf[ch], mi->overBuf[ch], mi->outBuf[ch], &si->sis[gr][ch], &bc);
	if (bc.subbandCapActive) {
		IMDCTClearDiscardedSubbands(mi, ch, bc.activeSubbands);
		if (mi->numPrevIMDCT[ch] > bc.activeSubbands)
			mi->numPrevIMDCT[ch] = bc.activeSubbands;
	}
	mi->prevType[ch] = si->sis[gr][ch].blockType;
	mi->prevWinSwitch[ch] = bc.currWinSwitch;		/* 0 means not a mixed block (either all short or all long) */
	mi->gb[ch] = bc.gbOut;

	ASSERT(mi->numPrevIMDCT[ch] <= NBANDS);

	/* output has gained 2 int bits */
	return 0;
}
