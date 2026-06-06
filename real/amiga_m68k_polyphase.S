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
#endif /* AMIGA_M68K_ASM_POLYPHASE */
