/* Amiga/m68k 68030 interleaved-stereo-s16 -> planar-s8 PCM conversion.
 * Assembled only for explicit AMIGA_M68K_ASM_PLANARS8 builds. Used by
 * DecodeStreamFillPlanarS8()'s true-stereo fast path in amiga_mp3dec.c.
 */

#if defined(AMIGA_M68K_ASM_PLANARS8)
	.text
	.even
	.globl	PlanarS8FromInterleavedStereo_Amiga_m68k

/* void PlanarS8FromInterleavedStereo_Amiga_m68k(signed char *left,
 *     signed char *right, const short *pcm, int count)
 * 4(%sp)  = left
 * 8(%sp)  = right
 * 12(%sp) = pcm (interleaved L0,R0,L1,R1,...)
 * 16(%sp) = count (stereo pairs)
 *
 * Converts `count` interleaved 16-bit stereo pairs into two planar signed-8-bit
 * arrays, matching Sample16ToS8()'s `(signed char)(s >> 8)` truncation in
 * amiga_mp3dec.c exactly. This target is big-endian, so the high byte of a
 * 16-bit sample is its first (lowest-address) byte -- no shift is needed,
 * each output byte is read directly out of the interleaved buffer.
 *
 * pcm[2i] and pcm[2i+1] are two separate 16-bit shorts (4 bytes total), not
 * four consecutive extracted bytes: relative to %a2 (the base of pair i),
 * pcm[2i]'s high byte is at offset 0 and pcm[2i+1]'s high byte is at offset
 * 2 (its low byte at offset 3) -- offset 1 is pcm[2i]'s own low byte, not
 * any part of the right-channel sample.
 */
PlanarS8FromInterleavedStereo_Amiga_m68k:
	move.l	%a2,-(%sp)
	move.l	8(%sp),%a0		| left
	move.l	12(%sp),%a1		| right
	move.l	16(%sp),%a2		| pcm
	move.l	20(%sp),%d0		| count
	tst.l	%d0
	beq.s	1f
0:
	move.b	(%a2),(%a0)+
	move.b	2(%a2),(%a1)+
	addq.l	#4,%a2
	subq.l	#1,%d0
	bne.s	0b
1:
	move.l	(%sp)+,%a2
	rts

	.text
	.even
#endif /* AMIGA_M68K_ASM_PLANARS8 */
