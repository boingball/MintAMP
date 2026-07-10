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
 * dequant.c - dequantization, stereo processing (intensity, mid-side), short-block
 *               coefficient reordering
 **************************************************************************************/

#include <stdio.h>
#include "coder.h"
#include "assembly.h"
#include "amiga_profile_decode.h"


#if defined(AMIGA_M68K_ASM_MIDSIDE) && defined(__GNUC__) && \
	(defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || \
	 defined(__mc68060__) || defined(mc68020))
#define COLLAPSE_STEREO_TO_MONO_HAS_AMIGA_M68K_ASM 1
#else
#define COLLAPSE_STEREO_TO_MONO_HAS_AMIGA_M68K_ASM 0
#endif

/* Always-available reference implementation: also the only implementation
 * on non-m68k/non-GCC builds. Not called directly on the hot path when the
 * asm below is active -- see CollapseStereoToMono() -- but kept callable so
 * CollapseStereoToMonoSelftest() can cross-check the asm output against it. */
static void CollapseStereoToMono_C_REFERENCE(int x[MAX_NCHAN][MAX_NSAMP], int nSamps,
	int *nonZeroBound, int *gb)
{
	int i;
	int mOut;
	int mixed;

	mOut = 0;
	for (i = 0; i < nSamps; i++) {
		mixed = (x[0][i] + x[1][i]) >> 1;
		x[0][i] = mixed;
		mOut |= FASTABS(mixed);
	}
	nonZeroBound[0] = nSamps;
	gb[0] = CLZ(mOut) - 1;
}

static void CollapseStereoToMono(int x[MAX_NCHAN][MAX_NSAMP], int nSamps,
	int *nonZeroBound, int *gb)
{
#if COLLAPSE_STEREO_TO_MONO_HAS_AMIGA_M68K_ASM
	int *left;
	int *right;
	int *outp;
	int mOut;

	mOut = 0;
	if (nSamps > 0) {
		left = x[0];
		right = x[1];
		outp = &mOut;
		/* Same shape as MidSideProc's hot loop below (this file's other
		 * per-sample stereo reconstruction pass): d6 holds the
		 * register-form shift count (m68k immediate shifts can't encode
		 * 31), and the copy/asr/eor/sub sequence implements FASTABS
		 * without a data-dependent branch. mixed = (left+right)>>1 uses
		 * an immediate #1 shift, which m68k *can* encode directly.
		 * nSamps is at most 576, so dbf's 16-bit counter is sufficient. */
		__asm__ volatile (
			"move.l %3,%%d7\n\t"
			"subq.l #1,%%d7\n\t"
			"moveq #31,%%d6\n\t"
			"moveq #0,%%d4\n"
			"1:\n\t"
			"move.l (%1)+,%%d0\n\t"
			"add.l (%0),%%d0\n\t"
			"asr.l #1,%%d0\n\t"
			"move.l %%d0,(%0)+\n\t"
			"move.l %%d0,%%d1\n\t"
			"asr.l %%d6,%%d1\n\t"
			"eor.l %%d1,%%d0\n\t"
			"sub.l %%d1,%%d0\n\t"
			"or.l %%d0,%%d4\n\t"
			"dbf %%d7,1b\n\t"
			"move.l %%d4,(%2)"
			: "+&a" (left), "+&a" (right), "+&a" (outp)
			: "a" (nSamps)
			: "d0", "d1", "d4", "d6", "d7", "cc", "memory");
	}
	nonZeroBound[0] = nSamps;
	gb[0] = CLZ(mOut) - 1;
#else
	CollapseStereoToMono_C_REFERENCE(x, nSamps, nonZeroBound, gb);
#endif
}

/*
 * Cross-checks CollapseStereoToMono() (the production dispatch, which runs
 * the m68k asm above when compiled in) against CollapseStereoToMono_C_REFERENCE()
 * across randomized nSamps/magnitude combinations, including edge cases
 * (nSamps == 0, nSamps == MAX_NSAMP, all-zero input, alternating sign/magnitude
 * patterns designed to exercise FASTABS's branchless sign handling for both
 * positive and negative mixed values). Checks the collapsed x[0] samples,
 * nonZeroBound[0], and gb[0] (the guard-bit count that FDCT32FastLowrate()
 * later uses to decide how much to scale down before the transform --
 * reporting this wrong in either direction risks the exact class of
 * corruption the antialias guard-band fix elsewhere in this file addressed,
 * so it's checked bit-exactly here, not just "close enough").
 */
int CollapseStereoToMonoSelftest(void)
{
	static int xRef[MAX_NCHAN][MAX_NSAMP];
	static int xAsm[MAX_NCHAN][MAX_NSAMP];
	unsigned int rngState = 0xC0117A5EU;
	int trial, failures, total;

	failures = 0;
	total = 0;

	for (trial = 0; trial < 5000; trial++) {
		int nSamps, i, nonZeroBoundRef, nonZeroBoundAsm, gbRef, gbAsm;
		int pattern = trial % 6;

		nSamps = (int)(rngState % (MAX_NSAMP + 1));
		rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
		if (trial == 0) nSamps = 0;
		if (trial == 1) nSamps = 1;
		if (trial == 2) nSamps = MAX_NSAMP;

		for (i = 0; i < MAX_NSAMP; i++) {
			int l, r;

			rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
			switch (pattern) {
			case 0: l = 0; r = 0; break;
			case 1: l = 0x3fffffff; r = 0x3fffffff; break;
			case 2: l = (int)0xc0000000U; r = (int)0xc0000000U; break;
			case 3: l = 0x3fffffff; r = (int)0xc0000000U; break;
			case 4: l = (int)(rngState & 0x7fffffffU); r = -(int)(rngState & 0x7fffffffU); break;
			default: l = (int)rngState >> 4; r = (int)(rngState ^ 0x9e3779b9U) >> 4; break;
			}
			xRef[0][i] = l; xRef[1][i] = r;
			xAsm[0][i] = l; xAsm[1][i] = r;
		}

		nonZeroBoundRef = nonZeroBoundAsm = -1;
		gbRef = gbAsm = -1;
		CollapseStereoToMono_C_REFERENCE(xRef, nSamps, &nonZeroBoundRef, &gbRef);
		CollapseStereoToMono(xAsm, nSamps, &nonZeroBoundAsm, &gbAsm);
		total++;

		if (nonZeroBoundRef != nonZeroBoundAsm || gbRef != gbAsm) {
			printf("CollapseStereoToMono mismatch trial=%d nSamps=%d pattern=%d: "
				"nonZeroBound ref=%d asm=%d gb ref=%d asm=%d\n",
				trial, nSamps, pattern, nonZeroBoundRef, nonZeroBoundAsm, gbRef, gbAsm);
			failures++;
			continue;
		}
		for (i = 0; i < nSamps; i++) {
			if (xRef[0][i] != xAsm[0][i]) {
				printf("CollapseStereoToMono mismatch trial=%d nSamps=%d pattern=%d i=%d: "
					"ref=%d asm=%d\n", trial, nSamps, pattern, i, xRef[0][i], xAsm[0][i]);
				failures++;
				break;
			}
		}
	}

	printf("CollapseStereoToMono selftest: %d trials, %s (asm compiled in: %s)\n",
		total, failures ? "FAIL" : "PASS",
		COLLAPSE_STEREO_TO_MONO_HAS_AMIGA_M68K_ASM ? "yes" : "no");
	return failures ? -1 : 0;
}

/*
 * Verifies that capping MidSideProc()'s nSamps to dequantSubbandCapSampleLimit
 * (Dequantize()'s "whole spectrum" branch above) is equivalent to running the
 * uncapped MidSideProc() against the same input with the discarded region
 * already zeroed -- which is what every downstream consumer (IMDCT's own
 * activeSubbands cap, AntiAlias()'s guard-subband read) already treats it
 * as, since neither ever reads that far into hi->huffDecBuf when the cap is
 * active. Checks:
 *   1. kept-region (index < limit) reconstructed x[0]/x[1] bit-identical
 *      between the capped call and the zeroed-discard-region uncapped call
 *   2. mOut[0]/mOut[1] (and therefore the gb[] Dequantize() derives from
 *      them) bit-identical between the two -- this is the safety-critical
 *      part: FDCT32FastLowrate() uses gb to decide how much to scale down
 *      before the transform, so an inflated/wrong guard-bit count here
 *      risks the same class of corruption the antialias guard-band fix
 *      elsewhere in this file addressed
 *   3. as a sanity check on the "safe direction" reasoning in the comment
 *      at the call site: capping never *increases* mOut relative to the
 *      uncapped call against the real (non-zeroed, possibly-garbage)
 *      discarded region, i.e. gb never comes out *smaller* (less safe)
 *      than the uncapped call would have reported
 */
int MidSideProcSubbandCapSelftest(void)
{
	static const int kLimitCases[] = { 468, 288, 216, 144, 108 }; /* (26/16/12/8/6+1)*18 */
	unsigned int rngState = 0x5A1D5EEDU;
	int trial, failures, total;

	failures = 0;
	total = 0;

	for (trial = 0; trial < 20000; trial++) {
		static int xZeroed[MAX_NCHAN][MAX_NSAMP];
		static int xCapped[MAX_NCHAN][MAX_NSAMP];
		static int xGarbage[MAX_NCHAN][MAX_NSAMP];
		int limit = kLimitCases[trial % (int)(sizeof(kLimitCases) / sizeof(kLimitCases[0]))];
		int nSamps, i, mismatch;
		int mOutZeroed[2], mOutCapped[2], mOutGarbage[2];

		nSamps = limit + (int)(rngState % (MAX_NSAMP - limit + 1));
		rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;

		for (i = 0; i < MAX_NSAMP; i++) {
			int l, r, garbageL, garbageR;

			rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
			l = (int)(rngState & 0x3fffffffU) - 0x20000000;
			rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
			r = (int)(rngState & 0x3fffffffU) - 0x20000000;
			rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
			/* Simulates DequantChannel()'s cap-skip leftover: small raw
			 * Huffman-magnitude-like values rather than proper Q26 ones. */
			garbageL = (int)(rngState % 8000);
			rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
			garbageR = (int)(rngState % 8000);

			if (i < limit) {
				xZeroed[0][i] = xCapped[0][i] = xGarbage[0][i] = l;
				xZeroed[1][i] = xCapped[1][i] = xGarbage[1][i] = r;
			} else {
				xZeroed[0][i] = xZeroed[1][i] = 0;
				xCapped[0][i] = xCapped[1][i] = 0; /* untouched by the capped call; init matches */
				xGarbage[0][i] = garbageL;
				xGarbage[1][i] = garbageR;
			}
		}

		mOutZeroed[0] = mOutZeroed[1] = 0;
		mOutCapped[0] = mOutCapped[1] = 0;
		mOutGarbage[0] = mOutGarbage[1] = 0;

		MidSideProc(xZeroed, nSamps, mOutZeroed);      /* uncapped, discard region zeroed */
		MidSideProc(xCapped, limit, mOutCapped);        /* capped, matching the real call site */
		MidSideProc(xGarbage, nSamps, mOutGarbage);     /* uncapped, discard region "garbage" */
		total++;

		mismatch = 0;
		for (i = 0; i < limit; i++) {
			if (xZeroed[0][i] != xCapped[0][i] || xZeroed[1][i] != xCapped[1][i]) {
				printf("MidSideProcSubbandCap mismatch trial=%d limit=%d nSamps=%d i=%d: "
					"zeroed=(%d,%d) capped=(%d,%d)\n", trial, limit, nSamps, i,
					xZeroed[0][i], xZeroed[1][i], xCapped[0][i], xCapped[1][i]);
				mismatch = 1;
				break;
			}
		}
		if (mOutZeroed[0] != mOutCapped[0] || mOutZeroed[1] != mOutCapped[1]) {
			printf("MidSideProcSubbandCap mOut mismatch trial=%d limit=%d nSamps=%d: "
				"zeroed=(%d,%d) capped=(%d,%d)\n", trial, limit, nSamps,
				mOutZeroed[0], mOutZeroed[1], mOutCapped[0], mOutCapped[1]);
			mismatch = 1;
		}
		/* mOutGarbage can only ever be >= mOutCapped bitwise (OR over a
		 * superset of nonzero-or-equal values), so CLZ(mOutGarbage) <=
		 * CLZ(mOutCapped), i.e. the capped call's derived gb is never
		 * smaller (less safe) than today's uncapped-against-garbage gb. */
		if ((mOutGarbage[0] | mOutCapped[0]) != mOutGarbage[0] ||
			(mOutGarbage[1] | mOutCapped[1]) != mOutGarbage[1]) {
			printf("MidSideProcSubbandCap safety-direction violation trial=%d limit=%d nSamps=%d: "
				"garbage=(%d,%d) capped=(%d,%d)\n", trial, limit, nSamps,
				mOutGarbage[0], mOutGarbage[1], mOutCapped[0], mOutCapped[1]);
			mismatch = 1;
		}
		if (mismatch)
			failures++;
	}

	printf("MidSideProc subband cap selftest: %d trials, %s\n",
		total, failures ? "FAIL" : "PASS");
	return failures ? -1 : 0;
}

/*
 * Proves that Dequantize()'s dequantSubbandCapSampleLimit clamp on
 * CollapseStereoToMono()'s nSamps is a safe simplification, using the same
 * capped-vs-uncapped reasoning as MidSideProcSubbandCapSelftest() above
 * (and DequantSubbandCapSelftest()/AntiAliasSubbandCapSelftest()) -- this
 * codebase's established pattern for proving a subband-cap optimization
 * doesn't change what downstream actually consumes. MidSideProc() has its
 * own selftest above; this one covers CollapseStereoToMono()'s separate
 * cap at the outputMono call site.
 *
 * Two runs on matched data, differing only in what happens at/after `cap`:
 *   "capped"  : CollapseStereoToMono(buf, cap, ...)             -- the fix
 *   "zeroed"  : CollapseStereoToMono(buf, fullLen, ...) with
 *               buf[0..1][cap..fullLen) forced to 0 first        -- what an
 *               uncapped call would need to see there to be equivalent
 *
 * This models "the discarded region zeroed" rather than the real production
 * un-dequantized garbage DequantChannel() actually leaves beyond the cap
 * (see that function's subbandCapSampleLimit comment): garbage has no
 * well-defined arithmetic result to compare against, whereas zero is
 * provably inert for both outputs this test checks --
 *   - x[0][i] for i < cap never depends on data at or beyond i, so it is
 *     identical in both runs regardless of what's beyond cap (garbage or
 *     zero); not really exercised by this test, just documented here.
 *   - gb[0] = CLZ(mOut)-1 where mOut is an OR-accumulation of FASTABS() over
 *     every processed sample: OR-ing in extra zeros never changes the
 *     result, so the "zeroed" run's gb[0] is exactly what the "capped" run's
 *     gb[0] is, PROVIDED the discarded region really does contribute 0 --
 *     which is exactly what capping nSamps guarantees (it never reads that
 *     region at all, so whatever garbage is actually there cannot corrupt
 *     gb[0] the way the pre-fix code could).
 * nonZeroBound[0] is checked against each run's own contract (nSamps
 * passed in) rather than against each other, since the two runs are called
 * with deliberately different nSamps (cap vs fullLen) -- that difference is
 * the entire point of the optimization, not something to hide.
 */
int CollapseStereoToMonoSubbandCapSelftest(void)
{
	/* dequantSubbandCapSampleLimit values Dequantize() actually computes:
	 * MIN(activeSubbands+1, NBANDS)*18 for the fast-lowrate activeSubbands
	 * cases in DequantSubbandCapSelftest's kActiveSubbandsCases (26, 20, 16,
	 * 12, 10, 8, 6). */
	static const int kCapCases[] = { 486, 378, 306, 234, 198, 162, 126 };
	static int bufCapped[MAX_NCHAN][MAX_NSAMP];
	static int bufZeroed[MAX_NCHAN][MAX_NSAMP];
	unsigned int rngState = 0xDEC0DEADU;
	int trial, caseIdx, failures, total;

	failures = 0;
	total = 0;

	for (trial = 0; trial < 2000; trial++) {
		int cap, fullLen, i, ch, pattern;
		int nonZeroBoundCapped, nonZeroBoundZeroed;
		int gbCapped[2], gbZeroed[2];
		int mismatch;

		caseIdx = trial % (int)(sizeof(kCapCases) / sizeof(kCapCases[0]));
		cap = kCapCases[caseIdx];

		rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
		if (trial < (int)(sizeof(kCapCases) / sizeof(kCapCases[0])))
			fullLen = MAX_NSAMP;			/* cap always binding at least once per case */
		else
			fullLen = cap + (int)(rngState % (unsigned int)(MAX_NSAMP - cap + 1));
		if (fullLen > MAX_NSAMP) fullLen = MAX_NSAMP;
		if (fullLen < cap) fullLen = cap;	/* cap can never exceed the true nSamps in production */

		pattern = trial % 5;
		for (i = 0; i < MAX_NSAMP; i++) {
			int l, r;

			rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
			if (i >= fullLen) {
				l = r = 0;
			} else {
				switch (pattern) {
				case 0: l = 0x3fffffff; r = 0x3fffffff; break;
				case 1: l = (int)0xc0000000U; r = (int)0xc0000000U; break;
				case 2: l = 0x3fffffff; r = (int)0xc0000000U; break;
				case 3: l = (int)(rngState & 0x7fffffffU); r = -(int)(rngState & 0x7fffffffU); break;
				default: l = (int)rngState >> 4; r = (int)(rngState ^ 0x9e3779b9U) >> 4; break;
				}
			}
			bufCapped[0][i] = l; bufCapped[1][i] = r;
			bufZeroed[0][i] = l; bufZeroed[1][i] = r;
		}
		/* "discarded region zeroed": only the zeroed run's tail is forced to
		 * 0; the capped run keeps its (irrelevant, never-read-beyond-cap)
		 * data so this also proves capping truly never reads past cap. */
		for (ch = 0; ch < MAX_NCHAN; ch++)
			for (i = cap; i < fullLen; i++)
				bufZeroed[ch][i] = 0;

		nonZeroBoundCapped = nonZeroBoundZeroed = -1;
		gbCapped[0] = gbCapped[1] = gbZeroed[0] = gbZeroed[1] = -1;
		CollapseStereoToMono(bufCapped, cap, &nonZeroBoundCapped, gbCapped);
		CollapseStereoToMono(bufZeroed, fullLen, &nonZeroBoundZeroed, gbZeroed);
		total++;

		mismatch = 0;
		for (i = 0; i < cap; i++) {
			if (bufCapped[0][i] != bufZeroed[0][i]) {
				printf("collapse subband cap selftest MISMATCH trial=%d cap=%d fullLen=%d i=%d: "
					"capped=%d zeroed=%d\n", trial, cap, fullLen, i, bufCapped[0][i], bufZeroed[0][i]);
				mismatch = 1;
				break;
			}
		}
		if (gbCapped[0] != gbZeroed[0]) {
			printf("collapse subband cap selftest MISMATCH trial=%d cap=%d fullLen=%d: "
				"gb[0] capped=%d zeroed=%d\n", trial, cap, fullLen, gbCapped[0], gbZeroed[0]);
			mismatch = 1;
		}
		if (nonZeroBoundCapped != cap) {
			printf("collapse subband cap selftest MISMATCH trial=%d cap=%d fullLen=%d: "
				"capped nonZeroBound[0]=%d expected=%d\n", trial, cap, fullLen, nonZeroBoundCapped, cap);
			mismatch = 1;
		}
		if (nonZeroBoundZeroed != fullLen) {
			printf("collapse subband cap selftest MISMATCH trial=%d cap=%d fullLen=%d: "
				"zeroed nonZeroBound[0]=%d expected=%d\n", trial, cap, fullLen, nonZeroBoundZeroed, fullLen);
			mismatch = 1;
		}
		if (mismatch)
			failures++;
	}

	printf("CollapseStereoToMono subband cap selftest: %d trials, %s\n",
		total, failures ? "FAIL" : "PASS");
	return failures ? -1 : 0;
}


/**************************************************************************************
 * Function:    Dequantize
 *
 * Description: dequantize coefficients, decode stereo, reorder short blocks
 *                (one granule-worth)
 *
 * Inputs:      MP3DecInfo structure filled by UnpackFrameHeader(), UnpackSideInfo(),
 *                UnpackScaleFactors(), and DecodeHuffman() (for this granule)
 *              index of current granule
 *
 * Outputs:     dequantized and reordered coefficients in hi->huffDecBuf
 *                (one granule-worth, all channels), format = Q26
 *              operates in-place on huffDecBuf but also needs di->workBuf
 *              updated hi->nonZeroBound index for both channels
 *
 * Return:      0 on success, -1 if null input pointers
 *
 * Notes:       In calling output Q(DQ_FRACBITS_OUT), we assume an implicit bias
 *                of 2^15. Some (floating-point) reference implementations factor this
 *                into the 2^(0.25 * gain) scaling explicitly. But to avoid precision
 *                loss, we don't do that. Instead take it into account in the final
 *                round to PCM (>> by 15 less than we otherwise would have).
 *              Equivalently, we can think of the dequantized coefficients as
 *                Q(DQ_FRACBITS_OUT - 15) with no implicit bias.
 **************************************************************************************/
int Dequantize(MP3DecInfo *mp3DecInfo, int gr)
{
	int i, ch, nSamps, mOut[2];
	int dequantSubbandCapSampleLimit;
	FrameHeader *fh;
	SideInfo *si;
	ScaleFactorInfo *sfi;
	HuffmanInfo *hi;
	DequantInfo *di;
	CriticalBandInfo *cbi;
	clock_t amigaProfileStart;

	/* validate pointers */
	if (!mp3DecInfo || !mp3DecInfo->FrameHeaderPS || !mp3DecInfo->SideInfoPS || !mp3DecInfo->ScaleFactorInfoPS ||
		!mp3DecInfo->HuffmanInfoPS || !mp3DecInfo->DequantInfoPS)
		return -1;

	fh = (FrameHeader *)(mp3DecInfo->FrameHeaderPS);

	/* si is an array of up to 4 structs, stored as gr0ch0, gr0ch1, gr1ch0, gr1ch1 */
	si = (SideInfo *)(mp3DecInfo->SideInfoPS);
	sfi = (ScaleFactorInfo *)(mp3DecInfo->ScaleFactorInfoPS);
	hi = (HuffmanInfo *)mp3DecInfo->HuffmanInfoPS;
	di = (DequantInfo *)mp3DecInfo->DequantInfoPS;
	cbi = di->cbi;
	mOut[0] = mOut[1] = 0;

	/* The IMDCT subband cap later discards activeSubbands and above, but
	 * AntiAlias() must still run the boundary between the final kept subband
	 * and the first discarded one.  That boundary reads the first eight samples
	 * of the first discarded subband, so DequantChannel() must keep one guard
	 * subband beyond the final IMDCT/subband cap.  Without that guard band the
	 * kept edge subband is antialiased against stale/zero data, which is exactly
	 * the kind of hard scratchy artefact heard at 22050 with caps around 16+.
	 * 0 means "no cap active", matching DequantChannel()'s contract.
	 */
	{
		int activeSubbands = MP3FastLowrateEffectiveActiveSubbands(mp3DecInfo);
		if (activeSubbands < NBANDS) {
			int dequantSubbands = activeSubbands + 1;
			if (dequantSubbands > NBANDS)
				dequantSubbands = NBANDS;
			dequantSubbandCapSampleLimit = dequantSubbands * 18;
		} else {
			dequantSubbandCapSampleLimit = 0;
		}
	}

	/* Pure mid/side joint stereo is special for mono output: after MPEG
	 * reconstruction, (L + R) / 2 is exactly the coded mid channel with the
	 * same 1/sqrt(2) scale already applied by DequantChannel().  The decoder can
	 * advance over the known-length side-channel Huffman payload, and the side
	 * channel does not affect mono PCM or dequant/IMDCT/synthesis. */
	ch = (gr >= 0 && gr < mp3DecInfo->nGrans &&
		mp3DecInfo->monoMSSideSkipGranule[gr]) ? 1 : mp3DecInfo->nChans;

	/* dequantize all samples needed by the synthesis path */
	AMIGA_PROFILE_START(amigaProfileStart);
	while (ch-- > 0) {
		hi->gb[ch] = DequantChannel(hi->huffDecBuf[ch], di->workBuf, &hi->nonZeroBound[ch], fh,
			&si->sis[gr][ch], &sfi->sfis[gr][ch], &cbi[ch], dequantSubbandCapSampleLimit);
	}
	if (gr >= 0 && gr < mp3DecInfo->nGrans &&
		mp3DecInfo->monoMSSideSkipGranule[gr]) {
		hi->nonZeroBound[1] = 0;
		hi->gb[1] = hi->gb[0];
		MP3AddDecodeCoreMonoMSSideSkip(2);
	}
	AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_DEQUANT, amigaProfileStart);

	AMIGA_PROFILE_START(amigaProfileStart);

	/* joint stereo processing assumes one guard bit in input samples
	 * it's extremely rare not to have at least one gb, so if this is the case
	 *   just make a pass over the data and clip to [-2^30+1, 2^30-1]
	 * in practice this may never happen
	 */
	if (!(gr >= 0 && gr < mp3DecInfo->nGrans &&
		mp3DecInfo->monoMSSideSkipGranule[gr]) &&
		fh->modeExt && (hi->gb[0] < 1 || hi->gb[1] < 1)) {
		for (i = 0; i < hi->nonZeroBound[0]; i++) {
			if (hi->huffDecBuf[0][i] < -0x3fffffff)	 hi->huffDecBuf[0][i] = -0x3fffffff;
			if (hi->huffDecBuf[0][i] >  0x3fffffff)	 hi->huffDecBuf[0][i] =  0x3fffffff;
		}
		for (i = 0; i < hi->nonZeroBound[1]; i++) {
			if (hi->huffDecBuf[1][i] < -0x3fffffff)	 hi->huffDecBuf[1][i] = -0x3fffffff;
			if (hi->huffDecBuf[1][i] >  0x3fffffff)	 hi->huffDecBuf[1][i] =  0x3fffffff;
		}
	}

	if (gr >= 0 && gr < mp3DecInfo->nGrans &&
		mp3DecInfo->monoMSSideSkipGranule[gr]) {
		/* Mid/side without intensity is cheap for mono: after MPEG MS
		 * reconstruction, (L + R) / 2 is the coded mid channel.  Keep
		 * channel 0 and skip right-channel IMDCT/subband synthesis. */
		/* Side was not decoded; keep the coded mid channel as mono. */
		nSamps = hi->nonZeroBound[0];
		hi->nonZeroBound[0] = nSamps;
	} else {
		/* do mid-side stereo processing, if enabled */
		if (fh->modeExt >> 1) {
			if (fh->modeExt & 0x01) {
				/* intensity stereo enabled - run mid-side up to start of right zero region */
				if (cbi[1].cbType == 0)
					nSamps = fh->sfBand->l[cbi[1].cbEndL + 1];
				else
					nSamps = 3 * fh->sfBand->s[cbi[1].cbEndSMax + 1];
			} else {
				/* intensity stereo disabled - run mid-side on whole spectrum */
				nSamps = MAX(hi->nonZeroBound[0], hi->nonZeroBound[1]);
			}
			/* Same reasoning as dequantSubbandCapSampleLimit above: samples
			 * at/beyond that boundary were left un-dequantized by
			 * DequantChannel() when a fast-lowrate subband cap is active,
			 * and IMDCT's own cap (driven by the same
			 * MP3FastLowrateEffectiveActiveSubbands() value, with this
			 * same +1 guard subband covering AntiAlias()'s boundary read)
			 * never reads that far into hi->huffDecBuf either way -- so
			 * reconstructing sum/difference values there is wasted work
			 * regardless of nonZeroBound/cbEndL/cbEndSMax being larger.
			 * Excluding those samples from MidSideProc's guard-bit scan
			 * can only *reduce* its mOut contribution (fewer values
			 * OR-ed in), which is the same safe direction dequant's own
			 * cap-skip already relies on -- see DequantSubbandCapSelftest
			 * and this cap's dedicated MidSideProcSubbandCapSelftest. */
			if (dequantSubbandCapSampleLimit > 0 && nSamps > dequantSubbandCapSampleLimit)
				nSamps = dequantSubbandCapSampleLimit;
			MidSideProc(hi->huffDecBuf, nSamps, mOut);
		}

		/* do intensity stereo processing, if enabled */
		if (fh->modeExt & 0x01) {
			nSamps = hi->nonZeroBound[0];
			if (fh->ver == MPEG1) {
				IntensityProcMPEG1(hi->huffDecBuf, nSamps, fh, &sfi->sfis[gr][1], di->cbi,
					fh->modeExt >> 1, si->sis[gr][1].mixedBlock, mOut);
			} else {
				IntensityProcMPEG2(hi->huffDecBuf, nSamps, fh, &sfi->sfis[gr][1], di->cbi, &sfi->sfjs,
					fh->modeExt >> 1, si->sis[gr][1].mixedBlock, mOut);
			}
		}

		/* adjust guard bit count and nonZeroBound if we did any stereo processing */
		if (fh->modeExt) {
			hi->gb[0] = CLZ(mOut[0]) - 1;
			hi->gb[1] = CLZ(mOut[1]) - 1;
			nSamps = MAX(hi->nonZeroBound[0], hi->nonZeroBound[1]);
			hi->nonZeroBound[0] = nSamps;
			hi->nonZeroBound[1] = nSamps;
		}

		if (mp3DecInfo->outputMono && mp3DecInfo->nChans == 2) {
			nSamps = MAX(hi->nonZeroBound[0], hi->nonZeroBound[1]);
			/* Long-block DequantChannel() calls leave sampleBuf[i..nonZeroBound)
			 * un-dequantized (raw Huffman magnitudes, not Q26 samples) beyond
			 * dequantSubbandCapSampleLimit when the fast-lowrate subband cap is
			 * active -- see that function's subbandCapSampleLimit comment.
			 * nonZeroBound itself is only shrunk by short-block reordering, so
			 * it can still exceed the cap here. Without this clamp,
			 * CollapseStereoToMono() would mix that raw data into x[0] and fold
			 * its magnitude into gb[0], exactly the class of guard-bit
			 * corruption the antialias guard-band fix elsewhere in this file
			 * addressed. dequantSubbandCapSampleLimit already includes the +1
			 * guard subband AntiAlias() needs, so reusing it here (rather than
			 * a tighter cap) preserves that fix -- same reasoning as
			 * MidSideProc's cap above; see CollapseStereoToMonoSubbandCapSelftest. */
			if (dequantSubbandCapSampleLimit > 0 && nSamps > dequantSubbandCapSampleLimit)
				nSamps = dequantSubbandCapSampleLimit;
			CollapseStereoToMono(hi->huffDecBuf, nSamps, hi->nonZeroBound, hi->gb);
		}
	}

	AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_STEREO_POST, amigaProfileStart);

	/* output format Q(DQ_FRACBITS_OUT) */
	return 0;
}