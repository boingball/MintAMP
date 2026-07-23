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
 * stproc.c - mid-side and intensity (MPEG1 and MPEG2) stereo processing
 **************************************************************************************/

#include "coder.h"
#include "assembly.h"

#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_INTENSITY) && defined(__GNUC__) && \
	(defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || \
	 defined(__mc68060__) || defined(mc68020))
#define INTENSITYSCALE_HAS_AMIGA_M68K_ASM 1
#else
#define INTENSITYSCALE_HAS_AMIGA_M68K_ASM 0
#endif

void IntensityScaleRun_C_REFERENCE(int *xL, int *xR, int fl, int fr,
	int count, int stride, int *mOutL, int *mOutR)
{
	int i, xl, xr, outL, outR;

	outL = *mOutL;
	outR = *mOutR;
	for (i = 0; i < count; i++) {
		xr = MULSHIFT32(fr, *xL) << 2;	xR[0] = xr;	outR |= FASTABS(xr);
		xl = MULSHIFT32(fl, *xL) << 2;	xL[0] = xl;	outL |= FASTABS(xl);
		xL += stride;
		xR += stride;
	}
	*mOutL = outL;
	*mOutR = outR;
}

#if INTENSITYSCALE_HAS_AMIGA_M68K_ASM
static __inline void IntensityScaleRun1_AmigaM68K(int *xL, int *xR, int fl, int fr,
	int count, int *mOutL, int *mOutR)
{
	int outL, outR;

	if (count <= 0)
		return;
	outL = *mOutL;
	outR = *mOutR;
	__asm__ volatile (
		"move.l %6,%%d7\n\t"
		"subq.l #1,%%d7\n\t"
		"moveq #31,%%d6\n\t"
		"move.l %2,%%d4\n\t"
		"move.l %3,%%d5\n"
		"1:\n\t"
		"move.l (%0),%%d0\n\t"
		"move.l %%d0,%%d1\n\t"
		"muls.l %4,%%d2:%%d0\n\t"
		"asl.l #2,%%d2\n\t"
		"move.l %%d2,(%1)+\n\t"
		"move.l %%d2,%%d3\n\t"
		"asr.l %%d6,%%d3\n\t"
		"eor.l %%d3,%%d2\n\t"
		"sub.l %%d3,%%d2\n\t"
		"or.l %%d2,%%d5\n\t"
		"muls.l %5,%%d2:%%d1\n\t"
		"asl.l #2,%%d2\n\t"
		"move.l %%d2,(%0)+\n\t"
		"move.l %%d2,%%d3\n\t"
		"asr.l %%d6,%%d3\n\t"
		"eor.l %%d3,%%d2\n\t"
		"sub.l %%d3,%%d2\n\t"
		"or.l %%d2,%%d4\n\t"
		"dbf %%d7,1b\n\t"
		"move.l %%d4,%2\n\t"
		"move.l %%d5,%3"
		: "+a" (xL), "+a" (xR), "+m" (outL), "+m" (outR)
		: "m" (fr), "m" (fl), "m" (count)
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "cc", "memory");
	*mOutL = outL;
	*mOutR = outR;
}

static __inline void IntensityScaleRun3_AmigaM68K(int *xL, int *xR, int fl, int fr,
	int count, int *mOutL, int *mOutR)
{
	int outL, outR;

	if (count <= 0)
		return;
	outL = *mOutL;
	outR = *mOutR;
	__asm__ volatile (
		"move.l %6,%%d7\n\t"
		"subq.l #1,%%d7\n\t"
		"moveq #31,%%d6\n\t"
		"move.l %2,%%d4\n\t"
		"move.l %3,%%d5\n"
		"1:\n\t"
		"move.l (%0),%%d0\n\t"
		"move.l %%d0,%%d1\n\t"
		"muls.l %4,%%d2:%%d0\n\t"
		"asl.l #2,%%d2\n\t"
		"move.l %%d2,(%1)\n\t"
		"lea 12(%1),%1\n\t"
		"move.l %%d2,%%d3\n\t"
		"asr.l %%d6,%%d3\n\t"
		"eor.l %%d3,%%d2\n\t"
		"sub.l %%d3,%%d2\n\t"
		"or.l %%d2,%%d5\n\t"
		"muls.l %5,%%d2:%%d1\n\t"
		"asl.l #2,%%d2\n\t"
		"move.l %%d2,(%0)\n\t"
		"lea 12(%0),%0\n\t"
		"move.l %%d2,%%d3\n\t"
		"asr.l %%d6,%%d3\n\t"
		"eor.l %%d3,%%d2\n\t"
		"sub.l %%d3,%%d2\n\t"
		"or.l %%d2,%%d4\n\t"
		"dbf %%d7,1b\n\t"
		"move.l %%d4,%2\n\t"
		"move.l %%d5,%3"
		: "+a" (xL), "+a" (xR), "+m" (outL), "+m" (outR)
		: "m" (fr), "m" (fl), "m" (count)
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "cc", "memory");
	*mOutL = outL;
	*mOutR = outR;
}
#endif

void IntensityScaleRun1_TEST_ACTIVE(int *xL, int *xR, int fl, int fr,
	int count, int *mOutL, int *mOutR)
{
#if INTENSITYSCALE_HAS_AMIGA_M68K_ASM
	IntensityScaleRun1_AmigaM68K(xL, xR, fl, fr, count, mOutL, mOutR);
#else
	IntensityScaleRun_C_REFERENCE(xL, xR, fl, fr, count, 1, mOutL, mOutR);
#endif
}

void IntensityScaleRun3_TEST_ACTIVE(int *xL, int *xR, int fl, int fr,
	int count, int *mOutL, int *mOutR)
{
#if INTENSITYSCALE_HAS_AMIGA_M68K_ASM
	IntensityScaleRun3_AmigaM68K(xL, xR, fl, fr, count, mOutL, mOutR);
#else
	IntensityScaleRun_C_REFERENCE(xL, xR, fl, fr, count, 3, mOutL, mOutR);
#endif
}

int IntensityScaleRun_HAS_AMIGA_M68K_ASM_RUNTIME(void)
{
	return INTENSITYSCALE_HAS_AMIGA_M68K_ASM;
}

/**************************************************************************************
 * Function:    MidSideProc
 *
 * Description: sum-difference stereo reconstruction
 *
 * Inputs:      vector x with dequantized samples from left and right channels
 *              number of non-zero samples (MAX of left and right)
 *              assume 1 guard bit in input
 *              guard bit mask (left and right channels)
 *
 * Outputs:     updated sample vector x
 *              updated guard bit mask
 *
 * Return:      none
 *
 * Notes:       assume at least 1 GB in input
 **************************************************************************************/
void MidSideProc(int x[MAX_NCHAN][MAX_NSAMP], int nSamps, int mOut[2])  
{
	/* L = (M+S)/sqrt(2), R = (M-S)/sqrt(2) 
	 * NOTE: 1/sqrt(2) done in DequantChannel() - see comments there
	 */
#if defined(AMIGA_M68K_ASM_MIDSIDE) && defined(__GNUC__) && \
	(defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || \
	 defined(__mc68060__) || defined(mc68020))
	int *left;
	int *right;
	int *out;

	if (nSamps <= 0)
		return;

	left = x[0];
	right = x[1];
	out = mOut;
	/*
	 * Keep both channel pointers, masks, and the sum/difference temporaries in
	 * registers for the complete hot loop.  d6 holds the register-form shift
	 * count because m68k immediate shifts cannot encode 31.  The resulting
	 * copy/asr/eor/sub sequence implements FASTABS without a data-dependent
	 * branch.  nSamps is at most 576, so dbf's 16-bit counter is sufficient.
	 */
	__asm__ volatile (
		"move.l %3,%%d7\n\t"
		"subq.l #1,%%d7\n\t"
		"moveq #31,%%d6\n\t"
		"move.l (%2),%%d4\n\t"
		"move.l 4(%2),%%d5\n"
		"1:\n\t"
		"move.l (%0),%%d0\n\t"
		"move.l (%1),%%d1\n\t"
		"move.l %%d0,%%d2\n\t"
		"add.l %%d1,%%d2\n\t"
		"sub.l %%d1,%%d0\n\t"
		"move.l %%d2,(%0)+\n\t"
		"move.l %%d0,(%1)+\n\t"
		"move.l %%d2,%%d3\n\t"
		"asr.l %%d6,%%d3\n\t"
		"eor.l %%d3,%%d2\n\t"
		"sub.l %%d3,%%d2\n\t"
		"or.l %%d2,%%d4\n\t"
		"move.l %%d0,%%d1\n\t"
		"asr.l %%d6,%%d1\n\t"
		"eor.l %%d1,%%d0\n\t"
		"sub.l %%d1,%%d0\n\t"
		"or.l %%d0,%%d5\n\t"
		"dbf %%d7,1b\n\t"
		"move.l %%d4,(%2)\n\t"
		"move.l %%d5,4(%2)"
		: "+&a" (left), "+&a" (right), "+&a" (out)
		: "a" (nSamps)
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "cc", "memory");
#else
	int i, xr, xl, mOutL, mOutR;

	mOutL = mOutR = 0;
	for(i = 0; i < nSamps; i++) {
		xl = x[0][i];
		xr = x[1][i];
		x[0][i] = xl + xr;
		x[1][i] = xl - xr;
		mOutL |= FASTABS(x[0][i]);
		mOutR |= FASTABS(x[1][i]);
	}
	mOut[0] |= mOutL;
	mOut[1] |= mOutR;
#endif
}

/**************************************************************************************
 * Function:    IntensityProcMPEG1
 *
 * Description: intensity stereo processing for MPEG1
 *
 * Inputs:      vector x with dequantized samples from left and right channels
 *              number of non-zero samples in left channel
 *              valid FrameHeader struct
 *              two each of ScaleFactorInfoSub, CriticalBandInfo structs (both channels)
 *              flags indicating midSide on/off, mixedBlock on/off
 *              guard bit mask (left and right channels)
 *
 * Outputs:     updated sample vector x
 *              updated guard bit mask
 *
 * Return:      none
 *
 * Notes:       assume at least 1 GB in input
 *
 * TODO:        combine MPEG1/2 into one function (maybe)
 *              make sure all the mixed-block and IIP logic is right
 **************************************************************************************/
void IntensityProcMPEG1(int x[MAX_NCHAN][MAX_NSAMP], int nSamps, FrameHeader *fh, ScaleFactorInfoSub *sfis, 
						CriticalBandInfo *cbi, int midSideFlag, int mixFlag, int mOut[2])
{
	int i=0, j=0, n=0, cb=0, w=0;
	int sampsLeft, isf, mOutL, mOutR, xl, xr;
	int fl, fr, fls[3], frs[3];
	int cbStartL=0, cbStartS=0, cbEndL=0, cbEndS=0;
	int *isfTab;

	(void)mixFlag;
	
	/* NOTE - this works fine for mixed blocks, as long as the switch point starts in the
	 *  short block section (i.e. on or after sample 36 = sfBand->l[8] = 3*sfBand->s[3]
	 * is this a safe assumption?
	 * TODO - intensity + mixed not quite right (diff = 11 on he_mode)
	 *  figure out correct implementation (spec ambiguous about when to do short block reorder)
	 */
	if (cbi[1].cbType == 0) {
		/* long block */
		cbStartL = cbi[1].cbEndL + 1;
		cbEndL =   cbi[0].cbEndL + 1;
		cbStartS = cbEndS = 0;
		i = fh->sfBand->l[cbStartL];
	} else if (cbi[1].cbType == 1 || cbi[1].cbType == 2) {
		/* short or mixed block */
		cbStartS = cbi[1].cbEndSMax + 1;
		cbEndS =   cbi[0].cbEndSMax + 1;
		cbStartL = cbEndL = 0;
		i = 3 * fh->sfBand->s[cbStartS];
	}

	sampsLeft = nSamps - i;		/* process to length of left */
	isfTab = (int *)ISFMpeg1[midSideFlag];
	mOutL = mOutR = 0;

	/* long blocks */
	for (cb = cbStartL; cb < cbEndL && sampsLeft > 0; cb++) {
		isf = sfis->l[cb];
		if (isf == 7) {
			fl = ISFIIP[midSideFlag][0];
			fr = ISFIIP[midSideFlag][1];
		} else {
			fl = isfTab[isf];	
			fr = isfTab[6] - isfTab[isf];
		}

		n = MIN(fh->sfBand->l[cb + 1] - fh->sfBand->l[cb], sampsLeft);
		IntensityScaleRun1_TEST_ACTIVE(&x[0][i], &x[1][i], fl, fr, n, &mOutL, &mOutR);
		i += n;
		sampsLeft -= n;
	}

	/* short blocks */
	for (cb = cbStartS; cb < cbEndS && sampsLeft >= 3; cb++) {
		for (w = 0; w < 3; w++) {
			isf = sfis->s[cb][w];
			if (isf == 7) {
				fls[w] = ISFIIP[midSideFlag][0];
				frs[w] = ISFIIP[midSideFlag][1];
			} else {
				fls[w] = isfTab[isf];
				frs[w] = isfTab[6] - isfTab[isf];
			}
		}

		n = fh->sfBand->s[cb + 1] - fh->sfBand->s[cb];
		for (j = 0; j < n && sampsLeft >= 3; j++, i+=3) {
			xr = MULSHIFT32(frs[0], x[0][i+0]) << 2;	x[1][i+0] = xr;	mOutR |= FASTABS(xr);
			xl = MULSHIFT32(fls[0], x[0][i+0]) << 2;	x[0][i+0] = xl;	mOutL |= FASTABS(xl);
			xr = MULSHIFT32(frs[1], x[0][i+1]) << 2;	x[1][i+1] = xr;	mOutR |= FASTABS(xr);
			xl = MULSHIFT32(fls[1], x[0][i+1]) << 2;	x[0][i+1] = xl;	mOutL |= FASTABS(xl);
			xr = MULSHIFT32(frs[2], x[0][i+2]) << 2;	x[1][i+2] = xr;	mOutR |= FASTABS(xr);
			xl = MULSHIFT32(fls[2], x[0][i+2]) << 2;	x[0][i+2] = xl;	mOutL |= FASTABS(xl);
			sampsLeft -= 3;
		}
	}
	mOut[0] = mOutL;
	mOut[1] = mOutR;
	
	return;
}

/**************************************************************************************
 * Function:    IntensityProcMPEG2
 *
 * Description: intensity stereo processing for MPEG2
 *
 * Inputs:      vector x with dequantized samples from left and right channels
 *              number of non-zero samples in left channel
 *              valid FrameHeader struct
 *              two each of ScaleFactorInfoSub, CriticalBandInfo structs (both channels)
 *              ScaleFactorJS struct with joint stereo info from UnpackSFMPEG2()
 *              flags indicating midSide on/off, mixedBlock on/off
 *              guard bit mask (left and right channels)
 *
 * Outputs:     updated sample vector x
 *              updated guard bit mask
 *
 * Return:      none
 *
 * Notes:       assume at least 1 GB in input
 *
 * TODO:        combine MPEG1/2 into one function (maybe)
 *              make sure all the mixed-block and IIP logic is right
 *                probably redo IIP logic to be simpler
 **************************************************************************************/
void IntensityProcMPEG2(int x[MAX_NCHAN][MAX_NSAMP], int nSamps, FrameHeader *fh, ScaleFactorInfoSub *sfis, 
						CriticalBandInfo *cbi, ScaleFactorJS *sfjs, int midSideFlag, int mixFlag, int mOut[2])
{
	int i, j, k, n, r, cb, w;
	int fl, fr, mOutL, mOutR;
	int sampsLeft;
	int isf, sfIdx, tmp, il[23];
	int *isfTab;
	int cbStartL, cbStartS, cbEndL, cbEndS;

	(void)mixFlag;
	
	isfTab = (int *)ISFMpeg2[sfjs->intensityScale][midSideFlag];
	mOutL = mOutR = 0;

	/* fill buffer with illegal intensity positions (depending on slen) */
	for (k = r = 0; r < 4; r++) {
		tmp = (1 << sfjs->slen[r]) - 1;
		for (j = 0; j < sfjs->nr[r]; j++, k++) 
			il[k] = tmp;
	}

	if (cbi[1].cbType == 0) {
		/* long blocks */
		il[21] = il[22] = 1;
		cbStartL = cbi[1].cbEndL + 1;	/* start at end of right */
		cbEndL =   cbi[0].cbEndL + 1;	/* process to end of left */
		i = fh->sfBand->l[cbStartL];
		sampsLeft = nSamps - i;

		for(cb = cbStartL; cb < cbEndL; cb++) {
			sfIdx = sfis->l[cb];
			if (sfIdx == il[cb]) {
				fl = ISFIIP[midSideFlag][0];
				fr = ISFIIP[midSideFlag][1];
			} else {
				isf = (sfis->l[cb] + 1) >> 1;
				fl = isfTab[(sfIdx & 0x01 ? isf : 0)];
				fr = isfTab[(sfIdx & 0x01 ? 0 : isf)];
			}
			n = MIN(fh->sfBand->l[cb + 1] - fh->sfBand->l[cb], sampsLeft);
			IntensityScaleRun1_TEST_ACTIVE(&x[0][i], &x[1][i], fl, fr, n, &mOutL, &mOutR);
			i += n;

			/* early exit once we've used all the non-zero samples */
			sampsLeft -= n;
			if (sampsLeft == 0)		
				break;
		}
	} else {
		/* short or mixed blocks */
		il[12] = 1;

		for(w = 0; w < 3; w++) {
			cbStartS = cbi[1].cbEndS[w] + 1;		/* start at end of right */
			cbEndS =   cbi[0].cbEndS[w] + 1;		/* process to end of left */
			i = 3 * fh->sfBand->s[cbStartS] + w;

			/* skip through sample array by 3, so early-exit logic would be more tricky */
			for(cb = cbStartS; cb < cbEndS; cb++) {
				sfIdx = sfis->s[cb][w];
				if (sfIdx == il[cb]) {
					fl = ISFIIP[midSideFlag][0];
					fr = ISFIIP[midSideFlag][1];
				} else {
					isf = (sfis->s[cb][w] + 1) >> 1;
					fl = isfTab[(sfIdx & 0x01 ? isf : 0)];
					fr = isfTab[(sfIdx & 0x01 ? 0 : isf)];
				}
				n = fh->sfBand->s[cb + 1] - fh->sfBand->s[cb];

				IntensityScaleRun3_TEST_ACTIVE(&x[0][i], &x[1][i], fl, fr, n, &mOutL, &mOutR);
				i += 3 * n;
			}
		}
	}
	mOut[0] = mOutL;
	mOut[1] = mOutR;

	return;
}
