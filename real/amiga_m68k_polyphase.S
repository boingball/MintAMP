/* Experimental Amiga/m68k 68030 mono fast polyphase synthesis.
 * Assembled only for explicit AMIGA_M68K_ASM_POLYPHASE builds.
 * The assembler macros below expand to one straight-line, fully unrolled
 * 32-sample mono synthesis kernel with register accumulators and muls.l high
 * products; there are no per-sample C calls or inner tap loops in the object.
 */

#if defined(AMIGA_M68K_ASM_POLYPHASE)
	.text
	.even
	.globl	AmigaM68KPolyphaseMonoFast
	.globl	AmigaM68KPolyphaseMonoFastStride2
	.globl	MonoFastPolyphaseStride4_Amiga_m68k
	.globl	StereoFastPolyphaseStride2Phase0_Amiga_m68k
	.globl	StereoFastPolyphaseStride4Half_Amiga_m68k
	.globl	StereoFastPolyphaseStride4Phase0_Amiga_m68k
	.globl	StereoFastPolyphaseStride4Phase1_Amiga_m68k
	.globl	StereoFastPolyphaseStride4Phase2_Amiga_m68k
	.globl	StereoFastPolyphaseStride4Phase3_Amiga_m68k
	.globl	StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts
	.globl	StereoFastPolyphaseStride5_Amiga_m68k
	.globl	StereoFastPolyphaseStride5_Amiga_m68k_PhaseCounts
/* void AmigaM68KPolyphaseMonoFast(short *pcm, int *vbuf, const int *coefBase)
 * 4(%sp)  = pcm
 * 8(%sp)  = vbuf
 * 12(%sp) = coefBase
 */
AmigaM68KPolyphaseMonoFast:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0
	move.l	52(%sp),%a1
	move.l	56(%sp),%a2

.macro PROD_ADD voff, coff, acc
	move.l	\voff(%a1),%d0
	move.l	\coff(%a2),%d1
	asl.l	#6,%d1
	muls.l	%d1,%d2:%d0
	add.l	%d2,\acc
.endm

.macro PROD_SUB voff, coff, acc
	move.l	\voff(%a1),%d0
	move.l	\coff(%a2),%d1
	asl.l	#6,%d1
	muls.l	%d1,%d2:%d0
	sub.l	%d2,\acc
.endm

.macro CLIP_STORE acc, poff
	move.l	\acc,%d0
	move.l	%d0,%d1
	asr.l	#8,%d1
	asr.l	#7,%d1		| d1 = acc >> 15
	move.l	%d0,%d2
	asr.l	#8,%d2
	asr.l	#8,%d2
	asr.l	#8,%d2
	asr.l	#7,%d2		| d2 = acc >> 31 (sign)
	cmp.l	%d1,%d2		| if sign == x>>15, no clip needed
	beq.s	990f
	move.l	%d2,%d0
	eori.l	#0x00007fff,%d0
990:
	move.w	%d0,\poff(%a0)
.endm

.macro SAMPLE0
	clr.l	%d3
	PROD_ADD 0,0,%d3
	PROD_SUB 92,4,%d3
	PROD_ADD 4,8,%d3
	PROD_SUB 88,12,%d3
	PROD_ADD 8,16,%d3
	PROD_SUB 84,20,%d3
	PROD_ADD 12,24,%d3
	PROD_SUB 80,28,%d3
	PROD_ADD 16,32,%d3
	PROD_SUB 76,36,%d3
	PROD_ADD 20,40,%d3
	PROD_SUB 72,44,%d3
	PROD_ADD 24,48,%d3
	PROD_SUB 68,52,%d3
	PROD_ADD 28,56,%d3
	PROD_SUB 64,60,%d3
	CLIP_STORE %d3,0
.endm

.macro SAMPLE16
	clr.l	%d3
	PROD_ADD 4096,1024,%d3
	PROD_ADD 4100,1028,%d3
	PROD_ADD 4104,1032,%d3
	PROD_ADD 4108,1036,%d3
	PROD_ADD 4112,1040,%d3
	PROD_ADD 4116,1044,%d3
	PROD_ADD 4120,1048,%d3
	PROD_ADD 4124,1052,%d3
	CLIP_STORE %d3,32
.endm

.macro SAMPLE16_AT poff
	clr.l	%d3
	PROD_ADD 4096,1024,%d3
	PROD_ADD 4100,1028,%d3
	PROD_ADD 4104,1032,%d3
	PROD_ADD 4108,1036,%d3
	PROD_ADD 4112,1040,%d3
	PROD_ADD 4116,1044,%d3
	PROD_ADD 4120,1048,%d3
	PROD_ADD 4124,1052,%d3
	CLIP_STORE %d3,\poff
.endm

.macro PAIR_LO pair, poff
	.set VB, (\pair * 256)
	.set CF, (\pair * 64)
	clr.l	%d3
	PROD_ADD VB+0,CF+0,%d3
	PROD_SUB VB+92,CF+4,%d3
	PROD_ADD VB+4,CF+8,%d3
	PROD_SUB VB+88,CF+12,%d3
	PROD_ADD VB+8,CF+16,%d3
	PROD_SUB VB+84,CF+20,%d3
	PROD_ADD VB+12,CF+24,%d3
	PROD_SUB VB+80,CF+28,%d3
	PROD_ADD VB+16,CF+32,%d3
	PROD_SUB VB+76,CF+36,%d3
	PROD_ADD VB+20,CF+40,%d3
	PROD_SUB VB+72,CF+44,%d3
	PROD_ADD VB+24,CF+48,%d3
	PROD_SUB VB+68,CF+52,%d3
	PROD_ADD VB+28,CF+56,%d3
	PROD_SUB VB+64,CF+60,%d3
	CLIP_STORE %d3,\poff
.endm

.macro PAIR_HI pair, poff
	.set VB, (\pair * 256)
	.set CF, (\pair * 64)
	clr.l	%d3
	PROD_ADD VB+0,CF+4,%d3
	PROD_ADD VB+92,CF+0,%d3
	PROD_ADD VB+4,CF+12,%d3
	PROD_ADD VB+88,CF+8,%d3
	PROD_ADD VB+8,CF+20,%d3
	PROD_ADD VB+84,CF+16,%d3
	PROD_ADD VB+12,CF+28,%d3
	PROD_ADD VB+80,CF+24,%d3
	PROD_ADD VB+16,CF+36,%d3
	PROD_ADD VB+76,CF+32,%d3
	PROD_ADD VB+20,CF+44,%d3
	PROD_ADD VB+72,CF+40,%d3
	PROD_ADD VB+24,CF+52,%d3
	PROD_ADD VB+68,CF+48,%d3
	PROD_ADD VB+28,CF+60,%d3
	PROD_ADD VB+64,CF+56,%d3
	CLIP_STORE %d3,\poff
.endm

.macro PAIR pair
	.set VB, (\pair * 256)
	.set CF, (\pair * 64)
	clr.l	%d3
	clr.l	%d4
	PROD_ADD VB+0,CF+0,%d3
	PROD_ADD VB+0,CF+4,%d4
	PROD_SUB VB+92,CF+4,%d3
	PROD_ADD VB+92,CF+0,%d4
	PROD_ADD VB+4,CF+8,%d3
	PROD_ADD VB+4,CF+12,%d4
	PROD_SUB VB+88,CF+12,%d3
	PROD_ADD VB+88,CF+8,%d4
	PROD_ADD VB+8,CF+16,%d3
	PROD_ADD VB+8,CF+20,%d4
	PROD_SUB VB+84,CF+20,%d3
	PROD_ADD VB+84,CF+16,%d4
	PROD_ADD VB+12,CF+24,%d3
	PROD_ADD VB+12,CF+28,%d4
	PROD_SUB VB+80,CF+28,%d3
	PROD_ADD VB+80,CF+24,%d4
	PROD_ADD VB+16,CF+32,%d3
	PROD_ADD VB+16,CF+36,%d4
	PROD_SUB VB+76,CF+36,%d3
	PROD_ADD VB+76,CF+32,%d4
	PROD_ADD VB+20,CF+40,%d3
	PROD_ADD VB+20,CF+44,%d4
	PROD_SUB VB+72,CF+44,%d3
	PROD_ADD VB+72,CF+40,%d4
	PROD_ADD VB+24,CF+48,%d3
	PROD_ADD VB+24,CF+52,%d4
	PROD_SUB VB+68,CF+52,%d3
	PROD_ADD VB+68,CF+48,%d4
	PROD_ADD VB+28,CF+56,%d3
	PROD_ADD VB+28,CF+60,%d4
	PROD_SUB VB+64,CF+60,%d3
	PROD_ADD VB+64,CF+56,%d4
	CLIP_STORE %d3,(\pair * 2)
	CLIP_STORE %d4,((32 - \pair) * 2)
.endm

	SAMPLE0
	SAMPLE16
	PAIR 1
	PAIR 2
	PAIR 3
	PAIR 4
	PAIR 5
	PAIR 6
	PAIR 7
	PAIR 8
	PAIR 9
	PAIR 10
	PAIR 11
	PAIR 12
	PAIR 13
	PAIR 14
	PAIR 15

	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts


/* void AmigaM68KPolyphaseMonoFastStride2(short *pcm, int *vbuf, const int *coefBase)
 * Emits the 16 stride-2 samples contiguously in pcm[0]..pcm[15].
 */
AmigaM68KPolyphaseMonoFastStride2:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0
	move.l	52(%sp),%a1
	move.l	56(%sp),%a2

	/* fastLowrateStride2Samples selects even samples:
	 * 0, 2, 4, ..., 14, 16, 18, ..., 30.
	 * PAIR indices are the actual polyphase sample numbers, so the
	 * low half uses pairs 2/4/6/8/10/12/14 and the high half mirrors
	 * those as 14/12/10/8/6/4/2 while storing contiguously.
	 */
	SAMPLE0
	PAIR_LO 2,2
	PAIR_LO 4,4
	PAIR_LO 6,6
	PAIR_LO 8,8
	PAIR_LO 10,10
	PAIR_LO 12,12
	PAIR_LO 14,14
	SAMPLE16_AT 16
	PAIR_HI 14,18
	PAIR_HI 12,20
	PAIR_HI 10,22
	PAIR_HI 8,24
	PAIR_HI 6,26
	PAIR_HI 4,28
	PAIR_HI 2,30

	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts


/* void MonoFastPolyphaseStride4_Amiga_m68k(short *pcm, int *vbuf, const int *coefBase)
 * Emits stride-4 phase-0 samples 0,4,8,12,16,20,24,28 contiguously
 * in pcm[0]..pcm[7].  Keep the coefficient offsets identical to the
 * straight-line PAIR_LO/PAIR_HI kernels above: pair P uses coefBase +
 * P*64 bytes (P*16 ints), while vbuf uses P*256 bytes (P*64 ints).
 */
MonoFastPolyphaseStride4_Amiga_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0		| pcm
	move.l	52(%sp),%a1		| vbuf
	move.l	56(%sp),%a2		| coefBase

	/* Matches fastLowrateStride4Samples[0] in polyphase.c:
	 * 0, 4, 8, 12, 16, 20, 24, 28.
	 */
	SAMPLE0
	PAIR_LO 4,2
	PAIR_LO 8,4
	PAIR_LO 12,6
	SAMPLE16_AT 8
	PAIR_HI 12,10
	PAIR_HI 8,12
	PAIR_HI 4,14

	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts


/* Stereo stride-4 polyphase functions
 * Each outputs 8 stereo pairs (16 shorts LRLRLR...) into pcm.
 * Register layout:
 *   %a0 = pcm
 *   %a1 = vbuf (L base)
 *   %a2 = coefBase
 *   %d3 = L accumulator
 *   %d5 = R accumulator
 *   %d0, %d1, %d2 = scratch
 * C keeps each stereo row as 32 left ints followed by 32 right ints.
 * For row/pair P, L base is vbuf + 64*P ints and R base is
 * vbuf + 64*P + 32 ints, so the right-channel byte offset from the
 * left row base is 128, not a whole 512-int half-buffer.
 */

.macro STEREO_PROD_ADD voff, coff, lacc, racc
	move.l	\voff(%a1),%d0
	move.l	\coff(%a2),%d1
	asl.l	#6,%d1
	muls.l	%d1,%d2:%d0
	add.l	%d2,\lacc
	move.l	\voff+128(%a1),%d0
	muls.l	%d1,%d2:%d0
	add.l	%d2,\racc
.endm

.macro STEREO_PROD_SUB voff, coff, lacc, racc
	move.l	\voff(%a1),%d0
	move.l	\coff(%a2),%d1
	asl.l	#6,%d1
	muls.l	%d1,%d2:%d0
	sub.l	%d2,\lacc
	move.l	\voff+128(%a1),%d0
	muls.l	%d1,%d2:%d0
	sub.l	%d2,\racc
.endm

.macro STEREO_SAMPLE0 poff
	clr.l	%d3
	clr.l	%d5
	STEREO_PROD_ADD 0,0,%d3,%d5
	STEREO_PROD_SUB 92,4,%d3,%d5
	STEREO_PROD_ADD 4,8,%d3,%d5
	STEREO_PROD_SUB 88,12,%d3,%d5
	STEREO_PROD_ADD 8,16,%d3,%d5
	STEREO_PROD_SUB 84,20,%d3,%d5
	STEREO_PROD_ADD 12,24,%d3,%d5
	STEREO_PROD_SUB 80,28,%d3,%d5
	STEREO_PROD_ADD 16,32,%d3,%d5
	STEREO_PROD_SUB 76,36,%d3,%d5
	STEREO_PROD_ADD 20,40,%d3,%d5
	STEREO_PROD_SUB 72,44,%d3,%d5
	STEREO_PROD_ADD 24,48,%d3,%d5
	STEREO_PROD_SUB 68,52,%d3,%d5
	STEREO_PROD_ADD 28,56,%d3,%d5
	STEREO_PROD_SUB 64,60,%d3,%d5
	CLIP_STORE %d3,\poff
	CLIP_STORE %d5,\poff+2
.endm

.macro STEREO_SAMPLE16 poff
	clr.l	%d3
	clr.l	%d5
	STEREO_PROD_ADD 4096,1024,%d3,%d5
	STEREO_PROD_ADD 4100,1028,%d3,%d5
	STEREO_PROD_ADD 4104,1032,%d3,%d5
	STEREO_PROD_ADD 4108,1036,%d3,%d5
	STEREO_PROD_ADD 4112,1040,%d3,%d5
	STEREO_PROD_ADD 4116,1044,%d3,%d5
	STEREO_PROD_ADD 4120,1048,%d3,%d5
	STEREO_PROD_ADD 4124,1052,%d3,%d5
	CLIP_STORE %d3,\poff
	CLIP_STORE %d5,\poff+2
.endm

.macro STEREO_PAIR_LO pair, poff
	.set VB, (\pair * 256)
	.set CF, (\pair * 64)
	clr.l	%d3
	clr.l	%d5
	STEREO_PROD_ADD VB+0,CF+0,%d3,%d5
	STEREO_PROD_SUB VB+92,CF+4,%d3,%d5
	STEREO_PROD_ADD VB+4,CF+8,%d3,%d5
	STEREO_PROD_SUB VB+88,CF+12,%d3,%d5
	STEREO_PROD_ADD VB+8,CF+16,%d3,%d5
	STEREO_PROD_SUB VB+84,CF+20,%d3,%d5
	STEREO_PROD_ADD VB+12,CF+24,%d3,%d5
	STEREO_PROD_SUB VB+80,CF+28,%d3,%d5
	STEREO_PROD_ADD VB+16,CF+32,%d3,%d5
	STEREO_PROD_SUB VB+76,CF+36,%d3,%d5
	STEREO_PROD_ADD VB+20,CF+40,%d3,%d5
	STEREO_PROD_SUB VB+72,CF+44,%d3,%d5
	STEREO_PROD_ADD VB+24,CF+48,%d3,%d5
	STEREO_PROD_SUB VB+68,CF+52,%d3,%d5
	STEREO_PROD_ADD VB+28,CF+56,%d3,%d5
	STEREO_PROD_SUB VB+64,CF+60,%d3,%d5
	CLIP_STORE %d3,\poff
	CLIP_STORE %d5,\poff+2
.endm

.macro STEREO_PAIR_HI pair, poff
	.set VB, (\pair * 256)
	.set CF, (\pair * 64)
	clr.l	%d3
	clr.l	%d5
	STEREO_PROD_ADD VB+0,CF+4,%d3,%d5
	STEREO_PROD_ADD VB+92,CF+0,%d3,%d5
	STEREO_PROD_ADD VB+4,CF+12,%d3,%d5
	STEREO_PROD_ADD VB+88,CF+8,%d3,%d5
	STEREO_PROD_ADD VB+8,CF+20,%d3,%d5
	STEREO_PROD_ADD VB+84,CF+16,%d3,%d5
	STEREO_PROD_ADD VB+12,CF+28,%d3,%d5
	STEREO_PROD_ADD VB+80,CF+24,%d3,%d5
	STEREO_PROD_ADD VB+16,CF+36,%d3,%d5
	STEREO_PROD_ADD VB+76,CF+32,%d3,%d5
	STEREO_PROD_ADD VB+20,CF+44,%d3,%d5
	STEREO_PROD_ADD VB+72,CF+40,%d3,%d5
	STEREO_PROD_ADD VB+24,CF+52,%d3,%d5
	STEREO_PROD_ADD VB+68,CF+48,%d3,%d5
	STEREO_PROD_ADD VB+28,CF+60,%d3,%d5
	STEREO_PROD_ADD VB+64,CF+56,%d3,%d5
	CLIP_STORE %d3,\poff
	CLIP_STORE %d5,\poff+2
.endm


/* void StereoFastPolyphaseStride2Phase0_Amiga_m68k(short *pcm, int *vbuf, const int *coefBase)
 * Caller contract: stride-2 low-rate synthesis never advances away from phase 0
 * because 32 %% 2 == 0.  This single phase emits logical sample rows
 * 0,2,4,...,30 as 16 contiguous interleaved stereo pairs:
 * L0,R0,L2,R2,...,L30,R30 (32 shorts total).
 */
StereoFastPolyphaseStride2Phase0_Amiga_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0		| pcm
	move.l	52(%sp),%a1		| vbuf
	move.l	56(%sp),%a2		| coefBase

	STEREO_SAMPLE0 0
	STEREO_PAIR_LO 2,4
	STEREO_PAIR_LO 4,8
	STEREO_PAIR_LO 6,12
	STEREO_PAIR_LO 8,16
	STEREO_PAIR_LO 10,20
	STEREO_PAIR_LO 12,24
	STEREO_PAIR_LO 14,28
	STEREO_SAMPLE16 32
	STEREO_PAIR_HI 14,36
	STEREO_PAIR_HI 12,40
	STEREO_PAIR_HI 10,44
	STEREO_PAIR_HI 8,48
	STEREO_PAIR_HI 6,52
	STEREO_PAIR_HI 4,56
	STEREO_PAIR_HI 2,60

	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts


/* void StereoFastPolyphaseStride4Half_Amiga_m68k(short *pcm, int *vbuf, const int *coefBase, int phase)
 * Dispatches the stride-4 half-rate stereo phase to the exact phase-specific
 * kernels below.  The phase-specific kernels preserve the C reference sample
 * order and left/right channel addressing; no mono-only symmetry is assumed.
 * The assembly kernel suffixes describe the compact sample lists in
 * fastLowrateStride4Samples[], so logical phase N dispatches to PhaseN.
 */
StereoFastPolyphaseStride4Half_Amiga_m68k:
	move.l	16(%sp),%d0		| phase (before callee register save)
	cmp.l	#3,%d0
	bhi.s	0f
	lea	StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts,%a0
	move.l	%d0,%d1
	lsl.l	#2,%d1
	addq.l	#1,(%a0,%d1.l)
0:
	tst.l	%d0
	beq	StereoFastPolyphaseStride4Phase0_Amiga_m68k
	cmp.l	#1,%d0
	beq	StereoFastPolyphaseStride4Phase1_Amiga_m68k
	cmp.l	#2,%d0
	beq	StereoFastPolyphaseStride4Phase2_Amiga_m68k
	cmp.l	#3,%d0
	beq	StereoFastPolyphaseStride4Phase3_Amiga_m68k
	rts


/* void StereoFastPolyphaseStride4Phase0_Amiga_m68k(short *pcm, int *vbuf, const int *coefBase)
 * Outputs stride-4 phase-0 samples: 0, 4, 8, 12, 16, 20, 24, 28
 */
StereoFastPolyphaseStride4Phase0_Amiga_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0		| pcm
	move.l	52(%sp),%a1		| vbuf
	move.l	56(%sp),%a2		| coefBase

	STEREO_SAMPLE0 0
	STEREO_PAIR_LO 4,4
	STEREO_PAIR_LO 8,8
	STEREO_PAIR_LO 12,12
	STEREO_SAMPLE16 16
	STEREO_PAIR_HI 12,20
	STEREO_PAIR_HI 8,24
	STEREO_PAIR_HI 4,28

	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts


/* void StereoFastPolyphaseStride4Phase1_Amiga_m68k(short *pcm, int *vbuf, const int *coefBase)
 * Outputs stride-4 phase-1 samples: 3, 7, 11, 15, 19, 23, 27, 31
 */
StereoFastPolyphaseStride4Phase1_Amiga_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0		| pcm
	move.l	52(%sp),%a1		| vbuf
	move.l	56(%sp),%a2		| coefBase

	STEREO_PAIR_LO 3,0
	STEREO_PAIR_LO 7,4
	STEREO_PAIR_LO 11,8
	STEREO_PAIR_LO 15,12
	STEREO_PAIR_HI 13,16
	STEREO_PAIR_HI 9,20
	STEREO_PAIR_HI 5,24
	STEREO_PAIR_HI 1,28

	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts


/* void StereoFastPolyphaseStride4Phase2_Amiga_m68k(short *pcm, int *vbuf, const int *coefBase)
 * Outputs stride-4 phase-2 samples: 2, 6, 10, 14, 18, 22, 26, 30
 */
StereoFastPolyphaseStride4Phase2_Amiga_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0		| pcm
	move.l	52(%sp),%a1		| vbuf
	move.l	56(%sp),%a2		| coefBase

	STEREO_PAIR_LO 2,0
	STEREO_PAIR_LO 6,4
	STEREO_PAIR_LO 10,8
	STEREO_PAIR_LO 14,12
	STEREO_PAIR_HI 14,16
	STEREO_PAIR_HI 10,20
	STEREO_PAIR_HI 6,24
	STEREO_PAIR_HI 2,28

	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts


/* void StereoFastPolyphaseStride4Phase3_Amiga_m68k(short *pcm, int *vbuf, const int *coefBase)
 * Outputs stride-4 phase-3 samples: 1, 5, 9, 13, 17, 21, 25, 29
 */
StereoFastPolyphaseStride4Phase3_Amiga_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0		| pcm
	move.l	52(%sp),%a1		| vbuf
	move.l	56(%sp),%a2		| coefBase

	STEREO_PAIR_LO 1,0
	STEREO_PAIR_LO 5,4
	STEREO_PAIR_LO 9,8
	STEREO_PAIR_LO 13,12
	STEREO_PAIR_HI 15,16
	STEREO_PAIR_HI 11,20
	STEREO_PAIR_HI 7,24
	STEREO_PAIR_HI 3,28

	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts


/* int StereoFastPolyphaseStride5_Amiga_m68k(short *pcm, int *vbuf, const int *coefBase, int phase)
 * Stride-5 (8820/8287 Hz) stereo fast-lowrate synthesis.  Dispatches the five
 * logical phases to phase-specific kernels that reproduce the C reference
 * PolyphaseStereoFastLowrateStride5 sample lists and left/right addressing
 * exactly.  Returns the number of emitted interleaved shorts in %d0: 14 for
 * phases 0 and 4 (7 stereo pairs), 12 for phases 1, 2 and 3 (6 stereo pairs).
 */
StereoFastPolyphaseStride5_Amiga_m68k:
	move.l	16(%sp),%d0		| phase (before callee register save)
	cmp.l	#4,%d0
	bhi.s	0f
	lea	StereoFastPolyphaseStride5_Amiga_m68k_PhaseCounts,%a0
	move.l	%d0,%d1
	lsl.l	#2,%d1
	addq.l	#1,(%a0,%d1.l)
0:
	tst.l	%d0
	beq	StereoFastPolyphaseStride5Phase0_Amiga_m68k
	cmp.l	#1,%d0
	beq	StereoFastPolyphaseStride5Phase1_Amiga_m68k
	cmp.l	#2,%d0
	beq	StereoFastPolyphaseStride5Phase2_Amiga_m68k
	cmp.l	#3,%d0
	beq	StereoFastPolyphaseStride5Phase3_Amiga_m68k
	cmp.l	#4,%d0
	beq	StereoFastPolyphaseStride5Phase4_Amiga_m68k
	moveq	#0,%d0
	rts


/* phase 0 samples: 0, 5, 10, 15, 20, 25, 30 (7 pairs) */
StereoFastPolyphaseStride5Phase0_Amiga_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0		| pcm
	move.l	52(%sp),%a1		| vbuf
	move.l	56(%sp),%a2		| coefBase

	STEREO_SAMPLE0 0
	STEREO_PAIR_LO 5,4
	STEREO_PAIR_LO 10,8
	STEREO_PAIR_LO 15,12
	STEREO_PAIR_HI 12,16
	STEREO_PAIR_HI 7,20
	STEREO_PAIR_HI 2,24

	moveq	#14,%d0
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts


/* phase 1 samples: 4, 9, 14, 19, 24, 29 (6 pairs) */
StereoFastPolyphaseStride5Phase1_Amiga_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0		| pcm
	move.l	52(%sp),%a1		| vbuf
	move.l	56(%sp),%a2		| coefBase

	STEREO_PAIR_LO 4,0
	STEREO_PAIR_LO 9,4
	STEREO_PAIR_LO 14,8
	STEREO_PAIR_HI 13,12
	STEREO_PAIR_HI 8,16
	STEREO_PAIR_HI 3,20

	moveq	#12,%d0
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts


/* phase 2 samples: 3, 8, 13, 18, 23, 28 (6 pairs) */
StereoFastPolyphaseStride5Phase2_Amiga_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0		| pcm
	move.l	52(%sp),%a1		| vbuf
	move.l	56(%sp),%a2		| coefBase

	STEREO_PAIR_LO 3,0
	STEREO_PAIR_LO 8,4
	STEREO_PAIR_LO 13,8
	STEREO_PAIR_HI 14,12
	STEREO_PAIR_HI 9,16
	STEREO_PAIR_HI 4,20

	moveq	#12,%d0
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts


/* phase 3 samples: 2, 7, 12, 17, 22, 27 (6 pairs) */
StereoFastPolyphaseStride5Phase3_Amiga_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0		| pcm
	move.l	52(%sp),%a1		| vbuf
	move.l	56(%sp),%a2		| coefBase

	STEREO_PAIR_LO 2,0
	STEREO_PAIR_LO 7,4
	STEREO_PAIR_LO 12,8
	STEREO_PAIR_HI 15,12
	STEREO_PAIR_HI 10,16
	STEREO_PAIR_HI 5,20

	moveq	#12,%d0
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts


/* phase 4 samples: 1, 6, 11, 16, 21, 26, 31 (7 pairs) */
StereoFastPolyphaseStride5Phase4_Amiga_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0		| pcm
	move.l	52(%sp),%a1		| vbuf
	move.l	56(%sp),%a2		| coefBase

	STEREO_PAIR_LO 1,0
	STEREO_PAIR_LO 6,4
	STEREO_PAIR_LO 11,8
	STEREO_SAMPLE16 12
	STEREO_PAIR_HI 11,16
	STEREO_PAIR_HI 6,20
	STEREO_PAIR_HI 1,24

	moveq	#14,%d0
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

	.even
	.data
	.even
StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts:
	.long	0,0,0,0
StereoFastPolyphaseStride5_Amiga_m68k_PhaseCounts:
	.long	0,0,0,0,0
	.text
	.even
#endif /* AMIGA_M68K_ASM_POLYPHASE */
