/*
 * polyphase60_ref_test.c - correctness test for the 68060 polyphase kernel.
 *
 * Compares the dedicated 68060 polyphase kernel (real/polyphase_68060.h)
 * against a bit-exact 64-bit reference polyphase (the same MADD64/SAR64 math
 * as PolyphaseMonoReference/PolyphaseStereoReference in real/polyphase.c),
 * driven with the real polyCoef table and randomised vbuf input.
 *
 * It reports, for mono and stereo, the maximum absolute PCM sample error and
 * the number of differing samples across many randomised frames plus boundary
 * cases. This is a HOST test: MulShift68060 is portable C and produces the same
 * result on any platform, so the numeric comparison is valid off-target. The
 * separate check that the kernel emits no register-pair muls.l is done by
 * disassembling an -m68060 build with tools/audit_68060_instructions.py.
 *
 * Build (host):
 *   gcc -O2 -Ipub -Ireal -DAMIGA_M68K tests/polyphase60_ref_test.c \
 *       real/trigtabs.c -o polyphase60_ref_test
 *
 * Exit status is non-zero if the error exceeds POLY60_MAX_ALLOWED_ERR.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coder.h"            /* polyCoef (via STATNAME mapping) */
#include "polyphase_68060.h"  /* PolyphaseMono68060 / PolyphaseStereo68060 */

/* Fixed-point layout, matching real/polyphase.c. */
#define REF_CSHIFT 12
#define REF_NFRAC  (25 - 2 - 2 - 15) /* DEF_NFRACBITS == 6 */

typedef long long W64;

/* Largest per-sample PCM error we accept between the fast 68060 kernel and the
 * bit-exact 64-bit reference. The fast path truncates each tap's product to the
 * output domain, so a few LSB of divergence is expected and matches the
 * already-shipped AMIGA_FAST_POLYPHASE C path. This bound is asserted, not
 * assumed -- if a change makes it worse the test fails loudly. */
#define POLY60_MAX_ALLOWED_ERR 2

#define VBUF_INTS 2048

static short RefClip(W64 sum)
{
	int x = (int)(sum >> (32 - REF_CSHIFT)); /* SAR64 by 20 */
	int sign;
	x >>= REF_NFRAC;                         /* ClipToShort's >>fracBits */
	sign = x >> 31;
	if (sign != (x >> 15))
		x = sign ^ ((1 << 15) - 1);
	return (short)x;
}

#define REF_RND ((W64)1 << (REF_NFRAC - 1 + (32 - REF_CSHIFT)))

#define RMC0(x) { \
	c1 = *coef; coef++; c2 = *coef; coef++; \
	vLo = *(vb1 + (x)); vHi = *(vb1 + (23 - (x))); \
	sum1 += (W64)vLo * c1; sum1 += (W64)vHi * (-c2); \
}
#define RMC1(x) { \
	c1 = *coef; coef++; \
	vLo = *(vb1 + (x)); \
	sum1 += (W64)vLo * c1; \
}
#define RMC2(x) { \
	c1 = *coef; coef++; c2 = *coef; coef++; \
	vLo = *(vb1 + (x)); vHi = *(vb1 + (23 - (x))); \
	sum1 += (W64)vLo * c1; sum2 += (W64)vLo * c2; \
	sum1 += (W64)vHi * (-c2); sum2 += (W64)vHi * c1; \
}

static void PolyphaseMonoRef(short *pcm, int *vbuf, const int *coefBase)
{
	int i;
	const int *coef;
	int *vb1;
	int vLo, vHi, c1, c2;
	W64 sum1, sum2;

	coef = coefBase; vb1 = vbuf; sum1 = REF_RND;
	RMC0(0) RMC0(1) RMC0(2) RMC0(3) RMC0(4) RMC0(5) RMC0(6) RMC0(7)
	*(pcm + 0) = RefClip(sum1);

	coef = coefBase + 256; vb1 = vbuf + 64 * 16; sum1 = REF_RND;
	RMC1(0) RMC1(1) RMC1(2) RMC1(3) RMC1(4) RMC1(5) RMC1(6) RMC1(7)
	*(pcm + 16) = RefClip(sum1);

	coef = coefBase + 16; vb1 = vbuf + 64; pcm++;
	for (i = 15; i > 0; i--) {
		sum1 = sum2 = REF_RND;
		RMC2(0) RMC2(1) RMC2(2) RMC2(3) RMC2(4) RMC2(5) RMC2(6) RMC2(7)
		vb1 += 64;
		*(pcm)         = RefClip(sum1);
		*(pcm + 2 * i) = RefClip(sum2);
		pcm++;
	}
}

#define RMC0S(x) { \
	c1 = *coef; coef++; c2 = *coef; coef++; \
	vLo = *(vb1 + (x)); vHi = *(vb1 + (23 - (x))); \
	sum1L += (W64)vLo * c1; sum1L += (W64)vHi * (-c2); \
	vLo = *(vb1 + 32 + (x)); vHi = *(vb1 + 32 + (23 - (x))); \
	sum1R += (W64)vLo * c1; sum1R += (W64)vHi * (-c2); \
}
#define RMC1S(x) { \
	c1 = *coef; coef++; \
	vLo = *(vb1 + (x)); sum1L += (W64)vLo * c1; \
	vLo = *(vb1 + 32 + (x)); sum1R += (W64)vLo * c1; \
}
#define RMC2S(x) { \
	c1 = *coef; coef++; c2 = *coef; coef++; \
	vLo = *(vb1 + (x)); vHi = *(vb1 + (23 - (x))); \
	sum1L += (W64)vLo * c1; sum2L += (W64)vLo * c2; \
	sum1L += (W64)vHi * (-c2); sum2L += (W64)vHi * c1; \
	vLo = *(vb1 + 32 + (x)); vHi = *(vb1 + 32 + (23 - (x))); \
	sum1R += (W64)vLo * c1; sum2R += (W64)vLo * c2; \
	sum1R += (W64)vHi * (-c2); sum2R += (W64)vHi * c1; \
}

static void PolyphaseStereoRef(short *pcm, int *vbuf, const int *coefBase)
{
	int i;
	const int *coef;
	int *vb1;
	int vLo, vHi, c1, c2;
	W64 sum1L, sum2L, sum1R, sum2R;

	coef = coefBase; vb1 = vbuf; sum1L = sum1R = REF_RND;
	RMC0S(0) RMC0S(1) RMC0S(2) RMC0S(3) RMC0S(4) RMC0S(5) RMC0S(6) RMC0S(7)
	*(pcm + 0) = RefClip(sum1L);
	*(pcm + 1) = RefClip(sum1R);

	coef = coefBase + 256; vb1 = vbuf + 64 * 16; sum1L = sum1R = REF_RND;
	RMC1S(0) RMC1S(1) RMC1S(2) RMC1S(3) RMC1S(4) RMC1S(5) RMC1S(6) RMC1S(7)
	*(pcm + 2 * 16 + 0) = RefClip(sum1L);
	*(pcm + 2 * 16 + 1) = RefClip(sum1R);

	coef = coefBase + 16; vb1 = vbuf + 64; pcm += 2;
	for (i = 15; i > 0; i--) {
		sum1L = sum2L = REF_RND;
		sum1R = sum2R = REF_RND;
		RMC2S(0) RMC2S(1) RMC2S(2) RMC2S(3) RMC2S(4) RMC2S(5) RMC2S(6) RMC2S(7)
		vb1 += 64;
		*(pcm + 0)             = RefClip(sum1L);
		*(pcm + 1)             = RefClip(sum1R);
		*(pcm + 2 * 2 * i + 0) = RefClip(sum2L);
		*(pcm + 2 * 2 * i + 1) = RefClip(sum2R);
		pcm += 2;
	}
}

/* -------- MulShift68060 exactness self-check -------- */
static int check_mulshift(void)
{
	int fails = 0;
	long i;
	unsigned int st = 0x1234567u;
	int probes[] = {0, 1, -1, 2, -2, 32767, -32768, 65535, 65536, -65536,
			0x7fffffff, (int)0x80000000, 0x40000000, (int)0xC0000000,
			0x00010001, (int)0xFFFF0001, 0x12345678, (int)0x9abcdef0};
	size_t n = sizeof(probes) / sizeof(probes[0]);
	size_t a, b;
	for (a = 0; a < n; a++)
		for (b = 0; b < n; b++) {
			int x = probes[a], y = probes[b];
			int got = MulShift68060(x, y);
			int want = (int)(((W64)x * (W64)y) >> 32);
			if (got != want) {
				if (fails < 5)
					printf("  mulshift FAIL x=%d y=%d got=%d want=%d\n",
					       x, y, got, want);
				fails++;
			}
		}
	for (i = 0; i < 2000000; i++) {
		int x, y, got, want;
		st = st * 1103515245u + 12345u; x = (int)st;
		st = st * 1103515245u + 12345u; y = (int)st;
		got = MulShift68060(x, y);
		want = (int)(((W64)x * (W64)y) >> 32);
		if (got != want) {
			if (fails < 5)
				printf("  mulshift FAIL x=%d y=%d got=%d want=%d\n",
				       x, y, got, want);
			fails++;
		}
	}
	if (fails)
		printf("MulShift68060: %d MISMATCH(es) vs 64-bit reference\n", fails);
	else
		printf("MulShift68060: exact vs 64-bit reference "
		       "(all probes + 2,000,000 random)\n");
	return fails;
}

static unsigned int g_state = 0x2468ACEu;
static int rnd_range(int bits)
{
	g_state = g_state * 1103515245u + 12345u;
	/* signed value in +/- 2^bits */
	return (int)(g_state >> (32 - (bits + 1))) - (1 << bits);
}

static void fill_vbuf(int *vbuf, int bits)
{
	int i;
	for (i = 0; i < VBUF_INTS; i++)
		vbuf[i] = rnd_range(bits);
}

int main(void)
{
	static int vbuf[VBUF_INTS];
	short pcm_ref[64], pcm_60[64];
	int frame, i;
	int mono_max = 0, mono_diff = 0, mono_tot = 0;
	int stereo_max = 0, stereo_diff = 0, stereo_tot = 0;
	int bits_choices[] = {18, 22, 24, 26, 28};
	int nbits = (int)(sizeof(bits_choices) / sizeof(bits_choices[0]));
	int rc = 0;

	printf("== 68060 polyphase kernel vs bit-exact 64-bit reference ==\n");
	rc |= (check_mulshift() != 0);

	for (frame = 0; frame < 4000; frame++) {
		int bits = bits_choices[frame % nbits];
		fill_vbuf(vbuf, bits);

		/* mono */
		memset(pcm_ref, 0, sizeof(pcm_ref));
		memset(pcm_60, 0, sizeof(pcm_60));
		PolyphaseMonoRef(pcm_ref, vbuf, polyCoef);
		PolyphaseMono68060(pcm_60, vbuf, polyCoef);
		for (i = 0; i < 32; i++) {
			int d = pcm_ref[i] - pcm_60[i];
			if (d < 0) d = -d;
			mono_tot++;
			if (d) mono_diff++;
			if (d > mono_max) mono_max = d;
		}

		/* stereo */
		memset(pcm_ref, 0, sizeof(pcm_ref));
		memset(pcm_60, 0, sizeof(pcm_60));
		PolyphaseStereoRef(pcm_ref, vbuf, polyCoef);
		PolyphaseStereo68060(pcm_60, vbuf, polyCoef);
		for (i = 0; i < 64; i++) {
			int d = pcm_ref[i] - pcm_60[i];
			if (d < 0) d = -d;
			stereo_tot++;
			if (d) stereo_diff++;
			if (d > stereo_max) stereo_max = d;
		}
	}

	printf("mono:   max err %d  differing %d/%d samples\n",
	       mono_max, mono_diff, mono_tot);
	printf("stereo: max err %d  differing %d/%d samples\n",
	       stereo_max, stereo_diff, stereo_tot);
	printf("allowed max err: %d\n", POLY60_MAX_ALLOWED_ERR);

	if (mono_max > POLY60_MAX_ALLOWED_ERR || stereo_max > POLY60_MAX_ALLOWED_ERR) {
		printf("RESULT: FAIL (error exceeds allowance)\n");
		rc = 1;
	} else if (rc) {
		printf("RESULT: FAIL (MulShift68060 not exact)\n");
	} else {
		printf("RESULT: PASS\n");
	}
	return rc;
}
