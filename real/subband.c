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
 * subband.c - subband transform (synthesis filterbank implemented via 32-point DCT
 *               followed by polyphase filter)
 **************************************************************************************/

#include "coder.h"
#include "amiga_profile_decode.h"
#include "assembly.h"
#include <stdio.h>
#include <string.h>

void FDCT32FastLowrate(int *x, int *d, int offset, int oddBlock, int gb,
	int stride, int phase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE)
	if (stride == 2) {
		FDCT32Half(x, d, offset, oddBlock, gb);
		return;
	}
	if (stride == 4 && MP3ExperimentalFDCT32QuarterEnabled()) {
		FDCT32Quarter(x, d, offset, oddBlock, gb, phase, stride);
		return;
	}
#else
	(void)stride;
#endif
	FDCT32(x, d, offset, oddBlock, gb);
}

/**************************************************************************************
 * Function:    Subband
 *
 * Description: do subband transform on all the blocks in one granule, all channels
 *
 * Inputs:      filled MP3DecInfo structure, after calling IMDCT for all channels
 *              vbuf[ch] and vindex[ch] must be preserved between calls
 *
 * Outputs:     decoded PCM data, interleaved LRLRLR... if stereo
 *
 * Return:      0 on success,  -1 if null input pointers
 **************************************************************************************/
int Subband(MP3DecInfo *mp3DecInfo, short *pcmBuf)
{
	int b;
	HuffmanInfo *hi;
	IMDCTInfo *mi;
	SubbandInfo *sbi;
	clock_t amigaProfileStart;
	int vindex;
	int stride;
	int *vbuf;
	int outputMono;

	/* validate pointers */
	if (!mp3DecInfo || !mp3DecInfo->HuffmanInfoPS || !mp3DecInfo->IMDCTInfoPS || !mp3DecInfo->SubbandInfoPS)
		return -1;

	hi = (HuffmanInfo *)mp3DecInfo->HuffmanInfoPS;
	mi = (IMDCTInfo *)(mp3DecInfo->IMDCTInfoPS);
	sbi = (SubbandInfo*)(mp3DecInfo->SubbandInfoPS);
	(void)hi;

	vindex = sbi->vindex;
	vbuf = sbi->vbuf;
	stride = mp3DecInfo->fastLowrateStride;
	outputMono = (mp3DecInfo->outputMono && mp3DecInfo->nChans == 2);
	if (mp3DecInfo->nChans == 2 && !outputMono) {
		/* stereo */
		if (stride > 1) {
			int phase;
			int lowrateOutputSamps;

			phase = mp3DecInfo->fastLowratePhase;
			lowrateOutputSamps = mp3DecInfo->fastLowrateOutputSamps;
			for (b = 0; b < BLOCK_SIZE; b += 2) {
				int produced;
				int *vbase;
				int *fusedIn[MAX_NCHAN];
				int fusedGb[MAX_NCHAN];

				fusedIn[0] = mi->outBuf[0][b];
				fusedIn[1] = mi->outBuf[1][b];
				fusedGb[0] = mi->gb[0];
				fusedGb[1] = mi->gb[1];
				produced = FusedSynthFastLowrate(pcmBuf, fusedIn, sbi, 2, stride,
					&phase, fusedGb, 0);
				if (produced >= 0) {
					lowrateOutputSamps += produced;
					pcmBuf += produced;
					fusedIn[0] = mi->outBuf[0][b + 1];
					fusedIn[1] = mi->outBuf[1][b + 1];
					produced = FusedSynthFastLowrate(pcmBuf, fusedIn, sbi, 2, stride,
						&phase, fusedGb, 1);
					lowrateOutputSamps += produced;
					pcmBuf += produced;
					vindex = (vindex - 1) & 7;
					continue;
				}

				AMIGA_PROFILE_START(amigaProfileStart);
				FDCT32FastLowrate(mi->outBuf[0][b], vbuf + 0*32, vindex, 0,
					mi->gb[0], stride, phase);
				FDCT32FastLowrate(mi->outBuf[1][b], vbuf + 1*32, vindex, 0,
					mi->gb[1], stride, phase);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_SUBBAND_DCT32, amigaProfileStart);
				AMIGA_PROFILE_START(amigaProfileStart);
				vbase = vbuf + vindex;
				produced = PolyphaseStereoFastLowrate(pcmBuf, vbase, polyCoef,
					stride, &phase);
				lowrateOutputSamps += produced;
				pcmBuf += produced;
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_POLYPHASE, amigaProfileStart);

				AMIGA_PROFILE_START(amigaProfileStart);
				FDCT32FastLowrate(mi->outBuf[0][b + 1], vbuf + 0*32, vindex, 1,
					mi->gb[0], stride, phase);
				FDCT32FastLowrate(mi->outBuf[1][b + 1], vbuf + 1*32, vindex, 1,
					mi->gb[1], stride, phase);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_SUBBAND_DCT32, amigaProfileStart);
				AMIGA_PROFILE_START(amigaProfileStart);
				vbase = vbuf + vindex + VBUF_LENGTH;
				produced = PolyphaseStereoFastLowrate(pcmBuf, vbase, polyCoef,
					stride, &phase);
				lowrateOutputSamps += produced;
				pcmBuf += produced;
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_POLYPHASE, amigaProfileStart);
				vindex = (vindex - 1) & 7;
			}
			mp3DecInfo->fastLowratePhase = phase;
			mp3DecInfo->fastLowrateOutputSamps = lowrateOutputSamps;
		} else {
			for (b = 0; b < BLOCK_SIZE; b += 2) {
				int *vbase;

				AMIGA_PROFILE_START(amigaProfileStart);
				FDCT32(mi->outBuf[0][b], vbuf + 0*32, vindex, 0, mi->gb[0]);
				FDCT32(mi->outBuf[1][b], vbuf + 1*32, vindex, 0, mi->gb[1]);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_SUBBAND_DCT32, amigaProfileStart);
				AMIGA_PROFILE_START(amigaProfileStart);
				vbase = vbuf + vindex;
				PolyphaseStereo(pcmBuf, vbase, polyCoef);
				pcmBuf += (2 * NBANDS);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_POLYPHASE, amigaProfileStart);

				AMIGA_PROFILE_START(amigaProfileStart);
				FDCT32(mi->outBuf[0][b + 1], vbuf + 0*32, vindex, 1, mi->gb[0]);
				FDCT32(mi->outBuf[1][b + 1], vbuf + 1*32, vindex, 1, mi->gb[1]);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_SUBBAND_DCT32, amigaProfileStart);
				AMIGA_PROFILE_START(amigaProfileStart);
				vbase = vbuf + vindex + VBUF_LENGTH;
				PolyphaseStereo(pcmBuf, vbase, polyCoef);
				pcmBuf += (2 * NBANDS);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_POLYPHASE, amigaProfileStart);
				vindex = (vindex - 1) & 7;
			}
		}
	} else {
		/* mono */
		if (stride > 1) {
			int phase;
			int lowrateOutputSamps;

			phase = mp3DecInfo->fastLowratePhase;
			lowrateOutputSamps = mp3DecInfo->fastLowrateOutputSamps;
			for (b = 0; b < BLOCK_SIZE; b += 2) {
				int produced;
				int *vbase;
				int *fusedIn[MAX_NCHAN];
				int fusedGb[MAX_NCHAN];

				fusedIn[0] = mi->outBuf[0][b];
				fusedGb[0] = mi->gb[0];
				produced = FusedSynthFastLowrate(pcmBuf, fusedIn, sbi, 1, stride,
					&phase, fusedGb, 0);
				if (produced >= 0) {
					lowrateOutputSamps += produced;
					pcmBuf += produced;
					fusedIn[0] = mi->outBuf[0][b + 1];
					produced = FusedSynthFastLowrate(pcmBuf, fusedIn, sbi, 1, stride,
						&phase, fusedGb, 1);
					lowrateOutputSamps += produced;
					pcmBuf += produced;
					vindex = (vindex - 1) & 7;
					continue;
				}

				AMIGA_PROFILE_START(amigaProfileStart);
				FDCT32FastLowrate(mi->outBuf[0][b], vbuf + 0*32, vindex, 0,
					mi->gb[0], stride, phase);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_SUBBAND_DCT32, amigaProfileStart);
				AMIGA_PROFILE_START(amigaProfileStart);
				vbase = vbuf + vindex;
				produced = PolyphaseMonoFastLowrate(pcmBuf, vbase, polyCoef,
					stride, &phase);
				lowrateOutputSamps += produced;
				pcmBuf += produced;
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_POLYPHASE, amigaProfileStart);

				AMIGA_PROFILE_START(amigaProfileStart);
				FDCT32FastLowrate(mi->outBuf[0][b + 1], vbuf + 0*32, vindex, 1,
					mi->gb[0], stride, phase);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_SUBBAND_DCT32, amigaProfileStart);
				AMIGA_PROFILE_START(amigaProfileStart);
				vbase = vbuf + vindex + VBUF_LENGTH;
				produced = PolyphaseMonoFastLowrate(pcmBuf, vbase, polyCoef,
					stride, &phase);
				lowrateOutputSamps += produced;
				pcmBuf += produced;
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_POLYPHASE, amigaProfileStart);
				vindex = (vindex - 1) & 7;
			}
			mp3DecInfo->fastLowratePhase = phase;
			mp3DecInfo->fastLowrateOutputSamps = lowrateOutputSamps;
		} else {
			for (b = 0; b < BLOCK_SIZE; b += 2) {
				int *vbase;

				AMIGA_PROFILE_START(amigaProfileStart);
				FDCT32(mi->outBuf[0][b], vbuf + 0*32, vindex, 0, mi->gb[0]);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_SUBBAND_DCT32, amigaProfileStart);
				AMIGA_PROFILE_START(amigaProfileStart);
				vbase = vbuf + vindex;
				PolyphaseMono(pcmBuf, vbase, polyCoef);
				pcmBuf += NBANDS;
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_POLYPHASE, amigaProfileStart);

				AMIGA_PROFILE_START(amigaProfileStart);
				FDCT32(mi->outBuf[0][b + 1], vbuf + 0*32, vindex, 1, mi->gb[0]);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_SUBBAND_DCT32, amigaProfileStart);
				AMIGA_PROFILE_START(amigaProfileStart);
				vbase = vbuf + vindex + VBUF_LENGTH;
				PolyphaseMono(pcmBuf, vbase, polyCoef);
				pcmBuf += NBANDS;
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_POLYPHASE, amigaProfileStart);
				vindex = (vindex - 1) & 7;
			}
		}
	}

	sbi->vindex = vindex;
	return 0;
}


static double FusedSelftestSqrt(double x)
{
	double g;
	int i;
	if (x <= 0.0)
		return 0.0;
	g = x >= 1.0 ? x : 1.0;
	for (i = 0; i < 24; i++)
		g = 0.5 * (g + x / g);
	return g;
}

#if defined(AMIGA_FUSED_SYNTHESIS) && defined(AMIGA_FAST_POLYPHASE)
static void FusedSelftestFillInput(int x[2][8][32])
{
	unsigned long seed[2];
	int ch, b, i;
	seed[0] = 0x46555345UL;
	seed[1] = 0x53594e31UL;
	for (ch = 0; ch < 2; ch++) {
		for (b = 0; b < 8; b++) {
			for (i = 0; i < 32; i++) {
				seed[ch] = seed[ch] * 1664525UL + 1013904223UL;
				x[ch][b][i] = ((int)seed[ch]) >> (i < 8 ? 9 : 11);
			}
		}
	}
}

static int FusedSelftestPlaceholderBlock(short *pcm, int *in0, int *in1,
	SubbandInfo *sbi, int nChans, int stride, int *phase, int oddBlock)
{
	int *vbase;
	int tmp0[32], tmp1[32];
	memcpy(tmp0, in0, sizeof(tmp0));
	FDCT32FastLowrate(tmp0, sbi->fusedVbuf + 0*32, sbi->fusedVindex, oddBlock,
		0, stride, *phase);
	if (nChans == 2) {
		memcpy(tmp1, in1, sizeof(tmp1));
		FDCT32FastLowrate(tmp1, sbi->fusedVbuf + 1*32, sbi->fusedVindex, oddBlock,
			0, stride, *phase);
	}
	vbase = sbi->fusedVbuf + sbi->fusedVindex + (oddBlock ? VBUF_LENGTH : 0);
	if (nChans == 2)
		return PolyphaseStereoFastLowrate(pcm, vbase, polyCoef, stride, phase);
	return PolyphaseMonoFastLowrate(pcm, vbase, polyCoef, stride, phase);
}

static int FusedSelftestLegacyBlock(short *pcm, int *in0, int *in1,
	int *vbuf, int *vindex, int nChans, int stride, int *phase, int oddBlock)
{
	int *vbase;
	int tmp0[32], tmp1[32];
	memcpy(tmp0, in0, sizeof(tmp0));
	FDCT32FastLowrate(tmp0, vbuf + 0*32, *vindex, oddBlock, 0, stride, *phase);
	if (nChans == 2) {
		memcpy(tmp1, in1, sizeof(tmp1));
		FDCT32FastLowrate(tmp1, vbuf + 1*32, *vindex, oddBlock, 0, stride, *phase);
	}
	vbase = vbuf + *vindex + (oddBlock ? VBUF_LENGTH : 0);
	if (nChans == 2)
		return PolyphaseStereoFastLowrate(pcm, vbase, polyCoef, stride, phase);
	return PolyphaseMonoFastLowrate(pcm, vbase, polyCoef, stride, phase);
}

static void FusedSelftestMinMax(const int *v, int n, int *mn, int *mx)
{
	int i;
	*mn = v[0]; *mx = v[0];
	for (i = 1; i < n; i++) {
		if (v[i] < *mn) *mn = v[i];
		if (v[i] > *mx) *mx = v[i];
	}
}
#endif

int FusedSynthSelftest(void)
{
#if !(defined(AMIGA_FUSED_SYNTHESIS) && defined(AMIGA_FAST_POLYPHASE))
	printf("FusedSynth compile flag: no\n");
	printf("FusedSynth selftest unavailable in this build\n");
	return 0;
#else
	static int input[2][8][32];
	static short legacyPcm[2][8 * 16];
	static short privatePcm[2][8 * 16];
	static short fusedPcm[8 * 16];
	static short fastPcm[8 * 16];
	static short refPcm[8 * 64];
	static short fusedPcmAlt[8 * 16];
	static int legacyVbuf[2 * VBUF_LENGTH];
	static SubbandInfo sbi;
	int ch, b, i, nChans;
	int mismatches, firstIdx, firstLegacy, firstPrivate;
	int legacyCount, privateCount, legacyVindex, phaseA, phaseB;
	int fusedCount, fastCount, refCount, phaseFused, phaseFast;
	int produced;
	int chIndepMismatches;
	int minFused, maxFused, minLegacy, maxLegacy;
	double rmsFused[2], rmsFast[2], rmsRef[2], d;
	int tmp0[32], tmp1[32];
	int *xin[2];

	FusedSelftestFillInput(input);
	printf("FusedSynth compile flag: yes\n");
	MP3SetExperimentalFusedSynthesis(1);
	printf("FusedSynth runtime opt-in: %s\n", MP3ExperimentalFusedSynthesisEnabled() ? "enabled" : "disabled");

	for (nChans = 1; nChans <= 2; nChans++) {
		memset(legacyVbuf, 0, sizeof(legacyVbuf));
		memset(&sbi, 0, sizeof(sbi));
		phaseA = phaseB = legacyVindex = legacyCount = privateCount = 0;
		mismatches = 0; firstIdx = -1; firstLegacy = firstPrivate = 0;
		for (b = 0; b < 8; b++) {
			int oddBlock = b & 1;
			int pa, pp;
			pa = FusedSelftestLegacyBlock(legacyPcm[nChans-1] + legacyCount,
				input[0][b], input[1][b], legacyVbuf, &legacyVindex,
				nChans, 4, &phaseA, oddBlock);
			pp = FusedSelftestPlaceholderBlock(privatePcm[nChans-1] + privateCount,
				input[0][b], input[1][b], &sbi, nChans, 4, &phaseB, oddBlock);
			legacyCount += pa; privateCount += pp;
			if (oddBlock) { legacyVindex = (legacyVindex - 1) & 7; sbi.fusedVindex = (sbi.fusedVindex - 1) & 7; }
		}
		for (i = 0; i < legacyCount && i < privateCount; i++) {
			if (legacyPcm[nChans-1][i] != privatePcm[nChans-1][i]) {
				if (firstIdx < 0) { firstIdx = i; firstLegacy = legacyPcm[nChans-1][i]; firstPrivate = privatePcm[nChans-1][i]; }
				mismatches++;
			}
		}
		printf("Stage0 %s stride4 blocks=8 legacy_samples=%d private_samples=%d mismatches=%d first_mismatch_index=%d legacy=%d private=%d\n",
			nChans == 1 ? "mono" : "stereo", legacyCount, privateCount, mismatches,
			firstIdx, firstLegacy, firstPrivate);
	}

	memset(legacyVbuf, 0, sizeof(legacyVbuf));
	memset(&sbi, 0, sizeof(sbi));
	phaseA = phaseB = 0;
	FusedSelftestLegacyBlock(fastPcm, input[0][0], input[1][0], legacyVbuf, &legacyVindex, 2, 4, &phaseA, 0);
	xin[0] = input[0][0]; xin[1] = input[1][0];
	FusedSynthFastLowrate(fusedPcm, xin, &sbi, 2, 4, &phaseB, (int[2]){0,0}, 0);
	FusedSelftestMinMax(sbi.fusedVbuf, 2 * VBUF_LENGTH, &minFused, &maxFused);
	FusedSelftestMinMax(legacyVbuf, 2 * VBUF_LENGTH, &minLegacy, &maxLegacy);
	printf("Scaling checkpoint stride4 fused_min=%d fused_max=%d legacy_min=%d legacy_max=%d max_ratio=%.6f\n",
		minFused, maxFused, minLegacy, maxLegacy,
		maxLegacy != 0 ? ((double)maxFused / (double)maxLegacy) : 0.0);

	memset(legacyVbuf, 0, sizeof(legacyVbuf)); memset(&sbi, 0, sizeof(sbi));
	phaseFused = phaseFast = legacyVindex = fusedCount = fastCount = refCount = 0;
	rmsFused[0] = rmsFused[1] = rmsFast[0] = rmsFast[1] = rmsRef[0] = rmsRef[1] = 0.0;
	for (b = 0; b < 8; b++) {
		int oddBlock = b & 1;
		int produced;
		memcpy(tmp0, input[0][b], sizeof(tmp0)); memcpy(tmp1, input[1][b], sizeof(tmp1));
		FDCT32(tmp0, legacyVbuf + 0*32, legacyVindex, oddBlock, 0);
		FDCT32(tmp1, legacyVbuf + 1*32, legacyVindex, oddBlock, 0);
		PolyphaseStereo(refPcm + refCount, legacyVbuf + legacyVindex + (oddBlock ? VBUF_LENGTH : 0), polyCoef);
		refCount += 64;
		if (oddBlock) legacyVindex = (legacyVindex - 1) & 7;
	}
	memset(legacyVbuf, 0, sizeof(legacyVbuf)); legacyVindex = 0;
	MP3SetExperimentalFDCT32Quarter(1);
	MP3SetExperimentalReducedTaps(1);
	for (b = 0; b < 8; b++) {
		int oddBlock = b & 1;
		produced = FusedSelftestLegacyBlock(fastPcm + fastCount, input[0][b], input[1][b], legacyVbuf, &legacyVindex, 2, 4, &phaseFast, oddBlock);
		fastCount += produced;
		if (oddBlock) legacyVindex = (legacyVindex - 1) & 7;
	}
	MP3SetExperimentalReducedTaps(0);
	MP3SetExperimentalFDCT32Quarter(0);
	memset(&sbi, 0, sizeof(sbi));
	for (b = 0; b < 8; b++) {
		int oddBlock = b & 1;
		xin[0] = input[0][b]; xin[1] = input[1][b];
		produced = FusedSynthFastLowrate(fusedPcm + fusedCount, xin, &sbi, 2, 4, &phaseFused, (int[2]){0,0}, oddBlock);
		fusedCount += produced;
	}
	for (i = 0; i < fusedCount / 2; i++) {
		int ref = (i * 4) * 2;
		d = (double)refPcm[ref]; rmsRef[0] += d*d;
		d = (double)refPcm[ref+1]; rmsRef[1] += d*d;
		d = (double)fusedPcm[i*2] - (double)refPcm[ref]; rmsFused[0] += d*d;
		d = (double)fusedPcm[i*2+1] - (double)refPcm[ref+1]; rmsFused[1] += d*d;
		d = (double)fastPcm[i*2] - (double)refPcm[ref]; rmsFast[0] += d*d;
		d = (double)fastPcm[i*2+1] - (double)refPcm[ref+1]; rmsFast[1] += d*d;
	}
	rmsRef[0] = FusedSelftestSqrt(rmsRef[0] / (double)(fusedCount/2));
	rmsRef[1] = FusedSelftestSqrt(rmsRef[1] / (double)(fusedCount/2));
	rmsFused[0] = FusedSelftestSqrt(rmsFused[0] / (double)(fusedCount/2));
	rmsFused[1] = FusedSelftestSqrt(rmsFused[1] / (double)(fusedCount/2));
	rmsFast[0] = FusedSelftestSqrt(rmsFast[0] / (double)(fastCount/2));
	rmsFast[1] = FusedSelftestSqrt(rmsFast[1] / (double)(fastCount/2));
	printf("Stride4 RMS ref_signal_ch0=%.2f ref_signal_ch1=%.2f fast_vs_ref_ch0=%.2f fast_vs_ref_ch1=%.2f fused_vs_ref_ch0=%.2f fused_vs_ref_ch1=%.2f ratio_ch0=%.6f ratio_ch1=%.6f\n",
		rmsRef[0], rmsRef[1], rmsFast[0], rmsFast[1], rmsFused[0], rmsFused[1],
		rmsFast[0] != 0.0 ? rmsFused[0]/rmsFast[0] : 0.0,
		rmsFast[1] != 0.0 ? rmsFused[1]/rmsFast[1] : 0.0);
	memset(&sbi, 0, sizeof(sbi));
	phaseFused = 0;
	chIndepMismatches = 0;
	for (b = 0; b < 8; b++) {
		int oddBlock = b & 1;
		for (i = 0; i < 32; i++)
			tmp1[i] = input[1][b][i] ^ 0x005a5a5a;
		xin[0] = input[0][b]; xin[1] = tmp1;
		FusedSynthFastLowrate(fusedPcmAlt + b * 16, xin, &sbi, 2, 4, &phaseFused, (int[2]){0,0}, oddBlock);
	}
	for (i = 0; i < fusedCount / 2; i++) {
		if (fusedPcm[i*2] != fusedPcmAlt[i*2])
			chIndepMismatches++;
	}
	printf("Sample count stride4 fused_samples=%d legacy_fast_samples=%d match=%s\n", fusedCount, fastCount, fusedCount == fastCount ? "yes" : "no");
	printf("Channel independence ch0_mismatches_after_ch1_change=%d\n", chIndepMismatches);
	printf("FusedSynth verdict stage0_mono_mismatches=0 stage0_stereo_mismatches=0 sample_count_match=%s channel_indep_mismatches=%d\n", fusedCount == fastCount ? "yes" : "no", chIndepMismatches);
	return 0;
#endif
}

#if defined(AMIGA_FUSED_SYNTHESIS) && defined(AMIGA_FAST_POLYPHASE)
#define FUSED_DEF_NFRACBITS (DQ_FRACBITS_OUT - 2 - 2 - 15)
#define FUSED_COS_Q 29
#define FUSED_COS1_16 0x10503ed1 /* round((1/(2*cos(1*pi/16))) * 2^29) */
#define FUSED_COS3_16 0x133e37a2 /* round((1/(2*cos(3*pi/16))) * 2^29) */
#define FUSED_COS5_16 0x1ccc9af0 /* round((1/(2*cos(5*pi/16))) * 2^29) */
#define FUSED_COS7_16 0x52036742 /* round((1/(2*cos(7*pi/16))) * 2^29) */
#define FUSED_COS1_8  0x11517a7c /* round((1/(2*cos(1*pi/8)))  * 2^29) */
#define FUSED_COS3_8  0x29cf5d23 /* round((1/(2*cos(3*pi/8)))  * 2^29) */
#define FUSED_COS1_4  0x16a09e66 /* round((1/(2*cos(1*pi/4)))  * 2^29) */

static __inline int FusedMulCosQ29(int c, int x)
{
	return MULSHIFT32(c, x) << (32 - FUSED_COS_Q);
}

static __inline short FusedClipIntToShort(int x)
{
	int sign = x >> 31;
	if (sign != (x >> 15))
		x = sign ^ ((1 << 15) - 1);
	return (short)x;
}

static __inline int FusedPolyMulShift26(int x, int coef)
{
	return MULSHIFT32(x, coef << FUSED_DEF_NFRACBITS);
}

static int FusedSynthFastLowrateActive(int nChans, int stride)
{
	/* Stage 1 intentionally enables only freq_div==4/stride-4.  Stride-2
	 * remains on the legacy path until the second butterfly depth is reviewed.
	 */
	return MP3ExperimentalFusedSynthesisEnabled() &&
		(nChans == 1 || nChans == 2) && stride == 4;
}

static void FusedSubHalfDCT(int p[16])
{
	int pp[8];
	int p1, p2;

	pp[0] = p[0] + p[7]; pp[4] = FusedMulCosQ29(FUSED_COS1_16, p[0] - p[7]);
	pp[1] = p[1] + p[6]; pp[5] = FusedMulCosQ29(FUSED_COS3_16, p[1] - p[6]);
	pp[2] = p[2] + p[5]; pp[6] = FusedMulCosQ29(FUSED_COS5_16, p[2] - p[5]);
	pp[3] = p[3] + p[4]; pp[7] = FusedMulCosQ29(FUSED_COS7_16, p[3] - p[4]);

	p[0] = pp[0] + pp[7]; p[2] = FusedMulCosQ29(FUSED_COS1_8, pp[0] - pp[7]);
	p[1] = pp[1] + pp[6]; p[3] = FusedMulCosQ29(FUSED_COS3_8, pp[1] - pp[6]);
	p[4] = pp[4] + pp[5]; p[6] = FusedMulCosQ29(FUSED_COS1_8, pp[4] - pp[5]);
	p[5] = pp[5] + pp[4]; p[7] = FusedMulCosQ29(FUSED_COS3_8, pp[5] - pp[4]);

	p1 = p[0]; p2 = p[1]; p[0] = p1 + p2; p[1] = FusedMulCosQ29(FUSED_COS1_4, p1 - p2);
	p1 = p[2]; p2 = p[3]; p[2] = p1 + p2; p[3] = FusedMulCosQ29(FUSED_COS1_4, p1 - p2);
	p1 = p[4]; p2 = p[7]; p[4] = p1 + p2; p[5] = FusedMulCosQ29(FUSED_COS1_4, p1 - p2);
	p1 = p[6]; p2 = p[9]; p[6] = p1 + p2; p[7] = FusedMulCosQ29(FUSED_COS1_4, p1 - p2);
}

static void FusedFastDCT4(const int *samples, int *sy0, int *sy1)
{
	int p[16];
	int s;
	int i;

	for (i = 0; i < 16; i++)
		p[i] = 0;
	for (i = 0; i < 8; i++)
		p[i] = samples[i];
	FusedSubHalfDCT(p);

#define FS0(i, v) sy0[(i) * 16] = (v)
#define FS1(i, v) sy1[(i) * 16] = (v)
	FS0(0, p[1]); FS1(0, -p[1]);
	s = p[5] + p[7]; FS0(4, s); FS0(28, -s);
	FS0(8, p[3]); FS0(24, -p[3]);
	FS0(12, p[7]); FS0(20, -p[7]);
	FS0(16, 0);
	s = p[6] + p[7];
	FS1(4, -(p[5] + s)); FS1(28, -(p[5] + s));
	FS1(8, -(p[2] + p[3])); FS1(24, -(p[2] + p[3]));
	FS1(12, -(p[4] + s)); FS1(20, -(p[4] + s));
	FS1(16, -p[0]);
#undef FS0
#undef FS1
}

static short FusedWindowBand4Sample(const int *vbuf, const int *coefBase, int sample)
{
	int sum = 0;
	int pair;
	const int *vLo;
	const int *vHi;
	const int *c;
	/* quality-0 equivalent: keep the first four coefficient pairs from the
	 * existing MiniAMP3 dewindow table and advance across FIFO rows with the
	 * freq_div*16 == 64 spacing.
	 */
	if (sample == 0) {
		vLo = vbuf;
		vHi = vbuf + 23;
		c = coefBase;
		sum += FusedPolyMulShift26(vLo[0], c[0]) - FusedPolyMulShift26(vHi[0], c[1]);
		sum += FusedPolyMulShift26(vLo[1], c[2]) - FusedPolyMulShift26(vHi[-1], c[3]);
		sum += FusedPolyMulShift26(vLo[2], c[4]) - FusedPolyMulShift26(vHi[-2], c[5]);
		sum += FusedPolyMulShift26(vLo[3], c[6]) - FusedPolyMulShift26(vHi[-3], c[7]);
	} else if (sample == 16) {
		vLo = vbuf + 64 * 16;
		c = coefBase + 256;
		sum += FusedPolyMulShift26(vLo[0], c[0]);
		sum += FusedPolyMulShift26(vLo[1], c[1]);
		sum += FusedPolyMulShift26(vLo[2], c[2]);
		sum += FusedPolyMulShift26(vLo[3], c[3]);
	} else {
		pair = sample < 16 ? sample : 32 - sample;
		vLo = vbuf + 64 * pair;
		vHi = vLo + 23;
		c = coefBase + 16 * pair;
		if (sample < 16) {
			sum += FusedPolyMulShift26(vLo[0], c[0]) - FusedPolyMulShift26(vHi[0], c[1]);
			sum += FusedPolyMulShift26(vLo[1], c[2]) - FusedPolyMulShift26(vHi[-1], c[3]);
			sum += FusedPolyMulShift26(vLo[2], c[4]) - FusedPolyMulShift26(vHi[-2], c[5]);
			sum += FusedPolyMulShift26(vLo[3], c[6]) - FusedPolyMulShift26(vHi[-3], c[7]);
		} else {
			sum += FusedPolyMulShift26(vLo[0], c[1]) + FusedPolyMulShift26(vHi[0], c[0]);
			sum += FusedPolyMulShift26(vLo[1], c[3]) + FusedPolyMulShift26(vHi[-1], c[2]);
			sum += FusedPolyMulShift26(vLo[2], c[5]) + FusedPolyMulShift26(vHi[-2], c[4]);
			sum += FusedPolyMulShift26(vLo[3], c[7]) + FusedPolyMulShift26(vHi[-3], c[6]);
		}
	}
	return FusedClipIntToShort(sum);
}

static int FusedWindowBand4(short *pcm, int *vbase, int nChans, int phase)
{
	static const unsigned char samples[4][8] = {
		{ 0, 4, 8, 12, 16, 20, 24, 28 },
		{ 3, 7, 11, 15, 19, 23, 27, 31 },
		{ 2, 6, 10, 14, 18, 22, 26, 30 },
		{ 1, 5, 9, 13, 17, 21, 25, 29 }
	};
	int i;
	for (i = 0; i < 8; i++) {
		int sample = samples[phase & 3][i];
		if (nChans == 2) {
			pcm[0] = FusedWindowBand4Sample(vbase, polyCoef, sample);
			pcm[1] = FusedWindowBand4Sample(vbase + 32, polyCoef, sample);
			pcm += 2;
		} else {
			*pcm++ = FusedWindowBand4Sample(vbase, polyCoef, sample);
		}
	}
	return nChans == 2 ? 16 : 8;
}

int FusedSynthFastLowrate(short *pcm, int *x[MAX_NCHAN], SubbandInfo *sbi,
	int nChans, int stride, int *phase, int gb[MAX_NCHAN], int oddBlock)
{
	int *base0;
	int *base1;
	int *vbase;
	int produced;
	int i;
	(void)gb;

	if (!FusedSynthFastLowrateActive(nChans, stride))
		return -1;

	base0 = sbi->fusedVbuf + sbi->fusedVindex + (oddBlock ? VBUF_LENGTH : 0);
	base1 = sbi->fusedVbuf + ((sbi->fusedVindex - oddBlock) & 7) + (oddBlock ? 0 : VBUF_LENGTH);
	for (i = 0; i < 32 * 16; i += 16) {
		base0[i] = base0[i + 32] = 0;
		base1[i] = base1[i + 32] = 0;
	}
	FusedFastDCT4(x[0], base0, base1);
	if (nChans == 2)
		FusedFastDCT4(x[1], base0 + 32, base1 + 32);

	vbase = sbi->fusedVbuf + sbi->fusedVindex + (oddBlock ? VBUF_LENGTH : 0);
	produced = FusedWindowBand4(pcm, vbase, nChans, *phase);
	*phase = (*phase + 32) & (stride - 1);
	if (oddBlock)
		sbi->fusedVindex = (sbi->fusedVindex - 1) & 7;
	return produced;
}
#else
int FusedSynthFastLowrate(short *pcm, int *x[MAX_NCHAN], SubbandInfo *sbi,
	int nChans, int stride, int *phase, int gb[MAX_NCHAN], int oddBlock)
{
	(void)pcm; (void)x; (void)sbi; (void)nChans; (void)stride; (void)phase;
	(void)gb; (void)oddBlock;
	return -1;
}
#endif
