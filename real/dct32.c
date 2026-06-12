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
 * dct32.c - optimized implementations of 32-point DCT for matrixing stage of
 *             polyphase filter
 **************************************************************************************/

#include "coder.h"
#include "assembly.h"

#define COS0_0  0x4013c251	/* Q31 */
#define COS0_1  0x40b345bd	/* Q31 */
#define COS0_2  0x41fa2d6d	/* Q31 */
#define COS0_3  0x43f93421	/* Q31 */
#define COS0_4  0x46cc1bc4	/* Q31 */
#define COS0_5  0x4a9d9cf0	/* Q31 */
#define COS0_6  0x4fae3711	/* Q31 */
#define COS0_7  0x56601ea7	/* Q31 */
#define COS0_8  0x5f4cf6eb	/* Q31 */
#define COS0_9  0x6b6fcf26	/* Q31 */
#define COS0_10 0x7c7d1db3	/* Q31 */
#define COS0_11 0x4ad81a97	/* Q30 */
#define COS0_12 0x5efc8d96	/* Q30 */
#define COS0_13 0x41d95790	/* Q29 */
#define COS0_14 0x6d0b20cf	/* Q29 */
#define COS0_15 0x518522fb	/* Q27 */

#define COS1_0  0x404f4672	/* Q31 */
#define COS1_1  0x42e13c10	/* Q31 */
#define COS1_2  0x48919f44	/* Q31 */
#define COS1_3  0x52cb0e63	/* Q31 */
#define COS1_4  0x64e2402e	/* Q31 */
#define COS1_5  0x43e224a9	/* Q30 */
#define COS1_6  0x6e3c92c1	/* Q30 */
#define COS1_7  0x519e4e04	/* Q28 */

#define COS2_0  0x4140fb46	/* Q31 */
#define COS2_1  0x4cf8de88	/* Q31 */
#define COS2_2  0x73326bbf	/* Q31 */
#define COS2_3  0x52036742	/* Q29 */

#define COS3_0  0x4545e9ef	/* Q31 */
#define COS3_1  0x539eba45	/* Q30 */

#define COS4_0  0x5a82799a	/* Q31 */

// faster in ROM
static const int dcttab[48] = {
	/* first pass */
	COS0_0, COS0_15, COS1_0,	/* 31, 27, 31 */
	COS0_1, COS0_14, COS1_1,	/* 31, 29, 31 */
	COS0_2, COS0_13, COS1_2,	/* 31, 29, 31 */
	COS0_3, COS0_12, COS1_3,	/* 31, 30, 31 */
	COS0_4, COS0_11, COS1_4,	/* 31, 30, 31 */
	COS0_5, COS0_10, COS1_5,	/* 31, 31, 30 */
	COS0_6, COS0_9,  COS1_6,	/* 31, 31, 30 */
	COS0_7, COS0_8,  COS1_7,	/* 31, 31, 28 */
	/* second pass */
	 COS2_0,  COS2_3, COS3_0,	/* 31, 29, 31 */
	 COS2_1,  COS2_2, COS3_1,	/* 31, 31, 30 */
	-COS2_0, -COS2_3, COS3_0, 	/* 31, 29, 31 */
	-COS2_1, -COS2_2, COS3_1, 	/* 31, 31, 30 */
	 COS2_0,  COS2_3, COS3_0, 	/* 31, 29, 31 */
	 COS2_1,  COS2_2, COS3_1, 	/* 31, 31, 30 */
	-COS2_0, -COS2_3, COS3_0, 	/* 31, 29, 31 */
	-COS2_1, -COS2_2, COS3_1, 	/* 31, 31, 30 */
};


#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_FDCT32) && defined(__GNUC__) && \
	(defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || \
	 defined(__mc68060__) || defined(mc68020))
#define FDCT32_HAS_AMIGA_M68K_ASM 1
static const int dct4 = COS4_0;
static __inline int FDCT32_AMIGA_M68K_MULSHIFT32(int x, int y)
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

/*
 * The first radix-4 pass is deliberately one extended-asm scheduling region.
 * Keeping all four live butterfly values in data registers avoids the spills
 * and reloads GCC introduces around 32 separate inline multiply regions on a
 * register-starved 68030.  Coefficients stream once through an address
 * register, and each signed 32x32 multiply consumes its high word directly.
 */
#define FDCT32_M68K_FIRST_BUTTERFLY(in0, in3, in1, in2, out0, out1, out2, out3, s0, s1, s2) \
	"\tmove.l " in0 "(%0),%%d0\n\t" \
	"move.l " in3 "(%0),%%d3\n\t" \
	"move.l %%d0,%%d4\n\t" \
	"add.l %%d3,%%d0\n\t" \
	"sub.l %%d3,%%d4\n\t" \
	"muls.l (%1)+,%%d3:%%d4\n\t" \
	"lsl.l #" s0 ",%%d3\n\t" \
	"move.l " in1 "(%0),%%d1\n\t" \
	"move.l " in2 "(%0),%%d2\n\t" \
	"move.l %%d1,%%d4\n\t" \
	"add.l %%d2,%%d1\n\t" \
	"sub.l %%d2,%%d4\n\t" \
	"muls.l (%1)+,%%d2:%%d4\n\t" \
	"lsl.l #" s1 ",%%d2\n\t" \
	"move.l %%d0,%%d4\n\t" \
	"add.l %%d1,%%d0\n\t" \
	"sub.l %%d1,%%d4\n\t" \
	"muls.l (%1),%%d5:%%d4\n\t" \
	"lsl.l #" s2 ",%%d5\n\t" \
	"move.l %%d3,%%d4\n\t" \
	"sub.l %%d2,%%d4\n\t" \
	"muls.l (%1)+,%%d6:%%d4\n\t" \
	"lsl.l #" s2 ",%%d6\n\t" \
	"add.l %%d3,%%d2\n\t" \
	"move.l %%d0," out0 "(%0)\n\t" \
	"move.l %%d5," out1 "(%0)\n\t" \
	"move.l %%d2," out2 "(%0)\n\t" \
	"move.l %%d6," out3 "(%0)\n\t"

static __inline const int *FDCT32_AMIGA_M68K_FIRST_PASS(int *buf, const int *cptr)
{
	__asm__ volatile (
		FDCT32_M68K_FIRST_BUTTERFLY("",   "124", "60", "64", "",   "60", "64", "124", "1", "5", "1")
		FDCT32_M68K_FIRST_BUTTERFLY("4",  "120", "56", "68", "4",  "56", "68", "120", "1", "3", "1")
		FDCT32_M68K_FIRST_BUTTERFLY("8",  "116", "52", "72", "8",  "52", "72", "116", "1", "3", "1")
		FDCT32_M68K_FIRST_BUTTERFLY("12", "112", "48", "76", "12", "48", "76", "112", "1", "2", "1")
		FDCT32_M68K_FIRST_BUTTERFLY("16", "108", "44", "80", "16", "44", "80", "108", "1", "2", "1")
		FDCT32_M68K_FIRST_BUTTERFLY("20", "104", "40", "84", "20", "40", "84", "104", "1", "1", "2")
		FDCT32_M68K_FIRST_BUTTERFLY("24", "100", "36", "88", "24", "36", "88", "100", "1", "1", "2")
		FDCT32_M68K_FIRST_BUTTERFLY("28", "96",  "32", "92", "28", "32", "92", "96",  "1", "1", "4")
		: "+a" (buf), "+a" (cptr)
		:
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "cc", "memory");
	return cptr;
}
#undef FDCT32_M68K_FIRST_BUTTERFLY

/*
 * Keep the four radix-8 groups in one small loop by default.  Each
 * half-butterfly uses d0-d6 with the same roles, writes its four
 * intermediates back to buf, then the final stage reuses those slots.  This
 * avoids compiler spills while keeping code size down.
 */
#define FDCT32_M68K_SECOND_GROUP(p0, p1, p2, p3, p4, p5, p6, p7) \
	/* even half: a0/a3/a4/a7 */ \
	"\tmove.l " p0 "(%0),%%d0\n\tmove.l " p7 "(%0),%%d1\n" \
	"\tmove.l %%d0,%%d2\n\tadd.l %%d1,%%d0\n\tsub.l %%d1,%%d2\n" \
	"\tmuls.l (%1)+,%%d3:%%d2\n\tlsl.l #1,%%d3\n" \
	"\tmove.l " p3 "(%0),%%d1\n\tmove.l " p4 "(%0),%%d2\n" \
	"\tmove.l %%d1,%%d4\n\tadd.l %%d2,%%d1\n\tsub.l %%d2,%%d4\n" \
	"\tmuls.l (%1)+,%%d2:%%d4\n\tlsl.l #3,%%d2\n" \
	"\tmove.l %%d0,%%d4\n\tsub.l %%d1,%%d4\n" \
	"\tmuls.l (%1),%%d5:%%d4\n\tlsl.l #1,%%d5\n\tadd.l %%d1,%%d0\n" \
	"\tmove.l %%d3,%%d4\n\tsub.l %%d2,%%d4\n" \
	"\tmuls.l (%1)+,%%d6:%%d4\n\tlsl.l #1,%%d6\n\tadd.l %%d3,%%d2\n" \
	"\tmove.l %%d0," p0 "(%0)\n\tmove.l %%d5," p3 "(%0)\n" \
	"\tmove.l %%d2," p4 "(%0)\n\tmove.l %%d6," p7 "(%0)\n" \
	/* odd half: a1/a2/a5/a6 */ \
	"\tmove.l " p1 "(%0),%%d0\n\tmove.l " p6 "(%0),%%d1\n" \
	"\tmove.l %%d0,%%d2\n\tadd.l %%d1,%%d0\n\tsub.l %%d1,%%d2\n" \
	"\tmuls.l (%1)+,%%d3:%%d2\n\tlsl.l #1,%%d3\n" \
	"\tmove.l " p2 "(%0),%%d1\n\tmove.l " p5 "(%0),%%d2\n" \
	"\tmove.l %%d1,%%d4\n\tadd.l %%d2,%%d1\n\tsub.l %%d2,%%d4\n" \
	"\tmuls.l (%1)+,%%d2:%%d4\n\tlsl.l #1,%%d2\n" \
	"\tmove.l %%d0,%%d4\n\tsub.l %%d1,%%d4\n" \
	"\tmuls.l (%1),%%d5:%%d4\n\tlsl.l #2,%%d5\n\tadd.l %%d1,%%d0\n" \
	"\tmove.l %%d3,%%d4\n\tsub.l %%d2,%%d4\n" \
	"\tmuls.l (%1)+,%%d6:%%d4\n\tlsl.l #2,%%d6\n\tadd.l %%d3,%%d2\n" \
	"\tmove.l %%d0," p1 "(%0)\n\tmove.l %%d5," p2 "(%0)\n" \
	"\tmove.l %%d2," p5 "(%0)\n\tmove.l %%d6," p6 "(%0)\n" \
	/* final radix-2 stage */ \
	"\tmove.l " p0 "(%0),%%d0\n\tmove.l " p1 "(%0),%%d1\n" \
	"\tmove.l %%d0,%%d2\n\tsub.l %%d1,%%d2\n\tadd.l %%d1,%%d0\n" \
	"\tmuls.l (%2),%%d3:%%d2\n\tlsl.l #1,%%d3\n" \
	"\tmove.l %%d0," p0 "(%0)\n\tmove.l %%d3," p1 "(%0)\n" \
	"\tmove.l " p2 "(%0),%%d0\n\tmove.l " p3 "(%0),%%d1\n" \
	"\tmove.l %%d1,%%d2\n\tsub.l %%d0,%%d2\n\tadd.l %%d1,%%d0\n" \
	"\tmuls.l (%2),%%d3:%%d2\n\tlsl.l #1,%%d3\n\tadd.l %%d3,%%d0\n" \
	"\tmove.l %%d0," p2 "(%0)\n\tmove.l %%d3," p3 "(%0)\n" \
	"\tmove.l " p4 "(%0),%%d0\n\tmove.l " p5 "(%0),%%d1\n" \
	"\tmove.l %%d0,%%d2\n\tsub.l %%d1,%%d2\n\tadd.l %%d1,%%d0\n" \
	"\tmuls.l (%2),%%d3:%%d2\n\tlsl.l #1,%%d3\n" \
	"\tmove.l %%d0," p4 "(%0)\n\tmove.l %%d3,%%d4\n" \
	"\tmove.l " p6 "(%0),%%d0\n\tmove.l " p7 "(%0),%%d1\n" \
	"\tmove.l %%d1,%%d2\n\tsub.l %%d0,%%d2\n\tadd.l %%d1,%%d0\n" \
	"\tmuls.l (%2),%%d3:%%d2\n\tlsl.l #1,%%d3\n\tadd.l %%d3,%%d0\n" \
	"\tmove.l " p4 "(%0),%%d1\n\tadd.l %%d0,%%d1\n\tmove.l %%d1," p4 "(%0)\n" \
	"\tmove.l %%d4,%%d1\n\tadd.l %%d3,%%d1\n\tmove.l %%d1," p5 "(%0)\n" \
	"\tmove.l %%d4,%%d1\n\tadd.l %%d0,%%d1\n\tmove.l %%d1," p6 "(%0)\n" \
	"\tmove.l %%d3," p7 "(%0)\n"

#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_FDCT32_UNROLL)
/*
 * 68030 speed variant: emit all four second-pass butterfly groups as
 * straight-line code, then fall through to the already-unrolled two-half output
 * shuffle/store path below.  This removes one lea/dbra pair per group and the
 * ~10-cycle taken branch cost.  The tradeoff is roughly +900 bytes of .text
 * versus the compact loop, so check the 68030 I-cache impact on target builds.
 */
static __inline void FDCT32_AMIGA_M68K_SECOND_PASS(int *buf, const int *cptr)
{
	__asm__ volatile (
		FDCT32_M68K_SECOND_GROUP("",   "4",  "8",  "12",  "16",  "20",  "24",  "28")
		FDCT32_M68K_SECOND_GROUP("32", "36", "40", "44",  "48",  "52",  "56",  "60")
		FDCT32_M68K_SECOND_GROUP("64", "68", "72", "76",  "80",  "84",  "88",  "92")
		FDCT32_M68K_SECOND_GROUP("96", "100","104","108", "112", "116", "120", "124")
		: "+a" (buf), "+a" (cptr)
		: "a" (&dct4)
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "cc", "memory");
}
#else
static __inline void FDCT32_AMIGA_M68K_SECOND_PASS(int *buf, const int *cptr)
{
	__asm__ volatile (
		"\tmoveq #3,%%d7\n"
		"1:\n"
		FDCT32_M68K_SECOND_GROUP("", "4", "8", "12", "16", "20", "24", "28")
		"\tlea 32(%0),%0\n\tdbra %%d7,1b\n"
		: "+a" (buf), "+a" (cptr)
		: "a" (&dct4)
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "cc", "memory");
}
#endif
#undef FDCT32_M68K_SECOND_GROUP

#define FDCT32_M68K_PAIR_STORE \
	"\tmove.l %%d0,(%1)\n\tmove.l %%d0,32(%1)\n\tlea 256(%1),%1\n"

/* Two bounded shuffle regions keep d0 as the sample and d1 as the reused sum. */
static __inline void FDCT32_AMIGA_M68K_OUTPUT_HIGH(const int *buf, int *d)
{
	__asm__ volatile (
		"\tmove.l 4(%0),%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 100(%0),%%d1\n\tadd.l 116(%0),%%d1\n"
		"\tmove.l 68(%0),%%d0\n\tadd.l %%d1,%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 36(%0),%%d0\n\tadd.l 52(%0),%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 84(%0),%%d0\n\tadd.l %%d1,%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 20(%0),%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 116(%0),%%d1\n\tadd.l 108(%0),%%d1\n"
		"\tmove.l 84(%0),%%d0\n\tadd.l %%d1,%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 52(%0),%%d0\n\tadd.l 44(%0),%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 76(%0),%%d0\n\tadd.l %%d1,%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 12(%0),%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 108(%0),%%d1\n\tadd.l 124(%0),%%d1\n"
		"\tmove.l 76(%0),%%d0\n\tadd.l %%d1,%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 44(%0),%%d0\n\tadd.l 60(%0),%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 92(%0),%%d0\n\tadd.l %%d1,%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 28(%0),%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 124(%0),%%d1\n"
		"\tmove.l 92(%0),%%d0\n\tadd.l %%d1,%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 60(%0),%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l %%d1,%%d0\n\tmove.l %%d0,(%1)\n\tmove.l %%d0,32(%1)\n"
		: "+a" (buf), "+a" (d)
		:
		: "d0", "d1", "cc", "memory");
}

static __inline void FDCT32_AMIGA_M68K_OUTPUT_LOW(const int *buf, int *d)
{
	__asm__ volatile (
		"\tmove.l 4(%0),%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 120(%0),%%d1\n\tadd.l 100(%0),%%d1\n"
		"\tmove.l 68(%0),%%d0\n\tadd.l %%d1,%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 56(%0),%%d0\n\tadd.l 36(%0),%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 88(%0),%%d0\n\tadd.l %%d1,%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 24(%0),%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 104(%0),%%d1\n\tadd.l 120(%0),%%d1\n"
		"\tmove.l 88(%0),%%d0\n\tadd.l %%d1,%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 40(%0),%%d0\n\tadd.l 56(%0),%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 72(%0),%%d0\n\tadd.l %%d1,%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 8(%0),%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 112(%0),%%d1\n\tadd.l 104(%0),%%d1\n"
		"\tmove.l 72(%0),%%d0\n\tadd.l %%d1,%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 48(%0),%%d0\n\tadd.l 40(%0),%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 80(%0),%%d0\n\tadd.l %%d1,%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 16(%0),%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 96(%0),%%d1\n\tadd.l 112(%0),%%d1\n"
		"\tmove.l 80(%0),%%d0\n\tadd.l %%d1,%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 32(%0),%%d0\n\tadd.l 48(%0),%%d0\n" FDCT32_M68K_PAIR_STORE
		"\tmove.l 64(%0),%%d0\n\tadd.l %%d1,%%d0\n"
		"\tmove.l %%d0,(%1)\n\tmove.l %%d0,32(%1)\n"
		: "+a" (buf), "+a" (d)
		:
		: "d0", "d1", "cc", "memory");
}
#undef FDCT32_M68K_PAIR_STORE
#else
#define FDCT32_HAS_AMIGA_M68K_ASM 0
#endif

#if defined(AMIGA_M68K)
#define FDCT32_MULSHIFT32(x, y) MULSHIFT32_C_REFERENCE((x), (y))
#else
#define FDCT32_MULSHIFT32(x, y) MULSHIFT32((x), (y))
#endif

#define D32FP(i, s0, s1, s2) { \
    a0 = buf[i];			a3 = buf[31-i]; \
	a1 = buf[15-i];			a2 = buf[16+i]; \
    b0 = a0 + a3;			b3 = FDCT32_MULSHIFT32(*cptr++, a0 - a3) << (s0);	\
	b1 = a1 + a2;			b2 = FDCT32_MULSHIFT32(*cptr++, a1 - a2) << (s1);	\
	buf[i] = b0 + b1;		buf[15-i] = FDCT32_MULSHIFT32(*cptr,   b0 - b1) << (s2); \
	buf[16+i] = b2 + b3;    buf[31-i] = FDCT32_MULSHIFT32(*cptr++, b3 - b2) << (s2); \
}

/**************************************************************************************
 * Function:    FDCT32
 *
 * Description: Ken's highly-optimized 32-point DCT (radix-4 + radix-8)
 *
 * Inputs:      input buffer, length = 32 samples
 *              require at least 6 guard bits in input vector x to avoid possibility
 *                of overflow in internal calculations (see bbtest_imdct test app)
 *              buffer offset and oddblock flag for polyphase filter input buffer
 *              number of guard bits in input
 *
 * Outputs:     output buffer, data copied and interleaved for polyphase filter
 *              no guarantees about number of guard bits in output
 *
 * Return:      none
 *
 * Notes:       number of muls = 4*8 + 12*4 = 80
 *              final stage of DCT is hardcoded to shuffle data into the proper order
 *                for the polyphase filterbank
 *              fully unrolled stage 1, for max precision (scale the 1/cos() factors
 *                differently, depending on magnitude)
 *              guard bit analysis verified by exhaustive testing of all 2^32
 *                combinations of max pos/max neg values in x[]
 *
 * TODO:        code organization and optimization for ARM
 *              possibly interleave stereo (cut # of coef loads in half - may not have
 *                enough registers)
 **************************************************************************************/
/* about 1ms faster in RAM */
void FDCT32_C_REFERENCE(int *buf, int *dest, int offset, int oddBlock, int gb)
{
    int i, s, tmp, es, oddBase, evenBase, delayOff, clipBits;
    const int *cptr = dcttab;
    int a0, a1, a2, a3, a4, a5, a6, a7;
    int b0, b1, b2, b3, b4, b5, b6, b7;
	int *d;

	/* scaling - ensure at least 6 guard bits for DCT
	 * (in practice this is already true 99% of time, so this code is
	 *  almost never triggered)
	 */
	es = 0;
	if (gb < 6) {
		es = 6 - gb;
		for (i = 0; i < 32; i++)
			buf[i] >>= es;
	}

	/* first pass */
	D32FP(0, 1, 5, 1);
	D32FP(1, 1, 3, 1);
	D32FP(2, 1, 3, 1);
	D32FP(3, 1, 2, 1);
	D32FP(4, 1, 2, 1);
	D32FP(5, 1, 1, 2);
	D32FP(6, 1, 1, 2);
	D32FP(7, 1, 1, 4);

	/* second pass */
	for (i = 4; i > 0; i--) {
		a0 = buf[0]; 	    a7 = buf[7];		a3 = buf[3];	    a4 = buf[4];
		b0 = a0 + a7;	    b7 = FDCT32_MULSHIFT32(*cptr++, a0 - a7) << 1;
		b3 = a3 + a4;	    b4 = FDCT32_MULSHIFT32(*cptr++, a3 - a4) << 3;
		a0 = b0 + b3;	    a3 = FDCT32_MULSHIFT32(*cptr,   b0 - b3) << 1;
		a4 = b4 + b7;		a7 = FDCT32_MULSHIFT32(*cptr++, b7 - b4) << 1;

		a1 = buf[1];	    a6 = buf[6];	    a2 = buf[2];	    a5 = buf[5];
		b1 = a1 + a6;	    b6 = FDCT32_MULSHIFT32(*cptr++, a1 - a6) << 1;
		b2 = a2 + a5;	    b5 = FDCT32_MULSHIFT32(*cptr++, a2 - a5) << 1;
		a1 = b1 + b2;		a2 = FDCT32_MULSHIFT32(*cptr,   b1 - b2) << 2;
		a5 = b5 + b6;	    a6 = FDCT32_MULSHIFT32(*cptr++, b6 - b5) << 2;

		b0 = a0 + a1;	    b1 = FDCT32_MULSHIFT32(COS4_0, a0 - a1) << 1;
		b2 = a2 + a3;	    b3 = FDCT32_MULSHIFT32(COS4_0, a3 - a2) << 1;
		buf[0] = b0;	    buf[1] = b1;
		buf[2] = b2 + b3;	buf[3] = b3;

		b4 = a4 + a5;	    b5 = FDCT32_MULSHIFT32(COS4_0, a4 - a5) << 1;
		b6 = a6 + a7;	    b7 = FDCT32_MULSHIFT32(COS4_0, a7 - a6) << 1;
		b6 += b7;
		buf[4] = b4 + b6;	buf[5] = b5 + b7;
		buf[6] = b5 + b6;	buf[7] = b7;

		buf += 8;
	}
	buf -= 32;	/* reset */

	oddBase = oddBlock ? VBUF_LENGTH : 0;
	evenBase = oddBlock ? 0 : VBUF_LENGTH;
	delayOff = (offset - oddBlock) & 7;

	/* sample 0 - always delayed one block */
	d = dest + 64*16 + delayOff + evenBase;
	s = buf[ 0];				d[0] = d[8] = s;

	/* samples 16 to 31 */
	d = dest + offset + oddBase;

	s = buf[ 1];				d[0] = d[8] = s;	d += 64;

	tmp = buf[25] + buf[29];
	s = buf[17] + tmp;			d[0] = d[8] = s;	d += 64;
	s = buf[ 9] + buf[13];		d[0] = d[8] = s;	d += 64;
	s = buf[21] + tmp;			d[0] = d[8] = s;	d += 64;

	tmp = buf[29] + buf[27];
	s = buf[ 5];				d[0] = d[8] = s;	d += 64;
	s = buf[21] + tmp;			d[0] = d[8] = s;	d += 64;
	s = buf[13] + buf[11];		d[0] = d[8] = s;	d += 64;
	s = buf[19] + tmp;			d[0] = d[8] = s;	d += 64;

	tmp = buf[27] + buf[31];
	s = buf[ 3];				d[0] = d[8] = s;	d += 64;
	s = buf[19] + tmp;			d[0] = d[8] = s;	d += 64;
	s = buf[11] + buf[15];		d[0] = d[8] = s;	d += 64;
	s = buf[23] + tmp;			d[0] = d[8] = s;	d += 64;

	tmp = buf[31];
	s = buf[ 7];				d[0] = d[8] = s;	d += 64;
	s = buf[23] + tmp;			d[0] = d[8] = s;	d += 64;
	s = buf[15];				d[0] = d[8] = s;	d += 64;
	s = tmp;					d[0] = d[8] = s;

	/* samples 16 to 1 (sample 16 used again) */
	d = dest + 16 + delayOff + evenBase;

	s = buf[ 1];				d[0] = d[8] = s;	d += 64;

	tmp = buf[30] + buf[25];
	s = buf[17] + tmp;			d[0] = d[8] = s;	d += 64;
	s = buf[14] + buf[ 9];		d[0] = d[8] = s;	d += 64;
	s = buf[22] + tmp;			d[0] = d[8] = s;	d += 64;
	s = buf[ 6];				d[0] = d[8] = s;	d += 64;

	tmp = buf[26] + buf[30];
	s = buf[22] + tmp;			d[0] = d[8] = s;	d += 64;
	s = buf[10] + buf[14];		d[0] = d[8] = s;	d += 64;
	s = buf[18] + tmp;			d[0] = d[8] = s;	d += 64;
	s = buf[ 2];				d[0] = d[8] = s;	d += 64;

	tmp = buf[28] + buf[26];
	s = buf[18] + tmp;			d[0] = d[8] = s;	d += 64;
	s = buf[12] + buf[10];		d[0] = d[8] = s;	d += 64;
	s = buf[20] + tmp;			d[0] = d[8] = s;	d += 64;
	s = buf[ 4];				d[0] = d[8] = s;	d += 64;

	tmp = buf[24] + buf[28];
	s = buf[20] + tmp;			d[0] = d[8] = s;	d += 64;
	s = buf[ 8] + buf[12];		d[0] = d[8] = s;	d += 64;
	s = buf[16] + tmp;			d[0] = d[8] = s;

	/* this is so rarely invoked that it's not worth making two versions of the output
	 *   shuffle code (one for no shift, one for clip + variable shift) like in IMDCT
	 * here we just load, clip, shift, and store on the rare instances that es != 0
	 */
	if (es) {
		clipBits = 31 - es;
		d = dest + 64*16 + delayOff + evenBase;
		s = d[0];	CLIP_2N(s, clipBits);	d[0] = d[8] = (s << es);

		d = dest + offset + oddBase;
		for (i = 16; i <= 31; i++) {
			s = d[0];	CLIP_2N(s, clipBits);	d[0] = d[8] = (s << es);	d += 64;
		}

		d = dest + 16 + delayOff + evenBase;
		for (i = 15; i >= 0; i--) {
			s = d[0];	CLIP_2N(s, clipBits);	d[0] = d[8] = (s << es);	d += 64;
		}
	}
}

/**************************************************************************************
 * Function:    FDCT32Half
 *
 * Description: compute only the 16 synthesis rows consumed by stride-2 output
 *
 * Notes:       the even PCM samples depend only on the even polyphase rows.  Those
 *              rows are built entirely from the low 16 values of the factored DCT,
 *              so the high half and two of the four first-pass multiplies per group
 *              can be skipped while remaining bit-identical to FDCT32.
 **************************************************************************************/
#if FDCT32_HAS_AMIGA_M68K_ASM
#define FDCT32_HALF_MULSHIFT32(x, y) FDCT32_AMIGA_M68K_MULSHIFT32((x), (y))
#else
#define FDCT32_HALF_MULSHIFT32(x, y) FDCT32_MULSHIFT32((x), (y))
#endif

void FDCT32Half(int *buf, int *dest, int offset, int oddBlock, int gb)
{
	int i, s, es, oddBase, evenBase, delayOff, clipBits;
	const int *cptr = dcttab;
	int a0, a1, a2, a3, a4, a5, a6, a7;
	int b0, b1, b2, b3, b4, b5, b6, b7;
	int *d;
	static const unsigned char firstPassShift[8] = { 1, 1, 1, 1, 1, 2, 2, 4 };

	es = 0;
	if (gb < 6) {
		es = 6 - gb;
		for (i = 0; i < 32; i++)
			buf[i] >>= es;
	}

	/* The low half needs only symmetric sums from the 32 input subbands. */
	for (i = 0; i < 8; i++) {
		a0 = buf[i];
		a3 = buf[31 - i];
		a1 = buf[15 - i];
		a2 = buf[16 + i];
		b0 = a0 + a3;
		b1 = a1 + a2;
		buf[i] = b0 + b1;
		buf[15 - i] = FDCT32_HALF_MULSHIFT32(cptr[2], b0 - b1) << firstPassShift[i];
		cptr += 3;
	}

	/* Only the first two radix-8 groups feed even polyphase rows. */
	for (i = 2; i > 0; i--) {
		a0 = buf[0];		a7 = buf[7];		a3 = buf[3];		a4 = buf[4];
		b0 = a0 + a7;		b7 = FDCT32_HALF_MULSHIFT32(*cptr++, a0 - a7) << 1;
		b3 = a3 + a4;		b4 = FDCT32_HALF_MULSHIFT32(*cptr++, a3 - a4) << 3;
		a0 = b0 + b3;		a3 = FDCT32_HALF_MULSHIFT32(*cptr, b0 - b3) << 1;
		a4 = b4 + b7;		a7 = FDCT32_HALF_MULSHIFT32(*cptr++, b7 - b4) << 1;

		a1 = buf[1];		a6 = buf[6];		a2 = buf[2];		a5 = buf[5];
		b1 = a1 + a6;		b6 = FDCT32_HALF_MULSHIFT32(*cptr++, a1 - a6) << 1;
		b2 = a2 + a5;		b5 = FDCT32_HALF_MULSHIFT32(*cptr++, a2 - a5) << 1;
		a1 = b1 + b2;		a2 = FDCT32_HALF_MULSHIFT32(*cptr, b1 - b2) << 2;
		a5 = b5 + b6;		a6 = FDCT32_HALF_MULSHIFT32(*cptr++, b6 - b5) << 2;

		b0 = a0 + a1;		b1 = FDCT32_HALF_MULSHIFT32(COS4_0, a0 - a1) << 1;
		b2 = a2 + a3;		b3 = FDCT32_HALF_MULSHIFT32(COS4_0, a3 - a2) << 1;
		buf[0] = b0;		buf[1] = b1;
		buf[2] = b2 + b3;	buf[3] = b3;

		b4 = a4 + a5;		b5 = FDCT32_HALF_MULSHIFT32(COS4_0, a4 - a5) << 1;
		b6 = a6 + a7;		b7 = FDCT32_HALF_MULSHIFT32(COS4_0, a7 - a6) << 1;
		b6 += b7;
		buf[4] = b4 + b6;	buf[5] = b5 + b7;
		buf[6] = b5 + b6;	buf[7] = b7;
		buf += 8;
	}
	buf -= 16;

	oddBase = oddBlock ? VBUF_LENGTH : 0;
	evenBase = oddBlock ? 0 : VBUF_LENGTH;
	delayOff = (offset - oddBlock) & 7;

#define FDCT32_HALF_STORE(value) do { \
	s = (value); \
	if (es) { clipBits = 31 - es; CLIP_2N(s, clipBits); s <<= es; } \
	d[0] = d[8] = s; \
} while (0)

	/* Even sample 0, then even samples 16..30. */
	d = dest + 64 * 16 + delayOff + evenBase;
	FDCT32_HALF_STORE(buf[0]);
	d = dest + offset + oddBase;
	FDCT32_HALF_STORE(buf[1]); d += 128;
	FDCT32_HALF_STORE(buf[9] + buf[13]); d += 128;
	FDCT32_HALF_STORE(buf[5]); d += 128;
	FDCT32_HALF_STORE(buf[13] + buf[11]); d += 128;
	FDCT32_HALF_STORE(buf[3]); d += 128;
	FDCT32_HALF_STORE(buf[11] + buf[15]); d += 128;
	FDCT32_HALF_STORE(buf[7]); d += 128;
	FDCT32_HALF_STORE(buf[15]);

	/* Even samples 16..2; sample 16 is reused on this side of the FIFO. */
	d = dest + 16 + delayOff + evenBase;
	FDCT32_HALF_STORE(buf[1]); d += 128;
	FDCT32_HALF_STORE(buf[14] + buf[9]); d += 128;
	FDCT32_HALF_STORE(buf[6]); d += 128;
	FDCT32_HALF_STORE(buf[10] + buf[14]); d += 128;
	FDCT32_HALF_STORE(buf[2]); d += 128;
	FDCT32_HALF_STORE(buf[12] + buf[10]); d += 128;
	FDCT32_HALF_STORE(buf[4]); d += 128;
	FDCT32_HALF_STORE(buf[8] + buf[12]);

#undef FDCT32_HALF_STORE
}


/**************************************************************************************
 * Function:    FDCT32Quarter
 *
 * Description: compute the 8-point lossy DCT used by stride-4 fast-lowrate output
 *
 * Notes:       this intentionally uses only subbands 0..7 and scatters only the
 *              quarter-rate rows consumed by the stride-4 polyphase path.
 **************************************************************************************/
void FDCT32Quarter(int *buf, int *dest, int offset, int oddBlock, int gb)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_FDCT32_QUARTER)
	int i, s, es, oddBase, evenBase, delayOff, clipBits;
	const int *cptr = dcttab;
	int a0, a1, a2, a3, a4, a5, a6, a7;
	int b0, b1, b2, b3, b4, b5, b6, b7;
	int *d;
	static const unsigned char firstPassShift[8] = { 1, 1, 1, 1, 1, 2, 2, 4 };

	es = 0;
	if (gb < 6) {
		es = 6 - gb;
		for (i = 0; i < 32; i++)
			buf[i] >>= es;
	}

	/* Match FDCT32Half's symmetric first pass so quarter-rate rows stay
	 * in the libhelix FDCT32 pipeline.
	 */
	for (i = 0; i < 8; i++) {
		a0 = buf[i];
		a3 = buf[31 - i];
		a1 = buf[15 - i];
		a2 = buf[16 + i];
		b0 = a0 + a3;
		b1 = a1 + a2;
		buf[i] = b0 + b1;
		buf[15 - i] = FDCT32_HALF_MULSHIFT32(cptr[2], b0 - b1) << firstPassShift[i];
		cptr += 3;
	}

	/* Run only FDCT32Half's first radix-8 group; buf[8..15] feed rows
	 * that stride-4 output never consumes.
	 */
	a0 = buf[0];		a7 = buf[7];		a3 = buf[3];		a4 = buf[4];
	b0 = a0 + a7;		b7 = FDCT32_HALF_MULSHIFT32(*cptr++, a0 - a7) << 1;
	b3 = a3 + a4;		b4 = FDCT32_HALF_MULSHIFT32(*cptr++, a3 - a4) << 3;
	a0 = b0 + b3;		a3 = FDCT32_HALF_MULSHIFT32(*cptr, b0 - b3) << 1;
	a4 = b4 + b7;		a7 = FDCT32_HALF_MULSHIFT32(*cptr++, b7 - b4) << 1;

	a1 = buf[1];		a6 = buf[6];		a2 = buf[2];		a5 = buf[5];
	b1 = a1 + a6;		b6 = FDCT32_HALF_MULSHIFT32(*cptr++, a1 - a6) << 1;
	b2 = a2 + a5;		b5 = FDCT32_HALF_MULSHIFT32(*cptr++, a2 - a5) << 1;
	a1 = b1 + b2;		a2 = FDCT32_HALF_MULSHIFT32(*cptr, b1 - b2) << 2;
	a5 = b5 + b6;		a6 = FDCT32_HALF_MULSHIFT32(*cptr++, b6 - b5) << 2;

	b0 = a0 + a1;		b1 = FDCT32_HALF_MULSHIFT32(COS4_0, a0 - a1) << 1;
	b2 = a2 + a3;		b3 = FDCT32_HALF_MULSHIFT32(COS4_0, a3 - a2) << 1;
	buf[0] = b0;		buf[1] = b1;
	buf[2] = b2 + b3;	buf[3] = b3;

	b4 = a4 + a5;		b5 = FDCT32_HALF_MULSHIFT32(COS4_0, a4 - a5) << 1;
	b6 = a6 + a7;		b7 = FDCT32_HALF_MULSHIFT32(COS4_0, a7 - a6) << 1;
	b6 += b7;
	buf[4] = b4 + b6;	buf[5] = b5 + b7;
	buf[6] = b5 + b6;	buf[7] = b7;

	oddBase = oddBlock ? VBUF_LENGTH : 0;
	evenBase = oddBlock ? 0 : VBUF_LENGTH;
	delayOff = (offset - oddBlock) & 7;

#define FDCT32_HALF_STORE(value) do { \
	s = (value); \
	if (es) { clipBits = 31 - es; CLIP_2N(s, clipBits); s <<= es; } \
	d[0] = d[8] = s; \
} while (0)

	/* Clear all rows the quarter-rate FIFO ranges can expose before scattering. */
	d = dest + offset + oddBase;
	for (i = 0; i < 16; i++) {
		d[0] = d[8] = 0;
		d += 64;
	}
	d = dest + 16 + delayOff + evenBase;
	for (i = 0; i < 16; i++) {
		d[0] = d[8] = 0;
		d += 64;
	}

	d = dest + 64 * 16 + delayOff + evenBase;
	FDCT32_HALF_STORE(buf[0]);

	d = dest + offset + oddBase;
	FDCT32_HALF_STORE(buf[1]);

	d = dest + 16 + delayOff + evenBase;
	FDCT32_HALF_STORE(buf[1]);

	d = dest + 16 + delayOff + evenBase + 64 * 4;
	FDCT32_HALF_STORE(buf[2]);

	d = dest + offset + oddBase + 64 * 4;
	FDCT32_HALF_STORE(buf[3]);

	d = dest + 16 + delayOff + evenBase + 64 * 6;
	FDCT32_HALF_STORE(buf[4]);

	d = dest + offset + oddBase + 64 * 2;
	FDCT32_HALF_STORE(buf[5]);

	d = dest + 16 + delayOff + evenBase + 64 * 2;
	FDCT32_HALF_STORE(buf[6]);

	d = dest + offset + oddBase + 64 * 6;
	FDCT32_HALF_STORE(buf[7]);

#undef FDCT32_HALF_STORE
#else
	(void)buf;
	(void)dest;
	(void)offset;
	(void)oddBlock;
	(void)gb;
#endif
}

#undef FDCT32_HALF_MULSHIFT32

#if FDCT32_HAS_AMIGA_M68K_ASM
#undef FDCT32_MULSHIFT32
#define FDCT32_MULSHIFT32(x, y) FDCT32_AMIGA_M68K_MULSHIFT32((x), (y))

static void FDCT32_AMIGA_M68K_ASM(int *buf, int *dest, int offset, int oddBlock, int gb)
{
    int i, s, es, oddBase, evenBase, delayOff, clipBits;
    const int *cptr = dcttab;
	int *d;

	/* scaling - ensure at least 6 guard bits for DCT
	 * (in practice this is already true 99% of time, so this code is
	 *  almost never triggered)
	 */
	es = 0;
	if (gb < 6) {
		es = 6 - gb;
		for (i = 0; i < 32; i++)
			buf[i] >>= es;
	}

	/* first pass: register-scheduled 68030 assembly kernel */
	cptr = FDCT32_AMIGA_M68K_FIRST_PASS(buf, cptr);

	/* second pass: compact register-scheduled loop, including final radix-2 */
	FDCT32_AMIGA_M68K_SECOND_PASS(buf, cptr);

	oddBase = oddBlock ? VBUF_LENGTH : 0;
	evenBase = oddBlock ? 0 : VBUF_LENGTH;
	delayOff = (offset - oddBlock) & 7;

	/* sample 0 - always delayed one block */
	d = dest + 64*16 + delayOff + evenBase;
	s = buf[ 0];				d[0] = d[8] = s;

	/* samples 16 to 31 */
	d = dest + offset + oddBase;

	FDCT32_AMIGA_M68K_OUTPUT_HIGH(buf, d);

	/* samples 16 to 1 (sample 16 used again) */
	d = dest + 16 + delayOff + evenBase;

	FDCT32_AMIGA_M68K_OUTPUT_LOW(buf, d);

	/* this is so rarely invoked that it's not worth making two versions of the output
	 *   shuffle code (one for no shift, one for clip + variable shift) like in IMDCT
	 * here we just load, clip, shift, and store on the rare instances that es != 0
	 */
	if (es) {
		clipBits = 31 - es;
		d = dest + 64*16 + delayOff + evenBase;
		s = d[0];	CLIP_2N(s, clipBits);	d[0] = d[8] = (s << es);

		d = dest + offset + oddBase;
		for (i = 16; i <= 31; i++) {
			s = d[0];	CLIP_2N(s, clipBits);	d[0] = d[8] = (s << es);	d += 64;
		}

		d = dest + 16 + delayOff + evenBase;
		for (i = 15; i >= 0; i--) {
			s = d[0];	CLIP_2N(s, clipBits);	d[0] = d[8] = (s << es);	d += 64;
		}
	}
}
#endif

int FDCT32_HAS_AMIGA_M68K_ASM_RUNTIME(void)
{
	return FDCT32_HAS_AMIGA_M68K_ASM;
}

void FDCT32(int *buf, int *dest, int offset, int oddBlock, int gb)
{
#if FDCT32_HAS_AMIGA_M68K_ASM
	FDCT32_AMIGA_M68K_ASM(buf, dest, offset, oddBlock, gb);
#else
	FDCT32_C_REFERENCE(buf, dest, offset, oddBlock, gb);
#endif
}
