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
 * July 2003
 *
 * huffman.c - Huffman decoding of transform coefficients
 **************************************************************************************/

#include "coder.h"
#if defined(AMIGA_M68K) || \
	(defined(__GNUC__) && (defined(__mc68000__) || defined(__mc68020__) || defined(mc68000)))
#include "assembly.h"
#else
static __inline unsigned int LOADBE16(const unsigned char *buf)
{
	return ((unsigned int)buf[0] << 8) | (unsigned int)buf[1];
}
#endif
#include "string.h"

/* helper macros - see comments in hufftabs.c about the format of the huffman tables */
#define GetMaxbits(x)   ((int)( (((unsigned short)(x)) >>  0) & 0x000f))
#define GetHLen(x)      ((int)( (((unsigned short)(x)) >> 12) & 0x000f))
#define GetCWY(x)       ((int)( (((unsigned short)(x)) >>  8) & 0x000f))
#define GetCWX(x)       ((int)( (((unsigned short)(x)) >>  4) & 0x000f))
#define GetSignBits(x)  ((int)( (((unsigned short)(x)) >>  0) & 0x000f))

#define GetHLenQ(x)     ((int)( (((unsigned char)(x)) >> 4) & 0x0f))
#define GetCWVQ(x)      ((int)( (((unsigned char)(x)) >> 3) & 0x01))
#define GetCWWQ(x)      ((int)( (((unsigned char)(x)) >> 2) & 0x01))
#define GetCWXQ(x)      ((int)( (((unsigned char)(x)) >> 1) & 0x01))
#define GetCWYQ(x)      ((int)( (((unsigned char)(x)) >> 0) & 0x01))

/* apply sign of s to the positive number x (save in MSB, will do two's complement in dequant) */
#define ApplySign(x, s)	{ (x) |= ((s) & 0x80000000); }

#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_HUFFMAN) && defined(__GNUC__) && \
	(defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || \
	 defined(__mc68060__) || defined(mc68020))
#define HUFFMAN_PAIRS_HAS_AMIGA_M68K_ASM 1
#else
#define HUFFMAN_PAIRS_HAS_AMIGA_M68K_ASM 0
#endif

#if HUFFMAN_PAIRS_HAS_AMIGA_M68K_ASM
static int gExperimentalHuffmanEnabled;

void MP3SetExperimentalHuffman(int enabled)
{
	gExperimentalHuffmanEnabled = enabled ? 1 : 0;
}

int MP3ExperimentalHuffmanEnabled(void)
{
	return gExperimentalHuffmanEnabled;
}
#else
void MP3SetExperimentalHuffman(int enabled)
{
	(void)enabled;
}

int MP3ExperimentalHuffmanEnabled(void)
{
	return 0;
}
#endif

#if HUFFMAN_PAIRS_HAS_AMIGA_M68K_ASM
static __inline unsigned int HuffmanLoadBE16_AMIGA_M68K_ASM(unsigned char **buf, int bitsLeft)
{
	unsigned int value;

	if (bitsLeft >= 32) {
		unsigned char *p = *buf;
		__asm__ volatile (
			"move.l (%1)+,%0\n\t"
			"lea -2(%1),%1\n\t"
			"swap %0\n\t"
			"and.l #0xffff,%0"
			: "=&d" (value), "+a" (p)
			:
			: "memory");
		*buf = p;
		return value;
	}

	value = LOADBE16(*buf);
	*buf += 2;
	return value;
}
#define HUFFMAN_LOADBE16_FAST(bufPtr, bitsLeft) HuffmanLoadBE16_AMIGA_M68K_ASM(&(bufPtr), (bitsLeft))
#else
#define HUFFMAN_LOADBE16_FAST(bufPtr, bitsLeft) LOADBE16(bufPtr)
#endif

#if HUFFMAN_PAIRS_HAS_AMIGA_M68K_ASM
/* GetHLen/GetCWX/GetCWY pull 4-bit fields out of the 16-bit codeword using a
 * generic shift+mask.  cw is passed in as a zero-extended unsigned int, so
 * the codeword occupies bits 15:0 of the register and its three nibbles sit
 * at bit-field offsets 16, 20, and 24 counted from the register MSB (offset
 * 0).  A single bfextu replaces the shift+and pair GCC would otherwise emit
 * at every codeword visited in the table walk.  Purely an arithmetic
 * substitution: it changes no control flow and produces the identical value
 * as the GetHLen/GetCWX/GetCWY macros for every possible cw, so it is wired
 * in only on the runtime-selectable experimental path (useAsmRefill) to keep
 * DecodeHuffmanPairs_C_REFERENCE as the untouched portable reference for
 * --selftest-huffman.
 */
static __inline unsigned int GetHLen_AMIGA_M68K_ASM(unsigned int cw)
{
	unsigned int result;
	__asm__ volatile ("bfextu %1{#16:#4},%0" : "=d" (result) : "d" (cw) : "cc");
	return result;
}
static __inline unsigned int GetCWY_AMIGA_M68K_ASM(unsigned int cw)
{
	unsigned int result;
	__asm__ volatile ("bfextu %1{#20:#4},%0" : "=d" (result) : "d" (cw) : "cc");
	return result;
}
static __inline unsigned int GetCWX_AMIGA_M68K_ASM(unsigned int cw)
{
	unsigned int result;
	__asm__ volatile ("bfextu %1{#24:#4},%0" : "=d" (result) : "d" (cw) : "cc");
	return result;
}
#define HUFF_GET_HLEN(cw, useAsm) ((useAsm) ? (int)GetHLen_AMIGA_M68K_ASM((unsigned int)(cw)) : GetHLen(cw))
#define HUFF_GET_CWX(cw, useAsm)  ((useAsm) ? (int)GetCWX_AMIGA_M68K_ASM((unsigned int)(cw))  : GetCWX(cw))
#define HUFF_GET_CWY(cw, useAsm)  ((useAsm) ? (int)GetCWY_AMIGA_M68K_ASM((unsigned int)(cw))  : GetCWY(cw))
#define HUFFMAN_PAIRS_FAST_NOTE_FIELDS " + m68k bfextu codeword field extract"
#else
#define HUFF_GET_HLEN(cw, useAsm) GetHLen(cw)
#define HUFF_GET_CWX(cw, useAsm)  GetCWX(cw)
#define HUFF_GET_CWY(cw, useAsm)  GetCWY(cw)
#define HUFFMAN_PAIRS_FAST_NOTE_FIELDS ""
#endif

#if HUFFMAN_PAIRS_HAS_AMIGA_M68K_ASM && MULSHIFT32_HAS_AMIGA_M68K_ASM
#define HUFFMAN_PAIRS_FAST_NOTE "m68k inline move.l pair/quad refill + m68k MULSHIFT32 available" HUFFMAN_PAIRS_FAST_NOTE_FIELDS
#elif HUFFMAN_PAIRS_HAS_AMIGA_M68K_ASM
#define HUFFMAN_PAIRS_FAST_NOTE "m68k inline move.l pair/quad refill available" HUFFMAN_PAIRS_FAST_NOTE_FIELDS
#else
#define HUFFMAN_PAIRS_FAST_NOTE "no (C reference path only in this build)"
#endif

/**************************************************************************************
 * Function:    DecodeHuffmanPairs
 *
 * Description: decode 2-way vector Huffman codes in the "bigValues" region of spectrum
 *
 * Inputs:      valid BitStreamInfo struct, pointing to start of pair-wise codes
 *              pointer to xy buffer to received decoded values
 *              number of codewords to decode
 *              index of Huffman table to use
 *              number of bits remaining in bitstream
 *
 * Outputs:     pairs of decoded coefficients in vwxy
 *              updated BitStreamInfo struct
 *
 * Return:      number of bits used, or -1 if out of bits
 *
 * Notes:       assumes that nVals is an even number
 *              si_huff.bit tests every Huffman codeword in every table (though not
 *                necessarily all linBits outputs for x,y > 15)
 **************************************************************************************/
// no improvement with section=data
static int DecodeHuffmanPairs_Impl(int *xy, int nVals, int tabIdx, int bitsLeft, unsigned char *buf, int bitOffset, int useAsmRefill)
{
	int x, y;
	int cachedBits, padBits, len, startBits, linBits, maxBits, minBits;
	int rootMaxBits, curMaxBits;
	HuffTabType tabType;
	unsigned short cw, *tBase, *tCurr;
	unsigned int cache;

	if(nVals <= 0) 
		return 0;

	if (bitsLeft < 0)
		return -1;
	startBits = bitsLeft;

	tBase = (unsigned short *)(huffTable + huffTabOffset[tabIdx]);
	linBits = huffTabLookup[tabIdx].linBits;
	tabType = huffTabLookup[tabIdx].tabType;

	ASSERT(!(nVals & 0x01));
	ASSERT(tabIdx < HUFF_PAIRTABS);
	ASSERT(tabIdx >= 0);
	ASSERT(tabType != invalidTab);

	/* initially fill cache with any partial byte */
	cache = 0;
	cachedBits = (8 - bitOffset) & 0x07;
	if (cachedBits)
		cache = (unsigned int)(*buf++) << (32 - cachedBits);
	bitsLeft -= cachedBits;

	if (tabType == noBits) {
		/* table 0 consumes no data and produces only zero coefficients. */
		memset(xy, 0, nVals * sizeof(*xy));
		return 0;
	} else if (tabType == oneShot) {
		/* single lookup, no escapes */
		maxBits = GetMaxbits(tBase[0]);
		tBase++;
		padBits = 0;
		while (nVals > 0) {
			/* refill cache - assumes cachedBits <= 16 */
			if (bitsLeft >= 16) {
				/* load 2 new bytes into left-justified cache */
				if (useAsmRefill) {
					cache |= HUFFMAN_LOADBE16_FAST(buf, bitsLeft) << (16 - cachedBits);
				} else {
					cache |= LOADBE16(buf) << (16 - cachedBits);
					buf += 2;
				}
				cachedBits += 16;
				bitsLeft -= 16;
			} else {
				/* last time through, pad cache with zeros and drain cache */
				if (cachedBits + bitsLeft <= 0)	return -1;
				if (bitsLeft > 0)	cache |= (unsigned int)(*buf++) << (24 - cachedBits);
				if (bitsLeft > 8)	cache |= (unsigned int)(*buf++) << (16 - cachedBits);
				cachedBits += bitsLeft;
				bitsLeft = 0;

				cache &= (signed int)0x80000000 >> (cachedBits - 1);
				padBits = 11;
				cachedBits += padBits;	/* okay if this is > 32 (0's automatically shifted in from right) */
			}

			/* largest maxBits = 9, plus 2 for sign bits, so make sure cache has at least 11 bits */
			while (nVals > 0 && cachedBits >= 11 ) {
				cw = tBase[cache >> (32 - maxBits)];
				len = HUFF_GET_HLEN(cw, useAsmRefill);
				cachedBits -= len;
				cache <<= len;

				x = HUFF_GET_CWX(cw, useAsmRefill);	if (x)	{ApplySign(x, cache); cache <<= 1; cachedBits--;}
				y = HUFF_GET_CWY(cw, useAsmRefill);	if (y)	{ApplySign(y, cache); cache <<= 1; cachedBits--;}

				/* ran out of real bits - clamp to padded zero bits rather than
				 * reporting a bad Huffman stream. Some valid encoders leave the
				 * final big_values codeword ending in the zero-padded tail.
				 */
				if (cachedBits < padBits)
					cachedBits = padBits;

				*xy++ = x;
				*xy++ = y;
				nVals -= 2;
			}
		}
		bitsLeft += (cachedBits - padBits);
		return (startBits - bitsLeft);
	} else if (tabType == loopNoLinbits) {
		/*
		 * The no-linbits tables never carry escape values.  Keep their common decode
		 * loop free of the two per-pair tabType/escape tests needed by the
		 * linbits tables; those branches are comparatively expensive on m68k.
		 */
		tCurr = tBase;
		padBits = 0;
		/*
		 * The root metadata word tBase[0] is re-read on every symbol in the
		 * original walk because tCurr is reset to tBase after each pair.  Hoist
		 * its maxBits and carry the current level's maxBits in curMaxBits: the
		 * common root level then needs no tCurr[0] load, and a subtable jump
		 * loads the sub-level maxBits once.  curMaxBits and tCurr persist across
		 * a mid-symbol refill, so the walk resumes correctly.
		 */
		rootMaxBits = GetMaxbits(tBase[0]);
		curMaxBits = rootMaxBits;
		while (nVals > 0) {
			/* refill cache - assumes cachedBits <= 16 */
			if (bitsLeft >= 16) {
				if (useAsmRefill) {
					cache |= HUFFMAN_LOADBE16_FAST(buf, bitsLeft) << (16 - cachedBits);
				} else {
					cache |= LOADBE16(buf) << (16 - cachedBits);
					buf += 2;
				}
				cachedBits += 16;
				bitsLeft -= 16;
			} else {
				if (cachedBits + bitsLeft <= 0)	return -1;
				if (bitsLeft > 0)	cache |= (unsigned int)(*buf++) << (24 - cachedBits);
				if (bitsLeft > 8)	cache |= (unsigned int)(*buf++) << (16 - cachedBits);
				cachedBits += bitsLeft;
				bitsLeft = 0;

				cache &= (signed int)0x80000000 >> (cachedBits - 1);
				padBits = 11;
				cachedBits += padBits;
			}

			while (nVals > 0 && cachedBits >= 11) {
				cw = tCurr[(cache >> (32 - curMaxBits)) + 1];
				len = HUFF_GET_HLEN(cw, useAsmRefill);
				if (!len) {
					cachedBits -= curMaxBits;
					cache <<= curMaxBits;
					tCurr += cw;
					curMaxBits = GetMaxbits(tCurr[0]);
					continue;
				}
				cachedBits -= len;
				cache <<= len;

				x = HUFF_GET_CWX(cw, useAsmRefill);	if (x)	{ApplySign(x, cache); cache <<= 1; cachedBits--;}
				y = HUFF_GET_CWY(cw, useAsmRefill);	if (y)	{ApplySign(y, cache); cache <<= 1; cachedBits--;}

				/* See oneShot case: do not turn a zero-padded tail into
				 * ERR_MP3_INVALID_HUFFCODES.
				 */
				if (cachedBits < padBits)
					cachedBits = padBits;

				*xy++ = x;
				*xy++ = y;
				nVals -= 2;
				tCurr = tBase;
				curMaxBits = rootMaxBits;
			}
		}
		bitsLeft += (cachedBits - padBits);
		return (startBits - bitsLeft);
	} else if (tabType == loopLinbits) {
		tCurr = tBase;
		padBits = 0;
		/* Same root-metadata hoist as loopNoLinbits: see comment above. */
		rootMaxBits = GetMaxbits(tBase[0]);
		curMaxBits = rootMaxBits;
		while (nVals > 0) {
			/* refill cache - assumes cachedBits <= 16 */
			if (bitsLeft >= 16) {
				/* load 2 new bytes into left-justified cache */
				if (useAsmRefill) {
					cache |= HUFFMAN_LOADBE16_FAST(buf, bitsLeft) << (16 - cachedBits);
				} else {
					cache |= LOADBE16(buf) << (16 - cachedBits);
					buf += 2;
				}
				cachedBits += 16;
				bitsLeft -= 16;
			} else {
				/* last time through, pad cache with zeros and drain cache */
				if (cachedBits + bitsLeft <= 0)	return -1;
				if (bitsLeft > 0)	cache |= (unsigned int)(*buf++) << (24 - cachedBits);
				if (bitsLeft > 8)	cache |= (unsigned int)(*buf++) << (16 - cachedBits);
				cachedBits += bitsLeft;
				bitsLeft = 0;

				cache &= (signed int)0x80000000 >> (cachedBits - 1);
				padBits = 11;
				cachedBits += padBits;	/* okay if this is > 32 (0's automatically shifted in from right) */
			}

			/* largest maxBits = 9, plus 2 for sign bits, so make sure cache has at least 11 bits */
			while (nVals > 0 && cachedBits >= 11 ) {
				cw = tCurr[(cache >> (32 - curMaxBits)) + 1];
				len = HUFF_GET_HLEN(cw, useAsmRefill);
				if (!len) {
					cachedBits -= curMaxBits;
					cache <<= curMaxBits;
					tCurr += cw;
					curMaxBits = GetMaxbits(tCurr[0]);
					continue;
				}
				cachedBits -= len;
				cache <<= len;

				x = HUFF_GET_CWX(cw, useAsmRefill);
				y = HUFF_GET_CWY(cw, useAsmRefill);

				if (x == 15) {
					minBits = linBits + 1 + (y ? 1 : 0);
					if (cachedBits + bitsLeft < minBits)
						return -1;
					while (cachedBits < minBits) {
						cache |= (unsigned int)(*buf++) << (24 - cachedBits);
						cachedBits += 8;
						bitsLeft -= 8;
					}
					if (bitsLeft < 0) {
						cachedBits += bitsLeft;
						bitsLeft = 0;
						cache &= (signed int)0x80000000 >> (cachedBits - 1);
					}
					x += (int)(cache >> (32 - linBits));
					cachedBits -= linBits;
					cache <<= linBits;
				}
				if (x)	{ApplySign(x, cache); cache <<= 1; cachedBits--;}

				if (y == 15) {
					minBits = linBits + 1;
					if (cachedBits + bitsLeft < minBits)
						return -1;
					while (cachedBits < minBits) {
						cache |= (unsigned int)(*buf++) << (24 - cachedBits);
						cachedBits += 8;
						bitsLeft -= 8;
					}
					if (bitsLeft < 0) {
						cachedBits += bitsLeft;
						bitsLeft = 0;
						cache &= (signed int)0x80000000 >> (cachedBits - 1);
					}
					y += (int)(cache >> (32 - linBits));
					cachedBits -= linBits;
					cache <<= linBits;
				}
				if (y)	{ApplySign(y, cache); cache <<= 1; cachedBits--;}

				/* ran out of real bits - clamp to padded zero bits rather than
				 * reporting a bad Huffman stream. Some valid encoders leave the
				 * final big_values codeword ending in the zero-padded tail.
				 */
				if (cachedBits < padBits)
					cachedBits = padBits;

				*xy++ = x;
				*xy++ = y;
				nVals -= 2;
				tCurr = tBase;
				curMaxBits = rootMaxBits;
			}
		}
		bitsLeft += (cachedBits - padBits);
		return (startBits - bitsLeft);
	}

	/* error in bitstream - trying to access unused Huffman table */
	return -1;
}

int DecodeHuffmanPairs_C_REFERENCE(int *xy, int nVals, int tabIdx, int bitsLeft, unsigned char *buf, int bitOffset)
{
	return DecodeHuffmanPairs_Impl(xy, nVals, tabIdx, bitsLeft, buf, bitOffset, 0);
}

int DecodeHuffmanPairs_TEST_ACTIVE(int *xy, int nVals, int tabIdx, int bitsLeft, unsigned char *buf, int bitOffset)
{
#if HUFFMAN_PAIRS_HAS_AMIGA_M68K_ASM
	return DecodeHuffmanPairs_Impl(xy, nVals, tabIdx, bitsLeft, buf, bitOffset, 1);
#else
	return DecodeHuffmanPairs_C_REFERENCE(xy, nVals, tabIdx, bitsLeft, buf, bitOffset);
#endif
}

int DecodeHuffmanPairs_HAS_AMIGA_M68K_ASM_RUNTIME(void)
{
	return HUFFMAN_PAIRS_HAS_AMIGA_M68K_ASM;
}

const char *DecodeHuffmanPairs_AMIGA_M68K_ASM_NOTE(void)
{
	return HUFFMAN_PAIRS_FAST_NOTE;
}

/**************************************************************************************
 * Function:    DecodeHuffmanQuads
 *
 * Description: decode 4-way vector Huffman codes in the "count1" region of spectrum
 *
 * Inputs:      valid BitStreamInfo struct, pointing to start of quadword codes
 *              pointer to vwxy buffer to received decoded values
 *              maximum number of codewords to decode
 *              index of quadword table (0 = table A, 1 = table B)
 *              number of bits remaining in bitstream
 *
 * Outputs:     quadruples of decoded coefficients in vwxy
 *              updated BitStreamInfo struct
 *
 * Return:      index of the first "zero_part" value (index of the first sample 
 *                of the quad word after which all samples are 0)
 * 
 * Notes:        si_huff.bit tests every vwxy output in both quad tables
 **************************************************************************************/
// no improvement with section=data
static int DecodeHuffmanQuads(int *vwxy, int nVals, int tabIdx, int bitsLeft, unsigned char *buf, int bitOffset)
{
	int i, v, w, x, y;
	int len, maxBits, cachedBits, padBits;
	unsigned int cache;
	unsigned char cw, *tBase;

	if (bitsLeft <= 0)
		return 0;

	tBase = (unsigned char *)quadTable + quadTabOffset[tabIdx];
	maxBits = quadTabMaxBits[tabIdx];

	/* initially fill cache with any partial byte */
	cache = 0;
	cachedBits = (8 - bitOffset) & 0x07;
	if (cachedBits)
		cache = (unsigned int)(*buf++) << (32 - cachedBits);
	bitsLeft -= cachedBits;

	i = padBits = 0;
	while (i < (nVals - 3)) {
		/* refill cache - assumes cachedBits <= 16 */
		if (bitsLeft >= 16) {
			/* load 2 new bytes into left-justified cache */
#if HUFFMAN_PAIRS_HAS_AMIGA_M68K_ASM
			cache |= HUFFMAN_LOADBE16_FAST(buf, bitsLeft) << (16 - cachedBits);
#else
			cache |= LOADBE16(buf) << (16 - cachedBits);
			buf += 2;
#endif
			cachedBits += 16;
			bitsLeft -= 16;
		} else {
			/* last time through, pad cache with zeros and drain cache */
			if (cachedBits + bitsLeft <= 0) return i;
			if (bitsLeft > 0)	cache |= (unsigned int)(*buf++) << (24 - cachedBits);
			if (bitsLeft > 8)	cache |= (unsigned int)(*buf++) << (16 - cachedBits);
			cachedBits += bitsLeft;
			bitsLeft = 0;

			cache &= (signed int)0x80000000 >> (cachedBits - 1);
			padBits = 10;
			cachedBits += padBits;	/* okay if this is > 32 (0's automatically shifted in from right) */
		}

		/* largest maxBits = 6, plus 4 for sign bits, so make sure cache has at least 10 bits */
		while (i < (nVals - 3) && cachedBits >= 10 ) {
			cw = tBase[cache >> (32 - maxBits)];
			len = GetHLenQ(cw);
			cachedBits -= len;
			cache <<= len;

			v = GetCWVQ(cw);	if(v) {ApplySign(v, cache); cache <<= 1; cachedBits--;}
			w = GetCWWQ(cw);	if(w) {ApplySign(w, cache); cache <<= 1; cachedBits--;}
			x = GetCWXQ(cw);	if(x) {ApplySign(x, cache); cache <<= 1; cachedBits--;}
			y = GetCWYQ(cw);	if(y) {ApplySign(y, cache); cache <<= 1; cachedBits--;}

			/* ran out of bits - okay (means we're done) */
			if (cachedBits <= padBits)
				return i;

			*vwxy++ = v;
			*vwxy++ = w;
			*vwxy++ = x;
			*vwxy++ = y;
			i += 4;
		}
	}

	/* decoded max number of quad values */
	return i;
}

/**************************************************************************************
 * Function:    DecodeHuffman
 *
 * Description: decode one granule, one channel worth of Huffman codes
 *
 * Inputs:      MP3DecInfo structure filled by UnpackFrameHeader(), UnpackSideInfo(),
 *                and UnpackScaleFactors() (for this granule)
 *              buffer pointing to start of Huffman data in MP3 frame
 *              pointer to bit offset (0-7) indicating starting bit in buf[0]
 *              number of bits in the Huffman data section of the frame
 *                (could include padding bits)
 *              index of current granule and channel
 *
 * Outputs:     decoded coefficients in hi->huffDecBuf[ch] (hi pointer in mp3DecInfo)
 *              updated bitOffset
 *
 * Return:      length (in bytes) of Huffman codes
 *              bitOffset also returned in parameter (0 = MSB, 7 = LSB of 
 *                byte located at buf + offset)
 *              -1 if null input pointers, huffBlockBits < 0, or decoder runs 
 *                out of bits prematurely (invalid bitstream)
 **************************************************************************************/
// .data about 1ms faster per frame
int DecodeHuffman(MP3DecInfo *mp3DecInfo, unsigned char *buf, int *bitOffset, int huffBlockBits, int gr, int ch)
{
	int r1Start, r2Start, rEnd[4];	/* region boundaries */
	int i, w, bitsUsed, bitsLeft;
	unsigned char *startBuf = buf;

	FrameHeader *fh;
	SideInfo *si;
	SideInfoSub *sis;
	HuffmanInfo *hi;

	/* validate pointers */
	if (!mp3DecInfo || !mp3DecInfo->FrameHeaderPS || !mp3DecInfo->SideInfoPS || !mp3DecInfo->ScaleFactorInfoPS || !mp3DecInfo->HuffmanInfoPS)
		return -1;

	fh = ((FrameHeader *)(mp3DecInfo->FrameHeaderPS));
	si = ((SideInfo *)(mp3DecInfo->SideInfoPS));
	sis = &si->sis[gr][ch];
	hi = (HuffmanInfo*)(mp3DecInfo->HuffmanInfoPS);

	if (huffBlockBits < 0)
		return -1;

	/* Pure mid/side joint stereo represents mono output entirely in channel 0.
	 * Side info already supplies the exact channel payload length, so advance
	 * over channel 1 rather than decoding coefficients Dequantize() discards.
	 * This removes a major bitrate-sensitive cost from mono playback. */
	if (gr >= 0 && gr < mp3DecInfo->nGrans &&
		mp3DecInfo->monoMSSideSkipGranule[gr] && ch == 1) {
		hi->nonZeroBound[ch] = 0;
		buf += (huffBlockBits + *bitOffset) >> 3;
		*bitOffset = (huffBlockBits + *bitOffset) & 0x07;
		return (buf - startBuf);
	}

	/* figure out region boundaries (the first 2*bigVals coefficients divided into 3 regions) */
	if (sis->winSwitchFlag && sis->blockType == 2) {
		if (sis->mixedBlock == 0) {
			r1Start = fh->sfBand->s[(sis->region0Count + 1)/3] * 3;
		} else {
			if (fh->ver == MPEG1) {
				r1Start = fh->sfBand->l[sis->region0Count + 1];
			} else {
				/* see MPEG2 spec for explanation */
				w = fh->sfBand->s[4] - fh->sfBand->s[3];
				r1Start = fh->sfBand->l[6] + 2*w;
			}
		}
		r2Start = MAX_NSAMP;	/* short blocks don't have region 2 */
	} else {
		r1Start = fh->sfBand->l[sis->region0Count + 1];
		r2Start = fh->sfBand->l[sis->region0Count + 1 + sis->region1Count + 1];
	}

	/* offset rEnd index by 1 so first region = rEnd[1] - rEnd[0], etc. */
	rEnd[3] = MIN(MAX_NSAMP, 2 * sis->nBigvals);
	rEnd[2] = MIN(r2Start, rEnd[3]);
	rEnd[1] = MIN(r1Start, rEnd[3]);
	rEnd[0] = 0;

	/* rounds up to first all-zero pair (we don't check last pair for (x,y) == (non-zero, zero)) */
	hi->nonZeroBound[ch] = rEnd[3];

	/* decode Huffman pairs (rEnd[i] are always even numbers) */
	bitsLeft = huffBlockBits;
	for (i = 0; i < 3; i++) {
		if (MP3ExperimentalHuffmanEnabled())
			bitsUsed = DecodeHuffmanPairs_TEST_ACTIVE(hi->huffDecBuf[ch] + rEnd[i], rEnd[i+1] - rEnd[i], sis->tableSelect[i], bitsLeft, buf, *bitOffset);
		else
			bitsUsed = DecodeHuffmanPairs_C_REFERENCE(hi->huffDecBuf[ch] + rEnd[i], rEnd[i+1] - rEnd[i], sis->tableSelect[i], bitsLeft, buf, *bitOffset);
		if (bitsUsed < 0 || bitsUsed > bitsLeft)	/* error - overran end of bitstream */
			return -1;

		/* update bitstream position */
		buf += (bitsUsed + *bitOffset) >> 3;
		*bitOffset = (bitsUsed + *bitOffset) & 0x07;
		bitsLeft -= bitsUsed;
	}

	/* decode Huffman quads (if any) */
	hi->nonZeroBound[ch] += DecodeHuffmanQuads(hi->huffDecBuf[ch] + rEnd[3], MAX_NSAMP - rEnd[3], sis->count1TableSelect, bitsLeft, buf, *bitOffset);

	ASSERT(hi->nonZeroBound[ch] <= MAX_NSAMP);
	memset(hi->huffDecBuf[ch] + hi->nonZeroBound[ch], 0,
		(MAX_NSAMP - hi->nonZeroBound[ch]) * sizeof(hi->huffDecBuf[ch][0]));
	
	/* If bits used for 576 samples < huffBlockBits, then the extras are considered
	 *  to be stuffing bits (throw away, but need to return correct bitstream position) 
	 */
	buf += (bitsLeft + *bitOffset) >> 3;
	*bitOffset = (bitsLeft + *bitOffset) & 0x07;
	
	return (buf - startBuf);
}
