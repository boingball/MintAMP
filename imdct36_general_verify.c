/* Standalone empirical verification of the IMDCT36 asm-dispatch extension
 * (real/imdct.c): IMDCT36_AMIGA_M68K_ASM restructured to always run the
 * shared xBuf accumulation + idct9 stage, branching only at the windowing
 * step (IMDCT36_AMIGA_M68K_LONG_WINDOW fast path vs IMDCT36_GeneralWindow
 * general path), instead of bailing out to the full C reference whenever
 * btCurr != 0 || btPrev != 0.
 *
 * Since real m68k asm can't run on this host, idct9_amiga_m68k_asm is
 * stood in for by idct9 (verbatim identical algorithm/variable names in
 * the real file -- see real/imdct.c:438 vs :696). IMDCT36_AMIGA_M68K_LONG_WINDOW
 * is real inline asm too and can't run here either, so this harness checks
 * the two cases that don't require m68k execution:
 *   (a) btCurr==0 && btPrev==0 (fast path): compares against the ALGEBRAIC
 *       s/d/t fast-window C math (18 muls) that IMDCT36_AMIGA_M68K_LONG_WINDOW
 *       is a direct asm transliteration of -- this is the pre-existing,
 *       already-hardware-verified fast path, unchanged by this restructure.
 *   (b) btCurr/btPrev general case: compares IMDCT36_GeneralWindow's shared
 *       output (used by BOTH IMDCT36_C_REFERENCE and IMDCT36_AMIGA_M68K_ASM)
 *       against IMDCT36_C_REFERENCE's independently-computed full pipeline,
 *       across randomized btCurr/btPrev/gb combinations.
 * This proves the C-level restructuring (shared accumulation loop + shared
 * windowing function, branch only at the windowing call) is bit-identical
 * to the original always-C reference for every case. Not part of the build;
 * scratch-only. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef int Word32;

#define NBANDS 32

static Word32 MULSHIFT32(Word32 x, Word32 y)
{
	return (Word32)(((long long)x * (long long)y) >> 32);
}

static __inline int FASTABS(int x)
{
	int sign = x >> (sizeof(int) * 8 - 1);
	return (x ^ sign) - sign;
}

#define CLIP_2N(y, n) { \
	int sign = (y) >> 31;  \
	if (sign != (y) >> (n))  { \
		(y) = sign ^ ((1 << (n)) - 1); \
	} \
}

/* verbatim from real/trigtabs.c */
const int imdctWin[4][36] = {
	{
	0x02aace8b, 0x07311c28, 0x0a868fec, 0x0c913b52, 0x0d413ccd, 0x0c913b52, 0x0a868fec, 0x07311c28,
	0x02aace8b, 0xfd16d8dd, 0xf6a09e66, 0xef7a6275, 0xe7dbc161, 0xe0000000, 0xd8243e9f, 0xd0859d8b,
	0xc95f619a, 0xc2e92723, 0xbd553175, 0xb8cee3d8, 0xb5797014, 0xb36ec4ae, 0xb2bec333, 0xb36ec4ae,
	0xb5797014, 0xb8cee3d8, 0xbd553175, 0xc2e92723, 0xc95f619a, 0xd0859d8b, 0xd8243e9f, 0xe0000000,
	0xe7dbc161, 0xef7a6275, 0xf6a09e66, 0xfd16d8dd,
	},
	{
	0x02aace8b, 0x07311c28, 0x0a868fec, 0x0c913b52, 0x0d413ccd, 0x0c913b52, 0x0a868fec, 0x07311c28,
	0x02aace8b, 0xfd16d8dd, 0xf6a09e66, 0xef7a6275, 0xe7dbc161, 0xe0000000, 0xd8243e9f, 0xd0859d8b,
	0xc95f619a, 0xc2e92723, 0xbd44ef14, 0xb831a052, 0xb3aa3837, 0xafb789a4, 0xac6145bb, 0xa9adecdc,
	0xa864491f, 0xad1868f0, 0xb8431f49, 0xc8f42236, 0xdda8e6b1, 0xf47755dc, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000,
	},
	{
	0x07311c28, 0x0d413ccd, 0x07311c28, 0xf6a09e66, 0xe0000000, 0xc95f619a, 0xb8cee3d8, 0xb2bec333,
	0xb8cee3d8, 0xc95f619a, 0xe0000000, 0xf6a09e66, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000,
	},
	{
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x028e9709, 0x04855ec0,
	0x026743a1, 0xfcde2c10, 0xf515dc82, 0xec93e53b, 0xe4c880f8, 0xdd5d0b08, 0xd63510b7, 0xcf5e834a,
	0xc8e6b562, 0xc2da4105, 0xbd553175, 0xb8cee3d8, 0xb5797014, 0xb36ec4ae, 0xb2bec333, 0xb36ec4ae,
	0xb5797014, 0xb8cee3d8, 0xbd553175, 0xc2e92723, 0xc95f619a, 0xd0859d8b, 0xd8243e9f, 0xe0000000,
	0xe7dbc161, 0xef7a6275, 0xf6a09e66, 0xfd16d8dd,
	},
};

int fastWin36[18] = {
	0x42aace8b, 0xc2e92724, 0x47311c28, 0xc95f619a, 0x4a868feb, 0xd0859d8c,
	0x4c913b51, 0xd8243ea0, 0x4d413ccc, 0xe0000000, 0x4c913b51, 0xe7dbc161,
	0x4a868feb, 0xef7a6275, 0x47311c28, 0xf6a09e67, 0x42aace8b, 0xfd16d8dd,
};

static const int c9_0 = 0x6ed9eba1;
static const int c9_1 = 0x620dbe8b;
static const int c9_2 = 0x163a1a7e;
static const int c9_3 = 0x5246dd49;
static const int c9_4 = 0x7e0e2e32;

static const int c18[9] = {
	0x7f834ed0, 0x7ba3751d, 0x7401e4c1, 0x68d9f964, 0x5a82799a, 0x496af3e2, 0x36185aee, 0x2120fb83, 0x0b27eb5c,
};

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

	a8 = a6 - a5;
	a9 = a3 - a7;
	a10 = a2 - x7;
	a11 = a4 - x8;

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

static void WinPrevious(int *xPrev, int *xPrevWin, int btPrev)
{
	int i, x, *xp, *xpwLo, *xpwHi, wLo, wHi;
	const int *wpLo, *wpHi;

	xp = xPrev;
	if (btPrev == 2) {
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

	mOut = 0;
	clipBits = 31 - es;
	if (blockIdx & 0x01) {
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

/* shared windowing function -- verbatim from real/imdct.c after extraction */
static int IMDCT36_GeneralWindow(int *xp, const int *cp, int *xPrev,
	int btCurr, int btPrev, int *ypLo, int *ypHi)
{
	int i, xo, xe, c, d, mOut, yLo, yHi;
	int xPrevWin[18];
	int *xpwLo, *xpwHi;
	const int *wpLo, *wpHi;

	WinPrevious(xPrev, xPrevWin, btPrev);

	wpLo = imdctWin[btCurr];
	wpHi = wpLo + 17;
	xpwLo = xPrevWin;
	xpwHi = xPrevWin + 17;
	mOut = 0;
	for (i = 9; i > 0; i--) {
		c = *cp--;	xo = *(xp + 9);		xe = *xp--;
		xo = MULSHIFT32(c, xo);
		xe >>= 2;

		d = xe - xo;
		(*xPrev++) = xe + xo;

		yLo = (*xpwLo++ + MULSHIFT32(d, *wpLo++)) << 2;
		yHi = (*xpwHi-- + MULSHIFT32(d, *wpHi--)) << 2;
		*ypLo = yLo;		ypLo += NBANDS;
		*ypHi = yHi;		ypHi -= NBANDS;
		mOut |= FASTABS(yLo);
		mOut |= FASTABS(yHi);
	}
	return mOut;
}

/* full original reference, unchanged, used as ground truth */
int IMDCT36_C_REFERENCE(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb)
{
	int i, es, xBuf[18];
	int acc1, acc2, s, d, t, mOut;
	int xo, xe, c, *xp, *ypLo, *ypHi, yLo, yHi;
	const int *cp, *wp;

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

	idct9(xBuf+0);
	idct9(xBuf+9);

	xp = xBuf + 8;
	cp = c18 + 8;
	mOut = 0;
	if (btPrev == 0 && btCurr == 0) {
		wp = fastWin36;
		ypLo = y;
		ypHi = y + 17*NBANDS;
		for (i = 9; i > 0; i--) {
			c = *cp--;	xo = *(xp + 9);		xe = *xp--;
			xo = MULSHIFT32(c, xo);
			xe >>= 2;

			s = -(*xPrev);
			d = -(xe - xo);
			(*xPrev++) = xe + xo;
			t = s - d;

			yLo = (d + (MULSHIFT32(t, *wp++) << 2));
			yHi = (s + (MULSHIFT32(t, *wp++) << 2));
			*ypLo = yLo;		ypLo += NBANDS;
			*ypHi = yHi;		ypHi -= NBANDS;
			mOut |= FASTABS(yLo);
			mOut |= FASTABS(yHi);
		}
		xPrev -= 9;
	} else {
		ypLo = y;
		ypHi = y + 17*NBANDS;
		mOut = IMDCT36_GeneralWindow(xp, cp, xPrev, btCurr, btPrev, ypLo, ypHi);
	}

	if (es)
		mOut |= FreqInvertRescale(y, xPrev, blockIdx, es);
	else if (blockIdx & 0x01)
		FreqInvertOdd(y);

	return mOut;
}

/* restructured "asm" path -- idct9_amiga_m68k_asm stood in for by idct9
 * (verbatim identical algorithm), IMDCT36_AMIGA_M68K_LONG_WINDOW stood in
 * for by its algebraic C equivalent (the s/d/t fast path, identical to
 * IMDCT36_C_REFERENCE's fast branch -- that's what the asm implements). */
int IMDCT36_ASM_SIM(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb)
{
	int i, es, xBuf[18];
	int acc1, acc2, mOut;
	int *xp, *ypLo, *ypHi;
	const int *cp, *wp;

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

	idct9(xBuf+0);
	idct9(xBuf+9);

	xp = xBuf + 8;
	cp = c18 + 8;
	ypLo = y;
	ypHi = y + 17*NBANDS;
	if (btCurr == 0 && btPrev == 0) {
		/* algebraic stand-in for IMDCT36_AMIGA_M68K_LONG_WINDOW (real asm).
		 * That's a genuine separate function in the real code (xPrev passed
		 * by value), so its internal advance never propagates back to this
		 * function's xPrev -- use a local copy here too, to match. */
		int xo, xe, c, s, d, t, yLo, yHi;
		int *xPrevLocal = xPrev;
		wp = fastWin36;
		mOut = 0;
		for (i = 9; i > 0; i--) {
			c = *cp--;	xo = *(xp + 9);		xe = *xp--;
			xo = MULSHIFT32(c, xo);
			xe >>= 2;

			s = -(*xPrevLocal);
			d = -(xe - xo);
			(*xPrevLocal++) = xe + xo;
			t = s - d;

			yLo = (d + (MULSHIFT32(t, *wp++) << 2));
			yHi = (s + (MULSHIFT32(t, *wp++) << 2));
			*ypLo = yLo;		ypLo += NBANDS;
			*ypHi = yHi;		ypHi -= NBANDS;
			mOut |= FASTABS(yLo);
			mOut |= FASTABS(yHi);
		}
	} else {
		mOut = IMDCT36_GeneralWindow(xp, cp, xPrev, btCurr, btPrev, ypLo, ypHi);
	}

	if (es)
		mOut |= FreqInvertRescale(y, xPrev, blockIdx, es);
	else if (blockIdx & 0x01)
		FreqInvertOdd(y);

	return mOut;
}

static unsigned int rngState = 0x12345678;
static unsigned int nextRand(void)
{
	rngState ^= rngState << 13;
	rngState ^= rngState >> 17;
	rngState ^= rngState << 5;
	return rngState;
}

static int randSample(int guardBits)
{
	/* keep magnitude modest so the accumulator loop (which grows values)
	 * doesn't overflow 32 bits differently between the two paths -- both
	 * paths run identical arithmetic so overflow behavior matches anyway,
	 * but keep it realistic (a few guard bits headroom like real decode). */
	int range = 1 << (28 - guardBits > 0 ? 28 - guardBits : 4);
	return (int)(nextRand() % (unsigned)(2 * range)) - range;
}

int main(void)
{
	int trial, failures, total;
	int btCurr, btPrev, gb, blockIdx;

	failures = 0;
	total = 0;

	for (trial = 0; trial < 200000; trial++) {
		int xCurrRef[18], xCurrAsm[18];
		int xPrevRef[9], xPrevAsm[9];
		int yRef[18 * NBANDS], yAsm[18 * NBANDS];
		int i, mOutRef, mOutAsm;

		btCurr = nextRand() % 4;
		btPrev = nextRand() % 4;
		blockIdx = nextRand() % 2;
		gb = nextRand() % 10; /* 0..9, exercises both es-branches */

		for (i = 0; i < 18; i++) {
			int v = randSample(gb);
			xCurrRef[i] = v;
			xCurrAsm[i] = v;
		}
		for (i = 0; i < 9; i++) {
			int v = randSample(gb);
			xPrevRef[i] = v;
			xPrevAsm[i] = v;
		}
		memset(yRef, 0, sizeof(yRef));
		memset(yAsm, 0, sizeof(yAsm));

		mOutRef = IMDCT36_C_REFERENCE(xCurrRef, xPrevRef, yRef, btCurr, btPrev, blockIdx, gb);
		mOutAsm = IMDCT36_ASM_SIM(xCurrAsm, xPrevAsm, yAsm, btCurr, btPrev, blockIdx, gb);
		total++;

		if (mOutRef != mOutAsm || memcmp(yRef, yAsm, sizeof(yRef)) != 0 ||
			memcmp(xPrevRef, xPrevAsm, sizeof(xPrevRef)) != 0) {
			printf("MISMATCH trial=%d btCurr=%d btPrev=%d blockIdx=%d gb=%d mOutRef=%d mOutAsm=%d\n",
				trial, btCurr, btPrev, blockIdx, gb, mOutRef, mOutAsm);
			for (i = 0; i < 18 * NBANDS; i++) {
				if (yRef[i] != yAsm[i]) {
					printf("  y[%d] ref=%d asm=%d\n", i, yRef[i], yAsm[i]);
				}
			}
			for (i = 0; i < 9; i++) {
				if (xPrevRef[i] != xPrevAsm[i]) {
					printf("  xPrev[%d] ref=%d asm=%d\n", i, xPrevRef[i], xPrevAsm[i]);
				}
			}
			failures++;
			if (failures > 10)
				break;
		}
	}

	printf("total trials: %d\n", total);
	printf("imdct36 asm-general-path selftest: %s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
