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

/**************************************************************************************
 * Function:    AntiAlias
 *
 * Description: smooth transition across DCT block boundaries (every 18 coefficients)
 *
 * Inputs:      vector of dequantized coefficients, length = (nBfly+1) * 18
 *              number of "butterflies" to perform (one butterfly means one
 *                inter-block smoothing operation)
 *
 * Outputs:     updated coefficient vector x
 *
 * Return:      none
 *
 * Notes:       weighted average of opposite bands (pairwise) from the 8 samples 
 *                before and after each block boundary
 *              nBlocks = (nonZeroBound + 7) / 18, since nZB is the first ZERO sample 
 *                above which all other samples are also zero
 *              max gain per sample = 1.372
 *                MAX(i) (abs(csa[i][0]) + abs(csa[i][1]))
 *              bits gained = 0
 *              assume at least 1 guard bit in x[] to avoid overflow
 *                (should be guaranteed from dequant, and max gain from stproc * max 
 *                 gain from AntiAlias < 2.0)
 **************************************************************************************/
#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_ANTIALIAS) && \
	defined(__GNUC__) && (defined(__mc68020__) || defined(__mc68030__) || \
	defined(__mc68040__) || defined(__mc68060__) || defined(mc68020))
#define ANTIALIAS_HAS_AMIGA_M68K_ASM 1
#else
#define ANTIALIAS_HAS_AMIGA_M68K_ASM 0
#endif

// a little bit faster in RAM (< 1 ms per block)
void AntiAlias_C_REFERENCE(int *x, int nBfly)
{
	int k, a0, b0, c0, c1;
	const int *c;

	/* csa = Q31 */
	for (k = nBfly; k > 0; k--) {
		c = csa[0];
		x += 18;

		a0 = x[-1];			c0 = *c;	c++;	b0 = x[0];		c1 = *c;	c++;
		x[-1] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;	
		x[0] =  (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

		a0 = x[-2];			c0 = *c;	c++;	b0 = x[1];		c1 = *c;	c++;
		x[-2] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;	
		x[1] =  (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;
		
		a0 = x[-3];			c0 = *c;	c++;	b0 = x[2];		c1 = *c;	c++;
		x[-3] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;	
		x[2] =  (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

		a0 = x[-4];			c0 = *c;	c++;	b0 = x[3];		c1 = *c;	c++;
		x[-4] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;	
		x[3] =  (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

		a0 = x[-5];			c0 = *c;	c++;	b0 = x[4];		c1 = *c;	c++;
		x[-5] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;	
		x[4] =  (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

		a0 = x[-6];			c0 = *c;	c++;	b0 = x[5];		c1 = *c;	c++;
		x[-6] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;	
		x[5] =  (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

		a0 = x[-7];			c0 = *c;	c++;	b0 = x[6];		c1 = *c;	c++;
		x[-7] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;	
		x[6] =  (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

		a0 = x[-8];			c0 = *c;	c++;	b0 = x[7];		c1 = *c;	c++;
		x[-8] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;	
		x[7] =  (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;
	}
}

#if ANTIALIAS_HAS_AMIGA_M68K_ASM
static __inline void AntiAlias_AmigaM68KBoundary(int *x)
{
	const int *c;
	int *xLo;
	int *xHi;

	c = csa[0];
	xLo = x;
	xHi = x;
	__asm__ volatile (
		"moveq #7,%%d7\n"
		"1:\n\t"
		/* Pre-decrement xLo and post-increment xHi to visit x[-1..-8] and x[0..7]. */
		"move.l -(%[xlo]),%%d0\n\t"
		"move.l (%[xhi])+,%%d1\n\t"
		/* xLo = MULSHIFT32(c0, xLo) - MULSHIFT32(c1, xHi) */
		"move.l %%d0,%%d4\n\t"
		"muls.l (%[c]),%%d5:%%d4\n\t"
		"move.l %%d1,%%d2\n\t"
		"muls.l 4(%[c]),%%d6:%%d2\n\t"
		"sub.l %%d6,%%d5\n\t"
		/* xHi = MULSHIFT32(c0, xHi) + MULSHIFT32(c1, xLo) */
		"move.l %%d1,%%d2\n\t"
		"muls.l (%[c]),%%d3:%%d2\n\t"
		"move.l %%d0,%%d4\n\t"
		"muls.l 4(%[c]),%%d6:%%d4\n\t"
		"add.l %%d6,%%d3\n\t"
		"add.l %%d5,%%d5\n\t"
		"add.l %%d3,%%d3\n\t"
		"move.l %%d5,4(%[xlo])\n\t"
		"move.l %%d3,-4(%[xhi])\n\t"
		"addq.l #8,%[c]\n\t"
		"dbra %%d7,1b"
		: [c] "+a" (c), [xlo] "+a" (xLo), [xhi] "+a" (xHi)
		:
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "cc", "memory");
}

static void AntiAlias_AmigaM68KAsm(int *x, int nBfly)
{
	int k;

	for (k = nBfly; k > 0; k--) {
		x += 18;
		AntiAlias_AmigaM68KBoundary(x);
	}
}
#endif

int AntiAlias_HAS_AMIGA_M68K_ASM_RUNTIME(void)
{
	return ANTIALIAS_HAS_AMIGA_M68K_ASM;
}

void AntiAlias_TEST_ACTIVE(int *x, int nBfly)
{
#if ANTIALIAS_HAS_AMIGA_M68K_ASM
	AntiAlias_AmigaM68KAsm(x, nBfly);
#else
	AntiAlias_C_REFERENCE(x, nBfly);
#endif
}

static void AntiAlias(int *x, int nBfly)
{
	AntiAlias_TEST_ACTIVE(x, nBfly);
}

/**************************************************************************************
 * Function:    WinPrevious
 *
 * Description: apply specified window to second half of previous IMDCT (overlap part)
 *
 * Inputs:      vector of 9 coefficients (xPrev)
 *
 * Outputs:     18 windowed output coefficients (gain 1 integer bit)
 *              window type (0, 1, 2, 3)
 *
 * Return:      none
 * 
 * Notes:       produces 9 output samples from 18 input samples via symmetry
 *              all blocks gain at least 1 guard bit via window (long blocks get extra
 *                sign bit, short blocks can have one addition but max gain < 1.0)
 **************************************************************************************/
static void WinPrevious(int *xPrev, int *xPrevWin, int btPrev)
{
	int i, x, *xp, *xpwLo, *xpwHi, wLo, wHi;
	const int *wpLo, *wpHi;

	xp = xPrev;
	/* mapping (see IMDCT12x3): xPrev[0-2] = sum[6-8], xPrev[3-8] = sum[12-17] */
	if (btPrev == 2) {
		/* this could be reordered for minimum loads/stores */
		wpLo = imdctWin[btPrev];
		xPrevWin[ 0] = MULSHIFT32(wpLo[ 6], xPrev[2]) + MULSHIFT32(wpLo[0], xPrev[6]);
		xPrevWin[ 1] = MULSHIFT32(wpLo[ 7], xPrev[1]) + MULSHIFT32(wpLo[1], xPrev[7]);
		xPrevWin[ 2] = MULSHIFT32(wpLo[ 8], xPrev[0]) + MULSHIFT32(wpLo[2], xPrev[8]);
		xPrevWin[ 3] = MULSHIFT32(wpLo[ 9], xPrev[0]) + MULSHIFT32(wpLo[3], xPrev[8]);
		xPrevWin[ 4] = MULSHIFT32(wpLo[10], xPrev[1]) + MULSHIFT32(wpLo[4], xPrev[7]);
		xPrevWin[ 5] = MULSHIFT32(wpLo[11], xPrev[2]) + MULSHIFT32(wpLo[5], xPrev[6]);
		xPrevWin[ 6] = MULSHIFT32(wpLo[ 6], xPrev[5]);
		xPrevWin[ 7] = MULSHIFT32(wpLo[ 7], xPrev[4]);
		xPrevWin[ 8] = MULSHIFT32(wpLo[ 8], xPrev[3]);
		xPrevWin[ 9] = MULSHIFT32(wpLo[ 9], xPrev[3]);
		xPrevWin[10] = MULSHIFT32(wpLo[10], xPrev[4]);
		xPrevWin[11] = MULSHIFT32(wpLo[11], xPrev[5]);
		xPrevWin[12] = xPrevWin[13] = xPrevWin[14] = xPrevWin[15] = xPrevWin[16] = xPrevWin[17] = 0;
	} else {
		/* use ARM-style pointers (*ptr++) so that ADS compiles well */
		wpLo = imdctWin[btPrev] + 18;
		wpHi = wpLo + 17;
		xpwLo = xPrevWin;
		xpwHi = xPrevWin + 17;
		for (i = 9; i > 0; i--) {
			x = *xp++;	wLo = *wpLo++;	wHi = *wpHi--;
			*xpwLo++ = MULSHIFT32(wLo, x);
			*xpwHi-- = MULSHIFT32(wHi, x);
		}
	}
}

/**************************************************************************************
 * Function:    FreqInvertRescale
 *
 * Description: do frequency inversion (odd samples of odd blocks) and rescale 
 *                if necessary (extra guard bits added before IMDCT)
 *
 * Inputs:      output vector y (18 new samples, spaced NBANDS apart)
 *              previous sample vector xPrev (9 samples)
 *              index of current block
 *              number of extra shifts added before IMDCT (usually 0)
 *
 * Outputs:     inverted and rescaled (as necessary) outputs
 *              rescaled (as necessary) previous samples
 *
 * Return:      updated mOut (from new outputs y)
 **************************************************************************************/
static __inline void FreqInvertOdd(int *y)
{
	int y0, y1, y2, y3, y4, y5, y6, y7, y8;

	y += NBANDS;
	y0 = *y;	y += 2*NBANDS;
	y1 = *y;	y += 2*NBANDS;
	y2 = *y;	y += 2*NBANDS;
	y3 = *y;	y += 2*NBANDS;
	y4 = *y;	y += 2*NBANDS;
	y5 = *y;	y += 2*NBANDS;
	y6 = *y;	y += 2*NBANDS;
	y7 = *y;	y += 2*NBANDS;
	y8 = *y;	y += 2*NBANDS;

	y -= 18*NBANDS;
	*y = -y0;	y += 2*NBANDS;
	*y = -y1;	y += 2*NBANDS;
	*y = -y2;	y += 2*NBANDS;
	*y = -y3;	y += 2*NBANDS;
	*y = -y4;	y += 2*NBANDS;
	*y = -y5;	y += 2*NBANDS;
	*y = -y6;	y += 2*NBANDS;
	*y = -y7;	y += 2*NBANDS;
	*y = -y8;
}

static int FreqInvertRescale(int *y, int *xPrev, int blockIdx, int es)
{
	int i, d, mOut, clipBits, shifted;

	/* undo pre-IMDCT scaling, clipping if necessary */
	mOut = 0;
	clipBits = 31 - es;
	if (blockIdx & 0x01) {
		/* frequency invert */
		for (i = 9; i > 0; i--) {
			d = *y;		CLIP_2N(d, clipBits);	shifted = d << es;	*y = shifted;	mOut |= FASTABS(shifted);	y += NBANDS;
			d = -*y;	CLIP_2N(d, clipBits);	shifted = d << es;	*y = shifted;	mOut |= FASTABS(shifted);	y += NBANDS;
			d = *xPrev;	CLIP_2N(d, clipBits);	*xPrev++ = d << es;
		}
	} else {
		for (i = 9; i > 0; i--) {
			d = *y;		CLIP_2N(d, clipBits);	shifted = d << es;	*y = shifted;	mOut |= FASTABS(shifted);	y += NBANDS;
			d = *y;		CLIP_2N(d, clipBits);	shifted = d << es;	*y = shifted;	mOut |= FASTABS(shifted);	y += NBANDS;
			d = *xPrev;	CLIP_2N(d, clipBits);	*xPrev++ = d << es;
		}
	}
	return mOut;
}

/* format = Q31
 * #define M_PI 3.14159265358979323846
 * double u = 2.0 * M_PI / 9.0;
 * float c0 = sqrt(3.0) / 2.0; 
 * float c1 = cos(u);          
 * float c2 = cos(2*u);        
 * float c3 = sin(u);          
 * float c4 = sin(2*u);
 */

static const int c9_0 = 0x6ed9eba1;
static const int c9_1 = 0x620dbe8b;
static const int c9_2 = 0x163a1a7e;
static const int c9_3 = 0x5246dd49;
static const int c9_4 = 0x7e0e2e32;

/* format = Q31
 * cos(((0:8) + 0.5) * (pi/18)) 
 */
static const int c18[9] = {
	0x7f834ed0, 0x7ba3751d, 0x7401e4c1, 0x68d9f964, 0x5a82799a, 0x496af3e2, 0x36185aee, 0x2120fb83, 0x0b27eb5c, 
};

/* require at least 3 guard bits in x[] to ensure no overflow */
static __inline void idct9(int *x)
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

	a8 = a6 - a5;		/* ie x[8] - x[4] */
	a9 = a3 - a7;		/* ie x[5] - x[7] */
	a10 = a2 - x7;		/* ie x[1] - x[5] - x[7] */
	a11 = a4 - x8;		/* ie x[2] - x[4] - x[8] */

	/* do the << 1 as constant shifts where mX is actually used (free, no stall or extra inst.) */
	m1 =  MULSHIFT32(c9_0, x3);
	m3 =  MULSHIFT32(c9_0, a10);
	m5 =  MULSHIFT32(c9_1, a5);
	m6 =  MULSHIFT32(c9_2, a6);
	m7 =  MULSHIFT32(c9_1, a8);
	m8 =  MULSHIFT32(c9_2, a5);
	m9 =  MULSHIFT32(c9_3, a9);
	m10 = MULSHIFT32(c9_4, a7);
	m11 = MULSHIFT32(c9_3, a3);
	m12 = MULSHIFT32(c9_4, a9);

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

/* let c(j) = cos(M_PI/36 * ((j)+0.5)), s(j) = sin(M_PI/36 * ((j)+0.5))
 * then fastWin[2*j+0] = c(j)*(s(j) + c(j)), j = [0, 8]
 *      fastWin[2*j+1] = c(j)*(s(j) - c(j))
 * format = Q30
 */
int fastWin36[18] = {
	0x42aace8b, 0xc2e92724, 0x47311c28, 0xc95f619a, 0x4a868feb, 0xd0859d8c,
	0x4c913b51, 0xd8243ea0, 0x4d413ccc, 0xe0000000, 0x4c913b51, 0xe7dbc161,
	0x4a868feb, 0xef7a6275, 0x47311c28, 0xf6a09e67, 0x42aace8b, 0xfd16d8dd,
};


#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_IMDCT) && defined(__GNUC__) && \
	(defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || \
	 defined(__mc68060__) || defined(mc68020))
#define IMDCT36_HAS_AMIGA_M68K_ASM 1
static __inline int IMDCT36_AMIGA_M68K_MULSHIFT32(int x, int y)
{
	int hi;
	int lo;

	lo = x;
	__asm__ volatile ("muls.l %2,%0:%1"
		: "=d" (hi), "+d" (lo)
		: "dmi" (y));
	(void)lo;
	return hi;
}
#else
#define IMDCT36_HAS_AMIGA_M68K_ASM 0
#endif

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
			mOut |= IMDCT36(xCurr, xPrev, &(y[0][i]), currWinIdx, prevWinIdx, i, bc->gbIn);
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
			mOut |= IMDCT36_C_REFERENCE(xCurr, xPrev, &(y[0][i]), currWinIdx, prevWinIdx, i, bc->gbIn);
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

	bc->gbOut = CLZ(mOut) - 1;

	return nBlocksOut;
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
	bc.prevType = mi->prevType[ch];
	bc.prevWinSwitch = mi->prevWinSwitch[ch];
	bc.currWinSwitch = (si->sis[gr][ch].mixedBlock ? blockCutoff : 0);	/* where WINDOW switches (not nec. transform) */
	bc.gbIn = hi->gb[ch];

	mi->numPrevIMDCT[ch] = HybridTransform(hi->huffDecBuf[ch], mi->overBuf[ch], mi->outBuf[ch], &si->sis[gr][ch], &bc);
	mi->prevType[ch] = si->sis[gr][ch].blockType;
	mi->prevWinSwitch[ch] = bc.currWinSwitch;		/* 0 means not a mixed block (either all short or all long) */
	mi->gb[ch] = bc.gbOut;

	ASSERT(mi->numPrevIMDCT[ch] <= NBANDS);

	/* output has gained 2 int bits */
	return 0;
}
